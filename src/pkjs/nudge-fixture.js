// Alternative PebbleKit JS entry for testing the stillness nudge end to end in the emulator:
//   PKJS_ENTRY=src/pkjs/nudge-fixture.js pebble build
// Used by tools/test-nudge-emulator.sh. ES5. One task with the active label, nudges after a single minute, on the
// real clock: the worker has to notice the (motionless) emulator by itself.

var protocol = require('../common/protocol');

var sender = protocol.createSender(function (dict, onOk, onErr) {
  Pebble.sendAppMessage(dict, onOk, onErr);
});

Pebble.addEventListener('ready', function () {
  console.log('NUDGE-FIXTURE ready');
  sender.sendAll([{ SETTINGS: [25, 5, 15, 4, 1, 3, 1] }, protocol.encodeStatus('')].concat(protocol.encodeTasks([
    { id: 102, title: 'Vacuum the flat', priority: 3, active: true, inProgress: false },
    { id: 103, title: 'Tax return', priority: 2, active: false, inProgress: false }
  ])));
});

Pebble.addEventListener('appmessage', function (e) {
  var msg = protocol.decodeWatchMessage(e.payload);
  if (msg) console.log('NUDGE-FIXTURE ' + msg.type + (msg.id ? ' ' + msg.id : ''));
});
