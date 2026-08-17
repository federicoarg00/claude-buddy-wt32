#pragma once
#include <lvgl.h>

/* Build the pomodoro UI inside the given tileview tile. */
void pomodoro_create(lv_obj_t *tile);

/* Feed the current local date (YYYY-MM-DD, from the companion) so the
 * daily cycle counter resets when the day changes. Safe to call often. */
void pomodoro_set_date(const char *dateLocal);

/* ---- history (for the 30-day calendar view) ---- */
struct PomoDay {
  uint16_t day;   /* days since 1970-01-01, local date */
  uint16_t count; /* completed focus cycles that day */
};
int pomodoro_get_history(PomoDay *out, int maxN); /* past days, oldest first */
int pomodoro_today_count();
uint16_t pomodoro_today_daynum();                 /* 0 = date not known yet */
void pomodoro_set_on_change(void (*cb)());        /* count/date changed */
void pomodoro_debug_dump();                       /* print NVS state to serial */
uint32_t pomodoro_idle_ms();                      /* ms since it last ran or was touched */
