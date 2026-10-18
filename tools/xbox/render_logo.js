#!/usr/bin/env node
// N64Recomp-X's mark, "Constellation X" (maintainer, 2026-10-18): stars and
// fine lines forming an X round a gold core star, on the loading screen's
// navy and nebula. The source is docs/look/logo/mark.svg (original art, no
// console or game marks); this renders the PNGs the build and the app use:
//
//   xbox/assets/logo.png   256x256: the platform build's icon and any game
//                          pack's without an icon.png of its own
//                          (tools/xbox/xbe_sections.py makes the XBE's
//                          128x128 DXT1 $$XTIMAGE and default.tbn from it)
//   app/build/icon.png     1024x1024: the desktop app's icon
//
//   node tools/xbox/render_logo.js
//
// Needs @resvg/resvg-js (the app's devDependency: cd app && npm install).
const fs = require('fs');
const path = require('path');
const { createRequire } = require('module');

const root = path.resolve(__dirname, '../..');
let Resvg;
try {
  ({ Resvg } = createRequire(path.join(root, 'app/package.json'))('@resvg/resvg-js'));
} catch {
  console.error('render_logo.js needs @resvg/resvg-js: run npm install in app/ first.');
  process.exit(1);
}

const svg = fs.readFileSync(path.join(root, 'docs/look/logo/mark.svg'));
for (const [out, size] of [['xbox/assets/logo.png', 256], ['app/build/icon.png', 1024]]) {
  const dest = path.join(root, out);
  fs.mkdirSync(path.dirname(dest), { recursive: true });
  fs.writeFileSync(dest, new Resvg(svg, { fitTo: { mode: 'width', value: size } }).render().asPng());
  console.log(`wrote ${out} (${size}x${size})`);
}
