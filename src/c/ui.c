// Notice Me — windows, all drawn by hand in one style: a "rotating drum" of rows (layout math in nm_wheel.c) whose
// selected row is full size between a band and a bar in its colour, and screens with a coloured header over a white
// body. The list shows the few tasks at the top of the task server's list (selecting one starts it); the session
// screen has the countdown, with its actions on a drum of their own; the nudge is a screen of the same kind.
#include "app.h"
#include "nm_wheel.h"
#include "nm_marquee.h"

#define STATUS_H 16
#define ROUND_STATUS_Y 4
#define ROUND_WHEEL_Y 20
#define BAR_H 3
#define BAND_H 15         // the selected row's top band when it carries text (BAR_H when it does not)
#define BADGE_GAP 2       // min. white rows between the band and the badge
#define REPEAT_MS 120
#define ANIM_MS 180
#define FRAME_MS 30
#define ROWS_MAX 6        // session row + up to 5 tasks

typedef enum { ROW_SESSION, ROW_TASK } RowKind;
typedef struct { uint8_t kind; uint8_t task; } Row;  // task: index into g_tasks.items

typedef enum { ACT_PAUSE, ACT_DONE, ACT_SKIP, ACT_STOP } Action;

// What a drum row shows. The strings must stay valid until the row is drawn.
typedef struct {
  const char *band;    // in the selected row's top band; NULL or "" = a plain bar
  const char *badge;   // short text in a rounded badge of the row's colour; NULL or "" = none
  const char *title;
  const char *detail;  // second line; NULL or "" = none
  GColor color;
} RowView;

// A drum of rows on a layer. count/row describe the rows; the drum keeps the selection and animates to it.
typedef struct {
  Layer *layer;
  WheelGeometry geom;
  int (*count)(void);
  void (*row)(int index, RowView *out);
  const char *empty_title, *empty_hint;  // shown instead of rows when there are none
  int sel;                 // selected row
  int32_t pos;             // drawn position, milli-rows
  int32_t anim_from;
  uint32_t anim_start;
  AppTimer *anim_timer;
  uint32_t marquee_start;  // the selected row's title last started scrolling (ms)
  uint32_t marquee_wait;   // ms until a marquee next moves, 0 = nothing scrolls
  AppTimer *marquee_timer;
} Drum;

static Window *s_list_window, *s_session_window, *s_menu_window, *s_nudge_window;
static Layer *s_list_status_layer, *s_menu_status_layer, *s_session_layer, *s_nudge_layer;
static Drum s_list, s_menu;

static GFont s_f14, s_f14b, s_f18, s_f18b, s_f24b, s_leco;

static Row s_rows[ROWS_MAX];
static int s_row_count;
static int s_more;         // open tasks that are not listed

static Action s_actions[4];
static int s_action_count;

// ---- Colours ------------------------------------------------------------------

// A task's colour is its priority's.
static GColor priority_color(int priority) {
#if defined(PBL_COLOR)
  switch (priority) {
    case 5: return GColorRed;
    case 4: return GColorOrange;
    case 3: return GColorChromeYellow;
    case 2: return GColorCobaltBlue;
    case 1: return GColorCadetBlue;
    default: return GColorDarkGray;
  }
#else
  (void)priority;
  return GColorBlack;
#endif
}

static GColor phase_color(void) {
#if defined(PBL_COLOR)
  if (g_session.paused) return GColorDarkGray;
  switch (g_session.phase) {
    case NM_PHASE_FOCUS: return GColorDarkCandyAppleRed;
    case NM_PHASE_BREAK: return GColorIslamicGreen;
    default: return GColorCobaltBlue;
  }
#else
  return GColorBlack;
#endif
}

// Text on a colour: black on the light ones.
static GColor text_on(GColor c) {
#if defined(PBL_COLOR)
  return 299 * c.r + 587 * c.g + 114 * c.b > 2000 ? GColorBlack : GColorWhite;
#else
  (void)c;
  return GColorWhite;
#endif
}

// ---- Text ---------------------------------------------------------------------

// Gothic fonts leave blank space above the glyphs; these approximate it so text can be centred visually.
typedef struct { GFont font; int16_t top, cap, line; } FontMetrics;

static FontMetrics font_info(GFont f) {
  if (f == s_f24b) return (FontMetrics){ f, 8, 16, 28 };
  if (f == s_f18 || f == s_f18b) return (FontMetrics){ f, 6, 12, 22 };
  return (FontMetrics){ f, 4, 10, 18 };
}

// Draws text in a single line whose glyph middle is at cy.
static void draw_line(GContext *ctx, const char *text, GFont font, int16_t x, int16_t w, int16_t cy,
                      GTextAlignment align) {
  FontMetrics fi = font_info(font);
  GRect r = GRect(x, cy - fi.cap / 2 - fi.top, w, fi.line);
  graphics_draw_text(ctx, text, font, r, GTextOverflowModeTrailingEllipsis, align, NULL);
}

static int16_t text_width(const char *text, GFont font) {
  FontMetrics fi = font_info(font);
  return graphics_text_layout_get_content_size(text, font, GRect(0, 0, 1200, fi.line),
                                               GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft).w;
}

static void fill_span(GContext *ctx, GColor c, int16_t x0, int16_t x1, int16_t y0, int16_t y1) {
  if (x1 <= x0 || y1 <= y0) return;
  graphics_context_set_fill_color(ctx, c);
  graphics_fill_rect(ctx, GRect(x0, y0, x1 - x0, y1 - y0), 0, GCornerNone);
}

static uint32_t now_ms(void) {
  time_t sec;
  uint16_t ms;
  time_ms(&sec, &ms);
  return (uint32_t)sec * 1000u + ms;
}

static void format_clock(int32_t t, char *buf, size_t size) {
  time_t tt = (time_t)t;
  strftime(buf, size, clock_is_24h_style() ? "%H:%M" : "%I:%M", localtime(&tt));
  if (buf[0] == '0') memmove(buf, buf + 1, strlen(buf));
}

static void format_countdown(char *buf, size_t size) {
  int32_t left = nm_session_remaining(&g_session, app_now());
  snprintf(buf, size, "%d:%02d", (int)(left / 60), (int)(left % 60));
}

// "Focus 2/4", "Break", "Long break"; "Paused" while it is.
static void format_phase(char *buf, size_t size) {
  if (g_session.paused) snprintf(buf, size, "Paused");
  else if (g_session.phase == NM_PHASE_FOCUS) snprintf(buf, size, "Focus %d/%d", g_session.round, g_settings.rounds);
  else snprintf(buf, size, "%s", g_session.phase == NM_PHASE_BREAK ? "Break" : "Long break");
}

// ---- Drum ---------------------------------------------------------------------

static void drum_anim_step(void *data) {
  Drum *d = data;
  d->anim_timer = NULL;
  int32_t target = (int32_t)d->sel * 1000;
  uint32_t t = now_ms() - d->anim_start;
  if (t >= ANIM_MS) {
    d->pos = target;
  } else {
    int32_t p = (int32_t)(t * 1000u / ANIM_MS);                  // linear progress 0..1000
    int32_t e = 1000 - (1000 - p) * (1000 - p) / 1000;           // quadratic ease-out
    d->pos = d->anim_from + (target - d->anim_from) * e / 1000;
    d->anim_timer = app_timer_register(FRAME_MS, drum_anim_step, d);
  }
  if (d->layer) layer_mark_dirty(d->layer);
}

static void drum_jump(Drum *d) {
  if (d->anim_timer) {
    app_timer_cancel(d->anim_timer);
    d->anim_timer = NULL;
  }
  d->pos = (int32_t)d->sel * 1000;
  d->marquee_start = now_ms();
  if (d->layer) layer_mark_dirty(d->layer);
}

// Animate from the current (possibly mid-animation) position to the selected row.
static void drum_animate(Drum *d) {
  if (app_testing()) {  // deterministic frames for the screenshots
    drum_jump(d);
    return;
  }
  if (d->anim_timer) app_timer_cancel(d->anim_timer);
  d->anim_from = d->pos;
  d->anim_start = now_ms();
  d->marquee_start = d->anim_start + ANIM_MS;  // the new row's marquee starts once it has arrived
  drum_anim_step(d);
}

static void drum_move(Drum *d, int delta) {
  int n = d->count(), sel = d->sel + delta;
  if (sel >= n) sel = n - 1;
  if (sel < 0) sel = 0;
  if (sel == d->sel) return;
  d->sel = sel;
  drum_animate(d);
}

// The rows changed: keep the selection inside them.
static void drum_reload(Drum *d) {
  int n = d->count();
  if (d->sel >= n) {
    d->sel = n > 0 ? n - 1 : 0;
    drum_jump(d);
  } else if (d->layer) {
    layer_mark_dirty(d->layer);
  }
}

static void drum_marquee_tick(void *data) {
  Drum *d = data;
  d->marquee_timer = NULL;
  if (d->layer) layer_mark_dirty(d->layer);
}

// One line of text in the box [bx, bx + bw), glyph middle at cy (text colour already set). Text wider than the box
// scrolls (nm_marquee); the overflow is clipped by repainting rows [y0, y1) outside the box white. Anything else on
// those rows (the badge) must be drawn afterwards.
static void draw_marquee(GContext *ctx, Drum *d, const char *text, GFont font, int16_t bx, int16_t bw, int16_t cy,
                         int16_t y0, int16_t y1, int16_t layer_w) {
  int16_t tw = text_width(text, font);
  if (tw <= bw) {
    draw_line(ctx, text, font, bx, bw, cy, GTextAlignmentLeft);
    return;
  }
  uint32_t wait = 0, now = now_ms();
  uint32_t elapsed = app_testing() || (int32_t)(now - d->marquee_start) <= 0 ? 0 : now - d->marquee_start;
  int off = nm_marquee_offset(tw, bw, elapsed, &wait);
  if (app_testing()) wait = 0;
  if (wait && (!d->marquee_wait || wait < d->marquee_wait)) d->marquee_wait = wait;
  draw_line(ctx, text, font, bx - off, tw + 4, cy, GTextAlignmentLeft);
  // the loop's next copy, following the text after a gap: it comes round to where the text started
  int16_t x2 = bx - off + tw + NM_MARQUEE_GAP;
  if (x2 < bx + bw) draw_line(ctx, text, font, x2, tw + 4, cy, GTextAlignmentLeft);
  fill_span(ctx, GColorWhite, 0, bx, y0, y1);
  fill_span(ctx, GColorWhite, bx + bw, layer_w, y0, y1);
}

static int16_t badge_width(const char *badge, GFont font, int16_t max_w) {
  if (!badge || !badge[0]) return 0;
  FontMetrics fi = font_info(font);
  int16_t pad = fi.cap >= 12 ? 5 : 3;
  int16_t w = text_width(badge, font) + 2 * pad, h = fi.cap + 2 * pad + 1;
  if (w < h) w = h;
  return w > max_w ? max_w : w;
}

// Badge: rounded rect in the row's colour centred on cy. It never reaches above min_y: when the room above cy is
// short it shrinks vertically (never below the glyph height) so it cannot run into the selected row's band.
static void draw_badge(GContext *ctx, const RowView *v, GFont font, int16_t x, int16_t cy, int16_t max_w, int16_t min_y) {
  int16_t w = badge_width(v->badge, font, max_w);
  if (!w) return;
  FontMetrics fi = font_info(font);
  int16_t pad = fi.cap >= 12 ? 5 : 3;
  int16_t h = fi.cap + 2 * pad + 1;
  int32_t room = 2 * ((int32_t)cy - min_y);
  if (h > room) h = room < fi.cap + 3 ? fi.cap + 3 : (int16_t)room;
  graphics_context_set_fill_color(ctx, v->color);
  graphics_fill_rect(ctx, GRect(x, cy - h / 2, w, h), 4, GCornersAll);
  graphics_context_set_text_color(ctx, text_on(v->color));
  draw_line(ctx, v->badge, font, x, w, cy, GTextAlignmentCenter);
}

static void draw_slot(GContext *ctx, Drum *d, const WheelSlot *s, int16_t width) {
  RowView v = { NULL, NULL, "", NULL, GColorBlack };
  d->row(s->index, &v);
  int16_t x = s->inset, w = width - 2 * s->inset;
  if (w <= 8) return;

  int16_t pad = s->level == 0 ? 4 : 3;
  int16_t ix = x + pad, iw = w - 2 * pad;
  // Level 0: a band in the row's colour on top (holding its band text when there is one) and a bar below.
  int16_t band_h = s->level == 0 ? (v.band && v.band[0] ? BAND_H : BAR_H) : 0;
  if (s->level == 0) {
    fill_span(ctx, v.color, x, x + w, s->y, s->y + band_h);
    fill_span(ctx, v.color, x, x + w, s->y + s->h - BAR_H, s->y + s->h);
    if (band_h > BAR_H) {
      graphics_context_set_text_color(ctx, text_on(v.color));
      draw_line(ctx, v.band, s_f14, ix, iw, s->y + band_h / 2, GTextAlignmentLeft);
    }
  }
  int16_t top = s->y + band_h, bottom = s->y + s->h - (s->level == 0 ? BAR_H : 0);
  int16_t ih = bottom - top;

  GColor fg = GColorBlack, dim = GColorBlack;
#if defined(PBL_COLOR)
  dim = GColorDarkGray;
  if (s->level == 2) fg = GColorDarkGray;
#endif

  GFont badge_font = s->level == 0 ? s_f18b : s_f14b;
  GFont title_font = s->level == 0 ? s_f24b : s->level == 1 ? s_f18b : s_f14;
  GFont detail_font = s->level == 0 ? s_f18 : s_f14;
  FontMetrics tfi = font_info(title_font), dfi = font_info(detail_font);
  // Rows shrink continuously while animating, so the detail line shows only where there is room for it.
  bool detail = v.detail && v.detail[0] && s->level < 2 && ih >= tfi.cap + dfi.cap + 8;
  int16_t title_cy, detail_cy = 0;
  if (detail) {
    int16_t gap = (ih - tfi.cap - dfi.cap) / 3;
    title_cy = top + gap + tfi.cap / 2;
    detail_cy = bottom - gap - dfi.cap / 2;
  } else {
    title_cy = top + ih / 2;
  }

  // Title line: badge, title. The selected row's title scrolls when it is too wide; the others are cut with an
  // ellipsis.
  int16_t bw = badge_width(v.badge, badge_font, iw / 2);
  int16_t tx = ix + (bw ? bw + 4 : 0), tw = ix + iw - tx;
  graphics_context_set_text_color(ctx, fg);
  if (s->level == 0) {
    int16_t y0 = title_cy - tfi.cap / 2 - tfi.top, y1 = detail ? detail_cy - dfi.cap / 2 - 1 : bottom;
    if (y0 < top) y0 = top;
    draw_marquee(ctx, d, v.title, title_font, tx, tw, title_cy, y0, y1, width);
    // Descenders reach below y1; clear them beside the box down to the title's line bottom. Safe: the badge and
    // the detail are drawn after this.
    int16_t ly1 = title_cy - tfi.cap / 2 - tfi.top + tfi.line;
    if (ly1 > bottom) ly1 = bottom;
    fill_span(ctx, GColorWhite, 0, tx, y1, ly1);
    fill_span(ctx, GColorWhite, tx + tw, width, y1, ly1);
  } else {
    draw_line(ctx, v.title, title_font, tx, tw, title_cy, GTextAlignmentLeft);
  }
  // Keep the selected row's badge BADGE_GAP px clear of its band.
  draw_badge(ctx, &v, badge_font, ix, title_cy, iw / 2, s->level == 0 ? top + BADGE_GAP : INT16_MIN);
  if (detail) {
    graphics_context_set_text_color(ctx, s->level == 0 ? GColorBlack : dim);
    draw_line(ctx, v.detail, detail_font, ix, iw, detail_cy, GTextAlignmentLeft);
  }
}

static void drum_update(Layer *layer, GContext *ctx) {
  Drum *d = *(Drum **)layer_get_data(layer);
  GRect b = layer_get_bounds(layer);
  d->marquee_wait = 0;
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  int count = d->count();
  if (count == 0) {
    graphics_context_set_text_color(ctx, GColorBlack);
    int16_t inset = PBL_IF_ROUND_ELSE(14, 4);
    GRect r = GRect(inset, b.size.h / 2 - 34, b.size.w - 2 * inset, 50);
    graphics_draw_text(ctx, d->empty_title, s_f18b, r, GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    r.origin.y = b.size.h / 2 + 18;
    r.size.h = 20;
    graphics_draw_text(ctx, d->empty_hint, s_f14, r, GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  } else {
    WheelSlot slots[WHEEL_MAX_SLOTS];
    int n = nm_wheel_layout(&d->geom, count, d->pos, slots, WHEEL_MAX_SLOTS);
    // Draw the selected row last so its bars sit on top of its neighbours.
    int sel = -1;
    for (int i = 0; i < n; i++) {
      if (slots[i].level == 0 && sel < 0) { sel = i; continue; }
      draw_slot(ctx, d, &slots[i], b.size.w);
    }
    if (sel >= 0) draw_slot(ctx, d, &slots[sel], b.size.w);
  }
  // Wake up when a marquee next moves; no timer while nothing scrolls or while the scroll animation redraws anyway.
  if (d->marquee_timer) {
    app_timer_cancel(d->marquee_timer);
    d->marquee_timer = NULL;
  }
  if (d->marquee_wait && !d->anim_timer) d->marquee_timer = app_timer_register(d->marquee_wait, drum_marquee_tick, d);
}

// The drum below a one-line status at the top of the window.
static void drum_create(Drum *d, Window *window, Layer **status_layer, LayerUpdateProc status_update) {
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  int16_t status_y = PBL_IF_ROUND_ELSE(ROUND_STATUS_Y, 0), wheel_y = PBL_IF_ROUND_ELSE(ROUND_WHEEL_Y, STATUS_H);
  window_set_background_color(window, GColorWhite);
  GRect wheel = GRect(0, wheel_y, b.size.w, b.size.h - wheel_y);
  nm_wheel_geometry(&d->geom, wheel.size.w, wheel.size.h, PBL_IF_ROUND_ELSE(true, false));
  d->layer = layer_create_with_data(wheel, sizeof(Drum *));
  *(Drum **)layer_get_data(d->layer) = d;
  layer_set_update_proc(d->layer, drum_update);
  layer_add_child(root, d->layer);
  *status_layer = layer_create(GRect(0, status_y, b.size.w, STATUS_H));
  layer_set_update_proc(*status_layer, status_update);
  layer_add_child(root, *status_layer);
  drum_jump(d);
}

static void drum_destroy(Drum *d, Layer **status_layer) {
  if (d->anim_timer) app_timer_cancel(d->anim_timer);
  if (d->marquee_timer) app_timer_cancel(d->marquee_timer);
  d->anim_timer = d->marquee_timer = NULL;
  layer_destroy(d->layer);
  d->layer = NULL;
  layer_destroy(*status_layer);
  *status_layer = NULL;
}

static void draw_status(Layer *layer, GContext *ctx, const char *text) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_text_color(ctx, GColorBlack);
  int16_t inset = PBL_IF_ROUND_ELSE(40, 2);
  graphics_draw_text(ctx, text, s_f14, GRect(inset, -2, b.size.w - 2 * inset, STATUS_H), GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);
}

// ---- List window: the session (when one runs) and the top tasks -----------------

static void build_rows(void) {
  bool running = g_session.phase != NM_PHASE_IDLE;
  uint8_t shown[6];
  int more = 0;
  // one more than shown, in case the session's own task (it has the session row) is among them
  int n = nm_tasks_visible(&g_tasks, shown, g_settings.show_max + 1, &more);
  int open = n + more, listed = 0;
  s_row_count = 0;
  if (running) s_rows[s_row_count++] = (Row){ ROW_SESSION, 0 };
  for (int i = 0; i < n && listed < g_settings.show_max; i++) {
    if (running && g_tasks.items[shown[i]].id == g_session.task_id) {
      open--;
      continue;
    }
    s_rows[s_row_count++] = (Row){ ROW_TASK, shown[i] };
    listed++;
  }
  s_more = open - listed;
}

static int list_count(void) {
  return s_row_count;
}

static void list_row(int index, RowView *v) {
  static const char *const marks[] = { "!", "!!", "!!!" };
  static const char *const names[] = { "", "Low priority", "Medium priority", "High priority", "Urgent", "Do now" };
  static char badge[8], band[24];
  if (index < 0 || index >= s_row_count) return;
  Row row = s_rows[index];
  if (row.kind == ROW_SESSION) {
    format_countdown(badge, sizeof badge);
    format_phase(band, sizeof band);
    v->band = band;
    v->badge = badge;
    v->title = app_session_title();
    v->detail = g_session.phase == NM_PHASE_FOCUS && g_session.active && !g_session.paused ? "keep moving" : NULL;
    v->color = phase_color();
    return;
  }
  const NmTask *task = &g_tasks.items[row.task];
  int priority = task->priority > 5 ? 5 : task->priority;
  bool started = (task->flags & NM_TASK_STARTED) != 0, active = (task->flags & NM_TASK_ACTIVE) != 0;
  v->band = started ? "In progress" : names[priority];
  v->badge = priority >= 3 ? marks[priority - 3] : NULL;
  v->title = task->title;
  v->detail = active ? "active: nudges on" : NULL;
  v->color = priority_color(priority);
}

// The time, and how much is being kept out of sight (or what the phone has to say).
static void list_status_update(Layer *layer, GContext *ctx) {
  char now[8], text[STATUS_LEN + 12];
  format_clock(app_now(), now, sizeof now);
  if (g_status[0]) snprintf(text, sizeof text, "%s  %s", now, g_status);
  else if (s_more > 0) snprintf(text, sizeof text, "%s  +%d more", now, s_more);
  else snprintf(text, sizeof text, "%s", now);
  draw_status(layer, ctx, text);
}

static void list_up(ClickRecognizerRef rec, void *ctx) {
  app_touched();
  drum_move(&s_list, -1);
}

static void list_down(ClickRecognizerRef rec, void *ctx) {
  app_touched();
  drum_move(&s_list, 1);
}

// Selecting a task starts it: it moves to "in progress" and its focus timer runs.
static void list_select(ClickRecognizerRef rec, void *ctx) {
  app_touched();
  if (s_list.sel >= s_row_count) return;
  Row row = s_rows[s_list.sel];
  if (row.kind == ROW_SESSION) ui_show_session();
  else app_start_task(g_tasks.items[row.task].id);
}

static void list_select_long(ClickRecognizerRef rec, void *ctx) {
  app_touched();
  app_request_refresh();
}

static void list_click_config(void *ctx) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, REPEAT_MS, list_up);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, REPEAT_MS, list_down);
  window_single_click_subscribe(BUTTON_ID_SELECT, list_select);
  window_long_click_subscribe(BUTTON_ID_SELECT, 0, list_select_long, NULL);
}

static void list_window_load(Window *window) {
  drum_create(&s_list, window, &s_list_status_layer, list_status_update);
}

static void list_window_unload(Window *window) {
  drum_destroy(&s_list, &s_list_status_layer);
}

// ---- Header screens: a coloured header with a title over a white body ------------

// Draws the header; returns its height.
static int16_t draw_header(GContext *ctx, GRect b, const char *title, GColor color) {
  int16_t inset = PBL_IF_ROUND_ELSE(22, 6), y = PBL_IF_ROUND_ELSE(16, 4), h = y + 36;
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  graphics_context_set_fill_color(ctx, color);
  graphics_fill_rect(ctx, GRect(0, 0, b.size.w, h), 0, GCornerNone);
  graphics_context_set_text_color(ctx, text_on(color));
  graphics_draw_text(ctx, title, s_f24b, GRect(inset, y, b.size.w - 2 * inset, 30), GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);
  return h;
}

// ---- Session window -----------------------------------------------------------

static void session_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  char text[24];
  format_phase(text, sizeof text);
  GColor color = phase_color();
  int16_t hh = draw_header(ctx, b, g_session.phase == NM_PHASE_IDLE ? "" : text, color);
  if (g_session.phase == NM_PHASE_IDLE) return;

  int16_t inset = PBL_IF_ROUND_ELSE(24, 6), w = b.size.w - 2 * inset;
  // the countdown, the task and a hint, spread over the room below the header
  int16_t room = b.size.h - hh - PBL_IF_ROUND_ELSE(14, 0), gap = (room - 42 - 40 - 14) / 4;
  if (gap < 0) gap = 0;
  int16_t y = hh + gap;
  graphics_context_set_text_color(ctx, GColorBlack);
  format_countdown(text, sizeof text);
  graphics_draw_text(ctx, text, s_leco, GRect(0, y - 6, b.size.w, 48), GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  y += 42 + gap;
  graphics_draw_text(ctx, app_session_title(), s_f18b, GRect(inset, y - 6, w, 44), GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);
  y += 40 + gap;
  const char *hint = g_session.paused ? "" : g_session.phase != NM_PHASE_FOCUS ? "rest" : g_session.active ? "keep moving" : "";
  graphics_draw_text(ctx, hint, s_f14, GRect(inset, y - 4, w, 18), GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  fill_span(ctx, color, 0, b.size.w, b.size.h - BAR_H, b.size.h);
}

static void show_menu(void);

static void session_select(ClickRecognizerRef rec, void *ctx) {
  app_touched();
  show_menu();
}

static void session_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, session_select);
}

static void session_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_session_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_session_layer, session_update);
  layer_add_child(root, s_session_layer);
}

static void session_window_unload(Window *window) {
  layer_destroy(s_session_layer);
  s_session_layer = NULL;
}

// ---- Session actions: a drum of their own --------------------------------------

static void build_actions(void) {
  s_action_count = 0;
  if (g_session.phase == NM_PHASE_IDLE) return;
  s_actions[s_action_count++] = ACT_PAUSE;
  if (g_session.task_id) s_actions[s_action_count++] = ACT_DONE;
  s_actions[s_action_count++] = ACT_SKIP;
  s_actions[s_action_count++] = ACT_STOP;
}

static int menu_count(void) {
  return s_action_count;
}

static void menu_row(int index, RowView *v) {
  if (index < 0 || index >= s_action_count) return;
  v->color = phase_color();
  switch (s_actions[index]) {
    case ACT_PAUSE: v->title = g_session.paused ? "Resume" : "Pause"; break;
    case ACT_DONE: v->title = "Task done"; break;
    case ACT_SKIP: v->title = g_session.phase == NM_PHASE_FOCUS ? "Skip to break" : "Skip break"; break;
    default: v->title = "Stop"; break;
  }
}

static void menu_status_update(Layer *layer, GContext *ctx) {
  draw_status(layer, ctx, app_session_title());
}

static void menu_up(ClickRecognizerRef rec, void *ctx) { drum_move(&s_menu, -1); }
static void menu_down(ClickRecognizerRef rec, void *ctx) { drum_move(&s_menu, 1); }

static void menu_select(ClickRecognizerRef rec, void *ctx) {
  if (s_menu.sel >= s_action_count) return;
  Action action = s_actions[s_menu.sel];
  uint32_t task = g_session.task_id;
  window_stack_remove(s_menu_window, false);
  switch (action) {
    case ACT_PAUSE: app_toggle_pause(); break;
    case ACT_DONE: app_task_done(task); break;
    case ACT_SKIP: app_skip_phase(); break;
    default: app_stop_session(); break;
  }
}

static void menu_click_config(void *ctx) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, REPEAT_MS, menu_up);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, REPEAT_MS, menu_down);
  window_single_click_subscribe(BUTTON_ID_SELECT, menu_select);
}

static void menu_window_load(Window *window) {
  s_menu.sel = 0;
  drum_create(&s_menu, window, &s_menu_status_layer, menu_status_update);
}

static void menu_window_unload(Window *window) {
  drum_destroy(&s_menu, &s_menu_status_layer);
}

static void show_menu(void) {
  build_actions();
  if (s_action_count == 0 || window_stack_contains_window(s_menu_window)) return;
  window_stack_push(s_menu_window, true);
}

// ---- Nudge window -------------------------------------------------------------

static void nudge_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  GColor color = PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorBlack);
  int16_t hh = draw_header(ctx, b, "Still sitting?", color);
  int16_t inset = PBL_IF_ROUND_ELSE(24, 6), w = b.size.w - 2 * inset;
  int16_t room = b.size.h - hh - PBL_IF_ROUND_ELSE(14, 0), gap = (room - 14 - 52 - 14) / 4;
  if (gap < 0) gap = 0;
  int16_t y = hh + gap;
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, "Back to", s_f14, GRect(inset, y - 4, w, 18), GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  y += 14 + gap;
  graphics_draw_text(ctx, app_session_title(), s_f24b, GRect(inset, y - 8, w, 58), GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);
  y += 52 + gap;
  graphics_draw_text(ctx, "any button: on it", s_f14, GRect(inset, y - 4, w, 18), GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);
  fill_span(ctx, color, 0, b.size.w, b.size.h - BAR_H, b.size.h);
}

static void nudge_dismiss(ClickRecognizerRef rec, void *ctx) {
  app_touched();
  window_stack_remove(s_nudge_window, true);
}

static void nudge_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_UP, nudge_dismiss);
  window_single_click_subscribe(BUTTON_ID_SELECT, nudge_dismiss);
  window_single_click_subscribe(BUTTON_ID_DOWN, nudge_dismiss);
}

static void nudge_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_nudge_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_nudge_layer, nudge_update);
  layer_add_child(root, s_nudge_layer);
}

static void nudge_window_unload(Window *window) {
  layer_destroy(s_nudge_layer);
  s_nudge_layer = NULL;
  app_nudge_answered();
}

// ---- Shared -------------------------------------------------------------------

void ui_tick(void) {
  if (s_list.layer) layer_mark_dirty(s_list.layer);
  if (s_list_status_layer) layer_mark_dirty(s_list_status_layer);
  if (s_session_layer) layer_mark_dirty(s_session_layer);
}

void ui_refresh(void) {
  build_rows();
  s_list.empty_title = g_status[0] ? g_status : "All clear";
  s_list.empty_hint = "Hold SELECT to refresh";
  if (s_list.layer) drum_reload(&s_list);
  // the session's actions depend on its state: a menu left open over a changed session closes
  if (window_stack_contains_window(s_menu_window)) window_stack_remove(s_menu_window, false);
  if (s_nudge_layer) layer_mark_dirty(s_nudge_layer);
  ui_tick();
}

void ui_show_session(void) {
  if (g_session.phase == NM_PHASE_IDLE || window_stack_contains_window(s_session_window)) return;
  window_stack_push(s_session_window, true);
}

void ui_hide_session(void) {
  window_stack_remove(s_nudge_window, false);
  window_stack_remove(s_menu_window, false);
  window_stack_remove(s_session_window, true);
}

bool ui_nudge_showing(void) {
  return window_stack_contains_window(s_nudge_window);
}

void ui_show_nudge(void) {
  if (window_stack_contains_window(s_nudge_window)) return;
  window_stack_push(s_nudge_window, true);
}

static Window *make_window(WindowHandler load, WindowHandler unload, ClickConfigProvider clicks) {
  Window *window = window_create();
  window_set_click_config_provider(window, clicks);
  window_set_window_handlers(window, (WindowHandlers){ .load = load, .unload = unload });
  return window;
}

void ui_init(void) {
  s_f14 = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  s_f14b = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  s_f18 = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  s_f18b = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  s_f24b = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  s_leco = fonts_get_system_font(FONT_KEY_LECO_42_NUMBERS);
  s_list.count = list_count;
  s_list.row = list_row;
  s_menu.count = menu_count;
  s_menu.row = menu_row;
  s_menu.empty_title = s_menu.empty_hint = "";
  s_list_window = make_window(list_window_load, list_window_unload, list_click_config);
  s_session_window = make_window(session_window_load, session_window_unload, session_click_config);
  s_menu_window = make_window(menu_window_load, menu_window_unload, menu_click_config);
  s_nudge_window = make_window(nudge_window_load, nudge_window_unload, nudge_click_config);
  ui_refresh();
  window_stack_push(s_list_window, true);
}

void ui_deinit(void) {
  window_destroy(s_nudge_window);
  window_destroy(s_menu_window);
  window_destroy(s_session_window);
  window_destroy(s_list_window);
}
