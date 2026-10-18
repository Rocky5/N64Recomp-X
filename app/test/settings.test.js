'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { createSettings, apply, DEFAULTS } = require('../src/settings');

const tmp = () => fs.mkdtempSync(path.join(os.tmpdir(), 'n64rx-set-'));
const crypt = { available: () => true, encrypt: (t) => Buffer.from(`enc:${t}`), decrypt: (b) => b.toString().replace(/^enc:/, '') };
const ABS = path.resolve(os.tmpdir(), 'N64Recomp-X');

test('a hand-edited or damaged settings file is checked like the renderer input', () => {
  const file = path.join(tmp(), 'settings.json');
  fs.writeFileSync(file, JSON.stringify({ outDir: 'relative/dir', video: '1080', fast: 'yes', toolchain: 'docker', xbox: { host: 'a\nb', user: 7, to: '/E/../x', pass: 'plain' } }));
  const s = createSettings({ file }).get();
  assert.equal(s.outDir, null, 'a relative folder is dropped: the picker asks');
  assert.equal(s.video, 'auto');
  assert.equal(s.fast, false);
  assert.equal(s.toolchain, 'auto');
  assert.deepEqual({ host: s.xbox.host, user: s.xbox.user, to: s.xbox.to }, { host: '', user: 'xbox', to: '/E/Games' });
  assert.equal(s.xbox.pass, DEFAULTS.xbox.pass, 'a plaintext password in the file is not read');
  fs.writeFileSync(file, '{not json');
  assert.equal(createSettings({ file }).get().video, 'auto');
});

test('the file is written whole (temp + rename) and never holds the password in plain text', () => {
  const dir = tmp();
  const file = path.join(dir, 'settings.json');
  const st = createSettings({ file, crypt });
  st.set({ video: '720', xbox: { host: '192.168.1.50', user: 'me', pass: 's3cret!' } });
  const raw = fs.readFileSync(file, 'utf8');
  assert.ok(!raw.includes('s3cret!'));
  assert.deepEqual(fs.readdirSync(dir), ['settings.json']);
  const again = createSettings({ file, crypt }).get();
  assert.equal(again.xbox.pass, 's3cret!');
  assert.equal(again.passwordSaved, true);
  assert.equal(again.video, '720');
});

test('without OS encryption the password is kept for the session only', () => {
  const file = path.join(tmp(), 'settings.json');
  const st = createSettings({ file, crypt: { available: () => false } });
  const s = st.set({ xbox: { host: 'xbox.local', pass: 'hunter2' } });
  assert.equal(s.xbox.pass, 'hunter2');
  assert.equal(s.passwordSaved, false);
  assert.ok(!fs.readFileSync(file, 'utf8').includes('hunter2'));
  assert.equal(createSettings({ file }).get().xbox.host, 'xbox.local');
  assert.equal(createSettings({ file }).get().xbox.pass, 'xbox');
});

test('apply ignores what is not valid', () => {
  const s = { ...DEFAULTS, xbox: { ...DEFAULTS.xbox } };
  apply(s, { outDir: ABS, video: '480', romBeside: true, xbox: { to: 'no-slash' } });
  assert.equal(s.outDir, ABS);
  assert.equal(s.video, '480');
  assert.equal(s.romBeside, true);
  assert.equal(s.xbox.to, '/E/Games');
  apply(s, null);
  apply(s, 'x');
  assert.equal(s.video, '480');
});
