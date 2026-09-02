/* Settings tile: WiFi network picker (tap to pin a network, AUTO to roam)
 * + pomodoro durations + screen-sleep timer. Persisted in NVS "cfg". */
#include "settings.h"
#include "wifimgr.h"
#include "pomodoro.h"
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>

#define C_BG     lv_color_hex(0x1F1E1B)
#define C_CARD   lv_color_hex(0x2B2A27)
#define C_ORANGE lv_color_hex(0xD97757)
#define C_TEXT   lv_color_hex(0xECEAE5)
#define C_MUTED  lv_color_hex(0x9B9892)
#define C_TRACK  lv_color_hex(0x413F3A)

static Preferences prefs;

/* option tables: dropdown index -> minutes (0 = never for screen sleep) */
static const uint16_t FOCUS_OPT[] = {25, 30, 45, 50};
static const uint16_t BREAK_OPT[] = {5, 10, 15};
static const uint16_t LONG_OPT[] = {15, 20, 30};
static const uint16_t SLEEP_OPT[] = {15, 30, 60, 0};

static uint8_t focusIdx = 0, breakIdx = 0, longIdx = 0, sleepIdx = 1; /* defaults 25/5/15/30 */

static lv_obj_t *netList = nullptr, *curNetLbl = nullptr;

uint32_t settings_focus_ms() { return FOCUS_OPT[focusIdx] * 60000UL; }
uint32_t settings_break_ms() { return BREAK_OPT[breakIdx] * 60000UL; }
uint32_t settings_longbreak_ms() { return LONG_OPT[longIdx] * 60000UL; }
uint32_t settings_screen_sleep_ms() { return SLEEP_OPT[sleepIdx] * 60000UL; }

void settings_init() {
  prefs.begin("cfg", false);
  focusIdx = min((uint8_t)prefs.getUChar("f", 0), (uint8_t)3);
  breakIdx = min((uint8_t)prefs.getUChar("b", 0), (uint8_t)2);
  longIdx = min((uint8_t)prefs.getUChar("lb", 0), (uint8_t)2);
  sleepIdx = min((uint8_t)prefs.getUChar("ss", 1), (uint8_t)3);
}

static void persist_cfg() {
  prefs.putUChar("f", focusIdx);
  prefs.putUChar("b", breakIdx);
  prefs.putUChar("lb", longIdx);
  prefs.putUChar("ss", sleepIdx);
}

/* ---------------------------------------------------------------- wifi list */
static void refresh_net_list() {
  if (!netList) return;
  String cur = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "";
  if (cur.length())
    lv_label_set_text_fmt(curNetLbl, "conectado: %s (%d dBm)", cur.c_str(), WiFi.RSSI());
  else
    lv_label_set_text(curNetLbl, "sin conexion");

  int pinned = wifimgr_pinned();
  uint32_t nBtns = lv_obj_get_child_cnt(netList);
  for (uint32_t i = 0; i < nBtns; i++) {
    lv_obj_t *btn = lv_obj_get_child(netList, i);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn); /* -1 = AUTO */
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    bool isPin = idx == pinned;
    bool isCur = idx >= 0 && cur.length() && cur == wifimgr_known_ssid(idx);
    lv_obj_set_style_border_width(btn, isCur ? 2 : 0, 0);
    lv_obj_set_style_bg_color(btn, isPin ? C_ORANGE : C_CARD, 0);
    lv_obj_set_style_text_color(lbl, isPin ? lv_color_hex(0x201A16) : C_TEXT, 0);
    if (idx == -1) {
      lv_label_set_text(lbl, pinned < 0 ? LV_SYMBOL_OK " AUTO (mejor senal)" : "AUTO (mejor senal)");
    } else {
      lv_label_set_text_fmt(lbl, "%s%s", isCur ? LV_SYMBOL_WIFI " " : "", wifimgr_known_ssid(idx));
    }
  }
}

static void net_btn_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
  wifimgr_pin(idx); /* -1 = auto */
  refresh_net_list();
}

/* ---------------------------------------------------------------- dropdowns */
static void style_dd(lv_obj_t *dd) {
  lv_obj_set_style_bg_color(dd, C_CARD, 0);
  lv_obj_set_style_text_color(dd, C_TEXT, 0);
  lv_obj_set_style_border_color(dd, C_TRACK, 0);
  lv_obj_set_style_border_width(dd, 1, 0);
  lv_obj_t *list = lv_dropdown_get_list(dd);
  lv_obj_set_style_bg_color(list, C_CARD, 0);
  lv_obj_set_style_text_color(list, C_TEXT, 0);
}

static void dd_cb(lv_event_t *e) {
  lv_obj_t *dd = lv_event_get_target(e);
  uint8_t sel = lv_dropdown_get_selected(dd);
  uint8_t *target = (uint8_t *)lv_obj_get_user_data(dd);
  *target = sel;
  persist_cfg();
  pomodoro_durations_changed();
}

static lv_obj_t *mk_setting_row(lv_obj_t *parent, const char *title, const char *opts,
                                uint8_t *idxVar, int y) {
  lv_obj_t *t = lv_label_create(parent);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(t, C_MUTED, 0);
  lv_label_set_text(t, title);
  lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, y + 8);

  lv_obj_t *dd = lv_dropdown_create(parent);
  lv_dropdown_set_options_static(dd, opts);
  lv_dropdown_set_selected(dd, *idxVar);
  lv_obj_set_width(dd, 105);
  lv_obj_align(dd, LV_ALIGN_TOP_RIGHT, 0, y);
  lv_obj_set_user_data(dd, idxVar);
  style_dd(dd);
  lv_obj_add_event_cb(dd, dd_cb, LV_EVENT_VALUE_CHANGED, nullptr);
  return dd;
}

/* ---------------------------------------------------------------- build */
void settings_create(lv_obj_t *tile) {
  lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(tile);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(title, C_MUTED, 0);
  lv_label_set_text(title, "A J U S T E S");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 14, 10);

  /* ---- left: wifi ---- */
  curNetLbl = lv_label_create(tile);
  lv_obj_set_style_text_font(curNetLbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(curNetLbl, C_TEXT, 0);
  lv_label_set_text(curNetLbl, "...");
  lv_obj_set_width(curNetLbl, 215);
  lv_label_set_long_mode(curNetLbl, LV_LABEL_LONG_DOT);
  lv_obj_align(curNetLbl, LV_ALIGN_TOP_LEFT, 14, 32);

  netList = lv_obj_create(tile);
  lv_obj_set_size(netList, 220, 250);
  lv_obj_align(netList, LV_ALIGN_TOP_LEFT, 10, 54);
  lv_obj_set_style_bg_opa(netList, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(netList, 0, 0);
  lv_obj_set_style_pad_all(netList, 2, 0);
  lv_obj_set_style_pad_row(netList, 6, 0);
  lv_obj_set_flex_flow(netList, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(netList, LV_DIR_VER);

  auto mk_net_btn = [](int idx) {
    lv_obj_t *btn = lv_btn_create(netList);
    lv_obj_set_size(btn, 204, 40);
    lv_obj_set_style_bg_color(btn, C_CARD, 0);
    lv_obj_set_style_radius(btn, 9, 0);
    lv_obj_set_style_border_color(btn, C_ORANGE, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_user_data(btn, (void *)(intptr_t)idx);
    lv_obj_add_event_cb(btn, net_btn_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = lv_label_create(btn);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(l, C_TEXT, 0);
    lv_obj_set_width(l, 188);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_label_set_text(l, "");
    lv_obj_center(l);
  };
  mk_net_btn(-1); /* AUTO */
  for (int i = 0; i < wifimgr_known_count(); i++) mk_net_btn(i);

  /* ---- right: settings ---- */
  lv_obj_t *panel = lv_obj_create(tile);
  lv_obj_set_size(panel, 226, 258);
  lv_obj_align(panel, LV_ALIGN_TOP_RIGHT, -10, 40);
  lv_obj_set_style_radius(panel, 12, 0);
  lv_obj_set_style_border_width(panel, 0, 0);
  lv_obj_set_style_bg_color(panel, C_CARD, 0);
  lv_obj_set_style_pad_all(panel, 12, 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

  mk_setting_row(panel, "foco (min)", "25\n30\n45\n50", &focusIdx, 0);
  mk_setting_row(panel, "descanso (min)", "5\n10\n15", &breakIdx, 60);
  mk_setting_row(panel, "descanso largo (min)", "15\n20\n30", &longIdx, 120);
  mk_setting_row(panel, "apagar pantalla", "15 min\n30 min\n60 min\nnunca", &sleepIdx, 180);

  lv_timer_create([](lv_timer_t *) { refresh_net_list(); }, 2000, nullptr);
  refresh_net_list();
}
