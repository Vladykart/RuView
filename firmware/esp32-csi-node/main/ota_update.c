/**
 * @file ota_update.c
 * @brief HTTP OTA firmware update for ESP32-S3 CSI Node.
 *
 * Uses ESP-IDF's native OTA API with rollback support.
 * The HTTP server runs on port 8032 and accepts:
 *   POST /ota — firmware binary payload (application/octet-stream)
 *   GET /ota/status — current firmware version and partition info
 */

#include "ota_update.h"

#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mmwave_sensor.h"
#include "display_hal.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "cJSON.h"

static const char *TAG = "ota_update";

/** OTA HTTP server port. */
#define OTA_PORT 8032

/** Maximum firmware size (900 KB — matches CI binary size gate). */
#define OTA_MAX_SIZE 0x1D0000  /* full OTA slot size (partitions_4mb.csv) */

/** NVS namespace and key for the OTA pre-shared key. */
#define OTA_NVS_NAMESPACE "security"
#define OTA_NVS_KEY       "ota_psk"

/** Maximum PSK length (hex-encoded SHA-256). */
#define OTA_PSK_MAX_LEN   65

/** Cached PSK loaded from NVS at init time. Empty = auth disabled. */
static char s_ota_psk[OTA_PSK_MAX_LEN] = {0};

/**
 * ADR-050: Verify the Authorization header contains the correct PSK.
 * Returns true if auth is disabled (no PSK provisioned) or if the
 * Bearer token matches the stored PSK.
 */
static bool ota_check_auth(httpd_req_t *req)
{
    if (s_ota_psk[0] == '\0') {
        /* No PSK provisioned — auth disabled (permissive for dev). */
        return true;
    }

    char auth_header[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header,
                                     sizeof(auth_header)) != ESP_OK) {
        return false;
    }

    /* Expect "Bearer <psk>" */
    const char *prefix = "Bearer ";
    if (strncmp(auth_header, prefix, strlen(prefix)) != 0) {
        return false;
    }

    const char *token = auth_header + strlen(prefix);
    /* Constant-time comparison to prevent timing attacks. */
    size_t psk_len = strlen(s_ota_psk);
    size_t tok_len = strlen(token);
    if (psk_len != tok_len) return false;
    volatile uint8_t result = 0;
    for (size_t i = 0; i < psk_len; i++) {
        result |= (uint8_t)(s_ota_psk[i] ^ token[i]);
    }
    return result == 0;
}

/**
 * POST /reboot — authenticated clean restart (esp_restart). Used by the web
 * dashboard's reboot button; also the clean way to bring the LCD up.
 */
static esp_err_t ota_reboot_handler(httpd_req_t *req)
{
    if (!ota_check_auth(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"rebooting\"}");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;  /* not reached */
}

/**
 * POST /config — write runtime config to NVS (csi_cfg) over the air, then
 * reboot. JSON body: {ssid,password,channel,node_id,target_ip} (all optional).
 * Used by the web dashboard's per-node Config form.
 */
static esp_err_t ota_config_handler(httpd_req_t *req)
{
    if (!ota_check_auth(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
        return ESP_OK;
    }
    int total = req->content_len;
    if (total <= 0 || total > 1024) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"bad body\"}");
        return ESP_OK;
    }
    char body[1025];
    int rcv = 0;
    while (rcv < total) {
        int r = httpd_req_recv(req, body + rcv, total - rcv);
        if (r <= 0) { httpd_resp_set_status(req, "400 Bad Request");
                      httpd_resp_sendstr(req, "{\"error\":\"recv\"}"); return ESP_OK; }
        rcv += r;
    }
    body[rcv] = '\0';

    cJSON *j = cJSON_Parse(body);
    if (!j) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"invalid json\"}");
        return ESP_OK;
    }
    nvs_handle_t h;
    if (nvs_open("csi_cfg", NVS_READWRITE, &h) != ESP_OK) {
        cJSON_Delete(j);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"nvs\"}");
        return ESP_OK;
    }
    int changed = 0;
    cJSON *it;
    if ((it = cJSON_GetObjectItem(j, "ssid")) && cJSON_IsString(it) && strlen(it->valuestring)) {
        nvs_set_str(h, "ssid", it->valuestring); changed++;
    }
    if ((it = cJSON_GetObjectItem(j, "password")) && cJSON_IsString(it)) {
        nvs_set_str(h, "password", it->valuestring); changed++;
    }
    if ((it = cJSON_GetObjectItem(j, "target_ip")) && cJSON_IsString(it) && strlen(it->valuestring)) {
        nvs_set_str(h, "target_ip", it->valuestring); changed++;
    }
    if ((it = cJSON_GetObjectItem(j, "channel")) && cJSON_IsNumber(it) && it->valueint > 0) {
        nvs_set_u8(h, "csi_channel", (uint8_t)it->valueint); changed++;
    }
    if ((it = cJSON_GetObjectItem(j, "node_id")) && cJSON_IsNumber(it) && it->valueint >= 0) {
        uint8_t nid = (uint8_t)it->valueint;
        nvs_set_u8(h, "node_id", nid);
        if (nid >= 1) nvs_set_u8(h, "tdm_slot", (uint8_t)(nid - 1));  /* keep slot aligned */
        changed++;
    }
    nvs_commit(h);
    nvs_close(h);
    cJSON_Delete(j);

    char resp[96];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"changed\":%d,\"message\":\"saved, rebooting\"}", changed);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
    return ESP_OK;  /* not reached */
}

/**
 * GET /ota/status — return firmware version and partition info.
 */
static esp_err_t ota_status_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);

    /* mmWave state — lets headless nodes report sensor detection remotely. */
    mmwave_state_t mw;
    bool has_mw = mmwave_sensor_get_state(&mw);

    /* Battery (StickS3 M5PM1 PMU). -1 when the board has no battery monitor. */
    int batt_mv = -1, batt_pct = -1;
#if CONFIG_DISPLAY_STICKS3
    {
        uint16_t mv = 0; uint8_t pct = 0;
        if (display_hal_battery(&mv, &pct) == ESP_OK) { batt_mv = mv; batt_pct = pct; }
    }
#endif

    char response[640];
    int len = snprintf(response, sizeof(response),
        "{\"version\":\"%s\",\"date\":\"%s\",\"time\":\"%s\","
        "\"running_partition\":\"%s\",\"next_partition\":\"%s\","
        "\"max_size\":%d,"
        "\"battery_mv\":%d,\"battery_pct\":%d,"
        "\"mmwave\":\"%s\",\"mmwave_present\":%s,\"mmwave_frames\":%lu}",
        app->version, app->date, app->time,
        running ? running->label : "unknown",
        update ? update->label : "none",
        OTA_MAX_SIZE,
        batt_mv, batt_pct,
        has_mw ? mmwave_type_name(mw.type) : "none",
        (has_mw && mw.person_present) ? "true" : "false",
        (unsigned long)(has_mw ? mw.frame_count : 0));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, len);
    return ESP_OK;
}

/**
 * GET /ota/diag — mmWave wiring/probe diagnostics for headless nodes.
 */
static esp_err_t ota_diag_handler(httpd_req_t *req)
{
    mmwave_state_t mw;
    bool has_mw = mmwave_sensor_get_state(&mw);
    char response[768];
    int len = snprintf(response, sizeof(response),
        "{\"mmwave\":\"%s\",\"pins\":\"%s\",\"frames\":%lu,\"errors\":%lu}",
        has_mw ? mmwave_type_name(mw.type) : "none",
        mmwave_get_diag(),
        (unsigned long)(has_mw ? mw.frame_count : 0),
        (unsigned long)(has_mw ? mw.error_count : 0));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, len);
    return ESP_OK;
}

/**
 * POST /ota — receive and flash firmware binary.
 */
static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    /* ADR-050: Authenticate before accepting firmware upload. */
    if (!ota_check_auth(req)) {
        ESP_LOGW(TAG, "OTA upload rejected: authentication failed");
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                            "Authentication required. Use: Authorization: Bearer <psk>");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA update started, content_length=%d", req->content_len);

    if (req->content_len <= 0 || req->content_len > OTA_MAX_SIZE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Invalid firmware size (exceeds OTA slot)");
        return ESP_FAIL;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "No OTA partition available");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "OTA begin failed");
        return ESP_FAIL;
    }

    /* Read firmware in chunks. */
    char buf[1024];
    int received = 0;
    int total = 0;

    while (total < req->content_len) {
        received = httpd_req_recv(req, buf, sizeof(buf));
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;  /* Retry on timeout. */
            }
            ESP_LOGE(TAG, "OTA receive error at byte %d", total);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "Receive error");
            return ESP_FAIL;
        }

        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed at byte %d: %s",
                     total, esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "OTA write failed");
            return ESP_FAIL;
        }

        total += received;
        if ((total % (64 * 1024)) == 0) {
            ESP_LOGI(TAG, "OTA progress: %d / %d bytes (%.0f%%)",
                     total, req->content_len,
                     (float)total * 100.0f / (float)req->content_len);
        }
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "OTA validation failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Set boot partition failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA update successful! Rebooting to partition '%s'...",
             update_partition->label);

    const char *resp = "{\"status\":\"ok\",\"message\":\"OTA update successful. Rebooting...\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));

    /* Delay briefly to let the response flush, then reboot. */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;  /* Never reached. */
}

/** Internal: start the HTTP server and register OTA endpoints. */
static esp_err_t ota_start_server(httpd_handle_t *out_handle)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = OTA_PORT;
    config.max_uri_handlers = 12;  /* Extra slots for WASM endpoints (ADR-040). */
    /* Increase receive timeout for large uploads. */
    config.recv_wait_timeout = 30;
    /* esp_ota_end() runs esp_image_verify() on this task; the default 4KB
     * stack overflows during the SHA-256 read-back (corrupted-backtrace
     * panic at ~95% upload). */
    config.stack_size = 10240;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start OTA HTTP server on port %d: %s",
                 OTA_PORT, esp_err_to_name(err));
        if (out_handle) *out_handle = NULL;
        return err;
    }

    httpd_uri_t reboot_uri = {
        .uri      = "/reboot",
        .method   = HTTP_POST,
        .handler  = ota_reboot_handler,
    };
    httpd_register_uri_handler(server, &reboot_uri);

    httpd_uri_t config_uri = {
        .uri      = "/config",
        .method   = HTTP_POST,
        .handler  = ota_config_handler,
    };
    httpd_register_uri_handler(server, &config_uri);

    httpd_uri_t status_uri = {
        .uri      = "/ota/status",
        .method   = HTTP_GET,
        .handler  = ota_status_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &status_uri);

    httpd_uri_t diag_uri = {
        .uri      = "/ota/diag",
        .method   = HTTP_GET,
        .handler  = ota_diag_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &diag_uri);

    httpd_uri_t upload_uri = {
        .uri      = "/ota",
        .method   = HTTP_POST,
        .handler  = ota_upload_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &upload_uri);

    ESP_LOGI(TAG, "OTA HTTP server started on port %d", OTA_PORT);
    ESP_LOGI(TAG, "  GET  /ota/status — firmware version info");
    ESP_LOGI(TAG, "  POST /ota        — upload new firmware binary");

    if (out_handle) *out_handle = server;
    return ESP_OK;
}

/** ADR-050: Load the OTA PSK from NVS if provisioned. */
static void ota_load_psk(void)
{
    nvs_handle_t nvs;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(s_ota_psk);
        if (nvs_get_str(nvs, OTA_NVS_KEY, s_ota_psk, &len) == ESP_OK) {
            ESP_LOGI(TAG, "OTA PSK loaded from NVS (%d chars) — authentication enabled", (int)len - 1);
        } else {
            ESP_LOGW(TAG, "No OTA PSK in NVS — OTA authentication DISABLED (provision with --ota-psk)");
        }
        nvs_close(nvs);
    } else {
        ESP_LOGW(TAG, "NVS namespace '%s' not found — OTA authentication DISABLED", OTA_NVS_NAMESPACE);
    }
}

esp_err_t ota_update_init(void)
{
    ota_load_psk();
    return ota_start_server(NULL);
}

esp_err_t ota_update_init_ex(void **out_server)
{
    ota_load_psk();
    return ota_start_server((httpd_handle_t *)out_server);
}
