#include "assistant.h"

// ---------------------------------------------------------------------------
// Lists.
//
// The chats list is the app's root window: [Settings] [New Chat] [past chats...]
// with "New Chat" preselected, so cancelling dictation lands you one button away
// from trying again.  Submenus (Settings -> Model / Thinking / Quick prompts)
// stack on top of it.
//
// Only the front list's rows are held in memory; going back re-requests the
// parent from the phone, which doubles as a cheap way to keep displayed values
// (current model, toggle states) honest after a change.
// ---------------------------------------------------------------------------

#define LIST_DEPTH      3
#define TICK_MS         33
#define HL_MS           150
#define SCROLL_MS       170
#define LOAD_TIMEOUT_MS 5000

#ifdef PBL_PLATFORM_EMERY
#define ROW_BASE_H      30
#define ROW_SUB_H       17
#else
#define ROW_BASE_H      24
#define ROW_SUB_H       14
#endif

static Window *s_windows[LIST_DEPTH];
static Layer *s_canvas[LIST_DEPTH];
static int32_t s_list_ids[LIST_DEPTH];
static int s_depth;

static ListRow s_rows[MAX_LIST_ROWS];
static int16_t s_row_h[MAX_LIST_ROWS];
static int s_row_count;
static int s_sel;
static int32_t s_loaded_list = -1;
static int32_t s_incoming_list = -1;
static char s_list_title[MAX_TITLE_LEN];
static bool s_loading;

static uint32_t s_phase;
static AppTimer *s_tick;
static uint32_t s_load_t0;

static int16_t s_scroll, s_scroll_from, s_scroll_to;
static uint32_t s_scroll_t0;
static bool s_scrolling;

static int16_t s_hl, s_hl_from, s_hl_to;
static uint32_t s_hl_t0;
static bool s_hl_moving;

static int16_t s_bounce;
static uint32_t s_bounce_t0;
static int8_t s_bounce_dir;
static bool s_bouncing;

static void ensure_tick(void);
static void relayout(void);

static uint32_t elapsed_ms(uint32_t start) { return (s_phase - start) * TICK_MS; }

static Layer *front_canvas(void) {
  int idx = s_depth - 1;
  return (idx >= 0 && idx < LIST_DEPTH) ? s_canvas[idx] : NULL;
}

static void mark_dirty(void) {
  Layer *l = front_canvas();
  if (l) layer_mark_dirty(l);
}

static int16_t row_top(int index) {
  int16_t y = 0;
  for (int i = 0; i < index && i < s_row_count; i++) y += s_row_h[i];
  return y;
}

static int16_t content_height(void) { return row_top(s_row_count); }

// --- data ingest ------------------------------------------------------------

void list_request(int32_t list_id) {
  s_incoming_list = list_id;
  s_loading = true;
  s_load_t0 = s_phase;
  comm_send(WREQ_OPEN_LIST, NULL, list_id, 0);
  ensure_tick();
  mark_dirty();
}

void list_window_begin(int32_t list_id, int32_t count, const char *title) {
  s_incoming_list = list_id;
  s_row_count = 0;
  memset(s_rows, 0, sizeof(s_rows));
  str_copy(s_list_title, MAX_TITLE_LEN, title);
  (void)count;
}

void list_window_add(int32_t row, const ListRow *item) {
  if (row < 0 || row >= MAX_LIST_ROWS || !item) return;
  memcpy(&s_rows[row], item, sizeof(ListRow));
  if (row + 1 > s_row_count) s_row_count = row + 1;
}

void list_window_end(int32_t list_id, int32_t selected) {
  s_loaded_list = list_id;
  s_loading = false;
  relayout();
  if (selected < 0) selected = 0;
  if (selected >= s_row_count) selected = s_row_count > 0 ? s_row_count - 1 : 0;
  s_sel = selected;
  s_hl = s_hl_to = row_top(s_sel);
  s_hl_moving = false;
  s_scroll = s_scroll_to = 0;
  // Keep the preselected row on screen when the history is long.
  Layer *l = front_canvas();
  if (l) {
    int16_t view = layer_get_bounds(l).size.h - theme_header_height();
    int16_t bottom = row_top(s_sel) + s_row_h[s_sel];
    if (bottom > view) s_scroll = s_scroll_to = bottom - view;
  }
  ensure_tick();
  mark_dirty();
}

static void install_offline_rows(void) {
  memset(s_rows, 0, sizeof(s_rows));
  str_copy(s_rows[0].label, MAX_ROW_LABEL_LEN, "Settings");
  s_rows[0].action = ACT_SUBMENU;
  s_rows[0].arg = LIST_SETTINGS;
  s_rows[0].flags = ROW_FLAG_CHEVRON;
  str_copy(s_rows[1].label, MAX_ROW_LABEL_LEN, "New Chat");
  s_rows[1].action = ACT_NEW_CHAT;
  s_rows[1].flags = ROW_FLAG_ACCENT;
  str_copy(s_rows[2].label, MAX_ROW_LABEL_LEN, "Phone not connected");
  str_copy(s_rows[2].sub, MAX_ROW_SUB_LEN, "Open the Pebble app");
  s_rows[2].action = ACT_NONE;
  s_row_count = 3;
  str_copy(s_list_title, MAX_TITLE_LEN, "Assistant");
  s_loading = false;
  s_loaded_list = LIST_CHATS;
  relayout();
  s_sel = 1;
  s_hl = s_hl_to = row_top(1);
}

static void relayout(void) {
  for (int i = 0; i < s_row_count; i++) {
    s_row_h[i] = ROW_BASE_H + (s_rows[i].sub[0] ? ROW_SUB_H : 0);
  }
}

// --- drawing ----------------------------------------------------------------

static void draw_row(GContext *ctx, GRect b, int index, int16_t y, bool selected) {
  const Theme *t = theme();
  const ListRow *row = &s_rows[index];
  int16_t h = s_row_h[index];
  int16_t right_pad = 6;

  if (selected) {
    graphics_context_set_fill_color(ctx, t->accent);
    graphics_fill_rect(ctx, GRect(2, y + 1, b.size.w - 4, h - 2), 4, GCornersAll);
  }

  GColor label_color = selected ? GColorBlack
                                : ((row->flags & ROW_FLAG_ACCENT) ? t->accent : t->text);
  GColor sub_color = selected ? GColorBlack : t->text_dim;

  // Trailing badge: toggle state, submenu chevron, or the "current value" dot.
  const char *badge = NULL;
  if (row->flags & ROW_FLAG_ON) badge = "ON";
  else if (row->flags & ROW_FLAG_OFF) badge = "OFF";
  else if (row->flags & ROW_FLAG_CHEVRON) badge = ">";

  int16_t badge_w = 0;
  if (badge) {
    badge_w = graphics_text_layout_get_content_size(badge, theme_font_small(),
                  GRect(0, 0, 60, 20), GTextOverflowModeTrailingEllipsis,
                  GTextAlignmentRight).w + 4;
    graphics_context_set_text_color(ctx, label_color);
    graphics_draw_text(ctx, badge, theme_font_small(),
                       GRect(b.size.w - badge_w - right_pad, y + 3, badge_w, 20),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
  } else if (row->flags & ROW_FLAG_CURRENT) {
    graphics_context_set_fill_color(ctx, selected ? GColorBlack : t->accent);
    graphics_fill_circle(ctx, GPoint(b.size.w - 10, y + ROW_BASE_H / 2), 3);
    badge_w = 12;
  }

  graphics_context_set_text_color(ctx, label_color);
  graphics_draw_text(ctx, row->label, theme_font_title(),
                     GRect(8, y + 1, b.size.w - 14 - badge_w - right_pad, ROW_BASE_H),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  if (row->sub[0]) {
    graphics_context_set_text_color(ctx, sub_color);
    graphics_draw_text(ctx, row->sub, theme_font_small(),
                       GRect(8, y + ROW_BASE_H - 4, b.size.w - 16, ROW_SUB_H + 4),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  }
}

static void draw_loading(GContext *ctx, GRect b) {
  const Theme *t = theme();
  int16_t radius = 13;
  GPoint c = GPoint(b.size.w / 2, b.size.h / 2);
  int32_t start = (int32_t)((s_phase * 11) % 360);
  graphics_context_set_fill_color(ctx, t->accent_dim);
  graphics_fill_radial(ctx, GRect(c.x - radius, c.y - radius, radius * 2, radius * 2),
                       GOvalScaleModeFitCircle, 3, DEG_TO_TRIGANGLE(0), DEG_TO_TRIGANGLE(360));
  graphics_context_set_fill_color(ctx, t->accent);
  graphics_fill_radial(ctx, GRect(c.x - radius, c.y - radius, radius * 2, radius * 2),
                       GOvalScaleModeFitCircle, 3,
                       DEG_TO_TRIGANGLE(start), DEG_TO_TRIGANGLE(start + 100));
}

static void canvas_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  const Theme *t = theme();
  graphics_context_set_fill_color(ctx, t->background);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  if (s_loading || s_row_count == 0) {
    draw_loading(ctx, b);
    theme_draw_header(ctx, b, s_list_title[0] ? s_list_title : "Assistant", NULL);
    return;
  }

  int16_t top = theme_header_height();
  int16_t origin = top - s_scroll + s_bounce;

  // The highlight is drawn from its own animated position so it glides between
  // rows instead of teleporting.
  int16_t hl_y = origin + s_hl;
  int16_t hl_h = s_row_h[s_sel];
  graphics_context_set_fill_color(ctx, t->accent);
  graphics_fill_rect(ctx, GRect(2, hl_y + 1, b.size.w - 4, hl_h - 2), 4, GCornersAll);

  for (int i = 0; i < s_row_count; i++) {
    int16_t y = origin + row_top(i);
    if (y > b.size.h || y + s_row_h[i] < top) continue;
    draw_row(ctx, b, i, y, i == s_sel);
  }

  // Edge affordances: these lists exit by scrolling off either end.
  graphics_context_set_stroke_color(ctx, t->accent_dim);
  int16_t cx = b.size.w / 2;
  if (s_sel == 0) {
    for (int i = 0; i < 3; i++)
      graphics_draw_line(ctx, GPoint(cx - 4 + i, top + i), GPoint(cx + 4 - i, top + i));
  }
  if (s_sel == s_row_count - 1) {
    for (int i = 0; i < 3; i++)
      graphics_draw_line(ctx, GPoint(cx - 4 + i, b.size.h - 1 - i), GPoint(cx + 4 - i, b.size.h - 1 - i));
  }

  theme_draw_header(ctx, b, s_list_title[0] ? s_list_title : "Assistant", NULL);
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
  if (s_hl_moving) {
    uint32_t e = elapsed_ms(s_hl_t0);
    s_hl = s_hl_from + (int16_t)ease_out_cubic((int32_t)e, HL_MS, s_hl_to - s_hl_from);
    if (e >= HL_MS) { s_hl = s_hl_to; s_hl_moving = false; }
    busy = busy || s_hl_moving;
  }
  if (s_bouncing) {
    uint32_t e = elapsed_ms(s_bounce_t0);
    int16_t peak = (int16_t)(-s_bounce_dir * 8);
    if (e < 110) s_bounce = (int16_t)ease_out_cubic((int32_t)e, 110, peak);
    else if (e < 220) s_bounce = peak - (int16_t)ease_out_cubic((int32_t)(e - 110), 110, peak);
    else { s_bounce = 0; s_bouncing = false; }
    busy = busy || s_bouncing;
  }
  if (s_loading) {
    if (elapsed_ms(s_load_t0) > LOAD_TIMEOUT_MS) {
      s_loading = false;
      if (s_depth == 1 && s_list_ids[0] == LIST_CHATS) install_offline_rows();
      else toast_show("No response");
    } else {
      busy = true;
    }
  }

  mark_dirty();
  if (busy && s_depth > 0) s_tick = app_timer_register(TICK_MS, tick_cb, NULL);
}

static void ensure_tick(void) {
  if (!s_tick && s_depth > 0) s_tick = app_timer_register(TICK_MS, tick_cb, NULL);
}

static void scroll_selection_into_view(void) {
  Layer *l = front_canvas();
  if (!l) return;
  int16_t view = layer_get_bounds(l).size.h - theme_header_height();
  int16_t top = row_top(s_sel);
  int16_t bottom = top + s_row_h[s_sel];
  int16_t target = s_scroll_to;
  if (top < target) target = top;
  if (bottom > target + view) target = bottom - view;
  int16_t max = content_height() - view;
  if (max < 0) max = 0;
  if (target > max) target = max;
  if (target < 0) target = 0;
  if (target != s_scroll_to) {
    s_scroll_from = s_scroll;
    s_scroll_to = target;
    s_scroll_t0 = s_phase;
    s_scrolling = true;
  }
}

static void move_selection(int delta) {
  int next = s_sel + delta;
  if (next < 0 || next >= s_row_count) return;
  s_sel = next;
  s_hl_from = s_hl;
  s_hl_to = row_top(s_sel);
  s_hl_t0 = s_phase;
  s_hl_moving = true;
  scroll_selection_into_view();
  ensure_tick();
}

// --- navigation -------------------------------------------------------------

static void pop_current(void) {
  if (s_depth <= 1) {
    // The root list exits to the conversation, if there is one to go back to.
    if (reply_window_has_content()) {
      reply_window_return();
    } else {
      s_bounce_dir = 0;
      s_bounce_t0 = s_phase;
      s_bouncing = true;
      vibe_bump();
      ensure_tick();
    }
    return;
  }
  window_stack_pop(true);
}

static void edge_exit(int8_t dir) {
  if (s_depth <= 1 && !reply_window_has_content()) {
    s_bounce_dir = dir;
    s_bounce_t0 = s_phase;
    s_bouncing = true;
    vibe_bump();
    ensure_tick();
    return;
  }
  vibe_soft();
  pop_current();
}

static void push_list(int32_t list_id) {
  if (s_depth >= LIST_DEPTH) { toast_show("Too deep"); return; }
  s_list_ids[s_depth] = list_id;
  window_stack_push(s_windows[s_depth], true);
  s_depth++;
  list_request(list_id);
}

void list_push_root(void) {
  s_depth = 1;
  s_list_ids[0] = LIST_CHATS;
  window_stack_push(s_windows[0], false);
  list_request(LIST_CHATS);
}

void list_pop_submenus(void) {
  while (s_depth > 1) {
    s_depth--;
    window_stack_remove(s_windows[s_depth], false);
  }
}

static void activate(void) {
  if (s_sel < 0 || s_sel >= s_row_count) return;
  const ListRow *row = &s_rows[s_sel];

  switch (row->action) {
    case ACT_NONE:
      vibe_bump();
      break;

    case ACT_NEW_CHAT:
      comm_send(WREQ_NEW_CHAT, NULL, 0, 0);
      list_pop_submenus();
      dictation_start();
      break;

    case ACT_SUBMENU:
      push_list(row->arg);
      break;

    case ACT_CLOSE:
      pop_current();
      break;

    case ACT_OPEN_CHAT:
      // The phone replies with a turn, which pops back to the reply view.
      comm_send(WREQ_LIST_ACTION, NULL, ACT_OPEN_CHAT, row->arg);
      reply_window_set_status("Opening...", SPIN_THINKING);
      break;

    case ACT_SET_MODEL:
    case ACT_SET_EFFORT:
      comm_send(WREQ_LIST_ACTION, NULL, row->action, row->arg);
      vibe_soft();
      pop_current();
      break;

    case ACT_TOGGLE:
      comm_send(WREQ_LIST_ACTION, NULL, ACT_TOGGLE, row->arg);
      vibe_bump();
      // The phone echoes the refreshed list, so the row redraws with its new state.
      list_request(s_list_ids[s_depth - 1]);
      break;

    case ACT_QUICK:
      comm_send(WREQ_LIST_ACTION, NULL, ACT_QUICK, row->arg);
      list_pop_submenus();
      reply_window_set_question(row->label);
      reply_window_set_status("Thinking", SPIN_THINKING);
      break;

    default:
      break;
  }
}

static void up_click(ClickRecognizerRef recognizer, void *context) {
  if (s_loading) return;
  if (s_sel <= 0) edge_exit(-1);
  else move_selection(-1);
}

static void down_click(ClickRecognizerRef recognizer, void *context) {
  if (s_loading) return;
  if (s_sel >= s_row_count - 1) edge_exit(1);
  else move_selection(1);
}

static void select_click(ClickRecognizerRef recognizer, void *context) {
  if (s_loading) return;
  activate();
}

static void select_long(ClickRecognizerRef recognizer, void *context) {
  comm_send(WREQ_NEW_CHAT, NULL, 0, 0);
  list_pop_submenus();
  dictation_start();
}

static void click_config(void *context) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 130, up_click);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 130, down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, select_long, NULL);
}

// --- lifecycle --------------------------------------------------------------

static void window_load(Window *window) {
  int idx = (int)(uintptr_t)window_get_user_data(window);
  Layer *root = window_get_root_layer(window);
  s_canvas[idx] = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_canvas[idx], canvas_update);
  layer_add_child(root, s_canvas[idx]);
}

static void window_unload(Window *window) {
  int idx = (int)(uintptr_t)window_get_user_data(window);
  if (s_canvas[idx]) { layer_destroy(s_canvas[idx]); s_canvas[idx] = NULL; }
  if (s_depth > idx) s_depth = idx;
}

static void window_appear(Window *window) {
  int idx = (int)(uintptr_t)window_get_user_data(window);
  if (idx + 1 > s_depth) s_depth = idx + 1;
  // Coming back from a submenu or the reply view: refresh, since the model,
  // toggles and chat history may all have moved on.
  if (s_loaded_list != s_list_ids[idx] || idx == 0) list_request(s_list_ids[idx]);
  ensure_tick();
}

static void window_disappear(Window *window) {
  if (s_tick) { app_timer_cancel(s_tick); s_tick = NULL; }
}

void list_window_init(void) {
  for (int i = 0; i < LIST_DEPTH; i++) {
    s_windows[i] = window_create();
    window_set_user_data(s_windows[i], (void *)(uintptr_t)i);
    window_set_background_color(s_windows[i], theme()->background);
    window_set_click_config_provider(s_windows[i], click_config);
    window_set_window_handlers(s_windows[i], (WindowHandlers) {
      .load = window_load,
      .unload = window_unload,
      .appear = window_appear,
      .disappear = window_disappear,
    });
  }
}

void list_window_deinit(void) {
  if (s_tick) { app_timer_cancel(s_tick); s_tick = NULL; }
  for (int i = 0; i < LIST_DEPTH; i++) {
    if (s_windows[i]) { window_destroy(s_windows[i]); s_windows[i] = NULL; }
  }
}
