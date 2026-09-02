#pragma once
#include <stdint.h>

/* WiFi manager: connects to known networks (compile-time + NVS-saved) and
 * runs a captive config portal (AP "ClaudeBuddy-Setup") to add new ones. */

void wifimgr_init();                      /* load saved networks from NVS */
bool wifimgr_connect(uint32_t perTryMs);  /* try all known networks */

void wifimgr_request_portal();            /* ask for the portal (long-press / serial) */
bool wifimgr_portal_requested();
void wifimgr_start_portal(bool keepSta);  /* keepSta: stay on current WiFi too */
void wifimgr_stop_portal();
bool wifimgr_portal_active();
bool wifimgr_portal_should_close();       /* saved new creds or timed out */
void wifimgr_handle();                    /* call from loop(): DNS + HTTP */

const char *wifimgr_ap_ssid();
const char *wifimgr_ap_pass();

/* Known-network list (compile-time + portal-saved) and manual selection.
 * Pinning a network makes the buddy connect ONLY to it, so it can't stay
 * trapped on a misbehaving one; -1 = automatic best-signal mode. */
int wifimgr_known_count();
const char *wifimgr_known_ssid(int i);
void wifimgr_pin(int idx);            /* -1 = auto; triggers a reconnect */
int wifimgr_pinned();
bool wifimgr_take_reconnect();        /* consumed by net_task */
bool wifimgr_connect_pinned(uint32_t waitMs);
