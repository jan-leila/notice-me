#include "nm_session.h"
#include <string.h>

void nm_settings_defaults(NmSettings *s) {
  s->focus_min = 25;
  s->break_min = 5;
  s->long_break_min = 15;
  s->rounds = 4;
  s->nudge_min = 5;
  s->show_max = 3;
  s->sensitivity = 1;
}

static uint8_t clamp(int v, int lo, int hi) {
  return (uint8_t)(v < lo ? lo : v > hi ? hi : v);
}

void nm_settings_from_bytes(NmSettings *s, const uint8_t *bytes, int len) {
  if (len > 0) s->focus_min = clamp(bytes[0], 1, 120);
  if (len > 1) s->break_min = clamp(bytes[1], 1, 60);
  if (len > 2) s->long_break_min = clamp(bytes[2], 1, 60);
  if (len > 3) s->rounds = clamp(bytes[3], 1, 8);
  if (len > 4) s->nudge_min = clamp(bytes[4], 1, 60);
  if (len > 5) s->show_max = clamp(bytes[5], 1, 5);
  if (len > 6) s->sensitivity = clamp(bytes[6], 0, 2);
}

void nm_session_start(NmSession *s, const NmSettings *cfg, uint32_t task_id, bool active, int32_t now) {
  memset(s, 0, sizeof *s);
  s->phase = NM_PHASE_FOCUS;
  s->round = 1;
  s->active = active;
  s->task_id = task_id;
  s->phase_end = now + cfg->focus_min * 60;
}

void nm_session_stop(NmSession *s) {
  memset(s, 0, sizeof *s);
}

void nm_session_pause(NmSession *s, int32_t now) {
  if (s->phase == NM_PHASE_IDLE || s->paused) return;
  s->remaining = s->phase_end > now ? s->phase_end - now : 0;
  s->paused = 1;
}

void nm_session_resume(NmSession *s, int32_t now) {
  if (s->phase == NM_PHASE_IDLE || !s->paused) return;
  s->phase_end = now + s->remaining;
  s->remaining = 0;
  s->paused = 0;
}

// The phase after the current one, starting at `at`.
static void next_phase(NmSession *s, const NmSettings *cfg, int32_t at) {
  switch (s->phase) {
    case NM_PHASE_FOCUS:
      if (s->round >= cfg->rounds) {
        s->phase = NM_PHASE_LONG_BREAK;
        s->phase_end = at + cfg->long_break_min * 60;
      } else {
        s->phase = NM_PHASE_BREAK;
        s->phase_end = at + cfg->break_min * 60;
      }
      break;
    case NM_PHASE_BREAK:
      s->phase = NM_PHASE_FOCUS;
      s->round++;
      s->phase_end = at + cfg->focus_min * 60;
      break;
    default:
      nm_session_stop(s);
      break;
  }
}

int nm_session_advance(NmSession *s, const NmSettings *cfg, int32_t now) {
  int n = 0;
  while (s->phase != NM_PHASE_IDLE && !s->paused && now >= s->phase_end) {
    next_phase(s, cfg, s->phase_end);
    n++;
  }
  return n;
}

int nm_session_skip(NmSession *s, const NmSettings *cfg, int32_t now) {
  if (s->phase == NM_PHASE_IDLE) return 0;
  s->paused = 0;
  s->remaining = 0;
  next_phase(s, cfg, now);
  return 1;
}

int32_t nm_session_remaining(const NmSession *s, int32_t now) {
  if (s->phase == NM_PHASE_IDLE) return 0;
  if (s->paused) return s->remaining;
  return s->phase_end > now ? s->phase_end - now : 0;
}

bool nm_session_watching(const NmSession *s, int32_t now) {
  return s->phase == NM_PHASE_FOCUS && !s->paused && s->active && now < s->phase_end;
}
