#include "assistant.h"

void str_copy(char *dst, size_t cap, const char *src) {
  if (!dst || cap == 0) return;
  if (!src) { dst[0] = '\0'; return; }
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

void str_append(char *dst, size_t cap, const char *src) {
  if (!dst || !src || cap == 0) return;
  size_t used = strlen(dst);
  if (used + 1 >= cap) return;
  size_t room = cap - used - 1;
  strncpy(dst + used, src, room);
  dst[cap - 1] = '\0';
}

// Cubic ease-out in 1/1024 fixed point.  Returns how far along `distance` the
// animation has travelled after `elapsed` of `duration` milliseconds.
int32_t ease_out_cubic(int32_t elapsed, int32_t duration, int32_t distance) {
  if (duration <= 0 || elapsed >= duration) return distance;
  if (elapsed <= 0) return 0;
  int32_t inv = 1024 - (elapsed * 1024) / duration;   // 1024 -> 0
  int32_t inv3 = (((inv * inv) / 1024) * inv) / 1024; // (1 - t)^3
  return (distance * (1024 - inv3)) / 1024;
}

void vibe_bump(void) {
  static const uint32_t segments[] = { 18 };
  VibePattern pattern = { .durations = segments, .num_segments = 1 };
  vibes_enqueue_custom_pattern(pattern);
}

void vibe_soft(void) {
  static const uint32_t segments[] = { 45 };
  VibePattern pattern = { .durations = segments, .num_segments = 1 };
  vibes_enqueue_custom_pattern(pattern);
}

void vibe_alert(void) {
  static const uint32_t segments[] = { 250, 120, 250, 120, 450 };
  VibePattern pattern = { .durations = segments, .num_segments = 5 };
  vibes_enqueue_custom_pattern(pattern);
}
