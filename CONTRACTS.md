# Notice Me — module contracts

Each part is built and tested on its own against the contract below. **Contract tests in `test/` are the spec**: when this document and a test disagree, the test wins (and this doc gets fixed).

Conventions everywhere:

- **Time** is epoch **seconds** (integers) in JS and C. Pure modules get `now` passed in.
- `src/common/**` and `src/pkjs/**` are **ES5** (`var`, `function`, no arrow functions, no template strings, no Promises). They run inside PebbleKit JS on the phone and, for `settings.js`, inside the settings page. Tests (`test/**`) and tools (`tools/**`) can use modern Node (v24).
- Async results use node-style callbacks: `cb(err, value)`.
- No npm dependencies.
- The `nm_*` C modules include no `pebble.h` and are compiled for the host by `test/c-model.test.js`.

```
 Vikunja ◄──── HTTPS ────► phone (PebbleKit JS in Gravel)                      watch (C)
   │                        pkjs/index.js (glue: XHR, localStorage, push)       main.c   state, storage, messages, wakeups
   │ webhook                  └─ sync ──► vikunja (the only backend)            ui.c     drum list, session, actions, nudge
   ▼                               └────► protocol ◄─── AppMessage ───►         nm_tasks / nm_session / nm_wheel / nm_marquee
 push server ─► distributor ─► Gravel ─► 'push' event                           worker.c + nm_motion (accelerometer)
 settings page (config/config-page.html + settings.js) ◄─ data: URI ─ pkjs
```

## 1. Settings (`src/common/settings.js`)

```
config = { version: 1,
           server: { url, token, project, inProgress, activeLabel },
           timer: { focus, shortBreak, longBreak, rounds },     // minutes; 1..120, 1..60, 1..60, 1..8
           movement: { nudgeMinutes, sensitivity },             // 1..60; 0 low, 1 normal, 2 high
           showMax }                                            // 1..5
```

`server.project` is a project's title or id. `server.inProgress` is the title of the kanban bucket a started task moves to (never empty). `server.activeLabel` is the label that marks a task as not sedentary (empty: no task is). `normalize(raw)` returns a valid config for any input and is a fixed point; `configured(config)` is true when url (with scheme), token and project are set. Stored by the phone under `noticeMe.config`.

## 2. AppMessage protocol (`src/common/protocol.js`, `src/c/main.c`)

Phone → watch. A refresh is one batch, sent in this order, one dictionary per message:

| keys | meaning |
|---|---|
| `SETTINGS` (7 bytes) | `NmSettings` in order: focus, short break, long break, rounds, nudge minutes, tasks shown, sensitivity. Out-of-range bytes are clamped on the watch. |
| `STATUS` (string ≤ 23 bytes) | what the watch shows next to the clock; `""` when all is well |
| `TASK_COUNT` | header of a task list (0..16). Starts a new list. |
| `TASK_INDEX`, `TASK_ID`, `TASK_TITLE` (≤ 39 bytes), `TASK_FLAGS` (bit 0 active, bit 1 in progress), `TASK_PRIORITY` (0..5) | one task. The list replaces the watch's once every index has arrived. |
| `QUIET` | on the last message of a batch that answers a push: the watch may close (§6) |
| `TEST_NOW`, `TEST_NUDGE` | UI fixture only: pin the watch's clock; show the nudge |

On a failed fetch the batch has no task list (the watch keeps the one it has).

Watch → phone:

| keys | meaning |
|---|---|
| `EVENT_ID`, `EVENT_KIND` (1 start, 2 done) | the wearer started or finished a task. Repeated until acknowledged; the phone treats repeats as one. |
| `REFRESH` | long SELECT on the list: fetch again |

## 3. Tasks on the watch (`src/c/nm_tasks.c`)

Up to 16 tasks in the phone's order. `nm_tasks_visible` lists the ones in progress first, then the rest, skipping finished ones. A start or finish made on the watch is flagged until the phone has acknowledged it (`nm_tasks_next_event` / `nm_task_event_sent`); while flagged it outlasts incoming lists, afterwards the phone's list is the truth. A session whose task is missing from a complete list ends.

## 4. Session (`src/c/nm_session.c`)

Focus → break → focus → … → (after `rounds` focus phases) long break → idle. Each phase starts where the previous one ended; `nm_session_advance(now)` walks through every phase that has ended. A wakeup is scheduled for the end of the running phase, so the app is launched to announce it. `nm_session_watching` (focus phase, not paused, active task) is when sitting still matters.

## 5. Stillness (`src/c/nm_motion.c`, `worker_src/c/worker.c`)

The worker subscribes to the accelerometer (25 samples at 10 Hz per batch) while `nm_session_watching`. A batch "moves" when its mean change per sample (|dx|+|dy|+|dz|, milli-g) reaches the sensitivity's threshold (150 / 90 / 55); three moving batches close together reset the stillness clock; samples taken while the watch vibrates are ignored. When the wearer has been still for `nudge_min` minutes since they last moved or were last nudged, the worker sets `NM_KEY_NUDGE`, messages the app and launches it. The thresholds are estimates, not measured on a wrist.

## 6. App ⇄ worker, storage (`src/c/nm_ipc.h`)

Persistent storage, shared by app and worker: `NM_KEY_SETTINGS` (`NmSettings`), `NM_KEY_SESSION` (`NmSession`), `NM_KEY_NUDGE`, `NM_KEY_TASK_COUNT`, `NM_KEY_TASK0 + i` (`NmTask`). Worker messages: `NM_MSG_RELOAD` (app → worker: read session and settings again), `NM_MSG_ACK` (app → worker: the wearer answered a nudge), `NM_MSG_NUDGE` (worker → app). The app launches the worker when a session on an active task exists and kills it otherwise, a second after the change.

Launched by the phone and handed a `QUIET` batch within 30 s without a button having been used, the app closes itself: it was opened for a pushed update.

## 7. Sync (`src/common/sync.js`, `src/pkjs/index.js`)

`createSync({storage, http, send, log})`. `refresh({quiet})`: carry out queued starts and finishes, fetch the tasks, send the batch of §2, make sure the webhook points at the push endpoint. Refreshes never overlap; one asked for meanwhile runs afterwards (quiet only if every request was). `handleWatch(msg)` queues a start or finish (persisted under `noticeMe.ops`; dropped when the server answers 404 or there is no bucket, given up on after 5 failures) and refreshes. The list sent to the watch is the fetched one with the queue laid over it, in-progress tasks first. `index.js` asks Gravel for the push endpoint on every `ready` (`Pebble.getPushEndpoint`), refreshes on `ready`, on `push` (quiet) and after the settings page saved.

## 8. Vikunja (`src/common/vikunja.js`)

The only place that knows the server's API (paths are listed at the top of the file). `board` resolves the project, its first manual kanban view and the in-progress bucket; `tasks` returns open tasks `{id, title, priority, created, active, inProgress}` sorted by priority (high first), then `created`, then id; `start` moves a task to the bucket; `done` GETs the task and POSTs it back whole with `done: true` (an update replaces the task); `ensureHook` / `dropHook` manage the project webhook for `task.created`, `task.updated`, `task.deleted`.

## 9. Settings page (`config/config-page.html`, `tools/build-config.js`)

Bundled into `src/pkjs/config-page.js` (generated, not committed) and opened as a `data:` URI with `window.__params = {config, remote, return_to}` injected at `<!--PARAMS-->`. `remote = {projects, buckets, status, push, syncedAt}` is what the phone learned at its last sync (the page itself makes no requests: the server's CORS rules would refuse a `data:` page). Every setting is an input with `data-field="<path>"`; `readForm()` returns the normalized config, `fillForm(config)` shows one; Save navigates to `return_to + encodeURIComponent(JSON.stringify({config}))`.
