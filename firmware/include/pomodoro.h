#pragma once
#include <lvgl.h>

/* Build the pomodoro UI inside the given tileview tile. */
void pomodoro_create(lv_obj_t *tile);

/* Feed the current local date (YYYY-MM-DD, from the companion) so the
 * daily cycle counter resets when the day changes. Safe to call often. */
void pomodoro_set_date(const char *dateLocal);
