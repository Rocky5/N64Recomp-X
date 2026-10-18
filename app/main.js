'use strict';
// N64Recomp-X, the app: one window over n64rx (tools/n64rx). The main
// process owns everything that touches the disk, the network or a process;
// the renderer asks through preload.js and every argument is checked here.
const { app, BrowserWindow, ipcMain, dialog, shell, protocol, net, clipboard, session, nativeTheme, safeStorage } = require('electron');
const fs = require('node:fs');
const fsp = require('node:fs/promises');
const path = require('node:path');
const { pathToFileURL } = require('node:url');

const paths = require('./src/paths');
const n64rx = require('./src/n64rx');
const toolchain = require('./src/toolchain');
const V = require('./src/validate');
const { Jobs } = require('./src/jobs');
const { createSettings } = require('./src/settings');

const REPO_URL = 'https://github.com/TechProGabe/N64Recomp-X';
const RENDERER = path.join(__dirname, 'renderer');
const CSP = "default-src 'none'; script-src 'self'; style-src 'self'; font-src 'self'; img-src 'self' data:; connect-src 'none'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'";
const IS_MAC = process.platform === 'darwin';
const IS_WIN = process.platform === 'win32';

protocol.registerSchemesAsPrivileged([{ scheme: 'app', privileges: { standard: true, secure: true, supportFetchAPI: false } }]);
// every byte the app writes goes to one data folder ("N64Recomp-X Data" next to a
// packaged app; paths.dataDir), Electron's own userData included, before ready
const DATA = paths.dataDir({ packaged: app.isPackaged });
if (process.env.N64RX_USER_DATA) app.setPath('userData', path.resolve(process.env.N64RX_USER_DATA));
else if (app.isPackaged) {
  app.setPath('userData', path.join(DATA.dir, 'electron'));
  app.setPath('sessionData', path.join(DATA.dir, 'electron'));
}
// one app at a time on a data folder: a second launch brings the first to the front
const PRIMARY = app.requestSingleInstanceLock();
if (!PRIMARY) app.quit();

let win = null;
const jobs = new Jobs();  // one n64rx job or the toolchain download at a time (src/jobs.js)
let lastLog = null;      // the last job's log file
const pickedOut = new Set();   // output folders the picker returned this session
const pickedIso = new Set();   // ISO files the save dialog returned this session
let srcReady = null;     // Promise of the source tree's root

const cache = () => DATA.dir;
const logDir = () => path.join(app.getPath('userData'), 'logs');

// ---------------------------------------------------------------- settings
let store = null;
function settingsStore() {
  if (!store) {
    store = createSettings({
      file: path.join(app.getPath('userData'), 'settings.json'),
      crypt: {
        available: () => safeStorage.isEncryptionAvailable(),
        encrypt: (t) => safeStorage.encryptString(t),
        decrypt: (b) => safeStorage.decryptString(b),
      },
    });
  }
  return store;
}
const loadSettings = () => settingsStore().get();
const setSettings = (patch) => settingsStore().set(patch);
function safeDocuments() {
  try {
    return app.getPath('documents');
  } catch {
    return null;
  }
}

// ---------------------------------------------------------------- the source tree
/** dev: this checkout. Packaged: resources/src copied to <cache>/src/<version> (writable) */
async function prepareSource() {
  if (process.env.N64RX_ROOT) return path.resolve(process.env.N64RX_ROOT);
  if (!app.isPackaged) return paths.devRoot();
  const from = path.join(process.resourcesPath, 'src');
  let stamp = '';
  try {
    stamp = await fsp.readFile(path.join(from, 'stage.json'), 'utf8');
  } catch { /* an unstaged build */ }
  const dest = paths.packagedSrcDir(cache(), app.getVersion());
  try {
    if (stamp && (await fsp.readFile(path.join(dest, 'stage.json'), 'utf8')) === stamp) return dest;
  } catch { /* not copied yet */ }
  const tmp = `${dest}.tmp-${process.pid}`;
  await fsp.rm(tmp, { recursive: true, force: true });
  await fsp.mkdir(path.dirname(dest), { recursive: true });
  await fsp.cp(from, tmp, { recursive: true });
  await fsp.rm(dest, { recursive: true, force: true });
  await fsp.rename(tmp, dest);
  return dest;
}

function pinnedEntry() {
  return toolchain.pinned(paths.platformKey(), { manifest: path.join(__dirname, 'toolchain.json') });
}

function useSystem() {
  return process.env.N64RX_TOOLCHAIN === 'system' || loadSettings().toolchain === 'system';
}

async function currentRunner() {
  const root = await srcReady;
  const entry = pinnedEntry();
  const bundle = useSystem() ? null : paths.installedBundle(cache(), entry.key);
  return paths.runner({ root, cache: cache(), bundle, packaged: app.isPackaged });
}

function toolchainStatus() {
  const entry = pinnedEntry();
  if (process.env.N64RX_FAKE_TOOLCHAIN === 'missing') {
    return { state: 'missing', size: entry.size || 680 * 1024 * 1024, available: true, platform: entry.platform };
  }
  if (process.env.N64RX_FAKE === '1') return { state: 'ready', mode: 'fake' };
  if (useSystem()) return { state: 'ready', mode: 'system' };
  const bundle = paths.installedBundle(cache(), entry.key);
  if (bundle) {
    const outdated = !!(entry.key && entry.url && bundle.key !== entry.key);
    return { state: outdated ? 'missing' : 'ready', mode: 'bundle', key: bundle.key, size: entry.size, available: !!entry.url, update: outdated };
  }
  if (!app.isPackaged && !process.env.N64RX_TOOLCHAIN_URL) return { state: 'ready', mode: 'system' };
  return { state: 'missing', size: entry.size, available: !!entry.url, platform: entry.platform };
}

// ---------------------------------------------------------------- jobs
function send(channel, payload) {
  if (win && !win.isDestroyed()) win.webContents.send(channel, payload);
}

function logFileFor(kind) {
  const stamp = new Date().toISOString().replace(/[:.]/g, '-');
  return path.join(logDir(), `${stamp}-${kind}.log`);
}

async function pruneLogs() {
  try {
    const files = (await fsp.readdir(logDir())).filter((f) => f.endsWith('.log')).sort();
    for (const f of files.slice(0, Math.max(0, files.length - 30))) await fsp.rm(path.join(logDir(), f), { force: true });
  } catch { /* none */ }
}

async function startJob(kind, sub, args, { env = null } = {}) {
  const id = `${kind}-${Date.now()}`;
  const logFile = logFileFor(kind);
  try {
    const r = await jobs.run(kind, currentRunner, (run) => {
      lastLog = logFile;
      const a = kind === 'build' && run.engine === 'native' ? [...args, '--engine', 'native'] : args;
      const h = n64rx.runEvents(run, sub, a, { logFile, env, onEvent: (ev) => send('n64rx:event', { id, kind, ev }) });
      send('n64rx:event', { id, kind, ev: { ev: 'start', mode: run.mode } });
      return h;
    });
    return { id, code: r.code, result: r.result, error: r.error, cancelled: r.cancelled, logFile };
  } finally {
    pruneLogs();
  }
}

function errorResult(e) {
  return { error: { message: e.message, hint: e.hint || null } };
}

function handle(channel, fn) {
  ipcMain.handle(channel, async (event, ...args) => {
    if (!win || event.sender !== win.webContents) throw new Error('not from the app window');
    try {
      return await fn(...args);
    } catch (e) {
      if (e instanceof V.InputError || e instanceof toolchain.ToolchainError) return errorResult(e);
      return errorResult({ message: e.message || String(e), hint: 'Open the log folder for the details.' });
    }
  });
}

function registerIpc() {
  handle('app:about', async () => ({
    version: app.getVersion(), platform: process.platform, arch: process.arch, packaged: app.isPackaged,
    fake: process.env.N64RX_FAKE === '1', electron: process.versions.electron, repo: REPO_URL, data: cache(),
    src: await srcReady.catch(() => null),
  }));
  handle('settings:get', () => loadSettings());
  // the output folder is set only by the folder picker below, never by the renderer
  handle('settings:set', (patch) => setSettings(patch && typeof patch === 'object' ? { ...patch, outDir: undefined, isoDir: undefined } : patch));

  handle('rom:pick', async () => {
    const r = await dialog.showOpenDialog(win, {
      title: 'Choose your N64 ROM', properties: ['openFile'],
      filters: [{ name: 'N64 ROM', extensions: V.ROM_EXT.map((e) => e.slice(1)) }, { name: 'All files', extensions: ['*'] }],
    });
    return r.canceled || !r.filePaths[0] ? null : r.filePaths[0];
  });
  handle('learn:pick', async () => {
    const r = await dialog.showOpenDialog(win, {
      title: 'Choose learn.txt (from the Xbox)', properties: ['openFile'],
      filters: [{ name: 'learn.txt or a serial log', extensions: ['txt', 'log'] }, { name: 'All files', extensions: ['*'] }],
    });
    return r.canceled || !r.filePaths[0] ? null : r.filePaths[0];
  });
  // where the Xbox folder goes: always asked (the native picker, in main), checked writable,
  // remembered; a build accepts only a folder this picker returned
  handle('out:pick', async () => {
    for (;;) {
      let dir = null;
      if (process.env.N64RX_TEST_PICK_OUT && !app.isPackaged) dir = process.env.N64RX_TEST_PICK_OUT === 'cancel' ? null : path.resolve(process.env.N64RX_TEST_PICK_OUT);
      else {
        const r = await dialog.showOpenDialog(win, {
          title: 'Where should the Xbox folder go?', buttonLabel: 'Save here',
          properties: ['openDirectory', 'createDirectory', 'promptToCreate'],
          defaultPath: loadSettings().outDir || safeDocuments() || undefined,
        });
        dir = r.canceled || !r.filePaths[0] ? null : r.filePaths[0];
      }
      if (!dir) return null;
      if (paths.canWrite(dir)) {
        pickedOut.add(path.resolve(dir));
        settingsStore().set({ outDir: dir });
        return dir;
      }
      if (process.env.N64RX_TEST_PICK_OUT) return { error: { message: `The app cannot write to ${dir}.`, hint: 'Choose another folder.' } };
      const again = await dialog.showMessageBox(win, {
        type: 'warning', buttons: ['Choose another folder', 'Cancel'], defaultId: 0, cancelId: 1,
        title: 'Folder not writable', message: 'The app cannot write to that folder.',
        detail: `${dir}\n\nChoose a folder you own, such as one in Documents.`,
      });
      if (again.response !== 0) return null;
    }
  });

  handle('rom:info', async (rom) => {
    V.romFile(rom);
    return n64rx.info(await currentRunner(), rom);
  });

  // a disc image instead: the save dialog, a FATX-safe "<title>.iso" in the last ISO folder
  handle('iso:pick', async (title) => {
    const name = `${V.fatxName(typeof title === 'string' ? title : 'game')}.iso`;
    const s = loadSettings();
    for (;;) {
      let file = null;
      if (process.env.N64RX_TEST_PICK_ISO && !app.isPackaged) file = process.env.N64RX_TEST_PICK_ISO === 'cancel' ? null : path.resolve(process.env.N64RX_TEST_PICK_ISO);
      else {
        const r = await dialog.showSaveDialog(win, {
          title: 'Where should the disc image go?', buttonLabel: 'Save here',
          defaultPath: path.join(s.isoDir || s.outDir || safeDocuments() || app.getPath('home'), name),
          filters: [{ name: 'Disc image', extensions: ['iso'] }], properties: ['createDirectory', 'showOverwriteConfirmation'],
        });
        file = r.canceled || !r.filePath ? null : r.filePath;
      }
      if (!file) return null;
      if (!/\.iso$/i.test(file)) file += '.iso';
      if (paths.canWrite(path.dirname(file))) {
        pickedIso.add(path.resolve(file));
        settingsStore().set({ isoDir: path.dirname(file) });
        return file;
      }
      if (process.env.N64RX_TEST_PICK_ISO) return { error: { message: `The app cannot write to ${path.dirname(file)}.`, hint: 'Choose another folder.' } };
      const again = await dialog.showMessageBox(win, {
        type: 'warning', buttons: ['Choose another place', 'Cancel'], defaultId: 0, cancelId: 1,
        title: 'Folder not writable', message: 'The app cannot write to that folder.', detail: path.dirname(file),
      });
      if (again.response !== 0) return null;
    }
  });

  handle('build:start', async (opts) => {
    const o = V.buildOptions(opts);
    if (o.iso ? !pickedIso.has(path.resolve(o.iso)) : !pickedOut.has(path.resolve(o.out))) {
      throw new V.InputError(o.iso ? 'Choose where the disc image goes first.' : 'Choose where the Xbox folder goes first.',
        'Press Build for Xbox again and pick the place.');
    }
    setSettings({ output: o.iso ? 'iso' : 'xbe', video: o.video, fast: o.fast, romBeside: o.romBeside });
    if (o.out) await fsp.mkdir(o.out, { recursive: true });
    const args = o.iso ? [o.rom, '--iso', o.iso, '--video', o.video] : [o.rom, '--out', o.out, '--video', o.video];
    if (o.fast) args.push('--fast');
    if (o.romBeside) args.push('--rom-beside');
    if (o.learn) args.push('--learn', o.learn);
    return startJob('build', 'build', args);
  });
  handle('deploy:start', async (folder, x) => {
    V.existingDir(folder, 'built folder');
    if (!fs.existsSync(path.join(folder, 'default.xbe'))) throw new V.InputError(`${folder} has no default.xbe.`, 'Build the game first.');
    const host = V.host(x && x.host);
    const user = V.ftpField(x.user || 'xbox', 'user name');
    const pass = V.ftpField(x.pass ?? 'xbox', 'password');
    const to = V.xboxPath(x.to || '/E/Games');
    setSettings({ xbox: { host, user, pass, to } });
    // the password by environment (cli.py reads N64RX_FTP_PASS), never in argv or the log;
    // --opt=value so a value starting with '-' is not taken for an option
    return startJob('deploy', 'deploy', [folder, `--ftp=${host}`, `--user=${user}`, `--to=${to}`], { env: { N64RX_FTP_PASS: pass } });
  });
  handle('job:cancel', () => {
    jobs.cancel();
    return true;
  });

  handle('shell:show', (p) => {
    if (typeof p !== 'string' || !path.isAbsolute(p) || !fs.existsSync(p)) throw new V.InputError('That file or folder is not there any more.');
    shell.showItemInFolder(p);
    return true;
  });
  handle('shell:openLogDir', async () => {
    await fsp.mkdir(logDir(), { recursive: true });
    if (lastLog && fs.existsSync(lastLog)) shell.showItemInFolder(lastLog);
    else await shell.openPath(logDir());
    return true;
  });
  handle('shell:openRepo', () => shell.openExternal(REPO_URL).then(() => true));
  handle('shell:openLicense', async () => {
    const root = await srcReady;
    const lic = path.join(root, 'LICENSE.md');
    if (fs.existsSync(lic)) await shell.openPath(lic);
    else await shell.openExternal(`${REPO_URL}/blob/main/LICENSE.md`);
    return true;
  });
  handle('log:copy', async () => {
    if (!lastLog || !fs.existsSync(lastLog)) return false;
    clipboard.writeText(await fsp.readFile(lastLog, 'utf8'));
    return true;
  });

  handle('data:info', async () => {
    const c = cache();
    return {
      dir: c, portable: DATA.portable, note: DATA.portable || DATA.reason === 'dev' || DATA.reason === 'N64RX_HOME' ? null : DATA.reason,
      builds: await dirSize(path.join(c, 'builds')), toolchain: await dirSize(path.join(c, 'toolchain')),
      src: await dirSize(path.join(c, 'src')), packaged: app.isPackaged,
    };
  });
  handle('data:open', async () => {
    await fsp.mkdir(cache(), { recursive: true });
    await shell.openPath(cache());
    return true;
  });
  handle('data:remove', async (what) => {
    if (!['builds', 'toolchain'].includes(what)) throw new V.InputError('Bad request.');
    if (jobs.busy()) throw new V.InputError('Something is running.', 'Remove it when the build or download has finished.');
    const tools = what === 'toolchain';
    const r = await dialog.showMessageBox(win, {
      type: 'question', buttons: ['Remove', 'Cancel'], defaultId: 1, cancelId: 1,
      title: tools ? 'Remove build tools' : 'Remove build cache',
      message: tools ? 'Remove the build tools?' : 'Remove the build cache?',
      detail: tools ? 'They are downloaded again before the next build. Your built folders are kept.'
        : 'The next build of each game takes longer. Your built folders and the build tools are kept.',
    });
    if (r.response !== 0) return { removed: false };
    const c = cache();
    const targets = tools ? [path.join(c, 'toolchain')] : [path.join(c, 'builds')];
    if (!tools) {
      try {
        for (const v of await fsp.readdir(path.join(c, 'src'))) if (v !== app.getVersion()) targets.push(path.join(c, 'src', v));
      } catch { /* none */ }
    }
    let freed = 0;
    for (const t of targets) {
      freed += await dirSize(t);
      await fsp.rm(t, { recursive: true, force: true });
    }
    return { removed: true, freed, status: toolchainStatus() };
  });

  handle('toolchain:status', () => toolchainStatus());
  handle('toolchain:useSystem', () => {
    setSettings({ toolchain: 'system' });
    return toolchainStatus();
  });
  handle('toolchain:install', async () => {
    const ac = new AbortController();
    const slot = jobs.claimToolchain(() => ac.abort());
    try {
      if (process.env.N64RX_FAKE_TOOLCHAIN === 'missing') return await fakeInstall(ac.signal);
      const dir = await toolchain.install({ cache: cache(), entry: pinnedEntry(), signal: ac.signal, onProgress: (p) => send('toolchain:progress', p) });
      setSettings({ toolchain: 'auto' });
      return { ok: true, dir, status: toolchainStatus() };
    } catch (e) {
      if (ac.signal.aborted) return { cancelled: true };
      throw e;
    } finally {
      jobs.release(slot);
    }
  });
}

/** N64RX_FAKE_TOOLCHAIN=missing: the first-run card's progress, without a download */
async function fakeInstall(signal) {
  const total = 680 * 1024 * 1024;
  for (let r = 0; r <= total; r += total / 60) {
    if (signal.aborted) return { cancelled: true };
    send('toolchain:progress', { phase: 'download', received: r, total, speed: 24 * 1024 * 1024, eta: (total - r) / (24 * 1024 * 1024) });
    await new Promise((res) => setTimeout(res, Number(process.env.N64RX_FAKE_TICK || 50)));
  }
  for (const phase of ['verify', 'unpack']) {
    send('toolchain:progress', { phase });
    await new Promise((res) => setTimeout(res, 400));
  }
  send('toolchain:progress', { phase: 'ready' });
  return { ok: true, status: { state: 'ready', mode: 'fake' } };
}

async function dirSize(d) {
  let n = 0;
  let ents;
  try {
    ents = await fsp.readdir(d, { withFileTypes: true });
  } catch {
    return 0;
  }
  for (const e of ents) {
    const p = path.join(d, e.name);
    if (e.isDirectory()) n += await dirSize(p);
    else if (e.isFile()) n += (await fsp.stat(p).catch(() => ({ size: 0 }))).size;
  }
  return n;
}

// ---------------------------------------------------------------- the window
function serveRenderer() {
  protocol.handle('app', async (req) => {
    const u = new URL(req.url);
    const rel = decodeURIComponent(u.pathname).replace(/^\/+/, '') || 'index.html';
    const file = path.normalize(path.join(RENDERER, rel));
    if (!file.startsWith(RENDERER + path.sep)) return new Response('no', { status: 403 });
    const res = await net.fetch(pathToFileURL(file).toString());
    const headers = new Headers(res.headers);
    headers.set('Content-Security-Policy', CSP);
    headers.set('X-Content-Type-Options', 'nosniff');
    return new Response(res.body, { status: res.status, headers });
  });
}

function createWindow() {
  const opts = {
    width: 980, height: 660, minWidth: 720, minHeight: 520,
    backgroundColor: '#0a1230', show: false, title: 'N64Recomp-X',
    ...(fs.existsSync(path.join(__dirname, 'build', 'icon.png')) ? { icon: path.join(__dirname, 'build', 'icon.png') } : {}),
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true, nodeIntegration: false, sandbox: true,
      webSecurity: true, spellcheck: false, devTools: !app.isPackaged || process.env.N64RX_DEVTOOLS === '1',
    },
  };
  if (IS_MAC) {
    opts.titleBarStyle = 'hiddenInset';
    opts.trafficLightPosition = { x: 16, y: 18 };
  } else if (IS_WIN) {
    opts.titleBarStyle = 'hidden';
    opts.titleBarOverlay = { color: '#00000000', symbolColor: '#c8d2e6', height: 40 };
  }
  win = new BrowserWindow(opts);
  win.setMenuBarVisibility(false);
  win.once('ready-to-show', () => win.show());
  win.webContents.on('will-navigate', (e) => e.preventDefault());
  win.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
  win.on('closed', () => {
    win = null;
  });
  win.loadURL('app://ui/index.html');
}

app.on('web-contents-created', (_e, wc) => {
  wc.on('will-attach-webview', (e) => e.preventDefault());
});

app.on('second-instance', () => {
  if (!win) return;
  if (win.isMinimized()) win.restore();
  win.show();
  win.focus();
});

if (PRIMARY) app.whenReady().then(() => {
  nativeTheme.themeSource = 'dark';
  session.defaultSession.setPermissionRequestHandler((_wc, _perm, cb) => cb(false));
  serveRenderer();
  toolchain.sweep(toolchain.toolchainDir(cache())).catch(() => {});   // what a killed install left
  srcReady = prepareSource();
  srcReady.catch(() => {});
  registerIpc();
  createWindow();
  if (app.isPackaged && fs.existsSync(path.join(process.resourcesPath, 'app-update.yml'))) {
    // inert unless package.json's build.publish is set (electron-builder then writes app-update.yml)
    try {
      require('electron-updater').autoUpdater.checkForUpdatesAndNotify().catch(() => {});
    } catch { /* no updater */ }
  }
  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow();
  });
});

function stopEverything() {
  jobs.cancel();
}
app.on('before-quit', stopEverything);
app.on('window-all-closed', () => {
  stopEverything();
  if (!IS_MAC) app.quit();
});
