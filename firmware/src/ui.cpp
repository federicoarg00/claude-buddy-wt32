/* Claude Buddy UI: Clawd (pixel-style mascot) + plan usage limit bars.
 * Screen is 480x320 landscape. */
#include "ui.h"
#include "pomodoro.h"
#include "histview.h"
#include <Arduino.h>

/* ---- palette (Claude-ish warm dark) ---- */
#define C_BG     lv_color_hex(0x1F1E1B)
#define C_CARD   lv_color_hex(0x2B2A27)
#define C_ORANGE lv_color_hex(0xD97757)
#define C_TEXT   lv_color_hex(0xECEAE5)
#define C_MUTED  lv_color_hex(0x9B9892)
#define C_GOOD   lv_color_hex(0xD97757)  /* normal bars stay brand orange */
#define C_WARN   lv_color_hex(0xE0A93E)
#define C_BAD    lv_color_hex(0xCC3F33)
#define C_EYE    lv_color_hex(0x2E2C28)
#define C_GRAYBODY lv_color_hex(0x8C8880)

enum BuddyState { ST_BOOT, ST_NO_WIFI, ST_NO_COMPANION, ST_SLEEP, ST_IDLE, ST_WORK, ST_CAPPED, ST_PORTAL };

static const int EYE_H = 36;

/* widgets */
static lv_obj_t *tv, *tilePomo, *tileHist;
static lv_obj_t *scr, *headerTitle, *planBadge, *planLbl, *connDot, *srcIcon;
static lv_obj_t *body, *eyeL, *eyeR, *mouth, *zzz, *sweat, *statusLbl, *provHint;
static lv_obj_t *footL, *footR;
static struct {
  lv_obj_t *group, *label, *pct, *bar, *sub;
} bars[3];

static BuddyState curState = ST_BOOT;
static lv_timer_t *blinkTimer = nullptr;

/* ---------------------------------------------------------------- helpers */
static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  return l;
}

static void anim_translate_y_cb(void *obj, int32_t v) {
  lv_obj_set_style_translate_y((lv_obj_t *)obj, v, 0);
}
static void anim_translate_x_cb(void *obj, int32_t v) {
  lv_obj_set_style_translate_x((lv_obj_t *)obj, v, 0);
}
static void anim_height_cb(void *obj, int32_t v) {
  lv_obj_set_height((lv_obj_t *)obj, v);
}
static void anim_opa_cb(void *obj, int32_t v) {
  lv_obj_set_style_opa((lv_obj_t *)obj, v, 0);
}

static void start_body_anim(int amplitude, int period) {
  lv_anim_del(body, anim_translate_y_cb);
  if (period <= 0) { lv_obj_set_style_translate_y(body, 0, 0); return; }
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, body);
  lv_anim_set_exec_cb(&a, anim_translate_y_cb);
  lv_anim_set_values(&a, 0, amplitude);
  lv_anim_set_time(&a, period);
  lv_anim_set_playback_time(&a, period);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_start(&a);
}

static void start_eye_dart(bool on) {
  lv_anim_del(eyeL, anim_translate_x_cb);
  lv_anim_del(eyeR, anim_translate_x_cb);
  if (!on) {
    lv_obj_set_style_translate_x(eyeL, 0, 0);
    lv_obj_set_style_translate_x(eyeR, 0, 0);
    return;
  }
  for (lv_obj_t *e : {eyeL, eyeR}) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, e);
    lv_anim_set_exec_cb(&a, anim_translate_x_cb);
    lv_anim_set_values(&a, -6, 6);
    lv_anim_set_time(&a, 700);
    lv_anim_set_playback_time(&a, 700);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
  }
}

static void set_eyes(int h) {
  lv_anim_del(eyeL, anim_height_cb);
  lv_anim_del(eyeR, anim_height_cb);
  lv_obj_set_height(eyeL, h);
  lv_obj_set_height(eyeR, h);
}

static void blink_once() {
  for (lv_obj_t *e : {eyeL, eyeR}) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, e);
    lv_anim_set_exec_cb(&a, anim_height_cb);
    lv_anim_set_values(&a, EYE_H, 5);
    lv_anim_set_time(&a, 90);
    lv_anim_set_playback_time(&a, 110);
    lv_anim_set_repeat_count(&a, 1);
    lv_anim_start(&a);
  }
}

static void blink_timer_cb(lv_timer_t *t) {
  if (curState == ST_IDLE || curState == ST_WORK || curState == ST_BOOT) blink_once();
  lv_timer_set_period(t, 2500 + (esp_random() % 3500));
}

static void start_zzz(bool on) {
  lv_anim_del(zzz, anim_opa_cb);
  if (!on) { lv_obj_add_flag(zzz, LV_OBJ_FLAG_HIDDEN); return; }
  lv_obj_clear_flag(zzz, LV_OBJ_FLAG_HIDDEN);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, zzz);
  lv_anim_set_exec_cb(&a, anim_opa_cb);
  lv_anim_set_values(&a, LV_OPA_20, LV_OPA_COVER);
  lv_anim_set_time(&a, 1200);
  lv_anim_set_playback_time(&a, 1200);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_start(&a);
}

/* ---------------------------------------------------------------- build */
void ui_init() {
  /* three swipeable tiles: buddy | pomodoro | 30-day calendar */
  tv = lv_tileview_create(lv_scr_act());
  lv_obj_set_style_bg_color(tv, C_BG, 0);
  lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);
  scr = lv_tileview_add_tile(tv, 0, 0, LV_DIR_RIGHT);
  tilePomo = lv_tileview_add_tile(tv, 1, 0, LV_DIR_HOR);
  tileHist = lv_tileview_add_tile(tv, 2, 0, LV_DIR_LEFT);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  /* header */
  headerTitle = mk_label(scr, &lv_font_montserrat_12, C_MUTED);
  lv_label_set_text(headerTitle, "C L A U D E   B U D D Y");
  lv_obj_align(headerTitle, LV_ALIGN_TOP_LEFT, 14, 10);

  connDot = lv_obj_create(scr);
  lv_obj_set_size(connDot, 10, 10);
  lv_obj_set_style_radius(connDot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(connDot, 0, 0);
  lv_obj_set_style_bg_color(connDot, C_MUTED, 0);
  lv_obj_align(connDot, LV_ALIGN_TOP_RIGHT, -14, 12);

  srcIcon = mk_label(scr, &lv_font_montserrat_12, C_MUTED);
  lv_label_set_text(srcIcon, "");
  lv_obj_align(srcIcon, LV_ALIGN_TOP_RIGHT, -92, 10);

  planBadge = lv_obj_create(scr);
  lv_obj_set_size(planBadge, 52, 20);
  lv_obj_set_style_radius(planBadge, 10, 0);
  lv_obj_set_style_border_width(planBadge, 0, 0);
  lv_obj_set_style_bg_color(planBadge, C_ORANGE, 0);
  lv_obj_align(planBadge, LV_ALIGN_TOP_RIGHT, -32, 7);
  lv_obj_clear_flag(planBadge, LV_OBJ_FLAG_SCROLLABLE);
  planLbl = mk_label(planBadge, &lv_font_montserrat_12, lv_color_hex(0x201A16));
  lv_label_set_text(planLbl, "...");
  lv_obj_center(planLbl);

  /* Clawd */
  body = lv_obj_create(scr);
  lv_obj_set_size(body, 150, 122);
  lv_obj_set_style_radius(body, 30, 0);
  lv_obj_set_style_border_width(body, 0, 0);
  lv_obj_set_style_bg_color(body, C_ORANGE, 0);
  lv_obj_align(body, LV_ALIGN_LEFT_MID, 28, -18);
  lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

  eyeL = lv_obj_create(body);
  eyeR = lv_obj_create(body);
  for (lv_obj_t *e : {eyeL, eyeR}) {
    lv_obj_set_size(e, 15, EYE_H);
    lv_obj_set_style_radius(e, 8, 0);
    lv_obj_set_style_border_width(e, 0, 0);
    lv_obj_set_style_bg_color(e, C_EYE, 0);
  }
  lv_obj_align(eyeL, LV_ALIGN_CENTER, -28, -8);
  lv_obj_align(eyeR, LV_ALIGN_CENTER, 28, -8);

  mouth = lv_obj_create(body);
  lv_obj_set_size(mouth, 22, 7);
  lv_obj_set_style_radius(mouth, 4, 0);
  lv_obj_set_style_border_width(mouth, 0, 0);
  lv_obj_set_style_bg_color(mouth, C_EYE, 0);
  lv_obj_align(mouth, LV_ALIGN_CENTER, 0, 34);

  zzz = mk_label(scr, &lv_font_montserrat_24, C_MUTED);
  lv_label_set_text(zzz, "z z z");
  lv_obj_align_to(zzz, body, LV_ALIGN_OUT_TOP_RIGHT, -10, 0);
  lv_obj_add_flag(zzz, LV_OBJ_FLAG_HIDDEN);

  sweat = lv_obj_create(scr);
  lv_obj_set_size(sweat, 12, 16);
  lv_obj_set_style_radius(sweat, 8, 0);
  lv_obj_set_style_border_width(sweat, 0, 0);
  lv_obj_set_style_bg_color(sweat, lv_color_hex(0x6FA8DC), 0);
  lv_obj_align_to(sweat, body, LV_ALIGN_TOP_RIGHT, -6, 6);
  lv_obj_add_flag(sweat, LV_OBJ_FLAG_HIDDEN);

  statusLbl = mk_label(scr, &lv_font_montserrat_16, C_TEXT);
  lv_label_set_text(statusLbl, "arrancando...");
  lv_obj_set_width(statusLbl, 190);
  lv_obj_set_style_text_align(statusLbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(body, LV_ALIGN_LEFT_MID, 28, -18);
  lv_obj_align(statusLbl, LV_ALIGN_LEFT_MID, 8, 78);

  provHint = mk_label(scr, &lv_font_montserrat_12, C_WARN);
  lv_obj_set_width(provHint, 190);
  lv_obj_set_style_text_align(provHint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(provHint, LV_ALIGN_LEFT_MID, 8, 104);
  lv_label_set_text(provHint, "");
  lv_obj_add_flag(provHint, LV_OBJ_FLAG_HIDDEN);

  /* limit bars, right side */
  for (int i = 0; i < 3; i++) {
    lv_obj_t *g = lv_obj_create(scr);
    lv_obj_set_size(g, 258, 74);
    lv_obj_set_style_radius(g, 12, 0);
    lv_obj_set_style_border_width(g, 0, 0);
    lv_obj_set_style_bg_color(g, C_CARD, 0);
    lv_obj_set_style_pad_all(g, 10, 0);
    lv_obj_align(g, LV_ALIGN_TOP_RIGHT, -10, 38 + i * 80);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g, LV_OBJ_FLAG_HIDDEN);

    bars[i].group = g;
    bars[i].label = mk_label(g, &lv_font_montserrat_16, C_TEXT);
    lv_obj_align(bars[i].label, LV_ALIGN_TOP_LEFT, 0, -2);
    bars[i].pct = mk_label(g, &lv_font_montserrat_16, C_TEXT);
    lv_obj_align(bars[i].pct, LV_ALIGN_TOP_RIGHT, 0, -2);

    bars[i].bar = lv_bar_create(g);
    lv_obj_set_size(bars[i].bar, 238, 10);
    lv_obj_align(bars[i].bar, LV_ALIGN_TOP_LEFT, 0, 24);
    lv_obj_set_style_bg_color(bars[i].bar, lv_color_hex(0x413F3A), LV_PART_MAIN);
    lv_obj_set_style_radius(bars[i].bar, 5, LV_PART_MAIN);
    lv_obj_set_style_radius(bars[i].bar, 5, LV_PART_INDICATOR);
    lv_bar_set_range(bars[i].bar, 0, 100);

    bars[i].sub = mk_label(g, &lv_font_montserrat_12, C_MUTED);
    lv_obj_align(bars[i].sub, LV_ALIGN_BOTTOM_LEFT, 0, 2);
    lv_label_set_text(bars[i].sub, "");
  }

  /* footer */
  footL = mk_label(scr, &lv_font_montserrat_12, C_MUTED);
  lv_obj_align(footL, LV_ALIGN_BOTTOM_LEFT, 14, -8);
  lv_label_set_text(footL, "");
  footR = mk_label(scr, &lv_font_montserrat_12, C_MUTED);
  lv_obj_align(footR, LV_ALIGN_BOTTOM_RIGHT, -14, -8);
  lv_label_set_text(footR, "");

  blinkTimer = lv_timer_create(blink_timer_cb, 3000, nullptr);
  start_body_anim(4, 2600); /* gentle breathing from boot */

  pomodoro_create(tilePomo);
  histview_create(tileHist);
  pomodoro_set_on_change(histview_refresh);
}

void ui_show_pomodoro_tile() {
  lv_obj_set_tile_id(tv, 1, 0, LV_ANIM_ON);
}

void ui_set_provision_hint(const char *ip) {
  if (ip && ip[0]) {
    lv_label_set_text_fmt(provHint, "provisioname: node provision.js\nIP: %s", ip);
    lv_obj_clear_flag(provHint, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(provHint, LV_OBJ_FLAG_HIDDEN);
  }
}

void ui_set_hint_text(const char *text) {
  if (text && text[0]) {
    lv_label_set_text(provHint, text);
    lv_obj_clear_flag(provHint, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(provHint, LV_OBJ_FLAG_HIDDEN);
  }
}

static void (*bodyLpCb)(void) = nullptr;
static void body_lp_event(lv_event_t *) {
  if (bodyLpCb) bodyLpCb();
}
void ui_on_body_longpress(void (*cb)(void)) {
  bodyLpCb = cb;
  lv_obj_add_event_cb(body, body_lp_event, LV_EVENT_LONG_PRESSED, nullptr);
}

/* ---------------------------------------------------------------- state */
static void apply_state(BuddyState s) {
  if (s == curState) return;
  curState = s;

  lv_obj_set_style_bg_color(body, C_ORANGE, 0);
  lv_obj_set_style_opa(body, LV_OPA_COVER, 0);
  lv_obj_set_width(mouth, 22);
  start_zzz(false);

  switch (s) {
    case ST_BOOT:
    case ST_IDLE:
      set_eyes(EYE_H);
      start_eye_dart(false);
      start_body_anim(4, 2600);
      break;
    case ST_WORK:
      set_eyes(EYE_H);
      start_eye_dart(true);
      start_body_anim(8, 520);
      break;
    case ST_SLEEP:
      set_eyes(5);
      start_eye_dart(false);
      start_body_anim(3, 3600);
      lv_obj_set_style_opa(body, LV_OPA_80, 0);
      start_zzz(true);
      break;
    case ST_CAPPED:
      set_eyes(8);
      start_eye_dart(false);
      start_body_anim(0, 0);
      lv_obj_set_style_bg_color(body, C_GRAYBODY, 0);
      lv_obj_set_width(mouth, 34);
      break;
    case ST_NO_WIFI:
    case ST_NO_COMPANION:
      set_eyes(EYE_H);
      start_eye_dart(false);
      start_body_anim(2, 1800);
      lv_obj_set_style_opa(body, LV_OPA_70, 0);
      break;
    case ST_PORTAL:
      set_eyes(EYE_H);
      start_eye_dart(true); /* curious: looking around for new networks */
      start_body_anim(3, 1200);
      break;
  }
}

static void fmt_reset(char *out, size_t n, long sec) {
  if (sec < 0) { out[0] = 0; return; }
  long h = sec / 3600, m = (sec % 3600) / 60;
  if (h > 48) snprintf(out, n, "resetea en %ldd %ldh", h / 24, h % 24);
  else if (h > 0) snprintf(out, n, "resetea en %ldh %02ldm", h, m);
  else snprintf(out, n, "resetea en %ldm", m);
}

void ui_update(ConnState conn, const BuddyData &d) {
  /* connection dot */
  lv_obj_set_style_bg_color(connDot,
      conn == CONN_OK ? lv_color_hex(0x7BAE6C)
    : conn == CONN_PORTAL ? C_ORANGE
    : conn == CONN_BOOTING ? C_MUTED : C_BAD, 0);

  int maxPct = -1;
  for (int i = 0; i < d.nLimits; i++) maxPct = LV_MAX(maxPct, d.limits[i].pct);

  /* derive buddy state */
  BuddyState s;
  if (conn == CONN_PORTAL) s = ST_PORTAL;
  else if (conn == CONN_NO_WIFI) s = ST_NO_WIFI;
  else if (conn == CONN_NO_COMPANION || (conn == CONN_OK && !d.companionOk)) s = ST_NO_COMPANION;
  else if (conn == CONN_BOOTING) s = ST_BOOT;
  else if (maxPct >= 100) s = ST_CAPPED;
  else if (d.active) s = ST_WORK;
  else if (d.lastActivityAgoSec < 0 || d.lastActivityAgoSec > 1800) s = ST_SLEEP;
  else s = ST_IDLE;
  apply_state(s);

  /* sweat drop when close to a limit (but still alive) */
  if (s != ST_CAPPED && maxPct >= 85) lv_obj_clear_flag(sweat, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(sweat, LV_OBJ_FLAG_HIDDEN);

  /* status text */
  switch (s) {
    case ST_BOOT:        lv_label_set_text(statusLbl, "arrancando..."); break;
    case ST_NO_WIFI:     lv_label_set_text(statusLbl, "sin WiFi..."); break;
    case ST_NO_COMPANION:lv_label_set_text(statusLbl, "buscando companion..."); break;
    case ST_SLEEP:       lv_label_set_text(statusLbl, "durmiendo"); break;
    case ST_IDLE:        lv_label_set_text(statusLbl, "listo para ayudar"); break;
    case ST_WORK:
      if (maxPct >= 85) lv_label_set_text(statusLbl, "trabajando... ojo el limite!");
      else if (d.activeSessions > 0)
        lv_label_set_text_fmt(statusLbl, "trabajando... (%d %s)",
             d.activeSessions, d.activeSessions == 1 ? "sesion" : "sesiones");
      else lv_label_set_text(statusLbl, "trabajando...");
      break;
    case ST_CAPPED:      lv_label_set_text(statusLbl, "limite alcanzado"); break;
    case ST_PORTAL:      lv_label_set_text(statusLbl, "config de WiFi"); break;
  }

  /* plan badge */
  if (d.plan[0]) {
    char up[12];
    for (int i = 0; i < 11 && d.plan[i]; i++) up[i] = toupper(d.plan[i]), up[i + 1] = 0;
    lv_label_set_text(planLbl, up);
  }

  /* limit bars */
  for (int i = 0; i < 3; i++) {
    if (i >= d.nLimits) { lv_obj_add_flag(bars[i].group, LV_OBJ_FLAG_HIDDEN); continue; }
    const LimitBar &L = d.limits[i];
    lv_obj_clear_flag(bars[i].group, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(bars[i].label, L.label);
    if (L.pct >= 0) {
      lv_label_set_text_fmt(bars[i].pct, "%d%%", L.pct);
      lv_bar_set_value(bars[i].bar, L.pct, LV_ANIM_ON);
    } else {
      lv_label_set_text(bars[i].pct, "--");
      lv_bar_set_value(bars[i].bar, 0, LV_ANIM_OFF);
    }
    lv_color_t c = C_GOOD;
    if (L.pct >= 90 || strcmp(L.severity, "exceeded") == 0) c = C_BAD;
    else if (L.pct >= 70 || strcmp(L.severity, "normal") != 0) c = C_WARN;
    lv_obj_set_style_bg_color(bars[i].bar, c, LV_PART_INDICATOR);
    char sub[40];
    fmt_reset(sub, sizeof(sub), L.resetsInSec);
    lv_label_set_text(bars[i].sub, sub);
  }

  /* data-source icon: home = companion, antenna = direct API */
  if (conn == CONN_OK && d.source == SRC_DIRECT) lv_label_set_text(srcIcon, LV_SYMBOL_WIFI);
  else if (conn == CONN_OK && d.source == SRC_COMPANION) lv_label_set_text(srcIcon, LV_SYMBOL_HOME);
  else lv_label_set_text(srcIcon, "");

  /* footer */
  if (conn == CONN_OK && d.companionOk) {
    if (d.tokensToday < 0) {
      lv_label_set_text(footL, "modo directo");
    } else {
    char tok[16];
    if (d.tokensToday >= 1000000) snprintf(tok, sizeof(tok), "%.1fM", d.tokensToday / 1e6);
    else if (d.tokensToday >= 1000) snprintf(tok, sizeof(tok), "%ldk", d.tokensToday / 1000);
    else snprintf(tok, sizeof(tok), "%ld", d.tokensToday);
    lv_label_set_text_fmt(footL, "hoy: %s tokens", tok);
    }
    if (d.lastActivityAgoSec >= 0) {
      long m = d.lastActivityAgoSec / 60;
      if (m < 1) lv_label_set_text(footR, "actividad: ahora");
      else if (m < 60) lv_label_set_text_fmt(footR, "actividad: hace %ldm", m);
      else lv_label_set_text_fmt(footR, "actividad: hace %ldh", m / 60);
    } else {
      lv_label_set_text(footR, "");
    }
  } else {
    lv_label_set_text(footL, "");
    lv_label_set_text(footR, "");
  }
}
