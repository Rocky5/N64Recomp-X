'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const V = require('../src/validate');

const tmp = () => fs.mkdtempSync(path.join(os.tmpdir(), 'n64rx-val-'));

test('host: IPv4, IPv6 and host names pass; anything else is refused', () => {
  for (const h of ['192.168.1.50', '10.0.0.1', 'xbox', 'xbox.local', 'my-xbox.lan', 'fe80::1', '::1']) assert.equal(V.host(h), h);
  assert.equal(V.host(' 192.168.1.50 '), '192.168.1.50');
  for (const h of ['', '256.1.1.1', '1.2.3', 'a b', 'x;rm -rf', '-oProxyCommand', 'host\nname', 'ftp://x', '..', 'a..b', '-x.local']) {
    assert.throws(() => V.host(h), V.InputError, h);
  }
  assert.throws(() => V.host(42), V.InputError);
});

test('xboxPath: a plain absolute path on the console', () => {
  for (const p of ['/E/Games', '/F/Games/N64', '/E/My Games']) assert.equal(V.xboxPath(p), p);
  for (const p of ['E:\\Games', 'Games', '/E/../C', '/E/Games;x', '/E/Ga\nmes', '']) assert.throws(() => V.xboxPath(p), V.InputError, p);
});

test('ftpField: no control characters, bounded', () => {
  assert.equal(V.ftpField('-xbox', 'user'), '-xbox');
  assert.equal(V.ftpField('', 'pass'), '');
  assert.throws(() => V.ftpField('a\rb', 'user'));
  assert.throws(() => V.ftpField('x'.repeat(200), 'user'));
  assert.throws(() => V.ftpField(null, 'user'));
});

test('buildOptions: the ROM must exist; unknown options fall back to safe values', () => {
  const d = tmp();
  const rom = path.join(d, 'test.z64');
  fs.writeFileSync(rom, Buffer.alloc(16));
  const out = path.join(d, 'out');
  assert.deepEqual(V.buildOptions({ rom, out, video: '720', fast: true, romBeside: 'yes', learn: null }),
    { rom, out, video: '720', fast: true, romBeside: false, learn: null });
  assert.equal(V.buildOptions({ rom, out, video: '1080' }).video, 'auto');
  assert.throws(() => V.buildOptions({ rom: path.join(d, 'gone.z64'), out }), /No ROM/);
  assert.throws(() => V.buildOptions({ rom: 'relative.z64', out }), /absolute/);
  assert.throws(() => V.buildOptions({ rom, out: 'relative/out' }), /absolute/);
  assert.throws(() => V.buildOptions({ rom, out, learn: path.join(d, 'nope.txt') }), /learn/);
  assert.throws(() => V.buildOptions({ rom: d, out }), /not a file/);
  assert.throws(() => V.buildOptions(null), /Bad build options/);
});

test('buildOptions: an ISO build takes an absolute .iso path instead of a folder', () => {
  const d = tmp();
  const rom = path.join(d, 'test.z64');
  fs.writeFileSync(rom, Buffer.alloc(16));
  const iso = path.join(d, 'Game.iso');
  assert.equal(V.buildOptions({ rom, iso }).iso, iso);
  assert.equal(V.buildOptions({ rom, iso }).out, undefined);
  assert.throws(() => V.buildOptions({ rom, iso: path.join(d, 'Game.img') }), /\.iso/);
  assert.throws(() => V.buildOptions({ rom, iso: 'Game.iso' }), /absolute/);
});

test('fatxName: the ISO default name is FATX-safe like the folder name', () => {
  assert.equal(V.fatxName('Pilotwings 64'), 'Pilotwings 64');
  assert.equal(V.fatxName('Cruis\'n: USA?'), 'Cruis\'n_ USA_');
  assert.equal(V.fatxName(' .x. '), 'x');
  assert.equal(V.fatxName(''), 'game');
  assert.equal(V.fatxName('A'.repeat(60)).length, 42);
});
