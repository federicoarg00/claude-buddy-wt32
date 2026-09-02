#pragma once
#include <lvgl.h>
#include <stdint.h>

/* Settings tile: WiFi network picker + pomodoro durations + screen sleep.
 * Values persist in NVS (namespace "cfg"). */

void settings_init();                 /* load NVS — call before pomodoro_create */
void settings_create(lv_obj_t *tile);

uint32_t settings_focus_ms();
uint32_t settings_break_ms();
uint32_t settings_longbreak_ms();
uint32_t settings_screen_sleep_ms();  /* 0 = never turn the screen off */
