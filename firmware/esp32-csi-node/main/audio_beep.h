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

/** Toggle mute (long-press BOOT). Returns the new muted state. */
bool audio_beep_toggle_mute(void);

bool audio_beep_muted(void);

/** Live sonar state for the on-device UI. */
typedef struct {
    bool  ready;        /* codec + task initialized          */
    bool  muted;
    bool  chirping;     /* person present, chirps being sent */
    float interval_s;   /* current chirp repeat interval     */
    float tone_hz;      /* current chirp tone                */
} audio_beep_status_t;

void audio_beep_get_status(audio_beep_status_t *out);

#endif /* AUDIO_BEEP_H */
