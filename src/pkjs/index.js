// PebbleKit JS entry: wires the sync to Pebble, XMLHttpRequest, localStorage and the phone app's push
// (CONTRACTS.md §7). Thin glue. ES5.

var protocol = require('../common/protocol');
var syncModule = require('../common/sync');

// The settings page is bundled with the app (see tools/build-config.js) and handed to the phone
// app as a data: URI, so no external server is needed.
var CONFIG_PAGE = require('./config-page');

var HTTP_TIMEOUT_MS = 20000;
var RETURN_TO = 'pebblejs://close#';
var PUSH_REASONS = {
  permission_denied: 'Allow Push for Notice Me under Watch App Permissions',
  no_distributor: 'Pick a push distributor (ntfy, for example) under Watch App Permissions',
  registration_failed: 'The push distributor refused the registration',
  timeout: 'The push distributor did not answer',
  unsupported: 'This phone app has no push for watch apps'
};

function log(msg) {
  console.log('[notice-me] ' + msg);
}

// cb(err, status, parsedJson). Calls cb exactly once; a body that is not JSON comes back as null.
function http(method, url, headers, body, cb) {
  var done = false;
  function finish(err, status, json) {
    if (done) return;
    done = true;
    cb(err, status, json);
  }
  var xhr;
  try {
    xhr = new XMLHttpRequest();
    xhr.open(method, url, true);
    xhr.timeout = HTTP_TIMEOUT_MS;
    for (var h in headers || {}) if (headers.hasOwnProperty(h)) xhr.setRequestHeader(h, headers[h]);
  } catch (e) {
    finish(e);
    return;
  }
  // Some PebbleKit JS runtimes ignore xhr.timeout; keep our own timer too.
  var timer = setTimeout(function () {
    try { xhr.abort(); } catch (e) { /* ignore */ }
    finish(new Error('Request timed out'));
  }, HTTP_TIMEOUT_MS + 1000);
  xhr.onload = function () {
    clearTimeout(timer);
    var json = null;
    try { json = JSON.parse(xhr.responseText); } catch (e) { /* not JSON */ }
    finish(null, xhr.status, json);
  };
  xhr.onerror = function () {
    clearTimeout(timer);
    finish(new Error('Network error'));
  };
  xhr.ontimeout = function () {
    clearTimeout(timer);
    finish(new Error('Request timed out'));
  };
  try {
    xhr.send(body === null || body === undefined ? null : body);
  } catch (e) {
    clearTimeout(timer);
    finish(e);
  }
}

var sender = protocol.createSender(function (dict, onOk, onErr) {
  Pebble.sendAppMessage(dict, onOk, function (e) {
    log('send failed: ' + JSON.stringify(dict));
    onErr(e);
  });
}, { retries: 2 });

var sync = syncModule.createSync({
  storage: localStorage,
  http: http,
  send: function (messages) { sender.sendAll(messages); },
  log: log
});

// The phone app (Gravel) hands out a UnifiedPush endpoint per watch app; the task server posts to it when a task
// changes, which opens the app and fires 'push' below. It can change, so it is asked for on every start.
function askForPush(then) {
  if (typeof Pebble.getPushEndpoint !== 'function') {
    sync.setPushEndpoint(null, PUSH_REASONS.unsupported);
    return then();
  }
  var answered = false;
  function answer(url, reason) {
    if (answered) return;
    answered = true;
    sync.setPushEndpoint(url, reason);
    then();
  }
  try {
    Pebble.getPushEndpoint(function (endpoint) {
      answer(endpoint && endpoint.url, PUSH_REASONS.registration_failed);
    }, function (reason) {
      log('no push: ' + reason);
      answer(null, PUSH_REASONS[reason] || String(reason));
    });
  } catch (e) {
    answer(null, PUSH_REASONS.unsupported);
  }
}

Pebble.addEventListener('ready', function () {
  log('PebbleKit JS ready');
  sync.load();
  sync.sendCached();   // what was known last time, at once
  askForPush(function () { sync.refresh(); });
});

// Something changed on the task server. The app may have been opened just for this: let it close again.
Pebble.addEventListener('push', function () {
  log('push');
  sync.refresh({ quiet: true });
});

Pebble.addEventListener('appmessage', function (e) {
  sync.handleWatch(protocol.decodeWatchMessage(e.payload));
});

Pebble.addEventListener('showConfiguration', function () {
  var p = sync.pageParams();
  var params = { config: p.config, remote: p.remote, return_to: RETURN_TO };
  var injected = '<script>window.__params = ' +
      JSON.stringify(params).replace(/</g, '\\u003c') + ';</script>';
  // Function replacement so '$' sequences in the JSON are not treated as patterns.
  var html = CONFIG_PAGE.replace('<!--PARAMS-->', function () { return injected; });
  Pebble.openURL('data:text/html;charset=utf-8,' + encodeURIComponent(html));
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response || e.response === 'CANCELLED') {
    log('settings closed without saving');
    return;
  }
  var result;
  try {
    result = JSON.parse(decodeURIComponent(e.response));
  } catch (err) {
    log('bad settings response: ' + e.response);
    return;
  }
  if (!result || !result.config) return;
  sync.setConfig(result.config);
  sync.refresh();
});
