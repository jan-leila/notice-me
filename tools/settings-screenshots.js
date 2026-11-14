#!/usr/bin/env node
// Visual regression screenshots of the settings page (config/config-page.html, bundled as src/pkjs/config-page.js)
// in headless Chromium.
//
//   node tools/settings-screenshots.js [--update] [--threshold F] [--only REGEX] [scheme...]
//                                                        # default colour schemes: light dark
//
// Rebuilds src/pkjs/config-page.js (tools/build-config.js), then for every colour scheme x SCENE loads the page with
// the same injected window.__params src/pkjs/index.js passes (config, remote, return_to) and saves a full-page
// screenshot at phone width to test/settings-screenshots/current/<scheme>/<scene>.png (with --update: .../baseline/).
// Without --update the images are compared with the baseline (tools/lib/png-diff.js): diff images go to
// test/settings-screenshots/diff/, a table is printed, and the exit status is non-zero when any image differs by more
// than F (fraction of pixels, default 0.001) or is missing. Page script errors also fail the run.
//
// Determinism: timezone and locale are pinned (the page formats the time of the last sync), the page loads nothing
// from the network, and CSS transitions, animations and the text caret are off.
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const { execFileSync } = require('child_process');
const { openPage, findChrome } = require('../test/helpers/chrome');

const ROOT = path.join(__dirname, '..');
const OUT = path.join(ROOT, 'test', 'settings-screenshots');
const ALL_SCHEMES = ['light', 'dark'];
const RETURN_TO = 'http://return.test/close?';

const server = { url: 'https://tasks.example.org', token: 'tk_0123456789abcdef', project: 'Home', inProgress: 'In Progress', activeLabel: 'active' };
const SYNCED = Date.UTC(2026, 10, 13, 7, 30, 0) / 1000;

// name, window.__params (return_to is added)
const SCENES = [
  ['01-first-run', {}],
  ['02-set-up', { config: { server }, remote: { projects: ['Inbox', 'Home', 'Work'], buckets: ['To-Do', 'In Progress', 'Done'], status: '', push: 'on', syncedAt: SYNCED } }],
  ['03-trouble', { config: { server: Object.assign({}, server, { project: 'Hoem' }), timer: { focus: 50, shortBreak: 10, longBreak: 30, rounds: 2 },
    movement: { nudgeMinutes: 10, sensitivity: 2 }, showMax: 5 },
    remote: { projects: ['Inbox', 'Home', 'Work'], buckets: [], status: 'Project not found',
      push: 'Pick a push distributor (ntfy, for example) under Watch App Permissions', syncedAt: 0 } }],
];

const STILL_CSS = '*, *::before, *::after { transition: none !important; animation: none !important; caret-color: transparent !important; }';

function pageHtml(params) {
  delete require.cache[require.resolve('../src/pkjs/config-page')];
  const page = require('../src/pkjs/config-page');
  // the same injection src/pkjs/index.js performs
  const injected = '<script>window.__params = ' +
    JSON.stringify(Object.assign({}, params, { return_to: RETURN_TO })).replace(/</g, '\\u003c') + ';</script>';
  return page.replace('<!--PARAMS-->', () => injected);
}

async function main(argv) {
  let update = false, threshold = 0.001, only = null;
  const schemes = [];
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--update') update = true;
    else if (argv[i] === '--threshold') threshold = Number(argv[++i]);
    else if (argv[i] === '--only') only = new RegExp(argv[++i]);
    else if (argv[i] === '-h' || argv[i] === '--help') {
      console.log(fs.readFileSync(__filename, 'utf8').split('\n').slice(1, 16).join('\n'));
      return 0;
    } else if (ALL_SCHEMES.includes(argv[i])) schemes.push(argv[i]);
    else { console.error('unknown argument ' + argv[i]); return 2; }
  }
  if (!schemes.length) schemes.push(...ALL_SCHEMES);
  if (!findChrome()) { console.error('[settings-screenshots] no chromium found (run inside nix-shell or set $CHROME)'); return 2; }

  execFileSync(process.execPath, [path.join(ROOT, 'tools', 'build-config.js')], { stdio: 'inherit' });
  const dest = path.join(OUT, update ? 'baseline' : 'current');
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'noticeme-settings-'));
  let fail = 0;
  try {
    for (const scheme of schemes) {
      const dir = path.join(dest, scheme);
      fs.mkdirSync(dir, { recursive: true });
      for (const f of fs.readdirSync(dir)) if (f.endsWith('.png') && (!only || only.test(f.slice(0, -4)))) fs.rmSync(path.join(dir, f));
      for (const [name, params] of SCENES) {
        if (only && !only.test(name)) continue;
        const file = path.join(tmp, `${scheme}-${name}.html`);
        fs.writeFileSync(file, pageHtml(params));
        const page = await openPage('file://' + file, { width: 400, height: 800, css: STILL_CSS, colorScheme: scheme, timezone: 'UTC', locale: 'en-US' });
        try {
          await page.settle();
          fs.writeFileSync(path.join(dir, name + '.png'), await page.screenshot());
          if (page.errors.length) { fail = 1; console.error(`[settings-screenshots ${scheme}] FAIL: ${name}: page errors: ${page.errors.join(' | ')}`); }
          else console.log(`[settings-screenshots ${scheme}] ${name}`);
        } catch (e) {
          fail = 1; console.error(`[settings-screenshots ${scheme}] FAIL: ${name}: ${e.message}`);
        } finally {
          await page.close();
        }
      }
    }
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
  if (update) { console.log('[settings-screenshots] baseline written to ' + dest); return fail; }

  // compare only the schemes (and with --only, the scenes) that ran
  const cmpBase = fs.mkdtempSync(path.join(os.tmpdir(), 'noticeme-cmp-'));
  const cmpCur = fs.mkdtempSync(path.join(os.tmpdir(), 'noticeme-cur-'));
  try {
    for (const scheme of schemes) {
      fs.mkdirSync(path.join(cmpBase, scheme), { recursive: true });
      const bdir = path.join(OUT, 'baseline', scheme);
      if (fs.existsSync(bdir)) {
        for (const f of fs.readdirSync(bdir)) {
          if (f.endsWith('.png') && (!only || only.test(f.slice(0, -4)))) fs.copyFileSync(path.join(bdir, f), path.join(cmpBase, scheme, f));
        }
      }
      fs.cpSync(path.join(OUT, 'current', scheme), path.join(cmpCur, scheme), { recursive: true });
    }
    fs.rmSync(path.join(OUT, 'diff'), { recursive: true, force: true });
    const code = execFileSyncStatus(process.execPath, [path.join(ROOT, 'tools', 'lib', 'png-diff.js'), 'compare',
      cmpBase, cmpCur, path.join(OUT, 'diff'), '--threshold', String(threshold)]);
    return code || fail;
  } finally {
    fs.rmSync(cmpBase, { recursive: true, force: true });
    fs.rmSync(cmpCur, { recursive: true, force: true });
  }
}

function execFileSyncStatus(cmd, args) {
  try { execFileSync(cmd, args, { stdio: 'inherit' }); return 0; } catch (e) { return e.status || 1; }
}

if (require.main === module) main(process.argv.slice(2)).then(c => process.exit(c), e => { console.error(e); process.exit(1); });
