/**
 * @file mmwave_sensor.h
 * @brief ADR-063: 60 GHz mmWave sensor auto-detection and UART driver.
 *
 * Supports:
 *   - Seeed MR60BHA2 (60 GHz, heart rate + breathing + presence)
 *   - HLK-LD2410  (24 GHz, presence + distance)
 *
 * Auto-detects sensor type at boot by probing UART for known frame headers.
 * Runs a background task that parses incoming frames and updates shared state.
 */

#ifndef MMWAVE_SENSOR_H
#define MMWAVE_SENSOR_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* ---- Sensor type enumeration ---- */
typedef enum {
    MMWAVE_TYPE_NONE      = 0,  /**< No sensor detected. */
    MMWAVE_TYPE_MR60BHA2  = 1,  /**< Seeed MR60BHA2 (60 GHz, HR + BR). */
    MMWAVE_TYPE_LD2410    = 2,  /**< HLK-LD2410 (24 GHz, presence + range). */
    MMWAVE_TYPE_C1001     = 3,  /**< DFRobot C1001 (60 GHz, presence + fall + HR + BR). */
    MMWAVE_TYPE_AT581X    = 4,  /**< AirTouch AT581x (5.8 GHz, PRESENCE/motion only). */
    MMWAVE_TYPE_MOCK      = 99, /**< Mock sensor for QEMU testing. */
} mmwave_type_t;

/* ---- Capability flags ---- */
#define MMWAVE_CAP_HEART_RATE   (1 << 0)
#define MMWAVE_CAP_BREATHING    (1 << 1)
#define MMWAVE_CAP_PRESENCE     (1 << 2)
#define MMWAVE_CAP_DISTANCE     (1 << 3)
#define MMWAVE_CAP_FALL         (1 << 4)
#define MMWAVE_CAP_MULTI_TARGET (1 << 5)

/* ---- Shared mmWave state (updated by background task) ---- */
typedef struct {
    /* Detection */
    mmwave_type_t type;         /**< Detected sensor type. */
    uint16_t      capabilities; /**< Bitmask of MMWAVE_CAP_* flags. */
    bool          detected;     /**< True if sensor responded on UART. */

    /* Vital signs (MR60BHA2) */
    float    heart_rate_bpm;    /**< Heart rate in BPM (0 if unavailable). */
    float    breathing_rate;    /**< Breathing rate in breaths/min. */

    /* Presence and range (LD2410 / MR60BHA2 / C1001) */
    bool     person_present;    /**< True if person detected. */
    bool     fall_detected;     /**< True if a fall event is reported (C1001). */
    float    distance_cm;       /**< Distance to nearest target in cm. */
    uint8_t  target_count;      /**< Number of detected targets. */

    /* Track point (C1001 fall mode): sensor-frame position, cm.
     * x = lateral (sign = side), y = forward from the sensor. */
    bool     track_valid;       /**< True if a recent track point exists. */
    int16_t  track_x_cm;        /**< Filtered (alpha-beta) position.      */
    int16_t  track_y_cm;
    int64_t  track_us;          /**< Timestamp of the last track point. */

    /* C1001 motion: 0 none, 1 static (present, still), 2 active; 0xFF = n/a */
    uint8_t  motion;

    /* C1001 fall-mode installation geometry as stored in the sensor (read
     * back on entering fall mode). Track x/y are computed from these, so a
     * mismatch with the real mounting skews the map. */
    bool     geom_valid;
    int16_t  install_angle_x, install_angle_y, install_angle_z;  /* degrees */
    uint16_t install_height_cm;
    uint16_t auto_height_cm;    /**< Sensor's own height measurement. */

    /* Quality metrics */
    uint32_t frame_count;       /**< Total parsed frames since boot. */
    uint32_t error_count;       /**< Parse errors / CRC failures. */
    int64_t  last_update_us;    /**< Timestamp of last valid frame. */
} mmwave_state_t;

/**
 * Initialize the mmWave sensor subsystem.
 *
 * Probes the configured UART for known sensor types. If a sensor is
 * detected, starts a background FreeRTOS task to parse incoming frames.
 *
 * @param uart_tx_pin  GPIO pin for UART TX (to sensor RX). Use -1 for default.
 * @param uart_rx_pin  GPIO pin for UART RX (from sensor TX). Use -1 for default.
 * @return ESP_OK if sensor detected, ESP_ERR_NOT_FOUND if no sensor.
 */
esp_err_t mmwave_sensor_init(int uart_tx_pin, int uart_rx_pin);

/**
 * Get a snapshot of the current mmWave state (thread-safe copy).
 *
 * @param state  Output state struct.
 * @return true if valid data is available (sensor detected and running).
 */
bool mmwave_sensor_get_state(mmwave_state_t *state);

/**
 * Get the detected sensor type name as a string.
 */
const char *mmwave_type_name(mmwave_type_t type);

/**
 * Get the boot-time wiring/probe diagnostic transcript (for remote
 * debugging of headless nodes via the OTA HTTP server).
 */
const char *mmwave_get_diag(void);

/* DFRobot C1001 work modes — mutually exclusive on the sensor (its DSP runs
 * one pipeline at a time). */
#define MMWAVE_C1001_MODE_VITALS 0   /* sleep mode: breathing + heart rate */
#define MMWAVE_C1001_MODE_FALL   1   /* fall mode: track + fall detection  */

/* Mode policy: a fixed mode, or AUTO = FALL while the person moves, VITALS
 * once they have been still for a while (see mmwave_sensor.c). */
#define MMWAVE_C1001_POLICY_VITALS MMWAVE_C1001_MODE_VITALS
#define MMWAVE_C1001_POLICY_FALL   MMWAVE_C1001_MODE_FALL
#define MMWAVE_C1001_POLICY_AUTO   2

/** Work mode the sensor is in right now (MMWAVE_C1001_MODE_*). */
uint8_t mmwave_c1001_get_mode(void);

/** Selected policy (MMWAVE_C1001_POLICY_*). */
uint8_t mmwave_c1001_get_policy(void);

/**
 * Select a mode policy. Applied by the UART task (which owns the port) within
 * ~150 ms and persisted to NVS (csi_cfg/mw_mode). No-op for other sensors.
 */
void mmwave_c1001_set_policy(uint8_t policy);

#endif /* MMWAVE_SENSOR_H */
