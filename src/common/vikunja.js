// Vikunja as the task list (CONTRACTS.md §8): the open tasks of one project, "in progress" as a kanban bucket,
// "active" as a label, and a project webhook that tells the phone when something changed. Pure; ES5.
// All requests go through the injected http(method, url, headers, body, cb(err, status, json)).
//
// API used (paths under <server>/api/v1, Authorization: Bearer <API token>):
//   GET  /projects                                              the projects, to find the configured one
//   GET  /projects/{p}/views, /projects/{p}/views/{v}/buckets   the kanban view and its buckets
//   GET  /projects/{p}/tasks?filter=done = false&sort_by=...    open tasks, most important and oldest first
//   GET  /projects/{p}/views/{v}/tasks?filter=... bucket_id = B the tasks in the "in progress" bucket
//   POST /projects/{p}/views/{v}/buckets/{b}/tasks {task_id}    start: move a task to "in progress"
//   GET + POST /tasks/{id}                                      done (an update replaces the whole task)
//   GET / PUT / DELETE /projects/{p}/webhooks                   the push hook

var PER_PAGE = 50;
var HOOK_EVENTS = ['task.created', 'task.updated', 'task.deleted'];

// 'https://tasks.example.org/' -> 'https://tasks.example.org/api/v1'
function apiBase(url) {
  var base = String(url || '').replace(/\/+$/, '');
  return /\/api\/v\d+$/.test(base) ? base : base + '/api/v1';
}

function same(a, b) {
  return String(a).toLowerCase() === String(b).toLowerCase();
}

// The project with that id, or else that title (case-insensitive); null when there is none.
function findProject(projects, wanted) {
  var i;
  wanted = String(wanted).replace(/^ +| +$/g, '');
  if (/^\d+$/.test(wanted)) {
    for (i = 0; i < projects.length; i++) if (projects[i].id === Number(wanted)) return projects[i];
  }
  for (i = 0; i < projects.length; i++) if (same(projects[i].title, wanted)) return projects[i];
  return null;
}

// The first kanban view whose buckets are filled by hand.
function findBoard(views) {
  for (var i = 0; i < views.length; i++) {
    if (views[i].view_kind === 'kanban' && views[i].bucket_configuration_mode === 'manual') return views[i];
  }
  return null;
}

function findBucket(buckets, title) {
  for (var i = 0; i < buckets.length; i++) if (same(buckets[i].title, title)) return buckets[i];
  return null;
}

function hasLabel(raw, label) {
  var labels = raw.labels || [];
  if (!label) return false;
  for (var i = 0; i < labels.length; i++) if (labels[i] && same(labels[i].title, label)) return true;
  return false;
}

function toTask(raw, activeLabel, inProgress) {
  var created = Date.parse(raw.created);
  return {
    id: raw.id,
    title: String(raw.title || ''),
    priority: Number(raw.priority) || 0,
    created: isFinite(created) ? Math.floor(created / 1000) : 0,
    active: hasLabel(raw, activeLabel),
    inProgress: !!inProgress
  };
}

// Highest priority first, then the task added first (then the lower id, so the order is total).
function sortTasks(tasks) {
  return tasks.slice().sort(function (a, b) {
    return b.priority - a.priority || a.created - b.created || a.id - b.id;
  });
}

function httpError(status) {
  var e = new Error('HTTP ' + status);
  e.status = status;
  return e;
}

function createClient(deps, server) {
  var base = apiBase(server.url);
  var headers = { Authorization: 'Bearer ' + server.token };

  function call(method, path, body, cb) {
    var h = { Authorization: headers.Authorization };
    if (body !== null) h['Content-Type'] = 'application/json';
    deps.http(method, base + path, h, body === null ? null : JSON.stringify(body), function (err, status, json) {
      if (err) return cb(err);
      if (status < 200 || status >= 300) return cb(httpError(status));
      cb(null, json);
    });
  }
  function list(path, cb) {
    call('GET', path, null, function (err, json) {
      if (err) return cb(err);
      cb(null, json instanceof Array ? json : []);
    });
  }
  function query(params) {
    var out = [];
    for (var i = 0; i < params.length; i++) out.push(params[i][0] + '=' + encodeURIComponent(params[i][1]));
    return '?' + out.join('&');
  }

  // cb(err, {project, view, bucket, projects, buckets}): the configured project with its kanban view and
  // "in progress" bucket (view and bucket are null when the project has none), plus the titles the settings page
  // offers. err.code is 'project' when the project is not there.
  function board(cb) {
    list('/projects' + query([['per_page', PER_PAGE]]), function (err, projects) {
      if (err) return cb(err);
      var titles = [];
      for (var i = 0; i < projects.length; i++) titles.push(String(projects[i].title));
      var project = findProject(projects, server.project);
      if (!project) {
        var e = new Error('project not found: ' + server.project);
        e.code = 'project';
        e.projects = titles;
        return cb(e);
      }
      list('/projects/' + project.id + '/views', function (err2, views) {
        if (err2) return cb(err2);
        var view = findBoard(views);
        if (!view) return cb(null, { project: project, view: null, bucket: null, projects: titles, buckets: [] });
        list('/projects/' + project.id + '/views/' + view.id + '/buckets', function (err3, buckets) {
          if (err3) return cb(err3);
          var names = [];
          for (var j = 0; j < buckets.length; j++) names.push(String(buckets[j].title));
          cb(null, { project: project, view: view, bucket: findBucket(buckets, server.inProgress), projects: titles, buckets: names });
        });
      });
    });
  }

  // cb(err, tasks): the open tasks of the board's project, sorted, each {id, title, priority, created, active,
  // inProgress}.
  function tasks(b, cb) {
    var open = query([['filter', 'done = false'], ['sort_by', 'priority'], ['order_by', 'desc'], ['sort_by', 'created'],
                      ['order_by', 'asc'], ['per_page', PER_PAGE]]);
    list('/projects/' + b.project.id + '/tasks' + open, function (err, raw) {
      if (err) return cb(err);
      if (!b.bucket) return finish(raw, []);
      var started = query([['filter', 'done = false && bucket_id = ' + b.bucket.id], ['per_page', PER_PAGE]]);
      list('/projects/' + b.project.id + '/views/' + b.view.id + '/tasks' + started, function (err2, inBucket) {
        if (err2) return cb(err2);
        finish(raw, inBucket);
      });
    });
    function finish(raw, inBucket) {
      var started = {}, seen = {}, out = [], i;
      for (i = 0; i < inBucket.length; i++) {
        // a server that ignores the bucket filter answers with buckets instead of tasks: no ids here, nothing marked
        if (inBucket[i] && typeof inBucket[i].id === 'number' && !inBucket[i].tasks) started[inBucket[i].id] = inBucket[i];
      }
      for (i = 0; i < raw.length; i++) {
        if (!raw[i] || typeof raw[i].id !== 'number' || raw[i].done) continue;
        seen[raw[i].id] = true;
        out.push(toTask(raw[i], server.activeLabel, !!started[raw[i].id]));
      }
      // a started task beyond the first page of open tasks still belongs on the watch
      for (var id in started) {
        if (started.hasOwnProperty(id) && !seen[id] && !started[id].done) out.push(toTask(started[id], server.activeLabel, true));
      }
      cb(null, sortTasks(out));
    }
  }

  function start(b, taskId, cb) {
    if (!b.bucket) {
      var e = new Error('no "' + server.inProgress + '" bucket');
      e.code = 'bucket';
      return cb(e);
    }
    call('POST', '/projects/' + b.project.id + '/views/' + b.view.id + '/buckets/' + b.bucket.id + '/tasks', { task_id: taskId }, cb);
  }

  // An update replaces the task, so the whole task goes back with done set.
  function done(taskId, cb) {
    call('GET', '/tasks/' + taskId, null, function (err, task) {
      if (err) return cb(err);
      if (!task || typeof task !== 'object') return cb(new Error('unexpected answer for task ' + taskId));
      if (task.done) return cb(null, task);
      task.done = true;
      call('POST', '/tasks/' + taskId, task, cb);
    });
  }

  // Makes sure the project posts its task changes to `url` (the phone's push endpoint). `known` is what an earlier
  // call returned ({project, id, url}, or null): a hook of ours that points elsewhere is removed.
  // cb(err, {project, id, url}).
  function ensureHook(b, url, known, cb) {
    var path = '/projects/' + b.project.id + '/webhooks';
    list(path, function (err, hooks) {
      if (err) return cb(err);
      var i, stale = [];
      for (i = 0; i < hooks.length; i++) {
        if (hooks[i].target_url === url) return cb(null, { project: b.project.id, id: hooks[i].id, url: url });
        if (known && known.project === b.project.id && hooks[i].id === known.id) stale.push(hooks[i].id);
      }
      (function drop() {
        if (stale.length) return call('DELETE', path + '/' + stale.shift(), null, drop);  // best effort
        call('PUT', path, { target_url: url, events: HOOK_EVENTS }, function (err2, hook) {
          if (err2) return cb(err2);
          cb(null, { project: b.project.id, id: hook && hook.id, url: url });
        });
      })();
    });
  }

  // Removes a hook an earlier ensureHook() made (in whatever project it was). Best effort: cb() always.
  function dropHook(known, cb) {
    call('DELETE', '/projects/' + known.project + '/webhooks/' + known.id, null, function () { cb(); });
  }

  return { board: board, tasks: tasks, start: start, done: done, ensureHook: ensureHook, dropHook: dropHook };
}

module.exports = {
  HOOK_EVENTS: HOOK_EVENTS,
  apiBase: apiBase, findProject: findProject, findBoard: findBoard, findBucket: findBucket, toTask: toTask,
  sortTasks: sortTasks, createClient: createClient
};
