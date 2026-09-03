#pragma once
#include <lvgl.h>

enum ConnState {
  CONN_BOOTING,
  CONN_NO_WIFI,
  CONN_NO_COMPANION,
  CONN_OK,
  CONN_PORTAL,   /* WiFi config portal (AP) is running */
};

enum DataSource {
  SRC_NONE,
  SRC_COMPANION,  /* rich data via the PC companion (home) */
  SRC_DIRECT,     /* straight from the Anthropic API (anywhere) */
};

struct LimitBar {
  char label[16];
  char kind[16];
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
  DataSource source = SRC_NONE;
  bool active = false;
  int activeSessions = 0;
  long lastActivityAgoSec = -1;
  long tokensToday = 0; /* -1 = unavailable (direct mode) */
  char date[12] = "";   /* local date YYYY-MM-DD (companion or NTP) */
};

void ui_init();
void ui_update(ConnState conn, const BuddyData &d);
void ui_show_pomodoro_tile();
void ui_show_buddy_tile();
/* Show "provisioná el buddy: IP x.x.x.x" while no OAuth token is stored. */
void ui_set_provision_hint(const char *ip);
/* Free-form hint under the status label (portal instructions etc). */
void ui_set_hint_text(const char *text);
/* Long-press on Claudito's body (e.g. to open the WiFi portal). */
void ui_on_body_longpress(void (*cb)(void));
