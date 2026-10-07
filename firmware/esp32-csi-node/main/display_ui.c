/**
 * @file display_ui.c
 * @brief ADR-045: LVGL 4-view swipeable UI — Dashboard | Vitals | Presence | System.
 *
 * Dark theme (#0a0a0f background) with cyan (#00d4ff) accent.
 * Glowing line effects via layered semi-transparent chart series.
 */

#include "display_ui.h"
#include "nvs_config.h"
#include "sdkconfig.h"

extern nvs_config_t g_nvs_config;

#if CONFIG_DISPLAY_ENABLE

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "edge_processing.h"
#include "mmwave_sensor.h"
#include "display_hal.h"

static const char *TAG = "disp_ui";

/* ---- Theme colors ---- */
#define COLOR_BG        lv_color_make(0x0A, 0x0A, 0x0F)
#define COLOR_CYAN      lv_color_make(0x00, 0xD4, 0xFF)
#define COLOR_AMBER     lv_color_make(0xFF, 0xB0, 0x00)
#define COLOR_GREEN     lv_color_make(0x00, 0xFF, 0x80)
#define COLOR_RED       lv_color_make(0xFF, 0x40, 0x40)
#define COLOR_DIM       lv_color_make(0x30, 0x30, 0x40)
#define COLOR_TEXT       lv_color_make(0xCC, 0xCC, 0xDD)
#define COLOR_TEXT_DIM   lv_color_make(0x66, 0x66, 0x77)

/* ---- Screen resolution for layout scaling ---- */
#if CONFIG_IDF_TARGET_ESP32C6
#define SCR_W  172
#define SCR_H  320
#elif CONFIG_DISPLAY_STICKS3
#define SCR_W  135
#define SCR_H  240
#elif CONFIG_DISPLAY_S3BOX3
#define SCR_W  320
#define SCR_H  240
#else
#define SCR_W  368
#define SCR_H  448
#endif

#if CONFIG_DISPLAY_STICKS3 || CONFIG_DISPLAY_S3BOX3
/* ===========================================================================
 * StickS3 / S3-BOX-3 simple status UI — one screen, essentials, no charts.
 *
 * The full 4-view dashboard starves CPU0 on this board (task_wdt) and is
 * unreadable at 135x240 anyway. Show the most important information plus
 * enough setup info (IP, OTA port) to find and manage the node.
 * =========================================================================*/

#include "esp_netif.h"

/* Card / accent palette (matches the approved 135x240 mock) */
#define CARD_BG      lv_color_make(0x12, 0x12, 0x1A)
#define CARD_BORDER  lv_color_make(0x26, 0x26, 0x36)
#define BAR_OFF      lv_color_make(0x30, 0x30, 0x40)

/* Layout grid */
#define PAD          6
#define CARD_W       (SCR_W - 2 * PAD)           /* 123 */
#define VCARD_W      ((SCR_W - 2 * PAD - 5) / 2) /* 59  */

static lv_obj_t *s_hero_card    = NULL;
static lv_obj_t *s_lbl_state    = NULL;   /* OCCUPIED / EMPTY / joining */
static lv_obj_t *s_lbl_substate = NULL;   /* "1 person" / "watching"    */
static lv_obj_t *s_lbl_br_val   = NULL;
static lv_obj_t *s_lbl_hr_val   = NULL;
static lv_obj_t *s_lbl_rssi     = NULL;
static lv_obj_t *s_lbl_ip       = NULL;
static lv_obj_t *s_lbl_target   = NULL;
static lv_obj_t *s_lbl_uptime   = NULL;
static lv_obj_t *s_lbl_batt     = NULL;   /* battery % (top-right) */
static lv_obj_t *s_wifi_bars[4] = {NULL};

static lv_obj_t *mk_card(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, CARD_BG, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, CARD_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
    return card;
}

static lv_obj_t *mk_text(lv_obj_t *parent, int x, int y, int w,
                         const lv_font_t *font, lv_color_t color,
                         lv_text_align_t align, const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_style_text_align(lbl, align, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(lbl, w);
    lv_obj_set_pos(lbl, x, y);
    return lbl;
}

void display_ui_create(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);

    /* ---- Header: node identity, hairline below ---- */
    char node[12];
    snprintf(node, sizeof(node), "NODE %d", g_nvs_config.node_id);
    mk_text(parent, PAD + 2, 7, SCR_W - 2 * PAD - 4, &lv_font_montserrat_14,
            COLOR_CYAN, LV_TEXT_ALIGN_LEFT, node);
    /* Battery % — right-aligned on the header row (populated in update). */
    s_lbl_batt = mk_text(parent, PAD + 2, 7, SCR_W - 2 * PAD - 4,
                         &lv_font_montserrat_14, COLOR_TEXT_DIM,
                         LV_TEXT_ALIGN_RIGHT, "");
    lv_obj_t *rule = lv_obj_create(parent);
    lv_obj_set_pos(rule, PAD + 2, 26);
    lv_obj_set_size(rule, CARD_W - 4, 1);
    lv_obj_set_style_bg_color(rule, CARD_BORDER, 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rule, 0, 0);
    lv_obj_set_style_radius(rule, 0, 0);

    /* ---- Hero: room presence ---- */
    s_hero_card = mk_card(parent, PAD, 33, CARD_W, 58);
    mk_text(parent, 0, 41, SCR_W, &lv_font_montserrat_12, COLOR_TEXT_DIM,
            LV_TEXT_ALIGN_CENTER, "ROOM");
    s_lbl_state = mk_text(parent, 0, 57, SCR_W, &lv_font_montserrat_20,
                          COLOR_TEXT_DIM, LV_TEXT_ALIGN_CENTER, "...");
    s_lbl_substate = mk_text(parent, 0, 78, SCR_W, &lv_font_montserrat_12,
                             COLOR_TEXT_DIM, LV_TEXT_ALIGN_CENTER, "starting");

    /* ---- Vitals: breathing + heart rate cards ---- */
    int vx2 = PAD + VCARD_W + 5;
    mk_card(parent, PAD, 97, VCARD_W, 64);
    mk_card(parent, vx2, 97, VCARD_W, 64);
    mk_text(parent, PAD, 104, VCARD_W, &lv_font_montserrat_12, COLOR_CYAN,
            LV_TEXT_ALIGN_CENTER, "BR");
    s_lbl_br_val = mk_text(parent, PAD, 118, VCARD_W, &lv_font_montserrat_28,
                           COLOR_TEXT, LV_TEXT_ALIGN_CENTER, "--");
    mk_text(parent, PAD, 148, VCARD_W, &lv_font_montserrat_12, COLOR_TEXT_DIM,
            LV_TEXT_ALIGN_CENTER, "bpm");
    mk_text(parent, vx2, 104, VCARD_W, &lv_font_montserrat_12, COLOR_AMBER,
            LV_TEXT_ALIGN_CENTER, "HR");
    s_lbl_hr_val = mk_text(parent, vx2, 118, VCARD_W, &lv_font_montserrat_28,
                           COLOR_TEXT, LV_TEXT_ALIGN_CENTER, "--");
    mk_text(parent, vx2, 148, VCARD_W, &lv_font_montserrat_12, COLOR_TEXT_DIM,
            LV_TEXT_ALIGN_CENTER, "bpm");

    /* ---- Network card: WiFi bars + RSSI + IP ---- */
    mk_card(parent, PAD, 167, CARD_W, 46);
    static const int bar_h[4] = {4, 7, 10, 13};
    for (int i = 0; i < 4; i++) {
        s_wifi_bars[i] = lv_obj_create(parent);
        lv_obj_set_pos(s_wifi_bars[i], 14 + i * 6, 188 - bar_h[i]);
        lv_obj_set_size(s_wifi_bars[i], 4, bar_h[i]);
        lv_obj_set_style_bg_color(s_wifi_bars[i], BAR_OFF, 0);
        lv_obj_set_style_bg_opa(s_wifi_bars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_wifi_bars[i], 0, 0);
        lv_obj_set_style_radius(s_wifi_bars[i], 1, 0);
    }
    s_lbl_rssi = mk_text(parent, 44, 176, CARD_W - 44, &lv_font_montserrat_14,
                         COLOR_TEXT, LV_TEXT_ALIGN_LEFT, "--");
    s_lbl_ip = mk_text(parent, 14, 196, CARD_W - 16, &lv_font_montserrat_12,
                       COLOR_TEXT_DIM, LV_TEXT_ALIGN_LEFT, "no network");

    /* ---- Footer: stream target + uptime ---- */
    s_lbl_target = mk_text(parent, PAD + 2, SCR_H - 18, 84,
                           &lv_font_montserrat_12, COLOR_TEXT_DIM,
                           LV_TEXT_ALIGN_LEFT, "");
    s_lbl_uptime = mk_text(parent, SCR_W - PAD - 44, SCR_H - 18, 42,
                           &lv_font_montserrat_12, COLOR_TEXT_DIM,
                           LV_TEXT_ALIGN_RIGHT, "");

    ESP_LOGI(TAG, "UI created: StickS3 card status view");
}

void display_ui_update(void)
{
    /* Refresh at 2 Hz — rendering stays cheap between updates */
    static uint32_t s_last_ms = 0;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (now_ms - s_last_ms < 500) return;
    s_last_ms = now_ms;

    char buf[40];

    /* Network state — the IP line doubles as the setup hint */
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {0};
    bool has_ip = sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK
                  && ip.ip.addr != 0;

    if (g_nvs_config.wifi_ssid[0] == '\0') {
        lv_label_set_text(s_lbl_ip, "SETUP: provision.py");
    } else if (!has_ip) {
        lv_label_set_text(s_lbl_ip, "joining WiFi...");
    } else {
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
        lv_label_set_text(s_lbl_ip, buf);
    }

    /* Stream target, shortened to the tail of the IP */
    const char *tail = strrchr(g_nvs_config.target_ip, '.');
    snprintf(buf, sizeof(buf), LV_SYMBOL_PLAY " %s:%u",
             tail ? tail : g_nvs_config.target_ip,
             (unsigned)g_nvs_config.target_port);
    lv_label_set_text(s_lbl_target, buf);

    edge_vitals_pkt_t vitals;
    bool has_vitals = edge_get_vitals(&vitals);

    /* ---- Hero presence state (AT581x radar + CSI edge, works offline) ---- */
    mmwave_state_t mw;
    bool has_mw = mmwave_sensor_get_state(&mw);
    bool present = (has_vitals && ((vitals.flags & 0x01) || vitals.n_persons > 0))
                   || (has_mw && mw.person_present);
    if (present) {
        lv_label_set_text(s_lbl_state, "OCCUPIED");
        lv_obj_set_style_text_color(s_lbl_state, COLOR_GREEN, 0);
        lv_obj_set_style_border_color(s_hero_card, COLOR_GREEN, 0);
        if (has_vitals && vitals.n_persons > 1)
            snprintf(buf, sizeof(buf), "%u persons", vitals.n_persons);
        else
            snprintf(buf, sizeof(buf), "1 person");
        lv_label_set_text(s_lbl_substate, buf);
        lv_obj_set_style_text_color(s_lbl_substate, COLOR_GREEN, 0);
    } else if (!has_ip && g_nvs_config.wifi_ssid[0] != '\0' && !has_mw) {
        /* Only show JOINING when there's no local sensor to report presence. */
        lv_label_set_text(s_lbl_state, "JOINING");
        lv_obj_set_style_text_color(s_lbl_state, COLOR_AMBER, 0);
        lv_label_set_text(s_lbl_substate, g_nvs_config.wifi_ssid);
        lv_obj_set_style_border_color(s_hero_card, CARD_BORDER, 0);
    } else {
        lv_label_set_text(s_lbl_state, "EMPTY");
        lv_obj_set_style_text_color(s_lbl_state, COLOR_TEXT_DIM, 0);
        lv_obj_set_style_border_color(s_hero_card, CARD_BORDER, 0);
        lv_label_set_text(s_lbl_substate, has_mw ? mmwave_type_name(mw.type) : "watching");
        lv_obj_set_style_text_color(s_lbl_substate, COLOR_TEXT_DIM, 0);
    }

    /* ---- Vitals ---- */
    if (has_vitals) {
        bool present = (vitals.flags & 0x01) || vitals.n_persons > 0;
        int br = (int)(vitals.breathing_rate / 100);
        int hr = (int)(vitals.heartrate / 10000);
        if (present && br > 0) {
            snprintf(buf, sizeof(buf), "%d", br);
            lv_label_set_text(s_lbl_br_val, buf);
            lv_obj_set_style_text_color(s_lbl_br_val, COLOR_TEXT, 0);
        } else {
            lv_label_set_text(s_lbl_br_val, "--");
            lv_obj_set_style_text_color(s_lbl_br_val, COLOR_TEXT_DIM, 0);
        }
        if (present && hr > 0) {
            snprintf(buf, sizeof(buf), "%d", hr);
            lv_label_set_text(s_lbl_hr_val, buf);
            lv_obj_set_style_text_color(s_lbl_hr_val, COLOR_TEXT, 0);
        } else {
            lv_label_set_text(s_lbl_hr_val, "--");
            lv_obj_set_style_text_color(s_lbl_hr_val, COLOR_TEXT_DIM, 0);
        }

        /* ---- Signal bars + RSSI ---- */
        snprintf(buf, sizeof(buf), "%d dBm", vitals.rssi);
        lv_label_set_text(s_lbl_rssi, buf);
        static const int thresh[4] = {-85, -75, -65, -55};
        for (int i = 0; i < 4; i++) {
            lv_obj_set_style_bg_color(s_wifi_bars[i],
                vitals.rssi >= thresh[i] ? COLOR_CYAN : BAR_OFF, 0);
        }
    }

#if CONFIG_DISPLAY_STICKS3
    /* ---- Battery (M5PM1 PMU), refreshed ~every 3 s ---- */
    static int s_batt_div = 0;
    if (s_lbl_batt && (s_batt_div++ % 30) == 0) {
        uint8_t bpct = 0;
        if (display_hal_battery(NULL, &bpct) == ESP_OK) {
            snprintf(buf, sizeof(buf), "%u%%", (unsigned)bpct);
            lv_label_set_text(s_lbl_batt, buf);
            lv_color_t bc = bpct > 50 ? COLOR_GREEN : (bpct > 20 ? COLOR_AMBER : COLOR_RED);
            lv_obj_set_style_text_color(s_lbl_batt, bc, 0);
        }
    }
#endif

    /* ---- Uptime ---- */
    uint32_t up_min = now_ms / 60000;
    if (up_min < 60)
        snprintf(buf, sizeof(buf), "%lum", (unsigned long)up_min);
    else
        snprintf(buf, sizeof(buf), "%luh%02lu", (unsigned long)(up_min / 60),
                 (unsigned long)(up_min % 60));
    lv_label_set_text(s_lbl_uptime, buf);
}

#else /* !CONFIG_DISPLAY_STICKS3 — full 4-view dashboard */

/* ---- Chart data points ---- */
#define CHART_POINTS    60

/* ---- View handles ---- */
static lv_obj_t *s_tileview = NULL;

/* Dashboard */
static lv_obj_t *s_dash_chart      = NULL;
static lv_chart_series_t *s_csi_series = NULL;
static lv_obj_t *s_dash_persons    = NULL;
static lv_obj_t *s_dash_rssi       = NULL;
static lv_obj_t *s_dash_motion     = NULL;

/* Vitals */
static lv_obj_t *s_vital_chart     = NULL;
static lv_chart_series_t *s_breath_series = NULL;
static lv_chart_series_t *s_hr_series     = NULL;
static lv_obj_t *s_vital_bpm_br    = NULL;
static lv_obj_t *s_vital_bpm_hr    = NULL;

/* Presence */
#define GRID_COLS  4
#define GRID_ROWS  4
static lv_obj_t *s_grid_cells[GRID_COLS * GRID_ROWS];
static lv_obj_t *s_presence_label = NULL;

/* System */
static lv_obj_t *s_sys_cpu         = NULL;
static lv_obj_t *s_sys_heap        = NULL;
static lv_obj_t *s_sys_psram       = NULL;
static lv_obj_t *s_sys_rssi        = NULL;
static lv_obj_t *s_sys_uptime      = NULL;
static lv_obj_t *s_sys_fps         = NULL;
static lv_obj_t *s_sys_node        = NULL;

/* ---- Style helpers ---- */
static lv_style_t s_style_bg;
static lv_style_t s_style_label;
static lv_style_t s_style_label_big;
static bool s_styles_inited = false;

static void init_styles(void)
{
    if (s_styles_inited) return;
    s_styles_inited = true;

    lv_style_init(&s_style_bg);
    lv_style_set_bg_color(&s_style_bg, COLOR_BG);
    lv_style_set_bg_opa(&s_style_bg, LV_OPA_COVER);
    lv_style_set_border_width(&s_style_bg, 0);
    lv_style_set_pad_all(&s_style_bg, 4);

    lv_style_init(&s_style_label);
    lv_style_set_text_color(&s_style_label, COLOR_TEXT);
    lv_style_set_text_font(&s_style_label, &lv_font_montserrat_14);

    lv_style_init(&s_style_label_big);
    lv_style_set_text_color(&s_style_label_big, COLOR_CYAN);
    lv_style_set_text_font(&s_style_label_big, &lv_font_montserrat_14);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_style_t *style)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    if (style) lv_obj_add_style(lbl, (lv_style_t *)style, 0);
    return lbl;
}

static lv_obj_t *make_tile(lv_obj_t *tv, uint8_t col, uint8_t row)
{
    lv_obj_t *tile = lv_tileview_add_tile(tv, col, row, LV_DIR_HOR);
    lv_obj_add_style(tile, &s_style_bg, 0);
    return tile;
}

/* ---- View 0: Dashboard ---- */
static void create_dashboard(lv_obj_t *tile)
{
    make_label(tile, "CSI Dashboard", &s_style_label);

    /* CSI amplitude chart — scale to screen */
    s_dash_chart = lv_chart_create(tile);
    lv_obj_set_size(s_dash_chart, SCR_W - 8, (SCR_H < 340) ? 100 : 130);
    lv_obj_align(s_dash_chart, LV_ALIGN_TOP_LEFT, 0, 20);
    lv_chart_set_type(s_dash_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_dash_chart, CHART_POINTS);
    lv_chart_set_range(s_dash_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_obj_set_style_bg_color(s_dash_chart, COLOR_BG, 0);
    lv_obj_set_style_border_color(s_dash_chart, COLOR_DIM, 0);
    lv_obj_set_style_line_width(s_dash_chart, 0, LV_PART_TICKS);

    s_csi_series = lv_chart_add_series(s_dash_chart, COLOR_CYAN, LV_CHART_AXIS_PRIMARY_Y);

    /* Stats below chart on small screens, right panel on large */
    s_dash_persons = make_label(tile, "Persons: 0", &s_style_label_big);
    lv_obj_align(s_dash_persons, LV_ALIGN_BOTTOM_LEFT, 4, -50);

    s_dash_rssi = make_label(tile, "RSSI: --", &s_style_label);
    lv_obj_align(s_dash_rssi, LV_ALIGN_BOTTOM_LEFT, 4, -30);

    s_dash_motion = make_label(tile, "Motion: 0.0", &s_style_label);
    lv_obj_align(s_dash_motion, LV_ALIGN_BOTTOM_LEFT, 4, -10);
}

/* ---- View 1: Vitals ---- */
static void create_vitals(lv_obj_t *tile)
{
    make_label(tile, "Vital Signs", &s_style_label);

    s_vital_chart = lv_chart_create(tile);
    lv_obj_set_size(s_vital_chart, SCR_W - 8, (SCR_H < 340) ? 120 : 150);
    lv_obj_align(s_vital_chart, LV_ALIGN_TOP_LEFT, 0, 20);
    lv_chart_set_type(s_vital_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_vital_chart, CHART_POINTS);
    lv_chart_set_range(s_vital_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 120);
    lv_obj_set_style_bg_color(s_vital_chart, COLOR_BG, 0);
    lv_obj_set_style_border_color(s_vital_chart, COLOR_DIM, 0);
    lv_obj_set_style_line_width(s_vital_chart, 0, LV_PART_TICKS);

    /* Breathing series (cyan) */
    s_breath_series = lv_chart_add_series(s_vital_chart, COLOR_CYAN, LV_CHART_AXIS_PRIMARY_Y);
    /* Heart rate series (amber) */
    s_hr_series = lv_chart_add_series(s_vital_chart, COLOR_AMBER, LV_CHART_AXIS_PRIMARY_Y);

    /* BPM readouts */
    s_vital_bpm_br = make_label(tile, "Breathing: -- BPM", &s_style_label);
    lv_obj_align(s_vital_bpm_br, LV_ALIGN_BOTTOM_LEFT, 4, -8);
    lv_obj_set_style_text_color(s_vital_bpm_br, COLOR_CYAN, 0);

    s_vital_bpm_hr = make_label(tile, "Heart Rate: -- BPM", &s_style_label);
    lv_obj_align(s_vital_bpm_hr, LV_ALIGN_BOTTOM_RIGHT, -4, -8);
    lv_obj_set_style_text_color(s_vital_bpm_hr, COLOR_AMBER, 0);
}

/* ---- View 2: Presence Grid ---- */
static void create_presence(lv_obj_t *tile)
{
    make_label(tile, "Occupancy Map", &s_style_label);

    int cell_w = (SCR_W < 200) ? 32 : 50;
    int cell_h = (SCR_H < 340) ? 30 : 45;
    int x_off  = (SCR_W - GRID_COLS * (cell_w + 4)) / 2;
    int y_off  = 26;

    for (int r = 0; r < GRID_ROWS; r++) {
        for (int c = 0; c < GRID_COLS; c++) {
            lv_obj_t *cell = lv_obj_create(tile);
            lv_obj_set_size(cell, cell_w, cell_h);
            lv_obj_set_pos(cell, x_off + c * (cell_w + 4), y_off + r * (cell_h + 4));
            lv_obj_set_style_bg_color(cell, COLOR_DIM, 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(cell, COLOR_DIM, 0);
            lv_obj_set_style_border_width(cell, 1, 0);
            lv_obj_set_style_radius(cell, 4, 0);
            s_grid_cells[r * GRID_COLS + c] = cell;
        }
    }

    s_presence_label = make_label(tile, "Persons: 0", &s_style_label);
    lv_obj_align(s_presence_label, LV_ALIGN_BOTTOM_MID, 0, -8);
}

/* ---- View 3: System ---- */
static void create_system(lv_obj_t *tile)
{
    make_label(tile, "System Info", &s_style_label);

    lv_obj_t *panel = lv_obj_create(tile);
    lv_obj_set_size(panel, SCR_W - 8, (SCR_H < 340) ? 250 : 180);
    lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 0, 20);
    lv_obj_set_style_bg_color(panel, lv_color_make(0x12, 0x12, 0x1A), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, COLOR_DIM, 0);
    lv_obj_set_style_pad_all(panel, 10, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    s_sys_node   = make_label(panel, "Node: --",        &s_style_label);
    s_sys_cpu    = make_label(panel, "CPU: --%",         &s_style_label);
    s_sys_heap   = make_label(panel, "Heap: -- KB free", &s_style_label);
    s_sys_psram  = make_label(panel, "PSRAM: -- KB free",&s_style_label);
    s_sys_rssi   = make_label(panel, "WiFi RSSI: --",   &s_style_label);
    s_sys_uptime = make_label(panel, "Uptime: --",      &s_style_label);
    s_sys_fps    = make_label(panel, "FPS: --",          &s_style_label);
}

/* ---- Public API ---- */

void display_ui_create(lv_obj_t *parent)
{
    init_styles();

    s_tileview = lv_tileview_create(parent);
    lv_obj_add_style(s_tileview, &s_style_bg, 0);
    lv_obj_set_style_bg_color(s_tileview, COLOR_BG, 0);

    lv_obj_t *t0 = make_tile(s_tileview, 0, 0);
    lv_obj_t *t1 = make_tile(s_tileview, 1, 0);
    lv_obj_t *t2 = make_tile(s_tileview, 2, 0);
    lv_obj_t *t3 = make_tile(s_tileview, 3, 0);

    create_dashboard(t0);
    create_vitals(t1);
    create_presence(t2);
    create_system(t3);

    ESP_LOGI(TAG, "UI created: 4 views (Dashboard|Vitals|Presence|System)");
}

/* ---- FPS tracking ---- */
static uint32_t s_frame_count = 0;
static uint32_t s_last_fps_time = 0;
static uint32_t s_current_fps = 0;

void display_ui_update(void)
{
    /* FPS counter */
    s_frame_count++;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (now_ms - s_last_fps_time >= 1000) {
        s_current_fps = s_frame_count;
        s_frame_count = 0;
        s_last_fps_time = now_ms;
    }

    /* Read edge data (thread-safe) */
    edge_vitals_pkt_t vitals;
    bool has_vitals = edge_get_vitals(&vitals);

    edge_person_vitals_t persons[EDGE_MAX_PERSONS];
    uint8_t n_active = 0;
    edge_get_multi_person(persons, &n_active);

    /* ---- Dashboard update ---- */
    if (s_dash_chart && has_vitals) {
        /* Push motion energy as amplitude proxy (scaled 0-100) */
        int val = (int)(vitals.motion_energy * 10.0f);
        if (val > 100) val = 100;
        if (val < 0) val = 0;
        lv_chart_set_next_value(s_dash_chart, s_csi_series, val);
    }

    if (s_dash_persons) {
        char buf[16];
        snprintf(buf, sizeof(buf), "Persons: %u", has_vitals ? vitals.n_persons : 0);
        lv_label_set_text(s_dash_persons, buf);
    }

    if (s_dash_rssi && has_vitals) {
        char buf[16];
        snprintf(buf, sizeof(buf), "RSSI: %d", vitals.rssi);
        lv_label_set_text(s_dash_rssi, buf);
    }

    if (s_dash_motion && has_vitals) {
        char buf[24];
        snprintf(buf, sizeof(buf), "Motion: %.1f", (double)vitals.motion_energy);
        lv_label_set_text(s_dash_motion, buf);
    }

    /* ---- Vitals update ---- */
    if (s_vital_chart && has_vitals) {
        int br = (int)(vitals.breathing_rate / 100);  /* Fixed-point to int BPM */
        int hr = (int)(vitals.heartrate / 10000);
        if (br > 120) br = 120;
        if (hr > 120) hr = 120;
        lv_chart_set_next_value(s_vital_chart, s_breath_series, br);
        lv_chart_set_next_value(s_vital_chart, s_hr_series, hr);

        char buf[32];
        snprintf(buf, sizeof(buf), "Breathing: %d BPM", br);
        lv_label_set_text(s_vital_bpm_br, buf);

        snprintf(buf, sizeof(buf), "Heart Rate: %d BPM", hr);
        lv_label_set_text(s_vital_bpm_hr, buf);
    }

    /* ---- Presence grid update ---- */
    if (has_vitals) {
        /* Simple visualization: color cells based on motion energy distribution */
        float energy = vitals.motion_energy;
        uint8_t active_cells = (uint8_t)(energy * 2);  /* Scale for visibility */
        if (active_cells > GRID_COLS * GRID_ROWS) active_cells = GRID_COLS * GRID_ROWS;

        for (int i = 0; i < GRID_COLS * GRID_ROWS; i++) {
            if (i < active_cells) {
                /* Color gradient: green → amber → red based on intensity */
                if (energy > 5.0f) {
                    lv_obj_set_style_bg_color(s_grid_cells[i], COLOR_RED, 0);
                } else if (energy > 2.0f) {
                    lv_obj_set_style_bg_color(s_grid_cells[i], COLOR_AMBER, 0);
                } else {
                    lv_obj_set_style_bg_color(s_grid_cells[i], COLOR_GREEN, 0);
                }
            } else {
                lv_obj_set_style_bg_color(s_grid_cells[i], COLOR_DIM, 0);
            }
        }

        char buf[20];
        snprintf(buf, sizeof(buf), "Persons: %u", vitals.n_persons);
        lv_label_set_text(s_presence_label, buf);
    }

    /* ---- System info update ---- */
    {
        char buf[48];

        snprintf(buf, sizeof(buf), "Node: %d", g_nvs_config.node_id);
        lv_label_set_text(s_sys_node, buf);

        snprintf(buf, sizeof(buf), "Heap: %lu KB free",
                 (unsigned long)(esp_get_free_heap_size() / 1024));
        lv_label_set_text(s_sys_heap, buf);

#if CONFIG_SPIRAM
        snprintf(buf, sizeof(buf), "PSRAM: %lu KB free",
                 (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
#else
        snprintf(buf, sizeof(buf), "PSRAM: N/A");
#endif
        lv_label_set_text(s_sys_psram, buf);

        if (has_vitals) {
            snprintf(buf, sizeof(buf), "WiFi RSSI: %d dBm", vitals.rssi);
            lv_label_set_text(s_sys_rssi, buf);
        }

        uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
        uint32_t h = uptime_s / 3600;
        uint32_t m = (uptime_s % 3600) / 60;
        uint32_t s = uptime_s % 60;
        snprintf(buf, sizeof(buf), "Uptime: %luh %02lum %02lus",
                 (unsigned long)h, (unsigned long)m, (unsigned long)s);
        lv_label_set_text(s_sys_uptime, buf);

        snprintf(buf, sizeof(buf), "FPS: %lu", (unsigned long)s_current_fps);
        lv_label_set_text(s_sys_fps, buf);
    }
}

#endif /* CONFIG_DISPLAY_STICKS3 */

#endif /* CONFIG_DISPLAY_ENABLE */
