'use strict';
// The source set the packaged app carries (electron-builder extraResources
// -> resources/src), copied on first run to <cache>/src/<version>/ where
// n64rx runs from it. Built from `git ls-files` (read only): only tracked
// files, so nothing a build or a ROM left in the checkout can get in.
//   node scripts/stage-src.js [--out DIR] [--list]
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { spawnSync } = require('node:child_process');

const APP = path.resolve(__dirname, '..');
const ROOT = path.resolve(APP, '..');

// what n64rx's build reads (pipeline.py, engines.py, xbox/build.sh, xbox/CMakeLists.txt)
const INCLUDE = [
  /^xbox\//,
  /^games\//,
  /^patches\//,
  /^tools\/n64rx\//,
  /^tools\/xbox\/[^/]+\.(py|sh)$/,    // compile_game.py, thinlto_link.py, patch_pbkit.py, xbe_sections.py, recomp_src.py, ...
  /^tools\/xbox\/(docker|msys)\//,
  /^VERSION$/,
  /^LICENSE\.md$/,
];
const EXCLUDE = [
  /^tools\/xbox\/test_[^/]*$/,        // host tests
  /(^|\/)(tests|docs)\//,
  /^tools\/recomp\/.*(^|\/)test\//,
  /(^|\/)\.git(hub|modules|attributes|ignore)?(\/|$)/,
  /(^|\/)__pycache__\//,
  /^(out|build)[^/]*\//,
  // never ROM material (CLAUDE.md), even if one were tracked by mistake
  /\.(z64|n64|v64|rom|eep|sra|fla|mpk|xbe|xiso|iso|bmp)$/i,
];

function git(args, cwd = ROOT) {
  const r = spawnSync('git', args, { cwd, encoding: 'buffer', maxBuffer: 256 * 1024 * 1024 });
  if (r.status !== 0) throw new Error(`git ${args.join(' ')} failed: ${r.stderr.toString()}`);
  return r.stdout;
}

function selected() {
  const all = git(['ls-files', '-z']).toString('utf8').split('\0').filter(Boolean);
  return all.filter((f) => INCLUDE.some((re) => re.test(f)) && !EXCLUDE.some((re) => re.test(f)));
}

/** every staged file through tools/xbox/rom_leak_check.py's own scan() (the same rules as CI): [[rel, why]] */
function leakCheck(dir) {
  const py = require('../src/paths').findSystemPython();
  if (!py) {
    console.error('stage-src: Python 3.11+ is needed to run tools/xbox/rom_leak_check.py over the staged files (or set N64RX_PYTHON).');
    process.exit(1);
  }
  const code = [
    'import json, os, sys',
    `sys.path.insert(0, ${JSON.stringify(path.join(ROOT, 'tools', 'xbox'))})`,
    'import rom_leak_check',
    'root = sys.argv[1]',
    'bad = []',
    'for d, _, names in os.walk(root):',
    '    for n in names:',
    '        p = os.path.join(d, n)',
    '        rel = os.path.relpath(p, root).replace(os.sep, "/")',
    '        with open(p, "rb") as f:',
    '            data = f.read()',
    '        bad += [[rel, w] for w in rom_leak_check.scan(rel, data)]',
    'print(json.dumps(bad))',
  ].join('\n');
  const r = spawnSync(py[0], [...py.slice(1), '-I', '-c', code, dir], { encoding: 'utf8', maxBuffer: 64 * 1024 * 1024 });
  if (r.status !== 0) {
    console.error(`stage-src: rom_leak_check failed to run:\n${r.stderr}`);
    process.exit(1);
  }
  return JSON.parse(r.stdout);
}

function main() {
  const a = process.argv.slice(2);
  const out = path.resolve(a.includes('--out') ? a[a.indexOf('--out') + 1] : path.join(APP, 'stage', 'src'));
  const files = selected();
  if (a.includes('--list')) {
    process.stdout.write(files.join('\n') + '\n');
    return;
  }
  fs.rmSync(out, { recursive: true, force: true });
  const h = crypto.createHash('sha256');
  let bytes = 0;
  for (const f of files) {
    const src = path.join(ROOT, f);
    let st;
    try {
      st = fs.lstatSync(src);
    } catch {
      continue;   // tracked but deleted in the working tree
    }
    if (!st.isFile()) continue;
    const data = fs.readFileSync(src);
    for (const w of [0x80371240, 0x37804012, 0x40123780]) {   // an N64 header at offset 0: never shipped
      if (data.length >= 4 && data.readUInt32BE(0) === w) {
        console.error(`stage-src: ${f} starts with an N64 ROM header; refusing.`);
        process.exit(1);
      }
    }
    const dst = path.join(out, f);
    fs.mkdirSync(path.dirname(dst), { recursive: true });
    fs.writeFileSync(dst, data, { mode: st.mode & 0o777 });
    h.update(f + '\0' + crypto.createHash('sha256').update(data).digest('hex') + '\0');
    bytes += data.length;
  }
  // N64Recomp's source is not shipped: the toolchain bundle's prebuilt recompiler runs, its key
  // passed as N64RX_RECOMPILER_KEY (paths.runner). The submodule's pin is kept for the record.
  let pin = null;
  try {
    pin = git(['rev-parse', 'HEAD'], path.join(ROOT, 'tools', 'recomp')).toString().trim();
  } catch { /* no submodule checkout */ }
  const version = fs.readFileSync(path.join(ROOT, 'VERSION'), 'utf8').trim();
  const stamp = { version, app: require(path.join(APP, 'package.json')).version, files: files.length, bytes, recompPin: pin, hash: h.digest('hex') };
  fs.writeFileSync(path.join(out, 'stage.json'), JSON.stringify(stamp, null, 2) + '\n');
  const problems = leakCheck(out);
  if (problems.length) {
    for (const [rel, why] of problems) console.error(`stage-src: ${rel}: ${why}`);
    fs.rmSync(out, { recursive: true, force: true });
    console.error(`stage-src: ${problems.length} problem(s): nothing staged (CLAUDE.md: never ship ROM material).`);
    process.exit(1);
  }
  console.log(`staged ${files.length} files, ${(bytes / 1048576).toFixed(1)} MB, into ${out}; rom_leak_check rules: clean`);
}

if (require.main === module) main();
module.exports = { INCLUDE, EXCLUDE, selected, leakCheck };
