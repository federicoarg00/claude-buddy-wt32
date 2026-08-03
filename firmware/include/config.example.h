/* Copy this file to config.h and fill in your values. config.h is gitignored. */
#pragma once

#define WIFI_SSID "TU_WIFI"
#define WIFI_PASS "TU_PASSWORD"

/* URL of the companion server running on your PC (companion/server.js).
 * Use the PC's IP on the SAME network the display's WiFi joins
 * (here that's the Wi-Fi adapter, not Ethernet — they're different subnets). */
#define COMPANION_URL "http://192.168.100.46:8787/status"

/* How often to poll the companion, in milliseconds. */
#define POLL_INTERVAL_MS 15000

/* Screen brightness 0-255. */
#define BACKLIGHT_LEVEL 200
