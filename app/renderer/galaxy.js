// The sky behind the app: the port's loading-screen palette (navy
// rgb(14,22,52) -> rgb(6,10,28), teal and indigo bands, pale teal sparks)
// made deep: cached nebula layers drifting at their own depth, a faint
// galaxy band, a planet's limb at the bottom edge, a three-layer starfield
// with star colours and bloom, cursor parallax, rare shooting stars, and a
// gravity well while a file hovers. At most 30 frames a second, none while
// hidden, one still frame under prefers-reduced-motion.
(function () {
  'use strict';
  const TAU = Math.PI * 2;
  const FPS = 30;
  const canvas = document.getElementById('sky');
  const ctx = canvas.getContext('2d', { alpha: false });
  const reduce = window.matchMedia('(prefers-reduced-motion: reduce)');

  let W = 0, H = 0, DPR = 1;
  let layers = null;     // offscreen canvases
  let stars = [];
  let sparks = [];
  let shooters = [];
  let raf = 0, last = 0, t0 = performance.now();
  let nextShot = 8000 + Math.random() * 10000;
  const mouse = { x: 0, y: 0, tx: 0, ty: 0 };
  const well = { x: 0.5, y: 0.5, s: 0, target: 0 };
  let flash = 0;
  let pace = 1, paceTarget = 1;

  // a seeded generator, so the sky is the same every launch
  function rng(seed) {
    let s = seed >>> 0;
    return () => {
      s = (s + 0x6d2b79f5) >>> 0;
      let t = s;
      t = Math.imul(t ^ (t >>> 15), t | 1);
      t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
      return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
  }

  const TEMPS = [
    [0.42, [205, 222, 255]],   // blue-white
    [0.30, [248, 250, 255]],   // white
    [0.13, [255, 236, 205]],   // warm
    [0.10, [190, 240, 255]],   // teal
    [0.05, [255, 205, 165]],   // orange
  ];
  function temp(r) {
    let a = 0;
    for (const [p, c] of TEMPS) {
      a += p;
      if (r < a) return c;
    }
    return TEMPS[0][1];
  }

  function off(w, h) {
    const c = document.createElement('canvas');
    c.width = Math.max(1, Math.ceil(w));
    c.height = Math.max(1, Math.ceil(h));
    return c;
  }

  function blob(g, x, y, r, rgb, a, sx = 1, sy = 1, rot = 0) {
    g.save();
    g.translate(x, y);
    g.rotate(rot);
    g.scale(sx, sy);
    const gr = g.createRadialGradient(0, 0, 0, 0, 0, r);
    gr.addColorStop(0, `rgba(${rgb},${a})`);
    gr.addColorStop(0.45, `rgba(${rgb},${a * 0.45})`);
    gr.addColorStop(1, `rgba(${rgb},0)`);
    g.fillStyle = gr;
    g.fillRect(-r, -r, 2 * r, 2 * r);
    g.restore();
  }

  function dither(g, w, h, amount) {
    // fine noise over the gradients: no banding on 8-bit panels
    const img = g.getImageData(0, 0, w, h);
    const d = img.data;
    const r = rng(7);
    for (let i = 0; i < d.length; i += 4) {
      const n = (r() - 0.5) * amount;
      d[i] += n;
      d[i + 1] += n;
      d[i + 2] += n;
    }
    g.putImageData(img, 0, 0);
  }

  function buildLayers() {
    const S = 0.5;                       // nebula at half resolution: soft anyway
    const mw = W * 1.16, mh = H * 1.16;  // a margin to drift and parallax into
    const r = rng(1977);

    // base: the splash's navy gradient with a cool vignette
    const base = off(W * DPR, H * DPR);
    {
      const g = base.getContext('2d');
      const lg = g.createLinearGradient(0, 0, 0, base.height);
      lg.addColorStop(0, 'rgb(14,22,52)');
      lg.addColorStop(0.55, 'rgb(9,15,40)');
      lg.addColorStop(1, 'rgb(6,10,28)');
      g.fillStyle = lg;
      g.fillRect(0, 0, base.width, base.height);
      const vg = g.createRadialGradient(base.width * 0.5, base.height * 0.42, base.height * 0.2, base.width * 0.5, base.height * 0.5, base.width * 0.8);
      vg.addColorStop(0, 'rgba(0,0,0,0)');
      vg.addColorStop(1, 'rgba(2,3,12,0.55)');
      g.fillStyle = vg;
      g.fillRect(0, 0, base.width, base.height);
      dither(g, base.width, base.height, 3);
    }

    // nebula A: the big clouds (teal, indigo, a little magenta) and the aurora bands
    const nebA = off(mw * S, mh * S);
    {
      const g = nebA.getContext('2d');
      const w = nebA.width, h = nebA.height, u = Math.max(w, h);
      g.globalCompositeOperation = 'lighter';
      blob(g, w * 0.22, h * 0.18, u * 0.42, '30,120,150', 0.20, 1.6, 0.7, -0.45);
      blob(g, w * 0.70, h * 0.30, u * 0.36, '58,46,150', 0.20, 1.5, 0.8, -0.35);
      blob(g, w * 0.88, h * 0.70, u * 0.30, '120,40,130', 0.10, 1.3, 0.9, 0.3);
      blob(g, w * 0.12, h * 0.78, u * 0.32, '24,70,140', 0.16, 1.4, 0.8, 0.2);
      blob(g, w * 0.45, h * 0.55, u * 0.30, '20,90,120', 0.10, 2.0, 0.6, -0.5);
      // the splash's two diagonal teal bands
      for (let i = 0; i < 2; i++) {
        const bx = w * (0.18 + i * 0.5), by = h * (0.32 + i * 0.12);
        for (let k = 0; k < 5; k++) {
          blob(g, bx + k * w * 0.07, by - k * h * 0.06, u * 0.12, i ? '70,80,190' : '40,160,175', 0.08, 2.8, 0.42, -0.62);
        }
      }
      for (let i = 0; i < 26; i++) {
        const c = r() < 0.55 ? '40,150,170' : r() < 0.7 ? '70,60,170' : '140,60,150';
        blob(g, r() * w, r() * h, u * (0.04 + r() * 0.1), c, 0.04 + r() * 0.05, 1 + r(), 0.6 + r() * 0.6, r() * TAU);
      }
    }

    // nebula B: wisps and the galaxy band (a nearer, faster layer)
    const nebB = off(mw * S, mh * S);
    {
      const g = nebB.getContext('2d');
      const w = nebB.width, h = nebB.height, u = Math.max(w, h);
      g.globalCompositeOperation = 'lighter';
      const ang = -0.38;
      const cx = w * 0.5, cy = h * 0.46;
      // the band's glow
      blob(g, cx, cy, u * 0.55, '90,110,190', 0.07, 1.9, 0.16, ang);
      blob(g, cx - w * 0.12, cy + h * 0.05, u * 0.3, '160,150,200', 0.05, 2.2, 0.12, ang);
      // its dust lane: darker cuts through the glow
      g.globalCompositeOperation = 'source-over';
      for (let i = 0; i < 18; i++) {
        const t = r() * 2 - 1;
        const x = cx + Math.cos(ang) * t * u * 0.5, y = cy + Math.sin(ang) * t * u * 0.5 + (r() - 0.5) * h * 0.03;
        blob(g, x, y, u * (0.03 + r() * 0.05), '4,6,18', 0.25, 2.4, 0.35, ang);
      }
      g.globalCompositeOperation = 'lighter';
      for (let i = 0; i < 14; i++) {
        blob(g, r() * w, r() * h, u * (0.05 + r() * 0.08), r() < 0.6 ? '50,170,190' : '90,70,180', 0.035, 2.5, 0.3, -0.5 + r() * 0.4);
      }
    }

    // the band's own stars, at full resolution (in the half-resolution nebula they would be blocks)
    const band = off(mw * DPR, mh * DPR);
    {
      const g = band.getContext('2d');
      const w = band.width, h = band.height, u = Math.max(w, h);
      const ang = -0.38, cx = w * 0.5, cy = h * 0.46;
      for (let i = 0; i < Math.round((W * H) / 900); i++) {
        const t = r() * 2 - 1;
        const spread = (r() + r() + r() - 1.5) * h * 0.06;
        const x = cx + Math.cos(ang) * t * u * 0.55 - Math.sin(ang) * spread;
        const y = cy + Math.sin(ang) * t * u * 0.55 + Math.cos(ang) * spread;
        const c = temp(r());
        const k = 1 - Math.abs(t);
        g.fillStyle = `rgba(${c[0]},${c[1]},${c[2]},${(0.06 + r() * 0.3) * k})`;
        const sz = (r() < 0.9 ? 0.8 : 1.4) * DPR;
        g.fillRect(x, y, sz, sz);
      }
    }

    // the planet's limb: a dark world below the bottom edge, its air lit from the upper left
    const planet = off(W * DPR, H * 0.42 * DPR);
    {
      const g = planet.getContext('2d');
      const w = planet.width, h = planet.height;
      const R = w * 1.35;
      const cx = w * 0.62, cy = h + R - h * 0.3;
      // atmosphere
      const atm = g.createRadialGradient(cx, cy, R * 0.985, cx, cy, R * 1.06);
      atm.addColorStop(0, 'rgba(90,200,220,0.55)');
      atm.addColorStop(0.25, 'rgba(60,150,200,0.22)');
      atm.addColorStop(1, 'rgba(40,80,180,0)');
      g.fillStyle = atm;
      g.beginPath();
      g.arc(cx, cy, R * 1.06, 0, TAU);
      g.fill();
      // the body, with a terminator: the limb fades on the right
      g.fillStyle = 'rgb(3,6,16)';
      g.beginPath();
      g.arc(cx, cy, R, 0, TAU);
      g.fill();
      const lit = g.createLinearGradient(cx - R * 0.5, cy - R, cx + R * 0.3, cy - R * 0.9);
      lit.addColorStop(0, 'rgba(40,120,150,0.30)');
      lit.addColorStop(0.5, 'rgba(20,50,90,0.10)');
      lit.addColorStop(1, 'rgba(0,0,0,0)');
      g.fillStyle = lit;
      g.beginPath();
      g.arc(cx, cy, R, 0, TAU);
      g.fill();
      // the thin bright rim
      g.globalCompositeOperation = 'lighter';
      g.strokeStyle = 'rgba(150,230,240,0.35)';
      g.lineWidth = 1.2 * DPR;
      g.beginPath();
      g.arc(cx, cy, R, Math.PI * 1.32, Math.PI * 1.72);
      g.stroke();
      g.globalCompositeOperation = 'destination-out';
      const fade = g.createLinearGradient(0, 0, w, 0);
      fade.addColorStop(0, 'rgba(0,0,0,0.85)');
      fade.addColorStop(0.35, 'rgba(0,0,0,0)');
      fade.addColorStop(0.85, 'rgba(0,0,0,0)');
      fade.addColorStop(1, 'rgba(0,0,0,0.6)');
      g.fillStyle = fade;
      g.fillRect(0, 0, w, h);
    }

    // bloom sprites, one per star colour
    const sprites = TEMPS.map(([, c]) => {
      const s = off(32, 32);
      const g = s.getContext('2d');
      const gr = g.createRadialGradient(16, 16, 0, 16, 16, 16);
      gr.addColorStop(0, `rgba(255,255,255,1)`);
      gr.addColorStop(0.12, `rgba(${c[0]},${c[1]},${c[2]},0.9)`);
      gr.addColorStop(0.35, `rgba(${c[0]},${c[1]},${c[2]},0.22)`);
      gr.addColorStop(1, `rgba(${c[0]},${c[1]},${c[2]},0)`);
      g.fillStyle = gr;
      g.fillRect(0, 0, 32, 32);
      return s;
    });
    layers = { base, nebA, nebB, band, planet, sprites, mw, mh };
  }

  function buildStars() {
    const r = rng(42);
    const area = W * H;
    const spec = [
      { n: Math.round(area / 2600), depth: 0.15, size: [0.5, 1.1], alpha: [0.25, 0.7], drift: 1.2 },
      { n: Math.round(area / 9000), depth: 0.45, size: [0.9, 1.6], alpha: [0.45, 0.9], drift: 3.2 },
      { n: Math.round(area / 32000), depth: 1.0, size: [1.4, 2.4], alpha: [0.7, 1.0], drift: 7 },
    ];
    stars = [];
    spec.forEach((L, li) => {
      for (let i = 0; i < L.n; i++) {
        const ci = (() => {
          const v = r();
          let a = 0;
          for (let k = 0; k < TEMPS.length; k++) {
            a += TEMPS[k][0];
            if (v < a) return k;
          }
          return 0;
        })();
        stars.push({
          x: r(), y: r(), layer: li, depth: L.depth, drift: L.drift,
          size: L.size[0] + r() * (L.size[1] - L.size[0]),
          a: L.alpha[0] + r() * (L.alpha[1] - L.alpha[0]),
          tw: 0.4 + r() * 1.8, ph: r() * TAU, amp: li === 0 ? 0.35 : 0.22,
          c: TEMPS[ci][1], ci, spike: li === 2 && r() < 0.35,
        });
      }
    });
    sparks = [];
    for (let i = 0; i < 18; i++) {
      sparks.push({ x: r(), speed: 6 + r() * 14, ph: r(), wob: r() * TAU });
    }
  }

  function resize() {
    DPR = Math.min(window.devicePixelRatio || 1, 2);
    W = window.innerWidth;
    H = window.innerHeight;
    canvas.width = Math.round(W * DPR);
    canvas.height = Math.round(H * DPR);
    buildLayers();
    buildStars();
    if (reduce.matches) frame(performance.now(), true);
  }

  function drawStars(t, dt) {
    const g = ctx;
    const px = mouse.x, py = mouse.y;
    const wx = well.x * W, wy = well.y * H, ws = well.s;
    const boost = 1 + flash * 0.9;
    for (const s of stars) {
      const span = W + 40, spanY = H + 40;
      let x = ((s.x * span - (t * s.drift * pace) / 1000 + px * s.depth * 24) % span + span) % span - 20;
      let y = ((s.y * spanY - (t * s.drift * 0.25 * pace) / 1000 + py * s.depth * 16) % spanY + spanY) % spanY - 20;
      if (ws > 0.001) {
        // the gravity well: pulled in and swirled around the drop zone
        const dx = wx - x, dy = wy - y;
        const d = Math.hypot(dx, dy) + 1;
        const pull = ws * Math.min(0.55, 90 / d) * (0.5 + s.depth);
        const sw = ws * 0.9 * Math.exp(-d / 260);
        x += dx * pull * 0.35 - dy * sw * 0.25;
        y += dy * pull * 0.35 + dx * sw * 0.25;
      }
      const tw = 1 - s.amp + s.amp * Math.sin(s.ph + (t / 1000) * s.tw * TAU * 0.25);
      const a = Math.min(1, s.a * tw * boost);
      if (s.layer === 2) {
        const sz = s.size * 7 * (0.8 + 0.2 * tw) * (1 + flash * 0.5);
        g.globalAlpha = a;
        g.drawImage(layers.sprites[s.ci], (x - sz / 2) * DPR, (y - sz / 2) * DPR, sz * DPR, sz * DPR);
        if (s.spike) {
          g.globalAlpha = a * 0.35;
          g.fillStyle = `rgb(${s.c[0]},${s.c[1]},${s.c[2]})`;
          const L = sz * 1.3;
          g.fillRect((x - L / 2) * DPR, (y - 0.25) * DPR, L * DPR, 0.5 * DPR);
          g.fillRect((x - 0.25) * DPR, (y - L / 2) * DPR, 0.5 * DPR, L * DPR);
        }
      } else {
        g.globalAlpha = a;
        g.fillStyle = `rgb(${s.c[0]},${s.c[1]},${s.c[2]})`;
        const sz = s.size * DPR;
        g.fillRect(x * DPR - sz / 2, y * DPR - sz / 2, sz, sz);
      }
    }
    g.globalAlpha = 1;
  }

  function drawSparks(t) {
    // the loading screen's sparks: pale teal, rising, brightest mid-climb
    const g = ctx;
    g.globalCompositeOperation = 'lighter';
    for (const s of sparks) {
      const span = H + 40;
      const climb = ((t / 1000) * s.speed + s.ph * span) % span;
      const life = climb / span;
      const a = (life < 0.5 ? life : 1 - life) * 2 * 0.55;
      const x = s.x * W + Math.sin(t / 900 + s.wob) * 6;
      const y = H + 20 - climb;
      g.globalAlpha = a;
      g.drawImage(layers.sprites[3], (x - 4) * DPR, (y - 4) * DPR, 8 * DPR, 8 * DPR);
    }
    g.globalAlpha = 1;
    g.globalCompositeOperation = 'source-over';
  }

  function shoot(big) {
    const fromLeft = Math.random() < 0.6;
    const ang = (fromLeft ? 0.32 : Math.PI - 0.32) + (Math.random() - 0.5) * 0.25;
    shooters.push({
      x: big ? -0.05 * W : (fromLeft ? Math.random() * 0.5 : 0.5 + Math.random() * 0.5) * W,
      y: big ? H * 0.12 : Math.random() * H * 0.45,
      ang: big ? 0.24 : ang, speed: big ? 1100 : 760 + Math.random() * 300,
      len: big ? 380 : 120 + Math.random() * 90, width: big ? 2.6 : 1.3,
      life: big ? 1.7 : 0.75 + Math.random() * 0.3, age: 0, big,
      trail: [],
    });
  }

  function drawShooters(dt) {
    const g = ctx;
    g.globalCompositeOperation = 'lighter';
    for (const s of shooters) {
      s.age += dt / 1000;
      const k = s.age / s.life;
      const fade = k < 0.15 ? k / 0.15 : k > 0.7 ? Math.max(0, (1 - k) / 0.3) : 1;
      s.x += Math.cos(s.ang) * s.speed * (dt / 1000);
      s.y += Math.sin(s.ang) * s.speed * (dt / 1000);
      const tx = s.x - Math.cos(s.ang) * s.len, ty = s.y - Math.sin(s.ang) * s.len;
      const gr = g.createLinearGradient(tx * DPR, ty * DPR, s.x * DPR, s.y * DPR);
      const head = s.big ? '255,240,205' : '220,240,255';
      gr.addColorStop(0, `rgba(${head},0)`);
      gr.addColorStop(0.7, `rgba(${s.big ? '255,210,140' : '150,220,240'},${0.35 * fade})`);
      gr.addColorStop(1, `rgba(${head},${0.95 * fade})`);
      g.strokeStyle = gr;
      g.lineCap = 'round';
      g.lineWidth = s.width * DPR;
      g.beginPath();
      g.moveTo(tx * DPR, ty * DPR);
      g.lineTo(s.x * DPR, s.y * DPR);
      g.stroke();
      g.globalAlpha = fade;
      const hs = (s.big ? 34 : 14) * DPR;
      g.drawImage(layers.sprites[s.big ? 2 : 0], s.x * DPR - hs / 2, s.y * DPR - hs / 2, hs, hs);
      if (s.big) {
        if (Math.random() < 0.8) s.trail.push({ x: s.x - Math.cos(s.ang) * 20 + (Math.random() - 0.5) * 10, y: s.y - Math.sin(s.ang) * 20 + (Math.random() - 0.5) * 10, a: 1 });
        for (const p of s.trail) {
          p.a -= dt / 900;
          p.y += dt * 0.01;
          if (p.a <= 0) continue;
          g.globalAlpha = p.a * 0.8 * fade;
          g.drawImage(layers.sprites[2], (p.x - 3) * DPR, (p.y - 3) * DPR, 6 * DPR, 6 * DPR);
        }
        s.trail = s.trail.filter((p) => p.a > 0);
      }
      g.globalAlpha = 1;
    }
    g.globalCompositeOperation = 'source-over';
    shooters = shooters.filter((s) => s.age < s.life);
  }

  function frame(now, still) {
    const t = now - t0;
    const dt = Math.min(100, last ? now - last : 33);
    last = now;
    mouse.x += (mouse.tx - mouse.x) * 0.06;
    mouse.y += (mouse.ty - mouse.y) * 0.06;
    well.s += (well.target - well.s) * (well.target > well.s ? 0.12 : 0.08);
    flash *= 0.94;
    pace += (paceTarget - pace) * 0.05;

    const g = ctx;
    g.globalCompositeOperation = 'source-over';
    g.globalAlpha = 1;
    g.drawImage(layers.base, 0, 0);
    const L = layers;
    const drift = still ? 0 : t / 1000;
    // nebula A: slow, deep
    {
      const ox = (L.mw - W) / 2 + Math.sin(drift * 0.021) * W * 0.035 - mouse.x * 6;
      const oy = (L.mh - H) / 2 + Math.cos(drift * 0.017) * H * 0.03 - mouse.y * 4;
      g.globalCompositeOperation = 'lighter';
      g.globalAlpha = 0.95 + flash * 0.3;
      g.drawImage(L.nebA, ox * 0.5, oy * 0.5, W * 0.5, H * 0.5, 0, 0, canvas.width, canvas.height);
    }
    // nebula B: nearer, a little faster
    {
      const ox = (L.mw - W) / 2 + Math.sin(drift * 0.035 + 1.3) * W * 0.05 - mouse.x * 12;
      const oy = (L.mh - H) / 2 + Math.cos(drift * 0.029 + 0.4) * H * 0.04 - mouse.y * 8;
      g.globalAlpha = 0.9;
      g.drawImage(L.nebB, ox * 0.5, oy * 0.5, W * 0.5, H * 0.5, 0, 0, canvas.width, canvas.height);
      g.globalAlpha = 1;
      g.drawImage(L.band, ox * DPR, oy * DPR, W * DPR, H * DPR, 0, 0, canvas.width, canvas.height);
    }
    g.globalAlpha = 1;
    g.globalCompositeOperation = 'source-over';
    drawStars(still ? 0 : t, dt);
    if (!still) drawSparks(t);
    // the planet in front of the stars, moving with the nearest layer
    g.drawImage(L.planet, -mouse.x * 14 * DPR, canvas.height - L.planet.height + (8 - mouse.y * 6) * DPR);
    if (!still) {
      nextShot -= dt;
      if (nextShot <= 0) {
        shoot(false);
        nextShot = 14000 + Math.random() * 16000;
      }
      if (shooters.length) drawShooters(dt);
    }
  }

  function loop(now) {
    raf = requestAnimationFrame(loop);
    if (now - last < 1000 / FPS - 2) return;
    frame(now, false);
  }

  function start() {
    cancelAnimationFrame(raf);
    raf = 0;
    if (reduce.matches) {
      frame(performance.now(), true);
      return;
    }
    if (document.hidden) return;
    last = 0;
    raf = requestAnimationFrame(loop);
  }

  window.addEventListener('resize', () => {
    clearTimeout(resize.t);
    resize.t = setTimeout(resize, 60);
  });
  window.addEventListener('pointermove', (e) => {
    mouse.tx = (e.clientX / Math.max(1, W)) * 2 - 1;
    mouse.ty = (e.clientY / Math.max(1, H)) * 2 - 1;
  }, { passive: true });
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) {
      cancelAnimationFrame(raf);
      raf = 0;
    } else start();
  });
  reduce.addEventListener('change', start);

  resize();
  start();

  window.galaxy = {
    /** a gravity well under the drop zone while a file hovers (x, y in 0..1 of the window) */
    well(on, x, y) {
      if (typeof x === 'number') {
        well.x = x;
        well.y = y;
      }
      well.target = on ? 1 : 0;
    },
    shoot(big) {
      if (!reduce.matches && layers) shoot(!!big);
    },
    flash() {
      flash = 1;
    },
    pace(p) {
      paceTarget = p;
    },
  };
}());
