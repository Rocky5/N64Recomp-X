// The screens: intro, build tools, home, game card, building, done, error.
// Everything that touches the disk or a process goes through window.n64rx
// (preload.js); this file only draws and reacts.
(function () {
  'use strict';
  const api = window.n64rx;
  const $ = (id) => document.getElementById(id);
  const el = (tag, cls, text) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  };
  const reduce = window.matchMedia('(prefers-reduced-motion: reduce)');
  document.body.classList.add(`platform-${api.platform}`);
  icons.fill();

  const state = {
    screen: null, rom: null, info: null, settings: null, learn: null,
    folder: null, buildStart: 0, buildSecs: 0, job: null, step: 0, timer: 0,
  };

  // ---------------------------------------------------------------- screens
  // one transition at a time: a show() during another's fade-out cancels it, so a
  // late timer can never put an older screen back over a newer one
  let pending = null;
  function show(name) {
    if (pending) {
      clearTimeout(pending);
      pending = null;
    }
    const next = document.querySelector(`.screen[data-screen="${name}"]`);
    const prev = document.querySelector(`.screen[data-screen="${state.screen}"]`);
    const same = state.screen === name;
    state.screen = name;
    document.body.dataset.screen = name;
    $('foot').style.opacity = name === 'home' || name === 'tools' ? '1' : '0';
    const enter = () => {
      pending = null;
      document.querySelectorAll('.screen').forEach((s) => {
        if (s === next) return;
        s.hidden = true;
        s.classList.remove('leave', 'enter');
      });
      if (same && !next.hidden && !next.classList.contains('leave')) return;
      next.classList.remove('leave');
      next.hidden = false;
      next.classList.remove('enter');
      void next.offsetWidth;
      next.classList.add('enter');
      $('stage').scrollTop = 0;
    };
    if (!same && prev && !prev.hidden && !reduce.matches) {
      prev.classList.remove('enter');
      prev.classList.add('leave');
      pending = setTimeout(enter, 170);
    } else enter();
  }

  let toastTimer = 0;
  function toast(text) {
    const t = $('toast');
    t.textContent = text;
    t.hidden = false;
    t.style.animation = 'none';
    void t.offsetWidth;
    t.style.animation = '';
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { t.hidden = true; }, 3200);
  }

  const mb = (b) => (b >= 1024 * 1024 * 1024 ? `${(b / 1073741824).toFixed(1)} GB` : `${Math.round(b / 1048576)} MB`);
  function dur(s) {
    s = Math.max(0, Math.round(s));
    const m = Math.floor(s / 60);
    return `${m}:${String(s % 60).padStart(2, '0')}`;
  }
  function words(s) {
    s = Math.round(s);
    if (s < 60) return `${s} s`;
    const m = Math.floor(s / 60);
    return `${m} min${s % 60 ? ` ${s % 60} s` : ''}`;
  }
  /** a long path, its end kept: "…\N64Recomp-X\Pilotwings 64" (the full path is the tooltip) */
  function shortPath(p, n) {
    p = String(p || '');
    return p.length <= n ? p : `…${p.slice(p.length - n + 1)}`;
  }
  const cap = (s) => (s ? s[0].toUpperCase() + s.slice(1) : s);
  function sentence(s) {
    s = cap(String(s || '').trim());
    return s && !/[.!?]$/.test(s) ? `${s}.` : s;
  }

  // ---------------------------------------------------------------- tier marks
  let uid = 0;
  function starPoints(cx, cy, R, r) {
    const pts = [];
    for (let i = 0; i < 10; i++) {
      const a = -Math.PI / 2 + (i * Math.PI) / 5;
      const rad = i % 2 ? r : R;
      pts.push([cx + Math.cos(a) * rad, cy + Math.sin(a) * rad]);
    }
    return pts;
  }
  function tierMark(tier) {
    const id = `t${++uid}`;
    if (tier === 'Gold' || tier === 'Silver') {
      const gold = tier === 'Gold';
      const stops = gold
        ? [['0', '#fff6d6'], ['0.28', '#ffd98a'], ['0.5', '#ffc86e'], ['0.78', '#c8862c'], ['1', '#ffe6a8']]
        : [['0', '#ffffff'], ['0.3', '#e3e9f5'], ['0.52', '#c8d2e6'], ['0.8', '#808ca6'], ['1', '#eef2fa']];
      const glow = gold ? '255,200,110' : '200,214,240';
      const pts = starPoints(28, 29.5, 22, 9.4);
      const d = `M${pts.map((p) => p.map((v) => v.toFixed(2)).join(' ')).join('L')}Z`;
      let facets = '';
      for (let i = 0; i < 10; i += 2) {
        const o = pts[i], l = pts[(i + 9) % 10], r = pts[(i + 1) % 10];
        facets += `<path d="M28 29.5L${o[0].toFixed(2)} ${o[1].toFixed(2)}L${l[0].toFixed(2)} ${l[1].toFixed(2)}Z" fill="rgba(255,255,255,0.22)"/>`;
        facets += `<path d="M28 29.5L${o[0].toFixed(2)} ${o[1].toFixed(2)}L${r[0].toFixed(2)} ${r[1].toFixed(2)}Z" fill="rgba(40,20,0,${gold ? 0.16 : 0.12})"/>`;
      }
      return `<svg viewBox="0 0 56 56" aria-hidden="true"><defs>
        <radialGradient id="${id}g"><stop offset="0" stop-color="rgba(${glow},0.55)"/><stop offset="0.5" stop-color="rgba(${glow},0.16)"/><stop offset="1" stop-color="rgba(${glow},0)"/></radialGradient>
        <linearGradient id="${id}m" x1="0.1" y1="0" x2="0.9" y2="1">${stops.map(([o, c]) => `<stop offset="${o}" stop-color="${c}"/>`).join('')}</linearGradient>
        <linearGradient id="${id}s" x1="0" x2="1"><stop offset="0" stop-color="rgba(255,255,255,0)"/><stop offset="0.5" stop-color="rgba(255,255,255,0.9)"/><stop offset="1" stop-color="rgba(255,255,255,0)"/></linearGradient>
        <clipPath id="${id}c"><path d="${d}"/></clipPath></defs>
        <circle cx="28" cy="29" r="30" fill="url(#${id}g)" class="star-glow"/>
        <path d="${d}" fill="url(#${id}m)"/>${facets}
        <g clip-path="url(#${id}c)"><g transform="skewX(-22)"><rect class="sheen" x="0" y="0" width="14" height="60" fill="url(#${id}s)"/></g></g>
        <path d="${d}" fill="none" stroke="rgba(255,250,235,${gold ? 0.75 : 0.85})" stroke-width="0.8" stroke-linejoin="round"/>
      </svg>`;
    }
    // Experimental: a dim comet with a faint trail
    return `<svg viewBox="0 0 56 56" aria-hidden="true"><defs>
      <linearGradient id="${id}t" x1="4" y1="52" x2="38" y2="18" gradientUnits="userSpaceOnUse"><stop offset="0" stop-color="rgba(154,165,192,0)"/><stop offset="1" stop-color="rgba(170,182,210,0.55)"/></linearGradient>
      <radialGradient id="${id}h"><stop offset="0" stop-color="#eef2fb"/><stop offset="0.35" stop-color="#b4bed6"/><stop offset="1" stop-color="rgba(154,165,192,0)"/></radialGradient></defs>
      <path class="comet-trail" d="M6 52 L33.5 15.5 Q38 13 40.5 17.5 Z" fill="url(#${id}t)"/>
      <path class="comet-trail" d="M14 50 L35 19" stroke="rgba(190,200,225,0.25)" stroke-width="0.8" stroke-linecap="round"/>
      <circle cx="38" cy="18" r="11" fill="url(#${id}h)" opacity="0.55"/>
      <circle cx="38" cy="18" r="4.2" fill="#c3cbe0"/>
      <circle cx="36.8" cy="16.8" r="1.6" fill="#f4f7ff"/>
    </svg>`;
  }
  const tierClass = (t) => (t === 'Gold' ? 'gold' : t === 'Silver' ? 'silver' : 'exp');

  // ---------------------------------------------------------------- intro
  function runIntro() {
    const intro = $('intro');
    // twinkles over the mark's own stars (renderer/mark-stars.js, read from mark.svg)
    const mark = $('intro-mark');
    (window.MARK_STARS || []).forEach((st, i) => {
      const t = el('span', `tw${st.r >= 12 ? ' core' : ''}`);
      t.style.left = `${(st.x / 512) * 100}%`;
      t.style.top = `${(st.y / 512) * 100}%`;
      t.style.setProperty('--s', `${Math.round(st.r * 3.4)}px`);
      t.style.animationDelay = `${300 + ((i * 377) % 1500)}ms`;
      mark.appendChild(t);
    });
    let gone = false;
    const end = () => {
      if (gone) return;
      gone = true;
      intro.classList.add('gone');
      document.body.classList.remove('intro-on');
      setTimeout(() => galaxy.shoot(false), 400);
    };
    intro.addEventListener('click', end);
    window.addEventListener('keydown', end, { once: true });
    setTimeout(end, reduce.matches ? 200 : 2500);
    return new Promise((res) => {
      const t = setInterval(() => {
        if (gone) {
          clearInterval(t);
          res();
        }
      }, 50);
    });
  }

  // ---------------------------------------------------------------- build tools
  const PHASE_TEXT = { space: 'Checking free space', download: 'Downloading', verify: 'Checking the download', unpack: 'Unpacking', ready: 'Ready' };
  function toolsScreen(st) {
    api.data.info().then((d) => {
      if (d && d.dir) {
        $('tools-where-path').textContent = shortPath(d.dir, 64);
        document.querySelector('.where-note').hidden = !d.portable;
        $('tools-where-path').title = d.dir;
      }
    });
    $('tools-size').textContent = st.size ? `About ${mb(st.size)}, once` : 'Downloaded once';
    $('tools-error').hidden = true;
    $('tools-progress').hidden = true;
    $('tools-go').disabled = false;
    if (!st.available) {
      toolsError({ message: 'No build tools download is set for this version of the app on this system yet.', hint: 'If this computer already has the toolchain (n64rx doctor), use it with the link below. Otherwise get a newer release of the app.' });
      $('tools-go').disabled = true;
      $('tools-go').querySelector('span').textContent = 'Download build tools';
    }
    show('tools');
  }
  function toolsError(e) {
    $('tools-error').hidden = false;
    $('tools-error-msg').textContent = e.message;
    $('tools-error-hint').textContent = e.hint || '';
    $('tools-go').querySelector('span').textContent = 'Try again';
  }
  api.onToolchainProgress((p) => {
    $('tools-progress').hidden = false;
    $('tools-phase').textContent = PHASE_TEXT[p.phase] || p.phase;
    const order = ['space', 'download', 'verify', 'unpack', 'ready'];
    const k = order.indexOf(p.phase);
    document.querySelectorAll('#tools-phases li').forEach((li) => {
      const i = order.indexOf(li.dataset.phase);
      li.classList.toggle('done', i < k);
      li.classList.toggle('now', i === k);
    });
    const bar = $('tools-bar').parentElement;
    if (p.phase === 'download' && p.total) {
      bar.classList.remove('indet');
      $('tools-bar').style.width = `${Math.min(100, (p.received / p.total) * 100).toFixed(1)}%`;
      $('tools-amount').textContent = `${mb(p.received)} of ${mb(p.total)}`;
      $('tools-speed').textContent = p.speed ? `${(p.speed / 1048576).toFixed(1)} MB/s` : '';
      $('tools-eta').textContent = p.eta != null ? `about ${words(p.eta)} left` : '';
    } else if (p.phase === 'ready') {
      bar.classList.remove('indet');
      $('tools-bar').style.width = '100%';
      $('tools-amount').textContent = '';
      $('tools-speed').textContent = '';
      $('tools-eta').textContent = '';
    } else {
      bar.classList.add('indet');
      $('tools-amount').textContent = p.phase === 'download' && p.received ? mb(p.received) : '';
      $('tools-speed').textContent = '';
      $('tools-eta').textContent = p.phase === 'unpack' ? 'This takes a minute' : '';
    }
  });
  $('tools-go').addEventListener('click', async () => {
    $('tools-error').hidden = true;
    $('tools-go').hidden = true;
    $('tools-cancel').hidden = false;
    $('tools-system').hidden = true;
    const r = await api.toolchain.install();
    $('tools-go').hidden = false;
    $('tools-cancel').hidden = true;
    $('tools-system').hidden = false;
    if (r && r.ok) {
      galaxy.flash();
      toast('Build tools ready');
      setTimeout(() => show('home'), 500);
    } else if (r && r.cancelled) {
      $('tools-progress').hidden = true;
      $('tools-go').querySelector('span').textContent = 'Continue download';
    } else toolsError((r && r.error) || { message: 'The download stopped.', hint: 'Press Try again.' });
  });
  $('tools-cancel').addEventListener('click', () => api.cancel());
  $('tools-system').addEventListener('click', async () => {
    await api.toolchain.useSystem();
    show('home');
  });

  // ---------------------------------------------------------------- home
  const drop = $('drop');
  let dragDepth = 0;
  function wellAtDrop(on) {
    const r = drop.getBoundingClientRect();
    galaxy.well(on, (r.left + r.width / 2) / window.innerWidth, (r.top + r.height * 0.32) / window.innerHeight);
  }
  const accepting = () => state.screen === 'home' || state.screen === 'game';
  window.addEventListener('dragenter', (e) => {
    e.preventDefault();
    if (!accepting()) return;
    if (++dragDepth === 1) {
      if (state.screen === 'game') show('home');
      drop.classList.add('over');
      wellAtDrop(true);
    }
  });
  window.addEventListener('dragleave', (e) => {
    e.preventDefault();
    if (--dragDepth <= 0) {
      dragDepth = 0;
      drop.classList.remove('over');
      galaxy.well(false);
    }
  });
  window.addEventListener('dragover', (e) => {
    e.preventDefault();
    if (e.dataTransfer) e.dataTransfer.dropEffect = accepting() ? 'copy' : 'none';
  });
  window.addEventListener('drop', (e) => {
    e.preventDefault();
    dragDepth = 0;
    drop.classList.remove('over');
    galaxy.well(false);
    if (!accepting()) return;
    const f = e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files[0];
    const p = f ? api.pathForFile(f) : null;
    if (p) loadRom(p);
  });
  async function chooseRom() {
    const p = await api.pickRom();
    if (p) loadRom(p);
  }
  $('choose-rom').addEventListener('click', (e) => {
    e.stopPropagation();
    chooseRom();
  });
  drop.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' || e.key === ' ') {
      e.preventDefault();
      chooseRom();
    }
  });

  function homeError(e) {
    $('home-error').hidden = !e;
    if (e) {
      $('home-error-msg').textContent = e.message;
      $('home-error-hint').textContent = e.hint || '';
    }
  }

  async function loadRom(p) {
    if (state.screen !== 'home') show('home');
    homeError(null);
    $('drop-busy').hidden = false;
    galaxy.pace(3);
    const t0 = performance.now();
    let r;
    try {
      r = await api.info(p);
    } finally {
      // keep the reading state up long enough to read as a moment, not a flicker
      await new Promise((res) => setTimeout(res, Math.max(0, 450 - (performance.now() - t0))));
      $('drop-busy').hidden = true;
      galaxy.pace(1);
    }
    if (!r || !r.ok) {
      homeError((r && r.error) || { message: 'This ROM could not be read.', hint: null });
      return;
    }
    state.rom = p;
    state.info = r.info;
    state.learn = null;
    renderGame();
    show('game');
  }

  // ---------------------------------------------------------------- the game card
  function splitName(name) {
    const m = /^(.*?)\s*\(([^()]*)\)\s*$/.exec(name || '');
    return m ? [m[1], m[2]] : [name, null];
  }
  const SEVERE = /stops the game|crash|does not start|won't start|cannot start|black screen|freez/i;

  function gameName() {
    const i = state.info;
    return splitName(i.compat.name || i.rom.title)[0] || i.rom.title;
  }

  function renderGame() {
    const { rom, compat, estimate } = state.info;
    const [title, sub] = splitName(compat.name || rom.title);
    const t = $('g-title');
    t.textContent = title;
    t.title = sub ? `${title} (${sub})` : title;
    const meta = $('g-meta');
    meta.textContent = '';
    for (const s of [rom.region, rom.revision ? `v${rom.revision}` : null, rom.size ? mb(rom.size) : null, rom.order ? `${rom.order} byte order` : null]) {
      if (s) meta.appendChild(el('span', '', s));
    }
    if (rom.code) meta.appendChild(el('span', 'code', rom.code));

    const tier = compat.tier || 'Experimental';
    const tc = tierClass(tier);
    $('g-tier').className = `tier ${tc}`;
    $('g-tier-mark').innerHTML = tierMark(tier);
    $('g-tier-name').textContent = tier.toUpperCase();
    $('g-tier-meaning').textContent = tier === 'Experimental' && !compat.name ? 'Not on the tested list yet.' : sentence(compat.meaning);

    const ul = $('g-problems');
    ul.textContent = '';
    const issues = compat.issues || [];
    if (tier === 'Gold' && !issues.length) {
      const li = el('li', 'ok');
      li.insertAdjacentHTML('afterbegin', icons.svg('check'));
      li.appendChild(el('span', '', 'No known problems'));
      ul.appendChild(li);
    } else if (issues.length) {
      for (const text of issues) {
        const severe = SEVERE.test(text);
        const li = el('li', severe ? 'severe' : '');
        li.appendChild(el('span', `dot${severe ? ' severe' : ''}`));
        li.appendChild(el('span', '', text));
        if (severe) li.appendChild(el('span', 'tag', 'Game-breaking'));
        ul.appendChild(li);
      }
    } else {
      const li = el('li', 'unknown');
      li.insertAdjacentHTML('afterbegin', icons.svg('info'));
      li.appendChild(el('span', '', compat.name ? 'No problems are listed for it yet.' : 'Not proven on a real Xbox yet: it may run, run in part, or not at all.'));
      ul.appendChild(li);
    }
    $('g-problems-label').textContent = tier === 'Experimental' && !compat.name ? 'What to expect' : 'Known problems';
    $('g-other').hidden = !compat.other_version;
    $('g-other-text').textContent = compat.other_version || '';
    $('g-estimate').textContent = estimate ? `Takes ${estimate}` : '';
    renderOptions();
  }

  // the Advanced drawer
  function renderOptions() {
    const s = state.settings;
    document.querySelectorAll('#opt-video button').forEach((b) => b.setAttribute('aria-checked', String(b.dataset.v === s.video)));
    $('opt-fast').setAttribute('aria-checked', String(!!s.fast));
    $('opt-beside').setAttribute('aria-checked', String(!!s.romBeside));
    const iso = s.output === 'iso';
    document.querySelectorAll('#g-output button').forEach((b) => b.setAttribute('aria-checked', String(b.dataset.v === (iso ? 'iso' : 'xbe'))));
    const where = iso ? s.isoDir : s.outDir;
    $('g-saves').textContent = iso ? (where ? `Disc image saves to ${shortPath(where, 52)}` : 'You name the disc image when you build')
      : (where ? `Saves to ${shortPath(where, 60)}` : 'You choose where it is saved when you build');
    $('g-saves').title = where || '';
    $('g-saves-change').textContent = where ? 'Change' : 'Choose now';
    $('g-saves-change').hidden = iso;   // an ISO's name and place are chosen together, at Build
    $('g-saves-sep').hidden = iso;
    $('g-build').querySelector('span').textContent = iso ? 'Build disc image' : 'Build for Xbox';
    $('g-saves').classList.toggle('is-path', !!where);
    $('opt-learn-name').textContent = state.learn ? state.learn.split(/[\\/]/).pop() : 'Code the Xbox ran but the build missed. Optional.';
    $('opt-learn-clear').hidden = !state.learn;
    const bits = [s.video === 'auto' ? 'Auto video' : s.video === '480' ? '480 video' : '720p video'];
    if (s.fast) bits.push('quick build');
    if (s.romBeside) bits.push('ROM beside');
    if (state.learn) bits.push('learn.txt');
    $('adv-summary').textContent = `· ${bits.join(', ')}`;
  }
  async function setOpt(patch) {
    state.settings = await api.settings.set(patch);
    renderOptions();
  }
  $('adv-toggle').addEventListener('click', () => {
    const open = $('adv').hidden;
    $('adv').hidden = !open;
    $('adv-toggle').setAttribute('aria-expanded', String(open));
    if (open) setTimeout(() => $('adv').scrollIntoView({ behavior: reduce.matches ? 'auto' : 'smooth', block: 'nearest' }), 50);
  });
  document.querySelectorAll('#opt-video button').forEach((b) => b.addEventListener('click', () => setOpt({ video: b.dataset.v })));
  $('opt-fast').addEventListener('click', () => setOpt({ fast: !state.settings.fast }));
  $('opt-beside').addEventListener('click', () => setOpt({ romBeside: !state.settings.romBeside }));
  $('opt-learn').addEventListener('click', async () => {
    const p = await api.pickLearn();
    if (p) {
      state.learn = p;
      renderOptions();
    }
  });
  $('opt-learn-clear').addEventListener('click', () => {
    state.learn = null;
    renderOptions();
  });
  /** the native folder picker (in main): the folder, or null when cancelled or refused */
  async function pickOut() {
    $('g-out-error').hidden = true;
    const r = await api.pickOut();
    if (r && r.error) {
      $('g-out-error').hidden = false;
      $('g-out-error-msg').textContent = r.error.message;
      $('g-out-error-hint').textContent = r.error.hint || '';
      return null;
    }
    if (!r) return null;
    state.settings = await api.settings.get();
    renderOptions();
    return r;
  }
  $('g-saves-change').addEventListener('click', () => pickOut());
  document.querySelectorAll('#g-output button').forEach((b) => b.addEventListener('click', () => setOpt({ output: b.dataset.v })));
  async function pickIso() {
    $('g-out-error').hidden = true;
    const r = await api.pickIso(gameName());
    if (r && r.error) {
      $('g-out-error').hidden = false;
      $('g-out-error-msg').textContent = r.error.message;
      $('g-out-error-hint').textContent = r.error.hint || '';
      return null;
    }
    if (!r) return null;
    state.settings = await api.settings.get();
    renderOptions();
    return r;
  }
  $('g-back').addEventListener('click', () => show('home'));
  $('g-build').addEventListener('click', async () => {
    // always asked, by the dialog that matches the output; cancel = no build
    if (state.settings.output === 'iso') {
      const iso = await pickIso();
      if (iso) startBuild({ iso });
    } else {
      const out = await pickOut();
      if (out) startBuild({ out });
    }
  });

  // ---------------------------------------------------------------- building
  const LABELS = ['ROM', 'RECOMP', 'COMPILE', 'XBE', 'FOLDER', 'DISC'];
  const HEIGHTS = [116, 58, 104, 46, 94, 62];
  let STARS = [];
  function layoutStars(n) {
    STARS = LABELS.slice(0, n).map((label, i) => ({ x: Math.round(56 + (i * 528) / (n - 1)), y: HEIGHTS[i], label }));
  }
  // the mark's four-point star (docs/look/logo/mark.svg), at the constellation's scale
  const SPARK = 'M0 -15Q2.5 -2.5 15 0Q2.5 2.5 0 15Q-2.5 2.5 -15 0Q-2.5 -2.5 0 -15Z';
  function buildConstellation(n = 5) {
    layoutStars(n);
    let s = `<svg viewBox="0 0 640 184" preserveAspectRatio="xMidYMid meet" aria-hidden="true"><defs>
      <filter id="c-blur" x="-20%" y="-200%" width="140%" height="500%"><feGaussianBlur stdDeviation="3"/></filter>
      <radialGradient id="c-bloom-g"><stop offset="0" stop-color="rgba(159,240,255,0.9)"/><stop offset="1" stop-color="rgba(159,240,255,0)"/></radialGradient>
      <radialGradient id="c-gold-g"><stop offset="0" stop-color="rgba(255,255,255,0.95)"/><stop offset="0.25" stop-color="rgba(255,200,110,0.6)"/><stop offset="1" stop-color="rgba(255,200,110,0)"/></radialGradient>
      <radialGradient id="c-green-g"><stop offset="0" stop-color="rgba(182,255,168,0.9)"/><stop offset="1" stop-color="rgba(120,200,110,0)"/></radialGradient></defs>`;
    for (let i = 0; i < STARS.length - 1; i++) {
      const a = STARS[i], b = STARS[i + 1];
      s += `<line class="c-line-base" x1="${a.x}" y1="${a.y}" x2="${b.x}" y2="${b.y}"/>`;
    }
    for (let i = 0; i < STARS.length - 1; i++) {
      const a = STARS[i], b = STARS[i + 1];
      const len = Math.hypot(b.x - a.x, b.y - a.y);
      for (const cls of ['c-line-glow', 'c-line']) {
        s += `<line class="${cls}" data-i="${i}" x1="${a.x}" y1="${a.y}" x2="${b.x}" y2="${b.y}" stroke-dasharray="${len}" stroke-dashoffset="${len}"/>`;
      }
    }
    STARS.forEach((p, i) => {
      s += `<g class="c-star" data-i="${i}" transform="translate(${p.x} ${p.y})">
        <circle class="c-bloom" r="22" fill="url(#c-bloom-g)"/>
        <circle class="c-gold" r="30" fill="url(#c-gold-g)"/>
        <circle class="c-green" r="30" fill="url(#c-green-g)"/>
        <circle class="c-ring" r="10"/>
        <path class="c-spark" d="${SPARK}"/>
        <circle class="c-core" r="3.2"/>
        <text y="${p.y < 80 ? 42 : 40}">${p.label}</text></g>`;
    });
    s += '</svg>';
    $('b-const').innerHTML = s;
    $('b-const').classList.remove('success');
  }
  function lightTo(n, fail) {
    // n: the current step (1-based); before it lit, it pulsing, lines drawn up to it
    document.querySelectorAll('#b-const .c-star').forEach((g) => {
      const i = Number(g.dataset.i) + 1;
      const was = g.classList.contains('now') || g.classList.contains('lit');
      g.classList.toggle('lit', i < n);
      g.classList.toggle('now', i === n && !fail);
      g.classList.toggle('fail', i === n && !!fail);
      if (i === n && !was && !fail) {
        g.classList.remove('ignite');
        void g.getBBox();
        g.classList.add('ignite');
        setTimeout(() => g.classList.remove('ignite'), 750);
      }
    });
    document.querySelectorAll('#b-const .c-line, #b-const .c-line-glow').forEach((l) => {
      const i = Number(l.dataset.i) + 1;
      if (i < n) l.setAttribute('stroke-dashoffset', '0');
      else l.setAttribute('stroke-dashoffset', l.getAttribute('stroke-dasharray'));
    });
  }
  function allLit() {
    document.querySelectorAll('#b-const .c-star').forEach((g) => {
      g.classList.remove('now');
      g.classList.add('lit');
    });
    document.querySelectorAll('#b-const .c-line, #b-const .c-line-glow').forEach((l) => l.setAttribute('stroke-dashoffset', '0'));
    $('b-const').classList.add('success');
  }

  // the Details log, batched to one DOM write a frame
  let logQueue = [];
  let logRaf = 0;
  function logLine(text, cls) {
    logQueue.push([text, cls]);
    if (!logRaf) {
      logRaf = requestAnimationFrame(() => {
        logRaf = 0;
        const box = $('b-log');
        const stick = box.scrollTop + box.clientHeight >= box.scrollHeight - 24;
        const frag = document.createDocumentFragment();
        for (const [t, c] of logQueue) {
          const s = el('span', c, t + '\n');
          frag.appendChild(s);
        }
        logQueue = [];
        box.appendChild(frag);
        while (box.childNodes.length > 2000) box.removeChild(box.firstChild);
        if (stick) box.scrollTop = box.scrollHeight;
      });
    }
  }
  $('b-details-toggle').addEventListener('click', () => {
    const open = $('b-log').hidden;
    $('b-log').hidden = !open;
    $('b-details-toggle').setAttribute('aria-expanded', String(open));
    $('b-details-toggle').closest('.card').classList.toggle('details-open', open);
    if (open) $('b-log').scrollTop = $('b-log').scrollHeight;
  });

  api.onEvent(({ kind, ev }) => {
    if (kind === 'build') onBuildEvent(ev);
    else if (kind === 'deploy') onDeployEvent(ev);
  });
  function onBuildEvent(ev) {
    switch (ev.ev) {
      case 'step':
        state.step = ev.n;
        lightTo(ev.n);
        $('b-step').innerHTML = '';
        if (ev.of && ev.of !== STARS.length) buildConstellation(ev.of);
        $('b-step').appendChild(el('span', 'step-count', `Step ${ev.n} of ${ev.of || STARS.length}`));
        $('b-step').appendChild(document.createTextNode(ev.text));
        $('b-note').innerHTML = '&nbsp;';
        logLine(`[${ev.n}/${ev.of || 5}] ${ev.text}`, 'l-step');
        break;
      case 'note':
        $('b-note').textContent = ev.text;
        logLine(`      ${ev.text}`, 'l-note');
        break;
      case 'done':
        logLine(`      done in ${ev.secs} s`, 'l-note');
        break;
      case 'error':
        logLine(`n64rx: ${ev.message}${ev.hint ? `\nWhat to do: ${ev.hint}` : ''}`, 'l-err');
        break;
      case 'log':
        logLine(ev.text, ev.stream === 'stderr' ? 'l-raw' : 'l-raw');
        break;
      default:
        break;
    }
  }

  function armCancel(on) {
    const b = $('b-cancel');
    b.classList.toggle('armed', on);
    b.querySelector('span').textContent = on ? 'Stop the build?' : 'Cancel';
  }
  let cancelArm = 0;
  $('b-cancel').addEventListener('click', () => {
    if (!$('b-cancel').classList.contains('armed')) {
      armCancel(true);
      clearTimeout(cancelArm);
      cancelArm = setTimeout(() => armCancel(false), 3500);
      return;
    }
    clearTimeout(cancelArm);
    $('b-cancel').disabled = true;
    $('b-cancel').querySelector('span').textContent = 'Stopping…';
    api.cancel();
  });

  async function startBuild(dest) {
    const s = state.settings;
    state.step = 0;
    buildConstellation(dest.iso ? 6 : 5);
    $('b-title').textContent = gameName();
    $('b-eyebrow').textContent = 'Building';
    $('b-step').innerHTML = '<span class="step-count">Starting</span>';
    $('b-note').innerHTML = '&nbsp;';
    $('b-log').textContent = '';
    $('b-cancel').disabled = false;
    $('b-cancel').hidden = false;
    armCancel(false);
    $('b-estimate').textContent = state.info.estimate ? `Usually ${state.info.estimate}` : '';
    state.buildStart = Date.now();
    $('b-elapsed').textContent = '0:00';
    clearInterval(state.timer);
    state.timer = setInterval(() => { $('b-elapsed').textContent = dur((Date.now() - state.buildStart) / 1000); }, 1000);
    show('build');
    galaxy.pace(1.8);
    const r = await api.build({ rom: state.rom, ...dest, video: s.video, fast: !!s.fast, romBeside: !!s.romBeside, learn: state.learn });
    clearInterval(state.timer);
    galaxy.pace(1);
    state.buildSecs = (Date.now() - state.buildStart) / 1000;
    if (r && r.cancelled) {
      toast('Build cancelled');
      show('game');
      return;
    }
    if (!r || r.error || r.code !== 0 || !r.result || !(r.result.folder || r.result.file)) {
      lightTo(state.step || 1, true);
      const e = (r && r.error) || { message: 'The build stopped without saying why.', hint: 'Open the log folder for the details.' };
      setTimeout(() => showError('Build stopped', e), r && r.id ? 700 : 0);
      return;
    }
    allLit();
    $('b-cancel').hidden = true;
    galaxy.flash();
    galaxy.shoot(true);
    $('b-eyebrow').textContent = 'Built';
    $('b-step').innerHTML = '';
    $('b-step').appendChild(el('span', 'step-count', 'Done'));
    $('b-step').appendChild(document.createTextNode(`in ${words(state.buildSecs)}`));
    $('b-note').innerHTML = '&nbsp;';
    state.folder = r.result.folder || null;
    state.isoFile = r.result.folder ? null : r.result.file;
    setTimeout(() => showDone(r.result), reduce.matches ? 0 : 1300);
  }

  // ---------------------------------------------------------------- done
  function showDone(result) {
    const tier = state.info.compat.tier || 'Experimental';
    $('d-sub').textContent = '';
    $('d-sub').appendChild(el('span', '', gameName()));
    $('d-sub').appendChild(el('span', '', `built in ${words(state.buildSecs)}`));
    const mini = $('d-tier');
    mini.className = `tier-mini ${tierClass(tier)}`;
    mini.innerHTML = tierMark(tier);
    mini.appendChild(el('span', 'tier-name pixel', tier.toUpperCase()));
    mini.title = result.status || '';
    const isoDone = !result.folder && !!result.file;
    const shown = isoDone ? result.file : result.folder;
    $('d-folder').textContent = shortPath(shown, 54);
    $('d-folder').title = shown;
    $('d-folder-label').textContent = isoDone ? 'Disc image' : 'Folder';
    $('d-show').textContent = isoDone ? 'Show file' : 'Show folder';
    $('d-hint-folder').hidden = isoDone;
    $('d-hint-iso').hidden = !isoDone;
    $('d-send').hidden = isoDone;
    $('d-send').closest('.tiles').classList.toggle('single', isoDone);
    $('d-folder').closest('.folder-box').querySelector(':scope > .ico').outerHTML = icons.svg(isoDone ? 'disc' : 'folder');
    $('send-form').hidden = true;
    $('d-send').setAttribute('aria-pressed', 'false');
    $('x-status').textContent = '';
    $('x-status').className = 'send-status';
    const x = state.settings.xbox || {};
    $('x-host').value = x.host || '';
    $('x-user').value = x.user || 'xbox';
    $('x-pass').value = x.pass ?? 'xbox';
    $('x-to').value = x.to || '/E/Games';
    $('x-pass-note').hidden = !!state.settings.passwordSaved;
    doneIdle();
    show('done');
  }
  $('d-show').addEventListener('click', () => api.showFolder(state.isoFile || state.folder));
  $('d-again').addEventListener('click', () => show('home'));
  $('d-send').addEventListener('click', () => {
    const open = $('send-form').hidden;
    $('send-form').hidden = !open;
    $('d-send').setAttribute('aria-pressed', String(open));
    if (open) {
      setTimeout(() => {
        $('send-form').scrollIntoView({ behavior: reduce.matches ? 'auto' : 'smooth', block: 'nearest' });
        if (!$('x-host').value) $('x-host').focus();
      }, 60);
    }
  });
  // while Send runs: the other actions off, a Cancel (an ISO is chosen before the build, on the game card)
  function doneBusy(text) {
    for (const id of ['d-send', 'd-again', 'x-go']) $(id).disabled = true;
    $('d-job-text').textContent = text;
    $('d-job-cancel').disabled = false;
    $('d-job').hidden = false;
  }
  function doneIdle() {
    for (const id of ['d-send', 'd-again', 'x-go']) $(id).disabled = false;
    $('d-job').hidden = true;
  }
  $('d-job-cancel').addEventListener('click', () => {
    $('d-job-cancel').disabled = true;
    $('d-job-text').textContent = 'Stopping…';
    api.cancel();
  });
  function onDeployEvent(ev) {
    const st = $('x-status');
    if (ev.ev === 'note') st.textContent = `Sent ${ev.text}`;
    else if (ev.ev === 'step') st.textContent = ev.text;
  }
  $('send-form').addEventListener('submit', async (e) => {
    e.preventDefault();
    const st = $('x-status');
    st.className = 'send-status';
    st.textContent = 'Connecting…';
    doneBusy('Sending to the Xbox…');
    const r = await api.deploy(state.folder, { host: $('x-host').value.trim(), user: $('x-user').value, pass: $('x-pass').value, to: $('x-to').value.trim() || '/E/Games' });
    doneIdle();
    state.settings = await api.settings.get();
    if (r && r.cancelled) {
      st.className = 'send-status';
      st.textContent = 'Sending stopped.';
    } else if (r && !r.error && r.code === 0) {
      st.className = 'send-status ok';
      st.textContent = r.result && r.result.dest ? `On the Xbox: ${r.result.dest}` : 'Sent.';
      galaxy.shoot(false);
    } else {
      const er = (r && r.error) || { message: 'Sending stopped.' };
      st.className = 'send-status err';
      st.textContent = `${er.message}${er.hint ? ` ${er.hint}` : ''}`;
    }
  });

  // ---------------------------------------------------------------- error
  let errorBack = 'game';
  function showError(title, e, back) {
    errorBack = back || (state.info ? 'game' : 'home');
    $('e-eyebrow').textContent = title;
    $('e-msg').textContent = e.message || 'Something went wrong.';
    const hint = e.hint || 'Open the log folder for the details.';
    $('e-hint').textContent = hint;
    $('e-hint').classList.toggle('mono', /\n/.test(hint));
    $('e-retry').querySelector('span').textContent = errorBack === 'done' ? 'Back' : 'Try again';
    const lines = Array.from($('b-log').childNodes).slice(-60).map((n) => n.textContent).join('');
    $('e-log').textContent = lines;
    $('e-log').hidden = true;
    $('e-details-toggle').hidden = !lines.trim() || back === 'done';
    $('e-details-toggle').setAttribute('aria-expanded', 'false');
    show('error');
  }
  $('e-details-toggle').addEventListener('click', () => {
    const open = $('e-log').hidden;
    $('e-log').hidden = !open;
    $('e-details-toggle').setAttribute('aria-expanded', String(open));
    if (open) $('e-log').scrollTop = $('e-log').scrollHeight;
  });
  $('e-copy').addEventListener('click', async () => toast((await api.copyLog()) ? 'Log copied' : 'No log yet'));
  $('e-logs').addEventListener('click', () => api.openLogDir());
  $('e-home').addEventListener('click', () => show('home'));
  $('e-retry').addEventListener('click', () => show(errorBack));

  // ---------------------------------------------------------------- menu and about
  const menu = $('menu');
  function menuOpen(on) {
    menu.hidden = !on;
    $('menu-btn').setAttribute('aria-expanded', String(on));
  }
  $('menu-btn').addEventListener('click', (e) => {
    e.stopPropagation();
    menuOpen(menu.hidden);
  });
  document.addEventListener('click', (e) => {
    if (!menu.hidden && !menu.contains(e.target)) menuOpen(false);
  });
  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') {
      menuOpen(false);
      $('about').hidden = true;
      $('settings').hidden = true;
    }
  });
  menu.addEventListener('click', async (e) => {
    const b = e.target.closest('button');
    if (!b) return;
    menuOpen(false);
    if (b.dataset.act === 'about') {
      const a = await api.about();
      $('about-ver').textContent = `Version ${a.version}${a.packaged ? '' : ' (development)'}${a.fake ? ', test mode' : ''} · ${a.platform} ${a.arch} · Electron ${a.electron}`;
      $('about-data').textContent = a.data || '';
      $('about').hidden = false;
    } else if (b.dataset.act === 'logs') api.openLogDir();
    else if (b.dataset.act === 'settings') openSettings();
  });
  // data and storage: one folder holds everything
  async function openSettings() {
    $('settings').hidden = false;
    $('s-tools').textContent = '…';
    $('s-builds').textContent = '…';
    const d = await api.data.info();
    if (!d || d.error) return;
    $('s-dir').textContent = shortPath(d.dir, 44);
    $('s-dir').title = d.dir;
    $('s-note').textContent = d.note ? `${d.note} So the data lives in your user folder instead.` : 'Everything the app keeps is in this one folder. Delete it to remove everything.';
    $('s-note').classList.toggle('warn', !!d.note);
    $('s-tools').textContent = d.toolchain ? mb(d.toolchain) : 'None';
    $('s-builds').textContent = d.builds ? mb(d.builds) : 'Empty';
    $('s-rm-tools').disabled = !d.toolchain;
    $('s-rm-builds').disabled = !d.builds;
    $('s-out').textContent = (state.settings && state.settings.outDir) || 'asked when you build';
  }
  async function removeData(what) {
    const r = await api.data.remove(what);
    if (r && r.error) {
      toast(r.error.message);
      return;
    }
    if (!r || !r.removed) return;
    toast(`${what === 'toolchain' ? 'Build tools removed' : 'Build cache removed'}${r.freed ? `, ${mb(r.freed)} freed` : ''}`);
    await openSettings();
    if (what === 'toolchain' && r.status && r.status.state === 'missing') {
      $('settings').hidden = true;
      toolsScreen(r.status);
    }
  }
  $('s-open').addEventListener('click', () => api.data.open());
  $('s-rm-tools').addEventListener('click', () => removeData('toolchain'));
  $('s-rm-builds').addEventListener('click', () => removeData('builds'));
  $('settings-close').addEventListener('click', () => { $('settings').hidden = true; });
  $('settings').addEventListener('click', (e) => {
    if (e.target === $('settings')) $('settings').hidden = true;
  });

  $('about-close').addEventListener('click', () => { $('about').hidden = true; });
  $('about').addEventListener('click', (e) => {
    if (e.target === $('about')) $('about').hidden = true;
  });
  $('about-repo').addEventListener('click', () => api.openRepo());
  $('about-lic').addEventListener('click', () => api.openLicense());

  // ---------------------------------------------------------------- start
  (async function boot() {
    const [settings, st] = await Promise.all([api.settings.get(), api.toolchain.status(), runIntro()]);
    state.settings = settings;
    if (st && st.state === 'missing') toolsScreen(st);
    else show('home');
  }());

  // for the screenshot and smoke scripts (test/): load a ROM without a file dialog
  window.__n64rxTest = { loadRom, state, show };
}());
