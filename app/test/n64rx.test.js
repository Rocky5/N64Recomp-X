'use strict';
// The spawn side against the fake n64rx (test/fake-n64rx.js): events, the
// result, errors from JSON and from text, cancel killing the tree.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const n64rx = require('../src/n64rx');
const paths = require('../src/paths');

const tmp = () => fs.mkdtempSync(path.join(os.tmpdir(), 'n64rx-run-'));

function testRom() {
  // a header-only stand-in: the fake reads the header; no game bytes anywhere
  const b = Buffer.alloc(0x1000);
  b.writeUInt32BE(0x80371240, 0);
  b.write('N64RECOMP-X TEST    ', 0x20, 'latin1');
  b.write('NXTE', 0x3b, 'latin1');
  const f = path.join(tmp(), 'test.z64');
  fs.writeFileSync(f, b);
  return f;
}

function fakeRun(env = {}) {
  return paths.runner({ root: paths.devRoot(), cache: tmp(), bundle: null, env: { ...process.env, N64RX_FAKE: '1', N64RX_FAKE_STEP_MS: '40', ...env } });
}

test('info --json gives the ROM, the tier and the problems', async () => {
  const r = await n64rx.info(fakeRun({ N64RX_FAKE_TIER: 'Silver' }), testRom());
  assert.equal(r.ok, true);
  assert.equal(r.info.rom.code, 'NXTE');
  assert.equal(r.info.compat.tier, 'Silver');
  assert.equal(r.info.compat.issues.length, 2);
});

test('info on a file that is not a ROM says what is wrong and what to do', async () => {
  const f = path.join(tmp(), 'notes.z64');
  fs.writeFileSync(f, 'hello, this is not a cartridge dump at all...........................');
  const r = await n64rx.info(fakeRun(), f);
  assert.equal(r.ok, false);
  assert.match(r.error.message, /not an N64 ROM/);
  assert.match(r.error.hint, /\.z64/);
});

test('a build streams five steps and ends with the result', async () => {
  const out = tmp();
  const evs = [];
  const logFile = path.join(tmp(), 'b.log');
  const h = n64rx.runEvents(fakeRun({ N64RX_FAKE_TIER: 'Gold' }), 'build', [testRom(), '--out', out], { onEvent: (e) => evs.push(e), logFile });
  const r = await h.done;
  assert.equal(r.code, 0);
  assert.equal(r.error, null);
  assert.deepEqual(evs.filter((e) => e.ev === 'step').map((e) => e.n), [1, 2, 3, 4, 5]);
  assert.ok(evs.some((e) => e.ev === 'log' && e.text.startsWith('[tool]')), 'non-JSON lines arrive as logs');
  assert.ok(fs.existsSync(path.join(r.result.folder, 'default.xbe')));
  assert.match(r.result.status, /^Status: Gold/);
  assert.match(fs.readFileSync(logFile, 'utf8'), /\[3\/5\] Compiling for the Xbox/);
});

test('a failing step gives an error event with message and hint', async () => {
  const h = n64rx.runEvents(fakeRun({ N64RX_FAKE_FAIL: '2' }), 'build', [testRom(), '--out', tmp()]);
  const r = await h.done;
  assert.equal(r.code, 2);
  assert.match(r.error.message, /failed/);
  assert.ok(r.error.hint);
  assert.equal(r.result, null);
});

test('cancel stops the whole process tree', async () => {
  const evs = [];
  const h = n64rx.runEvents(fakeRun({ N64RX_FAKE_HOLD: '2' }), 'build', [testRom(), '--out', tmp()], { onEvent: (e) => evs.push(e) });
  while (!evs.some((e) => e.ev === 'step' && e.n === 2)) await new Promise((r) => setTimeout(r, 20));
  const pid = h.child.pid;
  h.cancel();
  const r = await h.done;
  assert.equal(r.cancelled, true);
  assert.equal(r.error, null);
  let alive = true;
  try {
    process.kill(pid, 0);
  } catch {
    alive = false;
  }
  assert.equal(alive, false);
});

test('no Python: the error says how to get one', async () => {
  const run = { mode: 'system', cmd: null, args: [], env: process.env, cwd: tmp() };
  const r = await n64rx.runEvents(run, 'build', ['x']).done;
  assert.match(r.error.message, /Python/);
  assert.match(r.error.hint, /python\.org/);
  const i = await n64rx.info(run, 'x');
  assert.equal(i.ok, false);
});

test('plain-text n64rx errors are understood', () => {
  const e = n64rx.fromText(['[1/5] Reading the ROM', '', 'n64rx: No file at x.z64.', 'What to do: Give the path of your ROM.']);
  assert.deepEqual(e, { ev: 'error', message: 'No file at x.z64.', hint: 'Give the path of your ROM.' });
  assert.deepEqual(n64rx.firstJson('warning\n{"a":1}\n'), { a: 1 });
});

test('the FTP password is never written to the job log (argv redacted; the password goes by env)', async () => {
  assert.deepEqual(n64rx.redact(['deploy', 'f', '--pass', 's3cret', '--pass=other', '--password=x', '--user=me']),
    ['deploy', 'f', '--pass', '***', '--pass=***', '--password=***', '--user=me']);
  const folder = tmp();
  fs.writeFileSync(path.join(folder, 'default.xbe'), 'x');
  const logFile = path.join(tmp(), 'd.log');
  const r = await n64rx.runEvents(fakeRun(), 'deploy', [folder, '--ftp=192.168.1.50', '--user=-odd', '--to=/E/Games'], { logFile, env: { N64RX_FTP_PASS: 's3cret' } }).done;
  assert.equal(r.code, 0, JSON.stringify(r.error));
  assert.ok(!fs.readFileSync(logFile, 'utf8').includes('s3cret'));
});
