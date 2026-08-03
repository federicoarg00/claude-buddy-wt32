#pragma once
#include <lvgl.h>

enum ConnState {
  CONN_BOOTING,
  CONN_NO_WIFI,
  CONN_NO_COMPANION,
  CONN_OK,
};

struct LimitBar {
  char label[16];
  int pct;              // -1 = unknown
  char severity[12];
  long resetsInSec;     // -1 = unknown
  bool isActive;
};

struct BuddyData {
  bool companionOk = false;
  char plan[12] = "";
  int nLimits = 0;
  LimitBar limits[4];
  bool active = false;
  int activeSessions = 0;
  long lastActivityAgoSec = -1;
  long tokensToday = 0;
  char date[12] = "";   /* companion's local date YYYY-MM-DD */
};

void ui_init();
void ui_update(ConnState conn, const BuddyData &d);
void ui_show_pomodoro_tile();
