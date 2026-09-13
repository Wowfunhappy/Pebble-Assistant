#pragma once

#include <pebble.h>

// ---------------------------------------------------------------------------
// Watch <-> phone protocol
//
// The watch is deliberately a thin renderer: every menu, every label and every
// piece of conversation text is owned by the phone.  That keeps the dynamic
// model list, the settings copy and the conversation store in one place, and
// means a settings change never needs a new watch build.
//
// Message keys are append-only.  Reordering them breaks the C/JS contract.
// ---------------------------------------------------------------------------

// Watch -> phone, carried in MESSAGE_KEY_WREQ.
#define WREQ_HELLO         1   // WINT = launch reason, WINT2 = wakeup cookie (-1 if none)
#define WREQ_ASK           2   // WSTR = dictated text, WINT = turn to replace (-1 to append)
#define WREQ_CANCEL        3   // abandon the in-flight request
#define WREQ_NEW_CHAT      4
#define WREQ_OPEN_LIST     5   // WINT = list id
#define WREQ_LIST_ACTION   6   // WINT = action, WINT2 = action argument
#define WREQ_GET_TURN      7   // WINT = turn index, -1 for the latest turn
#define WREQ_WAKEUP_FIRED  8   // WINT = cookie
#define WREQ_SNOOZE        9   // WINT = cookie, WINT2 = minutes

// Phone -> watch, carried in MESSAGE_KEY_PEVT.
#define PEVT_READY         1   // PSTR = model label, PINT = effort idx, PINT2 = flags, PFLAG = font idx
#define PEVT_STATUS        2   // PSTR = status line, PINT = spinner kind
#define PEVT_TURN_BEGIN    3   // PINT = turn index, PINT2 = turn count, PSTR = chat title
#define PEVT_Q_CHUNK       4   // PSTR = next slice of the question text
#define PEVT_A_CHUNK       5   // PSTR = next slice of the answer text
#define PEVT_TURN_END      6   // PINT = 1 when this is the newest turn
#define PEVT_LIST_BEGIN    7   // PINT = list id, PINT2 = item count, PSTR = list title
#define PEVT_LIST_ITEM     8   // PINT = row, PSTR = "label\x1fsubtitle", PINT2 = action, PFLAG = flags
#define PEVT_LIST_END      9   // PINT = list id, PINT2 = row to preselect
#define PEVT_ERROR        10   // PSTR = human-readable failure
#define PEVT_WAKEUP_SET   11   // PINT = unix time, PINT2 = cookie, PSTR = title
#define PEVT_WAKEUP_CLR   12   // PINT = cookie, -1 clears every slot
#define PEVT_SETTINGS     13   // same shape as PEVT_READY
#define PEVT_TOAST        14   // PSTR = short confirmation banner
#define PEVT_DISMISS      15   // forget the shown turn and fall back to the chats list

// List identifiers.
#define LIST_CHATS         1   // root list: New Chat, Settings, then history
#define LIST_SETTINGS      2
#define LIST_MODELS        3
#define LIST_EFFORT        4
#define LIST_CHAT_ACTIONS  5   // long-press SELECT inside a conversation

// Row actions the phone can attach to a list item.
#define ACT_NONE           0
#define ACT_OPEN_CHAT      1   // arg = chat index
#define ACT_SET_MODEL      2   // arg = model index
#define ACT_SET_EFFORT     3   // arg = effort index
#define ACT_TOGGLE         4   // arg = toggle id
#define ACT_SUBMENU        5   // arg = list id
#define ACT_NEW_CHAT       6   // dismiss the list and start dictation
#define ACT_CLOSE          8   // dismiss the list
#define ACT_DELETE_CHAT    9   // delete the conversation being shown
#define ACT_REDO_TURN     10   // re-record the question for the turn on screen
#define ACT_GOTO_TURN     11   // arg = -1 for the previous turn, +1 for the next

// Row flags.
#define ROW_FLAG_CURRENT   (1 << 0)   // draw the "active value" dot
#define ROW_FLAG_ON        (1 << 1)   // toggle is on
#define ROW_FLAG_OFF       (1 << 2)   // toggle is off
#define ROW_FLAG_CHEVRON   (1 << 3)   // row opens a submenu
#define ROW_FLAG_ACCENT    (1 << 4)   // draw the label in the accent colour

// Settings flags (PEVT_READY / PEVT_SETTINGS, PINT2).
#define SFLAG_SEARCH       (1 << 0)
#define SFLAG_LOCATION     (1 << 1)
#define SFLAG_HEALTH       (1 << 2)
#define SFLAG_CONFIGURED   (1 << 3)
#define SFLAG_CONFIRM_DICT (1 << 4)
#define SFLAG_AUTO_DICT    (1 << 5)

// Spinner kinds for PEVT_STATUS.
#define SPIN_NONE          0
#define SPIN_THINKING      1
#define SPIN_SEARCHING     2
#define SPIN_TOOL          3

// ---------------------------------------------------------------------------
// Sizing.  These are static allocations, so they are deliberately modest.
// ---------------------------------------------------------------------------

#define MAX_QUESTION_LEN   600
#define MAX_ANSWER_LEN     2048
#define MAX_TITLE_LEN      96   // 63 characters, in bytes, with room for an ellipsis
#define MAX_STATUS_LEN     96   // also holds error text, which runs longer
#define MAX_ROW_LABEL_LEN  96   // ditto: long enough that the marquee has something to scroll
#define MAX_ROW_SUB_LEN    32
#define MAX_LIST_ROWS      20
#define MAX_WAKEUPS        8

// ---------------------------------------------------------------------------
// Theme (theme.c)
// ---------------------------------------------------------------------------

typedef struct {
  GColor background;
  GColor surface;
  GColor accent;
  GColor accent_dim;
  GColor text;
  GColor text_dim;
  GColor question;
  GColor danger;
} Theme;

const Theme *theme(void);
void theme_set_font_scale(int32_t index);
GFont theme_font_body(void);
GFont theme_font_question(void);
GFont theme_font_title(void);
GFont theme_font_small(void);
int16_t theme_header_height(void);
bool theme_draw_header(GContext *ctx, GRect bounds, const char *left, const char *right,
                       bool show_clock, uint32_t marquee_ms);
bool theme_draw_marquee(GContext *ctx, GRect box, GRect mask, const char *text,
                        GFont font, GColor ink, GColor background, uint32_t elapsed_ms);

// ---------------------------------------------------------------------------
// Communication (comm.c)
// ---------------------------------------------------------------------------

typedef struct {
  int32_t action;
  int32_t arg;
  uint8_t flags;
  char label[MAX_ROW_LABEL_LEN];
  char sub[MAX_ROW_SUB_LEN];
} ListRow;

typedef struct {
  int32_t index;
  int32_t count;
  bool is_live;
  char title[MAX_TITLE_LEN];
  char question[MAX_QUESTION_LEN];
  char answer[MAX_ANSWER_LEN];
} Turn;

void comm_init(void);
void comm_deinit(void);
bool comm_is_ready(void);
void comm_send(int32_t req, const char *str, int32_t a, int32_t b);
void comm_send_hello(void);
void comm_queue_question(const char *text, int32_t redo_index);

// Live settings mirrored from the phone.
const char *comm_model_label(void);
int32_t comm_effort(void);
int32_t comm_flags(void);

// ---------------------------------------------------------------------------
// Windows
//
// The app has exactly two screens.  The chats list is the root window; the
// reply view is pushed on top of it whenever there is a conversation turn to
// show.  Submenus (Settings, Models, Thinking) stack above whichever of those
// is in front.
// ---------------------------------------------------------------------------

typedef enum {
  REPLY_BUSY,    // question sent, waiting on the model
  REPLY_SHOW,    // showing a question + answer pair
  REPLY_ERROR,
  REPLY_ALERT,   // a reminder fired
} ReplyState;

void reply_window_init(void);
void reply_window_deinit(void);
ReplyState reply_window_state(void);
bool reply_window_has_content(void);
void reply_window_set_question(const char *text);
void reply_window_set_status(const char *text, int32_t spinner);
void reply_window_set_error(const char *text);
void reply_window_set_alert(const char *title, int32_t cookie);
void reply_window_show_turn(const Turn *turn);
void reply_window_hide(void);     // drop back to the chats list
void reply_window_refresh(void);  // re-measure after a font change, and redraw
void reply_window_forget(void);   // discard the shown turn, then hide
int32_t reply_window_turn_index(void);  // turn on screen, or -1 if none
void reply_window_goto_turn(int8_t dir);  // -1 back a turn, +1 forward

void list_window_init(void);
void list_window_deinit(void);
void list_window_refresh(void);   // re-measure after a font change, and redraw
void list_push_root(void);        // the chats list, pushed at boot
void list_window_begin(int32_t list_id, int32_t count, const char *title);
void list_window_add(int32_t row, const ListRow *item);
void list_window_end(int32_t list_id, int32_t selected);
void list_request(int32_t list_id);
void list_pop_submenus(void);     // leave only the root chats list on the stack
void list_open(int32_t list_id);  // push a list on top of whatever is in front
void list_phone_ready(void);      // the phone connected; retry anything that timed out

void dictation_start(void);
void dictation_start_redo(int32_t turn_index);
bool dictation_active(void);
void dictation_cleanup(void);

void toast_show(const char *text);
bool toast_active(void);
void toast_draw(GContext *ctx, GRect bounds);

// ---------------------------------------------------------------------------
// Reminders (timers.c)
// ---------------------------------------------------------------------------

void timers_init(void);
void timers_set(time_t when, int32_t cookie, const char *title);
void timers_clear(int32_t cookie);
bool timers_launch_cookie(int32_t *cookie_out);
const char *timers_title_for(int32_t cookie);

// ---------------------------------------------------------------------------
// Small helpers (util.c)
// ---------------------------------------------------------------------------

void str_append(char *dst, size_t cap, const char *src);
void str_copy(char *dst, size_t cap, const char *src);
int32_t ease_out_cubic(int32_t elapsed, int32_t duration, int32_t distance);
void vibe_soft(void);
void vibe_bump(void);
void vibe_alert(void);
