'use strict';
// The packaged full build, end to end, the way a user gets it: unzip, start
// the exe, drop the ROM-free test ROM (tools/xbox/make_test_rom.py, never a
// game), press Build. Nothing from this machine's own toolchain: PATH is the
// OS's folders only, the test ROM is made by the app's own Python, and the
// build log must name no system toolchain path. Only the folder picker is
// answered from the test (patched in the main process; the dev-only
// N64RX_TEST_PICK_OUT hook is off in a packaged app).
//   N64RX_PACKAGED_EXE="D:\some folder\N64Recomp-X-0.1.0-win-x64\N64Recomp-X.exe" npm run test:packaged
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const EXE = process.env.N64RX_PACKAGED_EXE ? path.resolve(process.env.N64RX_PACKAGED_EXE) : null;
const skip = !EXE ? 'set N64RX_PACKAGED_EXE to an unpacked full build\'s exe' : false;

/** the environment of a machine with none of the build tools: the OS's own folders on PATH */
function bareEnv() {
  const keep = ['SystemRoot', 'windir', 'SystemDrive', 'TEMP', 'TMP', 'USERPROFILE', 'LOCALAPPDATA', 'APPDATA', 'USERNAME',
    'HOMEDRIVE', 'HOMEPATH', 'ComSpec', 'PATHEXT', 'NUMBER_OF_PROCESSORS', 'PROCESSOR_ARCHITECTURE', 'HOME', 'USER', 'LANG', 'TMPDIR', 'DISPLAY'];
  const env = {};
  for (const [k, v] of Object.entries(process.env)) if (keep.some((n) => n.toLowerCase() === k.toLowerCase())) env[k] = v;
  if (process.platform === 'win32') {
    const sys = process.env.SystemRoot || 'C:\\Windows';
    env.PATH = [path.join(sys, 'System32'), sys].join(';');
  } else env.PATH = '/usr/bin:/bin';
  return env;
}

function walk(d, out = []) {
  for (const e of fs.readdirSync(d, { withFileTypes: true })) {
    const p = path.join(d, e.name);
    if (e.isDirectory()) walk(p, out);
    else out.push(p);
  }
  return out;
}

test('the full build builds the test ROM with only what is inside it', { skip, timeout: 15 * 60 * 1000 }, async () => {
  const home = path.dirname(EXE);
  const resources = process.platform === 'darwin' ? path.resolve(home, '..', 'Resources') : path.join(home, 'resources');
  const tc = path.join(resources, 'toolchain');
  const mf = JSON.parse(fs.readFileSync(path.join(tc, 'manifest.json'), 'utf8'));
  const py = path.join(tc, mf.python);

  // the test ROM, made by the app's own Python from the app's own source copy
  const work = fs.mkdtempSync(path.join(os.tmpdir(), 'n64rx packaged '));
  const rom = path.join(work, 'test.z64');
  const r = spawnSync(py, ['-I', path.join(resources, 'src', 'tools', 'xbox', 'make_test_rom.py'), rom], { encoding: 'utf8', env: bareEnv() });
  assert.equal(r.status, 0, r.stderr);
  const out = path.join(work, 'Xbox games');
  fs.mkdirSync(out);

  const { _electron: electron } = require('playwright-core');
  const t0 = Date.now();
  const app = await electron.launch({ executablePath: EXE, cwd: home, env: bareEnv() });
  try {
    await app.evaluate(({ dialog }, dir) => {
      dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [dir] });
    }, out);
    const page = await app.firstWindow();
    const errors = [];
    page.on('pageerror', (e) => errors.push(String(e)));
    await page.waitForSelector('#intro.gone', { state: 'attached', timeout: 60000 });
    // straight to home: no "Get the build tools" step
    await page.waitForSelector('[data-screen="home"]:not([hidden])', { timeout: 60000 });
    assert.equal(await page.isHidden('[data-screen="tools"]'), true);
    const st = await page.evaluate(() => window.n64rx.toolchain.status());
    assert.equal(st.state, 'ready');
    assert.equal(st.shipped, true);
    const about = await page.evaluate(() => window.n64rx.about());
    assert.equal(path.resolve(about.tools), path.resolve(tc));
    console.log(`# data folder ${about.data}; first window in ${((Date.now() - t0) / 1000).toFixed(1)} s`);

    await page.evaluate((p) => window.__n64rxTest.loadRom(p), rom);
    await page.waitForSelector('[data-screen="game"]:not([hidden])', { timeout: 120000 });
    assert.match(await page.textContent('#g-meta'), /NXTE/);
    const t1 = Date.now();
    await page.click('#g-build');
    await page.waitForSelector('[data-screen="build"]:not([hidden])', { timeout: 30000 });
    await page.waitForSelector('[data-screen="done"]:not([hidden]), [data-screen="error"]:not([hidden])', { timeout: 12 * 60 * 1000 });
    const failed = await page.isVisible('[data-screen="error"]');
    if (failed) assert.fail(`build failed: ${await page.textContent('#e-msg')} / ${await page.textContent('#e-hint')}`);
    const folder = await page.getAttribute('#d-folder', 'title');
    const xbe = path.join(folder, 'default.xbe');
    assert.ok(fs.existsSync(xbe), folder);
    assert.ok(folder.startsWith(out), folder);
    console.log(`# built ${xbe} (${fs.statSync(xbe).size} bytes) in ${((Date.now() - t1) / 1000).toFixed(0)} s`);

    // the build ran on the shipped toolchain alone
    const logs = walk(path.join(about.data, 'builds')).filter((f) => path.basename(f) === 'n64rx.log');
    assert.ok(logs.length, 'no n64rx.log in the data folder');
    for (const f of logs) {
      const text = fs.readFileSync(f, 'utf8').toLowerCase().replace(/\\/g, '/');
      for (const sysPath of ['c:/msys64', '/c/msys64', 'c:/xdev', '/c/xdev', '/opt/nxdk', '/opt/llvm21']) assert.ok(!text.includes(sysPath), `${f} names ${sysPath}`);
    }
    assert.deepEqual(errors, []);
  } finally {
    await app.close();
  }
});
