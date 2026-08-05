/**
 * @file display_hal.c
 * @brief ADR-045: Display HAL — supports multiple panels via compile-time detection.
 *
 * ESP32-S3: SH8601 QSPI AMOLED 368x448 (Waveshare ESP32-S3-Touch-AMOLED-1.8)
 * ESP32-C6: ST7789 SPI TFT 172x320  (Waveshare ESP32-C6-LCD-1.47)
 */

#include "display_hal.h"
#include "sdkconfig.h"

#if CONFIG_DISPLAY_ENABLE

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"

static const char *TAG = "disp_hal";

/* ===========================================================================
 * ESP32-C6: 172x320 SPI TFT — Waveshare C6-LCD-1.47 variants
 *
 * Non-touch board (ESP32-C6-LCD-1.47):       4MB flash, ST7789
 * Touch board (ESP32-C6-Touch-LCD-1.47):     8MB flash, JD9853
 *
 * Both use ST7789-compatible SPI command paths in ESP-IDF, but GPIO mappings
 * differ. Select mapping at runtime from detected flash size.
 * =========================================================================*/
#if CONFIG_IDF_TARGET_ESP32C6

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"  /* provides esp_lcd_new_panel_st7789() */
#include "driver/ledc.h"
#include "esp_flash.h"

/* Non-touch pin assignments (ESP32-C6-LCD-1.47, 4MB) */
#define C6_NT_SCLK     7
#define C6_NT_MOSI     6
#define C6_NT_CS       14
#define C6_NT_DC       15
#define C6_NT_RST      21
#define C6_NT_BLK      22

/* Touch pin assignments (ESP32-C6-Touch-LCD-1.47, 8MB) */
#define C6_T_SCLK      1
#define C6_T_MOSI      2
#define C6_T_CS        14
#define C6_T_DC        15
#define C6_T_RST       22
#define C6_T_BLK       23

#define ST_H_RES       172
#define ST_V_RES       320

static esp_lcd_panel_handle_t s_panel = NULL;
static int s_pin_sclk = C6_NT_SCLK;
static int s_pin_mosi = C6_NT_MOSI;
static int s_pin_cs = C6_NT_CS;
static int s_pin_dc = C6_NT_DC;
static int s_pin_rst = C6_NT_RST;
static int s_pin_blk = C6_NT_BLK;
static bool s_touch_variant = false;

static void detect_c6_variant_and_pins(void)
{
    uint32_t flash_size = 0;
    esp_err_t ret = esp_flash_get_size(NULL, &flash_size);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Flash size detect failed (%s), defaulting to non-touch pins",
                 esp_err_to_name(ret));
        flash_size = 0;
    }

    if (flash_size >= (8 * 1024 * 1024)) {
        s_touch_variant = true;
        s_pin_sclk = C6_T_SCLK;
        s_pin_mosi = C6_T_MOSI;
        s_pin_cs = C6_T_CS;
        s_pin_dc = C6_T_DC;
        s_pin_rst = C6_T_RST;
        s_pin_blk = C6_T_BLK;
        ESP_LOGI(TAG, "Detected touch board variant (8MB flash, JD9853)");
    } else {
        s_touch_variant = false;
        s_pin_sclk = C6_NT_SCLK;
        s_pin_mosi = C6_NT_MOSI;
        s_pin_cs = C6_NT_CS;
        s_pin_dc = C6_NT_DC;
        s_pin_rst = C6_NT_RST;
        s_pin_blk = C6_NT_BLK;
        ESP_LOGI(TAG, "Detected non-touch board variant (<8MB flash, ST7789)");
    }

    ESP_LOGI(TAG, "LCD pins SCLK=%d MOSI=%d CS=%d DC=%d RST=%d BLK=%d",
             s_pin_sclk, s_pin_mosi, s_pin_cs, s_pin_dc, s_pin_rst, s_pin_blk);
}

esp_err_t display_hal_init_panel(void)
{
    detect_c6_variant_and_pins();
    ESP_LOGI(TAG, "Initializing Waveshare C6%s-LCD-1.47 (172x320)...",
             s_touch_variant ? "-Touch" : "");

    /* Backlight — turn on via LEDC PWM */
    ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&bl_timer);

    ledc_channel_config_t bl_ch = {
        .gpio_num = s_pin_blk,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 200,  /* ~80% brightness */
        .hpoint = 0,
    };
    ledc_channel_config(&bl_ch);
    ESP_LOGI(TAG, "Backlight ON (GPIO %d, PWM)", s_pin_blk);

    /* Hardware reset */
    gpio_config_t rst_cfg = {
        .pin_bit_mask = (1ULL << s_pin_rst),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(s_pin_rst, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(s_pin_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* SPI bus */
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = s_pin_sclk,
        .mosi_io_num = s_pin_mosi,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = ST_H_RES * 40 * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    /* Panel IO */
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = s_pin_dc,
        .cs_gpio_num = s_pin_cs,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                             &io_config, &io_handle));

    /* ST7789-compatible panel driver */
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,  /* Already reset manually above */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    /* 172x320: invert colors and set column gap for 240-col controller space */
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 34, 0));

    /* Touch variant has opposite default horizontal scan direction */
    if (s_touch_variant) {
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));
    }

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    ESP_LOGI(TAG, "Panel init OK (%dx%d, %s)", ST_H_RES, ST_V_RES,
             s_touch_variant ? "touch/JD9853" : "non-touch/ST7789");

    return ESP_OK;
}

void display_hal_draw(int x_start, int y_start, int x_end, int y_end,
                      const void *color_data)
{
    if (!s_panel) return;
    esp_lcd_panel_draw_bitmap(s_panel, x_start, y_start, x_end, y_end, color_data);
}

esp_err_t display_hal_init_touch(void)
{
    /* Non-touch version: no touch controller */
    ESP_LOGI(TAG, "No touch controller on ESP32-C6-LCD-1.47");
    return ESP_ERR_NOT_FOUND;
}

bool display_hal_touch_read(uint16_t *x, uint16_t *y)
{
    return false;
}

void display_hal_set_brightness(uint8_t percent)
{
    if (percent > 100) percent = 100;
    uint32_t duty = (uint32_t)percent * 255 / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

#elif CONFIG_DISPLAY_STICKS3

/* ===========================================================================
 * M5Stack StickS3 (K150): ST7789 SPI TFT 135x240
 *
 * Pin profile per M5GFX 0.2.26 (commit 729297d6, src/M5GFX.cpp): SPI3
 * mode 0 @ 40 MHz, gap (52,40), color inversion on, backlight GPIO38
 * active-high plain GPIO (no PWM channel documented for K150).
 * =========================================================================*/

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/i2c.h"

#define S3S_SCLK       40
#define S3S_MOSI       39
#define S3S_CS         41
#define S3S_DC         45
#define S3S_RST        21
#define S3S_BLK        38

/* M5PM1 PMIC — gates the LCD power rail (M5GFX 0.2.26 board_M5StickS3) */
#define S3S_I2C_PORT   I2C_NUM_1
#define S3S_I2C_SDA    47
#define S3S_I2C_SCL    48
#define M5PM1_ADDR     0x6E

#define ST_H_RES       135
#define ST_V_RES       240

static esp_lcd_panel_handle_t s_panel = NULL;

static esp_err_t pm1_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_write_to_device(S3S_I2C_PORT, M5PM1_ADDR, buf, 2,
                                      pdMS_TO_TICKS(100));
}

static esp_err_t pm1_update_bit(uint8_t reg, uint8_t bit_mask, bool set)
{
    uint8_t val = 0;
    esp_err_t err = i2c_master_write_read_device(S3S_I2C_PORT, M5PM1_ADDR,
                                                 &reg, 1, &val, 1,
                                                 pdMS_TO_TICKS(100));
    if (err != ESP_OK) return err;
    val = set ? (val | bit_mask) : (val & ~bit_mask);
    return pm1_write_reg(reg, val);
}

/* Enable the LCD power rail via M5PM1 GPIO2 (register sequence from M5GFX) */
static esp_err_t sticks3_lcd_power_on(void)
{
    i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = S3S_I2C_SDA,
        .scl_io_num = S3S_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    esp_err_t err = i2c_param_config(S3S_I2C_PORT, &i2c_cfg);
    if (err == ESP_OK) err = i2c_driver_install(S3S_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "M5PM1 I2C init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Keep PMIC awake, then configure GPIO2 as push-pull output high */
    pm1_write_reg(0x09, 0x00);            /* disable I2C idle sleep       */
    pm1_update_bit(0x16, 1 << 2, false);
    pm1_update_bit(0x10, 1 << 2, true);   /* GPIO2 output enable          */
    pm1_update_bit(0x13, 1 << 2, false);
    err = pm1_update_bit(0x11, 1 << 2, true);  /* GPIO2 high → LCD power  */
    ESP_LOGI(TAG, "M5PM1 LCD power rail: %s", err == ESP_OK ? "ON" : "FAILED");
    return err;
}

esp_err_t display_hal_init_panel(void)
{
    ESP_LOGI(TAG, "Initializing M5Stack StickS3 ST7789 (%dx%d)...", ST_H_RES, ST_V_RES);

    /* LCD power rail must come up before the panel will respond */
    sticks3_lcd_power_on();
    vTaskDelay(pdMS_TO_TICKS(20));

    /* Backlight on — plain GPIO, active high */
    gpio_config_t blk_cfg = {
        .pin_bit_mask = (1ULL << S3S_BLK),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&blk_cfg);
    gpio_set_level(S3S_BLK, 1);

    /* Hardware reset */
    gpio_config_t rst_cfg = {
        .pin_bit_mask = (1ULL << S3S_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(S3S_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(S3S_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    spi_bus_config_t bus_cfg = {
        .sclk_io_num = S3S_SCLK,
        .mosi_io_num = S3S_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = ST_H_RES * 40 * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = S3S_DC,
        .cs_gpio_num = S3S_CS,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST,
                                             &io_config, &io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,  /* Already reset manually above */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    /* 135x240 window inside the 240x320 controller space */
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 52, 40));

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    ESP_LOGI(TAG, "Panel init OK (StickS3 ST7789 %dx%d)", ST_H_RES, ST_V_RES);
    return ESP_OK;
}

void display_hal_draw(int x_start, int y_start, int x_end, int y_end,
                      const void *color_data)
{
    if (!s_panel) return;
    esp_lcd_panel_draw_bitmap(s_panel, x_start, y_start, x_end, y_end, color_data);
}

esp_err_t display_hal_init_touch(void)
{
    ESP_LOGI(TAG, "No touch controller on StickS3");
    return ESP_ERR_NOT_FOUND;
}

bool display_hal_touch_read(uint16_t *x, uint16_t *y)
{
    return false;
}

void display_hal_set_brightness(uint8_t percent)
{
    /* Plain GPIO backlight: on/off only */
    gpio_set_level(S3S_BLK, percent > 0 ? 1 : 0);
}

#elif CONFIG_DISPLAY_S3BOX3

/* ===========================================================================
 * ESP32-S3-BOX-3: 320x240 2.4" panel (ST7789 on current units)
 *
 * Pin profile per espressif/esp-bsp bsp/esp-box-3: SPI3 @ 40 MHz,
 * MOSI=6, SCLK=7, CS=5, DC=4, RST=48, backlight GPIO47 PWM active-high,
 * mirror both axes, no gap.
 * =========================================================================*/

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_ili9341.h"
#include "driver/ledc.h"
#include "driver/i2c.h"

#define BOX3_MOSI      6
#define BOX3_SCLK      7
#define BOX3_CS        5
#define BOX3_DC        4
#define BOX3_RST       48
#define BOX3_BLK       47
#define BOX3_I2C_SDA   8
#define BOX3_I2C_SCL   18

#define ST_H_RES       320
#define ST_V_RES       240

static esp_lcd_panel_handle_t s_panel = NULL;

/* ILI9342C init sequence from espressif/esp-bsp esp-box-3 (older units). */
static const ili9341_lcd_init_cmd_t box3_ili9342c_init[] = {
    {0xC8, (uint8_t []){0xFF, 0x93, 0x42}, 3, 0},
    {0xC0, (uint8_t []){0x0E, 0x0E}, 2, 0},
    {0xC5, (uint8_t []){0xD0}, 1, 0},
    {0xC1, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0x00, 0x03, 0x08, 0x06, 0x13, 0x09, 0x39, 0x39, 0x48,
                        0x02, 0x0a, 0x08, 0x17, 0x17, 0x0F}, 15, 0},
    {0xE1, (uint8_t []){0x00, 0x28, 0x29, 0x01, 0x0d, 0x03, 0x3f, 0x33, 0x52,
                        0x04, 0x0f, 0x0e, 0x37, 0x38, 0x0F}, 15, 0},
    {0xB1, (uint8_t []){0x00, 0x1B}, 2, 0},
    {0x36, (uint8_t []){0x08}, 1, 0},
    {0x3A, (uint8_t []){0x55}, 1, 0},
    {0xB7, (uint8_t []){0x06}, 1, 0},
    {0x11, (uint8_t []){0}, 0x80, 0},
    {0x29, (uint8_t []){0}, 0x80, 0},
    {0, (uint8_t []){0}, 0xff, 0},
};

/* Probe an I2C address; BOX-3 panel type follows the touch chip:
 * TT21100 (0x24) pairs with ST7789, GT911 (0x5D/0x14) with ILI9342C. */
static bool box3_i2c_probe(uint8_t addr)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return err == ESP_OK;
}

esp_err_t display_hal_init_panel(void)
{
    ESP_LOGI(TAG, "Initializing S3-BOX-3 panel (%dx%d)...", ST_H_RES, ST_V_RES);

    /* Identify the panel generation via the touch controller. */
    i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BOX3_I2C_SDA,
        .scl_io_num = BOX3_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    bool is_st7789 = false;
    if (i2c_param_config(I2C_NUM_0, &i2c_cfg) == ESP_OK
        && i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0) == ESP_OK) {
        is_st7789 = box3_i2c_probe(0x24);           /* TT21100 */
        bool gt911 = box3_i2c_probe(0x5D) || box3_i2c_probe(0x14);
        ESP_LOGI(TAG, "Touch probe: TT21100=%d GT911=%d -> %s panel",
                 is_st7789, gt911, is_st7789 ? "ST7789" : "ILI9342C");
    } else {
        ESP_LOGW(TAG, "I2C probe unavailable — defaulting to ILI9342C");
    }

    /* Backlight via LEDC PWM, active high */
    ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&bl_timer);
    ledc_channel_config_t bl_ch = {
        .gpio_num = BOX3_BLK,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 200,
        .hpoint = 0,
    };
    ledc_channel_config(&bl_ch);

    spi_bus_config_t bus_cfg = {
        .sclk_io_num = BOX3_SCLK,
        .mosi_io_num = BOX3_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = ST_H_RES * 40 * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = BOX3_DC,
        .cs_gpio_num = BOX3_CS,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST,
                                             &io_config, &io_handle));

    /* Per esp-bsp: the BOX-3 LCD reset line is ACTIVE HIGH — let the
     * driver own it (a manual low->high "release" holds the panel in
     * reset and shows a white screen). Color order is BGR. */
    const ili9341_vendor_config_t vendor_config = {
        .init_cmds = box3_ili9342c_init,
        .init_cmds_size = sizeof(box3_ili9342c_init) / sizeof(box3_ili9342c_init[0]),
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BOX3_RST,
        .flags.reset_active_high = 1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    if (is_st7789) {
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &s_panel));
    } else {
        panel_config.vendor_config = (void *)&vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_handle, &panel_config, &s_panel));
    }

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    /* Per esp-bsp: mirror both axes, no inversion, no gap */
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    ESP_LOGI(TAG, "Panel init OK (S3-BOX-3 %dx%d, %s)", ST_H_RES, ST_V_RES,
             is_st7789 ? "ST7789" : "ILI9342C");
    return ESP_OK;
}

void display_hal_draw(int x_start, int y_start, int x_end, int y_end,
                      const void *color_data)
{
    if (!s_panel) return;
    esp_lcd_panel_draw_bitmap(s_panel, x_start, y_start, x_end, y_end, color_data);
}

esp_err_t display_hal_init_touch(void)
{
    /* GT911/TT21100 touch not wired up yet — status view needs none. */
    ESP_LOGI(TAG, "Touch not enabled on S3-BOX-3 (status view)");
    return ESP_ERR_NOT_FOUND;
}

bool display_hal_touch_read(uint16_t *x, uint16_t *y)
{
    return false;
}

void display_hal_set_brightness(uint8_t percent)
{
    if (percent > 100) percent = 100;
    uint32_t duty = (uint32_t)percent * 255 / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

#else /* original S3 AMOLED path */

#include "driver/i2c.h"

/* ---- QSPI Pin Definitions (Waveshare board) ---- */
#define DISP_QSPI_CS       12
#define DISP_QSPI_CLK      11
#define DISP_QSPI_D0       4
#define DISP_QSPI_D1       5
#define DISP_QSPI_D2       6
#define DISP_QSPI_D3       7

/* ---- I2C (shared: touch + TCA9554 expander) ---- */
#define I2C_SDA             15
#define I2C_SCL             14
#define TOUCH_INT_PIN       21
#define I2C_MASTER_NUM      I2C_NUM_0
#define I2C_MASTER_FREQ_HZ  400000

/* ---- TCA9554 I/O expander ---- */
#define TCA9554_ADDR        0x20
#define TCA9554_REG_OUTPUT  0x01
#define TCA9554_REG_CONFIG  0x03

/* ---- FT3168 touch controller ---- */
#define FT3168_ADDR         0x38

/* ---- Display dimensions ---- */
#define DISP_H_RES          368
#define DISP_V_RES          448

/* ---- QSPI opcodes (packed into lcd_cmd bits [31:24]) ---- */
#define LCD_OPCODE_WRITE_CMD   0x02
#define LCD_OPCODE_WRITE_COLOR 0x32

/* ---- State ---- */
static esp_lcd_panel_io_handle_t s_io_handle = NULL;
static bool s_i2c_initialized = false;
static bool s_touch_initialized = false;

/* ---- I2C helpers ---- */

static esp_err_t i2c_write_reg(uint8_t dev_addr, uint8_t reg, const uint8_t *data, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    if (data && len > 0) {
        i2c_master_write(cmd, data, len, true);
    }
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t i2c_read_reg(uint8_t dev_addr, uint8_t reg, uint8_t *data, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, data, len, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t init_i2c_bus(void)
{
    if (s_i2c_initialized) return ESP_OK;

    i2c_config_t i2c_cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = I2C_SDA,
        .scl_io_num       = I2C_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };

    esp_err_t ret = i2c_param_config(I2C_MASTER_NUM, &i2c_cfg);
    if (ret != ESP_OK) return ret;

    ret = i2c_driver_install(I2C_MASTER_NUM, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK) return ret;

    s_i2c_initialized = true;
    ESP_LOGI(TAG, "I2C bus init OK (SDA=%d, SCL=%d)", I2C_SDA, I2C_SCL);
    return ESP_OK;
}

/* ---- TCA9554 I/O expander: toggle pins for display power/reset ---- */

static esp_err_t tca9554_init_display_power(void)
{
    /* Set pins 0, 1, 2 as outputs */
    uint8_t cfg = 0xF8;
    esp_err_t ret = i2c_write_reg(TCA9554_ADDR, TCA9554_REG_CONFIG, &cfg, 1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "TCA9554 not found at 0x%02X: %s", TCA9554_ADDR, esp_err_to_name(ret));
        return ret;
    }

    /* Set pins 0,1,2 LOW (reset state) */
    uint8_t out = 0x00;
    i2c_write_reg(TCA9554_ADDR, TCA9554_REG_OUTPUT, &out, 1);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Set pins 0,1,2 HIGH (power on + release reset) */
    out = 0x07;
    i2c_write_reg(TCA9554_ADDR, TCA9554_REG_OUTPUT, &out, 1);
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "TCA9554 display power/reset toggled");
    return ESP_OK;
}

/* ---- Panel IO helpers: send commands via esp_lcd QSPI panel IO ---- */

static esp_err_t panel_write_cmd(uint8_t dcs_cmd, const void *data, size_t data_len)
{
    /* Pack as 32-bit lcd_cmd: [31:24]=opcode, [23:8]=dcs_cmd, [7:0]=0 */
    uint32_t lcd_cmd = ((uint32_t)LCD_OPCODE_WRITE_CMD << 24) | ((uint32_t)dcs_cmd << 8);
    return esp_lcd_panel_io_tx_param(s_io_handle, (int)lcd_cmd, data, data_len);
}

static esp_err_t panel_write_color(const void *color_data, size_t data_len)
{
    /* RAMWR (0x2C) packed as 32-bit lcd_cmd with quad opcode */
    uint32_t lcd_cmd = ((uint32_t)LCD_OPCODE_WRITE_COLOR << 24) | (0x2C << 8);
    return esp_lcd_panel_io_tx_color(s_io_handle, (int)lcd_cmd, color_data, data_len);
}

/* ---- SH8601 init sequence (from Waveshare reference) ---- */

typedef struct {
    uint8_t cmd;
    uint8_t data[4];
    uint8_t data_len;
    uint16_t delay_ms;
} sh8601_init_cmd_t;

static const sh8601_init_cmd_t sh8601_init_cmds[] = {
    {0x11, {0x00},                   0, 120},  /* Sleep Out + 120ms */
    {0x44, {0x01, 0xD1},             2, 0},    /* Partial area */
    {0x35, {0x00},                   1, 0},    /* Tearing Effect ON */
    {0x53, {0x20},                   1, 10},   /* Write CTRL Display */
    {0x2A, {0x00, 0x00, 0x01, 0x6F}, 4, 0},   /* CASET: 0-367 */
    {0x2B, {0x00, 0x00, 0x01, 0xBF}, 4, 0},   /* RASET: 0-447 */
    {0x51, {0x00},                   1, 10},   /* Brightness: 0 */
    {0x29, {0x00},                   0, 10},   /* Display ON */
    {0x51, {0xFF},                   1, 0},    /* Brightness: max */
    {0x00, {0x00},                   0xFF, 0}, /* End sentinel */
};

static esp_err_t send_init_sequence(void)
{
    for (int i = 0; sh8601_init_cmds[i].data_len != 0xFF; i++) {
        const sh8601_init_cmd_t *cmd = &sh8601_init_cmds[i];
        esp_err_t ret = panel_write_cmd(
            cmd->cmd,
            cmd->data_len > 0 ? cmd->data : NULL,
            cmd->data_len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "CMD 0x%02X failed: %s", cmd->cmd, esp_err_to_name(ret));
            return ret;
        }
        if (cmd->delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(cmd->delay_ms));
        }
    }
    return ESP_OK;
}

/* ---- Public API ---- */

esp_err_t display_hal_init_panel(void)
{
    ESP_LOGI(TAG, "Initializing Waveshare AMOLED 1.8\" (SH8601 368x448)...");

    /* Step 1: Init I2C bus */
    esp_err_t ret = init_i2c_bus();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus init failed");
        return ESP_ERR_NOT_FOUND;
    }

    /* Step 2: TCA9554 display power/reset (optional — only present on Waveshare board) */
    ret = tca9554_init_display_power();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "TCA9554 not found — assuming display power is always-on (direct wiring)");
        /* Continue without TCA9554 — the display may be powered directly */
    }

    /* Step 3: Initialize SPI bus */
    spi_bus_config_t bus_cfg = {
        .sclk_io_num     = DISP_QSPI_CLK,
        .data0_io_num    = DISP_QSPI_D0,
        .data1_io_num    = DISP_QSPI_D1,
        .data2_io_num    = DISP_QSPI_D2,
        .data3_io_num    = DISP_QSPI_D3,
        .max_transfer_sz = DISP_H_RES * DISP_V_RES * 2,
    };

    ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ESP_ERR_NOT_FOUND;
    }

    /* Step 4: Create panel IO with QSPI mode */
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num       = -1,       /* No DC pin in QSPI mode */
        .cs_gpio_num       = DISP_QSPI_CS,
        .pclk_hz           = 40 * 1000 * 1000,
        .lcd_cmd_bits      = 32,       /* 32-bit command: [opcode|dcs_cmd|0x00] */
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
        .flags = {
            .quad_mode = true,
        },
    };

    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &s_io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel IO init failed: %s", esp_err_to_name(ret));
        spi_bus_free(SPI2_HOST);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "QSPI panel IO created (40MHz, quad mode)");

    /* Step 5: Send SH8601 init sequence */
    ret = send_init_sequence();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SH8601 init sequence failed");
        esp_lcd_panel_io_del(s_io_handle);
        spi_bus_free(SPI2_HOST);
        s_io_handle = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    /* Step 6: Draw test pattern — cyan bar at top */
    ESP_LOGI(TAG, "Drawing test pattern...");
    uint16_t *line_buf = heap_caps_malloc(DISP_H_RES * 2, MALLOC_CAP_DMA);
    if (line_buf) {
        uint8_t caset[4] = {0, 0, (DISP_H_RES - 1) >> 8, (DISP_H_RES - 1) & 0xFF};
        uint8_t raset[4] = {0, 0, (DISP_V_RES - 1) >> 8, (DISP_V_RES - 1) & 0xFF};
        panel_write_cmd(0x2A, caset, 4);
        panel_write_cmd(0x2B, raset, 4);

        for (int y = 0; y < DISP_V_RES; y++) {
            uint16_t color = (y < 30) ? 0x07FF : 0x0841;
            for (int x = 0; x < DISP_H_RES; x++) {
                line_buf[x] = color;
            }
            panel_write_color(line_buf, DISP_H_RES * 2);
        }
        free(line_buf);
        ESP_LOGI(TAG, "Test pattern drawn");
    }

    ESP_LOGI(TAG, "SH8601 panel init OK (%dx%d)", DISP_H_RES, DISP_V_RES);
    return ESP_OK;
}

void display_hal_draw(int x_start, int y_start, int x_end, int y_end,
                      const void *color_data)
{
    if (!s_io_handle) return;

    /* SH8601 requires coordinates divisible by 2 */
    x_start &= ~1;
    y_start &= ~1;
    if (x_end & 1) x_end++;
    if (y_end & 1) y_end++;
    if (x_end > DISP_H_RES) x_end = DISP_H_RES;
    if (y_end > DISP_V_RES) y_end = DISP_V_RES;

    uint8_t caset[4] = {
        (x_start >> 8) & 0xFF, x_start & 0xFF,
        ((x_end - 1) >> 8) & 0xFF, (x_end - 1) & 0xFF,
    };
    panel_write_cmd(0x2A, caset, 4);

    uint8_t raset[4] = {
        (y_start >> 8) & 0xFF, y_start & 0xFF,
        ((y_end - 1) >> 8) & 0xFF, (y_end - 1) & 0xFF,
    };
    panel_write_cmd(0x2B, raset, 4);

    size_t len = (x_end - x_start) * (y_end - y_start) * 2;
    panel_write_color(color_data, len);
}

esp_err_t display_hal_init_touch(void)
{
    ESP_LOGI(TAG, "Probing FT3168 touch controller...");

    if (!s_i2c_initialized) {
        esp_err_t ret = init_i2c_bus();
        if (ret != ESP_OK) return ESP_ERR_NOT_FOUND;
    }

    gpio_config_t int_cfg = {
        .pin_bit_mask = (1ULL << TOUCH_INT_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&int_cfg);

    uint8_t chip_id = 0;
    esp_err_t ret = i2c_read_reg(FT3168_ADDR, 0xA8, &chip_id, 1);
    if (ret != ESP_OK || chip_id == 0x00 || chip_id == 0xFF) {
        ESP_LOGW(TAG, "FT3168 not found (ret=%s, id=0x%02X)", esp_err_to_name(ret), chip_id);
        return ESP_ERR_NOT_FOUND;
    }

    s_touch_initialized = true;
    ESP_LOGI(TAG, "FT3168 touch init OK (chip_id=0x%02X)", chip_id);
    return ESP_OK;
}

bool display_hal_touch_read(uint16_t *x, uint16_t *y)
{
    if (!s_touch_initialized) return false;

    uint8_t buf[7] = {0};
    esp_err_t ret = i2c_read_reg(FT3168_ADDR, 0x01, buf, 7);
    if (ret != ESP_OK) return false;

    uint8_t num_points = buf[1];
    if (num_points == 0 || num_points > 2) return false;

    *x = ((buf[2] & 0x0F) << 8) | buf[3];
    *y = ((buf[4] & 0x0F) << 8) | buf[5];
    return true;
}

void display_hal_set_brightness(uint8_t percent)
{
    if (!s_io_handle) return;
    if (percent > 100) percent = 100;
    uint8_t val = (uint8_t)((uint32_t)percent * 255 / 100);
    panel_write_cmd(0x51, &val, 1);
}

#endif /* !CONFIG_IDF_TARGET_ESP32C6 */
#endif /* CONFIG_DISPLAY_ENABLE */
