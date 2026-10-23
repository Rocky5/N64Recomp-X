'use strict';
// The build tools (Python, LLVM, nxdk, N64Recomp, MSYS2 on Windows) as one
// archive per OS, made by tools/xbox/pack_toolchain.py. The full build of the
// app ships them unpacked (resources/toolchain, paths.shippedBundle) and never
// comes here; otherwise they are downloaded once:
// free-space check, download with progress and resume (a .part file and an
// HTTP Range request), SHA-256 check (a mismatch is refused and the part
// deleted), unpack to a scratch folder, then one rename into
// <cache>/toolchain/<key>/.
const fs = require('node:fs');
const fsp = require('node:fs/promises');
const path = require('node:path');
const crypto = require('node:crypto');
const zlib = require('node:zlib');
const { pipeline } = require('node:stream/promises');
const { Transform } = require('node:stream');
const { spawn } = require('node:child_process');
const { fileURLToPath } = require('node:url');

class ToolchainError extends Error {
  constructor(message, hint) {
    super(message);
    this.hint = hint || null;
  }
}

const MB = 1024 * 1024;

// a packaged app with no toolchain inside it (the no-tools build) and no download set for this OS
const NO_TOOLS = {
  message: 'This copy of the app has no build tools inside it, and there is no download for them yet.',
  hint: 'Get the full N64Recomp-X zip for your system (the build tools are inside it), unzip it to a folder you own and run the app from there. '
    + 'If this computer already has the toolchain (n64rx doctor --install), press "Use the tools already on this computer" instead.',
};

/** the archive for this OS/arch: toolchain.json's entry, or N64RX_TOOLCHAIN_URL (+ _SHA256) */
function pinned(key, { env = process.env, manifest = path.join(__dirname, '..', 'toolchain.json') } = {}) {
  let table = {};
  try {
    table = JSON.parse(fs.readFileSync(manifest, 'utf8'));
  } catch { /* none */ }
  const e = { ...(table[key] || {}) };
  if (env.N64RX_TOOLCHAIN_URL) {
    e.url = env.N64RX_TOOLCHAIN_URL;
    e.sha256 = env.N64RX_TOOLCHAIN_SHA256 || null;
    e.size = Number(env.N64RX_TOOLCHAIN_SIZE) || null;
    e.key = env.N64RX_TOOLCHAIN_KEY || null;   // else the archive's manifest.json says
    e.override = true;
  }
  if (!e.url || /^PLACEHOLDER/i.test(e.url)) e.url = null;
  if (e.sha256 && !/^[0-9a-f]{64}$/i.test(e.sha256)) e.sha256 = null;
  return { platform: key, url: e.url || null, sha256: e.sha256 ? e.sha256.toLowerCase() : null,
    size: Number(e.size) || null, unpacked: Number(e.unpacked) || null, key: e.key || null, override: !!e.override };
}

function toolchainDir(cache) {
  return path.join(cache, 'toolchain');
}

function archiveName(url) {
  let base = 'toolchain';
  try {
    base = decodeURIComponent(path.posix.basename(new URL(url).pathname)) || base;
  } catch { /* keep */ }
  return base.replace(/[^A-Za-z0-9._-]/g, '_');
}

async function freeBytes(dir) {
  let d = dir;
  for (;;) {
    try {
      const s = await fsp.statfs(d);
      return Number(s.bavail) * Number(s.bsize);
    } catch {
      const up = path.dirname(d);
      if (up === d) return Infinity;   // cannot tell: do not block
      d = up;
    }
  }
}

async function checkSpace(dir, needed) {
  const free = await freeBytes(dir);
  if (free < needed) {
    throw new ToolchainError(
      `Not enough free disk space: the build tools need about ${Math.ceil(needed / MB)} MB and the drive has ${Math.floor(free / MB)} MB.`,
      `Free some space on the drive that holds ${dir}, then press Try again.`);
  }
  return free;
}

async function sha256File(file, signal) {
  const h = crypto.createHash('sha256');
  await pipeline(fs.createReadStream(file), new Transform({
    transform(c, _e, cb) {
      h.update(c);
      cb();
    },
  }), { signal });
  return h.digest('hex');
}

function speedMeter() {
  const samples = [];
  return (bytes) => {
    const now = Date.now();
    samples.push([now, bytes]);
    while (samples.length > 2 && now - samples[0][0] > 4000) samples.shift();
    const [t0, b0] = samples[0];
    return now > t0 ? ((bytes - b0) * 1000) / (now - t0) : 0;
  };
}

/** a readable for url from byte `start`: {stream, total, resumed} */
async function openSource(url, start, signal) {
  const u = new URL(url);
  if (u.protocol === 'file:') {
    const file = fileURLToPath(u);
    let st;
    try {
      st = await fsp.stat(file);
    } catch {
      throw new ToolchainError(`No build tools archive at ${file}.`, 'Check N64RX_TOOLCHAIN_URL.');
    }
    if (start >= st.size) return { stream: null, total: st.size, resumed: true };
    return { stream: fs.createReadStream(file, { start }), total: st.size, resumed: start > 0 };
  }
  if (u.protocol !== 'https:' && u.protocol !== 'http:') {
    throw new ToolchainError(`The build tools address is not http(s) or file: ${url}`, 'Check toolchain.json or N64RX_TOOLCHAIN_URL.');
  }
  let res;
  try {
    res = await fetch(url, { headers: start > 0 ? { Range: `bytes=${start}-` } : {}, redirect: 'follow', signal });
  } catch (e) {
    if (signal && signal.aborted) throw e;
    throw new ToolchainError(`Could not reach the download server (${e.cause?.code || e.message}).`,
      'Check the internet connection, then press Try again: the download continues where it stopped.');
  }
  if (res.status === 416) return { stream: null, total: start, resumed: true };
  if (res.status !== 200 && res.status !== 206) {
    throw new ToolchainError(`The download server answered ${res.status} ${res.statusText}.`,
      res.status === 404 ? 'This version\'s build tools are not published yet: try a newer release of the app.'
        : 'Try again in a few minutes.');
  }
  const len = Number(res.headers.get('content-length')) || 0;
  if (res.status === 206) {
    const m = /\/(\d+)\s*$/.exec(res.headers.get('content-range') || '');
    return { stream: require('node:stream').Readable.fromWeb(res.body), total: m ? Number(m[1]) : start + len, resumed: true };
  }
  return { stream: require('node:stream').Readable.fromWeb(res.body), total: len || null, resumed: false };
}

/**
 * download url to dest, resuming from dest + '.part'. onProgress({received,
 * total, speed, eta}). Returns dest.
 */
async function download(url, dest, { size = null, onProgress = () => {}, signal } = {}) {
  const part = dest + '.part';
  await fsp.mkdir(path.dirname(dest), { recursive: true });
  let start = 0;
  try {
    start = (await fsp.stat(part)).size;
  } catch { /* fresh */ }
  if (size && start > size) {
    await fsp.rm(part, { force: true });
    start = 0;
  }
  const src = await openSource(url, start, signal);
  if (!src.resumed) start = 0;
  const total = src.total || size || null;
  const speed = speedMeter();
  let received = start;
  let last = 0;
  const report = (force) => {
    const now = Date.now();
    if (!force && now - last < 200) return;
    last = now;
    const s = speed(received);
    onProgress({ received, total, speed: s, eta: total && s > 0 ? (total - received) / s : null });
  };
  report(true);
  if (src.stream) {
    const out = fs.createWriteStream(part, { flags: start > 0 ? 'a' : 'w' });
    await pipeline(src.stream, new Transform({
      transform(c, _e, cb) {
        received += c.length;
        report(false);
        cb(null, c);
      },
    }), out, { signal });
  }
  report(true);
  await fsp.rename(part, dest);
  return dest;
}

async function magic(file) {
  const fh = await fsp.open(file, 'r');
  try {
    const b = Buffer.alloc(6);
    await fh.read(b, 0, 6, 0);
    if (b[0] === 0x28 && b[1] === 0xb5 && b[2] === 0x2f && b[3] === 0xfd) return 'zst';
    if (b[0] === 0xfd && b.slice(1, 6).toString('latin1') === '7zXZ\0') return 'xz';
    if (b[0] === 0x1f && b[1] === 0x8b) return 'gz';
    if (b[0] === 0x50 && b[1] === 0x4b && b[2] === 0x03 && b[3] === 0x04) return 'zip';
    return null;
  } finally {
    await fh.close();
  }
}

function inside(root, p) {
  const r = path.resolve(root);
  const q = path.resolve(p);
  return q === r || q.startsWith(r + path.sep);
}

function safeJoin(root, name) {
  const p = path.resolve(root, name);
  if (!inside(root, p)) throw new ToolchainError(`The archive has a bad path: ${name}`, 'Download it again (Try again).');
  return p;
}

/** no folder between root and p is a link (so nothing is written through one out of root) */
function noLinkOnTheWay(root, p) {
  const r = path.resolve(root);
  let d = path.dirname(path.resolve(p));
  while (d.length > r.length && inside(r, d)) {
    try {
      if (fs.lstatSync(d).isSymbolicLink()) throw new ToolchainError(`The archive writes through a link: ${path.relative(r, p)}`, 'Download it again (Try again).');
    } catch (e) {
      if (e instanceof ToolchainError) throw e;
    }
    d = path.dirname(d);
  }
}

async function unzip(file, dest) {
  const yauzl = require('yauzl');
  const zip = await new Promise((res, rej) => yauzl.open(file, { lazyEntries: true, autoClose: true }, (e, z) => (e ? rej(e) : res(z))));
  await new Promise((resolve, reject) => {
    zip.on('error', reject);
    zip.on('end', resolve);
    zip.on('entry', (entry) => {
      const name = entry.fileName.replace(/\\/g, '/');
      let target;
      try {
        target = safeJoin(dest, name);
      } catch (e) {
        reject(e);
        return;
      }
      const mode = (entry.externalFileAttributes >>> 16) & 0o170777;
      if ((mode & 0o170000) === 0o120000) {   // a symbolic link: the bundles have none in their zips
        reject(new ToolchainError(`The archive has a link: ${name}`, 'Download it again (Try again).'));
        return;
      }
      try {
        noLinkOnTheWay(dest, target);
      } catch (e2) {
        reject(e2);
        return;
      }
      if (name.endsWith('/')) {
        fs.mkdirSync(target, { recursive: true });
        zip.readEntry();
        return;
      }
      fs.mkdirSync(path.dirname(target), { recursive: true });
      zip.openReadStream(entry, (e, rs) => {
        if (e) {
          reject(e);
          return;
        }
        const ws = fs.createWriteStream(target, { mode: (mode & 0o777) || 0o644 });
        pipeline(rs, ws).then(() => zip.readEntry(), reject);
      });
    });
    zip.readEntry();
  });
}

function systemTar() {
  if (process.platform === 'win32') {
    const t = path.join(process.env.SystemRoot || 'C:\\Windows', 'System32', 'tar.exe');
    return fs.existsSync(t) ? t : 'tar';
  }
  return 'tar';
}

/** unpack a .tar.zst / .tar.gz / .tar.xz / .zip into dest (made fresh) */
async function unpack(file, dest, { signal } = {}) {
  await fsp.rm(dest, { recursive: true, force: true });
  await fsp.mkdir(dest, { recursive: true });
  const kind = await magic(file);
  const tar = require('tar');
  const bad = [];
  // node-tar already refuses absolute and '..' paths and writing through a link; links
  // themselves must also point inside dest (a bundle's own: llvm/bin/clang -> clang-21)
  const filter = (p, entry) => {
    if (entry.type === 'SymbolicLink' || entry.type === 'Link') {
      const from = entry.type === 'Link' ? dest : path.dirname(path.resolve(dest, p));
      if (!entry.linkpath || path.isAbsolute(entry.linkpath) || !inside(dest, path.resolve(from, entry.linkpath))) {
        bad.push(p);
        return false;
      }
    }
    return true;
  };
  const tx = () => tar.x({ cwd: dest, strict: true, filter });
  if (kind === 'zst') {
    if (typeof zlib.createZstdDecompress !== 'function') {
      throw new ToolchainError('This system cannot unpack .tar.zst archives.', 'Use a .zip build tools archive, or a newer app.');
    }
    await pipeline(fs.createReadStream(file), zlib.createZstdDecompress(), tx(), { signal });
  } else if (kind === 'gz') {
    await pipeline(fs.createReadStream(file), zlib.createGunzip(), tx(), { signal });
  } else if (kind === 'xz') {
    // no xz in Node's zlib: the system's tar (bsdtar on Windows 10+, GNU or bsd tar elsewhere)
    await new Promise((resolve, reject) => {
      const p = spawn(systemTar(), ['-xJf', file, '-C', dest], { windowsHide: true, stdio: ['ignore', 'ignore', 'pipe'] });
      let err = '';
      p.stderr.on('data', (c) => { err += c; });
      p.on('error', reject);
      p.on('close', (c) => (c === 0 ? resolve() : reject(new ToolchainError(`Unpacking failed: ${err.trim() || `tar exit ${c}`}`, 'Download it again (Try again).'))));
      if (signal) signal.addEventListener('abort', () => p.kill(), { once: true });
    });
  } else if (kind === 'zip') {
    await unzip(file, dest);
  } else {
    throw new ToolchainError('The downloaded file is not an archive the app knows (.tar.zst, .tar.xz, .tar.gz, .zip).', 'Check N64RX_TOOLCHAIN_URL or toolchain.json.');
  }
  if (bad.length) {
    await fsp.rm(dest, { recursive: true, force: true });
    throw new ToolchainError(`The archive has a link pointing out of its folder: ${bad[0]}`, 'Download it again (Try again).');
  }
  return dest;
}

/** the folder holding manifest.json: dest itself or its one top-level folder */
async function bundleRoot(dest) {
  if (fs.existsSync(path.join(dest, 'manifest.json'))) return dest;
  const ents = (await fsp.readdir(dest, { withFileTypes: true })).filter((d) => d.isDirectory());
  if (ents.length === 1 && fs.existsSync(path.join(dest, ents[0].name, 'manifest.json'))) return path.join(dest, ents[0].name);
  throw new ToolchainError('The build tools archive has no manifest.json.', 'It was not made by tools/xbox/pack_toolchain.py: check the archive.');
}

async function removeLater(dir) {
  const trash = `${dir}.old-${Date.now()}`;
  try {
    await fsp.rename(dir, trash);
  } catch {
    return;
  }
  fsp.rm(trash, { recursive: true, force: true }).catch(() => {});
}

/** what a killed install leaves: .unpack-* scratch folders and *.old-* folders */
async function sweep(tdir) {
  let names = [];
  try {
    names = await fsp.readdir(tdir);
  } catch {
    return 0;
  }
  const stale = names.filter((n) => n.startsWith('.unpack-') || /\.old-\d+$/.test(n));
  for (const n of stale) await fsp.rm(path.join(tdir, n), { recursive: true, force: true }).catch(() => {});
  return stale.length;
}

/**
 * The whole first-run job. onProgress({phase: 'space'|'download'|'verify'|
 * 'unpack'|'ready', received, total, speed, eta}). Returns the installed
 * bundle's folder.
 */
async function install({ cache, entry, onProgress = () => {}, signal }) {
  if (!entry || !entry.url) throw new ToolchainError(NO_TOOLS.message, NO_TOOLS.hint);
  const tdir = toolchainDir(cache);
  await sweep(tdir);
  const dl = path.join(tdir, 'downloads');
  await fsp.mkdir(dl, { recursive: true });
  const archive = path.join(dl, archiveName(entry.url));
  let have = 0;
  try {
    have = (await fsp.stat(archive + '.part')).size;
  } catch { /* none */ }
  const done = fs.existsSync(archive);
  onProgress({ phase: 'space' });
  const size = entry.size || 0;
  await checkSpace(tdir, (done ? 0 : Math.max(0, size - have)) + (entry.unpacked || size * 3) + 200 * MB);

  if (!done) {
    onProgress({ phase: 'download', received: have, total: size || null, speed: 0, eta: null });
    await download(entry.url, archive, { size: entry.size, signal, onProgress: (p) => onProgress({ phase: 'download', ...p }) });
  }
  onProgress({ phase: 'verify' });
  if (entry.sha256) {
    const got = await sha256File(archive, signal);
    if (got !== entry.sha256) {
      await fsp.rm(archive, { force: true });
      await fsp.rm(archive + '.part', { force: true });
      throw new ToolchainError('The downloaded build tools are damaged (their checksum does not match), so they were deleted.',
        'Press Try again to download them again. If it keeps happening, the download is being changed on the way: try another network.');
    }
  } else if (!entry.override) {
    await fsp.rm(archive, { force: true });
    throw new ToolchainError('This app\'s build tools entry has no checksum, so the download was not used.', 'Get a release of the app with a complete toolchain.json.');
  }

  onProgress({ phase: 'unpack' });
  const scratch = path.join(tdir, `.unpack-${process.pid}-${Date.now()}`);
  try {
    await unpack(archive, scratch, { signal });
    const root = await bundleRoot(scratch);
    const mf = JSON.parse(await fsp.readFile(path.join(root, 'manifest.json'), 'utf8'));
    const key = String(mf.key || entry.key || '');
    if (!/^[A-Za-z0-9][A-Za-z0-9._+-]{0,127}$/.test(key) || key.includes('..')) {
      throw new ToolchainError(`The build tools manifest has a bad key (${JSON.stringify(key).slice(0, 40)}).`, 'Check the archive (tools/xbox/pack_toolchain.py writes one).');
    }
    if (entry.key && mf.key && entry.key !== mf.key) {
      throw new ToolchainError(`The build tools archive is for ${mf.key}, the app expects ${entry.key}.`, 'Get the matching release of the app, or set N64RX_TOOLCHAIN_URL to the right archive.');
    }
    const final = path.join(tdir, key);
    if (path.dirname(final) !== path.resolve(tdir)) throw new ToolchainError('The build tools key is not a plain name.', 'Check the archive.');
    if (fs.existsSync(final)) await removeLater(final);
    await fsp.rename(root, final);
    await fsp.writeFile(path.join(tdir, 'current.json'), JSON.stringify({ key, url: entry.url, sha256: entry.sha256, installed: new Date().toISOString() }, null, 2));
    await fsp.rm(archive, { force: true });
    onProgress({ phase: 'ready' });
    return final;
  } finally {
    await fsp.rm(scratch, { recursive: true, force: true }).catch(() => {});
  }
}

module.exports = { ToolchainError, NO_TOOLS, pinned, toolchainDir, archiveName, freeBytes, checkSpace, sha256File, download, unpack, bundleRoot, install, sweep };
