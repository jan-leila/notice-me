// src/common/vikunja.js against the in-memory server of test/helpers/fake-vikunja.js.
const test = require('node:test');
const assert = require('node:assert/strict');
const vikunja = require('../src/common/vikunja');
const { fakeVikunja } = require('./helpers/fake-vikunja');

const SERVER = { url: 'https://tasks.test', token: 'tk_secret', project: 'home', inProgress: 'in progress', activeLabel: 'active' };
const p = (fn, ...args) => new Promise((res, rej) => fn(...args, (err, v) => (err ? rej(err) : res(v))));
const client = (server, overrides) => vikunja.createClient({ http: server.http }, { ...SERVER, ...overrides });

test('vikunja: helpers', () => {
  assert.equal(vikunja.apiBase('https://tasks.test/'), 'https://tasks.test/api/v1');
  assert.equal(vikunja.apiBase('https://tasks.test/api/v1'), 'https://tasks.test/api/v1');
  const projects = [{ id: 1, title: 'Inbox' }, { id: 7, title: 'Home' }, { id: 9, title: '7' }];
  assert.equal(vikunja.findProject(projects, 'HOME').id, 7);
  assert.equal(vikunja.findProject(projects, '7').id, 7, 'a number is an id first');
  assert.equal(vikunja.findProject(projects, '9').id, 9);
  assert.equal(vikunja.findProject(projects, 'nope'), null);
  assert.equal(vikunja.findBoard([{ id: 1, view_kind: 'kanban', bucket_configuration_mode: 'filter' }]), null);
  assert.equal(vikunja.findBucket([{ id: 3, title: 'Doing' }], 'doing').id, 3);
  assert.deepEqual(vikunja.toTask({ id: 5, title: 'X', priority: 3, created: '2026-11-10T09:00:00Z', labels: [{ title: 'ACTIVE' }] }, 'active', true),
    { id: 5, title: 'X', priority: 3, created: 1794301200, active: true, inProgress: true });
  assert.equal(vikunja.toTask({ id: 5, title: 'X', labels: null }, 'active').active, false);
  assert.equal(vikunja.toTask({ id: 5, title: 'X', labels: [{ title: 'active' }] }, '').active, false, 'no label configured');
  assert.deepEqual(vikunja.sortTasks([{ id: 1, priority: 1, created: 5 }, { id: 2, priority: 3, created: 9 }, { id: 3, priority: 3, created: 4 },
    { id: 4, priority: 3, created: 4 }]).map(t => t.id), [3, 4, 2, 1], 'priority, then added first, then id');
});

test('vikunja: board finds the project, its kanban view and the in-progress bucket', async () => {
  const server = fakeVikunja();
  const b = await p(client(server).board);
  assert.equal(b.project.id, 7);
  assert.equal(b.view.id, 71);
  assert.equal(b.bucket.id, 711);
  assert.deepEqual(b.projects, ['Inbox', 'Home']);
  assert.deepEqual(b.buckets, ['To-Do', 'In Progress', 'Done']);
  // by id; a project without a board; an unknown bucket name
  assert.equal((await p(client(server, { project: '7' }).board)).project.title, 'Home');
  const inbox = await p(client(server, { project: 'Inbox' }).board);
  assert.equal(inbox.view, null);
  assert.equal(inbox.bucket, null);
  assert.equal((await p(client(server, { inProgress: 'Doing' }).board)).bucket, null);
  await assert.rejects(p(client(server, { project: 'Nope' }).board), e => e.code === 'project' && e.projects.length === 2);
  await assert.rejects(p(client(server, { token: 'tk_wrong' }).board), e => e.status === 401);
  server.offline = true;
  await assert.rejects(p(client(server).board), /Network error/);
});

test('vikunja: open tasks sorted by priority then age, with in-progress and active marked', async () => {
  const server = fakeVikunja();
  const c = client(server);
  const b = await p(c.board);
  const tasks = await p(c.tasks, b);
  assert.deepEqual(tasks.map(t => [t.id, t.priority, t.active, t.inProgress]), [
    [103, 4, false, true],    // same priority as 102, added a day earlier
    [102, 4, true, false],
    [101, 2, false, false],
    [105, 0, false, false],
  ]);
  const open = server.requests.find(r => r.path === '/projects/7/tasks');
  assert.equal(open.query, '?filter=done%20%3D%20false&sort_by=priority&order_by=desc&sort_by=created&order_by=asc&per_page=50');
  const started = server.requests.find(r => r.path === '/projects/7/views/71/tasks');
  assert.equal(started.query, '?filter=done%20%3D%20false%20%26%26%20bucket_id%20%3D%20711&per_page=50');
  // no board: tasks all the same, none in progress
  const c2 = client(server, { project: 'Inbox' });
  assert.deepEqual((await p(c2.tasks, await p(c2.board))).map(t => [t.id, t.inProgress]), [[201, false]]);
  // a started task that is not among the first page of open tasks is still listed
  server.task(103).project_id = 7;
  const real = server.http;
  server.http = (method, url, headers, body, cb) => real(method, url, headers, body, (err, status, json) => {
    if (/\/projects\/7\/tasks\?/.test(url)) json = json.filter(t => t.id !== 103);
    cb(err, status, json);
  });
  const c3 = client(server);
  assert.deepEqual((await p(c3.tasks, await p(c3.board))).map(t => t.id), [103, 102, 101, 105]);
});

test('vikunja: start moves the task to the in-progress bucket', async () => {
  const server = fakeVikunja();
  const c = client(server);
  const b = await p(c.board);
  await p(c.start, b, 102);
  assert.equal(server.task(102).bucket, 711);
  assert.deepEqual(server.requests.pop(), { method: 'POST', path: '/projects/7/views/71/buckets/711/tasks', query: '', body: { task_id: 102 } });
  assert.equal((await p(c.tasks, b)).find(t => t.id === 102).inProgress, true);
  await assert.rejects(p(c.start, b, 999), e => e.status === 404);
  const c2 = client(server, { inProgress: 'Doing' });
  await assert.rejects(p(c2.start, await p(c2.board), 102), e => e.code === 'bucket');
});

test('vikunja: done sends the whole task back, so nothing else is reset', async () => {
  const server = fakeVikunja();
  const c = client(server);
  await p(c.done, 101);
  const t = server.task(101);
  assert.equal(t.done, true);
  assert.equal(t.priority, 2);
  assert.equal(t.description, 'all of them');
  assert.equal(t.title, 'Water the plants');
  assert.equal(t.bucket, 712, 'the server moves a done task to the done bucket');
  const before = server.requests.length;
  await p(c.done, 101);   // already done: only looked at
  assert.deepEqual(server.requests.slice(before).map(r => r.method), ['GET']);
  await assert.rejects(p(c.done, 999), e => e.status === 404);
});

test('vikunja: the webhook is created once, reused, and replaced when the endpoint changes', async () => {
  const server = fakeVikunja();
  const c = client(server);
  const b = await p(c.board);
  const hook = await p(c.ensureHook, b, 'https://ntfy.test/up1', null);
  assert.deepEqual(hook, { project: 7, id: 900, url: 'https://ntfy.test/up1' });
  assert.deepEqual(server.webhooks, [{ id: 900, project_id: 7, target_url: 'https://ntfy.test/up1', events: ['task.created', 'task.updated', 'task.deleted'] }]);
  assert.deepEqual(await p(c.ensureHook, b, 'https://ntfy.test/up1', null), hook, 'found again, not duplicated');
  assert.equal(server.webhooks.length, 1);
  // someone else's hook stays; ours moves to the new endpoint
  server.webhooks.push({ id: 5, project_id: 7, target_url: 'https://other.test/hook', events: ['task.created'] });
  const moved = await p(c.ensureHook, b, 'https://ntfy.test/up2', hook);
  assert.equal(moved.url, 'https://ntfy.test/up2');
  assert.deepEqual(server.webhooks.map(h => h.target_url), ['https://other.test/hook', 'https://ntfy.test/up2']);
  await new Promise(res => c.dropHook(moved, res));
  assert.deepEqual(server.webhooks.map(h => h.id), [5]);
  await new Promise(res => c.dropHook(moved, res));   // gone already: still calls back
  const refusing = fakeVikunja({ refuseHooks: true });
  const c2 = client(refusing);
  await assert.rejects(p(c2.ensureHook, await p(c2.board), 'https://ntfy.test/up1', null), e => e.status === 412);
});
