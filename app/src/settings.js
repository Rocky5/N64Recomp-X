'use strict';
// The app's settings (settings.json in Electron's userData, inside the data
// folder). Every value is checked on the way in, from the renderer and from
// the file alike; the file is written whole (a temp file, then a rename).
// The FTP password is kept only encrypted by the OS (Electron safeStorage);
// where that is not available it is remembered for this session only.
const fs = require('node:fs');
const path = require('node:path');
const { xboxPath } = require('./validate');

const DEFAULTS = Object.freeze({
  outDir: null, isoDir: null, output: 'xbe', video: 'auto', fast: false, romBeside: false, toolchain: 'auto',
  xbox: Object.freeze({ host: '', user: 'xbox', pass: 'xbox', to: '/E/Games' }),
});

const plain = (v, max) => typeof v === 'string' && v.length < max && !/[\0\r\n]/.test(v);

/** patch's valid fields applied to s (s is changed); anything else is ignored */
function apply(s, patch) {
  if (!patch || typeof patch !== 'object') return s;
  if (plain(patch.outDir, 1024) && path.isAbsolute(patch.outDir)) s.outDir = patch.outDir;
  if (plain(patch.isoDir, 1024) && path.isAbsolute(patch.isoDir)) s.isoDir = patch.isoDir;
  if (['xbe', 'iso'].includes(patch.output)) s.output = patch.output;
  if (['auto', '480', '720'].includes(patch.video)) s.video = patch.video;
  if (typeof patch.fast === 'boolean') s.fast = patch.fast;
  if (typeof patch.romBeside === 'boolean') s.romBeside = patch.romBeside;
  if (['auto', 'system'].includes(patch.toolchain)) s.toolchain = patch.toolchain;
  const x = patch.xbox;
  if (x && typeof x === 'object') {
    if (plain(x.host, 254)) s.xbox.host = x.host.trim();
    if (plain(x.user, 128)) s.xbox.user = x.user;
    if (plain(x.pass, 128)) s.xbox.pass = x.pass;
    if (typeof x.to === 'string') {
      try {
        s.xbox.to = xboxPath(x.to);
      } catch { /* keep the old one */ }
    }
  }
  return s;
}

/**
 * crypt: {available(), encrypt(string) -> Buffer, decrypt(Buffer) -> string}
 * (Electron's safeStorage), or null.
 */
function createSettings({ file, crypt = null }) {
  let s = null;
  const canCrypt = () => {
    try {
      return !!(crypt && crypt.available());
    } catch {
      return false;
    }
  };

  function load() {
    if (s) return s;
    s = { ...DEFAULTS, xbox: { ...DEFAULTS.xbox } };
    let raw = {};
    try {
      raw = JSON.parse(fs.readFileSync(file, 'utf8'));
    } catch { /* first run, or a damaged file: the defaults */ }
    if (raw && typeof raw === 'object') {
      const { xbox = {}, ...rest } = raw;
      const x = xbox && typeof xbox === 'object' ? { host: xbox.host, user: xbox.user, to: xbox.to } : {};
      if (typeof xbox.passEnc === 'string' && canCrypt()) {
        try {
          x.pass = crypt.decrypt(Buffer.from(xbox.passEnc, 'base64'));
        } catch { /* another machine's: the default */ }
      }
      apply(s, { ...rest, xbox: x });
    }
    return s;
  }

  function save() {
    const out = { ...s, xbox: { host: s.xbox.host, user: s.xbox.user, to: s.xbox.to } };
    if (canCrypt() && s.xbox.pass !== DEFAULTS.xbox.pass) {
      try {
        out.xbox.passEnc = crypt.encrypt(s.xbox.pass).toString('base64');
      } catch { /* not kept */ }
    }
    try {
      fs.mkdirSync(path.dirname(file), { recursive: true });
      const tmp = `${file}.tmp-${process.pid}`;
      fs.writeFileSync(tmp, JSON.stringify(out, null, 2));
      fs.renameSync(tmp, file);
    } catch { /* not fatal: the settings live on for this session */ }
  }

  return {
    get() {
      const v = load();
      return { ...v, xbox: { ...v.xbox }, passwordSaved: canCrypt() };
    },
    set(patch) {
      apply(load(), patch);
      save();
      return this.get();
    },
  };
}

module.exports = { DEFAULTS, apply, createSettings };
