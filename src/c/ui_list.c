#include "assistant.h"

// ---------------------------------------------------------------------------
// Lists.
//
// The chats list is the app's root window: [Settings] [New Chat] [past chats...]
// with "New Chat" preselected, so cancelling dictation lands you one button away
// from trying again.  Submenus (Settings -> Model / Thinking) stack on top of it.
//
// Only the front list's rows are held in memory; going back re-requests the
// parent from the phone, which doubles as a cheap way to keep displayed values
// (current model, toggle states) honest after a change.
//
// The list is a stock MenuLayer.  Rows are drawn by hand -- badges, dots and a
// marquee on the selected label -- but selection, scrolling and the repeat rate
// belong to the platform, because a list that scrolls even slightly unlike every
// other list on the watch reads as broken however nice the animation is.
// ---------------------------------------------------------------------------

#define LIST_DEPTH      3
#define TICK_MS         33
#define LOAD_TIMEOUT_MS 5000

#ifdef PBL_PLATFORM_EMERY
#define ROW_BASE_H      30
#define ROW_SUB_H       17
#else
#define ROW_BASE_H      24
#define ROW_SUB_H       14
#endif

static Window *s_windows[LIST_DEPTH];
static Layer *s_canvas[LIST_DEPTH];     // background, header and the loading state
static MenuLayer *s_menus[LIST_DEPTH];
static Layer *s_overlays[LIST_DEPTH];   // toasts, above the menu
static int32_t s_list_ids[LIST_DEPTH];
static int s_depth;

static ListRow s_rows[MAX_LIST_ROWS];
static int16_t s_row_h[MAX_LIST_ROWS];
static int s_row_count;
static int32_t s_loaded_list = -1;
static int32_t s_incoming_list = -1;
static char s_list_title[MAX_TITLE_LEN];
static bool s_loading;
static bool s_offline;   // showing the locally synthesised rows
static bool s_row_scrolling;
static bool s_title_scrolling;
static uint32_t s_marquee_t0;

static uint32_t s_phase;
static AppTimer *s_tick;
static uint32_t s_load_t0;

static void ensure_tick(void);
static void relayout(void);
static void update_visibility(void);

static uint32_t elapsed_ms(uint32_t start) { return (s_phase - start) * TICK_MS; }

static int front_index(void) {
  int idx = s_depth - 1;
  return (idx >= 0 && idx < LIST_DEPTH) ? idx : -1;
}

static MenuLayer *front_menu(void) {
  int idx = front_index();
  return idx >= 0 ? s_menus[idx] : NULL;
}

static void mark_dirty(void) {
  int idx = front_index();
  if (idx < 0) return;
  if (s_canvas[idx]) layer_mark_dirty(s_canvas[idx]);
  if (s_menus[idx]) layer_mark_dirty(menu_layer_get_layer(s_menus[idx]));
  if (s_overlays[idx]) layer_mark_dirty(s_overlays[idx]);
}

static int selected_row(void) {
  MenuLayer *menu = front_menu();
  if (!menu) return 0;
  return (int)menu_layer_get_selected_index(menu).row;
}

// --- data ingest ------------------------------------------------------------

static void send_list_request(int32_t list_id) {
  s_incoming_list = list_id;
  s_loading = true;
  s_load_t0 = s_phase;
  comm_send(WREQ_OPEN_LIST, NULL, list_id, 0);
  update_visibility();
  ensure_tick();
  mark_dirty();
}

void list_request(int32_t list_id) {
  // A window's appear handler and an explicit request often land together;
  // asking twice only doubles the Bluetooth traffic.
  if (s_loading && s_incoming_list == list_id && elapsed_ms(s_load_t0) < 1500) return;
  send_list_request(list_id);
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

static void select_row(int row) {
  MenuLayer *menu = front_menu();
  if (!menu || s_row_count <= 0) return;
  if (row < 0) row = 0;
  if (row >= s_row_count) row = s_row_count - 1;
  // MenuRowAlignCenter is what brings a preselected row into view on its own.
  menu_layer_set_selected_index(menu, MenuIndex(0, (uint16_t)row),
                                MenuRowAlignCenter, false);
}

void list_window_end(int32_t list_id, int32_t selected) {
  s_loaded_list = list_id;
  s_loading = false;
  s_offline = false;
  relayout();
  s_marquee_t0 = s_phase;
  MenuLayer *menu = front_menu();
  if (menu) menu_layer_reload_data(menu);
  update_visibility();
  select_row((int)selected);
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
  s_offline = true;
  relayout();
  MenuLayer *menu = front_menu();
  if (menu) menu_layer_reload_data(menu);
  update_visibility();
  select_row(1);
}

// The phone's JS has started.
//
// A request sent before it registered its appmessage listener is ACKed by the
// transport and then dropped on the floor, so the watch sees a successful send
// and waits forever for an answer that nobody heard it ask for.  This always
// re-asks, deliberately bypassing the coalescing in list_request(): the pending
// request is exactly the one that was lost.
void list_phone_ready(void) {
  if (s_depth <= 0) return;
  int idx = s_depth - 1;
  if (s_offline || s_loaded_list != s_list_ids[idx]) send_list_request(s_list_ids[idx]);
}

static void relayout(void) {
  for (int i = 0; i < s_row_count; i++) {
    s_row_h[i] = ROW_BASE_H + (s_rows[i].sub[0] ? ROW_SUB_H : 0);
  }
}

// --- drawing ----------------------------------------------------------------

// One MenuLayer cell.  The cell's own background has already been filled by the
// menu in the right colour for its state, so everything here draws on top of it.
static void menu_draw_row(GContext *ctx, const Layer *cell_layer,
                          MenuIndex *cell_index, void *data) {
  const Theme *t = theme();
  int index = (int)cell_index->row;
  if (index < 0 || index >= s_row_count) return;
  const ListRow *row = &s_rows[index];
  GRect b = layer_get_bounds(cell_layer);
  bool selected = menu_cell_layer_is_highlighted(cell_layer);
  int16_t right_pad = 6;

  GColor row_bg = selected ? t->accent : t->background;
  GColor label_color = selected ? GColorBlack
                                : ((row->flags & ROW_FLAG_ACCENT) ? t->accent : t->text);
  GColor sub_color = selected ? GColorBlack : t->text_dim;

  // Trailing furniture: toggle state, submenu chevron, or the "current value"
  // dot.  Measured now, drawn after the label -- a scrolling label is painted
  // wide and masked back, and the mask must be free to sweep the whole row.
  const char *badge = NULL;
  if (row->flags & ROW_FLAG_ON) badge = "ON";
  else if (row->flags & ROW_FLAG_OFF) badge = "OFF";
  else if (row->flags & ROW_FLAG_CHEVRON) badge = ">";

  int16_t badge_w = 0;
  if (badge) {
    badge_w = graphics_text_layout_get_content_size(badge, theme_font_small(),
                  GRect(0, 0, 60, 20), GTextOverflowModeTrailingEllipsis,
                  GTextAlignmentRight).w + 4;
  } else if (row->flags & ROW_FLAG_CURRENT) {
    badge_w = 12;
  }

  // The selected row slides a long label sideways rather than cutting it off;
  // unselected rows stay still and simply truncate.
  GRect label_box = GRect(6, 1, b.size.w - 12 - badge_w - right_pad, ROW_BASE_H);
  if (selected) {
    if (theme_draw_marquee(ctx, label_box, GRect(1, 1, b.size.w - 2, ROW_BASE_H),
                           row->label, theme_font_title(), label_color, row_bg,
                           (s_phase - s_marquee_t0) * TICK_MS)) {
      s_row_scrolling = true;
    }
  } else {
    graphics_context_set_text_color(ctx, label_color);
    graphics_draw_text(ctx, row->label, theme_font_title(), label_box,
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  }

  if (badge) {
    graphics_context_set_text_color(ctx, label_color);
    graphics_draw_text(ctx, badge, theme_font_small(),
                       GRect(b.size.w - badge_w - right_pad, 3, badge_w, 20),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
  } else if (row->flags & ROW_FLAG_CURRENT) {
    graphics_context_set_fill_color(ctx, selected ? GColorBlack : t->accent);
    graphics_fill_circle(ctx, GPoint(b.size.w - 10, ROW_BASE_H / 2), 3);
  }

  if (row->sub[0]) {
    graphics_context_set_text_color(ctx, sub_color);
    graphics_draw_text(ctx, row->sub, theme_font_small(),
                       GRect(6, ROW_BASE_H - 4, b.size.w - 12, ROW_SUB_H + 4),
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

  if (s_loading || s_row_count == 0) draw_loading(ctx, b);
  if (theme_draw_header(ctx, b, s_list_title[0] ? s_list_title : "Assistant", NULL,
                        /*show_clock*/ true, (s_phase - s_marquee_t0) * TICK_MS)) {
    s_title_scrolling = true;
  }
}

static void overlay_update(Layer *layer, GContext *ctx) {
  toast_draw(ctx, layer_get_bounds(layer));
}

// --- menu callbacks ---------------------------------------------------------

static uint16_t menu_num_rows(MenuLayer *menu, uint16_t section, void *data) {
  return (uint16_t)s_row_count;
}

static int16_t menu_cell_height(MenuLayer *menu, MenuIndex *cell_index, void *data) {
  int index = (int)cell_index->row;
  if (index < 0 || index >= s_row_count) return ROW_BASE_H;
  return s_row_h[index];
}

// --- animation --------------------------------------------------------------

static void tick_cb(void *data) {
  s_tick = NULL;
  s_phase++;
  bool busy = false;

  // The menu draws its own rows, so this is the only place that can tell
  // whether a label is still sliding: read what the last frame reported, then
  // clear it for the frame this tick is about to ask for.
  if (s_row_scrolling || s_title_scrolling) busy = true;
  s_row_scrolling = false;
  s_title_scrolling = false;

  if (toast_active()) busy = true;
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

// While the rows are still on their way there is nothing to select, so the menu
// steps aside and lets the spinner have the screen.
static void update_visibility(void) {
  int idx = front_index();
  if (idx < 0 || !s_menus[idx]) return;
  bool empty = s_loading || s_row_count == 0;
  layer_set_hidden(menu_layer_get_layer(s_menus[idx]), empty);
}

// --- navigation -------------------------------------------------------------

// No list is left by scrolling off its end -- a MenuLayer simply stops there,
// which is both the platform's behaviour and the one asked for.  BACK leaves a
// submenu; the root list is the app, so there is nothing under it to go to.
static void pop_current(void) {
  if (s_depth <= 1) { vibe_bump(); return; }
  window_stack_pop(true);
}

void list_open(int32_t list_id) {
  if (s_depth >= LIST_DEPTH) { toast_show("Too deep"); return; }
  int target = s_depth;
  s_list_ids[target] = list_id;
  // window_stack_push runs the new window's appear handler synchronously, and
  // that handler is what owns s_depth (and issues the list request).  Do not
  // also bump s_depth here: counting the same push twice leaves front_canvas()
  // pointing at a window that was never loaded, and the list stops repainting.
  window_stack_push(s_windows[target], true);
  if (s_depth <= target) s_depth = target + 1;
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
  int sel = selected_row();
  if (sel < 0 || sel >= s_row_count) return;
  const ListRow *row = &s_rows[sel];

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
      list_open(row->arg);
      break;

    case ACT_CLOSE:
      pop_current();
      break;

    case ACT_OPEN_CHAT:
      // The phone replies with a turn, which pops back to the reply view.
      comm_send(WREQ_LIST_ACTION, NULL, ACT_OPEN_CHAT, row->arg);
      reply_window_set_status("Opening...", SPIN_THINKING);
      break;

    case ACT_REDO_TURN: {
      // Nothing is discarded until the replacement question actually arrives,
      // so backing out of the microphone leaves the conversation untouched.
      int32_t turn = reply_window_turn_index();
      if (turn < 0) { toast_show("Nothing to redo"); break; }
      list_pop_submenus();
      dictation_start_redo(turn);
      break;
    }

    case ACT_GOTO_TURN:
      // UP and DOWN belong to the scroller now, so this is how a conversation
      // with more than one turn is walked.
      pop_current();
      reply_window_goto_turn((int8_t)row->arg);
      break;

    case ACT_DELETE_CHAT:
      // The phone deletes it and answers with PEVT_DISMISS, which tears down
      // both this list and the conversation behind it.
      comm_send(WREQ_LIST_ACTION, NULL, ACT_DELETE_CHAT, row->arg);
      vibe_soft();
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

    default:
      break;
  }
}

static void menu_select(MenuLayer *menu, MenuIndex *cell_index, void *data) {
  if (s_loading) return;
  activate();
}

static void menu_select_long(MenuLayer *menu, MenuIndex *cell_index, void *data) {
  comm_send(WREQ_NEW_CHAT, NULL, 0, 0);
  list_pop_submenus();
  dictation_start();
}

// A new row starts its marquee from the beginning rather than mid-slide.
static void menu_selection_changed(struct MenuLayer *menu, MenuIndex new_index,
                                   MenuIndex old_index, void *data) {
  s_marquee_t0 = s_phase;
  ensure_tick();
}

// --- lifecycle --------------------------------------------------------------

static void window_load(Window *window) {
  int idx = (int)(uintptr_t)window_get_user_data(window);
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  const Theme *t = theme();

  s_canvas[idx] = layer_create(b);
  layer_set_update_proc(s_canvas[idx], canvas_update);
  layer_add_child(root, s_canvas[idx]);

  int16_t top = theme_header_height();
  s_menus[idx] = menu_layer_create(GRect(0, top, b.size.w, b.size.h - top));
  menu_layer_set_callbacks(s_menus[idx], NULL, (MenuLayerCallbacks) {
    .get_num_rows = menu_num_rows,
    .get_cell_height = menu_cell_height,
    .draw_row = menu_draw_row,
    .select_click = menu_select,
    .select_long_click = menu_select_long,
    .selection_changed = menu_selection_changed,
  });
  menu_layer_set_normal_colors(s_menus[idx], t->background, t->text);
  menu_layer_set_highlight_colors(s_menus[idx], t->accent, GColorBlack);
  menu_layer_set_click_config_onto_window(s_menus[idx], window);
  layer_add_child(root, menu_layer_get_layer(s_menus[idx]));

  // Toasts sit above the menu, which otherwise covers everything below the
  // header and would swallow them.
  s_overlays[idx] = layer_create(b);
  layer_set_update_proc(s_overlays[idx], overlay_update);
  layer_add_child(root, s_overlays[idx]);
}

static void window_unload(Window *window) {
  int idx = (int)(uintptr_t)window_get_user_data(window);
  if (s_overlays[idx]) { layer_destroy(s_overlays[idx]); s_overlays[idx] = NULL; }
  if (s_menus[idx]) { menu_layer_destroy(s_menus[idx]); s_menus[idx] = NULL; }
  if (s_canvas[idx]) { layer_destroy(s_canvas[idx]); s_canvas[idx] = NULL; }
  if (s_depth > idx) s_depth = idx;
}

static void window_appear(Window *window) {
  int idx = (int)(uintptr_t)window_get_user_data(window);
  if (idx + 1 > s_depth) s_depth = idx + 1;
  // Coming back from a submenu or the reply view: refresh, since the model,
  // toggles and chat history may all have moved on.
  if (s_loaded_list != s_list_ids[idx] || idx == 0) list_request(s_list_ids[idx]);
  update_visibility();
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
    window_set_window_handlers(s_windows[i], (WindowHandlers) {
      .load = window_load,
      .unload = window_unload,
      .appear = window_appear,
      .disappear = window_disappear,
    });
  }
}

// Row heights follow the font, so a text-size change has to re-measure.  Also
// used by the minute tick that redraws the header clock.
void list_window_refresh(void) {
  relayout();
  MenuLayer *menu = front_menu();
  if (menu) menu_layer_reload_data(menu);
  mark_dirty();
}

void list_window_deinit(void) {
  if (s_tick) { app_timer_cancel(s_tick); s_tick = NULL; }
  for (int i = 0; i < LIST_DEPTH; i++) {
    if (s_windows[i]) { window_destroy(s_windows[i]); s_windows[i] = NULL; }
  }
}
