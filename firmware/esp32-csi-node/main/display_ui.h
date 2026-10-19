/**
 * @file display_ui.h
 * @brief ADR-045: LVGL 4-view swipeable UI for CSI node stats.
 *
 * Views: Dashboard | Vitals | Presence | System
 * Dark theme with cyan (#00d4ff) accent.
 */

#ifndef DISPLAY_UI_H
#define DISPLAY_UI_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Create all LVGL views on the given tileview parent. */
void display_ui_create(lv_obj_t *parent);

/**
 * Update all views with latest data. Called every display refresh cycle.
 * Reads from edge_get_vitals() and edge_get_multi_person() internally.
 */
void display_ui_update(void);

/**
 * StickS3/BOX-3 simple UI only: power the mmWave sensor's 5 V rail at boot if
 * the battery allows, BEFORE mmwave_sensor_init() starts talking to it.
 * Blocks ~1.5 s while the sensor settles.
 */
void display_ui_sensor_power_boot(void);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_UI_H */
