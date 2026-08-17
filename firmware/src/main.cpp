/* Claude Buddy — WT32-SC01 firmware entry point.
 * LVGL runs on core 1 (Arduino loop); WiFi + HTTP polling on core 0. */
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <lvgl.h>
#include <time.h>
#include "display.h"
#include "ui.h"
#include "pomodoro.h"
#include "directapi.h"
#include "wifimgr.h"
#include "config.h"

/* Defaults for options older config.h files don't define. */
#ifndef TZ_OFFSET_SECONDS
#define TZ_OFFSET_SECONDS (-3 * 3600) /* Argentina */
#endif
/* Companion may live at different addresses depending on which network the
 * buddy joined; every entry is tried in order each poll cycle. */
#ifndef COMPANION_URLS
#define COMPANION_URLS X(COMPANION_URL)
#endif

static LGFX lcd;

/* LVGL draw buffer: 480 x 40 lines, allocated from DMA-capable heap at boot
 * (static buffers this size overflow the ESP32's dram0 segment at link time) */
static const size_t DRAW_BUF_PIXELS = 480 * 40;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf1;

/* shared state between net task and UI loop */
static SemaphoreHandle_t dataMutex;
static BuddyData gData;
static ConnState gConn = CONN_BOOTING;

/* Always-on LAN server: receives provisioning tokens (POST /token) and
 * pushed status from the companion (POST /status) — the push path lets a
 * companion on another subnet feed the buddy even when the buddy can't
 * reach it. Shares port 80 with the WiFi portal but they never run at the
 * same time (net_task stops this one first). */
static WebServer provServer(80);
static volatile bool provServerUp = false;
static volatile bool gPushSeen = false;
static volatile uint32_t gLastPushMs = 0;

static void flush_cb(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  int32_t w = area->x2 - area->x1 + 1;
  int32_t h = area->y2 - area->y1 + 1;
  lcd.startWrite();
  lcd.setAddrWindow(area->x1, area->y1, w, h);
  lcd.writePixels((lgfx::rgb565_t *)&color_p->full, w * h);
  lcd.endWrite();
  lv_disp_flush_ready(disp);
}

static void touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  uint16_t x, y;
  if (lcd.getTouch(&x, &y)) {
    data->state = LV_INDEV_STATE_PR;
    data->point.x = x;
    data->point.y = y;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

/* ---------------------------------------------------------------- net */
static bool parse_status_payload(const String &payload, BuddyData &out) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[net] json err: %s (len=%u)\n", err.c_str(), payload.length());
    return false;
  }
  Serial.printf("[net] parsed ok=%d limits=%u active=%d\n",
                (int)(doc["ok"] | false), doc["limits"].size(), (int)(doc["activity"]["active"] | false));

  out = BuddyData{};
  out.companionOk = doc["ok"] | false;
  out.source = SRC_COMPANION;
  strlcpy(out.plan, doc["plan"] | "", sizeof(out.plan));
  JsonArray limits = doc["limits"].as<JsonArray>();
  for (JsonObject l : limits) {
    if (out.nLimits >= 4) break;
    LimitBar &b = out.limits[out.nLimits++];
    strlcpy(b.label, l["label"] | "?", sizeof(b.label));
    strlcpy(b.kind, l["kind"] | "", sizeof(b.kind));
    b.pct = l["pct"].isNull() ? -1 : (int)l["pct"];
    strlcpy(b.severity, l["severity"] | "normal", sizeof(b.severity));
    b.resetsInSec = l["resetsInSec"].isNull() ? -1 : (long)l["resetsInSec"];
    b.isActive = l["isActive"] | false;
  }
  JsonObject act = doc["activity"];
  out.active = act["active"] | false;
  out.activeSessions = act["activeSessions"] | 0;
  out.lastActivityAgoSec = act["lastActivityAgoSec"].isNull() ? -1 : (long)act["lastActivityAgoSec"];
  out.tokensToday = act["tokensToday"] | 0L;
  strlcpy(out.date, doc["dateLocal"] | "", sizeof(out.date));
  return true;
}

static bool fetch_status_url(const char *url, BuddyData &out) {
  HTTPClient http;
  http.setTimeout(4000);
  http.setConnectTimeout(2000); /* fail fast when away from home */
  if (!http.begin(url)) return false;
  int code = http.GET();
  Serial.printf("[net] GET %s -> %d\n", url, code);
  if (code != 200) { http.end(); return false; }
  String payload = http.getString();
  http.end();
  return parse_status_payload(payload, out);
}

static bool fetch_status(BuddyData &out) {
#define X(url) if (fetch_status_url(url, out)) return true;
  COMPANION_URLS
#undef X
  return false;
}

static void prov_server_start();

static void set_conn(ConnState c) {
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  gConn = c;
  xSemaphoreGive(dataMutex);
}

static void net_task(void *arg) {
  WiFi.mode(WIFI_STA);

  bool timeStarted = false;
  int failStreak = 0;
  for (;;) {
    /* --- WiFi config portal lifecycle --- */
    if (wifimgr_portal_active()) {
      set_conn(CONN_PORTAL);
      if (wifimgr_portal_should_close()) {
        wifimgr_stop_portal();
        ui_set_hint_text(nullptr);
        failStreak = 0;
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if (wifimgr_portal_requested()) {
      if (provServerUp) { provServer.stop(); provServerUp = false; } /* free port 80 */
      wifimgr_start_portal(WiFi.status() == WL_CONNECTED);
      char hint[96];
      snprintf(hint, sizeof(hint), "red: %s\nclave: %s\nabri http://192.168.4.1",
               wifimgr_ap_ssid(), wifimgr_ap_pass());
      ui_set_hint_text(hint);
      continue;
    }

    if (WiFi.status() != WL_CONNECTED) {
      set_conn(CONN_NO_WIFI);
      if (!wifimgr_connect(8000)) {
        Serial.printf("[net] no known network found (streak %d)\n", ++failStreak);
        if (failStreak >= 2) { wifimgr_request_portal(); }
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;
      }
      failStreak = 0;
      timeStarted = false; /* re-sync NTP on a new network */
    }
    if (!timeStarted) {
      timeStarted = true;
      configTime(TZ_OFFSET_SECONDS, 0, "pool.ntp.org", "time.google.com");
      Serial.printf("[net] wifi OK ssid=%s ip=%s rssi=%d\n",
                    WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }
    /* the lwIP stack exists only once WiFi is up — start the LAN endpoints
     * here, never in setup() */
    if (!provServerUp) prov_server_start();

    /* companion pushes are fresher than anything we could pull */
    if (gPushSeen && millis() - gLastPushMs < 45000) {
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    /* companion first (rich data at home), direct API as fallback anywhere */
    BuddyData fresh;
    bool ok = fetch_status(fresh);
    if (!ok && directapi_has_token()) ok = directapi_fetch(fresh);

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    if (ok) {
      gData = fresh;
      gConn = CONN_OK;
    } else {
      gConn = CONN_NO_COMPANION;
    }
    xSemaphoreGive(dataMutex);

    uint32_t waitMs;
    if (!ok) waitMs = 5000;
    else if (fresh.source == SRC_DIRECT) waitMs = directapi_wait_ms();
    else waitMs = POLL_INTERVAL_MS;
    vTaskDelay(pdMS_TO_TICKS(waitMs));
  }
}

/* ------------------------------------------------------------ provisioning */
static void prov_server_start() {
  provServer.on("/token", HTTP_POST, []() {
    JsonDocument doc;
    if (deserializeJson(doc, provServer.arg("plain"))) {
      provServer.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
      return;
    }
    const char *at = doc["accessToken"] | "";
    const char *rt = doc["refreshToken"] | "";
    if (!at[0] || !rt[0]) {
      provServer.send(400, "application/json", "{\"ok\":false,\"error\":\"missing tokens\"}");
      return;
    }
    directapi_store_tokens(at, rt, doc["expiresAt"] | 0LL, doc["plan"] | "");
    provServer.send(200, "application/json", "{\"ok\":true}");
  });
  provServer.on("/status", HTTP_POST, []() {
    BuddyData fresh;
    if (!parse_status_payload(provServer.arg("plain"), fresh)) {
      provServer.send(400, "application/json", "{\"ok\":false}");
      return;
    }
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    gData = fresh;
    gConn = CONN_OK;
    xSemaphoreGive(dataMutex);
    gLastPushMs = millis();
    gPushSeen = true;
    provServer.send(200, "application/json", "{\"ok\":true}");
  });
  provServer.begin();
  provServerUp = true;
  Serial.println("[prov] LAN endpoints /token /status listening on :80");
}

/* ---------------------------------------------------------------- arduino */
void setup() {
  Serial.begin(115200);

  lcd.init();
  lcd.setRotation(1); /* landscape 480x320, USB on the right */
  lcd.setBrightness(BACKLIGHT_LEVEL);

  lv_init();
  buf1 = (lv_color_t *)heap_caps_malloc(DRAW_BUF_PIXELS * sizeof(lv_color_t), MALLOC_CAP_DMA);
  lv_disp_draw_buf_init(&draw_buf, buf1, nullptr, DRAW_BUF_PIXELS);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = 480;
  disp_drv.ver_res = 320;
  disp_drv.flush_cb = flush_cb;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touch_cb;
  indev_drv.long_press_time = 1000; /* deliberate hold to open the WiFi portal */
  lv_indev_drv_register(&indev_drv);

  ui_init();
  lv_mem_monitor_t mon;
  lv_mem_monitor(&mon);
  Serial.printf("[ui] lv_mem: %d%% used, frag %d%%, biggest free %u\n",
                mon.used_pct, mon.frag_pct, (unsigned)mon.free_biggest_size);
  directapi_init();
  wifimgr_init();
  ui_on_body_longpress([]() { wifimgr_request_portal(); });

  dataMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(net_task, "net", 10240, nullptr, 1, nullptr, 0);
}

void loop() {
  static uint32_t lastUiPush = 0;
  lv_timer_handler();
  wifimgr_handle();
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'p') wifimgr_request_portal();
    else if (c == 'd') pomodoro_debug_dump();
  }
  if (provServerUp) {
    provServer.handleClient();
    /* provisioning hint only while there is no OAuth token yet */
    static bool hintShown = false;
    if (!directapi_has_token() && WiFi.status() == WL_CONNECTED) {
      static uint32_t lastHint = 0;
      if (millis() - lastHint > 2000) {
        lastHint = millis();
        ui_set_provision_hint(WiFi.localIP().toString().c_str());
        hintShown = true;
      }
    } else if (hintShown) {
      hintShown = false;
      ui_set_provision_hint(nullptr);
    }
  }
  if (millis() - lastUiPush > 500) {
    lastUiPush = millis();
    BuddyData copy;
    ConnState conn;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    copy = gData;
    conn = gConn;
    xSemaphoreGive(dataMutex);
    ui_update(conn, copy);
    if (copy.date[0]) pomodoro_set_date(copy.date);
  }
  delay(5);
}
