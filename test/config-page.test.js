// Settings page: builds src/pkjs/config-page.js, loads it in headless Chromium with injected params, drives the
// DOM contract (CONTRACTS.md §9) and checks what Save hands back to the phone.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');
const { openPage, findChrome } = require('./helpers/chrome');

const root = path.join(__dirname, '..');
const skip = !findChrome() && 'chromium not found (run inside nix-shell)';
const settings = require('../src/common/settings');

const config = settings.normalize({
  server: { url: 'https://tasks.example.org', token: 'tk_abc', project: 'Home', inProgress: 'Doing', activeLabel: 'moving' },
  timer: { focus: 50, shortBreak: 10, longBreak: 20, rounds: 2 },
  movement: { nudgeMinutes: 8, sensitivity: 2 },
  showMax: 4,
});
const remote = { projects: ['Inbox', 'Home'], buckets: ['To-Do', 'Doing', 'Done'], status: '', push: 'on', syncedAt: 1794650000 };

function build() {
  const r = spawnSync(process.execPath, [path.join(root, 'tools/build-config.js')], { encoding: 'utf8', cwd: root });
  assert.equal(r.status, 0, 'build-config failed: ' + r.stderr);
  delete require.cache[require.resolve('../src/pkjs/config-page')];
  return require('../src/pkjs/config-page');
}

async function load(params) {
  const html = build();
  assert.ok(html.includes('<!--PARAMS-->'), 'page keeps the <!--PARAMS--> marker');
  assert.ok(!/(src|href)=["']https?:/i.test(html), 'page must not load external resources');
  const injected = '<script>window.__params = ' + JSON.stringify(params).replace(/</g, '\\u003c') + ';</script>';
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'noticeme-page-'));
  const file = path.join(dir, 'page.html');
  fs.writeFileSync(file, html.replace('<!--PARAMS-->', injected));
  const page = await openPage('file://' + file);
  const close = page.close;
  page.close = async () => { await close(); fs.rmSync(dir, { recursive: true, force: true }); };
  return page;
}

async function saved(page) {
  await page.eval(`document.getElementById('save').click()`);
  for (let i = 0; i < 50 && page.navigations.length === 0; i++) await new Promise(r => setTimeout(r, 50));
  assert.equal(page.navigations.length, 1, 'save navigates to return_to');
  const url = page.navigations[0];
  assert.ok(url.startsWith('http://return.test/close?'), url);
  return JSON.parse(decodeURIComponent(url.slice('http://return.test/close?'.length)));
}

const params = extra => Object.assign({ config, remote, return_to: 'http://return.test/close?' }, extra);
const field = name => `document.querySelector('[data-field="${name}"]')`;
const set = (name, v) => `(() => { const el = ${field(name)}; el.value = ${JSON.stringify(v)};
  el.dispatchEvent(new Event('input', {bubbles: true})); el.dispatchEvent(new Event('change', {bubbles: true})); })()`;

test('config page: shows the injected settings and saves them unchanged', { skip }, async () => {
  const page = await load(params());
  try {
    assert.deepEqual(page.errors, []);
    assert.equal(await page.eval(`${field('server.url')}.value`), 'https://tasks.example.org');
    assert.equal(await page.eval(`${field('server.token')}.value`), 'tk_abc');
    assert.equal(await page.eval(`${field('server.token')}.type`), 'password');
    assert.equal(await page.eval(`${field('server.project')}.value`), 'Home');
    assert.equal(await page.eval(`${field('server.inProgress')}.value`), 'Doing');
    assert.equal(await page.eval(`${field('server.activeLabel')}.value`), 'moving');
    assert.equal(await page.eval(`${field('timer.focus')}.value`), '50');
    assert.equal(await page.eval(`${field('movement.sensitivity')}.value`), '2');
    assert.equal(await page.eval(`${field('showMax')}.value`), '4');
    // what the phone learned from the server is offered
    assert.deepEqual(await page.eval(`[...document.querySelectorAll('#projects option')].map(o => o.value)`), ['Inbox', 'Home']);
    assert.deepEqual(await page.eval(`[...document.querySelectorAll('#buckets option')].map(o => o.value)`), ['To-Do', 'Doing', 'Done']);
    assert.match(await page.eval(`document.getElementById('sync-status').textContent`), /^Tasks fetched /);
    assert.equal(await page.eval(`document.getElementById('sync-note').classList.contains('bad')`), false);
    assert.match(await page.eval(`document.getElementById('push-status').textContent`), /^On:/);
    assert.deepEqual((await saved(page)).config, config);
  } finally { await page.close(); }
});

test('config page: edits are saved, clamped by the settings model', { skip }, async () => {
  const page = await load(params());
  try {
    await page.eval(set('server.url', ' https://other.example.org/ '));
    await page.eval(set('server.project', 'Inbox'));
    await page.eval(set('timer.focus', '500'));
    await page.eval(set('timer.rounds', ''));
    await page.eval(set('movement.nudgeMinutes', '3'));
    await page.eval(set('movement.sensitivity', '0'));
    await page.eval(set('showMax', '1'));
    const out = await saved(page);
    assert.equal(out.config.server.url, 'https://other.example.org');
    assert.equal(out.config.server.project, 'Inbox');
    assert.equal(out.config.timer.focus, 120);
    assert.equal(out.config.timer.rounds, 4, 'an emptied field falls back to the default');
    assert.deepEqual(out.config.movement, { nudgeMinutes: 3, sensitivity: 0 });
    assert.equal(out.config.showMax, 1);
    assert.deepEqual(page.errors, []);
  } finally { await page.close(); }
});

test('config page: first run and trouble', { skip }, async () => {
  // nothing stored yet: defaults, no notes
  let page = await load({ return_to: 'http://return.test/close?' });
  try {
    assert.equal(await page.eval(`${field('server.url')}.value`), '');
    assert.equal(await page.eval(`${field('server.inProgress')}.value`), 'In Progress');
    assert.equal(await page.eval(`${field('timer.focus')}.value`), '25');
    assert.equal(await page.eval(`document.getElementById('sync-note').hidden && document.getElementById('push-note').hidden`), true);
    assert.deepEqual((await saved(page)).config, settings.defaults());
    assert.deepEqual(page.errors, []);
  } finally { await page.close(); }
  // the last sync failed and push is not set up: both are said, in the warning colour
  page = await load(params({ remote: { projects: ['Inbox'], buckets: [], status: 'Project not found', push: 'Pick a push distributor', syncedAt: 0 } }));
  try {
    assert.equal(await page.eval(`document.getElementById('sync-status').textContent`), 'Project not found');
    assert.equal(await page.eval(`document.getElementById('sync-note').classList.contains('bad')`), true);
    assert.equal(await page.eval(`document.getElementById('push-status').textContent`), 'Pick a push distributor');
    assert.equal(await page.eval(`document.getElementById('push-note').classList.contains('bad')`), true);
  } finally { await page.close(); }
});

test('config page: fillForm(readForm()) is a fixed point; the page fits a data: URI', { skip }, async () => {
  const page = await load(params());
  try {
    assert.deepEqual(await page.eval(`(() => { const c = readForm(); fillForm(c); return readForm(); })()`), config);
  } finally { await page.close(); }
  assert.ok(encodeURIComponent(build()).length < 60000, 'the page stays small enough to open as a data: URI');
});
