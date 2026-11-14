#!/usr/bin/env bash
# End-to-end test of the stillness nudge in the Pebble emulator, with the real background worker and the real clock
# (src/pkjs/nudge-fixture.js: one active task, nudges after one minute).
#
#   tools/test-nudge-emulator.sh [platform]      (default basalt; takes about four minutes)
#
# 1. Starts the active task and leaves the emulator's accelerometer alone: the worker must bring up the nudge
#    ("nudge" in the app log) between one and two minutes later.
# 2. Answers the nudge and feeds the accelerometer a walking-like swing for 90 seconds: no nudge may come.
# 3. Stops moving: the nudge must come again.
# Screenshots and logs go to test/app-out/.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if ! command -v pebble >/dev/null 2>&1; then
  exec nix-shell "$ROOT/shell.nix" --run "bash $(printf '%q' "${BASH_SOURCE[0]}") $(printf '%q ' "$@")"
fi
P=${1:-basalt}
OUT="$ROOT/test/app-out"
STATE="$OUT/.state"
REAL_SDK_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/pebble-sdk"
mkdir -p "$STATE/xdg/pebble-sdk" "$STATE/tmp"
[ -e "$STATE/xdg/pebble-sdk/SDKs" ] || ln -s "$REAL_SDK_DIR/SDKs" "$STATE/xdg/pebble-sdk/SDKs"
export XDG_DATA_HOME="$STATE/xdg" TMPDIR="$STATE/tmp"
cd "$ROOT" || exit 1
LOG="$OUT/$P-nudge-logs.txt"
LOGS_PID=""

kill_emulator() {
  [ -n "$LOGS_PID" ] && kill "$LOGS_PID" 2>/dev/null
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
# A freshly wiped emulator takes a while to boot: the first install can find nothing listening yet.
install() {
  local try
  for try in 1 2 3; do
    timeout 120 pebble install --emulator "$P" "$PBW" >"$STATE/$P-install.log" 2>&1 && return 0
    sleep 5
  done
  return 1
}
fail() { echo "FAIL: $*" >&2; exit 1; }
e() { timeout 30 pebble "$1" --emulator "$P" "${@:2}" >/dev/null 2>&1; }
shot() { timeout 30 pebble screenshot --no-open --emulator "$P" "$OUT/$P-$1.png" >/dev/null 2>&1; }
nudges() { grep -c -- 'main.c:[0-9]*> nudge' "$LOG"; }
wait_nudges() {  # wait_nudges COUNT SECONDS
  local i
  for ((i = 0; i < $2; i++)); do
    [ "$(nudges)" -ge "$1" ] && return 0
    sleep 1
  done
  return 1
}

echo "[nudge $P] building with nudge-fixture.js"
pebble clean >/dev/null 2>&1   # waf does not track PKJS_ENTRY
PKJS_ENTRY=src/pkjs/nudge-fixture.js pebble build >"$STATE/build.log" 2>&1 || { tail -30 "$STATE/build.log" >&2; fail "pebble build"; }
PBW="$STATE/notice-me-nudge-fixture.pbw"
cp build/notice-me.pbw "$PBW"
pebble clean >/dev/null 2>&1   # the fixture bundle must never be mistaken for the real app

kill_emulator
fresh_watch
install || { cat "$STATE/$P-install.log" >&2; fail "install"; }
: >"$LOG"
pebble logs --emulator "$P" >"$LOG" 2>&1 &
LOGS_PID=$!
sleep 6

echo "[nudge $P] starting the active task, sitting still"
e emu-button click select || fail "select"
sleep 5
shot nudge-1-focus
grep -q 'NUDGE-FIXTURE start 102' "$LOG" || fail "the start was not reported to the phone"
[ "$(nudges)" = 0 ] || fail "nudged at once"
sleep 40
[ "$(nudges)" = 0 ] || fail "nudged before a minute had passed"
wait_nudges 1 80 || fail "no nudge within two minutes of sitting still"
sleep 2
shot nudge-2-nudged
echo "[nudge $P] nudged; answering and moving for 90 s"

# a swing of about 400 milli-g ten times a second, like an arm while walking
for i in $(seq 1 20); do printf '%s\n' '-400, 100, -900' '400, -100, -1100'; done >"$STATE/walk.txt"
e emu-button click select || fail "dismiss"
before=$(nudges)
end=$((SECONDS + 90))
while [ $SECONDS -lt $end ]; do
  e emu-accel custom "$STATE/walk.txt" || fail "emu-accel"
done
[ "$(nudges)" = "$before" ] || fail "nudged while moving"
shot nudge-3-moving

echo "[nudge $P] standing still again"
e emu-accel none
wait_nudges $((before + 1)) 120 || fail "no nudge after the movement stopped"
shot nudge-4-nudged-again
echo "[nudge $P] ok: $(nudges) nudges, logs in $LOG"
