// src/common/protocol.js: the AppMessage dictionaries of CONTRACTS.md §2 and the sender.
const test = require('node:test');
const assert = require('node:assert/strict');
const protocol = require('../src/common/protocol');
const settings = require('../src/common/settings');

test('protocol: settings bytes follow NmSettings', () => {
  const c = settings.normalize({ timer: { focus: 50, shortBreak: 10, longBreak: 30, rounds: 2 }, movement: { nudgeMinutes: 8, sensitivity: 2 }, showMax: 5 });
  assert.deepEqual(protocol.encodeSettings(c), { SETTINGS: [50, 10, 30, 2, 8, 5, 2] });
  assert.deepEqual(protocol.encodeSettings(settings.defaults()), { SETTINGS: [25, 5, 15, 4, 5, 3, 1] });
});

test('protocol: task list', () => {
  const tasks = [
    { id: 102, title: 'Vacuum the flat', priority: 4, active: true, inProgress: false },
    { id: 103, title: 'Tax return', priority: 9, active: false, inProgress: true },
    { id: 104, title: 'ä'.repeat(30), priority: -1, active: true, inProgress: true },
  ];
  assert.deepEqual(protocol.encodeTasks(tasks), [
    { TASK_COUNT: 3 },
    { TASK_INDEX: 0, TASK_ID: 102, TASK_TITLE: 'Vacuum the flat', TASK_FLAGS: 1, TASK_PRIORITY: 4 },
    { TASK_INDEX: 1, TASK_ID: 103, TASK_TITLE: 'Tax return', TASK_FLAGS: 2, TASK_PRIORITY: 5 },
    { TASK_INDEX: 2, TASK_ID: 104, TASK_TITLE: 'ä'.repeat(19), TASK_FLAGS: 3, TASK_PRIORITY: 0 },
  ]);
  assert.deepEqual(protocol.encodeTasks([]), [{ TASK_COUNT: 0 }]);
  const many = Array.from({ length: 40 }, (_, i) => ({ id: i + 1, title: 't' + i, priority: 0 }));
  const out = protocol.encodeTasks(many);
  assert.equal(out[0].TASK_COUNT, 16, 'the watch holds 16');
  assert.equal(out.length, 17);
  assert.equal(out[16].TASK_ID, 16);
});

test('protocol: titles and status are cut on code point boundaries', () => {
  assert.equal(protocol.truncateUtf8('abc', 2), 'ab');
  assert.equal(protocol.truncateUtf8('aé', 2), 'a');
  assert.equal(protocol.truncateUtf8('a😀b', 4), 'a');
  assert.equal(protocol.truncateUtf8('a😀b', 5), 'a😀');
  assert.equal(protocol.truncateUtf8(null, 5), '');
  assert.deepEqual(protocol.encodeStatus('x'.repeat(40)), { STATUS: 'x'.repeat(23) });
  assert.deepEqual(protocol.encodeStatus(''), { STATUS: '' });
});

test('protocol: watch messages', () => {
  assert.deepEqual(protocol.decodeWatchMessage({ EVENT_ID: 102, EVENT_KIND: 1 }), { type: 'start', id: 102 });
  assert.deepEqual(protocol.decodeWatchMessage({ EVENT_ID: 102, EVENT_KIND: 2 }), { type: 'done', id: 102 });
  assert.equal(protocol.decodeWatchMessage({ EVENT_ID: 102, EVENT_KIND: 7 }), null);
  assert.deepEqual(protocol.decodeWatchMessage({ REFRESH: 1 }), { type: 'refresh' });
  assert.equal(protocol.decodeWatchMessage({}), null);
  assert.equal(protocol.decodeWatchMessage(undefined), null);
});

test('protocol: sender sends in order, retries, and a new batch replaces the waiting one', () => {
  const sent = [];
  let pending = null;
  const sender = protocol.createSender((msg, ok, err) => { sent.push(msg); pending = { ok, err }; }, { retries: 2 });
  sender.sendAll([{ a: 1 }, { a: 2 }, { a: 3 }]);
  assert.deepEqual(sent, [{ a: 1 }]);
  assert.equal(sender.pending(), 3);
  const first = pending;
  first.ok();
  first.ok();     // a second callback for the same message is ignored
  assert.deepEqual(sent, [{ a: 1 }, { a: 2 }]);
  pending.err();
  assert.deepEqual(sent.slice(2), [{ a: 2 }], 'retried');
  sender.sendAll([{ b: 1 }]);   // replaces {a: 3}, after the message in flight
  pending.ok();
  assert.deepEqual(sent.slice(3), [{ b: 1 }]);
  pending.ok();
  assert.equal(sender.pending(), 0);
  // three failures in a row: the batch is dropped
  sender.sendAll([{ c: 1 }, { c: 2 }]);
  pending.err(); pending.err(); pending.err();
  assert.equal(sent.filter(m => m.c === 1).length, 3);
  assert.equal(sent.some(m => m.c === 2), false);
  assert.equal(sender.pending(), 0);
  // a throwing send counts as a failure
  const thrower = protocol.createSender(() => { throw new Error('no watch'); }, { retries: 1 });
  thrower.sendAll([{ d: 1 }]);
  assert.equal(thrower.pending(), 0);
});
