'use strict';
// Where things are: the n64rx source tree, Python, the cache, the toolchain
// bundle, and the environment n64rx runs with. Pure functions of their
// inputs (no Electron import), so the tests can call them directly.
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const APP_DIR = path.resolve(__dirname, '..');
const IS_WIN = process.platform === 'win32';

/** the cache n64rx itself uses (pipeline.cache_dir()): %LOCALAPPDATA%\n64rx, ~/.n64rx */
function cacheDir(env = process.env, platform = process.platform, home = os.homedir()) {
  if (env.N64RX_CACHE) return path.resolve(env.N64RX_CACHE);
  if (platform === 'win32' && env.LOCALAPPDATA) return path.join(env.LOCALAPPDATA, 'n64rx');
  return path.join(home, '.n64rx');
}

const DATA_NAME = 'N64Recomp-X Data';

/** where the app itself lives: the portable exe's folder, the folder holding the .app, the AppImage's folder */
function appHome({ env = process.env, platform = process.platform, execPath = process.execPath } = {}) {
  if (platform === 'win32') return env.PORTABLE_EXECUTABLE_DIR || path.dirname(execPath);
  if (platform === 'darwin') {
    const m = /^(.*?\.app)(\/|$)/.exec(execPath);   // .../N64Recomp-X.app/Contents/MacOS/N64Recomp-X
    return path.dirname(m ? m[1] : execPath);
  }
  return env.APPIMAGE ? path.dirname(env.APPIMAGE) : path.dirname(execPath);
}

function canWrite(dir) {
  if (/[\\/]AppTranslocation[\\/]/.test(dir)) return false;   // macOS runs a quarantined app from a read-only copy
  try {
    fs.mkdirSync(dir, { recursive: true });
    const probe = path.join(dir, `.write-test-${process.pid}`);
    fs.writeFileSync(probe, '');
    fs.rmSync(probe, { force: true });
    return true;
  } catch {
    return false;
  }
}

/**
 * Everything the app writes (build tools, the source copy, builds, settings,
 * logs): {dir, portable, reason}. Packaged: "N64Recomp-X Data" next to the
 * app, so deleting one folder removes it all; where that cannot be written
 * (a read-only folder, Program Files, macOS App Translocation) the user
 * cache n64rx itself uses. Dev mode: the user cache. N64RX_HOME overrides.
 */
function dataDir({ packaged, env = process.env, platform = process.platform, execPath = process.execPath, home = os.homedir(), writable = canWrite } = {}) {
  const override = env.N64RX_HOME || env.N64RX_CACHE;
  if (override) return { dir: path.resolve(override), portable: false, reason: 'N64RX_HOME' };
  const fallback = cacheDir({ ...env, N64RX_CACHE: '' }, platform, home);
  if (!packaged) return { dir: fallback, portable: false, reason: 'dev' };
  const want = path.join(appHome({ env, platform, execPath }), DATA_NAME);
  if (writable(want)) return { dir: want, portable: true, reason: null };
  return { dir: fallback, portable: false, reason: `The folder next to the app (${path.dirname(want)}) cannot be written to.` };
}

/** "win32-x64", "darwin-arm64", "linux-x64": the key into toolchain.json */
function platformKey(platform = process.platform, arch = process.arch) {
  return `${platform}-${arch}`;
}

/** the checkout the app runs against in dev mode */
function devRoot() {
  return path.resolve(APP_DIR, '..');
}

/** the source set copied out of the app's resources on first run */
function packagedSrcDir(cache, version) {
  return path.join(cache, 'src', version);
}

function n64rxScript(root) {
  return path.join(root, 'tools', 'n64rx', 'n64rx.py');
}

function fakeScript() {
  return path.join(APP_DIR, 'test', 'fake-n64rx.js');
}

let pythonCache = null;

/** every absolute file called name on PATH (never the working directory); on Windows only .exe/.com */
function whichAll(name, env = process.env, platform = process.platform) {
  const key = Object.keys(env).find((k) => k.toUpperCase() === 'PATH');
  const dirs = String((key && env[key]) || '').split(platform === 'win32' ? ';' : ':');
  const exts = platform === 'win32' ? ['.exe', '.com'] : [''];
  const out = [];
  for (const d of dirs) {
    const dir = d.trim().replace(/^"(.*)"$/, '$1');
    if (!dir || dir === '.' || !path.isAbsolute(dir)) continue;
    for (const e of exts) {
      const f = path.join(dir, name + e);
      try {
        if (fs.statSync(f).isFile() && !out.includes(f)) out.push(f);
      } catch { /* not here */ }
    }
  }
  return out;
}

/** [absolute command, ...prefix args] of a Python 3.11+ on this machine, or null */
function findSystemPython(env = process.env, platform = process.platform, probe = probePython, which = whichAll) {
  if (env.N64RX_PYTHON) return [env.N64RX_PYTHON];
  const cached = probe === probePython && which === whichAll;
  if (pythonCache && cached) return pythonCache;
  const candidates = platform === 'win32' ? [['py', '-3'], ['python'], ['python3']] : [['python3'], ['python']];
  for (const [name, ...pre] of candidates) {
    for (const exe of which(name, env, platform)) {   // every match: the Microsoft Store stub fails its probe
      const c = [exe, ...pre];
      if (probe(c)) {
        if (cached) pythonCache = c;
        return c;
      }
    }
  }
  return null;
}

function probePython(cmd) {
  try {
    const r = spawnSync(cmd[0], [...cmd.slice(1), '-c', 'import sys; print(sys.version_info[0], sys.version_info[1])'],
      { encoding: 'utf8', timeout: 15000, windowsHide: true, cwd: os.tmpdir() });
    if (r.status !== 0 || !r.stdout) return false;
    const [maj, min] = r.stdout.trim().split(/\s+/).map(Number);
    return maj === 3 && min >= 11;
  } catch {
    return false;
  }
}

/** the unpacked toolchain bundle's manifest, resolved: {dir, key, python, env, path} or null */
function readBundle(dir) {
  const mf = path.join(dir, 'manifest.json');
  let m;
  try {
    m = JSON.parse(fs.readFileSync(mf, 'utf8'));
  } catch {
    return null;
  }
  const abs = (p) => path.resolve(dir, p);
  const env = {};
  for (const [k, v] of Object.entries(m.env || {})) env[k] = abs(String(v));
  return {
    dir,
    key: m.key || path.basename(dir),
    os: m.os,
    arch: m.arch,
    python: m.python ? abs(m.python) : null,
    env,
    path: (m.path || []).map((p) => abs(String(p))),
    recompiler: m.recompiler ? String(m.recompiler) : null,   // the recompiler's key (engines.recompiler_pin())
  };
}

/** a bundle that can run: its manifest reads and its Python is there */
function usableBundle(dir) {
  const b = readBundle(dir);
  return b && (!b.python || fs.existsSync(b.python)) ? b : null;
}

/**
 * The toolchain shipped inside the app (the full build: resources/toolchain,
 * already unpacked), or N64RX_TOOLCHAIN_DIR (any unpacked bundle, for tests and
 * dev). Null when there is none: then installedBundle() and the download.
 */
function shippedBundle({ resources = null, env = process.env } = {}) {
  const dirs = [];
  if (env.N64RX_TOOLCHAIN_DIR) dirs.push(path.resolve(env.N64RX_TOOLCHAIN_DIR));
  if (resources) dirs.push(path.join(resources, 'toolchain'));
  for (const d of dirs) {
    const b = usableBundle(d);
    if (b) return { ...b, shipped: true };
  }
  return null;
}

/** the installed bundle: <cache>/toolchain/current.json names it, else the pinned key's folder */
function installedBundle(cache, pinnedKey) {
  const tdir = path.join(cache, 'toolchain');
  let cur = null;
  try {
    cur = JSON.parse(fs.readFileSync(path.join(tdir, 'current.json'), 'utf8'));
  } catch { /* none yet */ }
  const keys = [];
  if (pinnedKey) keys.push(pinnedKey);
  if (cur && cur.key && !keys.includes(cur.key)) keys.push(cur.key);
  for (const k of keys) {
    const b = usableBundle(path.join(tdir, k));
    if (b) return b;
  }
  return null;
}

/** which toolchain runs, in order: the shipped one, an unpacked download, else null (the download, or the system's) */
function pickBundle({ shipped, cache, pinnedKey }) {
  return shipped || installedBundle(cache, pinnedKey);
}

/**
 * How n64rx is run: {cmd, args (before the n64rx arguments), env, cwd, root, mode}.
 *   mode 'fake'   - N64RX_FAKE=1: the JS stand-in (test/fake-n64rx.js) run by this Node/Electron
 *   mode 'bundle' - the toolchain bundle's Python and environment (shipped in the app or downloaded)
 *   mode 'system' - the machine's Python; n64rx finds its own toolchain (engines.pick('auto'))
 */
function runner({ root, cache, bundle, env = process.env, packaged = false, execPath = process.execPath }) {
  const base = { ...env, PYTHONUTF8: '1', PYTHONUNBUFFERED: '1', PYTHONIOENCODING: 'utf-8' };
  // n64rx's own cache (pipeline.cache_dir(): last.json, logs) in the same data folder
  if (packaged || bundle || env.N64RX_HOME || env.N64RX_CACHE) base.N64RX_HOME = cache;
  if (env.N64RX_FAKE === '1') {
    return { mode: 'fake', cmd: execPath, args: [fakeScript()], env: { ...base, ELECTRON_RUN_AS_NODE: '1' }, cwd: cache, root };
  }
  const script = n64rxScript(root);
  if (bundle && env.N64RX_TOOLCHAIN !== 'system') {
    // the bundle's Python only: no PYTHONHOME, PYTHONPATH, PYTHONSTARTUP... from the user's environment
    const ours = new Set(['PYTHONUTF8', 'PYTHONUNBUFFERED', 'PYTHONIOENCODING']);
    const clean = Object.fromEntries(Object.entries(base).filter(([k]) => !/^PYTHON/i.test(k) || ours.has(k)));
    const e = { ...clean, ...bundle.env, N64RX_BUILD_ROOT: path.join(cache, 'builds') };
    // the source copy has no tools/recomp/.git: the bundle's recompiler key stands in for the pin
    if (bundle.recompiler) e.N64RX_RECOMPILER_KEY = bundle.recompiler;
    if (bundle.path.length) {
      const key = Object.keys(e).find((k) => k.toUpperCase() === 'PATH') || 'PATH';
      e[key] = [...bundle.path, e[key] || ''].join(path.delimiter);
    }
    const py = bundle.python || (findSystemPython(env) || [])[0];
    return { mode: 'bundle', cmd: py, args: [script], env: e, cwd: cache, root, engine: 'native' };
  }
  const py = findSystemPython(env);
  const e = { ...base };
  if (packaged) e.N64RX_BUILD_ROOT = path.join(cache, 'builds');
  if (!py) return { mode: 'system', cmd: null, args: [script], env: e, cwd: cache, root };
  return { mode: 'system', cmd: py[0], args: [...py.slice(1), script], env: e, cwd: cache, root };
}

/** the default output folder: Documents/N64Recomp-X, else ~/N64Recomp-X */
function defaultOutDir(documents, home = os.homedir()) {
  return documents ? path.join(documents, 'N64Recomp-X') : path.join(home, 'N64Recomp-X');
}

module.exports = {
  APP_DIR, IS_WIN, DATA_NAME, appHome, canWrite, dataDir, cacheDir, platformKey, devRoot, packagedSrcDir, n64rxScript, fakeScript,
  whichAll, findSystemPython, probePython, readBundle, shippedBundle, installedBundle, pickBundle, runner, defaultOutDir,
};
