#!/usr/bin/env bash
# Visual regression screenshots of the watch app in the Pebble emulator, driven by src/pkjs/ui-fixture.js.
#
#   tools/screenshots.sh [--update] [--threshold F] [platform...]   # default platforms: aplite basalt chalk diorite emery
#
# Builds the fixture app (PKJS_ENTRY=src/pkjs/ui-fixture.js, after `pebble clean`), then per platform walks the fixed
# SCENES below and saves test/screenshots/current/<platform>/<scene>.png (with --update: test/screenshots/baseline/...).
# Without --update the images are then compared with the baseline (tools/lib/png-diff.js): diff images go to
# test/screenshots/diff/, a table is printed, and the exit status is non-zero when any image differs by more than
# F (fraction of pixels, default 0.001) or is missing.
#
# Determinism: the fixture pins the watch's clock (TEST_NOW; `pebble emu-set-time` is ignored while the app runs, so
# it only pins the clock at install) and moves the session's countdown by sending a later TEST_NOW instead of
# waiting (the fixture's STEPS, advanced by a long SELECT on the list each); the nudge comes through the watch's test
# hook; TZ is UTC so the times on screen do not depend on the machine. No frame depends on when it is taken.
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if ! command -v pebble >/dev/null 2>&1; then
  exec nix-shell "$ROOT/shell.nix" --run "bash $(printf '%q' "${BASH_SOURCE[0]}") $(printf '%q ' "$@")"
fi

UPDATE=0
THRESHOLD=0.001
PLATFORMS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --update) UPDATE=1 ;;
    --threshold) THRESHOLD="$2"; shift ;;
    -h|--help) sed -n '2,15p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) PLATFORMS+=("$1") ;;
  esac
  shift
done
[ ${#PLATFORMS[@]} -gt 0 ] || PLATFORMS=(aplite basalt chalk diorite emery)

SHOTS="$ROOT/test/screenshots"
if [ "$UPDATE" = 1 ]; then DEST="$SHOTS/baseline"; else DEST="$SHOTS/current"; fi
STATE="$SHOTS/.state"
REAL_SDK_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/pebble-sdk"
mkdir -p "$STATE/xdg/pebble-sdk" "$STATE/tmp"
[ -e "$STATE/xdg/pebble-sdk/SDKs" ] || ln -s "$REAL_SDK_DIR/SDKs" "$STATE/xdg/pebble-sdk/SDKs"
export XDG_DATA_HOME="$STATE/xdg" TMPDIR="$STATE/tmp" TZ=UTC
cd "$ROOT" || exit 1

FIXED_NOW=$(sed -n 's/^var FIXED_NOW = \([0-9]*\);.*/\1/p' src/pkjs/ui-fixture.js)
[ -n "$FIXED_NOW" ] || { echo "FIXED_NOW not found in src/pkjs/ui-fixture.js" >&2; exit 1; }

fail=0
P=""
LOGS_PID=""
LOG=""
note() { echo "[screenshots $P] $*"; }
bad() { echo "[screenshots $P] FAIL: $*" >&2; fail=1; }

kill_emulator() {
  [ -n "$LOGS_PID" ] && kill "$LOGS_PID" 2>/dev/null
  LOGS_PID=""
  pebble kill >/dev/null 2>&1
  for sig in TERM KILL; do
    for pid in $(ps -eo pid,args | grep -E '[q]emu-pebble|[p]ypkjs' | grep -F "$STATE/" | awk '{print $1}'); do
      kill -s "$sig" "$pid" 2>/dev/null
    done
    sleep 1
  done
}
# The emulator keeps the watch's storage between runs (a session left running, tasks); start from an empty watch.
fresh_watch() {
  local d
  for d in "$STATE"/xdg/pebble-sdk/*/"$P"; do
    case "$d" in */SDKs/*) ;; *) rm -rf "$d" ;; esac
  done
}
trap 'kill_emulator; pebble clean >/dev/null 2>&1' EXIT

echo "[screenshots] building with ui-fixture.js"
pebble clean >/dev/null 2>&1
if ! PKJS_ENTRY=src/pkjs/ui-fixture.js pebble build >"$STATE/build.log" 2>&1; then
  tail -30 "$STATE/build.log" >&2
  echo "[screenshots] pebble build failed (see $STATE/build.log)" >&2
  exit 1
fi
PBW="$STATE/notice-me-ui-fixture.pbw"
cp build/notice-me.pbw "$PBW"
pebble clean >/dev/null 2>&1  # the fixture bundle must never be mistaken for the real app

# A freshly wiped emulator takes a while to boot: the first install can find nothing listening yet.
install() {
  local try
  for try in 1 2 3; do
    timeout 120 pebble install --emulator "$P" "$PBW" >"$STATE/$P-install.log" 2>&1 && return 0
    sleep 5
  done
  return 1
}
e() { timeout 30 pebble "$1" --emulator "$P" "${@:2}" >/dev/null 2>&1; }
btn() { e emu-button "$@" || bad "emu-button $*"; sleep 1; }
hold() { e emu-button push "$1" || bad "push $1"; sleep "$2"; e emu-button release "$1" || bad "release $1"; sleep 1; }
wait_log() {  # wait_log PATTERN SECONDS
  local i
  for ((i = 0; i < $2; i++)); do
    grep -q -- "$1" "$LOG" && return 0
    sleep 1
  done
  return 1
}

STEP=0
next_step() {  # long SELECT -> REFRESH -> the fixture runs its next STEP
  STEP=$((STEP + 1))
  e emu-button click select --duration 1200 || bad "long select"
  wait_log "UI-FIXTURE step ${STEP}\b" 15 || bad "fixture step $STEP not seen in logs"
  sleep 2
}

shot() {  # shot NAME
  local f="$DEST/$P/$1.png" try
  e emu-set-time "$FIXED_NOW" || bad "emu-set-time"
  sleep 1
  for try in 1 2 3; do
    rm -f "$f"
    timeout 30 pebble screenshot --no-open --emulator "$P" "$f" >/dev/null 2>&1
    [ -s "$f" ] && { note "$1"; return 0; }
    sleep 2
  done
  bad "screenshot $1 failed"
}

# The scene list. Each next_step must match the fixture's STEPS order (src/pkjs/ui-fixture.js), and happens on the list.
run_scenes() {
  next_step;            shot 01-list               # the top three tasks, the first selected
  btn click down;       shot 02-list-down
  btn click up
  btn click select;     shot 03-focus              # selecting starts the task: focus 1/4, 25:00
  btn click select;     shot 04-session-menu
  btn click back
  btn click back;       shot 05-list-running       # the session has its row, the next task moved up
  next_step
  btn click select;     shot 06-focus-mid          # 12:34 left
  btn click back
  next_step;            shot 07-break              # the focus phase ended: the break shows by itself
  btn click select
  btn click select;     shot 08-paused
  btn click select
  btn click down
  btn click select;     shot 09-after-done         # "Task done": the session is over, the task off the list
  btn click select;     shot 10-focus-active       # a task with the active label
  btn click back
  next_step;            shot 11-nudge
  btn click select
  btn click back
  next_step;            shot 12-empty              # no tasks; the phone's status next to the clock
}

for P in "${PLATFORMS[@]}"; do
  mkdir -p "$DEST/$P"
  rm -f "$DEST/$P"/*.png
  LOG="$STATE/$P-logs.txt"
  STEP=0
  kill_emulator
  fresh_watch
  note "starting emulator"
  e emu-set-time "$FIXED_NOW" || e emu-set-time "$FIXED_NOW" || bad "emu-set-time"
  if ! install; then
    cat "$STATE/$P-install.log" >&2
    bad "install failed"
    continue
  fi
  : >"$LOG"
  pebble logs --emulator "$P" >"$LOG" 2>&1 &
  LOGS_PID=$!
  sleep 6  # settings + list arrive
  run_scenes
  kill_emulator
done

P=""
if [ "$UPDATE" = 1 ]; then
  echo "[screenshots] baseline written to $DEST"
  exit "$fail"
fi
rm -rf "$SHOTS/diff"
sel=()
for P in "${PLATFORMS[@]}"; do sel+=("$P"); done
# compare only the platforms that ran
CMP_BASE="$STATE/cmp-baseline"; CMP_CUR="$STATE/cmp-current"
rm -rf "$CMP_BASE" "$CMP_CUR"; mkdir -p "$CMP_BASE" "$CMP_CUR"
for P in "${sel[@]}"; do
  [ -d "$SHOTS/baseline/$P" ] && cp -r "$SHOTS/baseline/$P" "$CMP_BASE/$P"
  [ -d "$DEST/$P" ] && cp -r "$DEST/$P" "$CMP_CUR/$P"
done
node tools/lib/png-diff.js compare "$CMP_BASE" "$CMP_CUR" "$SHOTS/diff" --threshold "$THRESHOLD" || fail=1
exit "$fail"
