// Host-side tests for src/c/nm_tasks.c — the task list from the phone (CONTRACTS.md §2, §3).
#include <stdio.h>
#include <string.h>
static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(got, want) do { checks++; long g_ = (long)(got), w_ = (long)(want); if (g_ != w_) { failures++; \
  printf("FAIL %s:%d: %s = %ld, want %ld\n", __FILE__, __LINE__, #got, g_, w_); } } while (0)
#include "nm_tasks.h"

static NmTask def(uint32_t id, const char *title, uint8_t flags, uint8_t priority) {
  NmTask d;
  memset(&d, 0, sizeof d);
  d.id = id;
  d.flags = flags;
  d.priority = priority;
  size_t n = strlen(title);
  memcpy(d.title, title, n < NM_TITLE_LEN - 1 ? n : NM_TITLE_LEN - 1);
  return d;
}

// Sends a complete list: ids in order, flags per entry.
static void send(NmTasks *t, int n, const uint32_t *ids, const uint8_t *flags) {
  nm_tasks_sync_begin(t, n);
  for (int i = 0; i < n; i++) {
    NmTask d = def(ids[i], "task", flags ? flags[i] : 0, 0);
    CHECK(nm_tasks_sync_put(t, i, &d));
  }
  CHECK(nm_tasks_sync_finish(t));
}

static void test_sync(void) {
  NmTasks t;
  nm_tasks_init(&t);
  NmTask d = def(7, "early", 0, 0);
  CHECK(!nm_tasks_sync_put(&t, 0, &d));  // no header yet
  CHECK(!nm_tasks_sync_finish(&t));

  nm_tasks_sync_begin(&t, 3);
  d = def(30, "third", NM_TASK_ACTIVE, 1);
  CHECK(nm_tasks_sync_put(&t, 2, &d));   // entries may arrive in any order
  d = def(10, "first", 0, 5);
  CHECK(nm_tasks_sync_put(&t, 0, &d));
  CHECK(!nm_tasks_sync_finish(&t));
  d = def(99, "bad", 0, 0);
  CHECK(!nm_tasks_sync_put(&t, 3, &d));
  CHECK(!nm_tasks_sync_put(&t, -1, &d));
  d = def(0, "no id", 0, 0);
  CHECK(!nm_tasks_sync_put(&t, 1, &d));
  d = def(20, "second, with a title that is far too long to fit in forty bytes", NM_TASK_STARTED, 3);
  CHECK(nm_tasks_sync_put(&t, 1, &d));
  CHECK(nm_tasks_sync_finish(&t));
  CHECK(!nm_tasks_sync_finish(&t));      // only once
  CHECK_EQ(t.count, 3);
  CHECK_EQ(t.items[0].id, 10); CHECK_EQ(t.items[1].id, 20); CHECK_EQ(t.items[2].id, 30);
  CHECK_EQ(t.items[0].priority, 5);
  CHECK_EQ(t.items[2].flags, NM_TASK_ACTIVE);
  CHECK_EQ(strlen(t.items[1].title), NM_TITLE_LEN - 1);
  CHECK(nm_tasks_find(&t, 20) == &t.items[1]);
  CHECK(nm_tasks_find(&t, 21) == NULL);

  // a new list: 20 is gone, 40 is new and first, 10 changed
  nm_tasks_sync_begin(&t, 3);
  d = def(40, "new", 0, 5);
  nm_tasks_sync_put(&t, 0, &d);
  d = def(10, "first again", NM_TASK_STARTED, 4);
  nm_tasks_sync_put(&t, 1, &d);
  CHECK_EQ(t.count, 4);                  // nothing is dropped before the list is complete
  d = def(30, "third", 0, 1);
  nm_tasks_sync_put(&t, 2, &d);
  CHECK(nm_tasks_sync_finish(&t));
  CHECK_EQ(t.count, 3);
  CHECK_EQ(t.items[0].id, 40); CHECK_EQ(t.items[1].id, 10); CHECK_EQ(t.items[2].id, 30);
  CHECK(strcmp(t.items[1].title, "first again") == 0);
  CHECK_EQ(t.items[1].flags, NM_TASK_STARTED);
  CHECK_EQ(t.items[2].flags, 0);         // the label was taken off

  // an empty list clears everything at once
  nm_tasks_sync_begin(&t, 0);
  CHECK(nm_tasks_sync_finish(&t));
  CHECK_EQ(t.count, 0);

  // more than fits: the count is clamped
  nm_tasks_sync_begin(&t, 99);
  CHECK_EQ(t.sync_count, NM_TASK_MAX);
  for (int i = 0; i < NM_TASK_MAX; i++) {
    d = def((uint32_t)(100 + i), "many", 0, 0);
    CHECK(nm_tasks_sync_put(&t, i, &d));
  }
  CHECK(nm_tasks_sync_finish(&t));
  CHECK_EQ(t.count, NM_TASK_MAX);
  // a full list replaced by other tasks: new entries take the place of ones the new list has not named,
  // but never of one with something still to report
  nm_task_start(&t.items[0]);
  nm_tasks_sync_begin(&t, NM_TASK_MAX);
  for (int i = 0; i < NM_TASK_MAX - 1; i++) {
    d = def((uint32_t)(500 + i), "other", 0, 0);
    CHECK(nm_tasks_sync_put(&t, i, &d));
  }
  d = def(599, "one too many", 0, 0);
  CHECK(!nm_tasks_sync_put(&t, NM_TASK_MAX - 1, &d));   // only the unreported task's slot is left
  d = def(100, "many", 0, 0);
  CHECK(nm_tasks_sync_put(&t, NM_TASK_MAX - 1, &d));
  CHECK(nm_tasks_sync_finish(&t));
  CHECK_EQ(t.count, NM_TASK_MAX);
  CHECK_EQ(t.items[0].id, 500);
  CHECK_EQ(t.items[NM_TASK_MAX - 1].id, 100);
  CHECK(t.items[NM_TASK_MAX - 1].flags & NM_TASK_SEND_START);
}

static void test_visible(void) {
  NmTasks t;
  uint8_t out[8];
  int more = -1;
  nm_tasks_init(&t);
  CHECK_EQ(nm_tasks_visible(&t, out, 3, &more), 0);
  CHECK_EQ(more, 0);
  const uint32_t ids[] = { 1, 2, 3, 4, 5 };
  const uint8_t flags[] = { 0, 0, NM_TASK_STARTED, 0, NM_TASK_STARTED };
  send(&t, 5, ids, flags);
  // in progress first, then the phone's order
  CHECK_EQ(nm_tasks_visible(&t, out, 3, &more), 3);
  CHECK_EQ(t.items[out[0]].id, 3); CHECK_EQ(t.items[out[1]].id, 5); CHECK_EQ(t.items[out[2]].id, 1);
  CHECK_EQ(more, 2);
  CHECK_EQ(nm_tasks_visible(&t, out, 8, &more), 5);
  CHECK_EQ(t.items[out[3]].id, 2); CHECK_EQ(t.items[out[4]].id, 4);
  CHECK_EQ(more, 0);
  CHECK_EQ(nm_tasks_visible(&t, out, 0, &more), 0);
  CHECK_EQ(more, 5);
  // finished tasks are out of sight and not counted
  nm_task_done(nm_tasks_find(&t, 3));
  CHECK_EQ(nm_tasks_visible(&t, out, 3, &more), 3);
  CHECK_EQ(t.items[out[0]].id, 5); CHECK_EQ(t.items[out[1]].id, 1); CHECK_EQ(t.items[out[2]].id, 2);
  CHECK_EQ(more, 1);
  // starting one moves it up
  nm_task_start(nm_tasks_find(&t, 4));
  CHECK_EQ(nm_tasks_visible(&t, out, 3, NULL), 3);
  CHECK_EQ(t.items[out[0]].id, 4); CHECK_EQ(t.items[out[1]].id, 5); CHECK_EQ(t.items[out[2]].id, 1);
}

static void test_events(void) {
  NmTasks t;
  NmTask *task;
  nm_tasks_init(&t);
  const uint32_t ids[] = { 1, 2, 3 };
  const uint8_t flags[] = { 0, NM_TASK_STARTED, 0 };
  send(&t, 3, ids, flags);
  CHECK_EQ(nm_tasks_next_event(&t, &task), NM_EVENT_NONE);
  CHECK(task == NULL);

  nm_task_start(nm_tasks_find(&t, 2));   // already in progress: nothing to report
  CHECK_EQ(nm_tasks_next_event(&t, &task), NM_EVENT_NONE);
  nm_task_start(nm_tasks_find(&t, 3));
  CHECK_EQ(nm_tasks_next_event(&t, &task), NM_EVENT_START);
  CHECK_EQ(task->id, 3);

  // the phone's list still says "not started": the unreported start outlasts it
  send(&t, 3, ids, flags);
  CHECK(nm_tasks_find(&t, 3)->flags & NM_TASK_STARTED);
  CHECK_EQ(nm_tasks_next_event(&t, &task), NM_EVENT_START);
  nm_task_event_sent(task, NM_EVENT_START);
  CHECK_EQ(nm_tasks_next_event(&t, &task), NM_EVENT_NONE);
  CHECK(nm_tasks_find(&t, 3)->flags & NM_TASK_STARTED);
  // once reported, the phone's word counts again
  send(&t, 3, ids, flags);
  CHECK(!(nm_tasks_find(&t, 3)->flags & NM_TASK_STARTED));

  // started and finished before the phone heard of either: one "done" event
  nm_task_start(nm_tasks_find(&t, 1));
  nm_task_done(nm_tasks_find(&t, 1));
  CHECK_EQ(nm_tasks_next_event(&t, &task), NM_EVENT_DONE);
  CHECK_EQ(task->id, 1);
  send(&t, 3, ids, flags);               // still hidden after a stale list
  CHECK(nm_tasks_find(&t, 1)->flags & NM_TASK_DONE);
  nm_task_event_sent(nm_tasks_find(&t, 1), NM_EVENT_DONE);
  CHECK_EQ(nm_tasks_next_event(&t, &task), NM_EVENT_NONE);
  CHECK(nm_tasks_find(&t, 1)->flags & NM_TASK_DONE);
  // the phone's next list no longer has it
  const uint32_t rest[] = { 2, 3 };
  send(&t, 2, rest, NULL);
  CHECK(nm_tasks_find(&t, 1) == NULL);
  CHECK_EQ(t.count, 2);
}

int main(void) {
  test_sync();
  test_visible();
  test_events();
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
