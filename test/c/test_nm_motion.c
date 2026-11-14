// Host-side tests for src/c/nm_motion.c — noticing that the wearer sits still (CONTRACTS.md §5).
#include <stdio.h>
#include <string.h>
static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(got, want) do { checks++; long g_ = (long)(got), w_ = (long)(want); if (g_ != w_) { failures++; \
  printf("FAIL %s:%d: %s = %ld, want %ld\n", __FILE__, __LINE__, #got, g_, w_); } } while (0)
#include "nm_motion.h"

#define N 25

// A batch whose samples swing by `swing` milli-g on every axis from one sample to the next.
static void batch(int16_t *xyz, int swing) {
  for (int i = 0; i < N; i++) {
    int16_t v = (int16_t)((i % 2) ? swing : 0);
    xyz[3 * i] = v;
    xyz[3 * i + 1] = (int16_t)(v - 20);
    xyz[3 * i + 2] = (int16_t)(-1000 + v);
  }
}

static void test_threshold(void) {
  CHECK(nm_motion_threshold(0) > nm_motion_threshold(1));
  CHECK(nm_motion_threshold(1) > nm_motion_threshold(2));
  CHECK(nm_motion_threshold(2) > 0);
  CHECK_EQ(nm_motion_threshold(-3), nm_motion_threshold(0));
  CHECK_EQ(nm_motion_threshold(9), nm_motion_threshold(2));
}

static void test_feed(void) {
  NmMotion m;
  int16_t still[N * 3], moving[N * 3];
  int th = nm_motion_threshold(1);
  batch(still, 5);       // 15 milli-g of change per sample: sensor noise
  batch(moving, 100);    // 300 per sample: walking
  nm_motion_reset(&m, 1000);
  CHECK_EQ(nm_motion_still_for(&m, 1000), 0);
  CHECK_EQ(nm_motion_still_for(&m, 1060), 60);
  CHECK_EQ(nm_motion_still_for(&m, 900), 0);

  for (int i = 0; i < 100; i++) CHECK(!nm_motion_feed(&m, still, N, th, 1003 + i * 3));
  CHECK_EQ(m.last_move, 1000);
  // one burst (reaching for a cup) is not getting up
  CHECK(!nm_motion_feed(&m, moving, N, th, 1400));
  CHECK(!nm_motion_feed(&m, still, N, th, 1403));
  CHECK(!nm_motion_feed(&m, still, N, th, 1406));
  CHECK(!nm_motion_feed(&m, moving, N, th, 1409));
  CHECK_EQ(m.last_move, 1000);
  // three moving batches in a row are
  nm_motion_reset(&m, 1000);
  CHECK(!nm_motion_feed(&m, moving, N, th, 2000));
  CHECK(!nm_motion_feed(&m, moving, N, th, 2003));
  CHECK(nm_motion_feed(&m, moving, N, th, 2006));
  CHECK_EQ(m.last_move, 2006);
  CHECK(nm_motion_feed(&m, moving, N, th, 2009));     // and it keeps counting while it lasts
  CHECK_EQ(m.last_move, 2009);
  // with a still batch in between it takes one more
  nm_motion_reset(&m, 1000);
  CHECK(!nm_motion_feed(&m, moving, N, th, 2000));
  CHECK(!nm_motion_feed(&m, still, N, th, 2003));
  CHECK(!nm_motion_feed(&m, moving, N, th, 2006));
  CHECK(!nm_motion_feed(&m, moving, N, th, 2009));
  CHECK(nm_motion_feed(&m, moving, N, th, 2012));

  // sensitivity: a gentle movement counts only when it is set high
  int16_t gentle[N * 3];
  batch(gentle, 25);     // 75 per sample
  nm_motion_reset(&m, 0);
  for (int i = 0; i < 5; i++) CHECK(!nm_motion_feed(&m, gentle, N, nm_motion_threshold(1), 10 + i));
  nm_motion_reset(&m, 0);
  nm_motion_feed(&m, gentle, N, nm_motion_threshold(2), 10);
  nm_motion_feed(&m, gentle, N, nm_motion_threshold(2), 11);
  CHECK(nm_motion_feed(&m, gentle, N, nm_motion_threshold(2), 12));

  // empty and single-sample batches change nothing
  nm_motion_reset(&m, 0);
  CHECK(!nm_motion_feed(&m, moving, 0, th, 5));
  CHECK(!nm_motion_feed(&m, moving, 1, th, 5));
  CHECK_EQ(m.streak, 0);
  // the change is measured across batches too: a jump between two one-sample batches counts
  const int16_t a[3] = { 0, 0, -1000 }, b[3] = { 500, 0, -1000 };
  nm_motion_reset(&m, 0);
  nm_motion_feed(&m, a, 1, th, 1);
  nm_motion_feed(&m, b, 1, th, 2);
  CHECK_EQ(m.streak, 2);
}

static void test_nudge(void) {
  NmMotion m;
  int16_t moving[N * 3];
  int th = nm_motion_threshold(1);
  batch(moving, 100);
  nm_motion_reset(&m, 1000);
  CHECK(!nm_motion_nudge_due(&m, 1299, 300));
  CHECK(nm_motion_nudge_due(&m, 1300, 300));
  // still sitting: the next nudge comes one interval after the last
  CHECK(!nm_motion_nudge_due(&m, 1301, 300));
  CHECK(!nm_motion_nudge_due(&m, 1599, 300));
  CHECK(nm_motion_nudge_due(&m, 1600, 300));
  // got up: the clock starts again from the movement
  nm_motion_feed(&m, moving, N, th, 1700);
  nm_motion_feed(&m, moving, N, th, 1703);
  CHECK(nm_motion_feed(&m, moving, N, th, 1706));
  CHECK(!nm_motion_nudge_due(&m, 1900, 300));
  CHECK(!nm_motion_nudge_due(&m, 2005, 300));
  CHECK(nm_motion_nudge_due(&m, 2006, 300));
  // answering a nudge resets as well
  nm_motion_reset(&m, 2100);
  CHECK(!nm_motion_nudge_due(&m, 2399, 300));
  CHECK(nm_motion_nudge_due(&m, 2400, 300));
}

int main(void) {
  test_threshold();
  test_feed();
  test_nudge();
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
