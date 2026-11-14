// Pure-C task list: the tasks the phone sends from the task server (already sorted by priority, then age), what was
// started or finished on the watch, and which few are shown (no pebble.h; host-tested in test/c/test_nm_tasks.c).
// See CONTRACTS.md §2 and §3.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define NM_TASK_MAX 16
#define NM_TITLE_LEN 40            // bytes, with the terminating NUL

#define NM_TASK_ACTIVE     0x01    // carries the "active" label: sitting still during its focus time gets a nudge
#define NM_TASK_STARTED    0x02    // in progress
#define NM_TASK_DONE       0x04    // finished on the watch
#define NM_TASK_SEND_START 0x40    // watch only: started here, the phone has not been told
#define NM_TASK_SEND_DONE  0x80    // watch only: finished here, the phone has not been told

typedef enum { NM_EVENT_NONE, NM_EVENT_START, NM_EVENT_DONE } NmEvent;

typedef struct {
  uint32_t id;                     // the server's task id, never 0
  uint8_t flags;                   // NM_TASK_*
  uint8_t priority;                // the server's, 0 (none) .. 5 (highest); the phone has already sorted by it
  uint8_t order;                   // position in the phone's list
  char title[NM_TITLE_LEN];
} NmTask;

typedef struct {
  NmTask items[NM_TASK_MAX];
  uint8_t count;
  // a list being received from the phone
  bool syncing;
  uint8_t sync_count;
  uint32_t sync_got;               // bit i: entry i arrived
  uint32_t sync_seen;              // bit k: items[k] is in the new list
} NmTasks;

void nm_tasks_init(NmTasks *t);
NmTask *nm_tasks_find(NmTasks *t, uint32_t id);

// The (at most max) tasks to show, as indexes into t->items: the ones in progress first, then the rest, each group
// in the phone's order. *more gets the number of open tasks left out. Returns how many indexes were written.
int nm_tasks_visible(const NmTasks *t, uint8_t *out, int max, int *more);

void nm_task_start(NmTask *task);  // in progress; to be reported unless it already was
void nm_task_done(NmTask *task);   // finished; to be reported
// The next thing to tell the phone, with its task in *task, or NM_EVENT_NONE. A finished task reports only that.
NmEvent nm_tasks_next_event(NmTasks *t, NmTask **task);
// The phone has the event.
void nm_task_event_sent(NmTask *task, NmEvent event);

// Receiving the phone's list (TASK_COUNT, then one entry per message). What the phone has not been told yet
// (a start, a finish) outlasts the new list; tasks missing from the complete list are dropped.
void nm_tasks_sync_begin(NmTasks *t, int count);
// def: id, flags (ACTIVE, STARTED), priority, title. False when no list is being received, the index is out of
// range or the list is full of tasks the new list also names.
bool nm_tasks_sync_put(NmTasks *t, int index, const NmTask *def);
// True once, when every entry has arrived: the list is then complete and in the phone's order.
bool nm_tasks_sync_finish(NmTasks *t);
