// src/common/settings.js: defaults, clamping, what counts as set up.
const test = require('node:test');
const assert = require('node:assert/strict');
const settings = require('../src/common/settings');

test('settings: defaults', () => {
  const d = settings.defaults();
  assert.deepEqual(d, {
    version: 1,
    server: { url: '', token: '', project: '', inProgress: 'In Progress', activeLabel: 'active' },
    timer: { focus: 25, shortBreak: 5, longBreak: 15, rounds: 4 },
    movement: { nudgeMinutes: 5, sensitivity: 1 },
    showMax: 3,
  });
  assert.deepEqual(settings.normalize(null), d);
  assert.deepEqual(settings.normalize('junk'), d);
  assert.deepEqual(settings.normalize(d), d, 'normalize is a fixed point');
});

test('settings: values are clamped and tidied', () => {
  const c = settings.normalize({
    server: { url: ' https://tasks.example.org/// ', token: ' tk_abc ', project: '  Home  ', inProgress: '', activeLabel: '' },
    timer: { focus: 500, shortBreak: 0, longBreak: 'x', rounds: 2.6 },
    movement: { nudgeMinutes: -4, sensitivity: 9 },
    showMax: '4',
    extra: 1,
  });
  assert.deepEqual(c.server, { url: 'https://tasks.example.org', token: 'tk_abc', project: 'Home', inProgress: 'In Progress', activeLabel: '' },
    'an empty bucket name falls back; an empty label means no task is active');
  assert.deepEqual(c.timer, { focus: 120, shortBreak: 1, longBreak: 15, rounds: 3 });
  assert.deepEqual(c.movement, { nudgeMinutes: 1, sensitivity: 2 });
  assert.equal(c.showMax, 4);
  assert.equal('extra' in c, false);
  assert.equal(settings.normalize({ showMax: 99 }).showMax, 5);
  assert.equal(settings.normalize({ server: { project: 7 } }).server.project, '7');
});

test('settings: configured needs a server, a token and a project', () => {
  const ok = { url: 'https://tasks.example.org', token: 'tk_abc', project: 'Home' };
  assert.equal(settings.configured(settings.normalize({ server: ok })), true);
  assert.equal(settings.configured(settings.defaults()), false);
  for (const k of ['url', 'token', 'project']) {
    assert.equal(settings.configured(settings.normalize({ server: { ...ok, [k]: '' } })), false, k);
  }
  assert.equal(settings.configured(settings.normalize({ server: { ...ok, url: 'tasks.example.org' } })), false, 'no scheme');
});
