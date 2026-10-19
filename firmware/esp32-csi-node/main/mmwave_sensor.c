/**
 * @file mmwave_sensor.c
 * @brief ADR-063: mmWave sensor UART driver with auto-detection.
 *
 * Supports Seeed MR60BHA2 (60 GHz) and HLK-LD2410 (24 GHz).
 * Under QEMU (CONFIG_CSI_MOCK_ENABLED), uses a mock generator
 * that produces synthetic vital signs for pipeline testing.
 *
 * MR60BHA2 frame format (Seeed mmWave protocol):
 *   [0]    SOF = 0x01
 *   [1-2]  Frame ID (uint16, big-endian)
 *   [3-4]  Data Length (uint16, big-endian)
 *   [5-6]  Frame Type (uint16, big-endian)
 *   [7]    Header Checksum = ~XOR(bytes 0..6)
 *   [8..N] Payload (N = data_length)
 *   [N+1]  Data Checksum = ~XOR(payload bytes)
 *
 *   Frame types: 0x0A14=breathing, 0x0A15=heart rate,
 *                0x0A16=distance, 0x0F09=presence
 *
 * LD2410 frame format (HLK binary, 256000 baud):
 *   Header:  0xF4 0xF3 0xF2 0xF1
 *   Length:  uint16 LE
 *   Data:    [type 0xAA] [target_state] [moving_dist LE] [energy] ...
 *   Footer:  0xF8 0xF7 0xF6 0xF5
 */

#include "mmwave_sensor.h"

#include <string.h>
#include <stdarg.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "sdkconfig.h"

#ifndef CONFIG_CSI_MOCK_ENABLED
#include "driver/uart.h"
#include "driver/gpio.h"
#endif

static const char *TAG = "mmwave";

/* ---- Configuration ---- */
#define MMWAVE_UART_NUM           UART_NUM_1
#define MMWAVE_MR60_BAUD          115200
#define MMWAVE_LD2410_BAUD        256000
#define MMWAVE_BUF_SIZE           256
#define MMWAVE_TASK_STACK         4096
#define MMWAVE_TASK_PRIORITY      3
#define MMWAVE_PROBE_TIMEOUT_MS   2000
#define MMWAVE_MR60_MAX_PAYLOAD   30   /* Sanity limit from Arduino lib */

/* ---- MR60BHA2 protocol constants (Seeed mmWave) ---- */
#define MR60_SOF            0x01

/* Frame types (big-endian uint16 at offset 5-6) */
#define MR60_TYPE_BREATHING     0x0A14
#define MR60_TYPE_HEARTRATE     0x0A15
#define MR60_TYPE_DISTANCE      0x0A16
#define MR60_TYPE_PRESENCE      0x0F09
#define MR60_TYPE_PHASE         0x0A13
#define MR60_TYPE_POINTCLOUD    0x0A04

/* ---- LD2410 protocol constants ---- */
#define LD2410_REPORT_HEAD  0xAA
#define LD2410_REPORT_TAIL  0x55

/* ---- DFRobot C1001 protocol constants (60 GHz, query/response) ----
 * Frame: 0x53 0x59 <con> <cmd> <len_hi> <len_lo> <data..> <cksum> 0x54 0x43
 * cksum = sum(bytes from 0x53 through last data byte) & 0xFF.
 * Queries carry one data byte 0x0F; responses echo <con>/<cmd> with values.
 */
#define C1001_BAUD          115200
#define C1001_HDR0          0x53
#define C1001_HDR1          0x59
#define C1001_TAIL0         0x54
#define C1001_TAIL1         0x43
#define C1001_CON_STATUS    0x80  /* cmd 0x81 presence, 0x82 motion, 0x84 distance (2B BE, cm) */
#define C1001_CON_BREATH    0x81  /* cmd 0x82 breathing rate */
#define C1001_CON_FALL      0x83  /* cmd 0x81 fall state */
#define C1001_CON_HEART     0x85  /* cmd 0x82 heart rate */
#define C1001_CON_MODE      0x02  /* cmd 0xA8 set work mode: data 0x00 sleep, 0x01 fall */
#define C1001_CON_GEOM      0x06  /* fall mode install: 0x81 angle x,y,z (deg), 0x82 height (cm) */
#define C1001_CMD_AUTOHGT   0x90  /* con 0x83: sensor's auto-measured height (cm) */
#define C1001_CMD_DISTANCE  0x84  /* sleep mode: target distance, 2B BE cm        */
#define C1001_CMD_SEAT_DIST 0x8D  /* fall mode: seated detection RANGE setting (not a reading) */
#define C1001_CMD_MOVE_DIST 0x8E  /* fall mode: motion detection RANGE setting (not a reading) */
#define C1001_CMD_TRACK     0x8E  /* con 0x83 (fall): track point x,y int16 BE ×2 */
#define C1001_CMD_SETMODE   0xA8
#define C1001_MODE_SLEEP    MMWAVE_C1001_MODE_VITALS
#define C1001_MODE_FALL     MMWAVE_C1001_MODE_FALL
#define C1001_MAX_DATA      40

/* ---- Shared state ---- */
static mmwave_state_t s_state;
static volatile bool s_running;

/* C1001 work mode (what the sensor runs now), the operator's policy, and a
 * pending policy request from another task (the UART task owns the port, so
 * it applies changes). 0xFF = none pending. */
static volatile uint8_t s_c1001_mode = MMWAVE_C1001_MODE_FALL;
static volatile uint8_t s_c1001_policy = MMWAVE_C1001_POLICY_AUTO;
static volatile uint8_t s_c1001_policy_req = 0xFF;

uint8_t mmwave_c1001_get_mode(void) { return s_c1001_mode; }
uint8_t mmwave_c1001_get_policy(void) { return s_c1001_policy; }

void mmwave_c1001_set_policy(uint8_t policy)
{
    if (policy > MMWAVE_C1001_POLICY_AUTO) return;
    s_c1001_policy_req = policy;
}

/* Track filter: alpha-beta on the raw track point (cm, sensor frame). Smooths
 * jitter, rejects jumps beyond the gate, coasts through zero readings. */
#define TRK_ALPHA       0.55f
#define TRK_BETA        0.15f
#define TRK_GATE_CM     120.0f
#define TRK_VMAX_CMS    300.0f
static float   s_fx, s_fy, s_fvx, s_fvy;
static int64_t s_f_us;
static bool    s_f_init;
static int     s_f_rejects;

/* Diagnostic transcript kept in RAM so headless nodes can serve it
 * remotely (GET /ota/diag). */
static char s_diag_buf[512];
static size_t s_diag_len;

static void __attribute__((unused)) diag_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (s_diag_len < sizeof(s_diag_buf) - 1) {
        int n = vsnprintf(s_diag_buf + s_diag_len,
                          sizeof(s_diag_buf) - s_diag_len, fmt, ap);
        if (n > 0) s_diag_len += (size_t)n;
    }
    va_end(ap);
}

const char *mmwave_get_diag(void)
{
    return s_diag_buf;
}

/* ======================================================================
 * MR60BHA2 Parser (corrected protocol from Seeed Arduino library)
 * ====================================================================== */

static uint8_t mr60_calc_checksum(const uint8_t *data, uint16_t len)
{
    uint8_t cksum = 0;
    for (uint16_t i = 0; i < len; i++) {
        cksum ^= data[i];
    }
    return ~cksum;
}

typedef enum {
    MR60_WAIT_SOF,
    MR60_READ_HEADER,   /* Accumulate bytes 1..7 (frame_id, len, type, hdr_cksum) */
    MR60_READ_DATA,
    MR60_READ_DATA_CKSUM,
} mr60_parse_state_t;

typedef struct {
    mr60_parse_state_t state;
    uint8_t  header[8];     /* Full header: SOF + frame_id(2) + len(2) + type(2) + hdr_cksum */
    uint8_t  hdr_idx;
    uint16_t data_len;
    uint16_t frame_type;
    uint16_t data_idx;
    uint8_t  data[MMWAVE_BUF_SIZE];
} mr60_parser_t;

static mr60_parser_t s_mr60;

static void mr60_process_frame(uint16_t type, const uint8_t *data, uint16_t len)
{
    s_state.frame_count++;
    s_state.last_update_us = esp_timer_get_time();

    switch (type) {
    case MR60_TYPE_BREATHING:
        if (len >= 4) {
            /* Breathing rate as float32 (little-endian in payload). */
            float br;
            memcpy(&br, data, sizeof(float));
            if (br >= 0.0f && br <= 60.0f) {
                s_state.breathing_rate = br;
            }
        }
        break;

    case MR60_TYPE_HEARTRATE:
        if (len >= 4) {
            float hr;
            memcpy(&hr, data, sizeof(float));
            if (hr >= 0.0f && hr <= 250.0f) {
                s_state.heart_rate_bpm = hr;
            }
        }
        break;

    case MR60_TYPE_DISTANCE:
        if (len >= 8) {
            /* Bytes 0-3: range flag (uint32 LE). 0 = no valid distance. */
            uint32_t range_flag;
            memcpy(&range_flag, data, sizeof(uint32_t));
            if (range_flag != 0 && len >= 8) {
                float dist;
                memcpy(&dist, &data[4], sizeof(float));
                s_state.distance_cm = dist;
            }
        }
        break;

    case MR60_TYPE_PRESENCE:
        if (len >= 1) {
            s_state.person_present = (data[0] != 0);
        }
        break;

    default:
        break;
    }
}

static void mr60_feed_byte(uint8_t b)
{
    switch (s_mr60.state) {
    case MR60_WAIT_SOF:
        if (b == MR60_SOF) {
            s_mr60.header[0] = b;
            s_mr60.hdr_idx = 1;
            s_mr60.state = MR60_READ_HEADER;
        }
        break;

    case MR60_READ_HEADER:
        s_mr60.header[s_mr60.hdr_idx++] = b;
        if (s_mr60.hdr_idx >= 8) {
            /* Validate header checksum: ~XOR(bytes 0..6) == byte 7 */
            uint8_t expected = mr60_calc_checksum(s_mr60.header, 7);
            if (expected != s_mr60.header[7]) {
                s_state.error_count++;
                s_mr60.state = MR60_WAIT_SOF;
                break;
            }
            /* Parse header fields (big-endian) */
            s_mr60.data_len = ((uint16_t)s_mr60.header[3] << 8) | s_mr60.header[4];
            s_mr60.frame_type = ((uint16_t)s_mr60.header[5] << 8) | s_mr60.header[6];
            s_mr60.data_idx = 0;

            if (s_mr60.data_len > MMWAVE_MR60_MAX_PAYLOAD) {
                s_state.error_count++;
                s_mr60.state = MR60_WAIT_SOF;
            } else if (s_mr60.data_len == 0) {
                s_mr60.state = MR60_READ_DATA_CKSUM;
            } else {
                s_mr60.state = MR60_READ_DATA;
            }
        }
        break;

    case MR60_READ_DATA:
        s_mr60.data[s_mr60.data_idx++] = b;
        if (s_mr60.data_idx >= s_mr60.data_len) {
            s_mr60.state = MR60_READ_DATA_CKSUM;
        }
        break;

    case MR60_READ_DATA_CKSUM:
        /* Validate data checksum */
        if (s_mr60.data_len > 0) {
            uint8_t expected = mr60_calc_checksum(s_mr60.data, s_mr60.data_len);
            if (expected == b) {
                mr60_process_frame(s_mr60.frame_type, s_mr60.data, s_mr60.data_len);
            } else {
                s_state.error_count++;
            }
        } else {
            /* Zero-length payload — checksum byte is for empty data */
            mr60_process_frame(s_mr60.frame_type, s_mr60.data, 0);
        }
        s_mr60.state = MR60_WAIT_SOF;
        break;
    }
}

/* ======================================================================
 * LD2410 Parser (HLK binary protocol, 256000 baud)
 * ====================================================================== */

typedef enum {
    LD_WAIT_F4, LD_WAIT_F3, LD_WAIT_F2, LD_WAIT_F1,
    LD_READ_LEN_L, LD_READ_LEN_H,
    LD_READ_DATA,
    LD_WAIT_F8, LD_WAIT_F7, LD_WAIT_F6, LD_WAIT_F5,
} ld2410_parse_state_t;

typedef struct {
    ld2410_parse_state_t state;
    uint16_t data_len;
    uint16_t data_idx;
    uint8_t  data[MMWAVE_BUF_SIZE];
} ld2410_parser_t;

static ld2410_parser_t s_ld;

static void ld2410_process_frame(const uint8_t *data, uint16_t len)
{
    s_state.frame_count++;
    s_state.last_update_us = esp_timer_get_time();

    if (len < 12) return;

    uint8_t data_type = data[0];   /* 0x02 = normal, 0x01 = engineering */
    uint8_t head_marker = data[1]; /* Must be 0xAA */

    if (head_marker != LD2410_REPORT_HEAD) return;

    /* Normal mode target report (data_type 0x02 or 0x01) */
    uint8_t  target_state  = data[2];
    uint16_t moving_dist   = data[3] | ((uint16_t)data[4] << 8);
    uint8_t  moving_energy = data[5];
    uint16_t static_dist   = data[6] | ((uint16_t)data[7] << 8);
    uint8_t  static_energy = data[8];
    uint16_t detect_dist   = data[9] | ((uint16_t)data[10] << 8);

    (void)moving_energy;
    (void)static_energy;
    (void)detect_dist;

    s_state.person_present = (target_state != 0);
    s_state.target_count = (target_state != 0) ? 1 : 0;

    if (target_state == 1 || target_state == 3) {
        s_state.distance_cm = (float)moving_dist;
    } else if (target_state == 2) {
        s_state.distance_cm = (float)static_dist;
    } else {
        s_state.distance_cm = 0.0f;
    }
}

static void ld2410_feed_byte(uint8_t b)
{
    switch (s_ld.state) {
    case LD_WAIT_F4: s_ld.state = (b == 0xF4) ? LD_WAIT_F3 : LD_WAIT_F4; break;
    case LD_WAIT_F3: s_ld.state = (b == 0xF3) ? LD_WAIT_F2 : LD_WAIT_F4; break;
    case LD_WAIT_F2: s_ld.state = (b == 0xF2) ? LD_WAIT_F1 : LD_WAIT_F4; break;
    case LD_WAIT_F1: s_ld.state = (b == 0xF1) ? LD_READ_LEN_L : LD_WAIT_F4; break;
    case LD_READ_LEN_L:
        s_ld.data_len = b;
        s_ld.state = LD_READ_LEN_H;
        break;
    case LD_READ_LEN_H:
        s_ld.data_len |= ((uint16_t)b << 8);
        s_ld.data_idx = 0;
        if (s_ld.data_len == 0 || s_ld.data_len > MMWAVE_BUF_SIZE) {
            s_ld.state = LD_WAIT_F4;
        } else {
            s_ld.state = LD_READ_DATA;
        }
        break;
    case LD_READ_DATA:
        s_ld.data[s_ld.data_idx++] = b;
        if (s_ld.data_idx >= s_ld.data_len) s_ld.state = LD_WAIT_F8;
        break;
    case LD_WAIT_F8: s_ld.state = (b == 0xF8) ? LD_WAIT_F7 : LD_WAIT_F4; break;
    case LD_WAIT_F7: s_ld.state = (b == 0xF7) ? LD_WAIT_F6 : LD_WAIT_F4; break;
    case LD_WAIT_F6: s_ld.state = (b == 0xF6) ? LD_WAIT_F5 : LD_WAIT_F4; break;
    case LD_WAIT_F5:
        if (b == 0xF5) {
            ld2410_process_frame(s_ld.data, s_ld.data_len);
        }
        s_ld.state = LD_WAIT_F4;
        break;
    }
}

/* ======================================================================
 * C1001 Parser + Querier (DFRobot query/response protocol, 115200 baud)
 * ====================================================================== */

typedef enum {
    C1001_WAIT_53, C1001_WAIT_59,
    C1001_READ_CON, C1001_READ_CMD,
    C1001_READ_LEN_H, C1001_READ_LEN_L,
    C1001_READ_DATA, C1001_READ_CKSUM,
    C1001_WAIT_54, C1001_WAIT_43,
} c1001_parse_state_t;

typedef struct {
    c1001_parse_state_t state;
    uint8_t  con;
    uint8_t  cmd;
    uint16_t data_len;
    uint16_t data_idx;
    uint8_t  data[C1001_MAX_DATA];
    uint16_t cksum_acc;   /* Running sum of header+data bytes. */
    uint8_t  cksum_rx;
} c1001_parser_t;

static c1001_parser_t s_c1001;

#ifndef CONFIG_CSI_MOCK_ENABLED
/* One-byte-payload frame. 0x0F is the DFRobot "request/read" magic; a set
 * command (e.g. work mode) passes the value to write instead. */
static void c1001_send(uint8_t con, uint8_t cmd, uint8_t data)
{
    uint8_t frame[10] = { C1001_HDR0, C1001_HDR1, con, cmd, 0x00, 0x01, data,
                          0, C1001_TAIL0, C1001_TAIL1 };
    uint16_t sum = 0;
    for (int i = 0; i < 7; i++) sum += frame[i];
    frame[7] = (uint8_t)(sum & 0xFF);
    uart_write_bytes(MMWAVE_UART_NUM, frame, sizeof(frame));
}

static void c1001_send_query(uint8_t con, uint8_t cmd)
{
    c1001_send(con, cmd, 0x0F);
}

/* Switch the C1001 work mode (UART task only). Values that only the other
 * mode produces are cleared so the UI never shows stale vitals or a stale
 * fall. Sent a few times since the sensor may be busy. */
static void c1001_apply_mode(uint8_t mode, const char *why)
{
    for (int i = 0; i < 3; i++) {
        c1001_send(C1001_CON_MODE, C1001_CMD_SETMODE, mode);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    s_c1001_mode = mode;
    s_state.distance_cm = 0;
    s_state.track_valid = false;
    s_state.motion = 0xFF;
    s_f_init = false;
    if (mode == MMWAVE_C1001_MODE_FALL) {
        s_state.breathing_rate = 0;
        s_state.heart_rate_bpm = 0;
    } else {
        s_state.fall_detected = false;
    }
    ESP_LOGI(TAG, "C1001 work mode -> %s (%s)",
             mode == MMWAVE_C1001_MODE_FALL ? "FALL (presence+track+fall)"
                                            : "VITALS (presence+distance+breathing+heart)",
             why);
}

static void c1001_save_policy(uint8_t policy)
{
    nvs_handle_t h;
    if (nvs_open("csi_cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "mw_mode", policy);
        nvs_commit(h);
        nvs_close(h);
    }
}
#endif

/* Ring of the last received frames, dumped as an "MWD" console line so the
 * real on-wire con/cmd/data can be checked against the protocol. */
#define C1001_DBG_N 8
static struct { uint8_t con, cmd, d0, d1; uint16_t len; } s_c1001_dbg[C1001_DBG_N];
static int s_c1001_dbg_idx = 0;

static void c1001_process_frame(uint8_t con, uint8_t cmd,
                                const uint8_t *data, uint16_t len)
{
    if (len < 1) return;
    s_state.frame_count++;
    s_state.last_update_us = esp_timer_get_time();

    s_c1001_dbg[s_c1001_dbg_idx].con = con;
    s_c1001_dbg[s_c1001_dbg_idx].cmd = cmd;
    s_c1001_dbg[s_c1001_dbg_idx].len = len;
    s_c1001_dbg[s_c1001_dbg_idx].d0 = data[0];
    s_c1001_dbg[s_c1001_dbg_idx].d1 = len > 1 ? data[1] : 0;
    s_c1001_dbg_idx = (s_c1001_dbg_idx + 1) % C1001_DBG_N;

    /* Query replies carry cmd|0x80; in sleep mode the sensor also pushes
     * unsolicited reports with the bare cmd (0x01, 0x02, 0x04 ...). Treat
     * both the same. */
    cmd |= 0x80;

    if (con == C1001_CON_STATUS && cmd == 0x81) {
        s_state.person_present = (data[0] != 0);
        s_state.target_count = (data[0] != 0) ? 1 : 0;
    } else if (con == C1001_CON_STATUS && cmd == 0x82) {
        /* Motion: 0=none, 1=static, 2=active — drives the AUTO policy. */
        if (data[0] <= 2) s_state.motion = data[0];
    } else if (con == C1001_CON_GEOM && cmd == 0x81 && len >= 6) {
        int16_t ax = (int16_t)((data[0] << 8) | data[1]);
        int16_t ay = (int16_t)((data[2] << 8) | data[3]);
        int16_t az = (int16_t)((data[4] << 8) | data[5]);
        if (!s_state.geom_valid || ax != s_state.install_angle_x ||
            ay != s_state.install_angle_y || az != s_state.install_angle_z)
            ESP_LOGI(TAG, "C1001 install angle x=%d y=%d z=%d deg", ax, ay, az);
        s_state.install_angle_x = ax;
        s_state.install_angle_y = ay;
        s_state.install_angle_z = az;
    } else if (con == C1001_CON_GEOM && cmd == 0x82 && len >= 2) {
        uint16_t h = (data[0] << 8) | data[1];
        if (!s_state.geom_valid || h != s_state.install_height_cm)
            ESP_LOGI(TAG, "C1001 install height %u cm", h);
        s_state.install_height_cm = h;
        s_state.geom_valid = true;
    } else if (con == C1001_CON_FALL && cmd == 0x90 && len >= 2) {
        uint16_t h = (data[0] << 8) | data[1];
        if (h != s_state.auto_height_cm)
            ESP_LOGI(TAG, "C1001 auto-measured height %u cm", h);
        s_state.auto_height_cm = h;
    } else if (con == C1001_CON_STATUS && cmd == C1001_CMD_DISTANCE) {
        /* Sleep mode target distance: 2 bytes big-endian, cm. */
        if (len >= 2) s_state.distance_cm = (float)(((uint16_t)data[0] << 8) | data[1]);
    } else if (con == C1001_CON_BREATH && cmd == 0x82) {
        if (data[0] > 0 && data[0] <= 60) s_state.breathing_rate = (float)data[0];
    } else if (con == C1001_CON_HEART && cmd == 0x82) {
        if (data[0] > 0 && data[0] <= 250) s_state.heart_rate_bpm = (float)data[0];
    } else if (con == C1001_CON_FALL && cmd == 0x81) {
        s_state.fall_detected = (data[0] != 0);
    } else if (con == C1001_CON_FALL && cmd == C1001_CMD_TRACK) {
        /* Track point (DFRobot track()): x = buf[6..7], y = buf[8..9], BE.
         * Signed, cm. All-zero means no target to track. */
        if (len >= 4) {
            int16_t x = (int16_t)(((uint16_t)data[0] << 8) | data[1]);
            int16_t y = (int16_t)(((uint16_t)data[2] << 8) | data[3]);
            if (x == 0 && y == 0) return;   /* no target: coast; consumers age it out */
            int64_t now = esp_timer_get_time();
            if (!s_f_init || now - s_f_us > 2000000) {
                s_fx = x; s_fy = y; s_fvx = s_fvy = 0;
                s_f_init = true;
                s_f_rejects = 0;
            } else {
                float dt = (now - s_f_us) / 1e6f;
                float px = s_fx + s_fvx * dt, py = s_fy + s_fvy * dt;
                float rx = x - px, ry = y - py;
                if (sqrtf(rx * rx + ry * ry) > TRK_GATE_CM && ++s_f_rejects < 3)
                    return;                  /* outlier: keep the estimate */
                if (s_f_rejects >= 3) {      /* persistent jump: re-acquire */
                    s_fx = x; s_fy = y; s_fvx = s_fvy = 0;
                } else {
                    s_fx = px + TRK_ALPHA * rx;
                    s_fy = py + TRK_ALPHA * ry;
                    if (dt > 0.02f) {
                        s_fvx += TRK_BETA * rx / dt;
                        s_fvy += TRK_BETA * ry / dt;
                    }
                    if (s_fvx >  TRK_VMAX_CMS) s_fvx =  TRK_VMAX_CMS;
                    if (s_fvx < -TRK_VMAX_CMS) s_fvx = -TRK_VMAX_CMS;
                    if (s_fvy >  TRK_VMAX_CMS) s_fvy =  TRK_VMAX_CMS;
                    if (s_fvy < -TRK_VMAX_CMS) s_fvy = -TRK_VMAX_CMS;
                }
                s_f_rejects = 0;
            }
            s_f_us = now;
            s_state.track_x_cm = (int16_t)lroundf(s_fx);
            s_state.track_y_cm = (int16_t)lroundf(s_fy);
            s_state.track_valid = true;
            s_state.track_us = now;
            /* In fall mode the track point is the only measured position. */
            s_state.distance_cm = sqrtf(s_fx * s_fx + s_fy * s_fy);
        }
    }
}

static void c1001_feed_byte(uint8_t b)
{
    switch (s_c1001.state) {
    case C1001_WAIT_53:
        if (b == C1001_HDR0) { s_c1001.cksum_acc = b; s_c1001.state = C1001_WAIT_59; }
        break;
    case C1001_WAIT_59:
        if (b == C1001_HDR1) { s_c1001.cksum_acc += b; s_c1001.state = C1001_READ_CON; }
        else s_c1001.state = C1001_WAIT_53;
        break;
    case C1001_READ_CON:
        s_c1001.con = b; s_c1001.cksum_acc += b; s_c1001.state = C1001_READ_CMD;
        break;
    case C1001_READ_CMD:
        s_c1001.cmd = b; s_c1001.cksum_acc += b; s_c1001.state = C1001_READ_LEN_H;
        break;
    case C1001_READ_LEN_H:
        s_c1001.data_len = (uint16_t)b << 8; s_c1001.cksum_acc += b;
        s_c1001.state = C1001_READ_LEN_L;
        break;
    case C1001_READ_LEN_L:
        s_c1001.data_len |= b; s_c1001.cksum_acc += b; s_c1001.data_idx = 0;
        if (s_c1001.data_len == 0 || s_c1001.data_len > C1001_MAX_DATA) {
            s_c1001.state = C1001_WAIT_53;
        } else {
            s_c1001.state = C1001_READ_DATA;
        }
        break;
    case C1001_READ_DATA:
        s_c1001.data[s_c1001.data_idx++] = b; s_c1001.cksum_acc += b;
        if (s_c1001.data_idx >= s_c1001.data_len) s_c1001.state = C1001_READ_CKSUM;
        break;
    case C1001_READ_CKSUM:
        s_c1001.cksum_rx = b; s_c1001.state = C1001_WAIT_54;
        break;
    case C1001_WAIT_54:
        s_c1001.state = (b == C1001_TAIL0) ? C1001_WAIT_43 : C1001_WAIT_53;
        break;
    case C1001_WAIT_43:
        if (b == C1001_TAIL1) {
            if ((uint8_t)(s_c1001.cksum_acc & 0xFF) == s_c1001.cksum_rx) {
                c1001_process_frame(s_c1001.con, s_c1001.cmd,
                                    s_c1001.data, s_c1001.data_len);
            } else {
                s_state.error_count++;
            }
        }
        s_c1001.state = C1001_WAIT_53;
        break;
    }
}

/* ======================================================================
 * Mock mmWave Generator (for QEMU testing)
 * ====================================================================== */

#ifdef CONFIG_CSI_MOCK_ENABLED

static void mock_mmwave_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Mock mmWave generator started (simulating MR60BHA2)");

    s_state.type = MMWAVE_TYPE_MOCK;
    s_state.detected = true;
    s_state.capabilities = MMWAVE_CAP_HEART_RATE | MMWAVE_CAP_BREATHING
                         | MMWAVE_CAP_PRESENCE | MMWAVE_CAP_DISTANCE;

    float hr_base = 72.0f;
    float br_base = 16.0f;
    uint32_t tick = 0;

    while (s_running) {
        tick++;

        /* Simulate realistic vital sign variation. */
        float hr_noise = 2.0f * sinf((float)tick * 0.1f) + 0.5f * sinf((float)tick * 0.37f);
        float br_noise = 1.0f * sinf((float)tick * 0.07f) + 0.3f * sinf((float)tick * 0.23f);

        s_state.heart_rate_bpm = hr_base + hr_noise;
        s_state.breathing_rate = br_base + br_noise;
        s_state.person_present = true;
        s_state.distance_cm = 150.0f + 20.0f * sinf((float)tick * 0.05f);
        s_state.target_count = 1;
        s_state.frame_count++;
        s_state.last_update_us = esp_timer_get_time();

        /* Simulate person leaving at tick 200-250 (for scenario testing). */
        if (tick >= 200 && tick <= 250) {
            s_state.person_present = false;
            s_state.heart_rate_bpm = 0.0f;
            s_state.breathing_rate = 0.0f;
            s_state.distance_cm = 0.0f;
            s_state.target_count = 0;
        }

        /* ~1 Hz update rate (matches real MR60BHA2). */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    vTaskDelete(NULL);
}

#endif /* CONFIG_CSI_MOCK_ENABLED */

/* ======================================================================
 * UART Auto-Detection and Task
 * ====================================================================== */

#ifndef CONFIG_CSI_MOCK_ENABLED

/**
 * Try to detect a sensor at the given baud rate.
 * Returns the sensor type if detected, MMWAVE_TYPE_NONE otherwise.
 */
static mmwave_type_t probe_at_baud(uint32_t baud)
{
    /* Reconfigure baud rate. */
    uart_set_baudrate(MMWAVE_UART_NUM, baud);
    uart_flush_input(MMWAVE_UART_NUM);

    uint8_t buf[128];
    int mr60_sof_seen = 0;
    int ld2410_header_seen = 0;

    int64_t deadline = esp_timer_get_time() + (int64_t)(MMWAVE_PROBE_TIMEOUT_MS / 2) * 1000;

    while (esp_timer_get_time() < deadline) {
        int len = uart_read_bytes(MMWAVE_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (len <= 0) continue;

        for (int i = 0; i < len; i++) {
            /* C1001 SY framing heard passively (e.g. a late reply to an
             * earlier probe query): definitive — never call it MR60. */
            if (i + 1 < len && buf[i] == C1001_HDR0 && buf[i + 1] == C1001_HDR1) {
                return MMWAVE_TYPE_C1001;
            }
            /* MR60BHA2: SOF = 0x01, followed by valid-looking frame_id bytes */
            if (buf[i] == MR60_SOF && baud == MMWAVE_MR60_BAUD) {
                mr60_sof_seen++;
            }
            /* LD2410: 4-byte header 0xF4F3F2F1 */
            if (i + 3 < len && buf[i] == 0xF4 && buf[i+1] == 0xF3
                && buf[i+2] == 0xF2 && buf[i+3] == 0xF1
                && baud == MMWAVE_LD2410_BAUD) {
                ld2410_header_seen++;
            }
        }

        if (mr60_sof_seen >= 3) return MMWAVE_TYPE_MR60BHA2;
        if (ld2410_header_seen >= 2) return MMWAVE_TYPE_LD2410;
    }

    if (mr60_sof_seen > 0) return MMWAVE_TYPE_MR60BHA2;
    if (ld2410_header_seen > 0) return MMWAVE_TYPE_LD2410;

    return MMWAVE_TYPE_NONE;
}

#if CONFIG_DISPLAY_STICKS3
static void c1001_io_pins_init(void);
static void c1001_io_pins_poll(void);
#endif

/**
 * Actively probe for a DFRobot C1001: it is query/response (silent until
 * asked), so send presence queries and look for a valid 0x53 0x59 reply.
 */
static mmwave_type_t probe_c1001_at(uint32_t baud);

static mmwave_type_t probe_c1001(void)
{
    /* 115200 is the documented default; scan fallbacks in case the unit
     * shipped configured differently. */
    static const uint32_t bauds[] = { 115200, 9600, 57600, 256000 };
    for (size_t b = 0; b < sizeof(bauds) / sizeof(bauds[0]); b++) {
        mmwave_type_t r = probe_c1001_at(bauds[b]);
        if (r != MMWAVE_TYPE_NONE) {
            ESP_LOGI(TAG, "C1001 answered at %lu baud", (unsigned long)bauds[b]);
            return r;
        }
    }
    return MMWAVE_TYPE_NONE;
}

static mmwave_type_t probe_c1001_at(uint32_t baud)
{
    uart_set_baudrate(MMWAVE_UART_NUM, baud);
    uart_flush_input(MMWAVE_UART_NUM);

    uint8_t buf[64];
    for (int attempt = 0; attempt < 2; attempt++) {
        /* Alternate device-status (0x01/0x83, what DFRobot begin() uses)
         * and presence queries — a booting C1001 answers status first. */
        if (attempt % 2 == 0) c1001_send_query(0x01, 0x83);
        else                  c1001_send_query(C1001_CON_STATUS, 0x81);
        int64_t deadline = esp_timer_get_time() + 500 * 1000;
        while (esp_timer_get_time() < deadline) {
            int len = uart_read_bytes(MMWAVE_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(50));
            if (len > 0) {
                /* Any bytes at all are diagnostic gold: garbled bytes mean
                 * wrong baud or noise; a clean 0x53 0x59 means C1001. */
                char hex[3 * 16 + 1] = {0};
                int n = len < 16 ? len : 16;
                for (int i = 0; i < n; i++)
                    snprintf(hex + i * 3, 4, "%02X ", buf[i]);
                ESP_LOGI(TAG, "C1001 probe RX %d bytes: %s", len, hex);
            }
            for (int i = 0; i + 1 < len; i++) {
                if (buf[i] == C1001_HDR0 && buf[i + 1] == C1001_HDR1) {
                    return MMWAVE_TYPE_C1001;
                }
            }
        }
    }
    return MMWAVE_TYPE_NONE;
}

/**
 * Auto-detect sensor by probing at both baud rates.
 * MR60BHA2 uses 115200, LD2410 uses 256000; C1001 needs an active query.
 */
static mmwave_type_t probe_sensor(void)
{
    /* C1001 FIRST: its query/response handshake is definitive (a valid SY
     * reply to OUR query). The passive MR60 heuristic counts bare 0x01
     * bytes and used to misdetect a chatty C1001 as MR60BHA2. */
    ESP_LOGI(TAG, "Probing at %d baud (C1001, active query)...", C1001_BAUD);
    mmwave_type_t result = probe_c1001();
    if (result != MMWAVE_TYPE_NONE) return result;

    ESP_LOGI(TAG, "Probing at %d baud (MR60BHA2)...", MMWAVE_MR60_BAUD);
    result = probe_at_baud(MMWAVE_MR60_BAUD);
    if (result != MMWAVE_TYPE_NONE) return result;

    ESP_LOGI(TAG, "Probing at %d baud (LD2410)...", MMWAVE_LD2410_BAUD);
    result = probe_at_baud(MMWAVE_LD2410_BAUD);
    return result;
}

static void mmwave_uart_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "mmWave UART task started (type=%s)",
             mmwave_type_name(s_state.type));

    uint8_t buf[128];
    /* C1001 is query/response: round-robin one query per 120 ms. Its two work
     * modes expose different data (DFRobot_HumanDetection: smHumanData vs
     * dmHumanData/getFallData), so each mode has its own query table. Motion
     * (0x80/0x82) is in both: it drives the AUTO policy. */
    static const uint8_t q_vitals[][2] = {
        { C1001_CON_STATUS, 0x81 },                /* presence  */
        { C1001_CON_STATUS, C1001_CMD_DISTANCE },  /* distance  */
        { C1001_CON_STATUS, 0x82 },                /* motion    */
        { C1001_CON_BREATH, 0x82 },                /* breathing */
        { C1001_CON_HEART,  0x82 },                /* heart     */
    };
    /* Fall mode: the track point is the position (and so the distance),
     * every other slot (~4 Hz). Fall state every 0.7 s. NOTE: 0x80/0x8D and
     * 0x80/0x8E are the configured seated/motion detection RANGES, not a
     * measured distance — they read 200/300 cm with nobody present. */
    static const uint8_t q_fall[][2] = {
        { C1001_CON_STATUS, 0x81 },                /* presence   */
        { C1001_CON_FALL,   C1001_CMD_TRACK },     /* track x/y  */
        { C1001_CON_STATUS, 0x82 },                /* motion     */
        { C1001_CON_FALL,   C1001_CMD_TRACK },     /* track x/y  */
        { C1001_CON_FALL,   0x81 },                /* fall state */
        { C1001_CON_FALL,   C1001_CMD_TRACK },     /* track x/y  */
    };
    /* One-shot queries (installation geometry), sent ahead of the table. */
    static const uint8_t q_geom[][2] = {
        { C1001_CON_GEOM, 0x81 },                  /* install angle  */
        { C1001_CON_GEOM, 0x82 },                  /* install height */
        { C1001_CON_FALL, C1001_CMD_AUTOHGT },     /* auto height    */
    };
    int q_idx = 0, geom_left = 0;
    int64_t next_query_us = 0, next_geom_us = 0;
    int64_t next_mwj_us = 0;

    /* AUTO policy timing (see the state machine below). */
    const int64_t AUTO_STILL_US  = 20 * 1000000LL;  /* still this long -> VITALS */
    const int64_t AUTO_ACTIVE_US =  1 * 1000000LL;  /* active this long -> FALL  */
    const int64_t AUTO_ABSENT_US = 10 * 1000000LL;  /* gone this long  -> FALL  */
    const int64_t AUTO_DWELL_US  = 15 * 1000000LL;  /* min time in VITALS... */
    const int64_t AUTO_DWELL_BACK_US = 4 * 1000000LL; /* ...before motion wins */
    int64_t mode_since = 0, still_since = 0, active_since = 0, absent_since = 0;

    if (s_state.type == MMWAVE_TYPE_C1001) {
        /* Restore the operator's policy (default AUTO). Values 0/1 saved by
         * earlier builds are fixed modes and keep their meaning. */
        nvs_handle_t h;
        uint8_t saved = MMWAVE_C1001_POLICY_AUTO;
        if (nvs_open("csi_cfg", NVS_READONLY, &h) == ESP_OK) {
            nvs_get_u8(h, "mw_mode", &saved);
            nvs_close(h);
        }
        s_c1001_policy = saved <= MMWAVE_C1001_POLICY_AUTO ? saved
                                                           : MMWAVE_C1001_POLICY_AUTO;
        uint8_t m = s_c1001_policy == MMWAVE_C1001_POLICY_AUTO
                    ? MMWAVE_C1001_MODE_FALL : s_c1001_policy;
        c1001_apply_mode(m, "boot");
        mode_since = esp_timer_get_time();
        if (m == MMWAVE_C1001_MODE_FALL) geom_left = 3;
    }

    while (s_running) {
        int64_t now = esp_timer_get_time();
        if (s_state.type == MMWAVE_TYPE_C1001 && s_c1001_policy_req != 0xFF) {
            uint8_t pol = s_c1001_policy_req;
            s_c1001_policy_req = 0xFF;
            s_c1001_policy = pol;
            c1001_save_policy(pol);
            ESP_LOGI(TAG, "C1001 policy -> %s (saved)",
                     pol == MMWAVE_C1001_POLICY_AUTO ? "AUTO"
                     : pol == MMWAVE_C1001_POLICY_FALL ? "FALL" : "VITALS");
            uint8_t want = pol == MMWAVE_C1001_POLICY_AUTO ? MMWAVE_C1001_MODE_FALL : pol;
            if (want != s_c1001_mode) {
                c1001_apply_mode(want, "operator");
                mode_since = now = esp_timer_get_time();
                q_idx = 0;
                if (want == MMWAVE_C1001_MODE_FALL) geom_left = 3;
            }
            still_since = active_since = absent_since = 0;
        }

        /* AUTO: FALL while the person moves (that is when falls happen, and
         * it gives the position track); VITALS once they have been still long
         * enough to measure breathing/heart; back to FALL as soon as real
         * motion starts or the room is empty. Never leaves FALL while a fall
         * is reported, so the alarm is not cut off; after the fall clears and
         * the person lies still, VITALS confirms they are breathing. */
        if (s_state.type == MMWAVE_TYPE_C1001 &&
            s_c1001_policy == MMWAVE_C1001_POLICY_AUTO) {
            bool present = s_state.person_present;
            uint8_t mo = s_state.motion;
            if (present && mo <= 1) { if (!still_since) still_since = now; } else still_since = 0;
            if (present && mo == 2) { if (!active_since) active_since = now; } else active_since = 0;
            if (!present) { if (!absent_since) absent_since = now; } else absent_since = 0;
            int64_t in_mode = now - mode_since;
            const char *why = NULL;
            uint8_t next = s_c1001_mode;
            if (s_c1001_mode == MMWAVE_C1001_MODE_FALL) {
                if (!s_state.fall_detected && in_mode > AUTO_DWELL_BACK_US &&
                    still_since && now - still_since > AUTO_STILL_US) {
                    next = MMWAVE_C1001_MODE_VITALS; why = "auto: still";
                }
            } else if (in_mode > AUTO_DWELL_BACK_US) {
                if (active_since && now - active_since >= AUTO_ACTIVE_US) {
                    next = MMWAVE_C1001_MODE_FALL; why = "auto: moving";
                } else if (absent_since && now - absent_since > AUTO_ABSENT_US &&
                           in_mode > AUTO_DWELL_US) {
                    next = MMWAVE_C1001_MODE_FALL; why = "auto: room empty";
                }
            }
            if (why) {
                c1001_apply_mode(next, why);
                mode_since = now = esp_timer_get_time();
                q_idx = 0;
                still_since = active_since = absent_since = 0;
                if (next == MMWAVE_C1001_MODE_FALL) geom_left = 3;
            }
        }

        if (s_state.type == MMWAVE_TYPE_C1001 && now >= next_query_us) {
            bool fall = s_c1001_mode == MMWAVE_C1001_MODE_FALL;
            if (fall && geom_left == 0 && now >= next_geom_us) geom_left = 3;
            if (fall && geom_left > 0) {
                /* Installation geometry: on entering fall mode, then each 60 s. */
                const uint8_t *g = q_geom[3 - geom_left];
                c1001_send_query(g[0], g[1]);
                if (--geom_left == 0) next_geom_us = now + 60 * 1000000LL;
            } else {
                const uint8_t (*q)[2] = fall ? q_fall : q_vitals;
                int n = fall ? (int)(sizeof(q_fall) / sizeof(q_fall[0]))
                             : (int)(sizeof(q_vitals) / sizeof(q_vitals[0]));
                q_idx %= n;
                c1001_send_query(q[q_idx][0], q[q_idx][1]);
                q_idx = (q_idx + 1) % n;
            }
            next_query_us = now + 120 * 1000;
        }

        /* MWJ console telemetry (2 Hz): one JSON line on the USB console so
         * host tools (mmwave_radar.py TUI, herdr sidebar) see the live state
         * without the network. Format is stable; parsers key on "MWJ {". */
        static int64_t next_mwd_us = 0;
        if (s_state.type == MMWAVE_TYPE_C1001 && esp_timer_get_time() >= next_mwd_us) {
            next_mwd_us = esp_timer_get_time() + 2000 * 1000;
            char line[C1001_DBG_N * 20 + 8];
            int n = snprintf(line, sizeof(line), "MWD");
            for (int i = 0; i < C1001_DBG_N; i++) {
                int k = (s_c1001_dbg_idx + i) % C1001_DBG_N;
                if (s_c1001_dbg[k].len == 0) continue;
                n += snprintf(line + n, sizeof(line) - n, " %02X/%02X:%u:%02X%02X",
                              s_c1001_dbg[k].con, s_c1001_dbg[k].cmd,
                              (unsigned)s_c1001_dbg[k].len,
                              s_c1001_dbg[k].d0, s_c1001_dbg[k].d1);
            }
            printf("%s\n", line);
        }

        if (esp_timer_get_time() >= next_mwj_us) {
            next_mwj_us = esp_timer_get_time() + 500 * 1000;
            mmwave_state_t mw;
            if (mmwave_sensor_get_state(&mw)) {
                /* Track point only while fresh (<2 s); null otherwise. */
                char xy[40] = "\"x_cm\":null,\"y_cm\":null";
                if (mw.track_valid && esp_timer_get_time() - mw.track_us < 2000000)
                    snprintf(xy, sizeof(xy), "\"x_cm\":%d,\"y_cm\":%d",
                             mw.track_x_cm, mw.track_y_cm);
                char geo[64] = "";
                if (mw.geom_valid)
                    snprintf(geo, sizeof(geo),
                             ",\"inst_h\":%u,\"auto_h\":%u,\"ang\":[%d,%d,%d]",
                             mw.install_height_cm, mw.auto_height_cm,
                             mw.install_angle_x, mw.install_angle_y,
                             mw.install_angle_z);
                printf("MWJ {\"type\":\"%s\",\"mode\":\"%s\",\"policy\":\"%s\","
                       "\"motion\":%d,\"present\":%s,\"dist_cm\":%.0f,"
                       "%s,\"targets\":%u,\"hr\":%.1f,\"br\":%.1f,\"fall\":%s,"
                       "\"frames\":%lu,\"errors\":%lu%s}\n",
                       mmwave_type_name(mw.type),
                       mw.type != MMWAVE_TYPE_C1001 ? "-"
                       : s_c1001_mode == MMWAVE_C1001_MODE_FALL ? "fall" : "vitals",
                       mw.type != MMWAVE_TYPE_C1001 ? "-"
                       : s_c1001_policy == MMWAVE_C1001_POLICY_AUTO ? "auto"
                       : s_c1001_policy == MMWAVE_C1001_POLICY_FALL ? "fall" : "vitals",
                       mw.motion == 0xFF ? -1 : (int)mw.motion,
                       mw.person_present ? "true" : "false",
                       (double)mw.distance_cm,
                       xy,
                       (unsigned)mw.target_count,
                       (double)mw.heart_rate_bpm,
                       (double)mw.breathing_rate,
                       mw.fall_detected ? "true" : "false",
                       (unsigned long)mw.frame_count,
                       (unsigned long)mw.error_count, geo);
            }
        }

#if CONFIG_DISPLAY_STICKS3
        c1001_io_pins_poll();
#endif

        int len = uart_read_bytes(MMWAVE_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (len <= 0) {
            vTaskDelay(1);
            continue;
        }

        for (int i = 0; i < len; i++) {
            if (s_state.type == MMWAVE_TYPE_MR60BHA2) {
                mr60_feed_byte(buf[i]);
            } else if (s_state.type == MMWAVE_TYPE_LD2410) {
                ld2410_feed_byte(buf[i]);
            } else if (s_state.type == MMWAVE_TYPE_C1001) {
                c1001_feed_byte(buf[i]);
            }
        }

        vTaskDelay(1);
    }

    vTaskDelete(NULL);
}

/* Candidate pin sets shared with the deferred re-probe task. */
static int s_pin_sets[5][2];
static int s_n_pin_sets;

#if CONFIG_DISPLAY_STICKS3 || CONFIG_DISPLAY_S3BOX3
/* Electrical diagnosis: a pin actively driven by the radar holds its level
 * regardless of our pull; a floating (unconnected) pin follows the pull. */
static void c1001_io_pin_diagnose(int pin)
{
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, GPIO_PULLDOWN_ONLY);
    vTaskDelay(pdMS_TO_TICKS(10));
    int with_down = gpio_get_level(pin);
    gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
    vTaskDelay(pdMS_TO_TICKS(10));
    int with_up = gpio_get_level(pin);
    const char *verdict = (with_down == with_up)
        ? (with_down ? "DRIVEN-HIGH" : "DRIVEN-LOW")
        : "FLOATING";
    ESP_LOGI(TAG, "C1001 IO G%d: pulldown=%d pullup=%d -> %s",
             pin, with_down, with_up, verdict);
    diag_log("G%d=%s; ", pin, verdict);
}
#endif /* CONFIG_DISPLAY_STICKS3 || CONFIG_DISPLAY_S3BOX3 */

#if CONFIG_DISPLAY_STICKS3
/* C1001 hardware presence/event outputs wired to the StickS3 HAT header. */
#define C1001_IO_PIN_A  5
#define C1001_IO_PIN_B  7

static void c1001_io_pins_init(void)
{
    c1001_io_pin_diagnose(C1001_IO_PIN_A);
    c1001_io_pin_diagnose(C1001_IO_PIN_B);
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << C1001_IO_PIN_A) | (1ULL << C1001_IO_PIN_B),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,  /* radar drives high on event */
    };
    gpio_config(&io_cfg);
}

/* Poll the radar's digital outputs. Logs transitions so the pin semantics
 * can be confirmed against observed behavior; feeds presence as a fallback
 * when the UART link has been silent for 5 s. */
static void c1001_io_pins_poll(void)
{
    static int last_a = -1, last_b = -1;
    int a = gpio_get_level(C1001_IO_PIN_A);
    int b = gpio_get_level(C1001_IO_PIN_B);
    if (a != last_a || b != last_b) {
        ESP_LOGI(TAG, "C1001 IO pins: G%d=%d G%d=%d",
                 C1001_IO_PIN_A, a, C1001_IO_PIN_B, b);
        last_a = a; last_b = b;
    }
    int64_t uart_age_us = esp_timer_get_time() - s_state.last_update_us;
    if (uart_age_us > 5 * 1000 * 1000) {
        s_state.person_present = (a != 0) || (b != 0);
    }
}
#endif /* CONFIG_DISPLAY_STICKS3 */

static mmwave_type_t probe_all_pinsets(void)
{
    mmwave_type_t detected = MMWAVE_TYPE_NONE;
    for (int p = 0; p < s_n_pin_sets && detected == MMWAVE_TYPE_NONE; p++) {
        uart_set_pin(MMWAVE_UART_NUM, s_pin_sets[p][0], s_pin_sets[p][1],
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        ESP_LOGI(TAG, "Probing UART%d (TX=%d, RX=%d) for mmWave sensor...",
                 MMWAVE_UART_NUM, s_pin_sets[p][0], s_pin_sets[p][1]);
        detected = probe_sensor();
    }
    return detected;
}

/* Configure state for a detected sensor and start the parser task. */
static esp_err_t mmwave_finalize(mmwave_type_t detected)
{
    uint32_t final_baud = (detected == MMWAVE_TYPE_LD2410)
                          ? MMWAVE_LD2410_BAUD : MMWAVE_MR60_BAUD;
    uart_set_baudrate(MMWAVE_UART_NUM, final_baud);

    s_state.type = detected;
    s_state.detected = true;

    switch (detected) {
    case MMWAVE_TYPE_MR60BHA2:
        s_state.capabilities = MMWAVE_CAP_HEART_RATE | MMWAVE_CAP_BREATHING
                             | MMWAVE_CAP_PRESENCE | MMWAVE_CAP_DISTANCE;
        break;
    case MMWAVE_TYPE_LD2410:
        s_state.capabilities = MMWAVE_CAP_PRESENCE | MMWAVE_CAP_DISTANCE;
        break;
    case MMWAVE_TYPE_C1001:
        s_state.capabilities = MMWAVE_CAP_HEART_RATE | MMWAVE_CAP_BREATHING
                             | MMWAVE_CAP_PRESENCE | MMWAVE_CAP_FALL;
        break;
    default:
        break;
    }

    ESP_LOGI(TAG, "Detected %s at %lu baud (caps=0x%04x)",
             mmwave_type_name(detected), (unsigned long)final_baud,
             s_state.capabilities);

    BaseType_t ret = xTaskCreatePinnedToCore(
        mmwave_uart_task, "mmwave_uart", MMWAVE_TASK_STACK,
        NULL, MMWAVE_TASK_PRIORITY, NULL, 0);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create mmWave UART task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* Deferred re-probe for sensors that boot slower than the firmware. */
static void mmwave_retry_task(void *arg)
{
    (void)arg;
    for (int round = 0; round < 3 && s_running; round++) {
        /* Wait ~12 s between rounds, watching the radar IO pins meanwhile. */
        for (int t = 0; t < 60 && s_running; t++) {
#if CONFIG_DISPLAY_STICKS3
            c1001_io_pins_poll();
#endif
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        mmwave_type_t detected = probe_all_pinsets();
        if (detected != MMWAVE_TYPE_NONE) {
            ESP_LOGI(TAG, "Deferred probe round %d found a sensor", round + 1);
            mmwave_finalize(detected);
            vTaskDelete(NULL);
            return;
        }
    }
    ESP_LOGI(TAG, "Deferred probes exhausted — no mmWave sensor on UART");
    uart_driver_delete(MMWAVE_UART_NUM);

#if CONFIG_DISPLAY_STICKS3
    /* UART dead but the radar's hardware presence outputs may still work.
     * Watch G5/G7: first activity switches us into GPIO-only presence mode. */
    ESP_LOGI(TAG, "Monitoring C1001 IO pins (G%d/G%d) for GPIO-only presence",
             C1001_IO_PIN_A, C1001_IO_PIN_B);
    bool announced = false;
    while (s_running) {
        c1001_io_pins_poll();
        if (!announced && s_state.person_present) {
            s_state.type = MMWAVE_TYPE_C1001;
            s_state.detected = true;
            s_state.capabilities = MMWAVE_CAP_PRESENCE;
            ESP_LOGI(TAG, "C1001 GPIO-only presence mode active");
            announced = true;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
#endif
    vTaskDelete(NULL);
}

#endif /* !CONFIG_CSI_MOCK_ENABLED */

/* ======================================================================
 * Public API
 * ====================================================================== */

const char *mmwave_type_name(mmwave_type_t type)
{
    switch (type) {
    case MMWAVE_TYPE_MR60BHA2: return "MR60BHA2";
    case MMWAVE_TYPE_LD2410:   return "LD2410";
    case MMWAVE_TYPE_C1001:    return "C1001";
    case MMWAVE_TYPE_AT581X:   return "AT581x";
    case MMWAVE_TYPE_MOCK:     return "Mock";
    case MMWAVE_TYPE_NONE:
    default:                   return "None";
    }
}

/* ===================================================================== */
/* AT581x 5.8 GHz micro-motion PRESENCE radar (ESP32-S3-BOX-3-SENSOR dock) */
/*                                                                         */
/* The AT581x is a presence/motion sensor ONLY — it has no heart-rate or   */
/* breathing capability. On the BOX-3-SENSOR dock it drives a digital      */
/* presence OUT line on GPIO21 (I2C on GPIO40/41, shared with the AHT30,    */
/* only tunes thresholds). We read the OUT line directly at the module's    */
/* default sensitivity and feed the shared mmWave state so the LCD and      */
/* edge reporting show presence with zero extra wiring.                     */
/* ===================================================================== */
#define AT581X_PRESENCE_GPIO   21
#define AT581X_POLL_MS         100
#define AT581X_HOLD_MS         1500   /* hold "present" this long after last HIGH */

static void at581x_task(void *arg)
{
    (void)arg;
    int64_t last_high_us = 0;
    bool last_present = false;
    while (s_running) {
        int64_t now = esp_timer_get_time();
        if (gpio_get_level(AT581X_PRESENCE_GPIO)) last_high_us = now;
        bool present = (now - last_high_us) < (int64_t)AT581X_HOLD_MS * 1000;
        s_state.person_present = present;
        s_state.target_count = present ? 1 : 0;
        s_state.distance_cm = 0.0f;   /* AT581x gives no range */
        s_state.frame_count++;
        s_state.last_update_us = now;
        if (present != last_present) {
            ESP_LOGI(TAG, "AT581x presence: %s", present ? "DETECTED" : "clear");
            last_present = present;
        }
        vTaskDelay(pdMS_TO_TICKS(AT581X_POLL_MS));
    }
    vTaskDelete(NULL);
}

static esp_err_t at581x_presence_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << AT581X_PRESENCE_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,  /* idle LOW when nothing detected */
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AT581x GPIO%d config failed: %s",
                 AT581X_PRESENCE_GPIO, esp_err_to_name(err));
        return err;
    }
    s_state.type          = MMWAVE_TYPE_AT581X;
    s_state.detected      = true;
    s_state.capabilities  = MMWAVE_CAP_PRESENCE;
    s_state.heart_rate_bpm = 0.0f;   /* not capable */
    s_state.breathing_rate = 0.0f;   /* not capable */
    s_state.last_update_us = esp_timer_get_time();
    ESP_LOGI(TAG, "AT581x 5.8GHz presence radar active on GPIO%d "
             "(presence/motion only — no HR/BR)", AT581X_PRESENCE_GPIO);
    BaseType_t r = xTaskCreatePinnedToCore(
        at581x_task, "at581x", MMWAVE_TASK_STACK, NULL,
        MMWAVE_TASK_PRIORITY, NULL, 0);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "AT581x task create failed");
        s_state.detected = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t mmwave_sensor_init(int uart_tx_pin, int uart_rx_pin)
{
    memset(&s_state, 0, sizeof(s_state));
    memset(&s_mr60, 0, sizeof(s_mr60));
    memset(&s_ld, 0, sizeof(s_ld));
    memset(&s_c1001, 0, sizeof(s_c1001));
    s_running = true;

#ifdef CONFIG_CSI_MOCK_ENABLED
    ESP_LOGI(TAG, "Mock mode: starting synthetic mmWave generator");

    BaseType_t ret = xTaskCreatePinnedToCore(
        mock_mmwave_task, "mmwave_mock", MMWAVE_TASK_STACK,
        NULL, MMWAVE_TASK_PRIORITY, NULL, 0);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create mock mmWave task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;

#else
#if CONFIG_DISPLAY_S3BOX3
    /* ESP32-S3-BOX-3-SENSOR dock: the onboard radar is an AT581x 5.8 GHz
     * presence sensor (not a UART vitals radar). Use it directly, unless the
     * caller passed explicit UART pins for an external MR60/C1001/LD2410. */
    if (uart_tx_pin < 0 && uart_rx_pin < 0) {
        if (at581x_presence_init() == ESP_OK) return ESP_OK;
        ESP_LOGW(TAG, "AT581x init failed — falling back to UART sensor probe");
    }
#endif
    /* Candidate (TX, RX) pin pairs. On the StickS3 the sensor hangs off the
     * Grove PORT.A connector (G9/G10) with unknown orientation; other boards
     * use the legacy UART1 pins 17/18. Explicit pins override the list. */
    int pin_sets[5][2];
    int n_pin_sets;
    if (uart_tx_pin >= 0 && uart_rx_pin >= 0) {
        pin_sets[0][0] = uart_tx_pin; pin_sets[0][1] = uart_rx_pin;
        n_pin_sets = 1;
    } else {
#if CONFIG_DISPLAY_STICKS3
        /* Grove PORT.A (G9/G10) first — the node-2 C1001 wiring — then the
         * HAT header G43/G44 (console is on USB-Serial-JTAG, so UART0's
         * default pins are free). G17/G18 are NOT probed here: on StickS3
         * they are the speaker's I2S pins, and a background re-probe would
         * reconfigure them under the codec. */
        pin_sets[0][0] = 10; pin_sets[0][1] = 9;
        pin_sets[1][0] = 9;  pin_sets[1][1] = 10;
        pin_sets[2][0] = 43; pin_sets[2][1] = 44;
        pin_sets[3][0] = 44; pin_sets[3][1] = 43;
        n_pin_sets = 4;
#elif CONFIG_DISPLAY_S3BOX3
        /* BOX-3 dock: PMOD2 exposes GPIO43/44 (default UART), PMOD1 has
         * GPIO42/21 as the fallback pair; console is USB-Serial-JTAG so
         * 43/44 are free. Legacy 17/18 last. */
        pin_sets[0][0] = 43; pin_sets[0][1] = 44;
        pin_sets[1][0] = 44; pin_sets[1][1] = 43;
        pin_sets[2][0] = 42; pin_sets[2][1] = 21;
        pin_sets[3][0] = 21; pin_sets[3][1] = 42;
        pin_sets[4][0] = 17; pin_sets[4][1] = 18;
        n_pin_sets = 5;
#else
        pin_sets[0][0] = 17; pin_sets[0][1] = 18;
        n_pin_sets = 1;
#endif
    }

#if CONFIG_DISPLAY_STICKS3
    c1001_io_pins_init();
    /* UART wire check before the driver claims the pins: a connected idle
     * UART TX from the sensor holds the line HIGH; floating = no wire. */
    c1001_io_pin_diagnose(43);
    c1001_io_pin_diagnose(44);
#elif CONFIG_DISPLAY_S3BOX3
    /* BOX-3 dock wire check: PMOD2 UART pair + PMOD1 fallback pair. */
    c1001_io_pin_diagnose(43);
    c1001_io_pin_diagnose(44);
    c1001_io_pin_diagnose(42);
    c1001_io_pin_diagnose(21);
#endif

    /* Install UART driver at MR60 baud (will be changed during probe). */
    uart_config_t uart_config = {
        .baud_rate = MMWAVE_MR60_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(MMWAVE_UART_NUM, MMWAVE_BUF_SIZE * 2,
                                        MMWAVE_BUF_SIZE * 2, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART driver install failed: %s", esp_err_to_name(err));
        return err;
    }
    uart_param_config(MMWAVE_UART_NUM, &uart_config);

    memcpy(s_pin_sets, pin_sets, sizeof(pin_sets));
    s_n_pin_sets = n_pin_sets;

    mmwave_type_t detected = probe_all_pinsets();

    if (detected == MMWAVE_TYPE_NONE) {
        /* Slow-boot sensors (C1001 needs ~10 s) can miss the boot-time
         * probe — keep the driver and retry in the background. */
        ESP_LOGI(TAG, "No mmWave sensor yet on UART%d — deferred re-probe scheduled",
                 MMWAVE_UART_NUM);
        BaseType_t rret = xTaskCreatePinnedToCore(
            mmwave_retry_task, "mmwave_retry", MMWAVE_TASK_STACK,
            NULL, MMWAVE_TASK_PRIORITY, NULL, 0);
        if (rret != pdPASS) uart_driver_delete(MMWAVE_UART_NUM);
        return ESP_ERR_NOT_FOUND;
    }

    return mmwave_finalize(detected);
#endif
}

bool mmwave_sensor_get_state(mmwave_state_t *state)
{
    if (!s_state.detected || state == NULL) return false;
    memcpy(state, &s_state, sizeof(mmwave_state_t));
    return true;
}
