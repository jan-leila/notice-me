// Keeps the watch's task list in step with the task server (CONTRACTS.md §7): fetches the open tasks, carries what
// the watch started and finished back to the server, and registers the push hook. ES5; everything from outside is
// injected:
//   deps = { storage (localStorage-like), http(method, url, headers, body, cb(err, status, json)),
//            send(messages), log(text), backend (default: ./vikunja) }

var settings = require('./settings');
var protocol = require('./protocol');
var vikunja = require('./vikunja');

var KEY = { config: 'noticeMe.config', tasks: 'noticeMe.tasks', ops: 'noticeMe.ops', hook: 'noticeMe.hook', remote: 'noticeMe.remote' };
var OP_TRIES = 5;   // a start or finish the server keeps refusing is given up on

function createSync(deps) {
  var backend = deps.backend || vikunja;
  var log = deps.log || function () {};
  var config = settings.defaults();
  var tasks = [];      // the server's open tasks, sorted, as of the last fetch
  var ops = [];        // [{kind: 'start' | 'done', id, tries}] not yet on the server
  var hook = null;     // the webhook registered for the push endpoint
  var remote = { projects: [], buckets: [], status: '', push: '', syncedAt: 0 };  // for the settings page
  var endpoint = null; // the phone's push endpoint URL, once known
  var busy = false;
  var again = null;    // a refresh asked for while one was running: {quiet}

  function read(key, fallback) {
    try {
      var v = JSON.parse(deps.storage.getItem(key));
      return v === null || v === undefined ? fallback : v;
    } catch (e) {
      return fallback;
    }
  }
  function write(key, value) {
    deps.storage.setItem(key, JSON.stringify(value));
  }

  function load() {
    config = settings.normalize(read(KEY.config, null));
    tasks = read(KEY.tasks, []);
    ops = read(KEY.ops, []);
    hook = read(KEY.hook, null);
    remote = read(KEY.remote, remote);
    if (!(tasks instanceof Array)) tasks = [];
    if (!(ops instanceof Array)) ops = [];
  }

  // The list as the watch should see it: the fetched tasks with what it did since laid over them, the ones in
  // progress first so they survive the cut to what the watch holds.
  function view() {
    var done = {}, started = {}, out = [], i;
    for (i = 0; i < ops.length; i++) (ops[i].kind === 'done' ? done : started)[ops[i].id] = true;
    for (var pass = 0; pass < 2; pass++) {
      for (i = 0; i < tasks.length; i++) {
        var t = tasks[i];
        var inProgress = t.inProgress || !!started[t.id];
        if (done[t.id] || inProgress !== (pass === 0)) continue;
        out.push({ id: t.id, title: t.title, priority: t.priority, active: t.active, inProgress: inProgress });
      }
    }
    return out;
  }

  function sendState(withTasks, quiet) {
    var messages = [protocol.encodeSettings(config), protocol.encodeStatus(remote.status)];
    if (withTasks) messages = messages.concat(protocol.encodeTasks(view()));
    if (quiet) messages[messages.length - 1].QUIET = 1;   // with the last message: the watch has it all by then
    deps.send(messages);
  }

  function statusFor(err) {
    if (err.code === 'project') return 'Project not found';
    if (err.status === 401 || err.status === 403) return 'Check API token';
    if (err.status) return 'Server error ' + err.status;
    return 'Offline';
  }

  // Carries the watch's starts and finishes to the server, one after the other. An op the server will never take
  // (the task is gone, there is no bucket to move it to) is dropped; others stay for the next refresh.
  function runOps(client, board, cb) {
    var todo = ops.slice();
    (function next() {
      var op = todo.shift();
      if (!op) return cb();
      function finished(err) {
        var keep = false;
        if (err) {
          log(op.kind + ' ' + op.id + ' failed: ' + err.message);
          op.tries = (op.tries || 0) + 1;
          keep = err.status !== 404 && err.code !== 'bucket' && op.tries < OP_TRIES;
        }
        if (!keep) {
          var rest = [];
          for (var i = 0; i < ops.length; i++) if (ops[i] !== op) rest.push(ops[i]);
          ops = rest;
        }
        write(KEY.ops, ops);
        next();
      }
      if (op.kind === 'done') client.done(op.id, finished);
      else client.start(board, op.id, finished);
    })();
  }

  // Points the project's webhook at the push endpoint, when there is one and it is not already set up.
  function syncHook(client, board, cb) {
    if (!endpoint) return cb();
    if (hook && hook.url === endpoint && hook.project === board.project.id) {
      remote.push = 'on';
      return cb();
    }
    function ensure() {
      client.ensureHook(board, endpoint, hook, function (err, made) {
        if (err) {
          log('webhook: ' + err.message);
          remote.push = 'The server refused the webhook (' + err.message + ')';
        } else {
          hook = made;
          write(KEY.hook, hook);
          remote.push = 'on';
        }
        cb();
      });
    }
    if (hook && hook.project !== board.project.id) client.dropHook(hook, ensure);
    else ensure();
  }

  // Fetches and sends everything. opts.quiet: this refresh answers a push, so the watch may close again after it.
  // cb(err, tasks) is optional.
  function refresh(opts, cb) {
    var quiet = !!(opts && opts.quiet);
    if (busy) {
      again = { quiet: again ? again.quiet && quiet : quiet };
      if (cb) cb(null, null);
      return;
    }
    function finish(err) {
      busy = false;
      if (err) {
        remote.status = statusFor(err);
        if (err.projects) remote.projects = err.projects;
        log('refresh failed: ' + err.message);
      }
      write(KEY.remote, remote);
      sendState(!err, quiet);   // on a failure the watch keeps the list it has
      if (cb) cb(err || null, err ? null : view());
      if (again) {
        var o = again;
        again = null;
        refresh(o);
      }
    }
    if (!settings.configured(config)) {
      remote.status = 'Set up on phone';
      tasks = [];
      write(KEY.tasks, tasks);
      write(KEY.remote, remote);
      sendState(true, quiet);
      if (cb) cb(null, []);
      return;
    }
    busy = true;
    var client = backend.createClient({ http: deps.http }, config.server);
    client.board(function (err, board) {
      if (err) return finish(err);
      remote.projects = board.projects;
      remote.buckets = board.buckets;
      runOps(client, board, function () {
        client.tasks(board, function (err2, list) {
          if (err2) return finish(err2);
          tasks = list;
          write(KEY.tasks, tasks);
          remote.status = board.bucket ? '' : 'No In Progress bucket';
          remote.syncedAt = Math.floor(Date.now() / 1000);
          syncHook(client, board, function () { finish(null); });
        });
      });
    });
  }

  // What the watch reports (protocol.decodeWatchMessage).
  function handleWatch(msg) {
    if (!msg) return;
    if (msg.type === 'start' || msg.type === 'done') {
      var have = false;
      for (var i = 0; i < ops.length; i++) if (ops[i].kind === msg.type && ops[i].id === msg.id) have = true;
      if (!have) ops.push({ kind: msg.type, id: msg.id, tries: 0 });
      write(KEY.ops, ops);
    }
    refresh();
  }

  // The settings page's Save. A different server or project starts from an empty list.
  function setConfig(raw) {
    var next = settings.normalize(raw);
    if (next.server.url !== config.server.url || next.server.project !== config.server.project) {
      tasks = [];
      ops = [];
      write(KEY.tasks, tasks);
      write(KEY.ops, ops);
    }
    config = next;
    write(KEY.config, config);
  }

  // The push endpoint (index.js asks the phone app for it), or null with the reason there is none.
  function setPushEndpoint(url, reason) {
    endpoint = url || null;
    if (!endpoint) {
      remote.push = reason || '';
      write(KEY.remote, remote);
    }
  }

  return {
    load: load, refresh: refresh, handleWatch: handleWatch, setConfig: setConfig, setPushEndpoint: setPushEndpoint,
    sendCached: function () { sendState(true, false); },
    getConfig: function () { return config; },
    pageParams: function () { return { config: config, remote: remote }; }
  };
}

module.exports = { KEY: KEY, createSync: createSync };
