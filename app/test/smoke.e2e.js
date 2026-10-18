'use strict';
// The app end to end (Playwright for Electron): launch, load a ROM, see the
// tier badge, build with the fake n64rx, reach Done with a folder holding
// default.xbe. A second run uses the real `n64rx info` on the test ROM
// (tools/xbox/make_test_rom.py) when Python is there.
// N64RX_SKIP_E2E=1 skips (no display).
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const paths = require('../src/paths');

const APP = path.resolve(__dirname, '..');
const skip = process.env.N64RX_SKIP_E2E === '1';
const tmp = () => fs.mkdtempSync(path.join(os.tmpdir(), 'n64rx-e2e-'));

async function launch(env) {
  const { _electron: electron } = require('playwright-core');
  const user = tmp();
  const app = await electron.launch({
    executablePath: require('electron'), args: [APP], cwd: APP,
    // the folder picker answers with N64RX_TEST_PICK_OUT (a dev-only hook in main.js)
    env: { ...process.env, N64RX_USER_DATA: user, N64RX_CACHE: path.join(user, 'cache'), N64RX_TEST_PICK_OUT: path.join(user, 'out'), N64RX_TEST_PICK_ISO: path.join(user, 'game.iso'), ...env },
  });
  const page = await app.firstWindow();
  const errors = [];
  page.on('pageerror', (e) => errors.push(String(e)));
  page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });
  await page.waitForSelector('#intro.gone', { state: 'attached', timeout: 15000 });
  await page.waitForSelector('[data-screen="home"]:not([hidden])');
  return { app, page, errors, user };
}

function headerRom() {
  const b = Buffer.alloc(0x1000);
  b.writeUInt32BE(0x80371240, 0);
  b.write('N64RECOMP-X TEST    ', 0x20, 'latin1');
  b.write('NXTE', 0x3b, 'latin1');
  const f = path.join(tmp(), 'test.z64');
  fs.writeFileSync(f, b);
  return f;
}

test('fake n64rx: ROM -> Experimental badge -> Build -> Done with default.xbe', { skip, timeout: 90000 }, async () => {
  const out = tmp();
  const { app, page, errors } = await launch({ N64RX_FAKE: '1', N64RX_FAKE_STEP_MS: '200', N64RX_TEST_PICK_OUT: out });
  try {
    await page.evaluate((r) => window.__n64rxTest.loadRom(r), headerRom());
    await page.waitForSelector('[data-screen="game"]:not([hidden])');
    assert.equal(await page.textContent('#g-tier-name'), 'EXPERIMENTAL');
    assert.match(await page.textContent('#g-problems'), /Not proven on a real Xbox yet/);
    await page.click('#g-build');
    await page.waitForSelector('[data-screen="build"]:not([hidden])');
    await page.waitForSelector('[data-screen="done"]:not([hidden])', { timeout: 30000 });
    const folder = await page.getAttribute('#d-folder', 'title');
    assert.ok(fs.existsSync(path.join(folder, 'default.xbe')), folder);
    assert.ok(folder.startsWith(out));
    assert.equal(await page.textContent('#d-tier .tier-name'), 'EXPERIMENTAL');
    assert.deepEqual(errors, []);
  } finally {
    await app.close();
  }
});

test('fake n64rx: a failing build shows the error card with its hint', { skip, timeout: 60000 }, async () => {
  const { app, page } = await launch({ N64RX_FAKE: '1', N64RX_FAKE_STEP_MS: '100', N64RX_FAKE_FAIL: '3', N64RX_FAKE_TIER: 'Gold' });
  try {
    await page.evaluate((r) => window.__n64rxTest.loadRom(r), headerRom());
    await page.waitForSelector('[data-screen="game"]:not([hidden])');
    assert.equal(await page.textContent('#g-tier-name'), 'GOLD');
    assert.match(await page.textContent('#g-problems'), /No known problems/);
    await page.click('#g-build');
    await page.waitForSelector('[data-screen="error"]:not([hidden])', { timeout: 30000 });
    assert.match(await page.textContent('#e-msg'), /failed/);
    assert.ok((await page.textContent('#e-hint')).length > 0);
  } finally {
    await app.close();
  }
});

test('real n64rx info on the test ROM', { skip: skip || !paths.findSystemPython(), timeout: 90000 }, async () => {
  const rom = path.join(tmp(), 'test.z64');
  const py = paths.findSystemPython();
  const r = spawnSync(py[0], [...py.slice(1), path.join(paths.devRoot(), 'tools', 'xbox', 'make_test_rom.py'), rom], { encoding: 'utf8' });
  assert.equal(r.status, 0, r.stderr);
  const { app, page } = await launch({ N64RX_TOOLCHAIN: 'system' });
  try {
    await page.evaluate((p) => window.__n64rxTest.loadRom(p), rom);
    await page.waitForSelector('[data-screen="game"]:not([hidden])', { timeout: 60000 });
    assert.equal(await page.textContent('#g-tier-name'), 'EXPERIMENTAL');
    assert.match(await page.textContent('#g-meta'), /NXTE/);
  } finally {
    await app.close();
  }
});

test('an instant build failure ends on the error screen alone (no late transition over it)', { skip, timeout: 60000 }, async () => {
  const { app, page } = await launch({ N64RX_FAKE: '1' });
  try {
    const rom = headerRom();
    await page.evaluate((r) => window.__n64rxTest.loadRom(r), rom);
    await page.waitForSelector('[data-screen="game"]:not([hidden])');
    fs.rmSync(rom);   // the main process refuses the build at once: the reply beats the 170 ms fade
    await page.click('#g-build');
    await page.waitForSelector('[data-screen="error"]:not([hidden])', { timeout: 10000 });
    await page.waitForTimeout(800);   // longer than any transition
    const visible = await page.evaluate(() => [...document.querySelectorAll('.screen')].filter((s) => !s.hidden).map((s) => s.dataset.screen));
    assert.deepEqual(visible, ['error']);
    assert.match(await page.textContent('#e-msg'), /No ROM/);
  } finally {
    await app.close();
  }
});

test('Send to Xbox with the fake: the password goes by env, Cancel shows while it runs', { skip, timeout: 60000 }, async () => {
  const out = fs.mkdtempSync(path.join(os.tmpdir(), 'n64rx-e2e-'));
  const { app, page } = await launch({ N64RX_FAKE: '1', N64RX_FAKE_STEP_MS: '60', N64RX_TEST_PICK_OUT: out });
  try {
    await page.evaluate((r) => window.__n64rxTest.loadRom(r), headerRom());
    await page.waitForSelector('[data-screen="game"]:not([hidden])');
    await page.click('#g-build');
    await page.waitForSelector('[data-screen="done"]:not([hidden])', { timeout: 30000 });
    await page.click('#d-send');
    await page.fill('#x-host', '192.168.1.50');
    await page.fill('#x-user', '-dash');
    await page.click('#x-go');
    await page.waitForSelector('#d-job:not([hidden])', { timeout: 5000 });
    assert.equal(await page.isDisabled('#d-again'), true);
    await page.waitForSelector('#x-status.ok', { timeout: 15000 });
    assert.equal(await page.isDisabled('#d-again'), false);
    assert.equal(await page.isHidden('#d-job'), true);
  } finally {
    await app.close();
  }
});

test('a second launch on the same data folder quits and leaves the first running', { skip, timeout: 60000 }, async () => {
  const { app, user } = await launch({ N64RX_FAKE: '1' });
  try {
    const t0 = Date.now();
    const r = spawnSync(require('electron'), [APP], { env: { ...process.env, N64RX_FAKE: '1', N64RX_USER_DATA: user, N64RX_CACHE: path.join(user, 'cache') }, timeout: 30000 });
    assert.equal(r.status, 0);
    assert.ok(Date.now() - t0 < 25000);
    const page = await app.firstWindow();
    assert.equal(await page.evaluate(() => document.body.dataset.screen), 'home');
  } finally {
    await app.close();
  }
});

test('Build asks where to save: cancel = no build; an unwritable folder is said, with a way to pick again', { skip, timeout: 60000 }, async () => {
  const blocker = path.join(tmp(), 'a-file');
  fs.writeFileSync(blocker, 'x');
  for (const [pick, expect] of [['cancel', 'none'], [path.join(blocker, 'sub'), 'error']]) {
    const { app, page } = await launch({ N64RX_FAKE: '1', N64RX_TEST_PICK_OUT: pick });
    try {
      await page.evaluate((r) => window.__n64rxTest.loadRom(r), headerRom());
      await page.waitForSelector('[data-screen="game"]:not([hidden])');
      assert.match(await page.textContent('#g-saves'), /choose where/i);
      await page.click('#g-build');
      await page.waitForTimeout(700);
      const visible = await page.evaluate(() => [...document.querySelectorAll('.screen')].filter((s) => !s.hidden).map((s) => s.dataset.screen));
      assert.deepEqual(visible, ['game']);
      assert.equal(await page.isHidden('#g-out-error'), expect === 'none');
      if (expect === 'error') assert.match(await page.textContent('#g-out-error'), /cannot write/);
      // a build without a folder from the picker is refused by main
      const r = await page.evaluate(() => window.n64rx.build({ rom: window.__n64rxTest.state.rom, out: 'C:/Windows', video: 'auto' }));
      assert.match(r.error.message, /Choose where/);
    } finally {
      await app.close();
    }
  }
});

test('Disc image (ISO): the save dialog, six stars, Done shows the .iso and no Send / Make ISO', { skip, timeout: 60000 }, async () => {
  const iso = path.join(tmp(), 'N64RECOMP-X TEST.iso');
  const { app, page } = await launch({ N64RX_FAKE: '1', N64RX_FAKE_STEP_MS: '200', N64RX_TEST_PICK_ISO: iso });
  try {
    await page.evaluate((r) => window.__n64rxTest.loadRom(r), headerRom());
    await page.waitForSelector('[data-screen="game"]:not([hidden])');
    await page.click('#g-output [data-v="iso"]');
    assert.equal(await page.getAttribute('#g-output [data-v="iso"]', 'aria-checked'), 'true');
    await page.click('#g-build');
    await page.waitForFunction(() => document.querySelectorAll('#b-const .c-star').length === 6 && window.__n64rxTest.state.step >= 6, null, { timeout: 30000 });
    await page.waitForSelector('[data-screen="done"]:not([hidden])', { timeout: 30000 });
    assert.equal(await page.getAttribute('#d-folder', 'title'), iso);
    assert.ok(fs.existsSync(iso));
    assert.equal(await page.isHidden('#d-send'), true);
    assert.equal(await page.$('#d-iso'), null, 'Make ISO is chosen before the build, not offered after it');
    assert.equal(await page.isVisible('#d-hint-iso'), true);
    assert.equal(await page.textContent('#d-show'), 'Show file');
    const s = await page.evaluate(() => window.n64rx.settings.get());
    assert.equal(s.output, 'iso');
  } finally {
    await app.close();
  }
});
