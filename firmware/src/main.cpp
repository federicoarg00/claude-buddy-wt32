/* Claude Buddy — WT32-SC01 firmware entry point.
 * LVGL runs on core 1 (Arduino loop); WiFi + HTTP polling on core 0. */
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <lvgl.h>
#include "display.h"
#include "ui.h"
#include "pomodoro.h"
#include "config.h"

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
static bool fetch_status(BuddyData &out) {
  HTTPClient http;
  http.setTimeout(6000);
  http.setConnectTimeout(4000);
  if (!http.begin(COMPANION_URL)) return false;
  int code = http.GET();
  Serial.printf("[net] GET %s -> %d\n", COMPANION_URL, code);
  if (code != 200) { http.end(); return false; }
  String payload = http.getString();
  http.end();

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
  strlcpy(out.plan, doc["plan"] | "", sizeof(out.plan));
  JsonArray limits = doc["limits"].as<JsonArray>();
  for (JsonObject l : limits) {
    if (out.nLimits >= 4) break;
    LimitBar &b = out.limits[out.nLimits++];
    strlcpy(b.label, l["label"] | "?", sizeof(b.label));
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

static void net_task(void *arg) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.printf("[net] wifi status=%d\n", WiFi.status());
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      gConn = CONN_NO_WIFI;
      xSemaphoreGive(dataMutex);
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }
    static bool ipLogged = false;
    if (!ipLogged) {
      ipLogged = true;
      Serial.printf("[net] wifi OK ip=%s gw=%s mask=%s rssi=%d\n",
                    WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(),
                    WiFi.subnetMask().toString().c_str(), WiFi.RSSI());
    }

    BuddyData fresh;
    bool ok = fetch_status(fresh);
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    if (ok) {
      gData = fresh;
      gConn = CONN_OK;
    } else {
      gConn = CONN_NO_COMPANION;
    }
    xSemaphoreGive(dataMutex);
    vTaskDelay(pdMS_TO_TICKS(ok ? POLL_INTERVAL_MS : 5000));
  }
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
  lv_indev_drv_register(&indev_drv);

  ui_init();

  dataMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(net_task, "net", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
  static uint32_t lastUiPush = 0;
  lv_timer_handler();
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
