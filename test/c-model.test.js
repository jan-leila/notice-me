// Compiles the host C tests in test/c/ against the pure watch modules in src/c/ and runs them.
const test = require('node:test');
const assert = require('node:assert/strict');
const { spawnSync } = require('child_process');
const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..');
const hasGcc = spawnSync('gcc', ['--version']).status === 0;
const skip = !hasGcc && 'gcc not found (run inside nix-shell)';

function runC(name, sources) {
  const outDir = path.join(root, 'test', 'out');
  fs.mkdirSync(outDir, { recursive: true });
  const exe = path.join(outDir, name);
  const cc = spawnSync('gcc', ['-std=c99', '-Wall', '-Wextra', '-Werror', '-D_POSIX_C_SOURCE=200809L',
    '-I', path.join(root, 'src/c'), ...sources.map(s => path.join(root, s)), '-lm', '-o', exe], { encoding: 'utf8' });
  assert.equal(cc.status, 0, 'compile failed:\n' + cc.stderr);
  const run = spawnSync(exe, [], { encoding: 'utf8', env: { ...process.env, TZ: 'UTC' } });
  assert.equal(run.status, 0, run.stdout + run.stderr);
}

test('C nm_tasks list (host gcc)', { skip }, () => runC('test_nm_tasks', ['test/c/test_nm_tasks.c', 'src/c/nm_tasks.c']));
test('C nm_session phases (host gcc)', { skip }, () => runC('test_nm_session', ['test/c/test_nm_session.c', 'src/c/nm_session.c']));
test('C nm_motion stillness (host gcc)', { skip }, () => runC('test_nm_motion', ['test/c/test_nm_motion.c', 'src/c/nm_motion.c']));
test('C nm_wheel layout (host gcc)', { skip }, () => runC('test_nm_wheel', ['test/c/test_nm_wheel.c', 'src/c/nm_wheel.c']));
test('C nm_marquee timing (host gcc)', { skip }, () => runC('test_nm_marquee', ['test/c/test_nm_marquee.c', 'src/c/nm_marquee.c']));
