// Host-side tests for src/c/nm_session.c — Pomodoro phases and the settings bytes (CONTRACTS.md §4).
#include <stdio.h>
#include <string.h>
static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(got, want) do { checks++; long g_ = (long)(got), w_ = (long)(want); if (g_ != w_) { failures++; \
  printf("FAIL %s:%d: %s = %ld, want %ld\n", __FILE__, __LINE__, #got, g_, w_); } } while (0)
#include "nm_session.h"

static void test_settings(void) {
  NmSettings s;
  nm_settings_defaults(&s);
  CHECK_EQ(s.focus_min, 25); CHECK_EQ(s.break_min, 5); CHECK_EQ(s.long_break_min, 15); CHECK_EQ(s.rounds, 4);
  CHECK_EQ(s.nudge_min, 5); CHECK_EQ(s.show_max, 3); CHECK_EQ(s.sensitivity, 1);
  const uint8_t bytes[NM_SETTINGS_BYTES] = { 50, 10, 30, 2, 8, 5, 2 };
  nm_settings_from_bytes(&s, bytes, NM_SETTINGS_BYTES);
  CHECK_EQ(s.focus_min, 50); CHECK_EQ(s.break_min, 10); CHECK_EQ(s.long_break_min, 30); CHECK_EQ(s.rounds, 2);
  CHECK_EQ(s.nudge_min, 8); CHECK_EQ(s.show_max, 5); CHECK_EQ(s.sensitivity, 2);
  // out of range values are clamped; a short array leaves the rest alone
  const uint8_t wild[] = { 0, 200, 0, 99 };
  nm_settings_from_bytes(&s, wild, 4);
  CHECK_EQ(s.focus_min, 1); CHECK_EQ(s.break_min, 60); CHECK_EQ(s.long_break_min, 1); CHECK_EQ(s.rounds, 8);
  CHECK_EQ(s.nudge_min, 8); CHECK_EQ(s.show_max, 5);
  const uint8_t tail[] = { 25, 5, 15, 4, 0, 9, 7 };
  nm_settings_from_bytes(&s, tail, 7);
  CHECK_EQ(s.nudge_min, 1); CHECK_EQ(s.show_max, 5); CHECK_EQ(s.sensitivity, 2);
  nm_settings_from_bytes(&s, tail, 0);
  CHECK_EQ(s.focus_min, 25);
}

static void test_phases(void) {
  NmSettings cfg;
  NmSession s;
  nm_settings_defaults(&cfg);
  cfg.rounds = 2;
  nm_session_stop(&s);
  CHECK_EQ(s.phase, NM_PHASE_IDLE);
  CHECK_EQ(nm_session_remaining(&s, 1000), 0);
  CHECK_EQ(nm_session_advance(&s, &cfg, 1000), 0);
  CHECK_EQ(nm_session_skip(&s, &cfg, 1000), 0);

  nm_session_start(&s, &cfg, 42, true, 1000);
  CHECK_EQ(s.phase, NM_PHASE_FOCUS); CHECK_EQ(s.round, 1); CHECK_EQ(s.task_id, 42); CHECK_EQ(s.active, 1);
  CHECK_EQ(s.phase_end, 1000 + 1500);
  CHECK_EQ(nm_session_remaining(&s, 1000), 1500);
  CHECK_EQ(nm_session_remaining(&s, 1600), 900);
  CHECK_EQ(nm_session_advance(&s, &cfg, 2499), 0);
  // focus 1 -> break
  CHECK_EQ(nm_session_advance(&s, &cfg, 2500), 1);
  CHECK_EQ(s.phase, NM_PHASE_BREAK); CHECK_EQ(s.round, 1);
  CHECK_EQ(s.phase_end, 2500 + 300);
  // break -> focus 2, a little late: the next phase still starts where the last one ended
  CHECK_EQ(nm_session_advance(&s, &cfg, 2810), 1);
  CHECK_EQ(s.phase, NM_PHASE_FOCUS); CHECK_EQ(s.round, 2);
  CHECK_EQ(s.phase_end, 2800 + 1500);
  // last focus -> long break -> the set is over
  CHECK_EQ(nm_session_advance(&s, &cfg, 4300), 1);
  CHECK_EQ(s.phase, NM_PHASE_LONG_BREAK);
  CHECK_EQ(s.phase_end, 4300 + 900);
  CHECK_EQ(nm_session_advance(&s, &cfg, 5200), 1);
  CHECK_EQ(s.phase, NM_PHASE_IDLE);
  CHECK_EQ(s.task_id, 0);

  // the watch was off for a long time: every missed phase is walked through
  nm_session_start(&s, &cfg, 42, false, 1000);
  CHECK_EQ(nm_session_advance(&s, &cfg, 2500 + 300 + 10), 2);
  CHECK_EQ(s.phase, NM_PHASE_FOCUS); CHECK_EQ(s.round, 2);
  nm_session_start(&s, &cfg, 42, false, 1000);
  CHECK_EQ(nm_session_advance(&s, &cfg, 1000000), 4);
  CHECK_EQ(s.phase, NM_PHASE_IDLE);
}

static void test_pause_skip(void) {
  NmSettings cfg;
  NmSession s;
  nm_settings_defaults(&cfg);
  nm_session_start(&s, &cfg, 1, true, 0);
  nm_session_pause(&s, 600);
  CHECK_EQ(s.paused, 1);
  CHECK_EQ(nm_session_remaining(&s, 99999), 900);
  CHECK_EQ(nm_session_advance(&s, &cfg, 99999), 0);   // a paused phase never ends
  CHECK(!nm_session_watching(&s, 700));
  nm_session_pause(&s, 800);                          // twice is harmless
  CHECK_EQ(nm_session_remaining(&s, 800), 900);
  nm_session_resume(&s, 5000);
  CHECK_EQ(s.paused, 0);
  CHECK_EQ(s.phase_end, 5900);
  nm_session_resume(&s, 6000);
  CHECK_EQ(s.phase_end, 5900);

  // skipping ends the phase now, also while paused
  CHECK_EQ(nm_session_skip(&s, &cfg, 5100), 1);
  CHECK_EQ(s.phase, NM_PHASE_BREAK);
  CHECK_EQ(s.phase_end, 5100 + 300);
  nm_session_pause(&s, 5200);
  CHECK_EQ(nm_session_skip(&s, &cfg, 5300), 1);
  CHECK_EQ(s.phase, NM_PHASE_FOCUS); CHECK_EQ(s.round, 2); CHECK_EQ(s.paused, 0);
  CHECK_EQ(s.phase_end, 5300 + 1500);
}

static void test_watching(void) {
  NmSettings cfg;
  NmSession s;
  nm_settings_defaults(&cfg);
  nm_session_stop(&s);
  CHECK(!nm_session_watching(&s, 0));
  nm_session_start(&s, &cfg, 1, false, 0);
  CHECK(!nm_session_watching(&s, 10));                // not an active task
  nm_session_start(&s, &cfg, 1, true, 0);
  CHECK(nm_session_watching(&s, 10));
  CHECK(!nm_session_watching(&s, 1500));              // the phase is over, even before anyone advanced it
  nm_session_advance(&s, &cfg, 1500);
  CHECK(!nm_session_watching(&s, 1510));              // breaks are for sitting
  nm_session_advance(&s, &cfg, 1800);
  CHECK(nm_session_watching(&s, 1810));
}

int main(void) {
  test_settings();
  test_phases();
  test_pause_skip();
  test_watching();
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
