// The settings model: where the tasks live, and the timer and movement options (CONTRACTS.md §1). Pure; ES5 (runs
// in PebbleKit JS and in the settings page).
//
// config = { version,
//            server: { url, token, project, inProgress, activeLabel },
//            timer: { focus, shortBreak, longBreak, rounds }, movement: { nudgeMinutes, sensitivity }, showMax }
// server.project is a project's title or id; server.inProgress the title of the kanban bucket a started task
// moves to; server.activeLabel the label that marks a task as not sedentary.

var VERSION = 1;

function clampInt(v, lo, hi, fallback) {
  var n = Number(v);
  if (v === null || v === undefined || v === '' || !isFinite(n)) return fallback;
  n = Math.round(n);
  return n < lo ? lo : n > hi ? hi : n;
}

function text(v, fallback, max) {
  if (v === null || v === undefined) return fallback;
  var s = String(v).replace(/\s+/g, ' ').replace(/^ | $/g, '');
  return s.substring(0, max);
}

function defaults() {
  return {
    version: VERSION,
    server: { url: '', token: '', project: '', inProgress: 'In Progress', activeLabel: 'active' },
    timer: { focus: 25, shortBreak: 5, longBreak: 15, rounds: 4 },
    movement: { nudgeMinutes: 5, sensitivity: 1 },
    showMax: 3
  };
}

function normalize(raw) {
  var d = defaults();
  raw = raw && typeof raw === 'object' ? raw : {};
  var server = raw.server || {}, timer = raw.timer || {}, movement = raw.movement || {};
  return {
    version: VERSION,
    server: {
      url: text(server.url, '', 300).replace(/\/+$/, ''),
      token: text(server.token, '', 300),
      project: text(server.project, '', 120),
      inProgress: text(server.inProgress, d.server.inProgress, 120) || d.server.inProgress,
      activeLabel: text(server.activeLabel, d.server.activeLabel, 120)
    },
    timer: {
      focus: clampInt(timer.focus, 1, 120, d.timer.focus),
      shortBreak: clampInt(timer.shortBreak, 1, 60, d.timer.shortBreak),
      longBreak: clampInt(timer.longBreak, 1, 60, d.timer.longBreak),
      rounds: clampInt(timer.rounds, 1, 8, d.timer.rounds)
    },
    movement: {
      nudgeMinutes: clampInt(movement.nudgeMinutes, 1, 60, d.movement.nudgeMinutes),
      sensitivity: clampInt(movement.sensitivity, 0, 2, d.movement.sensitivity)
    },
    showMax: clampInt(raw.showMax, 1, 5, d.showMax)
  };
}

// Enough to talk to the server.
function configured(config) {
  return /^https?:\/\//i.test(config.server.url) && config.server.token !== '' && config.server.project !== '';
}

module.exports = { VERSION: VERSION, defaults: defaults, normalize: normalize, configured: configured };
