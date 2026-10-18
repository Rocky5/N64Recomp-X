'use strict';
// A stand-in for tools/n64rx/n64rx.py speaking its app contract (info
// --json, --events json build/deploy/xiso), so the UI and the tests run
// without Python or a toolchain. N64RX_FAKE=1 makes the app use it.
//   N64RX_FAKE_TIER   Gold | Silver | Experimental | Unknown (default: Unknown)
//   N64RX_FAKE_FAIL   the build step (1-5) that fails
//   N64RX_FAKE_STEP_MS  time a build step takes (default 900)
//   N64RX_FAKE_HOLD   1: stay on the current step until killed (screenshots, cancel tests)
const fs = require('node:fs');
const path = require('node:path');

const MEANING = {
  Gold: 'fully working on a real Xbox',
  Silver: 'playable on a real Xbox; it has bugs, none that stop the game',
  Experimental: 'not proven on a real Xbox yet: it may run, run in part, or not at all',
};
const ROWS = {
  Gold: { tier: 'Gold', name: 'Pilotwings 64 (US; its pack or the ROM alone)', issues: [] },
  Silver: { tier: 'Silver', name: 'Mario Kart 64 (US)', issues: ['The TV on the map screen shows the wrong picture.', 'Drift boosts may not behave exactly as on the N64.'] },
  Experimental: { tier: 'Experimental', name: 'Mario Party (US)', issues: ['The game board is black, which stops the game (a fix waits for a console test).', 'The title logo is wrong (a fix waits for a console test).'] },
  Unknown: { tier: 'Experimental', name: null, issues: [] },
};

const out = (o) => process.stdout.write(JSON.stringify(o) + '\n');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function header(file) {
  const b = fs.readFileSync(file);
  if (b.length < 0x40) throw Object.assign(new Error('This file is not an N64 ROM (its first four bytes are not an N64 header).'), { hint: 'Check that it is a .z64, .n64 or .v64 dump of a cartridge, not a zip or a save file.' });
  const m = b.readUInt32BE(0);
  const order = m === 0x80371240 ? 'z64' : m === 0x37804012 ? 'v64' : m === 0x40123780 ? 'n64' : null;
  if (!order) throw Object.assign(new Error('This file is not an N64 ROM (its first four bytes are not an N64 header).'), { hint: 'Check that it is a .z64, .n64 or .v64 dump of a cartridge, not a zip or a save file.' });
  const z = Buffer.from(b.subarray(0, 0x40));
  if (order === 'v64') for (let i = 0; i < z.length; i += 2) [z[i], z[i + 1]] = [z[i + 1], z[i]];
  if (order === 'n64') for (let i = 0; i < z.length; i += 4) z.writeUInt32BE(z.readUInt32LE(i), i);
  const title = z.subarray(0x20, 0x34).toString('latin1').replace(/[^\x20-\x7e]/g, ' ').trim().replace(/\s+/g, ' ');
  const code = z.subarray(0x3b, 0x3f).toString('latin1');
  return { title: title || `N64 game ${code}`, code, version: z[0x3f], order, size: b.length };
}

async function info(rom) {
  const h = header(rom);
  const row = ROWS[process.env.N64RX_FAKE_TIER] || ROWS.Unknown;
  const regions = { E: 'USA', J: 'Japan', P: 'Europe', D: 'Germany', F: 'France', U: 'Australia' };
  out({
    rom: { path: path.resolve(rom), title: h.title, code: h.code, region: regions[h.code[3]] || 'region ?', revision: `1.${h.version}`,
      size: h.size, order: h.order, sha1: '0'.repeat(40), cic: 6102 },
    pack: row.tier === 'Gold' ? { id: 'pw64', title: 'Pilotwings 64' } : null,
    compat: { tier: row.tier, meaning: MEANING[row.tier], name: row.name, issues: row.issues,
      other_version: process.env.N64RX_FAKE_OTHER || null },
    estimate: 'about 1 to 3 minutes',
  });
}

async function build(args) {
  const rom = args[0];
  const iso = args.includes('--iso') ? args[args.indexOf('--iso') + 1] : null;
  const outDir = iso ? fs.mkdtempSync(path.join(require('node:os').tmpdir(), 'fake-iso-')) : args[args.indexOf('--out') + 1];
  const ms = Number(process.env.N64RX_FAKE_STEP_MS || 900);
  const fail = Number(process.env.N64RX_FAKE_FAIL || 0);
  const steps = ['Reading the ROM', 'Recompiling the game\'s code (N64Recomp)', 'Compiling for the Xbox', 'Finishing the XBE', 'Writing the folder for the Xbox'];
  if (iso) steps.push('Making the disc image (ISO)');
  const notes = [['FAKE TEST ROM: 8 MB, z64 byte order', 'no pack: analysed from the ROM alone', '214 functions in 3 segments'],
    ['1 912 functions in 24 files, 64-bit registers'], ['fast build: -O1, no LTO', 'linking default.xbe'], ['title ID, icon, the ROM inside'], ['README.txt'], ['extract-xiso -c']];
  const h = header(rom);
  for (let i = 0; i < steps.length; i++) {
    const t0 = Date.now();
    out({ ev: 'step', n: i + 1, of: steps.length, text: steps[i] });
    for (const n of notes[i]) {
      await sleep(ms / 4);
      out({ ev: 'note', text: n });
      process.stdout.write(`[tool] ${n.replace(/ /g, '_')}.o\n`);   // a non-JSON line, as the tools print
    }
    if (fail === i + 1) {
      out({ ev: 'error', message: 'Compiling or linking the XBE failed.', hint: 'The log says why: see the details below, or open the log folder.' });
      process.exit(2);
    }
    if (process.env.N64RX_FAKE_HOLD === String(i + 1)) await new Promise(() => setInterval(() => {}, 1000));
    await sleep(ms / 2);
    out({ ev: 'done', secs: (Date.now() - t0) / 1000 });
  }
  const folder = path.join(outDir, h.title.replace(/[<>:"/\\|?*]/g, '_'));
  fs.mkdirSync(folder, { recursive: true });
  fs.writeFileSync(path.join(folder, 'default.xbe'), 'XBEH fake\n');
  fs.writeFileSync(path.join(folder, 'README.txt'), 'A fake build from the app\'s tests.\n');
  const row = ROWS[process.env.N64RX_FAKE_TIER] || ROWS.Unknown;
  if (iso) {
    fs.writeFileSync(iso, 'fake iso\n');
    fs.rmSync(outDir, { recursive: true, force: true });
    out({ ev: 'result', file: path.resolve(iso), status: `Status: ${row.tier}, ${MEANING[row.tier]} (docs/compat.md).` });
    return;
  }
  out({ ev: 'result', folder, status: `Status: ${row.tier}, ${MEANING[row.tier]} (docs/compat.md).` });
}

async function main() {
  let a = process.argv.slice(2);
  if (a[0] === '--events') a = a.slice(2);
  const [sub, ...rest] = a;
  try {
    if (sub === 'info') return await info(rest[0]);
    if (sub === 'build') return await build(rest);
    if (sub === 'deploy') {
      const opt = (k) => {
        const eq = rest.find((a) => a.startsWith(`--${k}=`));
        return eq ? eq.slice(k.length + 3) : rest[rest.indexOf(`--${k}`) + 1];
      };
      const host = opt('ftp');
      if (!process.env.N64RX_FTP_PASS && !rest.includes('--pass')) throw Object.assign(new Error('No FTP password.'), { hint: 'N64RX_FTP_PASS' });
      out({ ev: 'step', n: 1, of: 1, text: `Sending to ${host}` });
      for (const f of ['default.xbe', 'default.tbn', 'README.txt']) {
        await sleep(300);
        out({ ev: 'note', text: f });
      }
      out({ ev: 'result', dest: `${host}:/E/Games/${path.basename(rest[0])}` });
      return;
    }
    if (sub === 'xiso') {
      out({ ev: 'step', n: 1, of: 1, text: 'Writing the disc image' });
      await sleep(600);
      const file = path.resolve(rest[0]) + '.iso';
      fs.writeFileSync(file, 'fake iso\n');
      out({ ev: 'result', file });
      return;
    }
    throw Object.assign(new Error(`The fake n64rx does not know "${sub}".`), { hint: 'Use info, build, deploy or xiso.' });
  } catch (e) {
    if (a.includes('json') || process.argv.includes('--events')) out({ ev: 'error', message: e.message, hint: e.hint || null });
    else process.stderr.write(`\nn64rx: ${e.message}\nWhat to do: ${e.hint || ''}\n`);
    process.exit(2);
  }
}

main();
