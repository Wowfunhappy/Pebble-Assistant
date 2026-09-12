#include "assistant.h"

// ---------------------------------------------------------------------------
// The toast overlay.
//
// Toasts are raised by things that finish on one screen and leave you on
// another -- changing model, deleting a chat -- so the state lives here rather
// than inside a single window, and every window draws it.
// ---------------------------------------------------------------------------

#define TOAST_MS 1700
#define TOAST_LEN 48

static char s_text[TOAST_LEN];
static uint64_t s_expires_at;

static uint64_t now_ms(void) {
  time_t seconds;
  uint16_t millis;
  time_ms(&seconds, &millis);
  return (uint64_t)seconds * 1000 + millis;
}

void toast_show(const char *text) {
  if (!text || !text[0]) return;
  str_copy(s_text, TOAST_LEN, text);
  s_expires_at = now_ms() + TOAST_MS;
  vibe_bump();
}

bool toast_active(void) {
  return s_text[0] != '\0' && now_ms() < s_expires_at;
}

void toast_draw(GContext *ctx, GRect bounds) {
  if (!toast_active()) return;
  const Theme *t = theme();
  int16_t height = 22;
  GRect box = GRect(bounds.origin.x + 4, bounds.origin.y + bounds.size.h - height - 4,
                    bounds.size.w - 8, height);
  graphics_context_set_fill_color(ctx, t->accent);
  graphics_fill_rect(ctx, box, 4, GCornersAll);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, s_text, theme_font_small(),
                     GRect(box.origin.x + 4, box.origin.y + 1, box.size.w - 8, height),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}
