// Pure-C "sitting still" detector fed with accelerometer batches (no pebble.h; host-tested in
// test/c/test_nm_motion.c). Shared by the background worker. See CONTRACTS.md §5.
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
  int16_t x, y, z;          // last sample, milli-g
  bool have_last;
  uint8_t streak;           // recent moving batches (see nm_motion_feed)
  int32_t last_move;        // unix seconds of the last real movement (or of the reset)
  int32_t last_nudge;       // unix seconds of the last nudge, 0 = none since the reset
} NmMotion;

// Starts the stillness clock at `now`, as if the wearer had just moved.
void nm_motion_reset(NmMotion *m, int32_t now);
// Mean change per sample (|dx| + |dy| + |dz|, milli-g) from which a batch counts as moving, for a sensitivity of
// 0 (low: needs big movements), 1 or 2 (high: small movements count).
int nm_motion_threshold(int sensitivity);
// One batch of n samples (xyz triples, milli-g) taken around `now`. A batch moves when its mean change per sample
// reaches the threshold; three moving batches close together are real movement (one reach for a cup is not).
// Returns true when this batch moved the stillness clock.
bool nm_motion_feed(NmMotion *m, const int16_t *xyz, int n, int threshold, int32_t now);
int32_t nm_motion_still_for(const NmMotion *m, int32_t now);
// True when the wearer has been still for `after` seconds since they last moved or were last nudged; the nudge is
// then recorded, so the next one is due `after` seconds later.
bool nm_motion_nudge_due(NmMotion *m, int32_t now, int32_t after);
