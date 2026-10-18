'use strict';
// Screenshots of every screen with the fake n64rx, for design review and
// docs/look/: node scripts/shots.js <out dir> <test ROM> [only]
// The ROM must be a test ROM (tools/xbox/make_test_rom.py), never a game.
const path = require('node:path');
const fs = require('node:fs');
const os = require('node:os');
const { _electron: electron } = require('playwright-core');

const APP = path.resolve(__dirname, '..');
const [outDir, rom, only] = process.argv.slice(2);
if (!outDir || !rom) {
  console.error('usage: node scripts/shots.js <out dir> <test ROM> [name]');
  process.exit(1);
}
fs.mkdirSync(outDir, { recursive: true });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function launch(env) {
  const user = fs.mkdtempSync(path.join(os.tmpdir(), 'n64rx-shot-'));
  const app = await electron.launch({
    executablePath: require('electron'), args: [APP], cwd: APP,
    env: { ...process.env, N64RX_FAKE: '1', N64RX_USER_DATA: user, N64RX_CACHE: path.join(user, 'cache'), N64RX_TEST_PICK_OUT: path.join(os.homedir(), 'Documents', 'N64Recomp-X'), ...env },
  });
  const page = await app.firstWindow();
  const errors = [];
  page.on('console', (m) => { if (m.type() === 'error' || m.type() === 'warning') errors.push(m.text()); });
  page.on('pageerror', (e) => errors.push(String(e)));
  await page.setViewportSize({ width: 980, height: 660 }).catch(() => {});
  return { app, page, errors };
}

async function shot(page, name) {
  const file = path.join(outDir, `${name}.png`);
  await page.screenshot({ path: file });
  console.log(file);
}

async function afterIntro(page) {
  await page.waitForSelector('#intro.gone', { state: 'attached', timeout: 10000 });
  await sleep(700);
}

async function gameCard(tier, extraEnv, name, fn) {
  const { app, page, errors } = await launch({ N64RX_FAKE_TIER: tier, ...extraEnv });
  try {
    await afterIntro(page);
    await page.evaluate((r) => window.__n64rxTest.loadRom(r), rom);
    await page.waitForSelector('[data-screen="game"]:not([hidden])');
    await sleep(900);
    await shot(page, name);
    if (fn) await fn(page);
  } finally {
    if (errors.length) console.log('console:', errors.join('\n'));
    await app.close();
  }
}

(async () => {
  const want = (n) => !only || only === n;
  if (want('intro') || want('home')) {
    const { app, page, errors } = await launch({});
    await sleep(1000);
    if (want('intro')) await shot(page, 'intro');
    await sleep(1150);
    if (want('intro')) await shot(page, 'intro-word');
    await afterIntro(page);
    await page.mouse.move(700, 200);
    await sleep(600);
    if (want('home')) await shot(page, 'home');
    if (want('home')) {
      await page.click('#menu-btn');
      await sleep(300);
      await shot(page, 'menu');
      await page.click('[data-act="about"]');
      await sleep(500);
      await shot(page, 'about');
      await page.click('#about-close');
      await page.click('#menu-btn');
      await sleep(250);
      await page.click('[data-act="settings"]');
      await sleep(900);
      await shot(page, 'settings');
    }
    if (errors.length) console.log('console:', errors.join('\n'));
    await app.close();
  }
  if (want('tools')) {
    const { app, page } = await launch({ N64RX_FAKE_TOOLCHAIN: 'missing', N64RX_FAKE_TICK: '120' });
    await afterIntro(page);
    await shot(page, 'tools');
    await page.click('#tools-go');
    await sleep(2600);
    await shot(page, 'tools-progress');
    await app.close();
  }
  if (want('gold')) {
    await gameCard('Gold', { N64RX_FAKE_STEP_MS: '1400' }, 'game-gold', async (page) => {
      await page.click('#g-build');
      await page.waitForFunction(() => window.__n64rxTest.state.step >= 3);
      await sleep(900);
      await shot(page, 'building');
      await page.click('#b-details-toggle');
      await sleep(400);
      await shot(page, 'building-details');
      await page.click('#b-details-toggle');
      await page.waitForSelector('#b-const.success', { timeout: 20000 });
      await sleep(250);
      await shot(page, 'building-success');
      await page.waitForSelector('[data-screen="done"]:not([hidden])', { timeout: 20000 });
      await sleep(800);
      await shot(page, 'done');
      await page.click('#d-send');
      await sleep(600);
      await shot(page, 'done-send');
      await page.fill('#x-host', '192.168.1.50');
      await page.click('#x-go');
      await sleep(450);
      await shot(page, 'done-sending');
    });
  }
  if (want('silver')) await gameCard('Silver', {}, 'game-silver');
  if (want('exp')) {
    await gameCard('Experimental', { N64RX_FAKE_STEP_MS: '1200', N64RX_TEST_PICK_ISO: path.join(os.tmpdir(), 'Mario Party.iso') }, 'game-experimental-named', async (page) => {
      await page.click('#g-output [data-v="iso"]');
      await sleep(400);
      await shot(page, 'game-iso-choice');
      await page.click('#g-build');
      await page.waitForFunction(() => window.__n64rxTest.state.step >= 5);
      await sleep(700);
      await shot(page, 'building-iso');
      await page.waitForSelector('[data-screen="done"]:not([hidden])', { timeout: 30000 });
      await sleep(800);
      await shot(page, 'done-iso');
    });
  }
  if (want('unknown')) {
    await gameCard('Unknown', { N64RX_FAKE_OTHER: 'Mario Kart 64 (US) (revision 1.0) is Silver; this version of the game has not been checked and starts Experimental.' }, 'game-experimental', async (page) => {
      await page.click('#adv-toggle');
      await sleep(600);
      await shot(page, 'game-advanced');
    });
  }
  if (want('error')) {
    await gameCard('Unknown', { N64RX_FAKE_FAIL: '3', N64RX_FAKE_STEP_MS: '500' }, 'game-for-error', async (page) => {
      await page.click('#g-build');
      await page.waitForSelector('[data-screen="error"]:not([hidden])', { timeout: 20000 });
      await sleep(700);
      await shot(page, 'error');
    });
  }
})().catch((e) => {
  console.error(e);
  process.exit(1);
});
