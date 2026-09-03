/* Settings tile: WiFi network picker (tap to pin a network, AUTO to roam)
 * + pomodoro durations + screen-sleep timer. Persisted in NVS "cfg". */
#include "settings.h"
#include "wifimgr.h"
#include "pomodoro.h"
#include "ui.h"
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
static void net_btn_cb(lv_event_t *e);

static void mk_net_btn(int idx) { /* idx -1 = AUTO */
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
}

static void refresh_net_list() {
  if (!netList) return;
  String cur = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "";

  /* keep one button per known network (the list is loaded after this tile
   * is built, and can grow when the portal saves a new network) */
  int want = wifimgr_known_count();
  while ((int)lv_obj_get_child_cnt(netList) - 1 < want)
    mk_net_btn((int)lv_obj_get_child_cnt(netList) - 1);
  while ((int)lv_obj_get_child_cnt(netList) - 1 > want)
    lv_obj_del(lv_obj_get_child(netList, lv_obj_get_child_cnt(netList) - 1));
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

/* ---- per-network action dialog: Conectar / Editar / Olvidar ---- */
static int actionIdx = -1;
static char actionSsid[33] = "";

static void open_edit_dialog();

static void msgbox_cb(lv_event_t *e) {
  lv_obj_t *mb = lv_event_get_current_target(e);
  const char *txt = lv_msgbox_get_active_btn_text(mb);
  lv_msgbox_close(mb);
  if (!txt) return;
  if (strcmp(txt, "Conectar") == 0) wifimgr_pin(actionIdx);
  else if (strcmp(txt, "Olvidar") == 0) wifimgr_forget(actionSsid);
  else if (strcmp(txt, "Editar") == 0) open_edit_dialog();
  refresh_net_list();
}

static void net_btn_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
  if (idx < 0) { /* AUTO acts immediately */
    wifimgr_pin(-1);
    refresh_net_list();
    return;
  }
  actionIdx = idx;
  strlcpy(actionSsid, wifimgr_known_ssid(idx), sizeof(actionSsid));
  bool saved = wifimgr_is_saved(actionSsid);
  static const char *BTNS_SAVED[] = {"Conectar", "Editar", "Olvidar", ""};
  static const char *BTNS_FIXED[] = {"Conectar", "Editar", ""};
  lv_obj_t *mb = lv_msgbox_create(nullptr, actionSsid,
      saved ? " " : "red de config.h: no se puede olvidar",
      saved ? BTNS_SAVED : BTNS_FIXED, true);
  lv_obj_set_style_bg_color(mb, C_CARD, 0);
  lv_obj_set_style_text_color(mb, C_TEXT, 0);
  lv_obj_t *btns = lv_msgbox_get_btns(mb);
  lv_obj_set_style_bg_color(btns, C_TRACK, LV_PART_ITEMS);
  lv_obj_set_style_text_color(btns, C_TEXT, LV_PART_ITEMS);
  lv_obj_add_event_cb(mb, msgbox_cb, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_center(mb);
}

/* ---- password editor with on-screen keyboard ---- */
static lv_obj_t *editModal = nullptr;

static void close_edit() {
  if (editModal) { lv_obj_del(editModal); editModal = nullptr; }
}

static void kb_event_cb(lv_event_t *e) {
  lv_obj_t *kb = lv_event_get_target(e);
  if (lv_event_get_code(e) == LV_EVENT_READY) {
    lv_obj_t *ta = (lv_obj_t *)lv_obj_get_user_data(kb);
    const char *pass = lv_textarea_get_text(ta);
    if (pass[0]) {
      wifimgr_update_password(actionSsid, pass);
      if (wifimgr_pinned() == actionIdx) wifimgr_pin(actionIdx); /* reconnect with the new key */
    }
    close_edit();
    refresh_net_list();
  } else if (lv_event_get_code(e) == LV_EVENT_CANCEL) {
    close_edit();
  }
}

static void open_edit_dialog() {
  close_edit();
  editModal = lv_obj_create(lv_layer_top());
  lv_obj_set_size(editModal, 480, 320);
  lv_obj_set_pos(editModal, 0, 0);
  lv_obj_set_style_bg_color(editModal, C_BG, 0);
  lv_obj_set_style_border_width(editModal, 0, 0);
  lv_obj_set_style_radius(editModal, 0, 0);
  lv_obj_clear_flag(editModal, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *t = lv_label_create(editModal);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(t, C_MUTED, 0);
  lv_label_set_text_fmt(t, "nueva clave de %s   (%s guarda, %s cancela)",
                        actionSsid, LV_SYMBOL_OK, LV_SYMBOL_KEYBOARD);
  lv_obj_align(t, LV_ALIGN_TOP_LEFT, 10, 6);

  lv_obj_t *ta = lv_textarea_create(editModal);
  lv_textarea_set_one_line(ta, true);
  lv_textarea_set_placeholder_text(ta, "contrasena");
  lv_obj_set_size(ta, 440, 40);
  lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 28);
  lv_obj_set_style_bg_color(ta, C_CARD, 0);
  lv_obj_set_style_text_color(ta, C_TEXT, 0);

  lv_obj_t *kb = lv_keyboard_create(editModal);
  lv_obj_set_size(kb, 470, 190);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(kb, C_BG, 0);
  lv_obj_set_style_bg_color(kb, C_CARD, LV_PART_ITEMS);
  lv_obj_set_style_text_color(kb, C_TEXT, LV_PART_ITEMS);
  lv_keyboard_set_textarea(kb, ta);
  lv_obj_set_user_data(kb, ta);
  lv_obj_add_event_cb(kb, kb_event_cb, LV_EVENT_ALL, nullptr);
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
  lv_obj_set_size(netList, 220, 206);
  lv_obj_align(netList, LV_ALIGN_TOP_LEFT, 10, 54);
  lv_obj_set_style_bg_opa(netList, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(netList, 0, 0);
  lv_obj_set_style_pad_all(netList, 2, 0);
  lv_obj_set_style_pad_row(netList, 6, 0);
  lv_obj_set_flex_flow(netList, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(netList, LV_DIR_VER);

  mk_net_btn(-1); /* AUTO; the network buttons are synced by refresh_net_list */

  /* add a NEW (unknown) network: opens the config portal and jumps to the
   * buddy tile, where the AP name/password/URL instructions appear */
  lv_obj_t *addBtn = lv_btn_create(tile);
  lv_obj_set_size(addBtn, 204, 40);
  lv_obj_align(addBtn, LV_ALIGN_TOP_LEFT, 12, 266);
  lv_obj_set_style_bg_color(addBtn, C_ORANGE, 0);
  lv_obj_set_style_radius(addBtn, 9, 0);
  lv_obj_add_event_cb(addBtn, [](lv_event_t *) {
    wifimgr_request_portal();
    ui_show_buddy_tile();
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *addLbl = lv_label_create(addBtn);
  lv_obj_set_style_text_font(addLbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(addLbl, lv_color_hex(0x201A16), 0);
  lv_label_set_text(addLbl, LV_SYMBOL_PLUS " red nueva (portal)");
  lv_obj_center(addLbl);

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
