#include "assistant.h"

// ---------------------------------------------------------------------------
// On-watch reminders.
//
// Pebble gives an app eight Wakeup slots.  They fire even when the phone is out
// of range, which is the whole point of putting a timer here rather than only in
// the Timeline: "remind me in 20 minutes" has to work on a walk without a phone.
// The phone additionally pins the same reminder to the Timeline, so it is
// visible by scrolling forward from the watchface.
// ---------------------------------------------------------------------------

#define PERSIST_KEY_WAKEUPS 100
#define WAKEUP_TITLE_LEN 48

typedef struct {
  WakeupId id;
  int32_t cookie;
  int32_t when;               // unix seconds; 0 marks a free slot
  char title[WAKEUP_TITLE_LEN];
} WakeupSlot;

static WakeupSlot s_slots[MAX_WAKEUPS];
static bool s_launched_by_wakeup;
static int32_t s_launch_cookie = -1;

static void save_slots(void) {
  persist_write_data(PERSIST_KEY_WAKEUPS, s_slots, sizeof(s_slots));
}

static void load_slots(void) {
  memset(s_slots, 0, sizeof(s_slots));
  if (persist_exists(PERSIST_KEY_WAKEUPS)) {
    persist_read_data(PERSIST_KEY_WAKEUPS, s_slots, sizeof(s_slots));
  }
  // Drop slots the system no longer knows about (fired, cancelled, or stale
  // after a reinstall) so the eight-slot budget does not leak.
  time_t query;
  for (int i = 0; i < MAX_WAKEUPS; i++) {
    if (s_slots[i].when == 0) continue;
    if (s_slots[i].id < 0 || !wakeup_query(s_slots[i].id, &query)) {
      memset(&s_slots[i], 0, sizeof(WakeupSlot));
    }
  }
  save_slots();
}

static int find_slot_by_cookie(int32_t cookie) {
  for (int i = 0; i < MAX_WAKEUPS; i++) {
    if (s_slots[i].when != 0 && s_slots[i].cookie == cookie) return i;
  }
  return -1;
}

static int find_free_slot(void) {
  for (int i = 0; i < MAX_WAKEUPS; i++) {
    if (s_slots[i].when == 0) return i;
  }
  // All eight are taken: recycle whichever fires furthest out.
  int furthest = 0;
  for (int i = 1; i < MAX_WAKEUPS; i++) {
    if (s_slots[i].when > s_slots[furthest].when) furthest = i;
  }
  if (s_slots[furthest].id >= 0) wakeup_cancel(s_slots[furthest].id);
  memset(&s_slots[furthest], 0, sizeof(WakeupSlot));
  return furthest;
}

static void wakeup_handler(WakeupId wakeup_id, int32_t cookie) {
  int slot = find_slot_by_cookie(cookie);
  const char *title = (slot >= 0) ? s_slots[slot].title : "Reminder";
  if (slot >= 0) memset(&s_slots[slot], 0, sizeof(WakeupSlot));
  save_slots();

  vibe_alert();
  light_enable_interaction();
  reply_window_set_alert(title, cookie);
  comm_send(WREQ_WAKEUP_FIRED, NULL, cookie, 0);
}

void timers_init(void) {
  load_slots();
  wakeup_service_subscribe(wakeup_handler);

  if (launch_reason() == APP_LAUNCH_WAKEUP) {
    WakeupId id;
    int32_t cookie;
    if (wakeup_get_launch_event(&id, &cookie)) {
      s_launched_by_wakeup = true;
      s_launch_cookie = cookie;
      int slot = find_slot_by_cookie(cookie);
      if (slot >= 0) {
        memset(&s_slots[slot], 0, sizeof(WakeupSlot));
        save_slots();
      }
    }
  }
}

void timers_set(time_t when, int32_t cookie, const char *title) {
  // A wakeup in the past (or within the service's minimum lead time) is
  // rejected by the firmware, so surface it immediately instead.
  if (when <= time(NULL) + 30) {
    vibe_alert();
    reply_window_set_alert(title ? title : "Reminder", cookie);
    return;
  }

  int existing = find_slot_by_cookie(cookie);
  if (existing >= 0) {
    if (s_slots[existing].id >= 0) wakeup_cancel(s_slots[existing].id);
    memset(&s_slots[existing], 0, sizeof(WakeupSlot));
  }

  // Another app may already own the exact minute; nudge forward and retry.
  WakeupId id = -1;
  for (int attempt = 0; attempt < 4 && id < 0; attempt++) {
    id = wakeup_schedule((time_t)(when + attempt * 60), cookie, true);
  }
  if (id < 0) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "wakeup_schedule failed: %d", (int)id);
    toast_show("Watch alarm full");
    return;
  }

  int slot = find_free_slot();
  s_slots[slot].id = id;
  s_slots[slot].cookie = cookie;
  s_slots[slot].when = (int32_t)when;
  str_copy(s_slots[slot].title, WAKEUP_TITLE_LEN, title ? title : "Reminder");
  save_slots();
}

void timers_clear(int32_t cookie) {
  if (cookie < 0) {
    for (int i = 0; i < MAX_WAKEUPS; i++) {
      if (s_slots[i].when != 0 && s_slots[i].id >= 0) wakeup_cancel(s_slots[i].id);
    }
    memset(s_slots, 0, sizeof(s_slots));
  } else {
    int slot = find_slot_by_cookie(cookie);
    if (slot < 0) return;
    if (s_slots[slot].id >= 0) wakeup_cancel(s_slots[slot].id);
    memset(&s_slots[slot], 0, sizeof(WakeupSlot));
  }
  save_slots();
}

bool timers_launch_cookie(int32_t *cookie_out) {
  if (cookie_out) *cookie_out = s_launch_cookie;
  return s_launched_by_wakeup;
}

const char *timers_title_for(int32_t cookie) {
  int slot = find_slot_by_cookie(cookie);
  return (slot >= 0) ? s_slots[slot].title : "Reminder";
}
