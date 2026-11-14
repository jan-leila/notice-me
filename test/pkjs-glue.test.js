// Integration: loads the real src/pkjs/index.js against a fake Pebble runtime (Pebble events, sendAppMessage,
// localStorage, XMLHttpRequest backed by the in-memory Vikunja, the phone app's push endpoint) and walks through
// ready -> list -> start -> done -> push -> settings.
const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('path');
const { fakeVikunja } = require('./helpers/fake-vikunja');

const tick = (ms = 80) => new Promise(r => setTimeout(r, ms));

function installFakes(server, storedConfig) {
  const handlers = {};
  const messages = [];
  const opened = [];
  const store = new Map(storedConfig ? [['noticeMe.config', JSON.stringify(storedConfig)]] : []);
  globalThis.Pebble = {
    addEventListener: (name, fn) => { (handlers[name] = handlers[name] || []).push(fn); },
    sendAppMessage: (dict, ok) => { messages.push(dict); setTimeout(() => ok && ok({}), 0); },
    openURL: url => opened.push(url),
    getPushEndpoint: ok => setTimeout(() => ok({ url: 'https://ntfy.test/upABC', p256dh: 'k', auth: 'a' }), 0),
  };
  globalThis.localStorage = {
    getItem: k => (store.has(k) ? store.get(k) : null),
    setItem: (k, v) => store.set(k, String(v)),
    removeItem: k => store.delete(k),
  };
  globalThis.XMLHttpRequest = function () {
    const xhr = this;
    const headers = {};
    xhr.open = (method, url) => { xhr.method = method; xhr.url = url; };
    xhr.setRequestHeader = (k, v) => { headers[k] = v; };
    xhr.abort = () => {};
    xhr.send = body => server.http(xhr.method, xhr.url, headers, body, (err, status, json) => {
      if (err) return xhr.onerror();
      xhr.status = status;
      xhr.responseText = JSON.stringify(json);
      xhr.onload();
    });
  };
  const fire = (name, e) => (handlers[name] || []).forEach(fn => fn(e || {}));
  const ids = () => {
    const start = messages.map(m => 'TASK_COUNT' in m).lastIndexOf(true);
    return messages.slice(start + 1).filter(m => 'TASK_ID' in m).map(m => m.TASK_ID);
  };
  return { handlers, messages, opened, store, fire, ids };
}

const pageParams = url => {
  assert.ok(url.startsWith('data:text/html'), url.slice(0, 40));
  const html = decodeURIComponent(url.slice(url.indexOf(',') + 1));
  return JSON.parse(html.match(/window\.__params = (\{.*?\});<\/script>/s)[1]);
};

test('pkjs glue: ready -> tasks, start and done reach the server, a push refreshes quietly, settings page', async () => {
  const server = fakeVikunja();
  const f = installFakes(server, { server: { url: 'https://tasks.test', token: 'tk_secret', project: 'Home' } });
  require(path.join(__dirname, '..', 'src/pkjs/index.js'));
  assert.ok(f.handlers.ready && f.handlers.push && f.handlers.appmessage, 'registers its events');

  f.fire('ready');
  await tick();
  assert.deepEqual(f.ids(), [103, 102, 101, 105]);
  assert.ok(f.messages.some(m => m.SETTINGS && m.SETTINGS.length === 7));
  assert.deepEqual(server.webhooks.map(h => h.target_url), ['https://ntfy.test/upABC'], 'the push endpoint is the project webhook');

  // the watch starts a task, then finishes it
  f.fire('appmessage', { payload: { EVENT_ID: 102, EVENT_KIND: 1 } });
  await tick();
  assert.equal(server.task(102).bucket, 711);
  assert.equal(f.messages.filter(m => m.TASK_ID === 102).pop().TASK_FLAGS, 3);
  f.fire('appmessage', { payload: { EVENT_ID: 102, EVENT_KIND: 2 } });
  await tick();
  assert.equal(server.task(102).done, true);
  assert.deepEqual(f.ids(), [103, 101, 105]);

  // a task added on the server: its webhook becomes a push, the watch gets the list and may close again
  server.tasks.push({ id: 110, project_id: 7, title: 'Fix the tap', priority: 5, created: '2026-11-14T09:00:00Z', done: false, labels: null, bucket: 710 });
  f.messages.length = 0;
  f.fire('push', { payload: '{"event_name":"task.created"}', encrypted: false });
  await tick();
  assert.deepEqual(f.ids(), [103, 110, 101, 105]);
  assert.equal(f.messages[f.messages.length - 1].QUIET, 1);

  // long SELECT on the watch
  f.messages.length = 0;
  f.fire('appmessage', { payload: { REFRESH: 1 } });
  await tick();
  assert.deepEqual(f.ids(), [103, 110, 101, 105]);
  assert.equal(f.messages.some(m => 'QUIET' in m), false);

  // settings page: opened with the stored settings and what the phone learned from the server
  f.fire('showConfiguration');
  const params = pageParams(f.opened[0]);
  assert.equal(params.config.server.project, 'Home');
  assert.equal(params.return_to, 'pebblejs://close#');
  assert.deepEqual(params.remote.projects, ['Inbox', 'Home']);
  assert.deepEqual(params.remote.buckets, ['To-Do', 'In Progress', 'Done']);
  assert.equal(params.remote.status, '');
  assert.equal(params.remote.push, 'on');

  // saved with other options: stored and sent
  const result = { config: Object.assign({}, params.config, { showMax: 2, timer: { focus: 50, shortBreak: 10, longBreak: 20, rounds: 2 } }) };
  f.messages.length = 0;
  f.fire('webviewclosed', { response: encodeURIComponent(JSON.stringify(result)) });
  await tick();
  assert.equal(JSON.parse(f.store.get('noticeMe.config')).showMax, 2);
  assert.deepEqual(f.messages.find(m => m.SETTINGS).SETTINGS, [50, 10, 20, 2, 5, 2, 1]);

  // closed without saving, junk: no crash, nothing sent
  f.messages.length = 0;
  f.fire('webviewclosed', { response: '' });
  f.fire('webviewclosed', { response: '%7Bnot json' });
  f.fire('appmessage', { payload: {} });
  await tick();
  assert.equal(f.messages.length, 0);
});
