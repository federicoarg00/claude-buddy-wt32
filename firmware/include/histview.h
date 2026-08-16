#pragma once
#include <lvgl.h>

/* 30-day pomodoro calendar tile. */
void histview_create(lv_obj_t *tile);
void histview_refresh(); /* call when counts or the date change */
