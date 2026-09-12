#include "assistant.h"

// ---------------------------------------------------------------------------
// The reply view: one conversation turn at a time.
//
// Scrolling past the top or bottom edge moves to the adjacent turn, and so does
// a double-tap of UP/DOWN.  Both gestures animate the outgoing turn off screen
// in the direction of travel and slide the incoming one in behind it, which is
// what makes a multi-turn conversation feel like a single scrollable thing
// rather than a series of unrelated screens.
// ---------------------------------------------------------------------------

#define PAD            5
#define TICK_MS        33
#define SCROLL_MS      190
#define SLIDE_OUT_MS   130
#define SLIDE_IN_MS    200
#define BOUNCE_MS      230
#define BOUNCE_PX      9
#define TURN_TIMEOUT_MS 5000

static Window *s_window;
static Layer *s_canvas;
static bool s_loaded;
static bool s_visible;

static ReplyState s_state = REPLY_BUSY;
static Turn s_turn;
static char s_status[MAX_STATUS_LEN] = "Thinking";
static int32_t s_spinner = SPIN_THINKING;
static int32_t s_alert_cookie = -1;

static int16_t s_content_h;
static int16_t s_view_h = 120;
static int16_t s_q_h;

// Everything below is driven off s_phase, a monotonic tick counter, so the
// animations never depend on wall-clock time.
static uint32_t s_phase;
static AppTimer *s_tick;

static int16_t s_scroll, s_scroll_from, s_scroll_to;
static uint32_t s_scroll_t0;
static bool s_scrolling;

static int16_t s_slide, s_slide_from, s_slide_to;
static uint32_t s_slide_t0;
static uint16_t s_slide_ms;
static bool s_sliding;

static int16_t s_bounce;
static uint32_t s_bounce_t0;
static int8_t s_bounce_dir;
static bool s_bouncing;

static int8_t s_pending_dir;
static bool s_awaiting_turn;
static uint32_t s_await_t0;

static void ensure_tick(void);
static void recompute_layout(void);

// --- small helpers ----------------------------------------------------------

static uint32_t elapsed_ms(uint32_t start) { return (s_phase - start) * TICK_MS; }

static int16_t max_scroll(void) {
  int16_t m = s_content_h - s_view_h;
  return m > 0 ? m : 0;
}

static int16_t scroll_step(void) {
  int16_t step = (int16_t)((s_view_h * 3) / 4);
  return step < 20 ? 20 : step;
}

ReplyState reply_window_state(void) { return s_state; }

bool reply_window_has_content(void) {
  return s_turn.answer[0] != '\0' || s_turn.question[0] != '\0';
}

static void ensure_pushed(void) {
  if (s_window && !s_loaded) window_stack_push(s_window, true);
}

// --- drawing ----------------------------------------------------------------

static void draw_spinner(GContext *ctx, GPoint center, int16_t radius) {
  const Theme *t = theme();
  GRect box = GRect(center.x - radius, center.y - radius, radius * 2, radius * 2);
  int32_t sweep = (s_spinner == SPIN_SEARCHING) ? 150 : 100;
  int32_t start = (int32_t)((s_phase * 11) % 360);

  graphics_context_set_fill_color(ctx, t->accent_dim);
  graphics_fill_radial(ctx, box, GOvalScaleModeFitCircle, 3,
                       DEG_TO_TRIGANGLE(0), DEG_TO_TRIGANGLE(360));
  graphics_context_set_fill_color(ctx, t->accent);
  graphics_fill_radial(ctx, box, GOvalScaleModeFitCircle, 3,
                       DEG_TO_TRIGANGLE(start), DEG_TO_TRIGANGLE(start + sweep));

  // A tool call gets a second, slower counter-rotating arc so the three kinds of
  // wait are distinguishable at a glance.
  if (s_spinner == SPIN_TOOL) {
    GRect inner = grect_inset(box, GEdgeInsets(5));
    int32_t back = (int32_t)(360 - (s_phase * 7) % 360);
    graphics_fill_radial(ctx, inner, GOvalScaleModeFitCircle, 2,
                         DEG_TO_TRIGANGLE(back), DEG_TO_TRIGANGLE(back + 70));
  }
}

static void draw_edge_hint(GContext *ctx, GRect b, bool up) {
  const Theme *t = theme();
  int16_t cx = b.size.w / 2;
  int16_t y = up ? (theme_header_height() + 2) : (b.size.h - 5);
  graphics_context_set_stroke_color(ctx, t->accent_dim);
  for (int i = 0; i < 3; i++) {
    int16_t dy = up ? i : -i;
    graphics_draw_line(ctx, GPoint(cx - 4 + i, y + dy), GPoint(cx + 4 - i, y + dy));
  }
}

static void draw_scrollbar(GContext *ctx, GRect b) {
  int16_t max = max_scroll();
  if (max <= 0) return;
  const Theme *t = theme();
  int16_t top = theme_header_height() + 2;
  int16_t track_h = b.size.h - top - 4;
  if (track_h <= 12) return;
  int16_t thumb_h = (int16_t)((int32_t)track_h * s_view_h / s_content_h);
  if (thumb_h < 10) thumb_h = 10;
  int16_t travel = track_h - thumb_h;
  int16_t clamped = s_scroll < 0 ? 0 : (s_scroll > max ? max : s_scroll);
  int16_t thumb_y = top + (int16_t)((int32_t)travel * clamped / max);

  graphics_context_set_fill_color(ctx, t->surface);
  graphics_fill_rect(ctx, GRect(b.size.w - 4, top, 2, track_h), 1, GCornersAll);
  graphics_context_set_fill_color(ctx, t->accent);
  graphics_fill_rect(ctx, GRect(b.size.w - 5, thumb_y, 4, thumb_h), 2, GCornersAll);
}

static void draw_conversation(GContext *ctx, GRect b) {
  const Theme *t = theme();
  int16_t w = b.size.w - 2 * PAD;
  int16_t y = theme_header_height() + PAD - s_scroll + s_slide + s_bounce;

  if (s_turn.question[0]) {
    graphics_context_set_text_color(ctx, t->question);
    graphics_draw_text(ctx, s_turn.question, theme_font_question(),
                       GRect(PAD, y, w, s_q_h + 4),
                       GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
    y += s_q_h + 4;
    graphics_context_set_stroke_color(ctx, t->accent_dim);
    graphics_draw_line(ctx, GPoint(PAD, y + 2), GPoint(PAD + w / 3, y + 2));
    y += 8;
  }
  if (s_turn.answer[0]) {
    graphics_context_set_text_color(ctx, t->text);
    graphics_draw_text(ctx, s_turn.answer, theme_font_body(),
                       GRect(PAD, y, w, s_content_h + 40),
                       GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
  }

  draw_scrollbar(ctx, b);
  if (s_turn.index > 0 && s_scroll <= 0) draw_edge_hint(ctx, b, true);
  if (s_turn.index + 1 < s_turn.count && s_scroll >= max_scroll()) draw_edge_hint(ctx, b, false);

  char badge[24];
  if (s_turn.count > 1) {
    snprintf(badge, sizeof(badge), "%d/%d", (int)s_turn.index + 1, (int)s_turn.count);
  } else {
    badge[0] = '\0';
  }
  theme_draw_header(ctx, b, s_turn.title[0] ? s_turn.title : comm_model_label(), badge);
}

static void draw_busy(GContext *ctx, GRect b) {
  const Theme *t = theme();
  int16_t w = b.size.w - 2 * PAD;
  int16_t y = theme_header_height() + PAD;

  if (s_turn.question[0]) {
    graphics_context_set_text_color(ctx, t->question);
    GSize q = graphics_text_layout_get_content_size(s_turn.question, theme_font_question(),
                  GRect(0, 0, w, 2000), GTextOverflowModeWordWrap, GTextAlignmentLeft);
    int16_t cap = b.size.h / 3;
    int16_t qh = q.h > cap ? cap : q.h;
    graphics_draw_text(ctx, s_turn.question, theme_font_question(),
                       GRect(PAD, y, w, qh),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    y += qh + 6;
  }

  int16_t radius = 15;
  int16_t remaining = b.size.h - y;
  GPoint center = GPoint(b.size.w / 2, y + remaining / 2 - 12);
  draw_spinner(ctx, center, radius);

  graphics_context_set_text_color(ctx, t->text_dim);
  graphics_draw_text(ctx, s_status, theme_font_small(),
                     GRect(PAD, center.y + radius + 4, w, 40),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  theme_draw_header(ctx, b, comm_model_label(), NULL);
}

static void draw_error(GContext *ctx, GRect b) {
  const Theme *t = theme();
  int16_t w = b.size.w - 2 * PAD;
  int16_t y = theme_header_height() + 10;

  graphics_context_set_fill_color(ctx, t->danger);
  graphics_fill_circle(ctx, GPoint(b.size.w / 2, y + 9), 9);
  graphics_context_set_fill_color(ctx, t->background);
  graphics_fill_rect(ctx, GRect(b.size.w / 2 - 1, y + 4, 2, 7), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(b.size.w / 2 - 1, y + 13, 2, 2), 0, GCornerNone);
  y += 26;

  graphics_context_set_text_color(ctx, t->text);
  graphics_draw_text(ctx, s_status, theme_font_body(),
                     GRect(PAD, y, w, b.size.h - y - 18),
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  graphics_context_set_text_color(ctx, t->text_dim);
  graphics_draw_text(ctx, "SELECT retry", theme_font_small(),
                     GRect(PAD, b.size.h - 18, w, 18),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  theme_draw_header(ctx, b, "Problem", NULL);
}

static void draw_alert(GContext *ctx, GRect b) {
  const Theme *t = theme();
  int16_t w = b.size.w - 2 * PAD;

  // Slow breathing ring so a reminder reads as urgent without a busy spinner.
  int32_t pulse = (int32_t)((s_phase / 2) % 40);
  int16_t grow = (int16_t)(pulse < 20 ? pulse : 40 - pulse);
  int16_t radius = 13 + grow / 3;
  GPoint center = GPoint(b.size.w / 2, theme_header_height() + 26);
  graphics_context_set_fill_color(ctx, t->accent);
  graphics_fill_radial(ctx, GRect(center.x - radius, center.y - radius, radius * 2, radius * 2),
                       GOvalScaleModeFitCircle, 4, DEG_TO_TRIGANGLE(0), DEG_TO_TRIGANGLE(360));

  int16_t y = center.y + radius + 8;
  graphics_context_set_text_color(ctx, t->text);
  graphics_draw_text(ctx, s_status, theme_font_body(),
                     GRect(PAD, y, w, b.size.h - y - 20),
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  graphics_context_set_text_color(ctx, t->text_dim);
  graphics_draw_text(ctx, "SELECT ok  UP +9m", theme_font_small(),
                     GRect(PAD, b.size.h - 18, w, 18),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  theme_draw_header(ctx, b, "Reminder", NULL);
}

static void canvas_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, theme()->background);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  switch (s_state) {
    case REPLY_SHOW:  draw_conversation(ctx, b); break;
    case REPLY_ERROR: draw_error(ctx, b);        break;
    case REPLY_ALERT: draw_alert(ctx, b);        break;
    case REPLY_BUSY:
    default:          draw_busy(ctx, b);         break;
  }
  toast_draw(ctx, b);
}

// --- animation --------------------------------------------------------------

static void tick_cb(void *data) {
  s_tick = NULL;
  s_phase++;
  bool busy = false;

  if (s_scrolling) {
    uint32_t e = elapsed_ms(s_scroll_t0);
    s_scroll = s_scroll_from + (int16_t)ease_out_cubic((int32_t)e, SCROLL_MS,
                                                       s_scroll_to - s_scroll_from);
    if (e >= SCROLL_MS) { s_scroll = s_scroll_to; s_scrolling = false; }
    busy = busy || s_scrolling;
  }

  if (s_sliding) {
    uint32_t e = elapsed_ms(s_slide_t0);
    s_slide = s_slide_from + (int16_t)ease_out_cubic((int32_t)e, s_slide_ms,
                                                     s_slide_to - s_slide_from);
    if (e >= s_slide_ms) { s_slide = s_slide_to; s_sliding = false; }
    busy = busy || s_sliding;
  }

  if (s_bouncing) {
    uint32_t e = elapsed_ms(s_bounce_t0);
    int16_t peak = (int16_t)(-s_bounce_dir * BOUNCE_PX);
    if (e < BOUNCE_MS / 2) {
      s_bounce = (int16_t)ease_out_cubic((int32_t)e, BOUNCE_MS / 2, peak);
    } else if (e < BOUNCE_MS) {
      s_bounce = peak - (int16_t)ease_out_cubic((int32_t)(e - BOUNCE_MS / 2),
                                                BOUNCE_MS / 2, peak);
    } else {
      s_bounce = 0; s_bouncing = false;
    }
    busy = busy || s_bouncing;
  }

  // If the phone never answers a turn request, restore the outgoing turn rather
  // than leaving the user on a blank screen.
  if (s_awaiting_turn) {
    if (elapsed_ms(s_await_t0) > TURN_TIMEOUT_MS) {
      s_awaiting_turn = false;
      s_pending_dir = 0;
      s_slide_from = s_slide; s_slide_to = 0;
      s_slide_t0 = s_phase; s_slide_ms = SLIDE_IN_MS; s_sliding = true;
      toast_show("No answer");
    }
    busy = true;
  }

  if (s_state == REPLY_BUSY || s_state == REPLY_ALERT) busy = true;
  if (toast_active()) busy = true;

  if (s_canvas) layer_mark_dirty(s_canvas);
  if (busy && s_visible) s_tick = app_timer_register(TICK_MS, tick_cb, NULL);
}

static void ensure_tick(void) {
  if (!s_tick && s_visible) s_tick = app_timer_register(TICK_MS, tick_cb, NULL);
}

static void scroll_to(int16_t target) {
  int16_t max = max_scroll();
  if (target < 0) target = 0;
  if (target > max) target = max;
  if (target == s_scroll_to && !s_scrolling) return;
  s_scroll_from = s_scroll;
  s_scroll_to = target;
  s_scroll_t0 = s_phase;
  s_scrolling = true;
  ensure_tick();
}

static void bounce(int8_t dir) {
  s_bounce_dir = dir;
  s_bounce_t0 = s_phase;
  s_bouncing = true;
  vibe_bump();
  ensure_tick();
}

static void goto_turn(int8_t dir) {
  if (s_awaiting_turn) return;
  int32_t target = s_turn.index + dir;
  if (target < 0 || target >= s_turn.count) { bounce(dir); return; }

  s_pending_dir = dir;
  s_awaiting_turn = true;
  s_await_t0 = s_phase;
  s_scrolling = false;
  s_slide_from = s_slide;
  s_slide_to = (int16_t)(-dir * s_view_h);
  s_slide_t0 = s_phase;
  s_slide_ms = SLIDE_OUT_MS;
  s_sliding = true;
  vibe_bump();
  comm_send(WREQ_GET_TURN, NULL, target, 0);
  ensure_tick();
}

static void recompute_layout(void) {
  if (!s_canvas) return;
  GRect b = layer_get_bounds(s_canvas);
  int16_t w = b.size.w - 2 * PAD;
  s_view_h = b.size.h - theme_header_height();

  GRect box = GRect(0, 0, w, 2000);
  int16_t h = PAD;
  s_q_h = 0;
  if (s_turn.question[0]) {
    s_q_h = graphics_text_layout_get_content_size(s_turn.question, theme_font_question(), box,
                GTextOverflowModeWordWrap, GTextAlignmentLeft).h;
    h += s_q_h + 12;
  }
  if (s_turn.answer[0]) {
    h += graphics_text_layout_get_content_size(s_turn.answer, theme_font_body(), box,
             GTextOverflowModeWordWrap, GTextAlignmentLeft).h;
  }
  s_content_h = h + 8;
}

// --- public state transitions ----------------------------------------------

void reply_window_set_question(const char *text) {
  memset(&s_turn, 0, sizeof(s_turn));
  str_copy(s_turn.question, MAX_QUESTION_LEN, text);
  s_turn.count = 1;
  s_scroll = s_scroll_to = 0;
  s_slide = 0;
  s_awaiting_turn = false;
  recompute_layout();
}

void reply_window_set_status(const char *text, int32_t spinner) {
  str_copy(s_status, MAX_STATUS_LEN, (text && text[0]) ? text : "Thinking");
  s_spinner = spinner;
  s_state = REPLY_BUSY;
  ensure_pushed();
  ensure_tick();
  if (s_canvas) layer_mark_dirty(s_canvas);
}

void reply_window_set_error(const char *text) {
  str_copy(s_status, MAX_STATUS_LEN, (text && text[0]) ? text : "Something went wrong");
  s_state = REPLY_ERROR;
  s_awaiting_turn = false;
  s_sliding = false;
  s_slide = 0;
  ensure_pushed();
  vibe_soft();
  ensure_tick();
  if (s_canvas) layer_mark_dirty(s_canvas);
}

void reply_window_set_alert(const char *title, int32_t cookie) {
  str_copy(s_status, MAX_STATUS_LEN, (title && title[0]) ? title : "Reminder");
  s_alert_cookie = cookie;
  s_state = REPLY_ALERT;
  ensure_pushed();
  ensure_tick();
  if (s_canvas) layer_mark_dirty(s_canvas);
}

void reply_window_show_turn(const Turn *turn) {
  if (!turn) return;
  int8_t dir = s_awaiting_turn ? s_pending_dir : 0;
  s_awaiting_turn = false;
  s_pending_dir = 0;

  memcpy(&s_turn, turn, sizeof(Turn));
  s_state = REPLY_SHOW;
  s_scroll = s_scroll_to = 0;
  s_scrolling = false;
  s_bouncing = false;
  s_bounce = 0;
  recompute_layout();

  // Slide the new turn in from the direction of travel; a freshly generated
  // answer rises gently from below instead.
  s_slide_from = dir ? (int16_t)(dir * s_view_h) : (int16_t)(s_view_h / 4);
  s_slide = s_slide_from;
  s_slide_to = 0;
  s_slide_t0 = s_phase;
  s_slide_ms = SLIDE_IN_MS;
  s_sliding = true;

  ensure_pushed();
  if (!dir) vibe_soft();
  ensure_tick();
  if (s_canvas) layer_mark_dirty(s_canvas);
}

void reply_window_return(void) {
  if (!reply_window_has_content()) return;
  ensure_pushed();
}

// --- buttons ----------------------------------------------------------------

static void select_click(ClickRecognizerRef recognizer, void *context) {
  if (s_state == REPLY_ALERT) {
    s_alert_cookie = -1;
    s_state = reply_window_has_content() ? REPLY_SHOW : REPLY_BUSY;
    if (s_state == REPLY_BUSY) { window_stack_pop(true); return; }
    recompute_layout();
    layer_mark_dirty(s_canvas);
    return;
  }
  dictation_start();
}

static void select_long(ClickRecognizerRef recognizer, void *context) {
  if (s_state == REPLY_ALERT) return;   // the buttons belong to the reminder
  list_open(LIST_CHAT_ACTIONS);
}

static void up_click(ClickRecognizerRef recognizer, void *context) {
  if (s_state == REPLY_ALERT) {
    comm_send(WREQ_SNOOZE, NULL, s_alert_cookie, 9);
    toast_show("Snoozed 9 min");
    s_state = reply_window_has_content() ? REPLY_SHOW : REPLY_BUSY;
    if (s_state == REPLY_BUSY) window_stack_pop(true);
    return;
  }
  if (s_state != REPLY_SHOW || s_awaiting_turn) return;
  if (s_scroll_to <= 0) goto_turn(-1);
  else scroll_to(s_scroll_to - scroll_step());
}

static void down_click(ClickRecognizerRef recognizer, void *context) {
  if (s_state == REPLY_ALERT) return;
  if (s_state != REPLY_SHOW || s_awaiting_turn) return;
  if (s_scroll_to >= max_scroll()) goto_turn(1);
  else scroll_to(s_scroll_to + scroll_step());
}

static void up_double(ClickRecognizerRef recognizer, void *context) {
  if (s_state != REPLY_SHOW) return;
  goto_turn(-1);
}

static void down_double(ClickRecognizerRef recognizer, void *context) {
  if (s_state != REPLY_SHOW) return;
  goto_turn(1);
}

static void back_click(ClickRecognizerRef recognizer, void *context) {
  if (s_state == REPLY_BUSY) comm_send(WREQ_CANCEL, NULL, 0, 0);
  window_stack_pop(true);
}

static void click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, select_long, NULL);
  window_single_click_subscribe(BUTTON_ID_UP, up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click);
  window_multi_click_subscribe(BUTTON_ID_UP, 2, 2, 260, true, up_double);
  window_multi_click_subscribe(BUTTON_ID_DOWN, 2, 2, 260, true, down_double);
  window_single_click_subscribe(BUTTON_ID_BACK, back_click);
}

// --- window lifecycle -------------------------------------------------------

static void window_load(Window *window) {
  s_loaded = true;
  Layer *root = window_get_root_layer(window);
  s_canvas = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_canvas, canvas_update);
  layer_add_child(root, s_canvas);
  recompute_layout();
}

static void window_unload(Window *window) {
  s_loaded = false;
  if (s_canvas) { layer_destroy(s_canvas); s_canvas = NULL; }
}

static void window_appear(Window *window) {
  s_visible = true;
  ensure_tick();
}

static void window_disappear(Window *window) {
  s_visible = false;
  if (s_tick) { app_timer_cancel(s_tick); s_tick = NULL; }
}

void reply_window_init(void) {
  s_window = window_create();
  window_set_background_color(s_window, theme()->background);
  window_set_click_config_provider(s_window, click_config);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
    .appear = window_appear,
    .disappear = window_disappear,
  });
}

void reply_window_deinit(void) {
  if (s_tick) { app_timer_cancel(s_tick); s_tick = NULL; }
  if (s_window) { window_destroy(s_window); s_window = NULL; }
}

void reply_window_hide(void) {
  if (s_window && s_loaded) window_stack_remove(s_window, true);
}

// The conversation is gone, so there is nothing to come back to: drop the turn
// as well as the window, or scrolling off the chats list would resurrect it.
void reply_window_forget(void) {
  memset(&s_turn, 0, sizeof(s_turn));
  s_scroll = s_scroll_to = 0;
  s_slide = 0;
  s_scrolling = false;
  s_sliding = false;
  s_awaiting_turn = false;
  s_state = REPLY_BUSY;
  reply_window_hide();
}
