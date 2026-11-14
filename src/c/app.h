// Notice Me — what main.c (state, storage, messages, session control) and ui.c (windows) share.
#pragma once
#include <pebble.h>
#include "nm_tasks.h"
#include "nm_session.h"

#define STATUS_LEN 24

extern NmTasks g_tasks;
extern NmSession g_session;
extern NmSettings g_settings;
extern char g_status[STATUS_LEN];            // what the phone has to say ("Offline", "Set up on phone"), or ""

// ---- main.c ----
int32_t app_now(void);                       // unix seconds (the test clock when the UI fixture pinned one)
const char *app_session_title(void);         // the session's task
void app_start_task(uint32_t task_id);       // moves it to in progress and starts the timer
void app_stop_session(void);
void app_toggle_pause(void);
void app_skip_phase(void);
void app_task_done(uint32_t id);
void app_request_refresh(void);              // asks the phone to fetch the task list again
void app_nudge_answered(void);               // the wearer dismissed a nudge
void app_touched(void);                      // a button was used: the app is not just here for a pushed update
bool app_testing(void);                      // the UI fixture pinned the clock: no animation, no scrolling text

// ---- ui.c ----
void ui_init(void);
void ui_deinit(void);
void ui_refresh(void);                       // tasks, settings or the session changed
void ui_tick(void);                          // a second passed
void ui_show_session(void);
void ui_hide_session(void);
void ui_show_nudge(void);
bool ui_nudge_showing(void);
