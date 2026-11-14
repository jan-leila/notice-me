#include "nm_tasks.h"
#include <string.h>

void nm_tasks_init(NmTasks *t) {
  memset(t, 0, sizeof *t);
}

NmTask *nm_tasks_find(NmTasks *t, uint32_t id) {
  for (int i = 0; i < t->count; i++) {
    if (t->items[i].id == id) return &t->items[i];
  }
  return NULL;
}

int nm_tasks_visible(const NmTasks *t, uint8_t *out, int max, int *more) {
  int n = 0, open = 0;
  for (int started = 1; started >= 0; started--) {
    for (int i = 0; i < t->count; i++) {
      const NmTask *task = &t->items[i];
      if ((task->flags & NM_TASK_DONE) || ((task->flags & NM_TASK_STARTED) != 0) != started) continue;
      open++;
      if (n < max) out[n++] = (uint8_t)i;
    }
  }
  if (more) *more = open - n;
  return n;
}

void nm_task_start(NmTask *task) {
  if (task->flags & (NM_TASK_STARTED | NM_TASK_DONE)) return;
  task->flags |= NM_TASK_STARTED | NM_TASK_SEND_START;
}

void nm_task_done(NmTask *task) {
  task->flags |= NM_TASK_DONE | NM_TASK_SEND_DONE;
}

NmEvent nm_tasks_next_event(NmTasks *t, NmTask **task) {
  for (int i = 0; i < t->count; i++) {
    uint8_t flags = t->items[i].flags;
    if (!(flags & (NM_TASK_SEND_START | NM_TASK_SEND_DONE))) continue;
    *task = &t->items[i];
    return (flags & NM_TASK_SEND_DONE) ? NM_EVENT_DONE : NM_EVENT_START;
  }
  *task = NULL;
  return NM_EVENT_NONE;
}

void nm_task_event_sent(NmTask *task, NmEvent event) {
  // "done" covers an unreported start too
  if (event == NM_EVENT_DONE) task->flags &= (uint8_t)~(NM_TASK_SEND_DONE | NM_TASK_SEND_START);
  else if (event == NM_EVENT_START) task->flags &= (uint8_t)~NM_TASK_SEND_START;
}

void nm_tasks_sync_begin(NmTasks *t, int count) {
  if (count < 0) count = 0;
  if (count > NM_TASK_MAX) count = NM_TASK_MAX;
  t->syncing = true;
  t->sync_count = (uint8_t)count;
  t->sync_got = 0;
  t->sync_seen = 0;
}

bool nm_tasks_sync_put(NmTasks *t, int index, const NmTask *def) {
  if (!t->syncing || index < 0 || index >= t->sync_count || def->id == 0) return false;
  NmTask *task = nm_tasks_find(t, def->id);
  if (!task) {
    if (t->count < NM_TASK_MAX) {
      task = &t->items[t->count++];
    } else {
      // full: take the place of a task the new list has not named (it would be dropped at the end anyway)
      for (int k = 0; k < t->count && !task; k++) {
        if (!(t->sync_seen & (1u << k)) && !(t->items[k].flags & (NM_TASK_SEND_START | NM_TASK_SEND_DONE))) task = &t->items[k];
      }
      if (!task) return false;
    }
    memset(task, 0, sizeof *task);
    task->id = def->id;
  }
  // the phone's word, except for what it has not heard from the watch yet
  uint8_t flags = def->flags & (NM_TASK_ACTIVE | NM_TASK_STARTED);
  if (task->flags & NM_TASK_SEND_START) flags |= NM_TASK_STARTED | NM_TASK_SEND_START;
  if (task->flags & NM_TASK_SEND_DONE) flags |= NM_TASK_DONE | NM_TASK_SEND_DONE;
  task->flags = flags;
  task->priority = def->priority;
  task->order = (uint8_t)index;
  strncpy(task->title, def->title, NM_TITLE_LEN - 1);
  task->title[NM_TITLE_LEN - 1] = '\0';
  t->sync_got |= 1u << index;
  t->sync_seen |= 1u << (task - t->items);
  return true;
}

bool nm_tasks_sync_finish(NmTasks *t) {
  if (!t->syncing) return false;
  uint32_t all = (1u << t->sync_count) - 1u;
  if ((t->sync_got & all) != all) return false;
  // keep the tasks of the new list, in the phone's order (insertion sort while compacting)
  int n = 0;
  for (int i = 0; i < t->count; i++) {
    if (!(t->sync_seen & (1u << i))) continue;
    NmTask task = t->items[i];
    int j = n++;
    while (j > 0 && t->items[j - 1].order > task.order) {
      t->items[j] = t->items[j - 1];
      j--;
    }
    t->items[j] = task;
  }
  t->count = (uint8_t)n;
  t->syncing = false;
  return true;
}
