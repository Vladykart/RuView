/**
 * @file audio_beep.c
 * @brief StickS3 proximity sonar — see audio_beep.h for the behavior spec.
 *
 * Pins per M5StickS3 (ES8311 codec): I2S MCLK=18 BCK=17 WS=15 DOUT=14,
 * codec on the PM1 I2C bus (I2C_NUM_1, SDA47/SCL48), PA via M5PM1 GPIO3.
 */
#include "audio_beep.h"
#include "sdkconfig.h"

#if CONFIG_DISPLAY_STICKS3

#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/i2c.h"
#include "es8311.h"
#include "display_hal.h"
#include "mmwave_sensor.h"

static const char *TAG = "beep";

#define BEEP_SR        16000              /* sample rate                    */
#define BEEP_I2C_PORT  I2C_NUM_1          /* shared with the M5PM1 PMIC     */
#define BEEP_GPIO_MCLK 18
#define BEEP_GPIO_BCK  17
#define BEEP_GPIO_WS   15
#define BEEP_GPIO_DOUT 14
#define BEEP_VOLUME    72                 /* ES8311 0..100                  */

/* Chirp map: distance (m) -> repeat interval (s) and tone (Hz). */
#define D_NEAR_M       0.3f
#define D_FAR_M        6.0f
#define IVAL_NEAR_S    0.12f
#define IVAL_FAR_S     1.6f
#define TONE_NEAR_HZ   2800.0f
#define TONE_FAR_HZ    900.0f
#define CHIRP_MS       55
#define PA_IDLE_OFF_S  2.0f               /* amp off after this much silence */

static i2s_chan_handle_t s_tx = NULL;
static volatile bool s_muted = false;
static bool s_ready = false;
static bool s_pa_on = false;
/* Live status for the on-device UI (written by the beep task only). */
static volatile bool s_alarming = false;
static volatile bool s_fall_acked = false;
static volatile bool s_chirping = false;
static volatile float s_cur_ival = 0, s_cur_hz = 0;

static void pa_set(bool on)
{
    if (on == s_pa_on) return;
    if (display_hal_audio_pa(on) == ESP_OK) s_pa_on = on;
    if (on) vTaskDelay(pdMS_TO_TICKS(12));   /* AW8737 settle, avoids pop */
}

/* Synthesize and play one tone burst (blocking, <= ~120 ms). */
static void play_tone(float hz, int ms)
{
    enum { CHUNK = 256 };
    static int16_t buf[CHUNK];
    int total = BEEP_SR * ms / 1000;
    float phase = 0, step = 2.0f * (float)M_PI * hz / BEEP_SR;
    int fade = BEEP_SR * 4 / 1000;           /* 4 ms fade in/out, no clicks */
    for (int done = 0; done < total; ) {
        int n = (total - done > CHUNK) ? CHUNK : (total - done);
        for (int i = 0; i < n; i++) {
            int k = done + i;
            float env = 1.0f;
            if (k < fade) env = (float)k / fade;
            else if (total - k < fade) env = (float)(total - k) / fade;
            buf[i] = (int16_t)(sinf(phase) * env * 12000);
            phase += step;
        }
        size_t written = 0;
        if (i2s_channel_write(s_tx, buf, n * sizeof(int16_t), &written,
                              pdMS_TO_TICKS(200)) != ESP_OK) return;
        done += n;
    }
}

static float mapf(float x, float x0, float x1, float y0, float y1)
{
    float t = (x - x0) / (x1 - x0);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return y0 + t * (y1 - y0);
}

static void beep_task(void *arg)
{
    (void)arg;
    float last_beep = -1e9f, last_sound = -1e9f;
    bool alarm_hi = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        float now = (float)(xTaskGetTickCount() * portTICK_PERIOD_MS) / 1000.0f;
        mmwave_state_t mw;
        bool has = mmwave_sensor_get_state(&mw);

        /* Fall alarm: a safety signal, so it sounds even when the proximity
         * sonar is muted. A button press acknowledges it until the sensor
         * clears the fall state. */
        if (has && !mw.fall_detected) s_fall_acked = false;
        s_alarming = has && mw.fall_detected && !s_fall_acked;
        if (s_alarming) {
            pa_set(true);
            play_tone(alarm_hi ? 1500 : 950, 140);
            alarm_hi = !alarm_hi;
            last_sound = now;
            continue;
        }

        if (s_muted || !has) {
            s_chirping = false;
            if (s_pa_on && now - last_sound > PA_IDLE_OFF_S) pa_set(false);
            continue;
        }
        if (mw.person_present && mw.distance_cm > 0) {
            float d = mw.distance_cm / 100.0f;
            float ival = mapf(d, D_NEAR_M, D_FAR_M, IVAL_NEAR_S, IVAL_FAR_S);
            float hz = mapf(d, D_NEAR_M, D_FAR_M, TONE_NEAR_HZ, TONE_FAR_HZ);
            s_chirping = true;
            s_cur_ival = ival;
            s_cur_hz = hz;
            if (now - last_beep >= ival) {
                pa_set(true);
                play_tone(hz, CHIRP_MS);
                last_beep = last_sound = now;
            }
            continue;
        }
        s_chirping = false;
        if (s_pa_on && now - last_sound > PA_IDLE_OFF_S) pa_set(false);
    }
}

esp_err_t audio_beep_start(void)
{
    /* ES8311 codec on the PM1 I2C bus (installed by display_hal). */
    es8311_handle_t codec = es8311_create(BEEP_I2C_PORT, ES8311_ADDRRES_0);
    if (!codec) {
        ESP_LOGW(TAG, "ES8311 not found — beeper disabled");
        return ESP_ERR_NOT_FOUND;
    }
    const es8311_clock_config_t clk = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = BEEP_SR * 256,
        .sample_frequency = BEEP_SR,
    };
    esp_err_t err = es8311_init(codec, &clk, ES8311_RESOLUTION_16,
                                ES8311_RESOLUTION_16);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ES8311 init failed: %s", esp_err_to_name(err));
        return err;
    }
    es8311_voice_volume_set(codec, BEEP_VOLUME, NULL);

    i2s_chan_config_t ch = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    err = i2s_new_channel(&ch, &s_tx, NULL);
    if (err != ESP_OK) return err;
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BEEP_SR),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BEEP_GPIO_MCLK, .bclk = BEEP_GPIO_BCK,
            .ws = BEEP_GPIO_WS, .dout = BEEP_GPIO_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    if ((err = i2s_channel_init_std_mode(s_tx, &std)) != ESP_OK) return err;
    if ((err = i2s_channel_enable(s_tx)) != ESP_OK) return err;

    if (xTaskCreatePinnedToCore(beep_task, "beep", 4096, NULL, 3, NULL, 0)
        != pdPASS) return ESP_FAIL;
    s_ready = true;
    ESP_LOGI(TAG, "proximity sonar ready (ES8311, I2S0, PA via PM1 G3)");
    return ESP_OK;
}

bool audio_beep_toggle_mute(void)
{
    s_muted = !s_muted;
    ESP_LOGI(TAG, "beeper %s", s_muted ? "MUTED" : "unmuted");
    return s_muted;
}

bool audio_beep_muted(void) { return s_muted; }

void audio_beep_get_status(audio_beep_status_t *out)
{
    out->ready = s_ready;
    out->muted = s_muted;
    out->alarming = s_alarming;
    out->chirping = s_chirping && !s_muted;
    out->interval_s = s_cur_ival;
    out->tone_hz = s_cur_hz;
}

bool audio_beep_ack_alarm(void)
{
    if (!s_alarming) return false;
    s_fall_acked = true;
    ESP_LOGI(TAG, "fall alarm acknowledged");
    return true;
}

#else /* !CONFIG_DISPLAY_STICKS3 */

esp_err_t audio_beep_start(void) { return ESP_ERR_NOT_SUPPORTED; }
bool audio_beep_toggle_mute(void) { return true; }
bool audio_beep_muted(void) { return true; }
void audio_beep_get_status(audio_beep_status_t *out)
{
    *out = (audio_beep_status_t){ .muted = true };
}
bool audio_beep_ack_alarm(void) { return false; }

#endif
