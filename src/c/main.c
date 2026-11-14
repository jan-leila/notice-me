// Notice Me — app state: tasks and settings from the phone, the Pomodoro session, storage, wakeups and the
// background worker that notices sitting still. The windows are in ui.c; the logic is in the pure nm_* modules.
#include "app.h"
#include "nm_ipc.h"

#define EVENT_RETRY_MS 10000
#define EVENT_RETRIES 6
#define QUIET_WINDOW_S 30      // a launch for a pushed update closes again only this soon after it
#define WORKER_DELAY_MS 1000
#define WAKEUP_COOKIE 1
#define WAKEUP_GAP 60          // Pebble refuses wakeups within a minute of another app's

NmTasks g_tasks;
NmSession g_session;
NmSettings g_settings;
char g_status[STATUS_LEN];

// Test clock ({TEST_NOW: unix seconds} from the UI fixture), 0 = the real clock. The emulator ignores
// `pebble emu-set-time` while an app is running, so screenshots can't pin it there.
static int32_t s_test_now;

static uint32_t s_sending_id;  // the task whose event is in the outbox
static NmEvent s_sending;      // and which event
static int s_event_tries;
static AppTimer *s_event_timer;
static AppTimer *s_worker_timer;
static int32_t s_launched;     // real unix seconds
static bool s_touched;         // a button was used since launch

int32_t app_now(void) {
  return s_test_now ? s_test_now : (int32_t)time(NULL);
}

const char *app_session_title(void) {
  NmTask *task = g_session.task_id ? nm_tasks_find(&g_tasks, g_session.task_id) : NULL;
  return task ? task->title : "Focus";
}

bool app_testing(void) {
  return s_test_now != 0;
}

void app_touched(void) {
  s_touched = true;
}

// ---- Storage ----------------------------------------------------------------

static void save_task(const NmTask *task) {
  int i = (int)(task - g_tasks.items);
  if (persist_write_data(NM_KEY_TASK0 + i, task, sizeof *task) != (int)sizeof *task) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "task %d not stored", i);
  }
}

static void save_tasks(void) {
  persist_write_int(NM_KEY_TASK_COUNT, g_tasks.count);
  for (int i = 0; i < g_tasks.count; i++) save_task(&g_tasks.items[i]);
}

static void load_state(void) {
  nm_tasks_init(&g_tasks);
  nm_settings_defaults(&g_settings);
  nm_session_stop(&g_session);
  persist_read_data(NM_KEY_SETTINGS, &g_settings, sizeof g_settings);
  if (persist_read_data(NM_KEY_SESSION, &g_session, sizeof g_session) != (int)sizeof g_session) nm_session_stop(&g_session);
  int n = persist_exists(NM_KEY_TASK_COUNT) ? (int)persist_read_int(NM_KEY_TASK_COUNT) : 0;
  for (int i = 0; i < n && i < NM_TASK_MAX; i++) {
    NmTask *task = &g_tasks.items[g_tasks.count];
    if (persist_read_data(NM_KEY_TASK0 + i, task, sizeof *task) == (int)sizeof *task) g_tasks.count++;
  }
}

// ---- Session: wakeup at the end of the phase, worker while it has to watch ----

static void schedule_wakeup(void) {
  wakeup_cancel_all();
  if (s_test_now || g_session.phase == NM_PHASE_IDLE || g_session.paused) return;
  time_t t = (time_t)g_session.phase_end;
  WakeupId id = E_RANGE;
  for (int tries = 0; tries < 5 && id == E_RANGE; tries++, t += WAKEUP_GAP) {
    id = wakeup_schedule(t, WAKEUP_COOKIE, true);  // E_RANGE: another app's wakeup is within a minute
  }
  APP_LOG(APP_LOG_LEVEL_INFO, "wakeup in %ds -> %d", (int)(g_session.phase_end - time(NULL)), (int)id);
}

// The worker runs only while a session on an active task exists (a watch runs one worker at a time). It is brought
// in step a moment after the change: launched in the same event that pushes the session window, the worker took
// the emulator's firmware down two runs in three.
static void sync_worker_now(void *data) {
  s_worker_timer = NULL;
  bool want = g_session.phase != NM_PHASE_IDLE && g_session.active;
  if (!want) {
    if (app_worker_is_running()) app_worker_kill();
  } else if (app_worker_is_running()) {
    AppWorkerMessage msg = { 0, 0, 0 };
    app_worker_send_message(NM_MSG_RELOAD, &msg);
  } else {
    app_worker_launch();  // it reads the session when it starts
  }
}

static void sync_worker(void) {
  if (!s_worker_timer) s_worker_timer = app_timer_register(WORKER_DELAY_MS, sync_worker_now, NULL);
}

static void session_changed(void) {
  persist_write_data(NM_KEY_SESSION, &g_session, sizeof g_session);
  schedule_wakeup();
  sync_worker();
  ui_refresh();
}

static void announce_phase(void) {
  static const uint32_t set_done[] = { 150, 100, 150, 100, 500 };
  switch (g_session.phase) {
    case NM_PHASE_FOCUS: vibes_long_pulse(); break;
    case NM_PHASE_IDLE: vibes_enqueue_custom_pattern((VibePattern){ .durations = set_done, .num_segments = 5 }); break;
    default: vibes_double_pulse(); break;
  }
  light_enable_interaction();
  if (g_session.phase == NM_PHASE_IDLE) ui_hide_session();
  else ui_show_session();
}

// Moves the session to the phase the clock says it is in.
static void update_session(void) {
  if (nm_session_advance(&g_session, &g_settings, app_now()) == 0) return;
  APP_LOG(APP_LOG_LEVEL_INFO, "phase %d round %d", g_session.phase, g_session.round);
  session_changed();
  announce_phase();
}

static void flush_events(void);

void app_start_task(uint32_t task_id) {
  NmTask *task = nm_tasks_find(&g_tasks, task_id);
  if (!task) return;
  nm_task_start(task);
  save_task(task);
  s_event_tries = 0;
  flush_events();
  nm_session_start(&g_session, &g_settings, task->id, (task->flags & NM_TASK_ACTIVE) != 0, app_now());
  session_changed();
  ui_show_session();
}

void app_stop_session(void) {
  nm_session_stop(&g_session);
  session_changed();
  ui_hide_session();
}

void app_toggle_pause(void) {
  if (g_session.paused) nm_session_resume(&g_session, app_now());
  else nm_session_pause(&g_session, app_now());
  session_changed();
}

void app_skip_phase(void) {
  if (nm_session_skip(&g_session, &g_settings, app_now()) == 0) return;
  session_changed();
  announce_phase();
}

// ---- Nudge --------------------------------------------------------------------

static void show_nudge(void) {
  static const uint32_t pattern[] = { 400, 200, 400, 200, 400 };
  persist_delete(NM_KEY_NUDGE);
  APP_LOG(APP_LOG_LEVEL_INFO, "nudge");
  vibes_enqueue_custom_pattern((VibePattern){ .durations = pattern, .num_segments = 5 });
  light_enable_interaction();
  ui_show_session();
  ui_show_nudge();
}

void app_nudge_answered(void) {
  if (!app_worker_is_running()) return;
  AppWorkerMessage msg = { 0, 0, 0 };
  app_worker_send_message(NM_MSG_ACK, &msg);
}

static void worker_message(uint16_t type, AppWorkerMessage *data) {
  if (type == NM_MSG_NUDGE && nm_session_watching(&g_session, app_now())) show_nudge();
}

// ---- Tasks: telling the phone what was started and finished --------------------

static void event_retry(void *data) {
  s_event_timer = NULL;
  flush_events();
}

static void retry_events_later(void) {
  if (!s_event_timer && ++s_event_tries <= EVENT_RETRIES) s_event_timer = app_timer_register(EVENT_RETRY_MS, event_retry, NULL);
}

// Sends the next unreported start or finish to the phone; the outbox callbacks continue from there.
static void flush_events(void) {
  if (s_sending_id) return;
  NmTask *task;
  NmEvent event = nm_tasks_next_event(&g_tasks, &task);
  if (event == NM_EVENT_NONE) return;
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) == APP_MSG_OK) {
    dict_write_int32(out, MESSAGE_KEY_EVENT_ID, (int32_t)task->id);
    dict_write_int32(out, MESSAGE_KEY_EVENT_KIND, event);
    if (app_message_outbox_send() == APP_MSG_OK) {
      s_sending_id = task->id;
      s_sending = event;
      return;
    }
  }
  // busy or no phone: try again in a while; any message from the phone also restarts this
  retry_events_later();
}

void app_task_done(uint32_t id) {
  NmTask *task = nm_tasks_find(&g_tasks, id);
  if (!task) return;
  nm_task_done(task);
  save_task(task);
  s_event_tries = 0;
  flush_events();
  if (g_session.phase != NM_PHASE_IDLE && g_session.task_id == id) app_stop_session();
  else ui_refresh();
}

void app_request_refresh(void) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) return;
  dict_write_int32(out, MESSAGE_KEY_REFRESH, 1);
  app_message_outbox_send();
}

// ---- AppMessage ---------------------------------------------------------------

static int32_t tuple_int(const Tuple *t) {
  if (!t) return 0;
  if (t->type == TUPLE_UINT) {
    return t->length == 1 ? t->value->uint8 : t->length == 2 ? t->value->uint16 : (int32_t)t->value->uint32;
  }
  if (t->type == TUPLE_INT) {
    return t->length == 1 ? t->value->int8 : t->length == 2 ? t->value->int16 : t->value->int32;
  }
  return 0;
}

// One entry of the task list (CONTRACTS.md §2).
static void receive_task(DictionaryIterator *iter, int index) {
  NmTask def;
  memset(&def, 0, sizeof def);
  def.id = (uint32_t)tuple_int(dict_find(iter, MESSAGE_KEY_TASK_ID));
  def.flags = (uint8_t)tuple_int(dict_find(iter, MESSAGE_KEY_TASK_FLAGS));
  def.priority = (uint8_t)tuple_int(dict_find(iter, MESSAGE_KEY_TASK_PRIORITY));
  Tuple *title = dict_find(iter, MESSAGE_KEY_TASK_TITLE);
  if (title && title->type == TUPLE_CSTRING) strncpy(def.title, title->value->cstring, NM_TITLE_LEN - 1);
  if (!nm_tasks_sync_put(&g_tasks, index, &def)) APP_LOG(APP_LOG_LEVEL_WARNING, "task %d dropped", index);
}

// The complete list arrived. A session whose task is gone from it (finished or deleted elsewhere) ends.
static void tasks_received(void) {
  APP_LOG(APP_LOG_LEVEL_INFO, "%d tasks", g_tasks.count);
  save_tasks();
  if (g_session.phase == NM_PHASE_IDLE || !g_session.task_id) return;
  NmTask *task = nm_tasks_find(&g_tasks, g_session.task_id);
  if (!task || (task->flags & NM_TASK_DONE)) {
    app_stop_session();
  } else if (((task->flags & NM_TASK_ACTIVE) != 0) != (g_session.active != 0)) {
    g_session.active = (task->flags & NM_TASK_ACTIVE) != 0;  // the label was added or removed
    session_changed();
  }
}

// The phone woke the app only to hand over a changed list (a push from the task server): leave again, unless the
// wearer has started using the app or it has something to say.
static void close_if_quiet(void) {
  if (launch_reason() != APP_LAUNCH_PHONE || s_touched || ui_nudge_showing()) return;
  if ((int32_t)time(NULL) - s_launched > QUIET_WINDOW_S) return;
  APP_LOG(APP_LOG_LEVEL_INFO, "pushed update stored, closing");
  window_stack_pop_all(false);
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *test_now = dict_find(iter, MESSAGE_KEY_TEST_NOW);
  if (test_now) {
    s_test_now = tuple_int(test_now);
    wakeup_cancel_all();  // the session now runs on the test clock
  }
  Tuple *settings = dict_find(iter, MESSAGE_KEY_SETTINGS);
  if (settings && settings->type == TUPLE_BYTE_ARRAY) {
    nm_settings_from_bytes(&g_settings, settings->value->data, settings->length);
    persist_write_data(NM_KEY_SETTINGS, &g_settings, sizeof g_settings);
    if (app_worker_is_running()) sync_worker();
  }
  Tuple *status = dict_find(iter, MESSAGE_KEY_STATUS);
  if (status && status->type == TUPLE_CSTRING) {
    strncpy(g_status, status->value->cstring, STATUS_LEN - 1);
    g_status[STATUS_LEN - 1] = '\0';
  }
  Tuple *count = dict_find(iter, MESSAGE_KEY_TASK_COUNT);
  if (count) nm_tasks_sync_begin(&g_tasks, (int)tuple_int(count));
  Tuple *index = dict_find(iter, MESSAGE_KEY_TASK_INDEX);
  if (index) receive_task(iter, (int)tuple_int(index));
  if ((count || index) && nm_tasks_sync_finish(&g_tasks)) tasks_received();
  update_session();
  ui_refresh();
  if (dict_find(iter, MESSAGE_KEY_TEST_NUDGE)) show_nudge();
  // the phone is there: report what was done while it was not
  s_event_tries = 0;
  flush_events();
  if (dict_find(iter, MESSAGE_KEY_QUIET)) close_if_quiet();
}

static void inbox_dropped(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "inbox dropped: %d", (int)reason);
}

static void outbox_sent(DictionaryIterator *iter, void *context) {
  if (!dict_find(iter, MESSAGE_KEY_EVENT_ID)) return;
  NmTask *task = nm_tasks_find(&g_tasks, s_sending_id);
  if (task) {
    nm_task_event_sent(task, s_sending);
    save_task(task);
  }
  s_sending_id = 0;
  flush_events();
}

static void outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "outbox failed: %d", (int)reason);
  if (!dict_find(iter, MESSAGE_KEY_EVENT_ID)) return;
  s_sending_id = 0;
  retry_events_later();
}

// ---- Clock --------------------------------------------------------------------

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  update_session();
  ui_tick();
}

static void wakeup_handler(WakeupId id, int32_t cookie) {
  update_session();
}

static void init(void) {
  s_launched = (int32_t)time(NULL);
  load_state();
  ui_init();

  app_message_register_inbox_received(inbox_received);
  app_message_register_inbox_dropped(inbox_dropped);
  app_message_register_outbox_sent(outbox_sent);
  app_message_register_outbox_failed(outbox_failed);
  app_message_open(256, 64);

  app_worker_message_subscribe(worker_message);
  wakeup_service_subscribe(wakeup_handler);
  tick_timer_service_subscribe(SECOND_UNIT, tick_handler);

  // Launched by the end of a phase (wakeup), by the worker (nudge) or by hand: catch up with the clock first.
  bool running = g_session.phase != NM_PHASE_IDLE;
  if (nm_session_advance(&g_session, &g_settings, app_now()) > 0) {
    session_changed();
    announce_phase();
  } else if (running) {
    sync_worker();
    ui_show_session();
  }
  bool nudge = launch_reason() == APP_LAUNCH_WORKER || persist_exists(NM_KEY_NUDGE);
  persist_delete(NM_KEY_NUDGE);
  if (nudge && nm_session_watching(&g_session, app_now())) show_nudge();
}

static void deinit(void) {
  if (s_event_timer) app_timer_cancel(s_event_timer);
  if (s_worker_timer) {  // do not leave a worker behind (or unstarted) on the way out
    app_timer_cancel(s_worker_timer);
    sync_worker_now(NULL);
  }
  tick_timer_service_unsubscribe();
  app_worker_message_unsubscribe();
  app_message_deregister_callbacks();
  ui_deinit();
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
