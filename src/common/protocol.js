// Phone <-> watch AppMessage protocol (CONTRACTS.md §2). Pure; ES5.

var TITLE_MAX_BYTES = 39;   // NM_TITLE_LEN - 1
var STATUS_MAX_BYTES = 23;  // STATUS_LEN - 1
var WATCH_TASKS = 16;       // NM_TASK_MAX
var FLAG_ACTIVE = 1, FLAG_STARTED = 2;
var EVENT_START = 1, EVENT_DONE = 2;

function codePointAt(str, i) {
  var c = str.charCodeAt(i);
  if (c >= 0xD800 && c <= 0xDBFF && i + 1 < str.length) {
    var d = str.charCodeAt(i + 1);
    if (d >= 0xDC00 && d <= 0xDFFF) return { units: 2, bytes: 4 };
  }
  return { units: 1, bytes: c < 0x80 ? 1 : c < 0x800 ? 2 : 3 };
}

// Longest prefix of str whose UTF-8 encoding fits in maxBytes, never splitting a code point.
function truncateUtf8(str, maxBytes) {
  str = str === null || str === undefined ? '' : String(str);
  var used = 0;
  var i = 0;
  while (i < str.length) {
    var cp = codePointAt(str, i);
    if (used + cp.bytes > maxBytes) break;
    used += cp.bytes;
    i += cp.units;
  }
  return str.substring(0, i);
}

// SETTINGS: the bytes of NmSettings (src/c/nm_session.h), in order.
function encodeSettings(config) {
  return { SETTINGS: [config.timer.focus, config.timer.shortBreak, config.timer.longBreak, config.timer.rounds,
                      config.movement.nudgeMinutes, config.showMax, config.movement.sensitivity] };
}

// What the watch shows next to the clock: '' when all is well.
function encodeStatus(status) {
  return { STATUS: truncateUtf8(status, STATUS_MAX_BYTES) };
}

// task: { id, title, priority, active, inProgress }
function encodeTask(task, index) {
  var priority = Math.round(Number(task.priority) || 0);
  return {
    TASK_INDEX: index,
    TASK_ID: task.id,
    TASK_TITLE: truncateUtf8(task.title, TITLE_MAX_BYTES),
    TASK_FLAGS: (task.active ? FLAG_ACTIVE : 0) | (task.inProgress ? FLAG_STARTED : 0),
    TASK_PRIORITY: priority < 0 ? 0 : priority > 5 ? 5 : priority
  };
}

// A {TASK_COUNT} header, then one message per task (the first WATCH_TASKS, in the given order). The watch replaces
// its list once every entry has arrived.
function encodeTasks(tasks) {
  var n = Math.min(tasks.length, WATCH_TASKS);
  var out = [{ TASK_COUNT: n }];
  for (var i = 0; i < n; i++) out.push(encodeTask(tasks[i], i));
  return out;
}

// {type: 'refresh'} | {type: 'start' | 'done', id} | null
function decodeWatchMessage(payload) {
  if (!payload) return null;
  if (payload.EVENT_ID !== undefined) {
    var kind = Number(payload.EVENT_KIND);
    if (kind !== EVENT_START && kind !== EVENT_DONE) return null;
    return { type: kind === EVENT_DONE ? 'done' : 'start', id: Number(payload.EVENT_ID) };
  }
  if (payload.REFRESH !== undefined) return { type: 'refresh' };
  return null;
}

// Sends messages one at a time through sendFn(dict, onOk, onErr) (= Pebble.sendAppMessage), retrying each a few
// times. A new sendAll() replaces whatever was still waiting: every batch is a complete picture.
function createSender(sendFn, opts) {
  var retries = opts && typeof opts.retries === 'number' ? opts.retries : 2;
  var queue = [];
  var busy = false;

  function next() {
    if (busy || queue.length === 0) return;
    var msg = queue.shift();
    var failures = 0;
    busy = true;
    (function transmit() {
      var done = false;
      function onOk() {
        if (done) return;
        done = true;
        busy = false;
        next();
      }
      function onErr() {
        if (done) return;
        done = true;
        if (++failures <= retries) return transmit();
        busy = false;
        queue = [];   // the rest of the batch makes no sense without this message
      }
      try {
        sendFn(msg, onOk, onErr);
      } catch (e) {
        onErr(e);
      }
    })();
  }

  return {
    sendAll: function (messages) {
      queue = messages.slice();
      next();
    },
    pending: function () { return queue.length + (busy ? 1 : 0); }
  };
}

module.exports = {
  TITLE_MAX_BYTES: TITLE_MAX_BYTES, WATCH_TASKS: WATCH_TASKS,
  truncateUtf8: truncateUtf8, encodeSettings: encodeSettings, encodeStatus: encodeStatus, encodeTask: encodeTask,
  encodeTasks: encodeTasks, decodeWatchMessage: decodeWatchMessage, createSender: createSender
};
