'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const paths = require('../src/paths');

const tmp = () => fs.mkdtempSync(path.join(os.tmpdir(), 'n64rx-paths-'));

test('the cache is n64rx\'s own: %LOCALAPPDATA%\\n64rx on Windows, ~/.n64rx elsewhere', () => {
  assert.equal(paths.cacheDir({ LOCALAPPDATA: 'C:\\Users\\u\\AppData\\Local' }, 'win32', '/home/u'), path.join('C:\\Users\\u\\AppData\\Local', 'n64rx'));
  assert.equal(paths.cacheDir({}, 'linux', '/home/u'), path.join('/home/u', '.n64rx'));
  assert.equal(paths.cacheDir({}, 'darwin', '/Users/u'), path.join('/Users/u', '.n64rx'));
  assert.equal(paths.cacheDir({ N64RX_CACHE: tmp() }, 'linux', '/home/u').length > 0, true);
});

test('dev mode runs against the checkout above app/', () => {
  const root = paths.devRoot();
  assert.ok(fs.existsSync(path.join(root, 'tools', 'n64rx', 'n64rx.py')), root);
  assert.equal(paths.n64rxScript(root), path.join(root, 'tools', 'n64rx', 'n64rx.py'));
});

test('platform keys match toolchain.json', () => {
  const table = JSON.parse(fs.readFileSync(path.join(paths.APP_DIR, 'toolchain.json'), 'utf8'));
  for (const k of ['win32-x64', 'darwin-arm64', 'darwin-x64', 'linux-x64']) assert.ok(k in table, k);
  assert.equal(paths.platformKey('win32', 'x64'), 'win32-x64');
});

test('N64RX_PYTHON wins; otherwise the first Python 3.11+ that answers, by absolute path', () => {
  assert.deepEqual(paths.findSystemPython({ N64RX_PYTHON: '/opt/py/bin/python3' }, 'linux'), ['/opt/py/bin/python3']);
  const seen = [];
  const which = (n) => (n === 'py' ? [] : [`C:/Stub/${n}.exe`, `C:/Py312/${n}.exe`]);
  const probe = (c) => {
    seen.push(c.join(' '));
    return c[0] === 'C:/Py312/python.exe';
  };
  assert.deepEqual(paths.findSystemPython({}, 'win32', probe, which), ['C:/Py312/python.exe']);
  assert.deepEqual(seen, ['C:/Stub/python.exe', 'C:/Py312/python.exe']);
  assert.equal(paths.findSystemPython({}, 'linux', () => false, () => ['/usr/bin/python3']), null);
});

test('PATH lookup skips the working directory and relative entries', () => {
  const d = tmp();
  const exe = process.platform === 'win32' ? 'tool.exe' : 'tool';
  fs.writeFileSync(path.join(d, exe), '');
  const sep = process.platform === 'win32' ? ';' : ':';
  assert.deepEqual(paths.whichAll('tool', { PATH: ['.', 'rel/dir', '', d].join(sep) }), [path.join(d, exe)]);
  if (process.platform === 'win32') {
    fs.writeFileSync(path.join(d, 'other.cmd'), '');
    assert.deepEqual(paths.whichAll('other', { Path: d }), []);   // no .cmd/.bat: they need a shell
  }
});

test('bundle mode drops the PYTHON* variables of the user environment', () => {
  const cache = tmp();
  const dir = path.join(cache, 'toolchain', 'k');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(path.join(dir, 'manifest.json'), JSON.stringify({ key: 'k', env: {}, path: [] }));
  const run = paths.runner({ root: '/src', cache, bundle: paths.readBundle(dir), env: { PYTHONHOME: 'C:/py', PYTHONPATH: 'x', pythonstartup: 'y', PATH: '/bin', N64RX_PYTHON: 'p' } });
  assert.equal(run.env.PYTHONHOME, undefined);
  assert.equal(run.env.PYTHONPATH, undefined);
  assert.equal(run.env.pythonstartup, undefined);
  assert.equal(run.env.PYTHONUTF8, '1');
  assert.equal(run.env.PATH, '/bin');
  const sys = paths.runner({ root: '/src', cache, bundle: null, env: { PYTHONPATH: 'x', N64RX_PYTHON: 'p' } });
  assert.equal(sys.env.PYTHONPATH, 'x');   // system mode: the user's own Python setup
});

function makeBundle(cache, key) {
  const dir = path.join(cache, 'toolchain', key);
  fs.mkdirSync(path.join(dir, 'python'), { recursive: true });
  const exe = path.join(dir, 'python', process.platform === 'win32' ? 'python.exe' : 'python3');
  fs.writeFileSync(exe, '');
  fs.writeFileSync(path.join(dir, 'manifest.json'), JSON.stringify({
    key, os: process.platform, arch: process.arch, python: path.relative(dir, exe),
    env: { MSYS2_ROOT: 'msys64', NXDK_DIR: 'nxdk', LLVM: 'llvm', N64RX_RECOMP: 'recomp' }, path: ['bin'],
  }));
  return { dir, exe };
}

test('a bundle\'s manifest resolves to absolute paths, and the runner uses its python and env', () => {
  const cache = tmp();
  const { dir, exe } = makeBundle(cache, 'llvm21-nxdk1234-recompabcd');
  const b = paths.installedBundle(cache, 'llvm21-nxdk1234-recompabcd');
  assert.ok(b);
  assert.equal(b.python, exe);
  assert.equal(b.env.NXDK_DIR, path.join(dir, 'nxdk'));
  assert.equal(b.env.N64RX_RECOMP, path.join(dir, 'recomp'));
  const run = paths.runner({ root: '/src', cache, bundle: b, env: { PATH: '/usr/bin' } });
  assert.equal(run.mode, 'bundle');
  assert.equal(run.cmd, exe);
  assert.equal(run.engine, 'native');
  assert.deepEqual(run.args, [path.join('/src', 'tools', 'n64rx', 'n64rx.py')]);
  assert.equal(run.env.N64RX_BUILD_ROOT, path.join(cache, 'builds'));
  assert.equal(run.env.MSYS2_ROOT, path.join(dir, 'msys64'));
  assert.ok(run.env.PATH.startsWith(path.join(dir, 'bin') + path.delimiter));
  assert.equal(run.env.PYTHONUTF8, '1');
});

test('current.json finds a bundle with another key (a N64RX_TOOLCHAIN_URL install)', () => {
  const cache = tmp();
  makeBundle(cache, 'local-test');
  fs.writeFileSync(path.join(cache, 'toolchain', 'current.json'), JSON.stringify({ key: 'local-test' }));
  assert.equal(paths.installedBundle(cache, 'pinned-key').key, 'local-test');
  assert.equal(paths.installedBundle(tmp(), 'pinned-key'), null);
});

test('N64RX_TOOLCHAIN=system ignores the bundle; N64RX_FAKE=1 runs the stand-in', () => {
  const cache = tmp();
  makeBundle(cache, 'k');
  const b = paths.installedBundle(cache, 'k');
  const sys = paths.runner({ root: '/src', cache, bundle: b, env: { N64RX_TOOLCHAIN: 'system', N64RX_PYTHON: 'mypython' } });
  assert.equal(sys.mode, 'system');
  assert.equal(sys.cmd, 'mypython');
  assert.equal(sys.env.N64RX_BUILD_ROOT, undefined);
  const packaged = paths.runner({ root: '/src', cache, bundle: null, env: { N64RX_PYTHON: 'p' }, packaged: true });
  assert.equal(packaged.env.N64RX_BUILD_ROOT, path.join(cache, 'builds'));
  const fake = paths.runner({ root: '/src', cache, bundle: b, env: { N64RX_FAKE: '1' }, execPath: '/node' });
  assert.equal(fake.mode, 'fake');
  assert.equal(fake.cmd, '/node');
  assert.equal(fake.args[0], paths.fakeScript());
  assert.equal(fake.env.ELECTRON_RUN_AS_NODE, '1');
});

test('the default output folder is Documents/N64Recomp-X, else ~/N64Recomp-X', () => {
  assert.equal(paths.defaultOutDir('/Users/u/Documents', '/Users/u'), path.join('/Users/u/Documents', 'N64Recomp-X'));
  assert.equal(paths.defaultOutDir(null, '/home/u'), path.join('/home/u', 'N64Recomp-X'));
});

test('packaged: the data folder sits next to the app when that folder can be written', () => {
  const home = tmp();
  const exeDir = path.join(home, 'My Apps');   // a space, as users have
  fs.mkdirSync(exeDir);
  const d = paths.dataDir({ packaged: true, env: {}, platform: 'win32', execPath: path.join(exeDir, 'N64Recomp-X.exe'), home });
  assert.equal(d.dir, path.join(exeDir, 'N64Recomp-X Data'));
  assert.equal(d.portable, true);
  assert.ok(fs.existsSync(d.dir));
  // the portable exe runs from a temp copy: PORTABLE_EXECUTABLE_DIR names where the user put it
  const p = paths.dataDir({ packaged: true, env: { PORTABLE_EXECUTABLE_DIR: exeDir }, platform: 'win32', execPath: 'C:/Temp/2abc/N64Recomp-X.exe', home, writable: () => true });
  assert.equal(p.dir, path.join(exeDir, 'N64Recomp-X Data'));
});

test('packaged: an unwritable app folder falls back to the user cache, with the reason', () => {
  const home = tmp();
  const d = paths.dataDir({ packaged: true, env: {}, platform: 'linux', execPath: '/opt/x/n64recomp-x', home, writable: () => false });
  assert.equal(d.dir, path.join(home, '.n64rx'));
  assert.equal(d.portable, false);
  assert.match(d.reason, /cannot be written/);
  const w = paths.dataDir({ packaged: true, env: { LOCALAPPDATA: 'C:/Users/u/AppData/Local' }, platform: 'win32', execPath: 'C:/Program Files/N64Recomp-X/N64Recomp-X.exe', home, writable: () => false });
  assert.equal(w.dir, path.join('C:/Users/u/AppData/Local', 'n64rx'));
});

test('macOS: next to the .app bundle, never inside it; App Translocation is read-only', () => {
  const where = paths.appHome({ platform: 'darwin', execPath: '/Users/u/Games/N64Recomp-X.app/Contents/MacOS/N64Recomp-X' });
  assert.equal(where, path.dirname('/Users/u/Games/N64Recomp-X.app'));
  assert.equal(paths.canWrite('/private/var/folders/xy/AppTranslocation/ABC/d/N64Recomp-X Data'), false);
  const d = paths.dataDir({ packaged: true, env: {}, platform: 'darwin', execPath: '/private/var/folders/xy/AppTranslocation/ABC/d/N64Recomp-X.app/Contents/MacOS/N64Recomp-X', home: '/Users/u' });
  assert.equal(d.dir, path.join('/Users/u', '.n64rx'));
});

test('Linux AppImage: next to the .AppImage file, not its read-only mount', () => {
  assert.equal(paths.appHome({ platform: 'linux', env: { APPIMAGE: '/home/u/Apps/N64Recomp-X.AppImage' }, execPath: '/tmp/.mount_N64abc/n64recomp-x' }), path.dirname('/home/u/Apps/N64Recomp-X.AppImage'));
});

test('dev mode and N64RX_HOME', () => {
  const home = tmp();
  assert.equal(paths.dataDir({ packaged: false, env: {}, platform: 'linux', home }).dir, path.join(home, '.n64rx'));
  const o = tmp();
  assert.equal(paths.dataDir({ packaged: true, env: { N64RX_HOME: o }, platform: 'linux', home }).dir, path.resolve(o));
});

test('the runner hands n64rx the data folder (N64RX_HOME) and the bundle\'s recompiler key', () => {
  const cache = tmp();
  const dir = path.join(cache, 'toolchain', 'k');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(path.join(dir, 'manifest.json'), JSON.stringify({ key: 'k', recompiler: 'ffb39cd+abc', env: {}, path: [] }));
  const b = paths.readBundle(dir);
  const run = paths.runner({ root: '/src', cache, bundle: b, env: { N64RX_PYTHON: 'py' } });
  assert.equal(run.env.N64RX_HOME, cache);
  assert.equal(run.env.N64RX_RECOMPILER_KEY, 'ffb39cd+abc');
  const pk = paths.runner({ root: '/src', cache, bundle: null, env: { N64RX_PYTHON: 'py' }, packaged: true });
  assert.equal(pk.env.N64RX_HOME, cache);
});
