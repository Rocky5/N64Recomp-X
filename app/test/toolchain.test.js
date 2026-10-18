'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const http = require('node:http');
const crypto = require('node:crypto');
const zlib = require('node:zlib');
const { pathToFileURL } = require('node:url');
const tar = require('tar');
const tc = require('../src/toolchain');
const paths = require('../src/paths');

const tmp = () => fs.mkdtempSync(path.join(os.tmpdir(), 'n64rx-tc-'));
const sha = (b) => crypto.createHash('sha256').update(b).digest('hex');

/** a small bundle tree: manifest.json, a fake python, a tool */
function bundleTree(key) {
  const d = tmp();
  const root = path.join(d, 'bundle');
  fs.mkdirSync(path.join(root, 'python'), { recursive: true });
  fs.mkdirSync(path.join(root, 'bin'), { recursive: true });
  fs.writeFileSync(path.join(root, 'python', 'python.exe'), 'not really python');
  fs.writeFileSync(path.join(root, 'bin', 'tool.sh'), '#!/bin/sh\necho hi\n', { mode: 0o755 });
  fs.writeFileSync(path.join(root, 'manifest.json'), JSON.stringify({ key, os: process.platform, arch: process.arch, python: 'python/python.exe', env: { NXDK_DIR: 'nxdk' }, path: ['bin'] }));
  return { d, root };
}

async function tarball(key, compress) {
  const { d, root } = bundleTree(key);
  const t = path.join(d, 'b.tar');
  await tar.c({ file: t, cwd: root, portable: true }, ['manifest.json', 'python', 'bin']);
  const raw = fs.readFileSync(t);
  const out = path.join(d, `toolchain.tar.${compress}`);
  fs.writeFileSync(out, compress === 'zst' ? zlib.zstdCompressSync(raw) : zlib.gzipSync(raw));
  return out;
}

/** a stored (uncompressed) zip, written by hand: enough to test the unzip path */
function zipOf(entries) {
  const locals = [];
  const centrals = [];
  let off = 0;
  for (const [name, data] of entries) {
    const n = Buffer.from(name);
    const crc = zlib.crc32(data);
    const lh = Buffer.alloc(30);
    lh.writeUInt32LE(0x04034b50, 0); lh.writeUInt16LE(20, 4); lh.writeUInt32LE(crc, 14);
    lh.writeUInt32LE(data.length, 18); lh.writeUInt32LE(data.length, 22); lh.writeUInt16LE(n.length, 26);
    const ch = Buffer.alloc(46);
    ch.writeUInt32LE(0x02014b50, 0); ch.writeUInt16LE(0x031e, 4); ch.writeUInt16LE(20, 6); ch.writeUInt32LE(crc, 16);
    ch.writeUInt32LE(data.length, 20); ch.writeUInt32LE(data.length, 24); ch.writeUInt16LE(n.length, 28);
    ch.writeUInt32LE(((name.endsWith('/') ? 0o040755 : 0o100644) << 16) >>> 0, 38); ch.writeUInt32LE(off, 42);
    locals.push(lh, n, data);
    centrals.push(ch, n);
    off += 30 + n.length + data.length;
  }
  const cd = Buffer.concat(centrals);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0); end.writeUInt16LE(entries.length, 8); end.writeUInt16LE(entries.length, 10);
  end.writeUInt32LE(cd.length, 12); end.writeUInt32LE(off, 16);
  return Buffer.concat([...locals, cd, end]);
}

function server(body, { honourRange = true } = {}) {
  const seen = [];
  const s = http.createServer((req, res) => {
    seen.push(req.headers.range || null);
    const m = honourRange && /^bytes=(\d+)-$/.exec(req.headers.range || '');
    if (m) {
      const start = Number(m[1]);
      if (start >= body.length) {
        res.writeHead(416, { 'Content-Range': `bytes */${body.length}` });
        res.end();
        return;
      }
      res.writeHead(206, { 'Content-Length': body.length - start, 'Content-Range': `bytes ${start}-${body.length - 1}/${body.length}` });
      res.end(body.subarray(start));
      return;
    }
    res.writeHead(200, { 'Content-Length': body.length });
    res.end(body);
  });
  return new Promise((r) => s.listen(0, '127.0.0.1', () => r({ s, seen, url: `http://127.0.0.1:${s.address().port}/toolchain-win32-x64.tar.zst` })));
}

test('a download resumes from its .part file with a Range request', async () => {
  const body = crypto.randomBytes(300 * 1024);
  const { s, seen, url } = await server(body);
  try {
    const dest = path.join(tmp(), 'tc.tar.zst');
    fs.writeFileSync(dest + '.part', body.subarray(0, 100 * 1024));
    const progress = [];
    await tc.download(url, dest, { size: body.length, onProgress: (p) => progress.push(p) });
    assert.deepEqual(seen, [`bytes=${100 * 1024}-`]);
    assert.equal(sha(fs.readFileSync(dest)), sha(body));
    assert.ok(!fs.existsSync(dest + '.part'));
    assert.equal(progress[0].received, 100 * 1024);
    assert.equal(progress.at(-1).received, body.length);
    assert.equal(progress.at(-1).total, body.length);
  } finally {
    s.close();
  }
});

test('a server that ignores Range gets a fresh download, not a corrupt append', async () => {
  const body = crypto.randomBytes(64 * 1024);
  const { s, url } = await server(body, { honourRange: false });
  try {
    const dest = path.join(tmp(), 'tc.zip');
    fs.writeFileSync(dest + '.part', Buffer.alloc(1000, 7));
    await tc.download(url, dest, { size: body.length });
    assert.equal(sha(fs.readFileSync(dest)), sha(body));
  } finally {
    s.close();
  }
});

test('file:// URLs download (and resume) for local testing', async () => {
  const body = crypto.randomBytes(50 * 1024);
  const src = path.join(tmp(), 'toolchain.tar.zst');
  fs.writeFileSync(src, body);
  const dest = path.join(tmp(), 'out.tar.zst');
  fs.writeFileSync(dest + '.part', body.subarray(0, 20000));
  await tc.download(pathToFileURL(src).href, dest, {});
  assert.equal(sha(fs.readFileSync(dest)), sha(body));
});

test('a checksum mismatch is refused and the download deleted', async () => {
  const archive = await tarball('k-bad', 'zst');
  const cache = tmp();
  const entry = { url: pathToFileURL(archive).href, sha256: 'f'.repeat(64), size: fs.statSync(archive).size, key: 'k-bad' };
  await assert.rejects(tc.install({ cache, entry }), (e) => e instanceof tc.ToolchainError && /checksum/.test(e.message) && !!e.hint);
  const dl = path.join(cache, 'toolchain', 'downloads');
  assert.deepEqual(fs.readdirSync(dl), []);
  assert.ok(!fs.existsSync(path.join(cache, 'toolchain', 'k-bad')));
});

test('a pinned entry without a checksum is refused', async () => {
  const archive = await tarball('k-nosha', 'zst');
  await assert.rejects(tc.install({ cache: tmp(), entry: { url: pathToFileURL(archive).href, sha256: null, size: 1 } }), /no checksum/);
});

for (const kind of ['zst', 'gz']) {
  test(`install unpacks a .tar.${kind} and renames it into place`, async () => {
    const archive = await tarball(`k-${kind}`, kind);
    const cache = tmp();
    const phases = [];
    const entry = { url: pathToFileURL(archive).href, sha256: sha(fs.readFileSync(archive)), size: fs.statSync(archive).size, key: `k-${kind}` };
    const dir = await tc.install({ cache, entry, onProgress: (p) => phases.push(p.phase) });
    assert.equal(dir, path.join(cache, 'toolchain', `k-${kind}`));
    assert.ok(fs.existsSync(path.join(dir, 'python', 'python.exe')));
    if (process.platform !== 'win32') assert.ok(fs.statSync(path.join(dir, 'bin', 'tool.sh')).mode & 0o100);
    for (const p of ['space', 'download', 'verify', 'unpack', 'ready']) assert.ok(phases.includes(p), p);
    const cur = JSON.parse(fs.readFileSync(path.join(cache, 'toolchain', 'current.json'), 'utf8'));
    assert.equal(cur.key, `k-${kind}`);
    // no scratch folders or archive left behind
    assert.deepEqual(fs.readdirSync(path.join(cache, 'toolchain')).sort(), ['current.json', 'downloads', `k-${kind}`]);
    assert.deepEqual(fs.readdirSync(path.join(cache, 'toolchain', 'downloads')), []);
    const b = paths.installedBundle(cache, `k-${kind}`);
    assert.equal(b.python, path.join(dir, 'python', 'python.exe'));
  });
}

test('install unpacks a .zip whose manifest is inside one top-level folder', async () => {
  const z = zipOf([
    ['tc/', Buffer.alloc(0)],
    ['tc/manifest.json', Buffer.from(JSON.stringify({ key: 'k-zip', python: 'py/python.exe', env: {} }))],
    ['tc/py/', Buffer.alloc(0)],
    ['tc/py/python.exe', Buffer.from('x')],
  ]);
  const file = path.join(tmp(), 'toolchain.zip');
  fs.writeFileSync(file, z);
  const cache = tmp();
  const dir = await tc.install({ cache, entry: { url: pathToFileURL(file).href, sha256: sha(z), size: z.length, key: 'k-zip' } });
  assert.ok(fs.existsSync(path.join(dir, 'manifest.json')));
  assert.ok(fs.existsSync(path.join(dir, 'py', 'python.exe')));
});

test('a zip entry that climbs out of the folder is refused', async () => {
  const z = zipOf([['../evil.txt', Buffer.from('x')], ['manifest.json', Buffer.from('{"key":"e"}')]]);
  const file = path.join(tmp(), 'evil.zip');
  fs.writeFileSync(file, z);
  const dest = path.join(tmp(), 'out');
  await assert.rejects(tc.unpack(file, dest), /bad path|invalid relative path/);
  assert.ok(!fs.existsSync(path.join(path.dirname(dest), 'evil.txt')));
});

test('an archive for another key than the pinned one is refused', async () => {
  const archive = await tarball('k-other', 'zst');
  const entry = { url: pathToFileURL(archive).href, sha256: sha(fs.readFileSync(archive)), size: 1, key: 'k-pinned' };
  await assert.rejects(tc.install({ cache: tmp(), entry }), /expects k-pinned/);
});

test('not enough disk space stops before downloading, with what to do', async () => {
  await assert.rejects(tc.checkSpace(tmp(), 1e18), (e) => /disk space/.test(e.message) && /Free some space/.test(e.hint));
  const free = await tc.freeBytes(tmp());
  assert.ok(free > 0);
});

test('toolchain.json placeholders mean no download; N64RX_TOOLCHAIN_URL overrides', () => {
  const e = tc.pinned('win32-x64', { env: {} });
  assert.equal(e.url, null);
  const o = tc.pinned('win32-x64', { env: { N64RX_TOOLCHAIN_URL: 'file:///tmp/t.tar.zst', N64RX_TOOLCHAIN_SHA256: 'A'.repeat(64) } });
  assert.equal(o.url, 'file:///tmp/t.tar.zst');
  assert.equal(o.sha256, 'a'.repeat(64));
  assert.equal(o.override, true);
});

/** a tar written by hand: [name, type ('0' file, '1' hard link, '2' symlink), body or link target] */
function rawTar(entries) {
  const blocks = [];
  for (const [name, type, data] of entries) {
    const h = Buffer.alloc(512);
    const body = type === '0' ? Buffer.from(data) : Buffer.alloc(0);
    h.write(name, 0, 'utf8');
    h.write('0000644\0', 100);
    h.write('0000000\0', 108);
    h.write('0000000\0', 116);
    h.write(body.length.toString(8).padStart(11, '0') + '\0', 124);
    h.write('00000000000\0', 136);
    h.write('        ', 148);
    h.write(type, 156);
    if (type !== '0') h.write(data, 157, 'utf8');
    h.write('ustar\0', 257);
    h.write('00', 263);
    let sum = 0;
    for (const b of h) sum += b;
    h.write(sum.toString(8).padStart(6, '0') + '\0 ', 148);
    blocks.push(h, body, Buffer.alloc((512 - (body.length % 512)) % 512));
  }
  blocks.push(Buffer.alloc(1024));
  return Buffer.concat(blocks);
}

for (const [label, entries] of [
  ['a symlink pointing out of the folder', [['manifest.json', '0', '{"key":"t"}'], ['out', '2', '../../escaped']]],
  ['an absolute symlink', [['manifest.json', '0', '{"key":"t"}'], ['abs', '2', process.platform === 'win32' ? 'C:\Windows' : '/etc']]],
  ['a hard link to a file outside', [['manifest.json', '0', '{"key":"t"}'], ['hl', '1', '../outside.txt']]],
]) {
  test(`tar: ${label} is refused`, async () => {
    const d = tmp();
    fs.writeFileSync(path.join(d, 'outside.txt'), 'outside');
    const file = path.join(d, 'evil.tar.gz');
    fs.writeFileSync(file, zlib.gzipSync(rawTar(entries)));
    const dest = path.join(d, 'x', 'out');
    await assert.rejects(tc.unpack(file, dest), /link|path/i);
    assert.ok(!fs.existsSync(path.join(d, 'escaped')));
  });
}

test('tar: a link inside the folder is kept (a bundle\'s llvm/bin/clang -> clang-21)', { skip: process.platform === 'win32' }, async () => {
  const d = tmp();
  const file = path.join(d, 'ok.tar.gz');
  fs.writeFileSync(file, zlib.gzipSync(rawTar([['bin/clang-21', '0', 'x'], ['bin/clang', '2', 'clang-21']])));
  const dest = path.join(d, 'out');
  await tc.unpack(file, dest);
  assert.equal(fs.readlinkSync(path.join(dest, 'bin', 'clang')), 'clang-21');
});

test('zip: a symlink entry is refused', async () => {
  const z = zipOf([['manifest.json', Buffer.from('{"key":"z"}')], ['link', Buffer.from('../../x')]]);
  // mark the second entry a symlink (unix mode 0120777 in the central directory's external attributes)
  const cd = z.indexOf(Buffer.from([0x50, 0x4b, 0x01, 0x02]), z.indexOf(Buffer.from([0x50, 0x4b, 0x01, 0x02])) + 1);
  z.writeUInt32LE((0o120777 << 16) >>> 0, cd + 38);
  const file = path.join(tmp(), 'link.zip');
  fs.writeFileSync(file, z);
  await assert.rejects(tc.unpack(file, path.join(tmp(), 'out')), /link/);
});

for (const key of ['..', '.hidden', '../up', 'a/b', '']) {
  test(`a manifest key ${JSON.stringify(key)} is refused`, async () => {
    const d = tmp();
    const file = path.join(d, 'k.tar.gz');
    fs.writeFileSync(file, zlib.gzipSync(rawTar([['manifest.json', '0', JSON.stringify({ key })]])));
    const cache = tmp();
    await assert.rejects(tc.install({ cache, entry: { url: pathToFileURL(file).href, sha256: sha(fs.readFileSync(file)), size: 10 } }), /key/);
    assert.deepEqual(fs.readdirSync(path.join(cache, 'toolchain')).sort(), ['downloads']);
  });
}

test('stale .unpack-* and *.old-* folders are swept', async () => {
  const tdir = path.join(tmp(), 'toolchain');
  for (const n of ['.unpack-123-456', 'k1.old-1700000000000', 'k1', 'downloads']) fs.mkdirSync(path.join(tdir, n), { recursive: true });
  assert.equal(await tc.sweep(tdir), 2);
  assert.deepEqual(fs.readdirSync(tdir).sort(), ['downloads', 'k1']);
});
