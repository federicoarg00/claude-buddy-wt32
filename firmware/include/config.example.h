/* Copy this file to config.h and fill in your values. config.h is gitignored. */
#pragma once

#define WIFI_SSID "TU_WIFI"
#define WIFI_PASS "TU_PASSWORD"

/* Extra known networks (work, phone hotspot, ...). The buddy connects to
 * whichever known network it finds. Leave empty if you only use one. */
#define WIFI_EXTRA_NETWORKS \
  /* X("WIFI_TRABAJO", "password") */ \
  /* X("HotspotCelu", "password") */

/* Local timezone offset for the pomodoro daily counter in direct mode. */
#define TZ_OFFSET_SECONDS (-3 * 3600) /* Argentina */

/* WiFi config portal (long-press Clawd, or automatic when no known network
 * is found). The AP password is shown on the display. */
#define PORTAL_AP_SSID "ClaudeBuddy-Setup"
#define PORTAL_AP_PASS "clawd123"

/* URL of the companion server running on your PC (companion/server.js). */
#define COMPANION_URL "http://192.168.100.46:8787/status"

/* If the PC can be reached at different IPs depending on the network the
 * buddy joins, list them all — each poll tries them in order. */
#define COMPANION_URLS \
  X(COMPANION_URL) \
  /* X("http://192.168.68.50:8787/status") */

/* How often to poll the companion, in milliseconds. */
#define POLL_INTERVAL_MS 15000

/* Screen brightness 0-255. */
#define BACKLIGHT_LEVEL 200
