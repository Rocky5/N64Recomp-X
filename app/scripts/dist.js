'use strict';
// electron-builder for this OS, with the build tools inside (the default) or
// without them.
//   node scripts/dist.js             the full build: stage/toolchain -> resources/toolchain (download, unzip, run)
//   node scripts/dist.js --no-tools  the app alone; it downloads the tools from toolchain.json's URL on first run
//   node scripts/dist.js --dir       the unpacked folder only (dist/win-unpacked ...), no zip
// npm run dist runs make-icon.js, stage-src.js and stage-toolchain.js first.
const fs = require('node:fs');
const path = require('node:path');

const APP = path.resolve(__dirname, '..');
const TOOLS = path.join(APP, 'stage', 'toolchain');

function dirStats(d) {
  let files = 0;
  let bytes = 0;
  const walk = (x) => {
    for (const e of fs.readdirSync(x, { withFileTypes: true })) {
      const p = path.join(x, e.name);
      if (e.isDirectory()) walk(p);
      else if (e.isFile()) {
        files += 1;
        bytes += fs.statSync(p).size;
      }
    }
  };
  walk(d);
  return { files, bytes };
}

const MB = (n) => `${(n / 1e6).toFixed(0)} MB`;

async function main() {
  const a = process.argv.slice(2);
  const tools = !a.includes('--no-tools');
  const dir = a.includes('--dir');
  const config = {};
  if (tools) {
    if (!fs.existsSync(path.join(TOOLS, 'manifest.json'))) {
      console.error('dist: no toolchain in stage/toolchain: run node scripts/stage-toolchain.js (npm run dist does), or use --no-tools.');
      process.exit(1);
    }
    // already unpacked, so the first launch needs no extraction; recomp/.n64rx-key is a dotfile and must come along
    config.extraResources = [{ from: 'stage/toolchain', to: 'toolchain', filter: ['**/*', '**/.*'] }];
  } else {
    config.win = { artifactName: 'N64Recomp-X-${version}-win-${arch}-no-tools.${ext}' };
    config.mac = { artifactName: 'N64Recomp-X-${version}-mac-${arch}-no-tools.${ext}' };
    config.appImage = { artifactName: 'N64Recomp-X-${version}-${arch}-no-tools.AppImage' };
  }
  const builder = require('electron-builder');
  const out = await builder.build({ projectDir: APP, publish: 'never', dir, config });
  // what was made, measured
  const dist = path.join(APP, 'dist');
  for (const f of out || []) {
    if (fs.existsSync(f) && fs.statSync(f).isFile()) console.log(`dist: ${path.relative(APP, f)}  ${MB(fs.statSync(f).size)}`);
  }
  for (const u of ['win-unpacked', 'linux-unpacked']) {
    const d = path.join(dist, u);
    if (!fs.existsSync(d)) continue;
    const s = dirStats(d);
    console.log(`dist: ${path.relative(APP, d)}  ${s.files} files, ${MB(s.bytes)}`);
    const tc = path.join(d, 'resources', 'toolchain');
    if (tools) {
      for (const need of ['manifest.json', path.join('recomp', '.n64rx-key')]) {
        if (!fs.existsSync(path.join(tc, need))) {
          console.error(`dist: ${path.relative(APP, path.join(tc, need))} is missing from the build.`);
          process.exit(1);
        }
      }
      console.log(`dist: resources/toolchain is in (key ${JSON.parse(fs.readFileSync(path.join(tc, 'manifest.json'), 'utf8')).key})`);
    } else if (fs.existsSync(tc)) {
      console.error('dist: --no-tools, yet resources/toolchain is in the build.');
      process.exit(1);
    }
  }
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
