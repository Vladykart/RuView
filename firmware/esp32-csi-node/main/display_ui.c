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
#include <math.h>
#include "driver/gpio.h"
#include "audio_beep.h"

/* ---- Buttons: StickS3 KEY1 = G11 (front), KEY2 = G12 (side), read directly
 * by the ESP32-S3 (M5 StickS3 pinmap), active-LOW with the board pull-ups.
 * Polled in the display task (no ISR), debounced.
 *   KEY1 short: next page          KEY1 hold: volume LOW>MID>HIGH>OFF
 *   KEY2 short: radar zoom 6/3 m   KEY2 hold: C1001 mode FALL <-> VITALS
 *   any key while a fall alarm sounds: acknowledge it
 *   both keys held 2 s: force sensor power on (battery-policy override) ---- */
#define UI_KEY1_GPIO       GPIO_NUM_11
#define UI_KEY2_GPIO       GPIO_NUM_12
#define UI_BTN_DEBOUNCE_MS 40
#define UI_BTN_LONG_MS     900
#define UI_BTN_BOTH_MS     2000

typedef enum { UI_PAGE_STATUS = 0, UI_PAGE_RADAR } ui_page_t;
static ui_page_t s_ui_page = UI_PAGE_RADAR;   /* radar is the node's job */
static lv_obj_t *s_page_status = NULL;
static lv_obj_t *s_page_radar  = NULL;

/* ---- Radar page geometry (135x240 portrait, sector opens upward) ---- */
#define RAD_CX        (SCR_W / 2)     /* sensor position, x  */
#define RAD_CY        118             /* sensor position, y  */
#define RAD_R_PX      58              /* full-scale radius   */
#define RAD_SWEEP_S   2.0f            /* one wiper pass, seconds */
#define RAD_TRAIL_N   6               /* track-point history dots */

static void ui_keys_init(void);

static float s_rad_max_m = 6.0f;      /* zoom: 6 m or 3 m full scale */
static lv_obj_t *s_rad_ring_lbl[3];   /* range labels at 1/3, 2/3, 3/3 */
static lv_obj_t *s_rad_trail[RAD_TRAIL_N];
static lv_obj_t *s_rad_banner = NULL; /* transient mode/zoom message  */
static uint32_t  s_banner_until_ms = 0;

static lv_obj_t *s_rad_sweep   = NULL; /* rotating beam (lv_line)        */
static lv_obj_t *s_rad_blip    = NULL; /* target dot on the boresight    */
static lv_obj_t *s_rad_arc     = NULL; /* C1001 range arc (lv_arc)       */
static lv_obj_t *s_rad_dist    = NULL; /* big distance readout           */
static lv_obj_t *s_rad_state   = NULL; /* PRESENT / CLEAR / FALL         */
static lv_obj_t *s_rad_vitals  = NULL; /* "BR 14  HR 62" strip           */
static lv_obj_t *s_rad_sound   = NULL; /* sonar status line              */
static lv_obj_t *s_rad_title   = NULL;
static lv_point_t s_sweep_pts[2];
static float s_sweep_deg = 180.0f;     /* 180..360, 270 = boresight (up) */
static float s_sweep_dir = 1.0f;

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

static lv_obj_t *mk_page(lv_obj_t *parent)
{
    lv_obj_t *pg = lv_obj_create(parent);
    lv_obj_set_pos(pg, 0, 0);
    lv_obj_set_size(pg, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(pg, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(pg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pg, 0, 0);
    lv_obj_set_style_radius(pg, 0, 0);
    lv_obj_set_style_pad_all(pg, 0, 0);
    lv_obj_clear_flag(pg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(pg, LV_SCROLLBAR_MODE_OFF);
    return pg;
}

/* ---- Radar page: 180° sector, sensor at the bottom center ---- */
static void create_radar_page(lv_obj_t *pg)
{
    s_rad_title = mk_text(pg, PAD + 2, 7, SCR_W - 2 * PAD - 4,
                          &lv_font_montserrat_14, COLOR_CYAN,
                          LV_TEXT_ALIGN_LEFT, "RADAR");
    s_rad_state = mk_text(pg, PAD + 2, 7, SCR_W - 2 * PAD - 4,
                          &lv_font_montserrat_14, COLOR_TEXT_DIM,
                          LV_TEXT_ALIGN_RIGHT, "...");

    /* Static range arcs at 1/3, 2/3, 3/3 of full scale (semicircle: LVGL
     * angles 180..360); their labels follow the zoom (ui_radar_set_zoom). */
    for (int i = 0; i < 3; i++) {
        int r = RAD_R_PX * (i + 1) / 3;
        lv_obj_t *arc = lv_arc_create(pg);
        lv_obj_set_size(arc, r * 2, r * 2);
        lv_obj_set_pos(arc, RAD_CX - r, RAD_CY - r);
        lv_arc_set_bg_angles(arc, 180, 360);
        lv_arc_set_value(arc, 0);
        lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
        lv_obj_remove_style(arc, NULL, LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(arc, 1, LV_PART_MAIN);
        lv_obj_set_style_arc_color(arc, CARD_BORDER, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        s_rad_ring_lbl[i] = mk_text(pg, RAD_CX + 2, RAD_CY - r - 12, 24,
                                    &lv_font_montserrat_12, COLOR_TEXT_DIM,
                                    LV_TEXT_ALIGN_LEFT, "");
    }
    /* Baseline under the sector */
    lv_obj_t *base = lv_obj_create(pg);
    lv_obj_set_pos(base, RAD_CX - RAD_R_PX, RAD_CY);
    lv_obj_set_size(base, RAD_R_PX * 2 + 1, 1);
    lv_obj_set_style_bg_color(base, CARD_BORDER, 0);
    lv_obj_set_style_bg_opa(base, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(base, 0, 0);

    /* C1001 range arc: the sensor reports distance without bearing, so the
     * honest mark is an arc segment around the boresight, not a point. */
    s_rad_arc = lv_arc_create(pg);
    lv_arc_set_bg_angles(s_rad_arc, 245, 295);
    lv_arc_set_value(s_rad_arc, 0);
    lv_obj_remove_style(s_rad_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_style(s_rad_arc, NULL, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_rad_arc, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_rad_arc, COLOR_GREEN, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_rad_arc, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_rad_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_rad_arc, LV_OBJ_FLAG_HIDDEN);

    /* Track-point trail: older positions as fading dots (fall mode). */
    for (int i = 0; i < RAD_TRAIL_N; i++) {
        lv_obj_t *d = lv_obj_create(pg);
        lv_obj_set_size(d, 4, 4);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, COLOR_GREEN, 0);
        lv_obj_set_style_bg_opa(d, (lv_opa_t)(LV_OPA_70 - i * 10), 0);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
        s_rad_trail[i] = d;
    }

    /* Target blip: real x/y in fall mode, boresight at range in vitals mode */
    s_rad_blip = lv_obj_create(pg);
    lv_obj_set_size(s_rad_blip, 8, 8);
    lv_obj_set_style_radius(s_rad_blip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_rad_blip, COLOR_GREEN, 0);
    lv_obj_set_style_bg_opa(s_rad_blip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_rad_blip, 0, 0);
    lv_obj_add_flag(s_rad_blip, LV_OBJ_FLAG_HIDDEN);

    /* Sweep beam */
    s_sweep_pts[0].x = RAD_CX; s_sweep_pts[0].y = RAD_CY;
    s_sweep_pts[1].x = RAD_CX - RAD_R_PX; s_sweep_pts[1].y = RAD_CY;
    s_rad_sweep = lv_line_create(pg);
    lv_line_set_points(s_rad_sweep, s_sweep_pts, 2);
    lv_obj_set_style_line_width(s_rad_sweep, 2, 0);
    lv_obj_set_style_line_color(s_rad_sweep, COLOR_CYAN, 0);
    lv_obj_set_style_line_opa(s_rad_sweep, LV_OPA_70, 0);

    /* Transient banner (mode / zoom / mute changes) above the sector */
    s_rad_banner = mk_text(pg, 0, 28, SCR_W, &lv_font_montserrat_14,
                           COLOR_AMBER, LV_TEXT_ALIGN_CENTER, "");
    lv_obj_add_flag(s_rad_banner, LV_OBJ_FLAG_HIDDEN);

    /* Data block under the sector: distance, position/vitals, sonar */
    s_rad_dist = mk_text(pg, 0, RAD_CY + 6, SCR_W, &lv_font_montserrat_28,
                         COLOR_TEXT, LV_TEXT_ALIGN_CENTER, "--.-");
    s_rad_vitals = mk_text(pg, 0, RAD_CY + 40, SCR_W, &lv_font_montserrat_12,
                           COLOR_TEXT_DIM, LV_TEXT_ALIGN_CENTER, "");
    s_rad_sound = mk_text(pg, 0, RAD_CY + 56, SCR_W, &lv_font_montserrat_12,
                          COLOR_TEXT_DIM, LV_TEXT_ALIGN_CENTER, LV_SYMBOL_VOLUME_MAX " --");

    /* Controls legend, two lines (135 px is too narrow for one) */
    mk_text(pg, 0, SCR_H - 32, SCR_W, &lv_font_montserrat_12, COLOR_TEXT_DIM,
            LV_TEXT_ALIGN_CENTER, "K1 page | hold vol");
    mk_text(pg, 0, SCR_H - 17, SCR_W, &lv_font_montserrat_12, COLOR_TEXT_DIM,
            LV_TEXT_ALIGN_CENTER, "K2 zoom | hold mode");
}

static void ui_radar_set_zoom(float max_m)
{
    s_rad_max_m = max_m;
    char b[8];
    for (int i = 0; i < 3; i++) {
        float m = max_m * (i + 1) / 3.0f;
        snprintf(b, sizeof(b), m == (int)m ? "%.0fm" : "%.1fm", (double)m);
        lv_label_set_text(s_rad_ring_lbl[i], b);
    }
}

static void ui_banner(const char *msg, uint32_t now_ms)
{
    lv_label_set_text(s_rad_banner, msg);
    lv_obj_clear_flag(s_rad_banner, LV_OBJ_FLAG_HIDDEN);
    s_banner_until_ms = now_ms + 1800;
}

void display_ui_create(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);

    /* Two pages toggled by KEY1: radar sector (default) | status card. */
    s_page_status = mk_page(parent);
    s_page_radar  = mk_page(parent);
    create_radar_page(s_page_radar);
    ui_radar_set_zoom(6.0f);
    lv_obj_add_flag(s_page_status, LV_OBJ_FLAG_HIDDEN);

#if CONFIG_DISPLAY_STICKS3
    gpio_config_t btn = {
        .pin_bit_mask = (1ULL << UI_KEY1_GPIO) | (1ULL << UI_KEY2_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);
    /* Idle level check: both should read 1 (released, active-LOW). */
    ESP_LOGI(TAG, "keys idle: KEY1(G11)=%d KEY2(G12)=%d",
             gpio_get_level(UI_KEY1_GPIO), gpio_get_level(UI_KEY2_GPIO));
    ui_keys_init();
#endif

    parent = s_page_status;  /* the status widgets below live on page 0 */

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

/* ---- Sensor power sequencing ----
 * The C1001 must get its 5 V (EXT_5V boost) BEFORE anything drives its UART:
 * the mmWave probe toggling an unpowered sensor's RX back-powers it through
 * its I/O pins, and switching its supply on afterwards collapsed the 3.3 V
 * rail below the 2.44 V brownout level even with a 3.9 V cell. So:
 *   - display_ui_sensor_power_boot() runs from app_main before
 *     mmwave_sensor_init(): if the battery can carry it, power the sensor
 *     there and let it settle, with nothing else talking to it;
 *   - if the battery only becomes good enough later (or the operator forces
 *     it with both keys), reboot once so the power-up happens first again.
 * RTC memory keeps a brownout streak across those resets: the bar rises
 * after EXT5V_MAX_STREAK brownouts and powering stops entirely after
 * EXT5V_GIVEUP_STREAK, so nothing can reset-loop forever. */
#define EXT5V_ON_MV          3700
#define EXT5V_ON_MV_STREAK   4000
#define EXT5V_REBOOT_MARGIN  50     /* mid-run reboot needs need+margin */
#define EXT5V_MAX_STREAK     3
#define EXT5V_GIVEUP_STREAK  6
#define EXT5V_SETTLE_MS      1500
#define EXT5V_RTC_MAGIC      0x5E750A60u

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static RTC_NOINIT_ATTR uint32_t s_rtc_magic;
static RTC_NOINIT_ATTR uint32_t s_rtc_bod_streak;
static RTC_NOINIT_ATTR uint32_t s_rtc_force_next;

#if CONFIG_DISPLAY_STICKS3
static bool s_ext5v_on = false;
#else
static bool s_ext5v_on = true;    /* sensor power is not switched here */
#endif
static bool s_ext5v_force = false;   /* both-keys operator override */
static int  s_vbat_mv = -1;
static int  s_ext5v_need_mv = EXT5V_ON_MV;

static int ext5v_need_mv(void)
{
    if (s_rtc_bod_streak >= EXT5V_GIVEUP_STREAK) return 99999;
    return s_rtc_bod_streak >= EXT5V_MAX_STREAK ? EXT5V_ON_MV_STREAK : EXT5V_ON_MV;
}

void display_ui_sensor_power_boot(void)
{
#if CONFIG_DISPLAY_STICKS3
    esp_reset_reason_t why = esp_reset_reason();
    if (s_rtc_magic != EXT5V_RTC_MAGIC || why == ESP_RST_POWERON) {
        s_rtc_magic = EXT5V_RTC_MAGIC;
        s_rtc_bod_streak = 0;
        s_rtc_force_next = 0;
    }
    if (why == ESP_RST_BROWNOUT) s_rtc_bod_streak++;
    bool force = s_rtc_force_next != 0;
    s_rtc_force_next = 0;

    /* Hold the sensor's UART pins high-impedance until it has power. */
    gpio_reset_pin(GPIO_NUM_9);
    gpio_reset_pin(GPIO_NUM_10);
    gpio_set_direction(GPIO_NUM_9, GPIO_MODE_INPUT);
    gpio_set_direction(GPIO_NUM_10, GPIO_MODE_INPUT);

    uint16_t mv = 0;
    if (display_hal_battery(&mv, NULL) == ESP_OK) s_vbat_mv = mv;
    s_ext5v_need_mv = ext5v_need_mv();
    bool ok = force || s_vbat_mv >= s_ext5v_need_mv
              || (s_vbat_mv < 0 && s_rtc_bod_streak < EXT5V_MAX_STREAK);
    ESP_LOGI(TAG, "sensor power at boot: reset=%d vbat=%d mV need=%d mV "
             "streak=%lu%s -> %s", (int)why, s_vbat_mv, s_ext5v_need_mv,
             (unsigned long)s_rtc_bod_streak, force ? " forced" : "",
             ok ? "ON" : "deferred");
    if (ok && display_hal_ext5v(true) == ESP_OK) {
        s_ext5v_on = true;
        vTaskDelay(pdMS_TO_TICKS(EXT5V_SETTLE_MS));  /* sensor inrush, alone */
    }
#endif
}

static void ui_power_policy(uint32_t now_ms)
{
#if CONFIG_DISPLAY_STICKS3
    static uint32_t s_last_ms = 0, s_last_log_ms = 0;
    if (s_last_ms && now_ms - s_last_ms < 3000) return;
    s_last_ms = now_ms;

    uint16_t mv = 0;
    if (display_hal_battery(&mv, NULL) == ESP_OK) s_vbat_mv = mv;
    if (s_ext5v_on) {
        /* Ran 30 s with the sensor powered: the brownout streak is over. */
        if (s_rtc_bod_streak && now_ms > 30000) s_rtc_bod_streak = 0;
        return;
    }

    s_ext5v_need_mv = ext5v_need_mv();
    bool ready = s_vbat_mv >= s_ext5v_need_mv + EXT5V_REBOOT_MARGIN;
    if (s_ext5v_force || ready) {
        /* The probe has been driving the unpowered sensor's UART: power it
         * only from a clean boot. */
        ESP_LOGW(TAG, "sensor power ready (vbat=%d mV%s): rebooting to power "
                 "it first", s_vbat_mv, s_ext5v_force ? ", forced" : "");
        s_rtc_force_next = s_ext5v_force ? 1 : 0;
        if (s_ext5v_force) s_rtc_bod_streak = 0;
        lv_label_set_text(s_rad_banner, "SENSOR ON: REBOOT");
        lv_obj_clear_flag(s_rad_banner, LV_OBJ_FLAG_HIDDEN);
        lv_refr_now(NULL);
        vTaskDelay(pdMS_TO_TICKS(600));
        esp_restart();
    }
    if (now_ms - s_last_log_ms >= 15000) {
        s_last_log_ms = now_ms;
        ESP_LOGI(TAG, "sensor power deferred: vbat=%d mV need=%d mV (streak %lu)",
                 s_vbat_mv, s_ext5v_need_mv + EXT5V_REBOOT_MARGIN,
                 (unsigned long)s_rtc_bod_streak);
    }
#else
    (void)now_ms;
#endif
}

/* Key handling (bindings: see UI_KEY1_GPIO). Runs in the display task, so
 * LVGL calls are safe here. */
static void ui_toggle_page(void)
{
    s_ui_page = (s_ui_page == UI_PAGE_STATUS) ? UI_PAGE_RADAR : UI_PAGE_STATUS;
    if (s_ui_page == UI_PAGE_RADAR) {
        lv_obj_clear_flag(s_page_radar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_page_status, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_page_status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_page_radar, LV_OBJ_FLAG_HIDDEN);
    }
    ESP_LOGI(TAG, "UI page -> %s", s_ui_page == UI_PAGE_RADAR ? "radar" : "status");
}

/* Falling edges latched by ISR: the display task polls at ~10 Hz, so a quick
 * tap could otherwise land between two polls. */
static volatile bool s_key_fell[2];

static void IRAM_ATTR key_isr(void *arg)
{
    s_key_fell[(int)(intptr_t)arg] = true;
}

static void ui_keys_init(void)
{
    gpio_set_intr_type(UI_KEY1_GPIO, GPIO_INTR_NEGEDGE);
    gpio_set_intr_type(UI_KEY2_GPIO, GPIO_INTR_NEGEDGE);
    esp_err_t e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "key ISR service: %s (polling only)", esp_err_to_name(e));
        return;
    }
    gpio_isr_handler_add(UI_KEY1_GPIO, key_isr, (void *)(intptr_t)0);
    gpio_isr_handler_add(UI_KEY2_GPIO, key_isr, (void *)(intptr_t)1);
}

static void ui_key_short(int k, uint32_t now_ms)
{
    if (k == 0) {
        ui_toggle_page();
    } else {
        ui_radar_set_zoom(s_rad_max_m > 4.0f ? 3.0f : 6.0f);
        ui_banner(s_rad_max_m > 4.0f ? "ZOOM 6 m" : "ZOOM 3 m", now_ms);
    }
}

static void ui_key_long(int k, uint32_t now_ms)
{
    if (k == 0) {
        static const char *const names[] = { "VOLUME OFF", "VOLUME LOW",
                                             "VOLUME MID", "VOLUME HIGH" };
        ui_banner(names[audio_beep_cycle_volume()], now_ms);
    } else {
        uint8_t next = mmwave_c1001_get_mode() == MMWAVE_C1001_MODE_FALL
                       ? MMWAVE_C1001_MODE_VITALS : MMWAVE_C1001_MODE_FALL;
        mmwave_c1001_set_mode(next);
        ui_banner(next == MMWAVE_C1001_MODE_FALL ? "MODE: FALL" : "MODE: VITALS", now_ms);
    }
}

static void ui_poll_button(uint32_t now_ms)
{
    static const gpio_num_t pins[2] = { UI_KEY1_GPIO, UI_KEY2_GPIO };
    static struct { bool down, consumed; uint32_t t; } st[2];
    static bool combo = false, combo_fired = false;
    static uint32_t combo_t = 0;

    for (int k = 0; k < 2; k++) {
        int lvl = gpio_get_level(pins[k]);      /* 0 = pressed */
        bool fell = s_key_fell[k];
        s_key_fell[k] = false;
        if (!st[k].down && (lvl == 0 || fell)) {
            st[k].down = true;
            st[k].t = now_ms;
            /* A press while the fall alarm sounds only acknowledges it. */
            st[k].consumed = audio_beep_ack_alarm();
        }
    }

    /* Both keys: 2 s hold forces sensor power on; never a single action. */
    if (st[0].down && st[1].down) {
        if (!combo) { combo = true; combo_fired = false; combo_t = now_ms; }
        st[0].consumed = st[1].consumed = true;
        if (!combo_fired && now_ms - combo_t >= UI_BTN_BOTH_MS) {
            combo_fired = true;
            s_ext5v_force = true;
            ui_banner("SENSOR FORCED ON", now_ms);
            ESP_LOGW(TAG, "sensor power forced on by operator");
        }
    }

    for (int k = 0; k < 2; k++) {
        if (!st[k].down) continue;
        if (gpio_get_level(pins[k]) == 1) {     /* released */
            if (!st[k].consumed) ui_key_short(k, now_ms);
            st[k].down = false;
        } else if (!st[k].consumed && now_ms - st[k].t >= UI_BTN_LONG_MS) {
            st[k].consumed = true;
            ui_key_long(k, now_ms);
        }
    }
    if (!st[0].down && !st[1].down) combo = false;
}

/* Sweep beam: full-frame-rate wiper across the sector. */
static void ui_radar_sweep(uint32_t now_ms)
{
    static uint32_t s_prev_ms = 0;
    float dt = s_prev_ms ? (now_ms - s_prev_ms) / 1000.0f : 0;
    s_prev_ms = now_ms;
    s_sweep_deg += s_sweep_dir * dt * (180.0f / RAD_SWEEP_S);
    if (s_sweep_deg >= 358.0f) { s_sweep_deg = 358.0f; s_sweep_dir = -1.0f; }
    if (s_sweep_deg <= 182.0f) { s_sweep_deg = 182.0f; s_sweep_dir = 1.0f; }
    float a = s_sweep_deg * (float)M_PI / 180.0f;
    s_sweep_pts[1].x = RAD_CX + (lv_coord_t)(cosf(a) * RAD_R_PX);
    s_sweep_pts[1].y = RAD_CY + (lv_coord_t)(sinf(a) * RAD_R_PX);
    lv_line_set_points(s_rad_sweep, s_sweep_pts, 2);
}

/* Radar page data: C1001 presence/range/vitals at 2 Hz. */
static void ui_radar_place(lv_obj_t *o, int sz, float x_m, float y_m)
{
    float k = RAD_R_PX / s_rad_max_m;
    if (y_m < 0) y_m = 0;
    float r = sqrtf(x_m * x_m + y_m * y_m);
    if (r > s_rad_max_m) { x_m *= s_rad_max_m / r; y_m *= s_rad_max_m / r; }
    lv_obj_set_pos(o, RAD_CX + (lv_coord_t)(x_m * k) - sz / 2,
                      RAD_CY - (lv_coord_t)(y_m * k) - sz / 2);
}

static void ui_radar_data(void)
{
    char buf[40];
    mmwave_state_t mw;
    bool has_mw = mmwave_sensor_get_state(&mw);
    bool fall_mode = mmwave_c1001_get_mode() == MMWAVE_C1001_MODE_FALL;
    audio_beep_status_t snd;
    audio_beep_get_status(&snd);

    snprintf(buf, sizeof(buf), "n%d %s%s", g_nvs_config.node_id,
             fall_mode ? "FALL" : "VITALS",
             snd.muted ? " " LV_SYMBOL_MUTE : "");
    lv_label_set_text(s_rad_title, buf);

    /* ---- state (top right) ---- */
    if (has_mw && mw.fall_detected) {
        lv_label_set_text(s_rad_state, snd.alarming ? "FALL!" : "FALL ack");
        lv_obj_set_style_text_color(s_rad_state, COLOR_RED, 0);
    } else if (has_mw && mw.person_present) {
        lv_label_set_text(s_rad_state, "PRESENT");
        lv_obj_set_style_text_color(s_rad_state, COLOR_GREEN, 0);
    } else if (!s_ext5v_on) {
        lv_label_set_text(s_rad_state, "LOW BATT");
        lv_obj_set_style_text_color(s_rad_state, COLOR_AMBER, 0);
    } else {
        lv_label_set_text(s_rad_state, has_mw ? "CLEAR" : "search");
        lv_obj_set_style_text_color(s_rad_state, COLOR_TEXT_DIM, 0);
    }

    /* ---- track history (new point -> shift the trail) ---- */
    static float hx[RAD_TRAIL_N + 1], hy[RAD_TRAIL_N + 1];
    static int hn = 0;
    static int64_t last_track_us = 0;
    int64_t now_us = esp_timer_get_time();
    bool track = fall_mode && has_mw && mw.person_present && mw.track_valid
                 && now_us - mw.track_us < 2000000;
    if (track && mw.track_us != last_track_us) {
        last_track_us = mw.track_us;
        for (int i = RAD_TRAIL_N; i > 0; i--) { hx[i] = hx[i - 1]; hy[i] = hy[i - 1]; }
        hx[0] = mw.track_x_cm / 100.0f;
        hy[0] = mw.track_y_cm / 100.0f;
        if (hn < RAD_TRAIL_N + 1) hn++;
    }
    if (!track) hn = 0;

    lv_color_t c = (has_mw && mw.fall_detected) ? COLOR_RED : COLOR_GREEN;
    lv_obj_set_style_bg_color(s_rad_blip, c, 0);
    float dist_m = 0;

    if (track) {
        /* Real position: blip at x/y, trail of older points, no range arc. */
        lv_obj_add_flag(s_rad_arc, LV_OBJ_FLAG_HIDDEN);
        ui_radar_place(s_rad_blip, 8, hx[0], hy[0]);
        lv_obj_clear_flag(s_rad_blip, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < RAD_TRAIL_N; i++) {
            if (i + 1 < hn) {
                ui_radar_place(s_rad_trail[i], 4, hx[i + 1], hy[i + 1]);
                lv_obj_set_style_bg_color(s_rad_trail[i], c, 0);
                lv_obj_clear_flag(s_rad_trail[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_rad_trail[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        dist_m = sqrtf(hx[0] * hx[0] + hy[0] * hy[0]);
        snprintf(buf, sizeof(buf), "x %+.2f  y %.2f m", (double)hx[0], (double)hy[0]);
        lv_label_set_text(s_rad_vitals, buf);
        lv_obj_set_style_text_color(s_rad_vitals, COLOR_GREEN, 0);
    } else {
        for (int i = 0; i < RAD_TRAIL_N; i++) lv_obj_add_flag(s_rad_trail[i], LV_OBJ_FLAG_HIDDEN);
        bool range = has_mw && mw.person_present && mw.distance_cm > 0;
        if (range) {
            /* Range without bearing: arc around the boresight + blip on it. */
            dist_m = mw.distance_cm / 100.0f;
            float d = dist_m > s_rad_max_m ? s_rad_max_m : dist_m;
            int r = (int)(RAD_R_PX * (d / s_rad_max_m));
            if (r < 4) r = 4;
            lv_obj_set_size(s_rad_arc, r * 2, r * 2);
            lv_obj_set_pos(s_rad_arc, RAD_CX - r, RAD_CY - r);
            lv_obj_set_style_arc_color(s_rad_arc, c, LV_PART_MAIN);
            lv_obj_clear_flag(s_rad_arc, LV_OBJ_FLAG_HIDDEN);
            ui_radar_place(s_rad_blip, 8, 0, d);
            lv_obj_clear_flag(s_rad_blip, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_rad_arc, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_rad_blip, LV_OBJ_FLAG_HIDDEN);
        }

        if (!s_ext5v_on) {
            if (s_ext5v_need_mv > 9000)   /* gave up after brownouts */
                snprintf(buf, sizeof(buf), "sensor off: K1+K2 2s");
            else
                snprintf(buf, sizeof(buf), "batt %.2fV need %.2fV",
                         s_vbat_mv / 1000.0, s_ext5v_need_mv / 1000.0);
            lv_label_set_text(s_rad_vitals, buf);
            lv_obj_set_style_text_color(s_rad_vitals, COLOR_AMBER, 0);
        } else if (!fall_mode && has_mw && mw.person_present &&
                   (mw.breathing_rate > 0 || mw.heart_rate_bpm > 0)) {
            snprintf(buf, sizeof(buf), "BR %.0f  HR %.0f",
                     (double)mw.breathing_rate, (double)mw.heart_rate_bpm);
            lv_label_set_text(s_rad_vitals, buf);
            lv_obj_set_style_text_color(s_rad_vitals, COLOR_CYAN, 0);
        } else {
            lv_label_set_text(s_rad_vitals, fall_mode ? "no track point" : "BR --  HR --");
            lv_obj_set_style_text_color(s_rad_vitals, COLOR_TEXT_DIM, 0);
        }
    }

    if (dist_m > 0) {
        snprintf(buf, sizeof(buf), "%.2fm", (double)dist_m);
        lv_label_set_text(s_rad_dist, buf);
        lv_obj_set_style_text_color(s_rad_dist, COLOR_TEXT, 0);
    } else {
        lv_label_set_text(s_rad_dist, "--.-");
        lv_obj_set_style_text_color(s_rad_dist, COLOR_TEXT_DIM, 0);
    }

    /* ---- sonar line ---- */
    static const char *const vol_names[] = { "OFF", "LOW", "MID", "HIGH" };
    if (snd.alarming) {
        lv_label_set_text(s_rad_sound, LV_SYMBOL_WARNING " FALL ALARM: press key");
        lv_obj_set_style_text_color(s_rad_sound, COLOR_RED, 0);
    } else if (!snd.ready) {
        lv_label_set_text(s_rad_sound, LV_SYMBOL_MUTE " no audio");
        lv_obj_set_style_text_color(s_rad_sound, COLOR_TEXT_DIM, 0);
    } else if (snd.muted) {
        lv_label_set_text(s_rad_sound, LV_SYMBOL_MUTE " sonar off (hold K1)");
        lv_obj_set_style_text_color(s_rad_sound, COLOR_AMBER, 0);
    } else if (snd.chirping) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_VOLUME_MAX " %s %.2fs %.1fk",
                 vol_names[snd.volume], (double)snd.interval_s,
                 (double)(snd.tone_hz / 1000.0f));
        lv_label_set_text(s_rad_sound, buf);
        lv_obj_set_style_text_color(s_rad_sound, COLOR_CYAN, 0);
    } else {
        snprintf(buf, sizeof(buf), LV_SYMBOL_VOLUME_MID " sonar %s armed",
                 vol_names[snd.volume]);
        lv_label_set_text(s_rad_sound, buf);
        lv_obj_set_style_text_color(s_rad_sound, COLOR_TEXT_DIM, 0);
    }
}

void display_ui_update(void)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    ui_power_policy(now_ms);
#if CONFIG_DISPLAY_STICKS3
    ui_poll_button(now_ms);
#endif
    if (s_banner_until_ms && now_ms >= s_banner_until_ms) {
        lv_obj_add_flag(s_rad_banner, LV_OBJ_FLAG_HIDDEN);
        s_banner_until_ms = 0;
    }
    if (s_ui_page == UI_PAGE_RADAR) ui_radar_sweep(now_ms);

    /* Data refresh: radar 5 Hz (the track point moves ~3 Hz), status card
     * 2 Hz — a handful of labels, cheap on CPU0. */
    static uint32_t s_last_ms = 0;
    uint32_t period = (s_ui_page == UI_PAGE_RADAR) ? 200 : 500;
    if (now_ms - s_last_ms < period) return;
    s_last_ms = now_ms;

    if (s_ui_page == UI_PAGE_RADAR) { ui_radar_data(); return; }

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
        lv_label_set_text(s_lbl_substate, !s_ext5v_on ? "sensor off: low batt"
                          : has_mw ? mmwave_type_name(mw.type) : "watching");
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
