/* WiFi manager with captive config portal.
 *
 * Known networks = WIFI_SSID/WIFI_PASS + WIFI_EXTRA_NETWORKS (config.h) +
 * up to 8 networks saved from the portal (NVS namespace "wifinets").
 * The portal runs an AP + DNS catch-all so phones pop the page automatically. */
#include "wifimgr.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "config.h"

#ifndef PORTAL_AP_SSID
#define PORTAL_AP_SSID "ClaudeBuddy-Setup"
#endif
#ifndef PORTAL_AP_PASS
#define PORTAL_AP_PASS "claudito123"
#endif
#ifndef WIFI_EXTRA_NETWORKS
#define WIFI_EXTRA_NETWORKS
#endif

static const int MAX_SAVED = 8;
static const uint32_t PORTAL_TIMEOUT_MS = 5 * 60 * 1000;

static Preferences prefs;
static String savedSsid[MAX_SAVED], savedPass[MAX_SAVED];
static int nSaved = 0;

static WebServer portal(80);
static DNSServer dns;
static const IPAddress AP_IP(192, 168, 4, 1);
static volatile bool portalUp = false;
static volatile bool portalReq = false;
static volatile bool credsSaved = false;
static uint32_t portalStartedAt = 0;
static uint32_t credsSavedAt = 0;

/* last scan results (filled before the AP starts / on rescan) */
struct ScanHit { String ssid; int rssi; bool open; };
static ScanHit scanHits[16];
static int nScan = 0;

const char *wifimgr_ap_ssid() { return PORTAL_AP_SSID; }
const char *wifimgr_ap_pass() { return PORTAL_AP_PASS; }

/* ---------------------------------------------------------------- NVS */
static void load_saved() {
  nSaved = prefs.getInt("n", 0);
  if (nSaved > MAX_SAVED) nSaved = MAX_SAVED;
  char key[4];
  for (int i = 0; i < nSaved; i++) {
    snprintf(key, sizeof(key), "s%d", i);
    savedSsid[i] = prefs.getString(key, "");
    snprintf(key, sizeof(key), "p%d", i);
    savedPass[i] = prefs.getString(key, "");
  }
}

static void persist_saved() {
  prefs.putInt("n", nSaved);
  char key[4];
  for (int i = 0; i < nSaved; i++) {
    snprintf(key, sizeof(key), "s%d", i);
    prefs.putString(key, savedSsid[i]);
    snprintf(key, sizeof(key), "p%d", i);
    prefs.putString(key, savedPass[i]);
  }
}

static void save_network(const String &ssid, const String &pass) {
  for (int i = 0; i < nSaved; i++) {
    if (savedSsid[i] == ssid) { savedPass[i] = pass; persist_saved(); return; }
  }
  if (nSaved == MAX_SAVED) { /* drop the oldest */
    for (int i = 1; i < MAX_SAVED; i++) { savedSsid[i - 1] = savedSsid[i]; savedPass[i - 1] = savedPass[i]; }
    nSaved--;
  }
  savedSsid[nSaved] = ssid;
  savedPass[nSaved] = pass;
  nSaved++;
  persist_saved();
}

/* ---- known networks (compile-time + saved) + manual pinning ---- */
static const int MAX_KNOWN = 12;
static String knownS[MAX_KNOWN], knownP[MAX_KNOWN];
static int nKnown = 0;
static volatile int pinnedIdx = -1;
static volatile bool reconnectReq = false;

static void known_add(const char *s, const char *p) {
  if (!s || !s[0]) return;
  for (int i = 0; i < nKnown; i++) if (knownS[i] == s) return;
  if (nKnown >= MAX_KNOWN) return;
  knownS[nKnown] = s;
  knownP[nKnown] = p ? p : "";
  nKnown++;
}

static void rebuild_known() {
  String pinnedSsid = (pinnedIdx >= 0 && pinnedIdx < nKnown) ? knownS[pinnedIdx] : "";
  nKnown = 0;
  known_add(WIFI_SSID, WIFI_PASS);
#define X(ssid, pass) known_add(ssid, pass);
  WIFI_EXTRA_NETWORKS
#undef X
  for (int i = 0; i < nSaved; i++) known_add(savedSsid[i].c_str(), savedPass[i].c_str());
  /* keep the pin pointing at the same ssid if it still exists */
  pinnedIdx = -1;
  for (int i = 0; i < nKnown; i++) if (pinnedSsid.length() && knownS[i] == pinnedSsid) pinnedIdx = i;
}

int wifimgr_known_count() { return nKnown; }
const char *wifimgr_known_ssid(int i) { return (i >= 0 && i < nKnown) ? knownS[i].c_str() : ""; }
int wifimgr_pinned() { return pinnedIdx; }

void wifimgr_pin(int idx) {
  pinnedIdx = (idx >= 0 && idx < nKnown) ? idx : -1;
  reconnectReq = true;
  Serial.printf("[wifi] pin -> %s\n", pinnedIdx >= 0 ? knownS[pinnedIdx].c_str() : "AUTO");
}

bool wifimgr_take_reconnect() {
  bool r = reconnectReq;
  reconnectReq = false;
  return r;
}

bool wifimgr_connect_pinned(uint32_t waitMs) {
  if (pinnedIdx < 0 || pinnedIdx >= nKnown) return false;
  WiFi.begin(knownS[pinnedIdx].c_str(), knownP[pinnedIdx].c_str());
  uint32_t t0 = millis();
  while (millis() - t0 < waitMs) {
    if (WiFi.status() == WL_CONNECTED) return true;
    delay(100);
  }
  return false;
}

void wifimgr_init() {
  prefs.begin("wifinets", false);
  load_saved();
  rebuild_known();
  Serial.printf("[wifi] %d saved, %d known network(s)\n", nSaved, nKnown);
}

/* ---------------------------------------------------------------- connect */
bool wifimgr_connect(uint32_t perTryMs) {
  /* Best-RSSI pick among all known networks. Which network can reach the
   * companion doesn't matter here: the firmware tries every COMPANION_URLS
   * entry and falls back to direct mode anyway. */
  WiFiMulti m;
  m.addAP(WIFI_SSID, WIFI_PASS);
#define X(ssid, pass) m.addAP(ssid, pass);
  WIFI_EXTRA_NETWORKS
#undef X
  for (int i = 0; i < nSaved; i++) m.addAP(savedSsid[i].c_str(), savedPass[i].c_str());
  return m.run(perTryMs) == WL_CONNECTED;
}

/* ---------------------------------------------------------------- scan */
static void do_scan() {
  int n = WiFi.scanNetworks(/*async*/ false, /*hidden*/ false);
  nScan = 0;
  for (int i = 0; i < n && nScan < 16; i++) {
    String s = WiFi.SSID(i);
    if (!s.length() || s == PORTAL_AP_SSID) continue;
    bool dup = false;
    for (int j = 0; j < nScan; j++) if (scanHits[j].ssid == s) { dup = true; break; }
    if (dup) continue;
    scanHits[nScan].ssid = s;
    scanHits[nScan].rssi = WiFi.RSSI(i);
    scanHits[nScan].open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
    nScan++;
  }
  WiFi.scanDelete();
  Serial.printf("[wifi] scan: %d network(s)\n", nScan);
}

/* ---------------------------------------------------------------- pages */
static String esc(const String &s) {
  String o = s;
  o.replace("&", "&amp;"); o.replace("<", "&lt;"); o.replace(">", "&gt;"); o.replace("\"", "&quot;");
  return o;
}

static String html_page(const String &msg) {
  String h;
  h.reserve(6000);
  h += F("<!DOCTYPE html><html lang='es'><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>Claude Buddy</title><style>"
         "body{font-family:system-ui,sans-serif;background:#1F1E1B;color:#ECEAE5;margin:0;padding:20px}"
         "h1{color:#D97757;font-size:1.3em}h2{font-size:1em;color:#9B9892;margin-top:24px}"
         ".card{background:#2B2A27;border-radius:12px;padding:14px;margin:8px 0}"
         ".net{display:flex;justify-content:space-between;cursor:pointer;padding:10px 14px}"
         ".net:active{background:#3a3835}.rssi{color:#9B9892;font-size:.85em}"
         "input{width:100%;box-sizing:border-box;padding:12px;margin:6px 0;border-radius:8px;"
         "border:1px solid #413F3A;background:#1F1E1B;color:#ECEAE5;font-size:1em}"
         "button{width:100%;padding:12px;margin-top:8px;border:0;border-radius:8px;"
         "background:#D97757;color:#201A16;font-size:1em;font-weight:600}"
         ".del{width:auto;background:#413F3A;color:#ECEAE5;padding:6px 12px;font-size:.8em}"
         ".msg{color:#8AA672;font-weight:600}a{color:#D97757}"
         "</style></head><body><h1>&#9650; Claude Buddy &mdash; WiFi</h1>");
  if (msg.length()) { h += F("<p class='msg'>"); h += esc(msg); h += F("</p>"); }

  h += F("<h2>Redes encontradas <a href='/rescan'>(re-escanear)</a></h2>");
  if (!nScan) h += F("<div class='card'>ninguna &mdash; prob&aacute; re-escanear</div>");
  for (int i = 0; i < nScan; i++) {
    h += F("<div class='card net' onclick=\"document.getElementById('s').value=this.dataset.n;"
           "document.getElementById('p').focus()\" data-n=\"");
    h += esc(scanHits[i].ssid);
    h += F("\"><span>");
    h += esc(scanHits[i].ssid);
    if (scanHits[i].open) h += F(" (abierta)");
    h += F("</span><span class='rssi'>");
    h += String(scanHits[i].rssi);
    h += F(" dBm</span></div>");
  }

  h += F("<h2>Agregar red</h2><div class='card'><form method='POST' action='/save'>"
         "<input id='s' name='ssid' placeholder='Nombre de la red (SSID)' required>"
         "<input id='p' name='pass' type='password' placeholder='Contrase&ntilde;a (vac&iacute;o si es abierta)'>"
         "<button type='submit'>Guardar y conectar</button></form></div>");

  if (nSaved) {
    h += F("<h2>Redes guardadas</h2>");
    for (int i = 0; i < nSaved; i++) {
      h += F("<div class='card net'><span>");
      h += esc(savedSsid[i]);
      h += F("</span><form method='POST' action='/del' style='margin:0'>"
             "<input type='hidden' name='i' value='");
      h += String(i);
      h += F("'><button class='del'>borrar</button></form></div>");
    }
  }
  h += F("<p class='rssi'>Las redes de f&aacute;brica (config.h) no se listan ac&aacute; pero siguen activas.</p>"
         "</body></html>");
  return h;
}

/* ---------------------------------------------------------------- portal */
static void handle_root() { portal.send(200, "text/html", html_page("")); }

static void handle_save() {
  String ssid = portal.arg("ssid");
  String pass = portal.arg("pass");
  ssid.trim();
  if (!ssid.length()) { portal.send(200, "text/html", html_page("Falta el nombre de la red")); return; }
  save_network(ssid, pass);
  rebuild_known();
  credsSaved = true;
  credsSavedAt = millis();
  Serial.printf("[wifi] portal saved network '%s'\n", ssid.c_str());
  portal.send(200, "text/html", html_page("Guardada! El buddy va a intentar conectarse en unos segundos..."));
}

static void handle_del() {
  int i = portal.arg("i").toInt();
  if (i >= 0 && i < nSaved) {
    Serial.printf("[wifi] portal deleted network '%s'\n", savedSsid[i].c_str());
    for (int j = i + 1; j < nSaved; j++) { savedSsid[j - 1] = savedSsid[j]; savedPass[j - 1] = savedPass[j]; }
    nSaved--;
    persist_saved();
    rebuild_known();
  }
  portal.send(200, "text/html", html_page("Red borrada"));
}

static void handle_rescan() {
  do_scan();
  portal.sendHeader("Location", "/", true);
  portal.send(302, "text/plain", "");
}

static void handle_captive() { /* any unknown URL -> the portal page */
  portal.sendHeader("Location", String("http://") + AP_IP.toString() + "/", true);
  portal.send(302, "text/plain", "");
}

void wifimgr_request_portal() { portalReq = true; }
bool wifimgr_portal_requested() { return portalReq; }
bool wifimgr_portal_active() { return portalUp; }

bool wifimgr_portal_should_close() {
  if (credsSaved && millis() - credsSavedAt > 3000) return true; /* let the page deliver */
  if (millis() - portalStartedAt > PORTAL_TIMEOUT_MS) return true;
  return false;
}

void wifimgr_start_portal(bool keepSta) {
  portalReq = false;
  credsSaved = false;
  if (!keepSta) WiFi.disconnect();
  WiFi.mode(keepSta ? WIFI_AP_STA : WIFI_AP_STA); /* AP_STA either way: needed to scan */
  do_scan();
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(PORTAL_AP_SSID, PORTAL_AP_PASS);
  dns.start(53, "*", AP_IP);
  portal.on("/", handle_root);
  portal.on("/save", HTTP_POST, handle_save);
  portal.on("/del", HTTP_POST, handle_del);
  portal.on("/rescan", handle_rescan);
  portal.onNotFound(handle_captive);
  portal.begin();
  portalUp = true;
  portalStartedAt = millis();
  Serial.printf("[wifi] portal up: AP '%s' pass '%s' -> http://%s/\n",
                PORTAL_AP_SSID, PORTAL_AP_PASS, AP_IP.toString().c_str());
}

void wifimgr_stop_portal() {
  portal.stop();
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  portalUp = false;
  Serial.println("[wifi] portal closed");
}

void wifimgr_handle() {
  if (!portalUp) return;
  dns.processNextRequest();
  portal.handleClient();
}
