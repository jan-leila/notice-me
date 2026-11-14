// Alternative PebbleKit JS entry for testing the watch UI alone:
//   PKJS_ENTRY=src/pkjs/ui-fixture.js pebble build
// Used by tools/screenshots.sh. ES5; stands in for the task server with a fixed list.
//
// Deterministic: the watch's clock is pinned with {TEST_NOW} (the emulator ignores `pebble emu-set-time` while an
// app runs, so the script's emu-set-time only pins it at install), and the session's countdown is moved by sending
// a later TEST_NOW, never by waiting. The nudge is shown through the watch's test hook ({TEST_NUDGE}); nudges are
// set to 30 minutes so the real worker stays quiet during a run.
//
// On 'ready': settings and the task list. Every REFRESH (long SELECT on the list) from the watch runs the next
// entry of STEPS and logs 'UI-FIXTURE step N'; tools/screenshots.sh walks these steps in order: keep the two in
// sync. What the watch starts and finishes is logged as 'UI-FIXTURE event start|done ID'.

var protocol = require('../common/protocol');

var FIXED_NOW = 1768464000;   // 2026-01-15 08:00:00 UTC

var step = 0;

var sender = protocol.createSender(function (dict, onOk, onErr) {
  Pebble.sendAppMessage(dict, onOk, onErr);
});

var TASKS = [
  { id: 103, title: 'Tax return', priority: 4, active: false, inProgress: false },
  { id: 102, title: 'Vacuum the flat', priority: 3, active: true, inProgress: false },
  { id: 101, title: 'Water the plants on the balcony and the window sill', priority: 2, active: true, inProgress: false },
  { id: 105, title: 'Call the dentist', priority: 0, active: false, inProgress: false },
  { id: 106, title: 'Sort the recycling', priority: 0, active: true, inProgress: false }
];

function sendList(tasks, status, extra) {
  var messages = [{ SETTINGS: [25, 5, 15, 4, 30, 3, 1] }, protocol.encodeStatus(status)].concat(protocol.encodeTasks(tasks));
  var last = messages[messages.length - 1];
  for (var k in extra || {}) if (extra.hasOwnProperty(k)) last[k] = extra[k];
  sender.sendAll(messages);
}

function clock(offset) { sender.sendAll([{ TEST_NOW: FIXED_NOW + offset }]); }

// One entry per REFRESH, in order (tools/screenshots.sh relies on this order).
var STEPS = [
  function () { clock(0); },                              // 1 the clock pinned: list, start a task, session, menu
  function () { clock(746); },                            // 2 12:34 left in the focus phase
  function () { clock(1510); },                           // 3 the focus phase is over: break
  function () { sender.sendAll([{ TEST_NUDGE: 1 }]); },   // 4 sat still on an active task
  function () { sendList([], 'Offline'); }                // 5 nothing left, and the phone says why
];

Pebble.addEventListener('ready', function () {
  console.log('UI-FIXTURE ready');
  sendList(TASKS, '', { TEST_NOW: FIXED_NOW });
});

Pebble.addEventListener('appmessage', function (e) {
  var msg = protocol.decodeWatchMessage(e.payload);
  if (!msg) return;
  if (msg.type === 'refresh') {
    if (step < STEPS.length) STEPS[step++]();
    console.log('UI-FIXTURE step ' + step);
  } else {
    console.log('UI-FIXTURE event ' + msg.type + ' ' + msg.id);
  }
});
