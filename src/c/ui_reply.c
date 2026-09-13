#include "assistant.h"

// ---------------------------------------------------------------------------
// The reply view: one conversation turn at a time.
//
// The turn sits in a stock ScrollLayer, which owns UP and DOWN.  An earlier
// version scrolled by hand so that running off either end could carry you into
// the neighbouring turn, but hand-rolled scrolling never feels like the rest of
// the watch no matter how carefully it is tuned -- and matching the platform
// matters more here than the gesture did.  Moving between turns lives in the
// options list behind a long press of SELECT.
// ---------------------------------------------------------------------------

#define PAD            5
#define TICK_MS        33
#define TURN_TIMEOUT_MS 5000

static Window *s_window;
static Layer *s_canvas;        // background, header, and the non-scrolling states
static ScrollLayer *s_scroller;
static TextLayer *s_question;  // the turn, inside the scroller
static TextLayer *s_answer;
static Layer *s_rule;          // the hairline between the two
static Layer *s_arrow_up;      // where the ContentIndicator paints its arrows
static Layer *s_arrow_down;
static Layer *s_overlay;       // toasts, above everything
static bool s_loaded;
static bool s_visible;

static ReplyState s_state = REPLY_BUSY;
static Turn s_turn;
static char s_status[MAX_STATUS_LEN] = "Thinking";
static int32_t s_spinner = SPIN_THINKING;
static int32_t s_alert_cookie = -1;

static int16_t s_content_h;
static int16_t s_q_h;

// Everything below is driven off s_phase, a monotonic tick counter, so the
// animations never depend on wall-clock time.
static uint32_t s_phase;
static AppTimer *s_tick;

static bool s_title_scrolling;
static uint32_t s_marquee_t0;
static bool s_awaiting_turn;
static uint32_t s_await_t0;

static void ensure_tick(void);
static void recompute_layout(void);
static void apply_click_config(void);
static void update_arrows(void);

// --- small helpers ----------------------------------------------------------

static uint32_t elapsed_ms(uint32_t start) { return (s_phase - start) * TICK_MS; }

static GRect scroll_frame(void) {
  GRect b = layer_get_bounds(s_canvas);
  int16_t top = theme_header_height();
  return GRect(0, top, b.size.w, b.size.h - top);
}

ReplyState reply_window_state(void) { return s_state; }

// Which turn the user is looking at.  Only a settled conversation counts: a
// question still in flight has nothing to go back to.
int32_t reply_window_turn_index(void) {
  if (s_state != REPLY_SHOW) return -1;
  if (!s_turn.answer[0] && !s_turn.question[0]) return -1;
  return s_turn.index;
}

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

// All that is left to draw by hand between the question and the answer.
static void rule_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_stroke_color(ctx, theme()->accent_dim);
  graphics_draw_line(ctx, GPoint(0, b.size.h / 2), GPoint(b.size.w / 3, b.size.h / 2));
}

// In this state the canvas draws the header and nothing else: the turn belongs
// to the scroller layered over it.
static void draw_conversation(GContext *ctx, GRect b) {
  char badge[24];
  if (s_turn.count > 1) {
    snprintf(badge, sizeof(badge), "%d/%d", (int)s_turn.index + 1, (int)s_turn.count);
  } else {
    badge[0] = '\0';
  }
  s_title_scrolling = theme_draw_header(ctx, b,
      s_turn.title[0] ? s_turn.title : comm_model_label(), badge,
      /*show_clock*/ false, (s_phase - s_marquee_t0) * TICK_MS);
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

  s_title_scrolling = theme_draw_header(ctx, b, comm_model_label(), NULL, false,
                                        (s_phase - s_marquee_t0) * TICK_MS);
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

  s_title_scrolling = theme_draw_header(ctx, b, "Problem", NULL, false, 0);
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

  s_title_scrolling = theme_draw_header(ctx, b, "Reminder", NULL, false, 0);
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
}

static void overlay_update(Layer *layer, GContext *ctx) {
  toast_draw(ctx, layer_get_bounds(layer));
}

// --- animation --------------------------------------------------------------

static void tick_cb(void *data) {
  s_tick = NULL;
  s_phase++;
  bool busy = false;

  // If the phone never answers a turn request, say so rather than leaving the
  // user looking at a turn that is no longer the one they asked for.
  if (s_awaiting_turn) {
    if (elapsed_ms(s_await_t0) > TURN_TIMEOUT_MS) {
      s_awaiting_turn = false;
      toast_show("No answer");
    }
    busy = true;
  }

  if (s_state == REPLY_BUSY || s_state == REPLY_ALERT) busy = true;
  if (toast_active() || s_title_scrolling) busy = true;

  if (s_canvas) layer_mark_dirty(s_canvas);
  if (s_overlay) layer_mark_dirty(s_overlay);
  if (busy && s_visible) s_tick = app_timer_register(TICK_MS, tick_cb, NULL);
}

static void ensure_tick(void) {
  if (!s_tick && s_visible) s_tick = app_timer_register(TICK_MS, tick_cb, NULL);
}

static void scroll_to_top(void) {
  if (s_scroller) scroll_layer_set_content_offset(s_scroller, GPointZero, false);
  update_arrows();
}

static void goto_turn(int8_t dir) {
  if (s_state != REPLY_SHOW || s_awaiting_turn) return;
  int32_t target = s_turn.index + dir;
  if (target < 0 || target >= s_turn.count) {
    vibe_bump();
    toast_show(dir < 0 ? "First turn" : "Last turn");
    ensure_tick();
    return;
  }
  s_awaiting_turn = true;
  s_await_t0 = s_phase;
  vibe_bump();
  comm_send(WREQ_GET_TURN, NULL, target, 0);
  ensure_tick();
}

// Lays the two text layers out one under the other and tells the scroller how
// tall the result is.  TextLayer will not size itself, so the measuring that was
// here before is still here -- it just feeds frames instead of draw calls.
static void recompute_layout(void) {
  if (!s_canvas || !s_scroller || !s_question || !s_answer) return;
  GRect view = scroll_frame();
  layer_set_frame(scroll_layer_get_layer(s_scroller), view);

  // TextLayer holds the pointer rather than a copy, and s_turn is where the text
  // lives for as long as it is on screen, so this only has to be re-pointed when
  // the turn changes -- which is exactly when this runs.
  text_layer_set_font(s_question, theme_font_question());
  text_layer_set_font(s_answer, theme_font_body());
  text_layer_set_text(s_question, s_turn.question);
  text_layer_set_text(s_answer, s_turn.answer);

  int16_t w = view.size.w - 2 * PAD;
  GRect box = GRect(0, 0, w, 2000);
  int16_t y = PAD;

  bool has_q = s_turn.question[0] != '\0';
  layer_set_hidden(text_layer_get_layer(s_question), !has_q);
  layer_set_hidden(s_rule, !has_q);
  if (has_q) {
    s_q_h = graphics_text_layout_get_content_size(s_turn.question, theme_font_question(), box,
                GTextOverflowModeWordWrap, GTextAlignmentLeft).h;
    layer_set_frame(text_layer_get_layer(s_question), GRect(PAD, y, w, s_q_h + 4));
    y += s_q_h + 4;
    layer_set_frame(s_rule, GRect(PAD, y, w, 8));
    y += 8;
  } else {
    s_q_h = 0;
  }

  bool has_a = s_turn.answer[0] != '\0';
  layer_set_hidden(text_layer_get_layer(s_answer), !has_a);
  if (has_a) {
    int16_t answer_h = graphics_text_layout_get_content_size(s_turn.answer, theme_font_body(), box,
                           GTextOverflowModeWordWrap, GTextAlignmentLeft).h;
    layer_set_frame(text_layer_get_layer(s_answer), GRect(PAD, y, w, answer_h + 4));
    y += answer_h;
  }

  s_content_h = y + PAD;
  // A ScrollLayer shorter than its frame still has to fill it, or the shadow
  // appears over a half-painted screen.
  if (s_content_h < view.size.h) s_content_h = view.size.h;
  scroll_layer_set_content_size(s_scroller, GSize(view.size.w, s_content_h));
  update_arrows();
}

// The stock arrows, which only appear when there is something to scroll to.
static void update_arrows(void) {
  if (!s_scroller) return;
  ContentIndicator *indicator = scroll_layer_get_content_indicator(s_scroller);
  if (!indicator) return;
  GRect view = scroll_frame();
  int16_t offset = -scroll_layer_get_content_offset(s_scroller).y;
  bool showing = s_state == REPLY_SHOW;
  content_indicator_set_content_available(indicator, ContentIndicatorDirectionUp,
                                          showing && offset > 0);
  content_indicator_set_content_available(indicator, ContentIndicatorDirectionDown,
                                          showing && offset + view.size.h < s_content_h);
}

// The scroller owns UP and DOWN whenever it is on screen; a reminder takes them
// back, because there UP means snooze.
static void update_visibility(void) {
  if (!s_scroller) return;
  layer_set_hidden(scroll_layer_get_layer(s_scroller), s_state != REPLY_SHOW);
  update_arrows();
  apply_click_config();
}

// --- public state transitions ----------------------------------------------

void reply_window_set_question(const char *text) {
  memset(&s_turn, 0, sizeof(s_turn));
  str_copy(s_turn.question, MAX_QUESTION_LEN, text);
  s_turn.count = 1;
  s_awaiting_turn = false;
  recompute_layout();
  scroll_to_top();
}

void reply_window_set_status(const char *text, int32_t spinner) {
  str_copy(s_status, MAX_STATUS_LEN, (text && text[0]) ? text : "Thinking");
  s_spinner = spinner;
  s_state = REPLY_BUSY;
  ensure_pushed();
  update_visibility();
  ensure_tick();
  if (s_canvas) layer_mark_dirty(s_canvas);
}

void reply_window_set_error(const char *text) {
  str_copy(s_status, MAX_STATUS_LEN, (text && text[0]) ? text : "Something went wrong");
  s_state = REPLY_ERROR;
  s_awaiting_turn = false;
  ensure_pushed();
  update_visibility();
  vibe_soft();
  ensure_tick();
  if (s_canvas) layer_mark_dirty(s_canvas);
}

void reply_window_set_alert(const char *title, int32_t cookie) {
  str_copy(s_status, MAX_STATUS_LEN, (title && title[0]) ? title : "Reminder");
  s_alert_cookie = cookie;
  s_state = REPLY_ALERT;
  ensure_pushed();
  update_visibility();
  ensure_tick();
  if (s_canvas) layer_mark_dirty(s_canvas);
}

void reply_window_show_turn(const Turn *turn) {
  if (!turn) return;
  bool asked_for = s_awaiting_turn;
  s_awaiting_turn = false;

  memcpy(&s_turn, turn, sizeof(Turn));
  s_state = REPLY_SHOW;
  s_marquee_t0 = s_phase;
  ensure_pushed();
  update_visibility();
  recompute_layout();
  scroll_to_top();

  // A turn fetched by hand is a navigation, already acknowledged by the press
  // that asked for it; an answer arriving on its own is news.
  if (!asked_for) vibe_soft();
  ensure_tick();
  if (s_canvas) layer_mark_dirty(s_canvas);
}

// --- buttons ----------------------------------------------------------------

static void select_click(ClickRecognizerRef recognizer, void *context) {
  if (s_state == REPLY_ALERT) {
    s_alert_cookie = -1;
    s_state = reply_window_has_content() ? REPLY_SHOW : REPLY_BUSY;
    if (s_state == REPLY_BUSY) { window_stack_pop(true); return; }
    update_visibility();
    recompute_layout();
    layer_mark_dirty(s_canvas);
    return;
  }
  dictation_start();
}

// --- the options menu -------------------------------------------------------
//
// The system's own action menu rather than a list fetched from the phone: it
// opens on the press instead of after a round trip, and the watch is the side
// that knows which turn is on screen, so it can offer a direction only when
// there is a turn that way.

static void action_performed(ActionMenu *menu, const ActionMenuItem *action, void *context) {
  int32_t act = (int32_t)(intptr_t)action_menu_item_get_action_data(action);
  switch (act) {
    case ACT_GOTO_TURN_BACK:  goto_turn(-1); break;
    case ACT_GOTO_TURN_FWD:   goto_turn(1);  break;
    case ACT_REDO_TURN: {
      // Nothing is discarded until the replacement question actually arrives,
      // so backing out of the microphone leaves the conversation untouched.
      int32_t turn = reply_window_turn_index();
      if (turn < 0) { toast_show("Nothing to redo"); break; }
      dictation_start_redo(turn);
      break;
    }
    case ACT_DELETE_CHAT:
      // The phone deletes it and answers with PEVT_DISMISS, which tears down
      // the conversation behind this menu.
      comm_send(WREQ_LIST_ACTION, NULL, ACT_DELETE_CHAT, 0);
      vibe_soft();
      break;
    default:
      break;
  }
}

// The hierarchy is built fresh on every press, so it is freed on every close.
static void action_menu_closed(ActionMenu *menu, const ActionMenuItem *performed, void *context) {
  action_menu_hierarchy_destroy(action_menu_get_root_level(menu), NULL, NULL);
}

static void select_long(ClickRecognizerRef recognizer, void *context) {
  if (s_state == REPLY_ALERT) return;   // the buttons belong to the reminder
  if (s_state != REPLY_SHOW) return;

  ActionMenuLevel *root = action_menu_level_create(4);
  if (!root) return;
  if (s_turn.index > 0) {
    action_menu_level_add_action(root, "Previous turn", action_performed,
                                 (void *)(intptr_t)ACT_GOTO_TURN_BACK);
  }
  if (s_turn.index + 1 < s_turn.count) {
    action_menu_level_add_action(root, "Next turn", action_performed,
                                 (void *)(intptr_t)ACT_GOTO_TURN_FWD);
  }
  action_menu_level_add_action(root, "Ask again", action_performed,
                               (void *)(intptr_t)ACT_REDO_TURN);
  action_menu_level_add_action(root, "Delete chat", action_performed,
                               (void *)(intptr_t)ACT_DELETE_CHAT);

  const Theme *t = theme();
  ActionMenuConfig config = {
    .root_level = root,
    .colors = { .background = t->accent, .foreground = GColorBlack },
    .align = ActionMenuAlignTop,
    .did_close = action_menu_closed,
  };
  action_menu_open(&config);
}

// UP only means something of its own in a reminder, where it snoozes.  Every
// other state leaves UP and DOWN to the ScrollLayer.
static void up_snooze(ClickRecognizerRef recognizer, void *context) {
  if (s_state != REPLY_ALERT) return;
  comm_send(WREQ_SNOOZE, NULL, s_alert_cookie, 9);
  toast_show("Snoozed 9 min");
  s_state = reply_window_has_content() ? REPLY_SHOW : REPLY_BUSY;
  if (s_state == REPLY_BUSY) { window_stack_pop(true); return; }
  update_visibility();
}

static void back_click(ClickRecognizerRef recognizer, void *context) {
  if (s_state == REPLY_BUSY) comm_send(WREQ_CANCEL, NULL, 0, 0);
  window_stack_pop(true);
}

static void scroller_moved(ScrollLayer *scroller, void *context) { update_arrows(); }

// Called by the ScrollLayer after it has claimed UP and DOWN, so this must not
// touch them.
static void scroller_clicks(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, select_long, NULL);
  window_single_click_subscribe(BUTTON_ID_BACK, back_click);
}

static void alert_clicks(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, up_snooze);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  window_single_click_subscribe(BUTTON_ID_BACK, back_click);
}

static void apply_click_config(void) {
  if (!s_window || !s_scroller) return;
  if (s_state == REPLY_ALERT) {
    window_set_click_config_provider(s_window, alert_clicks);
  } else {
    scroll_layer_set_click_config_onto_window(s_scroller, s_window);
  }
}

// --- window lifecycle -------------------------------------------------------

static void window_load(Window *window) {
  s_loaded = true;
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);

  s_canvas = layer_create(b);
  layer_set_update_proc(s_canvas, canvas_update);
  layer_add_child(root, s_canvas);

  const Theme *t = theme();
  GRect view = scroll_frame();
  s_scroller = scroll_layer_create(view);
  scroll_layer_set_shadow_hidden(s_scroller, false);
  scroll_layer_set_callbacks(s_scroller, (ScrollLayerCallbacks) {
    .click_config_provider = scroller_clicks,
    .content_offset_changed_handler = scroller_moved,
  });

  s_question = text_layer_create(GRect(PAD, 0, view.size.w - 2 * PAD, 20));
  text_layer_set_background_color(s_question, GColorClear);
  text_layer_set_text_color(s_question, t->question);
  text_layer_set_font(s_question, theme_font_question());
  text_layer_set_overflow_mode(s_question, GTextOverflowModeWordWrap);
  scroll_layer_add_child(s_scroller, text_layer_get_layer(s_question));

  s_rule = layer_create(GRect(PAD, 0, view.size.w - 2 * PAD, 8));
  layer_set_update_proc(s_rule, rule_update);
  scroll_layer_add_child(s_scroller, s_rule);

  s_answer = text_layer_create(GRect(PAD, 0, view.size.w - 2 * PAD, 20));
  text_layer_set_background_color(s_answer, GColorClear);
  text_layer_set_text_color(s_answer, t->text);
  text_layer_set_font(s_answer, theme_font_body());
  text_layer_set_overflow_mode(s_answer, GTextOverflowModeWordWrap);
  scroll_layer_add_child(s_scroller, text_layer_get_layer(s_answer));

  layer_add_child(root, scroll_layer_get_layer(s_scroller));

  // The stock arrows paint into layers of our choosing, laid over the top and
  // bottom edges of the scrolling area.
  ContentIndicator *indicator = scroll_layer_get_content_indicator(s_scroller);
  s_arrow_up = layer_create(GRect(0, view.origin.y, b.size.w, 14));
  s_arrow_down = layer_create(GRect(0, b.size.h - 14, b.size.w, 14));
  layer_add_child(root, s_arrow_up);
  layer_add_child(root, s_arrow_down);
  content_indicator_configure_direction(indicator, ContentIndicatorDirectionUp,
    &(ContentIndicatorConfig) { .layer = s_arrow_up, .times_out = false,
      .alignment = GAlignCenter,
      .colors = { .foreground = t->accent, .background = t->background } });
  content_indicator_configure_direction(indicator, ContentIndicatorDirectionDown,
    &(ContentIndicatorConfig) { .layer = s_arrow_down, .times_out = false,
      .alignment = GAlignCenter,
      .colors = { .foreground = t->accent, .background = t->background } });

  // Toasts sit above the scroller, which otherwise covers the lower half of the
  // screen and would swallow them.
  s_overlay = layer_create(b);
  layer_set_update_proc(s_overlay, overlay_update);
  layer_add_child(root, s_overlay);

  update_visibility();
  recompute_layout();
}

static void window_unload(Window *window) {
  s_loaded = false;
  if (s_overlay) { layer_destroy(s_overlay); s_overlay = NULL; }
  if (s_arrow_up) { layer_destroy(s_arrow_up); s_arrow_up = NULL; }
  if (s_arrow_down) { layer_destroy(s_arrow_down); s_arrow_down = NULL; }
  if (s_scroller) { scroll_layer_destroy(s_scroller); s_scroller = NULL; }
  if (s_question) { text_layer_destroy(s_question); s_question = NULL; }
  if (s_answer) { text_layer_destroy(s_answer); s_answer = NULL; }
  if (s_rule) { layer_destroy(s_rule); s_rule = NULL; }
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

// Text metrics depend on the font, so a text-size change has to re-measure the
// turn.  Also used by the minute tick that redraws the header clock.
void reply_window_refresh(void) {
  recompute_layout();
  if (s_canvas) layer_mark_dirty(s_canvas);
}

void reply_window_hide(void) {
  if (s_window && s_loaded) window_stack_remove(s_window, true);
}

// The conversation is gone, so there is nothing to come back to: drop the turn
// as well as the window, or scrolling off the chats list would resurrect it.
void reply_window_forget(void) {
  memset(&s_turn, 0, sizeof(s_turn));
  s_awaiting_turn = false;
  s_state = REPLY_BUSY;
  scroll_to_top();
  reply_window_hide();
}
