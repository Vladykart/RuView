/**
 * @file audio_beep.h
 * @brief StickS3 proximity sonar: Geiger-style chirps from the C1001 state.
 *
 * Person present -> short chirps; rate AND pitch rise as the target closes
 * (6 m: low blip every ~1.6 s ... 0.3 m: high pip every ~0.12 s), so distance
 * is audible without looking at the screen. Fall -> two-tone alarm.
 * Clear -> silence, power amp off (no idle hiss, saves battery).
 *
 * Audio path (StickS3): I2S0 -> ES8311 codec (I2C 0x18 on the PM1 bus) ->
 * AW8737 PA, gated by M5PM1 GPIO3 (display_hal_audio_pa).
 */
#ifndef AUDIO_BEEP_H
#define AUDIO_BEEP_H

#include <stdbool.h>
#include "esp_err.h"

/** Start the beeper task (after display_hal panel init: shares the PM1 I2C
 *  bus). Safe to call when audio hardware is absent — it logs and disables. */
esp_err_t audio_beep_start(void);

/** Sonar volume levels. The fall alarm never plays below BEEP_VOL_MID. */
typedef enum {
    BEEP_VOL_OFF = 0,
    BEEP_VOL_LOW,
    BEEP_VOL_MID,
    BEEP_VOL_HIGH,
} beep_vol_t;

/** Cycle LOW -> MID -> HIGH -> OFF -> LOW (hold KEY1); saved to NVS
 *  (csi_cfg/beep_vol). Returns the new level. */
beep_vol_t audio_beep_cycle_volume(void);

beep_vol_t audio_beep_volume(void);

/** True when the proximity sonar is silent (volume OFF). */
bool audio_beep_muted(void);

/** Live sonar state for the on-device UI. */
typedef struct {
    bool  ready;        /* codec + task initialized          */
    bool  muted;        /* volume OFF                        */
    beep_vol_t volume;
    bool  alarming;     /* fall alarm sounding (ignores mute)*/
    bool  chirping;     /* person present, chirps being sent */
    float interval_s;   /* current chirp repeat interval     */
    float tone_hz;      /* current chirp tone                */
} audio_beep_status_t;

void audio_beep_get_status(audio_beep_status_t *out);

/** Silence a sounding fall alarm until the sensor clears the fall state.
 *  Returns true if an alarm was acknowledged. */
bool audio_beep_ack_alarm(void);

#endif /* AUDIO_BEEP_H */
