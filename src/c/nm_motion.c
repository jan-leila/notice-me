#include "nm_motion.h"
#include <string.h>

#define STREAK_STEP 2   // a moving batch adds this, a still one takes 1 away
#define STREAK_MOVE 6   // the streak that counts as real movement

void nm_motion_reset(NmMotion *m, int32_t now) {
  memset(m, 0, sizeof *m);
  m->last_move = now;
}

int nm_motion_threshold(int sensitivity) {
  return sensitivity <= 0 ? 150 : sensitivity == 1 ? 90 : 55;
}

static int32_t absdiff(int16_t a, int16_t b) {
  return a > b ? (int32_t)a - b : (int32_t)b - a;
}

bool nm_motion_feed(NmMotion *m, const int16_t *xyz, int n, int threshold, int32_t now) {
  int32_t sum = 0;
  int deltas = 0;
  for (int i = 0; i < n; i++) {
    int16_t x = xyz[3 * i], y = xyz[3 * i + 1], z = xyz[3 * i + 2];
    if (m->have_last) {
      sum += absdiff(x, m->x) + absdiff(y, m->y) + absdiff(z, m->z);
      deltas++;
    }
    m->x = x;
    m->y = y;
    m->z = z;
    m->have_last = true;
  }
  if (deltas == 0) return false;
  if (sum >= (int32_t)threshold * deltas) {
    m->streak += STREAK_STEP;
    if (m->streak >= STREAK_MOVE) {
      m->streak = STREAK_MOVE;
      m->last_move = now;
      m->last_nudge = 0;
      return true;
    }
  } else if (m->streak > 0) {
    m->streak--;
  }
  return false;
}

int32_t nm_motion_still_for(const NmMotion *m, int32_t now) {
  return now > m->last_move ? now - m->last_move : 0;
}

bool nm_motion_nudge_due(NmMotion *m, int32_t now, int32_t after) {
  int32_t since = m->last_nudge > m->last_move ? m->last_nudge : m->last_move;
  if (now - since < after) return false;
  m->last_nudge = now;
  return true;
}
