#include "assistant.h"

// Launching straight into dictation is the point of the app, but give the root
// window one frame to paint first so backing out of the microphone reveals a
// drawn chats list rather than a black screen.
#define AUTO_DICTATION_DELAY_MS 60

static AppTimer *s_boot_timer;

static void boot_dictation(void *data) {
  s_boot_timer = NULL;
  dictation_start();
}

// The header carries a clock, so it has to be redrawn when the minute turns.
static void minute_tick(struct tm *tick_time, TimeUnits units_changed) {
  reply_window_refresh();
  list_window_refresh();
}

static void init(void) {
  comm_init();
  timers_init();
  reply_window_init();
  list_window_init();
  list_push_root();
  tick_timer_service_subscribe(MINUTE_UNIT, minute_tick);
  comm_send_hello();

  int32_t cookie = -1;
  if (timers_launch_cookie(&cookie)) {
    // Woken by a reminder: show it instead of listening.
    reply_window_set_alert(timers_title_for(cookie), cookie);
  } else if (comm_flags() & SFLAG_AUTO_DICT) {
    s_boot_timer = app_timer_register(AUTO_DICTATION_DELAY_MS, boot_dictation, NULL);
  }
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  if (s_boot_timer) {
    app_timer_cancel(s_boot_timer);
    s_boot_timer = NULL;
  }
  dictation_cleanup();
  list_window_deinit();
  reply_window_deinit();
  comm_deinit();
}

int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}
