// An in-memory stand-in for the Vikunja API routes src/common/vikunja.js uses, behind the same
// http(method, url, headers, body, cb(err, status, json)) the phone code gets. Behaves like the real server where
// it matters: bearer token, flat project task list with the `done = false` filter and sorting, kanban view tasks
// filtered by bucket, full-replace task updates, done <-> done bucket, project webhooks.
//   const server = fakeVikunja(); server.http(...); server.requests -> [{method, path, body}]
'use strict';

function fakeVikunja(opts = {}) {
  const base = opts.base || 'https://tasks.test/api/v1';
  const s = {
    token: 'tk_secret',
    offline: false,
    requests: [],
    projects: [{ id: 1, title: 'Inbox' }, { id: 7, title: 'Home' }],
    views: { 7: [{ id: 70, view_kind: 'list', bucket_configuration_mode: 'none' },
                 { id: 71, view_kind: 'kanban', bucket_configuration_mode: 'manual', default_bucket_id: 710, done_bucket_id: 712 }],
             1: [{ id: 10, view_kind: 'list', bucket_configuration_mode: 'none' }] },
    buckets: { 71: [{ id: 710, title: 'To-Do' }, { id: 711, title: 'In Progress' }, { id: 712, title: 'Done' }] },
    // bucket: the task's bucket in view 71
    tasks: [
      { id: 101, project_id: 7, title: 'Water the plants', priority: 2, created: '2026-11-10T09:00:00Z', done: false, labels: null, bucket: 710, description: 'all of them' },
      { id: 102, project_id: 7, title: 'Vacuum the flat', priority: 4, created: '2026-11-12T09:00:00Z', done: false, labels: [{ id: 5, title: 'Active' }], bucket: 710 },
      { id: 103, project_id: 7, title: 'Tax return', priority: 4, created: '2026-11-11T09:00:00Z', done: false, labels: [{ id: 6, title: 'paperwork' }], bucket: 711 },
      { id: 104, project_id: 7, title: 'Old news', priority: 5, created: '2026-11-01T09:00:00Z', done: true, labels: null, bucket: 712 },
      { id: 105, project_id: 7, title: 'Call the dentist', priority: 0, created: '2026-11-09T09:00:00Z', done: false, labels: null, bucket: 710 },
      { id: 201, project_id: 1, title: 'Elsewhere', priority: 5, created: '2026-11-01T09:00:00Z', done: false, labels: null, bucket: 0 },
    ],
    webhooks: [],
    nextHook: 900,
  };

  const pub = t => { const { bucket, ...rest } = t; return { ...rest, bucket_id: 0 }; };
  const sorted = (list, sortBy, orderBy) => list.slice().sort((a, b) => {
    for (let i = 0; i < sortBy.length; i++) {
      const k = sortBy[i], dir = orderBy[i] === 'desc' ? -1 : 1;
      const av = k === 'created' ? Date.parse(a.created) : a[k], bv = k === 'created' ? Date.parse(b.created) : b[k];
      if (av !== bv) return av < bv ? -dir : dir;
    }
    return a.id - b.id;
  });

  function route(method, path, query, body) {
    let m;
    if (method === 'GET' && path === '/projects') return [200, s.projects];
    if ((m = /^\/projects\/(\d+)\/views$/.exec(path)) && method === 'GET') return [200, s.views[m[1]] || []];
    if ((m = /^\/projects\/(\d+)\/views\/(\d+)\/buckets$/.exec(path)) && method === 'GET') return [200, s.buckets[m[2]] || []];
    if ((m = /^\/projects\/(\d+)\/tasks$/.exec(path)) && method === 'GET') {
      if (!s.projects.some(p => p.id === Number(m[1]))) return [404, { message: 'not found' }];
      let list = s.tasks.filter(t => t.project_id === Number(m[1]));
      if (query.getAll('filter').join(' ').includes('done = false')) list = list.filter(t => !t.done);
      list = sorted(list, query.getAll('sort_by'), query.getAll('order_by'));
      return [200, list.slice(0, Number(query.get('per_page')) || 50).map(pub)];
    }
    if ((m = /^\/projects\/(\d+)\/views\/(\d+)\/tasks$/.exec(path)) && method === 'GET') {
      const filter = query.get('filter') || '';
      const b = /bucket_id = (\d+)/.exec(filter);
      let list = s.tasks.filter(t => t.project_id === Number(m[1]));
      if (filter.includes('done = false')) list = list.filter(t => !t.done);
      // without a bucket in the filter a kanban view answers with its buckets, each with its tasks
      if (!b) return [200, (s.buckets[m[2]] || []).map(k => ({ ...k, tasks: list.filter(t => t.bucket === k.id).map(pub) }))];
      return [200, list.filter(t => t.bucket === Number(b[1])).map(pub)];
    }
    if ((m = /^\/projects\/(\d+)\/views\/(\d+)\/buckets\/(\d+)\/tasks$/.exec(path)) && method === 'POST') {
      const task = s.tasks.find(t => t.id === body.task_id && t.project_id === Number(m[1]));
      const view = (s.views[m[1]] || []).find(v => v.id === Number(m[2]));
      if (!task || !view || !(s.buckets[m[2]] || []).some(k => k.id === Number(m[3]))) return [404, { message: 'not found' }];
      task.bucket = Number(m[3]);
      task.done = task.bucket === view.done_bucket_id;
      return [200, { task_id: task.id, bucket_id: task.bucket, project_view_id: view.id }];
    }
    if ((m = /^\/tasks\/(\d+)$/.exec(path))) {
      const i = s.tasks.findIndex(t => t.id === Number(m[1]));
      if (i < 0) return [404, { message: 'not found' }];
      if (method === 'GET') return [200, pub(s.tasks[i])];
      if (method === 'POST') {
        // an update replaces the task: what the body leaves out is reset (labels are kept)
        const old = s.tasks[i];
        const view = (s.views[old.project_id] || []).find(v => v.view_kind === 'kanban');
        const next = { id: old.id, project_id: old.project_id, created: old.created, labels: old.labels, bucket: old.bucket,
          title: body.title || old.title, priority: body.priority || 0, done: !!body.done, description: body.description || '' };
        if (view && next.done !== old.done) next.bucket = next.done ? view.done_bucket_id : view.default_bucket_id;
        s.tasks[i] = next;
        return [200, pub(next)];
      }
    }
    if ((m = /^\/projects\/(\d+)\/webhooks$/.exec(path))) {
      if (method === 'GET') return [200, s.webhooks.filter(h => h.project_id === Number(m[1]))];
      if (method === 'PUT') {
        if (opts.refuseHooks) return [412, { message: 'webhook target is not routable' }];
        const hook = { id: s.nextHook++, project_id: Number(m[1]), target_url: body.target_url, events: body.events };
        s.webhooks.push(hook);
        return [201, hook];
      }
    }
    if ((m = /^\/projects\/(\d+)\/webhooks\/(\d+)$/.exec(path)) && method === 'DELETE') {
      const n = s.webhooks.length;
      s.webhooks = s.webhooks.filter(h => !(h.project_id === Number(m[1]) && h.id === Number(m[2])));
      return s.webhooks.length < n ? [200, { message: 'ok' }] : [404, { message: 'not found' }];
    }
    return [404, { message: 'no such route: ' + method + ' ' + path }];
  }

  s.http = (method, url, headers, body, cb) => {
    setTimeout(() => {
      if (s.offline) return cb(new Error('Network error'));
      if (!url.startsWith(base)) return cb(new Error('unexpected host: ' + url));
      const u = new URL(url);
      const path = u.pathname.slice(new URL(base).pathname.length);
      const parsed = body ? JSON.parse(body) : null;
      s.requests.push({ method, path, query: u.search, body: parsed });
      if ((headers || {}).Authorization !== 'Bearer ' + s.token) return cb(null, 401, { message: 'invalid token' });
      const [status, json] = route(method, path, u.searchParams, parsed);
      cb(null, status, JSON.parse(JSON.stringify(json)));
    }, 0);
  };
  s.task = id => s.tasks.find(t => t.id === id);
  return s;
}

module.exports = { fakeVikunja };
