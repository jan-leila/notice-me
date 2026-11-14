// Pure-C Pomodoro session: focus and break phases on the clock, and the settings the phone sends
// (no pebble.h; host-tested in test/c/test_nm_session.c). See CONTRACTS.md §4.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define NM_SETTINGS_BYTES 7

typedef struct {
  uint8_t focus_min;        // length of a focus phase
  uint8_t break_min;        // short break between focus phases
  uint8_t long_break_min;   // break after the last focus phase of a set
  uint8_t rounds;           // focus phases in a set
  uint8_t nudge_min;        // minutes of sitting still (active task, focus phase) before a nudge
  uint8_t show_max;         // tasks listed at once
  uint8_t sensitivity;      // movement detection: 0 low, 1 normal, 2 high
} NmSettings;

void nm_settings_defaults(NmSettings *s);
// The SETTINGS byte array of CONTRACTS.md §2 (fields in struct order); out-of-range values are clamped, missing
// trailing bytes keep their current value.
void nm_settings_from_bytes(NmSettings *s, const uint8_t *bytes, int len);

typedef enum { NM_PHASE_IDLE, NM_PHASE_FOCUS, NM_PHASE_BREAK, NM_PHASE_LONG_BREAK } NmPhase;

typedef struct {
  uint8_t phase;            // NmPhase
  uint8_t round;            // which focus phase of the set (1-based)
  uint8_t paused;
  uint8_t active;           // the task is not sedentary: the worker watches for sitting still
  uint32_t task_id;         // 0 = no task
  int32_t phase_end;        // unix seconds (while running)
  int32_t remaining;        // seconds left (while paused)
} NmSession;

void nm_session_start(NmSession *s, const NmSettings *cfg, uint32_t task_id, bool active, int32_t now);
void nm_session_stop(NmSession *s);
void nm_session_pause(NmSession *s, int32_t now);
void nm_session_resume(NmSession *s, int32_t now);
// Moves through every phase that has ended by `now` (each starts when the previous one ended; the set ends after
// its long break). Returns the number of phase changes.
int nm_session_advance(NmSession *s, const NmSettings *cfg, int32_t now);
// Ends the current phase now. Returns the number of phase changes (1, or 0 when idle).
int nm_session_skip(NmSession *s, const NmSettings *cfg, int32_t now);
int32_t nm_session_remaining(const NmSession *s, int32_t now);
// True while sitting still should be noticed: a running focus phase of an active task.
bool nm_session_watching(const NmSession *s, int32_t now);
