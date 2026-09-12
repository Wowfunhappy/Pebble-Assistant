#include "assistant.h"

static Theme s_theme;
static bool s_theme_ready;
static int32_t s_font_scale;   // 0 normal, 1 large, 2 extra large

const Theme *theme(void) {
  if (!s_theme_ready) {
    s_theme.background = GColorBlack;
    s_theme.surface    = PBL_IF_COLOR_ELSE(GColorOxfordBlue, GColorBlack);
    s_theme.accent     = PBL_IF_COLOR_ELSE(GColorVividCerulean, GColorWhite);
    s_theme.accent_dim = PBL_IF_COLOR_ELSE(GColorCobaltBlue, GColorWhite);
    s_theme.text       = GColorWhite;
    s_theme.text_dim   = PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite);
    s_theme.question   = PBL_IF_COLOR_ELSE(GColorPastelYellow, GColorWhite);
    s_theme.danger     = PBL_IF_COLOR_ELSE(GColorMelon, GColorWhite);
    s_theme_ready = true;
  }
  return &s_theme;
}

void theme_set_font_scale(int32_t index) {
  if (index < 0) index = 0;
  if (index > 2) index = 2;
  s_font_scale = index;
}

// Pebble Time 2 (Emery) is 200x228 and carries a noticeably denser panel than
// the 144x168 platforms, so it gets its own font ladder rather than scaled-up
// versions of the small one.
GFont theme_font_body(void) {
#ifdef PBL_PLATFORM_EMERY
  switch (s_font_scale) {
    case 2:  return fonts_get_system_font(FONT_KEY_GOTHIC_28);
    case 1:  return fonts_get_system_font(FONT_KEY_GOTHIC_24);
    default: return fonts_get_system_font(FONT_KEY_GOTHIC_24);
  }
#else
  switch (s_font_scale) {
    case 2:  return fonts_get_system_font(FONT_KEY_GOTHIC_24);
    case 1:  return fonts_get_system_font(FONT_KEY_GOTHIC_18);
    default: return fonts_get_system_font(FONT_KEY_GOTHIC_18);
  }
#endif
}

GFont theme_font_question(void) {
#ifdef PBL_PLATFORM_EMERY
  return fonts_get_system_font(s_font_scale >= 2 ? FONT_KEY_GOTHIC_24_BOLD
                                                 : FONT_KEY_GOTHIC_18_BOLD);
#else
  return fonts_get_system_font(s_font_scale >= 2 ? FONT_KEY_GOTHIC_18_BOLD
                                                 : FONT_KEY_GOTHIC_14_BOLD);
#endif
}

GFont theme_font_title(void) {
#ifdef PBL_PLATFORM_EMERY
  return fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
#else
  return fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
#endif
}

GFont theme_font_small(void) {
  return fonts_get_system_font(FONT_KEY_GOTHIC_14);
}

int16_t theme_header_height(void) {
#ifdef PBL_PLATFORM_EMERY
  return 26;
#else
  return 20;
#endif
}

// Honours the watch's own 12/24-hour setting.
static void header_clock(char *buffer, size_t capacity) {
  time_t now = time(NULL);
  struct tm *local = localtime(&now);
  if (clock_is_24h_style()) {
    strftime(buffer, capacity, "%H:%M", local);
  } else {
    strftime(buffer, capacity, "%I:%M", local);
    if (buffer[0] == '0') memmove(buffer, buffer + 1, strlen(buffer));
  }
}

static int16_t text_width(const char *text, GFont font, int16_t limit) {
  return graphics_text_layout_get_content_size(text, font, GRect(0, 0, limit, 40),
             GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft).w;
}

// A compact title bar: accent rule, left title, then the optional badge and the
// clock packed against the right edge.  It is a watch; the time earns its place.
void theme_draw_header(GContext *ctx, GRect bounds, const char *left, const char *right) {
  const Theme *t = theme();
  const int16_t height = theme_header_height();
  const int16_t top = bounds.origin.y;

  graphics_context_set_fill_color(ctx, t->surface);
  graphics_fill_rect(ctx, GRect(bounds.origin.x, top, bounds.size.w, height), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, t->accent);
  graphics_fill_rect(ctx, GRect(bounds.origin.x, top + height - 2, bounds.size.w, 2), 0, GCornerNone);

  GFont small = theme_font_small();
  int16_t edge = bounds.origin.x + bounds.size.w - 4;
  int16_t baseline = top + (height - 16) / 2 - 2;

  char clock_text[10];
  header_clock(clock_text, sizeof(clock_text));
  int16_t clock_w = text_width(clock_text, small, bounds.size.w);
  graphics_context_set_text_color(ctx, t->text_dim);
  graphics_draw_text(ctx, clock_text, small, GRect(edge - clock_w, baseline, clock_w, 18),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
  edge -= clock_w + 6;

  if (right && right[0]) {
    int16_t badge_w = text_width(right, small, bounds.size.w);
    graphics_context_set_text_color(ctx, t->accent);
    graphics_draw_text(ctx, right, small, GRect(edge - badge_w, baseline, badge_w, 18),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
    edge -= badge_w + 6;
  }

  if (left && left[0]) {
    int16_t title_w = edge - (bounds.origin.x + 4);
    if (title_w > 8) {
      graphics_context_set_text_color(ctx, t->text);
      graphics_draw_text(ctx, left, theme_font_title(),
          GRect(bounds.origin.x + 4, top - 1, title_w, height),
          GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    }
  }
}
