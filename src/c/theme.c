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

// A compact title bar: accent rule, left title, optional right-aligned badge.
void theme_draw_header(GContext *ctx, GRect bounds, const char *left, const char *right) {
  const Theme *t = theme();
  const int16_t height = theme_header_height();

  graphics_context_set_fill_color(ctx, t->surface);
  graphics_fill_rect(ctx, GRect(bounds.origin.x, bounds.origin.y, bounds.size.w, height), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, t->accent);
  graphics_fill_rect(ctx, GRect(bounds.origin.x, bounds.origin.y + height - 2, bounds.size.w, 2), 0, GCornerNone);

  GFont font = theme_font_title();
  int16_t right_w = 0;
  if (right && right[0]) {
    right_w = graphics_text_layout_get_content_size(right, font,
                  GRect(0, 0, bounds.size.w, height), GTextOverflowModeTrailingEllipsis,
                  GTextAlignmentRight).w + 6;
    graphics_context_set_text_color(ctx, t->accent);
    graphics_draw_text(ctx, right, font,
        GRect(bounds.origin.x + bounds.size.w - right_w - 4, bounds.origin.y - 1, right_w, height),
        GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
  }
  if (left && left[0]) {
    graphics_context_set_text_color(ctx, t->text);
    graphics_draw_text(ctx, left, font,
        GRect(bounds.origin.x + 4, bounds.origin.y - 1, bounds.size.w - 8 - right_w, height),
        GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  }
}
