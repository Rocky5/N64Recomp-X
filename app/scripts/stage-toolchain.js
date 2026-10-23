'use strict';
// The toolchain the full build of the app carries (electron-builder
// extraResources -> resources/toolchain), already unpacked, so the app
// builds a game on first launch with no download and no install
// (paths.shippedBundle). Made by tools/xbox/pack_toolchain.py from what
// tools/xbox/setup.sh installed on this machine.
//   node scripts/stage-toolchain.js            reuse app/stage/toolchain if its key is this checkout's, else pack it
//   node scripts/stage-toolchain.js --fresh    pack it again
//   node scripts/stage-toolchain.js --from DIR copy an unpacked bundle (pack_toolchain.py's stage folder, --install-to)
const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const paths = require('../src/paths');

const APP = path.resolve(__dirname, '..');
const ROOT = path.resolve(APP, '..');
const PACKER = path.join(ROOT, 'tools', 'xbox', 'pack_toolchain.py');
const DEFAULT_OUT = path.join(APP, 'stage', 'toolchain');
// a Windows unzip fails past 260 characters: keep the bundle's own paths well under, so the app folder can sit deep
const LONGEST_OK = 140;
const ROM_EXT = /\.(z64|n64|v64|rom|eep|sra|fla|mpk|xbe|xiso|iso)$/i;

function die(msg) {
  console.error(`stage-toolchain: ${msg}`);
  process.exit(1);
}

function python() {
  const py = paths.findSystemPython();
  if (!py) die('Python 3.11+ is needed to run tools/xbox/pack_toolchain.py (or set N64RX_PYTHON).');
  return py;
}

/** the identity this checkout packs: {key, os, arch, nxdk, recompiler} */
function wanted(py) {
  const r = spawnSync(py[0], [...py.slice(1), PACKER, '--key'], { encoding: 'utf8', cwd: ROOT });
  if (r.status !== 0) die(`pack_toolchain.py --key failed:\n${r.stderr}`);
  return JSON.parse(r.stdout);
}

function manifest(dir) {
  try {
    return JSON.parse(fs.readFileSync(path.join(dir, 'manifest.json'), 'utf8'));
  } catch {
    return null;
  }
}

/** walk the bundle: files, bytes, the longest path, and anything that looks like ROM material */
function survey(dir) {
  let files = 0;
  let bytes = 0;
  let longest = '';
  const bad = [];
  const head = Buffer.alloc(4);
  const walk = (d) => {
    for (const e of fs.readdirSync(d, { withFileTypes: true })) {
      const p = path.join(d, e.name);
      if (e.isDirectory()) {
        walk(p);
        continue;
      }
      const rel = path.relative(dir, p).replace(/\\/g, '/');
      if (rel.length > longest.length) longest = rel;
      if (!e.isFile()) continue;
      files += 1;
      const st = fs.statSync(p);
      bytes += st.size;
      if (ROM_EXT.test(e.name)) bad.push(`${rel}: a ROM or image extension`);
      if (st.size >= 4) {
        const fd = fs.openSync(p, 'r');
        try {
          fs.readSync(fd, head, 0, 4, 0);
        } finally {
          fs.closeSync(fd);
        }
        if ([0x80371240, 0x37804012, 0x40123780].includes(head.readUInt32BE(0))) bad.push(`${rel}: starts with an N64 ROM header`);
      }
    }
  };
  walk(dir);
  return { files, bytes, longest, bad };
}

function main() {
  const a = process.argv.slice(2);
  const opt = (name) => (a.includes(name) ? a[a.indexOf(name) + 1] : null);
  const out = path.resolve(opt('--out') || DEFAULT_OUT);
  const from = opt('--from') ? path.resolve(opt('--from')) : null;
  const py = python();
  const want = wanted(py);
  const have = manifest(out);

  if (from) {
    const m = manifest(from);
    if (!m) die(`${from} has no manifest.json: point --from at an unpacked bundle (pack_toolchain.py's stage folder).`);
    if (m.key !== want.key) die(`${from} is bundle ${m.key}; this checkout packs ${want.key} (nxdk or recompiler pin moved). Pack it again: node scripts/stage-toolchain.js --fresh`);
    if (path.resolve(from) !== out) {
      console.log(`stage-toolchain: copying ${from} -> ${out}`);
      fs.rmSync(out, { recursive: true, force: true });
      fs.cpSync(from, out, { recursive: true });
    }
  } else if (have && have.key === want.key && !a.includes('--fresh')) {
    console.log(`stage-toolchain: reusing ${out} (key ${have.key}, made ${have.created}); --fresh packs it again`);
  } else {
    console.log(`stage-toolchain: packing the toolchain into ${out} (key ${want.key}; about 2 minutes)`);
    const r = spawnSync(py[0], [...py.slice(1), PACKER, '--no-archive', '--stage', out], { stdio: 'inherit', cwd: ROOT });
    if (r.status !== 0) die('pack_toolchain.py failed (its message is above).');
  }

  const m = manifest(out);
  if (!m || m.key !== want.key) die(`${out} is not a bundle for this checkout after staging.`);
  if (m.recompiler !== want.recompiler) die(`${out}'s recompiler (${m.recompiler}) is not this checkout's (${want.recompiler}).`);
  const b = paths.readBundle(out);
  if (!b.python || !fs.existsSync(b.python)) die(`${out} has no Python at ${m.python}.`);
  if (!fs.existsSync(path.join(out, 'recomp', '.n64rx-key'))) die(`${out}/recomp/.n64rx-key is missing.`);
  const s = survey(out);
  if (s.bad.length) {
    for (const w of s.bad.slice(0, 20)) console.error(`stage-toolchain: ${w}`);
    die(`${s.bad.length} file(s) look like ROM material: refusing (CLAUDE.md: never ship ROM material).`);
  }
  if (s.longest.length > LONGEST_OK) {
    die(`the longest path in the bundle is ${s.longest.length} characters (${s.longest}): a Windows unzip would fail. Prune it in pack_toolchain.py (NXDK_PRUNE).`);
  }
  console.log(`stage-toolchain: ${out}: key ${m.key}, ${s.files} files, ${(s.bytes / 1e6).toFixed(0)} MB, longest path ${s.longest.length} characters; no ROM material`);
}

if (require.main === module) main();
module.exports = { survey, LONGEST_OK };
