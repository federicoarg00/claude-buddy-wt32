/* Pomodoro timer tile: 25' focus / 5' break, 15' long break every 4 cycles.
 * Break auto-starts after focus; the next focus waits for a tap.
 * Daily completed-cycle counter persisted in NVS, day rollover driven by
 * the companion's dateLocal. */
#include "pomodoro.h"
#include "ui.h"
#include <Arduino.h>
#include <Preferences.h>

#define C_BG     lv_color_hex(0x1F1E1B)
#define C_CARD   lv_color_hex(0x2B2A27)
#define C_ORANGE lv_color_hex(0xD97757)
#define C_TEXT   lv_color_hex(0xECEAE5)
#define C_MUTED  lv_color_hex(0x9B9892)
#define C_GREEN  lv_color_hex(0x8AA672)
#define C_TRACK  lv_color_hex(0x413F3A)

static const uint32_t FOCUS_MS = 25UL * 60 * 1000;
static const uint32_t BREAK_MS = 5UL * 60 * 1000;
static const uint32_t LONG_MS = 15UL * 60 * 1000;

enum Phase { PH_IDLE, PH_FOCUS, PH_BREAK };

static Phase phase = PH_IDLE;
static bool paused = false;      /* only meaningful in FOCUS/BREAK */
static bool longBreak = false;   /* current/last break is the long one */
static uint32_t durMs = FOCUS_MS;
static uint32_t remainMs = FOCUS_MS;
static uint32_t lastMs = 0;
static int setIdx = 0;           /* focus sessions completed in current set, 0..4 */
static int todayCount = 0;
static char today[12] = "";

static Preferences prefs;

/* history of past days, oldest first, persisted as an NVS blob */
static const int HIST_MAX = 40;
static PomoDay hist[HIST_MAX];
static int nHist = 0;
static void (*onChange)() = nullptr;

/* widgets */
static lv_obj_t *arc, *timeLbl, *phaseLbl, *hintLbl, *countLbl, *dots[4];
static lv_obj_t *flashOverlay = nullptr;

/* days since 1970-01-01 from "YYYY-MM-DD" (days-from-civil, H. Hinnant) */
static uint16_t day_num(const char *date) {
  int Y, M, D;
  if (sscanf(date, "%d-%d-%d", &Y, &M, &D) != 3) return 0;
  int y = Y - (M <= 2);
  int era = y / 400;
  int yoe = y - era * 400;
  int doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (uint16_t)((long)era * 146097 + doe - 719468);
}

/* ---------------------------------------------------------------- persist */
static void persist() {
  prefs.putInt("count", todayCount);
  prefs.putString("date", today);
}

static void hist_save() { prefs.putBytes("hist", hist, nHist * sizeof(PomoDay)); }

static void hist_load() {
  size_t len = prefs.getBytesLength("hist");
  nHist = min((int)(len / sizeof(PomoDay)), HIST_MAX);
  if (nHist) prefs.getBytes("hist", hist, nHist * sizeof(PomoDay));
}

static void hist_append(uint16_t day, uint16_t count) {
  if (!day || !count) return;
  for (int i = 0; i < nHist; i++)
    if (hist[i].day == day) { hist[i].count = max(hist[i].count, count); hist_save(); return; }
  if (nHist == HIST_MAX) {
    for (int i = 1; i < HIST_MAX; i++) hist[i - 1] = hist[i];
    nHist--;
  }
  hist[nHist].day = day;
  hist[nHist].count = count;
  nHist++;
  hist_save();
}

void pomodoro_set_date(const char *dateLocal) {
  if (!dateLocal || !dateLocal[0]) return;
  if (strcmp(today, dateLocal) == 0) return;
  bool firstSync = (today[0] == 0);
  char prev[12];
  strlcpy(prev, today, sizeof(prev));
  strlcpy(today, dateLocal, sizeof(today));
  if (firstSync) {
    /* boot: keep the persisted count if it's from the same day; if the
     * device slept past midnight, archive the stale day into history */
    String saved = prefs.getString("date", "");
    if (saved == today) {
      todayCount = prefs.getInt("count", 0);
    } else {
      if (saved.length()) hist_append(day_num(saved.c_str()), prefs.getInt("count", 0));
      todayCount = 0;
      persist();
    }
  } else {
    hist_append(day_num(prev), todayCount); /* midnight rollover */
    todayCount = 0;
    persist();
  }
  if (onChange) onChange();
}

int pomodoro_get_history(PomoDay *out, int maxN) {
  int n = min(nHist, maxN);
  memcpy(out, hist + (nHist - n), n * sizeof(PomoDay));
  return n;
}

int pomodoro_today_count() { return todayCount; }
uint16_t pomodoro_today_daynum() { return today[0] ? day_num(today) : 0; }
void pomodoro_set_on_change(void (*cb)()) { onChange = cb; }

/* ---------------------------------------------------------------- render */
static void render() {
  uint32_t sec = (remainMs + 999) / 1000;
  lv_label_set_text_fmt(timeLbl, "%02u:%02u", (unsigned)(sec / 60), (unsigned)(sec % 60));
  lv_arc_set_range(arc, 0, durMs / 1000);
  lv_arc_set_value(arc, sec);

  lv_color_t accent = (phase == PH_BREAK) ? C_GREEN : C_ORANGE;
  lv_obj_set_style_arc_color(arc, accent, LV_PART_INDICATOR);

  switch (phase) {
    case PH_IDLE:
      lv_label_set_text(phaseLbl, "FOCO");
      lv_label_set_text(hintLbl, "toca el circulo para empezar");
      break;
    case PH_FOCUS:
      lv_label_set_text(phaseLbl, "FOCO");
      lv_label_set_text(hintLbl, paused ? "pausado - toca para seguir" : "toca para pausar");
      break;
    case PH_BREAK:
      lv_label_set_text(phaseLbl, longBreak ? "DESCANSO LARGO" : "DESCANSO");
      lv_label_set_text(hintLbl, paused ? "pausado - toca para seguir" : "a despejarse!");
      break;
  }
  lv_obj_set_style_text_color(phaseLbl, accent, 0);

  lv_label_set_text_fmt(countLbl, "%d", todayCount);
  for (int i = 0; i < 4; i++) {
    bool done = i < setIdx;
    bool current = (i == setIdx && phase != PH_IDLE) || (i == setIdx && setIdx > 0);
    lv_obj_set_style_bg_color(dots[i], done ? C_ORANGE : C_TRACK, 0);
    lv_obj_set_style_border_color(dots[i], (i == setIdx && phase == PH_FOCUS) ? C_ORANGE : C_TRACK, 0);
    (void)current;
  }
}

/* ---------------------------------------------------------------- alert */
static void flash_anim_cb(void *obj, int32_t v) {
  lv_obj_set_style_opa((lv_obj_t *)obj, v, 0);
}
static void flash_done_cb(lv_anim_t *) {
  if (flashOverlay) lv_obj_add_flag(flashOverlay, LV_OBJ_FLAG_HIDDEN);
}
static void alert() {
  if (!flashOverlay) {
    flashOverlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(flashOverlay, 480, 320);
    lv_obj_set_pos(flashOverlay, 0, 0);
    lv_obj_set_style_bg_color(flashOverlay, C_ORANGE, 0);
    lv_obj_set_style_border_width(flashOverlay, 0, 0);
    lv_obj_set_style_radius(flashOverlay, 0, 0);
    lv_obj_clear_flag(flashOverlay, LV_OBJ_FLAG_CLICKABLE);
  }
  lv_obj_clear_flag(flashOverlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_opa(flashOverlay, LV_OPA_TRANSP, 0);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, flashOverlay);
  lv_anim_set_exec_cb(&a, flash_anim_cb);
  lv_anim_set_values(&a, 0, 150);
  lv_anim_set_time(&a, 160);
  lv_anim_set_playback_time(&a, 200);
  lv_anim_set_repeat_count(&a, 3);
  lv_anim_set_ready_cb(&a, flash_done_cb);
  lv_anim_start(&a);
  ui_show_pomodoro_tile();
}

/* ---------------------------------------------------------------- machine */
static void load_focus() {
  phase = PH_IDLE;
  paused = false;
  durMs = FOCUS_MS;
  remainMs = FOCUS_MS;
}

static void session_end() {
  if (phase == PH_FOCUS) {
    todayCount++;
    setIdx++;
    persist();
    if (onChange) onChange();
    longBreak = (setIdx >= 4);
    phase = PH_BREAK;
    paused = false; /* break auto-starts */
    durMs = longBreak ? LONG_MS : BREAK_MS;
    remainMs = durMs;
  } else { /* break finished */
    if (longBreak) { setIdx = 0; longBreak = false; }
    load_focus();
  }
  alert();
  render();
}

static void tick_cb(lv_timer_t *) {
  uint32_t now = millis();
  uint32_t dt = now - lastMs;
  lastMs = now;
  if ((phase == PH_FOCUS || phase == PH_BREAK) && !paused) {
    if (remainMs <= dt) { session_end(); return; }
    remainMs -= dt;
    render();
  }
}

/* ---------------------------------------------------------------- input */
static void center_tap_cb(lv_event_t *) {
  if (phase == PH_IDLE) { phase = PH_FOCUS; paused = false; }
  else paused = !paused;
  render();
}

static void skip_cb(lv_event_t *) {
  if (phase == PH_FOCUS) load_focus();       /* abandon focus, no count */
  else if (phase == PH_BREAK) {              /* cut the break short */
    if (longBreak) { setIdx = 0; longBreak = false; }
    load_focus();
  }
  render();
}

static void reset_cb(lv_event_t *) {
  remainMs = durMs;
  if (phase != PH_IDLE) paused = true;
  render();
}

/* ---------------------------------------------------------------- build */
void pomodoro_create(lv_obj_t *tile) {
  prefs.begin("pomo", false);
  hist_load();
  todayCount = 0; /* until the companion tells us the date */

  lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

  phaseLbl = lv_label_create(tile);
  lv_obj_set_style_text_font(phaseLbl, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(phaseLbl, C_ORANGE, 0);
  lv_obj_align(phaseLbl, LV_ALIGN_TOP_MID, -60, 16);

  arc = lv_arc_create(tile);
  lv_obj_set_size(arc, 225, 225);
  lv_obj_align(arc, LV_ALIGN_LEFT_MID, 55, 6);
  lv_arc_set_rotation(arc, 270);
  lv_arc_set_bg_angles(arc, 0, 360);
  lv_arc_set_mode(arc, LV_ARC_MODE_REVERSE);
  lv_obj_set_style_arc_width(arc, 12, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, 12, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc, C_TRACK, LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc, C_ORANGE, LV_PART_INDICATOR);
  lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
  lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);

  /* invisible tap target over the arc's center */
  lv_obj_t *tap = lv_obj_create(tile);
  lv_obj_set_size(tap, 170, 170);
  lv_obj_align_to(tap, arc, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_opa(tap, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(tap, 0, 0);
  lv_obj_set_style_radius(tap, LV_RADIUS_CIRCLE, 0);
  lv_obj_clear_flag(tap, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(tap, center_tap_cb, LV_EVENT_CLICKED, nullptr);

  timeLbl = lv_label_create(tile);
  lv_obj_set_style_text_font(timeLbl, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(timeLbl, C_TEXT, 0);
  lv_obj_align_to(timeLbl, arc, LV_ALIGN_CENTER, 0, 0);
  lv_label_set_text(timeLbl, "25:00");

  hintLbl = lv_label_create(tile);
  lv_obj_set_style_text_font(hintLbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(hintLbl, C_MUTED, 0);
  lv_obj_align(hintLbl, LV_ALIGN_BOTTOM_MID, -60, -10);

  /* right column: set dots, daily count, buttons */
  for (int i = 0; i < 4; i++) {
    dots[i] = lv_obj_create(tile);
    lv_obj_set_size(dots[i], 16, 16);
    lv_obj_set_style_radius(dots[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dots[i], C_TRACK, 0);
    lv_obj_set_style_border_width(dots[i], 2, 0);
    lv_obj_set_style_border_color(dots[i], C_TRACK, 0);
    lv_obj_align(dots[i], LV_ALIGN_TOP_RIGHT, -95 + i * 24, 20);
    lv_obj_clear_flag(dots[i], LV_OBJ_FLAG_CLICKABLE);
  }

  lv_obj_t *countCard = lv_obj_create(tile);
  lv_obj_set_size(countCard, 120, 96);
  lv_obj_set_style_radius(countCard, 12, 0);
  lv_obj_set_style_border_width(countCard, 0, 0);
  lv_obj_set_style_bg_color(countCard, C_CARD, 0);
  lv_obj_align(countCard, LV_ALIGN_RIGHT_MID, -22, -34);
  lv_obj_clear_flag(countCard, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *countTitle = lv_label_create(countCard);
  lv_obj_set_style_text_font(countTitle, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(countTitle, C_MUTED, 0);
  lv_label_set_text(countTitle, "ciclos hoy");
  lv_obj_align(countTitle, LV_ALIGN_TOP_MID, 0, 0);

  countLbl = lv_label_create(countCard);
  lv_obj_set_style_text_font(countLbl, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(countLbl, C_ORANGE, 0);
  lv_label_set_text(countLbl, "0");
  lv_obj_align(countLbl, LV_ALIGN_BOTTOM_MID, 0, 4);

  struct BtnDef { const char *sym; lv_event_cb_t cb; int xofs; };
  BtnDef defs[] = {
    { LV_SYMBOL_NEXT, skip_cb, -22 },
    { LV_SYMBOL_REFRESH, reset_cb, -86 },
  };
  for (auto &b : defs) {
    lv_obj_t *btn = lv_btn_create(tile);
    lv_obj_set_size(btn, 56, 44);
    lv_obj_set_style_bg_color(btn, C_CARD, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_RIGHT, b.xofs, -22);
    lv_obj_add_event_cb(btn, b.cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = lv_label_create(btn);
    lv_obj_set_style_text_color(l, C_MUTED, 0);
    lv_label_set_text(l, b.sym);
    lv_obj_center(l);
  }

  lastMs = millis();
  lv_timer_create(tick_cb, 250, nullptr);
  load_focus();
  render();
}
