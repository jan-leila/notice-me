# Notice Me

A Pebble watchapp that is an ADHD accommodation: it keeps the task list short, times the task you chose, and notices when "cleaning the kitchen" has turned into sitting on the sofa.

- **Three tasks, not thirty.** The watch shows the top of your task list: the open tasks of one [Vikunja](https://vikunja.io) project, highest priority first, then the one added first. The rest stay out of sight (`+4 more`). How many show is a setting (1 to 5, default 3).
- **Selecting a task starts it.** It moves to the *In Progress* column of the project's kanban board and a Pomodoro round begins: focus, short break, focus, … long break. The watch buzzes at every change, also when the app is closed. Tasks already in progress sit at the top of the list.
- **Active tasks watch for sitting.** A task that carries your *active* label (the name is yours to choose) is not sedentary. While you are in a focus phase of such a task, a background worker reads the accelerometer; sit still for the set number of minutes and the watch buzzes and tells you which task to get back to. It repeats until you move or answer. Breaks are for sitting.
- **From the session screen**, SELECT opens its actions: pause, *Task done* (marks it done on the server), skip the phase, stop.
- **Live updates.** The phone registers a webhook on the project that points at the push endpoint [Gravel](https://github.com/jan-leila/gravel) gives the app (UnifiedPush). Add or change a task anywhere and the server posts to it; Gravel opens the app, the list is fetched again, and the app closes again if you were not using it. Hold SELECT on the list to fetch by hand.

What the watch did while the phone or the server was unreachable (started, finished) is kept and delivered later, on both the watch and the phone.

The watch UI follows [Deutschebahned](../deutschebahned): a rotating drum of rows whose selected row sits between a band and a bar in its colour (a task's colour is its priority), and screens with a coloured header over a white body. No stock menus.

## Setting up

1. **Vikunja.** Pick the project. Its kanban view needs a column for work in progress (default name *In Progress*; the name is a setting). Make a label for tasks that get you moving (default *active*). Create an API token (Settings → API Tokens) that may: read projects, views and buckets; read and update tasks; move tasks between buckets; read, create and delete the project's webhooks.
2. **Gravel** (Settings → Apps → Watch App Permissions): allow *Network* and *Push* for Notice Me and pick a push distributor (ntfy, for example).
3. **The app's settings page:** server address, API token, project (title or number). Save. Open the page again to see whether the last sync worked and whether live updates are on; it then also offers the project and column names the server has.

Without Push everything else still works; the list then updates when the app is opened or on a long SELECT.

Vikunja 2.2 and newer refuse webhook targets that are not publicly routable. If your push server is on a private network, allow it in Vikunja's config (`outgoingrequests.allownonroutableips`, before 2.4 `webhooks.allownonroutableips`). The settings page says when the server refused the webhook.

The first time a session on an active task starts, the watch may ask whether Notice Me may run in the background (a watch runs one background worker at a time). The worker runs only while such a session exists.

## Build

```
nix-shell                       # pebble.nix pinned with npins (see shell.nix)
pebble sdk install 4.9.169      # once
pebble build                    # -> build/notice-me.pbw (also regenerates src/pkjs/config-page.js from config/)
pebble install --emulator basalt
```

## Tests

Each part is tested on its own (see `CONTRACTS.md` for the interfaces):

| part | test | command (inside nix-shell) |
|---|---|---|
| settings model | `test/settings.test.js` | `node tools/run-tests.js settings` |
| phone⇄watch protocol | `test/protocol.test.js` | `node tools/run-tests.js protocol` |
| Vikunja client (against an in-memory server) | `test/vikunja.test.js`, `test/helpers/fake-vikunja.js` | `node tools/run-tests.js vikunja` |
| sync: fetch, start/done, offline queue, push, webhook | `test/sync.test.js` | `node tools/run-tests.js sync` |
| settings page (headless Chromium) | `test/config-page.test.js` | `node tools/run-tests.js config-page` |
| settings page screenshots (visual regression) | `test/settings-screenshots/baseline/` | `node tools/settings-screenshots.js [--update] [--only REGEX] [light\|dark]` |
| watch logic: task list, Pomodoro phases, stillness, drum layout, marquee (host gcc) | `test/c/test_nm_*.c` | `node tools/run-tests.js c-model` |
| integration (the real phone code against a fake Pebble runtime) | `test/pkjs-glue.test.js` | `node tools/run-tests.js pkjs-glue` |
| watch UI screenshots in the emulator (visual regression) | `test/screenshots/baseline/` | `tools/screenshots.sh [--update] [platform...]` |
| stillness nudge end to end in the emulator (real worker, real clock) | | `tools/test-nudge-emulator.sh [platform]` |

`node tools/run-tests.js` runs every `test/*.test.js`.

### Watch screenshots

`tools/screenshots.sh` builds the app with `src/pkjs/ui-fixture.js` in place of the real phone code, walks 12 scenes per platform in the emulator and compares each screenshot pixel by pixel with `test/screenshots/baseline/` (`tools/lib/png-diff.js`). The fixture pins the watch's clock and moves the countdown by sending a later time, so no frame depends on when it was taken. A run writes `current/`, plus red-on-grey diff images in `diff/` for anything that changed, and exits non-zero when an image differs by more than `--threshold` (fraction of pixels, default 0.001) or is missing. After an intended change, rerun with `--update` and commit the new baselines. `node tools/contact-sheet.js test/screenshots/baseline/basalt sheet.png` tiles a platform's scenes into one image.

### Settings page screenshots

`tools/settings-screenshots.js` renders the settings page in headless Chromium at phone width (3 scenes, in light and dark colour schemes) and compares the same way with `test/settings-screenshots/baseline/`. The page is loaded with the same `window.__params` injection `src/pkjs/index.js` uses; timezone and locale are pinned.

## Releases

Releases follow Deutschebahned's process (`.github/workflows/release.yml`). Each push to the `release` branch builds `notice-me.pbw` and publishes it as a GitHub release `v<version>` (version from `package.json`), with the asset `notice-me-v<version>.pbw`. If that release already exists the run stops without building. After a release, the workflow commits a minor version bump to `main` (`tools/bump-version.js`; Pebble only uses major.minor). If `main` was already bumped by hand, it is left alone.

To release: `git push <github remote> main:release`.
