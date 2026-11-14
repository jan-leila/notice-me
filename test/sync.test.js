// src/common/sync.js: the phone's side of keeping the watch and the task server in step.
const test = require('node:test');
const assert = require('node:assert/strict');
const { createSync, KEY } = require('../src/common/sync');
const { fakeVikunja } = require('./helpers/fake-vikunja');

const CONFIG = { server: { url: 'https://tasks.test', token: 'tk_secret', project: 'Home' } };

function setup(config = CONFIG, server = fakeVikunja()) {
  const store = new Map(config ? [[KEY.config, JSON.stringify(config)]] : []);
  const batches = [];
  const sync = createSync({
    storage: { getItem: k => (store.has(k) ? store.get(k) : null), setItem: (k, v) => store.set(k, String(v)) },
    http: server.http,
    send: messages => batches.push(messages),
    log: () => {},
  });
  sync.load();
  const refresh = opts => new Promise((res, rej) => sync.refresh(opts, (err, tasks) => (err ? rej(err) : res(tasks))));
  const last = () => batches[batches.length - 1];
  const ids = batch => batch.filter(m => 'TASK_ID' in m).map(m => m.TASK_ID);
  const settle = () => new Promise(r => setTimeout(r, 60));
  return { sync, server, store, batches, refresh, last, ids, settle };
}

test('sync: a refresh sends settings, status and the tasks, in-progress first', async () => {
  const t = setup();
  const tasks = await t.refresh();
  assert.deepEqual(tasks.map(x => x.id), [103, 102, 101, 105]);
  assert.deepEqual(t.last(), [
    { SETTINGS: [25, 5, 15, 4, 5, 3, 1] },
    { STATUS: '' },
    { TASK_COUNT: 4 },
    { TASK_INDEX: 0, TASK_ID: 103, TASK_TITLE: 'Tax return', TASK_FLAGS: 2, TASK_PRIORITY: 4 },
    { TASK_INDEX: 1, TASK_ID: 102, TASK_TITLE: 'Vacuum the flat', TASK_FLAGS: 1, TASK_PRIORITY: 4 },
    { TASK_INDEX: 2, TASK_ID: 101, TASK_TITLE: 'Water the plants', TASK_FLAGS: 0, TASK_PRIORITY: 2 },
    { TASK_INDEX: 3, TASK_ID: 105, TASK_TITLE: 'Call the dentist', TASK_FLAGS: 0, TASK_PRIORITY: 0 },
  ]);
  // a low-priority task in progress still comes first
  t.server.task(105).bucket = 711;
  await t.refresh();
  assert.deepEqual(t.ids(t.last()), [103, 105, 102, 101]);
  // the list is kept for the next start of the phone side
  const again = setup(CONFIG, t.server);
  again.store.set(KEY.tasks, t.store.get(KEY.tasks));
  again.sync.load();
  again.sync.sendCached();
  assert.deepEqual(again.ids(again.last()), [103, 105, 102, 101]);
  assert.equal(again.server.requests.length, t.server.requests.length, 'sendCached asks the server nothing');
});

test('sync: not set up', async () => {
  const t = setup(null);
  assert.deepEqual(await t.refresh(), []);
  assert.deepEqual(t.last(), [{ SETTINGS: [25, 5, 15, 4, 5, 3, 1] }, { STATUS: 'Set up on phone' }, { TASK_COUNT: 0 }]);
  assert.equal(t.server.requests.length, 0);
});

test('sync: failures leave the watch its list and say why', async () => {
  const t = setup();
  await t.refresh();
  t.server.offline = true;
  await assert.rejects(t.refresh());
  assert.deepEqual(t.last(), [{ SETTINGS: [25, 5, 15, 4, 5, 3, 1] }, { STATUS: 'Offline' }]);
  t.server.offline = false;
  t.server.token = 'tk_changed';
  await assert.rejects(t.refresh());
  assert.deepEqual(t.last()[1], { STATUS: 'Check API token' });
  assert.equal(t.sync.pageParams().remote.status, 'Check API token');
  t.server.token = 'tk_secret';
  await t.refresh();
  assert.deepEqual(t.last()[1], { STATUS: '' });

  const wrong = setup({ server: { ...CONFIG.server, project: 'Nope' } });
  await assert.rejects(wrong.refresh());
  assert.deepEqual(wrong.last()[1], { STATUS: 'Project not found' });
  assert.deepEqual(wrong.sync.pageParams().remote.projects, ['Inbox', 'Home'], 'the page can offer the real ones');

  const noBucket = setup({ server: { ...CONFIG.server, inProgress: 'Doing' } });
  await noBucket.refresh();
  assert.deepEqual(noBucket.last()[1], { STATUS: 'No In Progress bucket' });
  assert.deepEqual(noBucket.ids(noBucket.last()), [103, 102, 101, 105], 'the tasks still come');
  assert.deepEqual(noBucket.sync.pageParams().remote.buckets, ['To-Do', 'In Progress', 'Done']);
});

test('sync: a start on the watch moves the task on the server; a finish marks it done', async () => {
  const t = setup();
  await t.refresh();
  t.sync.handleWatch({ type: 'start', id: 102 });
  await t.settle();
  assert.equal(t.server.task(102).bucket, 711);
  assert.deepEqual(t.ids(t.last()), [103, 102, 101, 105]);
  assert.equal(t.last().find(m => m.TASK_ID === 102).TASK_FLAGS, 3);
  assert.equal(t.store.get(KEY.ops), '[]');

  t.sync.handleWatch({ type: 'done', id: 103 });
  await t.settle();
  assert.equal(t.server.task(103).done, true);
  assert.equal(t.server.task(103).priority, 4, 'the rest of the task survived the update');
  assert.deepEqual(t.ids(t.last()), [102, 101, 105]);
});

test('sync: what the watch did while the server was unreachable is laid over the list and retried', async () => {
  const t = setup();
  await t.refresh();
  t.server.offline = true;
  t.sync.handleWatch({ type: 'start', id: 101 });
  t.sync.handleWatch({ type: 'done', id: 102 });
  t.sync.handleWatch({ type: 'done', id: 102 });   // the watch repeats itself after a lost ack
  await t.settle();
  assert.equal(JSON.parse(t.store.get(KEY.ops)).length, 2);
  // the cached list, as the watch must see it meanwhile
  t.sync.sendCached();
  assert.deepEqual(t.ids(t.last()), [103, 101, 105]);
  assert.equal(t.last().find(m => m.TASK_ID === 101).TASK_FLAGS, 2);
  // the phone side restarts, the server is back
  const next = setup(CONFIG, t.server);
  for (const k of [KEY.tasks, KEY.ops]) next.store.set(k, t.store.get(k));
  next.sync.load();
  t.server.offline = false;
  await next.refresh();
  assert.equal(t.server.task(101).bucket, 711);
  assert.equal(t.server.task(102).done, true);
  assert.equal(next.store.get(KEY.ops), '[]');
  assert.deepEqual(next.ids(next.last()), [103, 101, 105]);
});

test('sync: an op the server will never take is dropped', async () => {
  const t = setup();
  await t.refresh();
  t.sync.handleWatch({ type: 'done', id: 999 });   // deleted meanwhile
  await t.settle();
  assert.equal(t.store.get(KEY.ops), '[]');
  const noBucket = setup({ server: { ...CONFIG.server, inProgress: 'Doing' } });
  noBucket.sync.handleWatch({ type: 'start', id: 102 });
  await noBucket.settle();
  assert.equal(noBucket.store.get(KEY.ops), '[]');
  assert.equal(noBucket.server.task(102).bucket, 710);
  // a server error is retried a few times, then given up on
  const flaky = setup();
  const real = flaky.server.http;
  flaky.server.http = (m, url, h, b, cb) => (m === 'POST' ? setTimeout(() => cb(null, 500, null), 0) : real(m, url, h, b, cb));
  const s2 = setup(CONFIG, flaky.server);
  s2.sync.handleWatch({ type: 'start', id: 102 });
  await s2.settle();
  assert.equal(JSON.parse(s2.store.get(KEY.ops))[0].tries, 1);
  for (let i = 0; i < 4; i++) await s2.refresh();
  assert.equal(s2.store.get(KEY.ops), '[]');
});

test('sync: a refresh for a push tells the watch it may close; overlapping refreshes run one after the other', async () => {
  const t = setup();
  await t.refresh({ quiet: true });
  assert.equal(t.last()[t.last().length - 1].QUIET, 1);
  assert.equal(t.last().filter(m => 'QUIET' in m).length, 1, 'only on the last message');
  // quiet failure: still quiet
  t.server.offline = true;
  await assert.rejects(t.refresh({ quiet: true }));
  assert.deepEqual(t.last()[1], { STATUS: 'Offline', QUIET: 1 });
  t.server.offline = false;
  // ready's refresh is running when the push arrives: a second, quiet one follows
  const before = t.batches.length;
  t.sync.refresh();
  t.sync.refresh({ quiet: true });
  t.sync.refresh({ quiet: true });
  await t.settle();
  assert.equal(t.batches.length, before + 2);
  assert.equal(t.batches[before].some(m => 'QUIET' in m), false);
  assert.equal(t.last()[t.last().length - 1].QUIET, 1);
  // ... but not quiet when the watch itself also asked
  t.sync.refresh();
  t.sync.refresh({ quiet: true });
  t.sync.refresh();
  await t.settle();
  assert.equal(t.last().some(m => 'QUIET' in m), false);
});

test('sync: the push endpoint becomes the project webhook', async () => {
  const t = setup();
  t.sync.setPushEndpoint(null, 'Pick a push distributor');
  await t.refresh();
  assert.equal(t.server.webhooks.length, 0);
  assert.equal(t.sync.pageParams().remote.push, 'Pick a push distributor');

  t.sync.setPushEndpoint('https://ntfy.test/up1');
  await t.refresh();
  assert.deepEqual(t.server.webhooks.map(h => [h.project_id, h.target_url]), [[7, 'https://ntfy.test/up1']]);
  assert.equal(t.sync.pageParams().remote.push, 'on');
  const n = t.server.requests.length;
  await t.refresh();
  assert.equal(t.server.requests.slice(n).some(r => /webhooks/.test(r.path)), false, 'not asked again once set up');
  // a new endpoint replaces the hook; another project moves it
  t.sync.setPushEndpoint('https://ntfy.test/up2');
  await t.refresh();
  assert.deepEqual(t.server.webhooks.map(h => h.target_url), ['https://ntfy.test/up2']);
  t.server.views[1].push({ id: 11, view_kind: 'kanban', bucket_configuration_mode: 'manual' });
  t.sync.setConfig({ server: { ...CONFIG.server, project: 'Inbox' } });
  await t.refresh();
  assert.deepEqual(t.server.webhooks.map(h => [h.project_id, h.target_url]), [[1, 'https://ntfy.test/up2']]);
  assert.deepEqual(t.ids(t.last()), [201]);

  const refusing = setup(CONFIG, fakeVikunja({ refuseHooks: true }));
  refusing.sync.setPushEndpoint('http://192.168.1.5/up1');
  await refusing.refresh();
  assert.match(refusing.sync.pageParams().remote.push, /refused the webhook \(HTTP 412\)/);
  assert.deepEqual(refusing.ids(refusing.last()), [103, 102, 101, 105], 'tasks still sync');
});

test('sync: saving settings for another server forgets the old list', async () => {
  const t = setup();
  await t.refresh();
  t.server.offline = true;
  t.sync.handleWatch({ type: 'start', id: 101 });
  await t.settle();
  t.sync.setConfig({ server: { ...CONFIG.server, url: 'https://other.test' }, showMax: 5 });
  assert.equal(t.store.get(KEY.tasks), '[]');
  assert.equal(t.store.get(KEY.ops), '[]');
  assert.equal(t.sync.getConfig().showMax, 5);
  // same server, other options: the list stays
  const u = setup();
  await u.refresh();
  u.sync.setConfig({ ...CONFIG, showMax: 2 });
  u.sync.sendCached();
  assert.deepEqual(u.last()[0], { SETTINGS: [25, 5, 15, 4, 5, 2, 1] });
  assert.equal(u.ids(u.last()).length, 4);
});
