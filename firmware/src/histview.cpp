/* 30-day pomodoro calendar: real calendar layout (Mon-Sun columns, weeks as
 * rows), color-coded per day, plus a small stats panel.
 *   >10 cycles: green · 5-10: yellow · 1-4: red · 0 / no data: neutral gray */
#include "histview.h"
#include "pomodoro.h"
#include <Arduino.h>

#define C_BG     lv_color_hex(0x1F1E1B)
#define C_CARD   lv_color_hex(0x2B2A27)
#define C_ORANGE lv_color_hex(0xD97757)
#define C_TEXT   lv_color_hex(0xECEAE5)
#define C_MUTED  lv_color_hex(0x9B9892)
#define C_GREEN  lv_color_hex(0x7BAE6C)
#define C_YELLOW lv_color_hex(0xE0A93E)
#define C_RED    lv_color_hex(0xCC3F33)
#define C_EMPTY  lv_color_hex(0x2B2A27)

static const int DAYS = 30;
static const int COLS = 7;
static const int ROWS = 6;

static const int GREEN_MIN = 11; /* "más de 10" */
static const int YELLOW_MIN = 5; /* 5 a 10 */

static lv_obj_t *cells[ROWS][COLS];
static lv_obj_t *cellLbls[ROWS][COLS];
static lv_obj_t *titleLbl, *totalLbl, *avgLbl, *bestLbl, *todayLbl, *waitLbl;

/* civil-from-days: day-of-month for a days-since-epoch value */
static int day_of_month(long z) {
  z += 719468;
  long era = z / 146097;
  long doe = z - era * 146097;
  long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  long mp = (5 * doy + 2) / 153;
  return (int)(doy - (153 * mp + 2) / 5 + 1);
}

static int weekday_mon0(long d) { return (int)((d + 3) % 7); } /* 1970-01-01 = Thursday */

static lv_color_t color_for(int count, bool hasData) {
  if (!hasData || count == 0) return C_EMPTY;
  if (count >= GREEN_MIN) return C_GREEN;
  if (count >= YELLOW_MIN) return C_YELLOW;
  return C_RED;
}

void histview_refresh() {
  uint16_t today = pomodoro_today_daynum();
  if (!today) {
    lv_obj_clear_flag(waitLbl, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_add_flag(waitLbl, LV_OBJ_FLAG_HIDDEN);

  PomoDay hist[40];
  int nHist = pomodoro_get_history(hist, 40);

  uint16_t first = today - (DAYS - 1);
  long firstMonday = first - weekday_mon0(first);

  int total = 0, best = 0, daysWithData = 0;

  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++) {
      lv_obj_add_flag(cells[r][c], LV_OBJ_FLAG_HIDDEN);
    }

  for (int i = 0; i < DAYS; i++) {
    uint16_t d = first + i;
    int col = weekday_mon0(d);
    int row = (int)((d - weekday_mon0(d) - firstMonday) / 7);
    if (row < 0 || row >= ROWS) continue;

    int count = 0;
    bool hasData = false;
    for (int j = 0; j < nHist; j++)
      if (hist[j].day == d) { count = hist[j].count; hasData = true; break; }
    if (d == today) { /* live count wins, but never below an archived value */
      count = max(count, pomodoro_today_count());
      hasData = true;
    }

    if (hasData) { total += count; daysWithData++; best = max(best, count); }

    lv_obj_t *cell = cells[row][col];
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(cell, color_for(count, hasData), 0);
    lv_obj_set_style_border_width(cell, d == today ? 2 : 0, 0);
    bool colored = hasData && count > 0;
    lv_obj_set_style_text_color(cellLbls[row][col],
        colored ? lv_color_hex(0x201A16) : C_MUTED, 0);
    lv_label_set_text_fmt(cellLbls[row][col], "%d", day_of_month(d));
  }

  lv_label_set_text_fmt(totalLbl, "%d", total);
  char avg[16];
  snprintf(avg, sizeof(avg), "%.1f", daysWithData ? (float)total / DAYS : 0.0f);
  lv_label_set_text(avgLbl, avg);
  lv_label_set_text_fmt(bestLbl, "%d", best);
  lv_label_set_text_fmt(todayLbl, "%d", pomodoro_today_count());
}

static lv_obj_t *stat_row(lv_obj_t *parent, const char *title, int y, lv_obj_t **valOut) {
  lv_obj_t *t = lv_label_create(parent);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(t, C_MUTED, 0);
  lv_label_set_text(t, title);
  lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, y);
  lv_obj_t *v = lv_label_create(parent);
  lv_obj_set_style_text_font(v, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(v, C_TEXT, 0);
  lv_label_set_text(v, "-");
  lv_obj_align(v, LV_ALIGN_TOP_RIGHT, 0, y - 2);
  *valOut = v;
  return t;
}

void histview_create(lv_obj_t *tile) {
  lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

  titleLbl = lv_label_create(tile);
  lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(titleLbl, C_TEXT, 0);
  lv_label_set_text(titleLbl, "Pomodoros - ultimos 30 dias");
  lv_obj_align(titleLbl, LV_ALIGN_TOP_LEFT, 14, 12);

  static const char *wd[] = {"L", "M", "X", "J", "V", "S", "D"};
  const int x0 = 14, y0 = 62, cw = 45, ch = 36, gap = 3;
  for (int c = 0; c < COLS; c++) {
    lv_obj_t *h = lv_label_create(tile);
    lv_obj_set_style_text_font(h, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(h, C_MUTED, 0);
    lv_label_set_text(h, wd[c]);
    lv_obj_set_pos(h, x0 + c * (cw + gap) + cw / 2 - 4, y0 - 20);
  }
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++) {
      lv_obj_t *cell = lv_obj_create(tile);
      lv_obj_set_size(cell, cw, ch);
      lv_obj_set_pos(cell, x0 + c * (cw + gap), y0 + r * (ch + gap));
      lv_obj_set_style_radius(cell, 7, 0);
      lv_obj_set_style_border_width(cell, 0, 0);
      lv_obj_set_style_border_color(cell, C_TEXT, 0);
      lv_obj_set_style_bg_color(cell, C_EMPTY, 0);
      lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_flag(cell, LV_OBJ_FLAG_HIDDEN);
      cells[r][c] = cell;
      lv_obj_t *l = lv_label_create(cell);
      lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
      lv_obj_set_style_text_color(l, C_MUTED, 0);
      lv_label_set_text(l, "");
      lv_obj_center(l);
      cellLbls[r][c] = l;
    }

  /* stats panel */
  lv_obj_t *panel = lv_obj_create(tile);
  lv_obj_set_size(panel, 112, 214);
  lv_obj_align(panel, LV_ALIGN_TOP_RIGHT, -10, 48);
  lv_obj_set_style_radius(panel, 12, 0);
  lv_obj_set_style_border_width(panel, 0, 0);
  lv_obj_set_style_bg_color(panel, C_CARD, 0);
  lv_obj_set_style_pad_all(panel, 12, 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

  stat_row(panel, "total", 0, &totalLbl);
  stat_row(panel, "prom/dia", 46, &avgLbl);
  stat_row(panel, "mejor dia", 92, &bestLbl);
  stat_row(panel, "hoy", 138, &todayLbl);

  /* legend */
  struct { lv_color_t c; const char *t; } leg[] = {
    { C_GREEN, ">10" }, { C_YELLOW, "5-10" }, { C_RED, "<5" },
  };
  for (int i = 0; i < 3; i++) {
    lv_obj_t *sw = lv_obj_create(tile);
    lv_obj_set_size(sw, 12, 12);
    lv_obj_set_style_radius(sw, 4, 0);
    lv_obj_set_style_border_width(sw, 0, 0);
    lv_obj_set_style_bg_color(sw, leg[i].c, 0);
    lv_obj_set_pos(sw, 250 + i * 62, 16);
    lv_obj_t *lt = lv_label_create(tile);
    lv_obj_set_style_text_font(lt, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lt, C_MUTED, 0);
    lv_label_set_text(lt, leg[i].t);
    lv_obj_set_pos(lt, 266 + i * 62, 14);
  }

  waitLbl = lv_label_create(tile);
  lv_obj_set_style_text_font(waitLbl, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(waitLbl, C_MUTED, 0);
  lv_label_set_text(waitLbl, "esperando fecha...");
  lv_obj_center(waitLbl);

  /* self-refresh once a minute (midnight, external count changes) */
  lv_timer_create([](lv_timer_t *) { histview_refresh(); }, 60000, nullptr);
  histview_refresh();
}
