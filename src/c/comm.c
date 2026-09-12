#include "assistant.h"

// ---------------------------------------------------------------------------
// AppMessage plumbing.
//
// Only one outbox transfer can be in flight at a time, so requests are queued.
// Exactly one queued request may carry text (a dictated question), which keeps
// the queue itself tiny -- important on the 24 KB platforms.
// ---------------------------------------------------------------------------

#define PERSIST_KEY_SETTINGS 101
#define OUTBOX_QUEUE_LEN 6
#define PENDING_TEXT_LEN 512

typedef struct {
  int32_t req;
  int32_t a;
  int32_t b;
  bool carries_text;
} OutboxItem;

static OutboxItem s_queue[OUTBOX_QUEUE_LEN];
static uint8_t s_queue_head;
static uint8_t s_queue_len;
static bool s_sending;
static AppTimer *s_retry_timer;
static uint8_t s_retry_count;

static char s_pending_text[PENDING_TEXT_LEN];
static char s_held_question[PENDING_TEXT_LEN];   // asked before the phone said hello
static bool s_has_held_question;

static bool s_ready;
static char s_model_label[MAX_ROW_LABEL_LEN] = "Assistant";
static int32_t s_effort;
static int32_t s_flags = SFLAG_AUTO_DICT;

static Turn s_turn;
static bool s_turn_open;
static int32_t s_font_scale;

// Mirrored settings are persisted so the very first thing the app does on a cold
// launch -- deciding whether to open the microphone -- does not have to wait for
// the phone to connect.
typedef struct {
  int32_t flags;
  int32_t effort;
  int32_t font_scale;
  char model[MAX_ROW_LABEL_LEN];
} CachedSettings;

static void save_cached_settings(void) {
  CachedSettings cached;
  memset(&cached, 0, sizeof(cached));
  cached.flags = s_flags;
  cached.effort = s_effort;
  cached.font_scale = s_font_scale;
  str_copy(cached.model, MAX_ROW_LABEL_LEN, s_model_label);
  persist_write_data(PERSIST_KEY_SETTINGS, &cached, sizeof(cached));
}

static void load_cached_settings(void) {
  if (!persist_exists(PERSIST_KEY_SETTINGS)) return;
  CachedSettings cached;
  memset(&cached, 0, sizeof(cached));
  if (persist_read_data(PERSIST_KEY_SETTINGS, &cached, sizeof(cached)) <= 0) return;
  s_flags = cached.flags;
  s_effort = cached.effort;
  s_font_scale = cached.font_scale;
  theme_set_font_scale(s_font_scale);
  if (cached.model[0]) str_copy(s_model_label, sizeof(s_model_label), cached.model);
}

static void pump_outbox(void);

bool comm_is_ready(void) { return s_ready; }
const char *comm_model_label(void) { return s_model_label; }
int32_t comm_effort(void) { return s_effort; }
int32_t comm_flags(void) { return s_flags; }

// --- outbox -----------------------------------------------------------------

static void retry_timer_cb(void *data) {
  s_retry_timer = NULL;
  pump_outbox();
}

static void schedule_retry(uint32_t delay_ms) {
  if (s_retry_timer) app_timer_cancel(s_retry_timer);
  s_retry_timer = app_timer_register(delay_ms, retry_timer_cb, NULL);
}

static void pump_outbox(void) {
  if (s_sending || s_queue_len == 0) return;

  DictionaryIterator *iter;
  AppMessageResult result = app_message_outbox_begin(&iter);
  if (result != APP_MSG_OK) {
    schedule_retry(120);
    return;
  }

  OutboxItem *item = &s_queue[s_queue_head];
  dict_write_int32(iter, MESSAGE_KEY_WREQ, item->req);
  dict_write_int32(iter, MESSAGE_KEY_WINT, item->a);
  dict_write_int32(iter, MESSAGE_KEY_WINT2, item->b);
  if (item->carries_text) {
    dict_write_cstring(iter, MESSAGE_KEY_WSTR, s_pending_text);
  }
  dict_write_end(iter);

  s_sending = true;
  if (app_message_outbox_send() != APP_MSG_OK) {
    s_sending = false;
    schedule_retry(160);
  }
}

static void pop_queue(void) {
  if (s_queue_len == 0) return;
  if (s_queue[s_queue_head].carries_text) s_pending_text[0] = '\0';
  s_queue_head = (s_queue_head + 1) % OUTBOX_QUEUE_LEN;
  s_queue_len--;
  s_retry_count = 0;
}

void comm_send(int32_t req, const char *str, int32_t a, int32_t b) {
  if (s_queue_len >= OUTBOX_QUEUE_LEN) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "outbox queue full, dropping req %d", (int)req);
    return;
  }
  uint8_t slot = (s_queue_head + s_queue_len) % OUTBOX_QUEUE_LEN;
  s_queue[slot].req = req;
  s_queue[slot].a = a;
  s_queue[slot].b = b;
  s_queue[slot].carries_text = (str != NULL && str[0] != '\0');
  if (s_queue[slot].carries_text) str_copy(s_pending_text, PENDING_TEXT_LEN, str);
  s_queue_len++;
  pump_outbox();
}

void comm_queue_question(const char *text) {
  if (!text || !text[0]) return;
  if (s_ready) {
    comm_send(WREQ_ASK, text, 0, 0);
  } else {
    str_copy(s_held_question, PENDING_TEXT_LEN, text);
    s_has_held_question = true;
    reply_window_set_status("Connecting...", SPIN_THINKING);
  }
}

void comm_send_hello(void) {
  int32_t cookie = -1;
  timers_launch_cookie(&cookie);
  comm_send(WREQ_HELLO, NULL, (int32_t)launch_reason(), cookie);
}

static void outbox_sent(DictionaryIterator *iter, void *context) {
  s_sending = false;
  pop_queue();
  pump_outbox();
}

static void outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  s_sending = false;
  s_retry_count++;
  APP_LOG(APP_LOG_LEVEL_WARNING, "outbox failed: %d (attempt %d)", (int)reason, (int)s_retry_count);

  // Launching the watch app before the phone-side JS is up makes the first few
  // sends fail, so failures must be ridden out rather than treated as fatal.
  // Backing off to a few seconds keeps a genuinely absent phone from burning
  // the radio, and only a request that has failed for the better part of a
  // minute is abandoned.
  if (s_retry_count > 10) {
    pop_queue();
    if (reply_window_state() == REPLY_BUSY) reply_window_set_error("Phone unreachable");
    s_retry_count = 0;
  }

  uint32_t backoff = 250;
  for (uint8_t i = 0; i < s_retry_count && i < 4; i++) backoff *= 2;
  schedule_retry(backoff);
}

// --- inbox ------------------------------------------------------------------

static void apply_settings(Tuple *str, Tuple *pint, Tuple *pint2, Tuple *pflag) {
  if (str) str_copy(s_model_label, sizeof(s_model_label), str->value->cstring);
  if (pint) s_effort = pint->value->int32;
  if (pint2) s_flags = pint2->value->int32;
  if (pflag) {
    s_font_scale = pflag->value->int32;
    theme_set_font_scale(s_font_scale);
  }
  save_cached_settings();
}

static void handle_list_item(Tuple *pint, Tuple *pstr, Tuple *pint2, Tuple *pflag) {
  if (!pint || !pstr) return;
  ListRow row;
  memset(&row, 0, sizeof(row));
  row.action = pint2 ? pint2->value->int32 : ACT_NONE;
  row.flags = pflag ? (uint8_t)pflag->value->int32 : 0;

  // "label\x1fsubtitle\x1farg"
  const char *raw = pstr->value->cstring;
  const char *sep = strchr(raw, '\x1f');
  if (sep) {
    size_t label_len = (size_t)(sep - raw);
    if (label_len >= MAX_ROW_LABEL_LEN) label_len = MAX_ROW_LABEL_LEN - 1;
    memcpy(row.label, raw, label_len);
    row.label[label_len] = '\0';

    const char *sub = sep + 1;
    const char *sep2 = strchr(sub, '\x1f');
    if (sep2) {
      size_t sub_len = (size_t)(sep2 - sub);
      if (sub_len >= MAX_ROW_SUB_LEN) sub_len = MAX_ROW_SUB_LEN - 1;
      memcpy(row.sub, sub, sub_len);
      row.sub[sub_len] = '\0';
      row.arg = atoi(sep2 + 1);
    } else {
      str_copy(row.sub, MAX_ROW_SUB_LEN, sub);
      row.arg = pint->value->int32;
    }
  } else {
    str_copy(row.label, MAX_ROW_LABEL_LEN, raw);
    row.arg = pint->value->int32;
  }
  list_window_add(pint->value->int32, &row);
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *evt_t = dict_find(iter, MESSAGE_KEY_PEVT);
  if (!evt_t) return;

  Tuple *pstr  = dict_find(iter, MESSAGE_KEY_PSTR);
  Tuple *pint  = dict_find(iter, MESSAGE_KEY_PINT);
  Tuple *pint2 = dict_find(iter, MESSAGE_KEY_PINT2);
  Tuple *pflag = dict_find(iter, MESSAGE_KEY_PFLAG);

  switch (evt_t->value->int32) {
    case PEVT_READY:
      apply_settings(pstr, pint, pint2, pflag);
      s_ready = true;
      // Proof the phone is alive.  Anything the watch asked for before it was
      // listening went nowhere, so give the front list another chance instead
      // of leaving "Phone not connected" on screen until the user backs out.
      list_phone_ready();
      if (s_has_held_question) {
        s_has_held_question = false;
        comm_send(WREQ_ASK, s_held_question, 0, 0);
        s_held_question[0] = '\0';
      }
      break;

    case PEVT_SETTINGS:
      apply_settings(pstr, pint, pint2, pflag);
      break;

    case PEVT_STATUS:
      reply_window_set_status(pstr ? pstr->value->cstring : "",
                              pint ? pint->value->int32 : SPIN_THINKING);
      break;

    case PEVT_TURN_BEGIN:
      memset(&s_turn, 0, sizeof(s_turn));
      s_turn.index = pint ? pint->value->int32 : 0;
      s_turn.count = pint2 ? pint2->value->int32 : 1;
      if (pstr) str_copy(s_turn.title, MAX_TITLE_LEN, pstr->value->cstring);
      s_turn_open = true;
      break;

    case PEVT_Q_CHUNK:
      if (s_turn_open && pstr) str_append(s_turn.question, MAX_QUESTION_LEN, pstr->value->cstring);
      break;

    case PEVT_A_CHUNK:
      if (s_turn_open && pstr) str_append(s_turn.answer, MAX_ANSWER_LEN, pstr->value->cstring);
      break;

    case PEVT_TURN_END:
      if (!s_turn_open) break;
      s_turn_open = false;
      s_turn.is_live = pint ? (pint->value->int32 != 0) : true;
      list_pop_submenus();
      reply_window_show_turn(&s_turn);
      break;

    case PEVT_LIST_BEGIN:
      list_window_begin(pint ? pint->value->int32 : 0,
                        pint2 ? pint2->value->int32 : 0,
                        pstr ? pstr->value->cstring : "");
      break;

    case PEVT_LIST_ITEM:
      handle_list_item(pint, pstr, pint2, pflag);
      break;

    case PEVT_LIST_END:
      list_window_end(pint ? pint->value->int32 : 0,
                      pint2 ? pint2->value->int32 : 0);
      break;

    case PEVT_ERROR:
      reply_window_set_error(pstr ? pstr->value->cstring : "Something went wrong");
      break;

    case PEVT_TOAST:
      if (pstr) toast_show(pstr->value->cstring);
      break;

    case PEVT_DISMISS:
      list_pop_submenus();
      reply_window_forget();
      break;

    case PEVT_WAKEUP_SET:
      if (pint) {
        timers_set((time_t)pint->value->int32,
                   pint2 ? pint2->value->int32 : 0,
                   pstr ? pstr->value->cstring : "Reminder");
      }
      break;

    case PEVT_WAKEUP_CLR:
      timers_clear(pint ? pint->value->int32 : -1);
      break;

    default:
      break;
  }
}

static void inbox_dropped(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "inbox dropped: %d", (int)reason);
  // A dropped chunk would silently corrupt the turn being assembled, so give up
  // on it rather than showing the user half a sentence.
  if (s_turn_open) {
    s_turn_open = false;
    reply_window_set_error("Lost part of the reply");
  }
}

void comm_init(void) {
  load_cached_settings();
  app_message_register_inbox_received(inbox_received);
  app_message_register_inbox_dropped(inbox_dropped);
  app_message_register_outbox_sent(outbox_sent);
  app_message_register_outbox_failed(outbox_failed);
  app_message_open(app_message_inbox_size_maximum(), app_message_outbox_size_maximum());
}

void comm_deinit(void) {
  if (s_retry_timer) {
    app_timer_cancel(s_retry_timer);
    s_retry_timer = NULL;
  }
  app_message_deregister_callbacks();
}
