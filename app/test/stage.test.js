'use strict';
// scripts/stage-src.js: the source set carries nothing a ROM made, checked by
// tools/xbox/rom_leak_check.py's own rules.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const paths = require('../src/paths');
const stage = require('../scripts/stage-src');

const noPython = !paths.findSystemPython();

test('the leak check flags ROM header words, images, generated code and ROM extensions', { skip: noPython }, () => {
  const d = fs.mkdtempSync(path.join(os.tmpdir(), 'n64rx-stage-'));
  const put = (rel, data) => {
    fs.mkdirSync(path.dirname(path.join(d, rel)), { recursive: true });
    fs.writeFileSync(path.join(d, rel), data);
  };
  put('xbox/src/ok.c', 'int main(void) { return 0; }\n');
  put('xbox/assets/blob.bin', Buffer.concat([Buffer.from('abcd'), Buffer.from([0x80, 0x37, 0x12, 0x40])]));
  put('games/x/frame.png', Buffer.alloc(8));
  put('games/x/funcs_0.c', 'void f(uint8_t* rdram, recomp_context* ctx) {}\n');
  put('games/x/save.eep', Buffer.alloc(8));
  const bad = stage.leakCheck(d).map(([rel]) => rel).sort();
  assert.deepEqual(bad, ['games/x/frame.png', 'games/x/funcs_0.c', 'games/x/save.eep', 'xbox/assets/blob.bin']);
});

test('the staged set is only tracked sources: no tests, docs, outputs or the recompiler source', () => {
  const files = stage.selected();
  assert.ok(files.includes('tools/n64rx/n64rx.py'));
  assert.ok(files.includes('xbox/build.sh'));
  for (const f of files) {
    assert.doesNotMatch(f, /^(docs|tests|out|build)|^tools\/recomp\/|^tools\/xbox\/test_/, f);
    assert.doesNotMatch(f, /\.(z64|n64|v64|eep|xbe|iso)$/i, f);
  }
});
