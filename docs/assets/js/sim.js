/* Crop Mor3 sensor mode simulator.
 * Mode data mirrors src/crop_rec/crop_rec.c (Crop Mor3 menu: Preset / Aspect ratio / Output)
 * and src/mlv_lite/mlv_lite.c (RAW video value / crop factor).
 * The LCD is drawn with Magic Lantern's own bitmap fonts (argnor23 / argnor32 RBF) and
 * menu icons (src/ico.c), converted to PNG atlases in assets/ml/ (GPLv2, same as ML). */
(() => {
  'use strict';

  /* ---------- repo links ---------- */
  const meta = (n) => (document.querySelector(`meta[name="${n}"]`) || {}).content || '';
  const REPO = meta('repo').replace(/\/$/, '');
  const TAG = meta('release-tag');
  if (REPO && !REPO.includes('/OWNER/')) {
    document.querySelectorAll('[data-repo]').forEach((a) => { a.href = REPO + a.dataset.repo; });
    document.querySelectorAll('[data-asset]').forEach((a) => { a.href = `${REPO}/releases/download/${TAG}/${a.dataset.asset}`; });
  }
  document.querySelectorAll('[data-tag]').forEach((e) => { e.textContent = TAG; });

  if (!document.getElementById('stage')) return;

  /* ---------- mode data ---------- */
  const SENSOR = { w: 5760, h: 3840 };
  const FAMILIES = [
    { name: 'OFF', short: 'OFF' },
    {
      name: '1:1 crop', short: '1:1 crop', x5: true,
      presets: [
        { name: '1280p (3:2)', tag: '1280p', ctag: '3x 3:2', ar: '3:2', w: 1920, h: 1280, hires: false },
        { name: '1080p (16:9)', tag: '1080p', ctag: '3x 1080p', ar: '16:9', w: 1920, h: 1080, hires: false },
        { name: '816p (2.35:1)', tag: '816p', ctag: '3x 2.35', ar: '2.35:1', w: 1920, h: 816, hires: false },
        { name: '2.3K (16:9)', tag: '2.3K', ctag: '2.3K', ar: '16:9', w: 2336, h: 1312, hires: true },
        { name: '3.1K (2.35:1)', tag: '3.1K', ctag: '3.1K', ar: '2.35:1', w: 3104, h: 1320, hires: true },
        { name: '3.5K (2.71:1)', tag: '3.5K', ctag: '3.5K', ar: '2.71:1', w: 3584, h: 1320, hires: true },
      ],
    },
    {
      name: '1x3', short: '1x3',
      presets: [
        { name: '3.4K (16:9)', tag: '3.4K', ar: '16:9' },
        { name: '4.5K (2.35:1)', tag: '4.5K', ar: '2.35:1' },
        { name: '5.4K (2040p)', tag: '5.4K', ar: '2.66:1' },
        { name: '5.7K (3:1 Max)', tag: '5.7K', ar: '3:1' },
      ],
    },
    {
      name: '3x3', short: '3x3', hfps: true,
      presets: [
        { name: '50/60p (HFR)', w: 1920, h24: 800, h25: 960, fps24: 60, fps25: 50, ar24: '2.40:1', ar25: '2:1' },
        { name: '45/48p Tall', w: 1920, h24: 1040, h25: 1080, fps24: 48, fps25: 45, ar24: '1.85:1', ar25: '16:9' },
        { name: '2.40:1 Scope', w: 1920, h24: 800, h25: 800, fps24: 60, fps25: 50, ar24: '2.40:1', ar25: '2.40:1' },
      ],
    },
  ];
  const TALL = {
    24: { h: [1920, 1920, 2040, 1920], w: [1136, 1504, 1808, 1920] },
    25: { h: [1728, 1728, 1768, 1728], w: [1024, 1360, 1568, 1920] },
    30: { h: [1536, 1536, 1536, 1536], w: [912, 1200, 1360, 1920] },
  };
  const FPS_REAL = { 24: 23.976, 25: 25, 30: 29.97 };
  /* the sample photo: Canon 5D Mark III CR2, 62 mm, f/2.8, 1/200 s, ISO 1250 */
  const EXIF = { focal: 62, f: '2.8', shutter: '200', iso: 1250 };

  const TEXT = [
    {
      title: 'Stock 1080p · 3×3 binned',
      body: 'Full sensor width, every 3×3 block of photosites merged into one pixel.',
      good: 'Widest view, lowest data rate.',
      watch: 'Soft image. Fine patterns can alias and show moiré.',
    },
    {
      title: '1:1 crop · every photosite, 1080p to 3.5K',
      body: 'An unbinned window from the sensor center, one photosite per pixel. 1280p, 1080p and 816p are WYSIWYG and pan with the joystick. 2.3K to 3.5K show the center 1920 px in LiveView.',
      good: 'Telephoto reach and maximum detail. No moiré.',
      watch: 'x5 LiveView runs at 29.776 fps: set FPS override to 23.976 for 24p. 2.3K+ needs a fast CF card.',
    },
    {
      title: '1x3 anamorphic · full width, up to 5.7K',
      body: 'Every line in a band across the sensor, 3 horizontal photosites per pixel. Stretch ×3 in post: up to 5760×1920 or 5424×2040. WYSIWYG.',
      good: 'Full horizontal field of view in scope and anamorphic ratios.',
      watch: 'Desqueeze ×3 in post. RAWC metadata does it automatically.',
    },
    {
      title: '3x3 HFR · full frame, 45p to 60p',
      body: 'Canon 720p mode with a taller readout: 800 to 1080 lines at 45 to 60 fps, full sensor width.',
      good: 'Full-frame slow motion, taller than stock 720p.',
      watch: 'Set Canon to 720p 50 (PAL) or 60 (NTSC) first.',
    },
  ];

  /* ---------- state ---------- */
  const state = {
    family: 2,
    preset: [0, 0, 3, 0],
    fps: 24,
    override: false,
    bits: 14,
    lossless: true,
    compare: false,
    focal: EXIF.focal,
    info: true,
    menu: null,       // null | 'tab' | 'sub'
    tabRow: 6,        // "Crop Mor3" row in the Movie tab
    subRow: 0,
    rec: false,
    recStart: 0,
    recName: '',
    pan: { x: 0, y: 0 },
    recView: 'final', // 'final' | 'raw'
    steps: { menu: false, mode: false, framing: false, exit: false, rec: false },
  };

  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));

  function geo() {
    const f = state.family;
    const p = state.preset[f] || 0;
    const g = { family: f, preset: p, hires: false };
    if (f === 0) {
      Object.assign(g, { rawW: 1920, rawH: 1080, binX: 3, binY: 3, label: 'Crop Mor3 OFF', tag: '1080p', ar: '16:9', ctag: '' });
    } else if (f === 1) {
      const P = FAMILIES[1].presets[p];
      Object.assign(g, { rawW: P.w, rawH: P.h, binX: 1, binY: 1, label: `1:1 crop (${P.tag})`, tag: P.tag, ctag: P.ctag, ar: P.ar, hires: P.hires });
    } else if (f === 2) {
      const t = TALL[state.fps];
      const P = FAMILIES[2].presets[p];
      Object.assign(g, { rawW: t.w[p], rawH: t.h[p], binX: 3, binY: 1, label: `1x3 (${P.tag})`, tag: P.tag, ctag: P.tag, ar: P.ar });
    } else {
      const P = FAMILIES[3].presets[p];
      const is25 = state.fps === 25;
      const rh = is25 ? P.h25 : P.h24;
      const rfps = is25 ? P.fps25 : P.fps24;
      Object.assign(g, {
        rawW: P.w, rawH: rh, binX: 3, binY: 3, label: `3x3 (${rh}p ${rfps}fps)`, tag: `${rh}p ${rfps}fps`,
        ctag: p === 2 ? `${rfps}p 2.40` : `${rfps}p 3x3`, ar: is25 ? P.ar25 : P.ar24, hfpsVal: rfps,
      });
    }
    g.sensorW = g.rawW * g.binX;
    g.sensorH = g.rawH * g.binY;
    g.outW = f === 2 ? g.rawW * 3 : g.rawW;
    g.outH = g.rawH;
    g.x5 = !!FAMILIES[f].x5;
    g.canPan = f === 1 && !g.hires;
    g.fps = f === 3 ? g.hfpsVal : g.x5 ? (state.override ? 23.976 : 29.776) : FPS_REAL[state.fps];

    // capture window on the sensor (1:1 WYSIWYG presets can be panned with the joystick)
    let cx = SENSOR.w / 2, cy = SENSOR.h / 2;
    if (g.canPan) {
      cx = clamp(cx + state.pan.x, g.sensorW / 2, SENSOR.w - g.sensorW / 2);
      cy = clamp(cy + state.pan.y, g.sensorH / 2, SENSOR.h - g.sensorH / 2);
    }
    g.sx = cx - g.sensorW / 2;
    g.sy = cy - g.sensorH / 2;
    g.cap = { x: g.sx, y: g.sy, w: g.sensorW, h: g.sensorH };

    // what the 3:2 LCD shows: x5 modes show a 1920-wide 1:1 window, the others the capture width
    const vw = g.x5 ? 1920 : g.sensorW;
    const vh = vw / 1.5;
    g.view = { x: clamp(cx - vw / 2, 0, SENSOR.w - vw), y: clamp(cy - vh / 2, 0, SENSOR.h - vh), w: vw, h: vh };

    g.hFov = g.sensorW / SENSOR.w;
    g.vFov = g.sensorH / SENSOR.h;
    g.crop = SENSOR.w / g.sensorW;
    return g;
  }

  function rates(g) {
    const unc = (g.rawW * g.rawH * state.bits / 8) * g.fps / 1048576;
    return { unc, lo: unc * 0.5, hi: unc * 0.65 };
  }

  const fmt = (n) => n.toLocaleString('en-US');
  const res = (w, h) => `${w}×${h}`;
  const resX = (w, h) => `${w}x${h}`;
  const fpsTxt = (f) => (Number.isInteger(f) ? String(f) : f.toFixed(3));
  const pct = (v) => `${Math.round(v * 100)}%`;
  const aspectTxt = (w, h) => {
    const r = w / h;
    const known = [[16 / 9, '16:9'], [1.5, '3:2'], [3, '3:1']];
    for (const [v, t] of known) if (Math.abs(r - v) < 0.015) return t;
    return `${r.toFixed(2)}:1`;
  };

  /* ---------- helpers ---------- */
  const $ = (id) => document.getElementById(id);
  const dpr = () => Math.min(2, window.devicePixelRatio || 1);
  function prep(cv, w, h) {
    const d = dpr();
    if (cv.width !== w * d || cv.height !== h * d) { cv.width = w * d; cv.height = h * d; }
    const ctx = cv.getContext('2d');
    ctx.setTransform(d, 0, 0, d, 0, 0);
    ctx.imageSmoothingEnabled = true;
    ctx.imageSmoothingQuality = 'high';
    return ctx;
  }
  function fit(aspect, W, H) {
    let w = W, h = W / aspect;
    if (h > H) { h = H; w = H * aspect; }
    return { x: (W - w) / 2, y: (H - h) / 2, w, h };
  }
  const loadImg = (src) => new Promise((ok, fail) => {
    const i = new Image();
    i.onload = () => ok(i);
    i.onerror = fail;
    i.src = src;
  });

  let toastTimer = 0;
  function toast(msg, ms = 2200) {
    const t = $('toast');
    t.textContent = msg;
    t.classList.add('show');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => t.classList.remove('show'), ms);
  }

  /* ---------- scene: the full sensor as a mip chain (sensor px -> level px) ---------- */
  let scene = null;           // { levels: [{ c, k }], id }
  let sampleScene = null;
  let sceneSeq = 0;

  function makeScene(img, coverW, coverH) {
    // level 0 at the given size (cover-fit to 3:2), then halves down to ~720 px
    const levels = [];
    let c = document.createElement('canvas');
    c.width = coverW; c.height = coverH;
    const x = c.getContext('2d');
    x.imageSmoothingQuality = 'high';
    const s = Math.max(coverW / img.width, coverH / img.height);
    x.drawImage(img, (coverW - img.width * s) / 2, (coverH - img.height * s) / 2, img.width * s, img.height * s);
    levels.push({ c, k: coverW / SENSOR.w });
    while (c.width > 900) {
      const n = document.createElement('canvas');
      n.width = Math.round(c.width / 2); n.height = Math.round(c.height / 2);
      const nx = n.getContext('2d');
      nx.imageSmoothingQuality = 'high';
      nx.drawImage(c, 0, 0, n.width, n.height);
      levels.push({ c: n, k: n.width / SENSOR.w });
      c = n;
    }
    return { levels, id: ++sceneSeq };
  }

  /* draw sensor rect r (sensor px) into dest; picks the smallest level that still has enough pixels */
  function drawSensor(ctx, r, dx, dy, dw, dh) {
    if (!scene) { ctx.fillStyle = '#111'; ctx.fillRect(dx, dy, dw, dh); return; }
    const need = (dw * dpr()) / r.w;
    let L = scene.levels[0];
    for (const lv of scene.levels) if (lv.k >= need) L = lv;
    ctx.drawImage(L.c, r.x * L.k, r.y * L.k, r.w * L.k, r.h * L.k, dx, dy, dw, dh);
  }

  function loadPhoto(file) {
    const url = URL.createObjectURL(file);
    loadImg(url).then((img) => {
      URL.revokeObjectURL(url);
      const w = Math.min(SENSOR.w, Math.max(1440, Math.round(Math.max(img.width, img.height * 1.5))));
      scene = makeScene(img, w, Math.round(w / 1.5));
      $('photo-reset').hidden = false;
      renderCapture(); requestLcd();
    }).catch(() => { URL.revokeObjectURL(url); toast('That file could not be read as an image.'); });
  }

  /* ---------- Magic Lantern bitmap fonts & icons ---------- */
  const ML = { fonts: {}, icons: null, tint: new Map() };
  const SYM = { '°': 0x83, '×': 0x88, '±': 0x85, 'µ': 0x84, '…': 0x7F };

  async function loadML() {
    const base = 'assets/ml/';
    const meta = await (await fetch(base + 'ml-fonts.json')).json();
    const names = Object.keys(meta.fonts);
    const imgs = await Promise.all(names.map((n) => loadImg(`${base}ml-font-${n}.png`)));
    names.forEach((n, i) => { ML.fonts[n] = { ...meta.fonts[n], img: imgs[i] }; });
    ML.icons = { ...meta.icons, img: await loadImg(base + 'ml-icons.png') };
  }

  function tinted(img, color) {
    const key = img.src + color;
    let c = ML.tint.get(key);
    if (!c) {
      c = document.createElement('canvas');
      c.width = img.width; c.height = img.height;
      const x = c.getContext('2d');
      x.drawImage(img, 0, 0);
      x.globalCompositeOperation = 'source-in';
      x.fillStyle = color; x.fillRect(0, 0, c.width, c.height);
      ML.tint.set(key, c);
    }
    return c;
  }

  const codesOf = (s) => Array.from(s, (ch) => SYM[ch] || (ch.charCodeAt(0) < 160 ? ch.charCodeAt(0) : 63));
  function textWidth(font, s) {
    const F = ML.fonts[font];
    return F ? codesOf(s).reduce((a, c) => a + (F.w[c] || 0), 0) : 0;
  }

  /* ML-style text: 1:1 bitmap glyphs, optional SHADOW_FONT outline. Returns the width. */
  function mlText(ctx, font, s, x, y, color, o = {}) {
    const F = ML.fonts[font];
    if (!F || !s) return 0;
    const codes = codesOf(s);
    let w = codes.reduce((a, c) => a + (F.w[c] || 0), 0);
    const squeeze = o.fitW && w > o.fitW ? o.fitW / w : 1;   // FONT_ALIGN_JUSTIFIED
    if (squeeze < 1) w = o.fitW;
    if (o.align === 'right') x -= w; else if (o.align === 'center') x -= Math.round(w / 2);
    x = Math.round(x); y = Math.round(y);
    const pass = (img, ox, oy) => {
      let cx = 0;
      for (const c of codes) {
        const cw = F.w[c] || 0;
        if (cw) ctx.drawImage(img, (c % 16) * F.cw, (c >> 4) * F.h, cw, F.h, x + ox + Math.round(cx * squeeze), y + oy, cw, F.h);
        cx += cw;
      }
    };
    const smooth = ctx.imageSmoothingEnabled;
    ctx.imageSmoothingEnabled = false;
    if (o.shadow) {
      const sh = tinted(F.img, o.shadowColor || '#000');
      for (const [ox, oy] of [[-1, 0], [1, 0], [0, -1], [0, 1], [-1, -1], [1, 1], [-1, 1], [1, -1]]) pass(sh, ox, oy);
    }
    pass(tinted(F.img, color), 0, 0);
    ctx.imageSmoothingEnabled = smooth;
    return w;
  }

  /* ML menu icons (src/ico.c): a audio, b expo, c overlay, d movie, e shoot, f focus, g display,
     h prefs, i debug, j info, k mymenu, l script, m Q-forward, n Q-back, o forward, p modules, q modified */
  function mlIcon(ctx, ch, x, y, color) {
    const I = ML.icons;
    if (!I) return 0;
    const i = I.names.indexOf(ch);
    if (i < 0) return 0;
    const smooth = ctx.imageSmoothingEnabled;
    ctx.imageSmoothingEnabled = false;
    ctx.drawImage(tinted(I.img, color), i * I.cw, 0, I.cw, I.h, Math.round(x), Math.round(y), I.cw, I.h);
    ctx.imageSmoothingEnabled = smooth;
    return I.w[i];
  }

  /* ---------- LCD ---------- */
  const LW = 720, LH = 480;
  const lcd = $('lcd');
  const COL = {
    bg: '#151b35', head: '#6f7689', bar: '#0b20e0', barL: '#5f6787', white: '#ffffff', gray: '#8d95ac',
    dim: '#5d6480', green: '#46f046', cyan: '#39ddf7', yellow: '#f2e530', mask: '#04060c', line: '#e8e8e8',
  };
  let anim = null, from = null, target = null, tweenT0 = 0;
  const TWEEN_MS = 480;
  let lcdQueued = false;

  function setViewTarget(animate = true) {
    const g = geo();
    const t = { view: { ...g.view }, cap: { ...g.cap } };
    if (!anim || !animate || window.matchMedia('(prefers-reduced-motion: reduce)').matches) {
      anim = t; target = null;
    } else {
      from = { view: { ...anim.view }, cap: { ...anim.cap } }; target = t; tweenT0 = performance.now();
    }
    requestLcd();
  }

  function requestLcd() {
    if (lcdQueued) return;
    lcdQueued = true;
    requestAnimationFrame(drawLcd);
  }

  function drawLcd(now) {
    lcdQueued = false;
    const ctx = prep(lcd, LW, LH);
    ctx.fillStyle = '#000'; ctx.fillRect(0, 0, LW, LH);

    if (target) {
      const t = Math.min(1, (now - tweenT0) / TWEEN_MS);
      const e = 1 - Math.pow(1 - t, 3);
      const mix = (a, b) => { const o = {}; for (const k of ['x', 'y', 'w', 'h']) o[k] = a[k] + (b[k] - a[k]) * e; return o; };
      anim = { view: mix(from.view, target.view), cap: mix(from.cap, target.cap) };
      if (t >= 1) { anim = target; target = null; }
    }

    if (state.menu) drawMenu(ctx);
    else drawLiveView(ctx, now);

    updateClip(now);
    if (target || state.rec) requestLcd();
  }

  /* histogram + spotmeter from the visible area (cached per view) */
  let statsKey = '', lvStats = null;
  const probe = document.createElement('canvas');
  probe.width = 144; probe.height = 96;
  function lvMeasure(v) {
    if (!scene) return null;
    const key = `${scene.id}|${Math.round(v.x)}|${Math.round(v.y)}|${Math.round(v.w)}`;
    if (key === statsKey) return lvStats;
    const x = probe.getContext('2d', { willReadFrequently: true });
    const L = scene.levels[scene.levels.length - 1];
    x.drawImage(L.c, v.x * L.k, v.y * L.k, v.w * L.k, v.h * L.k, 0, 0, 144, 96);
    const d = x.getImageData(0, 0, 144, 96).data;
    const bins = 64, h = [new Uint32Array(bins), new Uint32Array(bins), new Uint32Array(bins)];
    for (let i = 0; i < d.length; i += 4) for (let c = 0; c < 3; c++) h[c][d[i + c] >> 2]++;
    let spot = 0, n = 0;
    for (let yy = 45; yy < 51; yy++) for (let xx = 69; xx < 75; xx++) {
      const i = (yy * 144 + xx) * 4;
      spot += 0.299 * d[i] + 0.587 * d[i + 1] + 0.114 * d[i + 2]; n++;
    }
    statsKey = key;
    lvStats = { hist: h, spot: Math.round(spot / n / 2.55) };
    return lvStats;
  }

  function drawLiveView(ctx, now) {
    const g = geo();
    const v = anim.view, c = anim.cap;
    const s = LW / v.w;
    drawSensor(ctx, v, 0, 0, LW, LH);

    // mlv_lite masks + white frame lines around the recorded area
    const x0 = clamp((c.x - v.x) * s, 0, LW), x1 = clamp((c.x + c.w - v.x) * s, 0, LW);
    const y0 = clamp((c.y - v.y) * s, 0, LH), y1 = clamp((c.y + c.h - v.y) * s, 0, LH);
    ctx.fillStyle = COL.mask;
    ctx.fillRect(0, 0, LW, y0); ctx.fillRect(0, y1, LW, LH - y1);
    ctx.fillRect(0, y0, x0, y1 - y0); ctx.fillRect(x1, y0, LW - x1, y1 - y0);
    ctx.fillStyle = COL.line;
    if (y0 > 1) ctx.fillRect(0, Math.round(y0) - 2, LW, 2);
    if (y1 < LH - 1) ctx.fillRect(0, Math.round(y1), LW, 2);
    if (x0 > 1) ctx.fillRect(Math.round(x0) - 2, y0, 2, y1 - y0);
    if (x1 < LW - 1) ctx.fillRect(Math.round(x1), y0, 2, y1 - y0);

    if (!state.info) { drawRecIndicator(ctx, now, 8); return; }
    const sh = { shadow: true };
    const font = 'argnor23';

    // ML top bar
    ctx.fillStyle = 'rgba(8,9,16,0.72)'; ctx.fillRect(0, 0, LW, 36);
    const d = new Date();
    const freeGB = 29.3 - (state.rec ? rates(g).lo * (now - state.recStart) / 1000 / 1024 : 0);
    mlText(ctx, font, `${d.getHours()}:${String(d.getMinutes()).padStart(2, '0')}`, 52, 7, COL.white, sh);
    mlText(ctx, font, 'RAW', 188, 7, COL.green, sh);
    mlText(ctx, font, `${g.x5 ? 35 : 32}°C`, 316, 7, COL.white, sh);
    mlText(ctx, font, g.fps.toFixed(3), 442, 7, COL.white, sh);
    mlText(ctx, font, `${Math.max(0, freeGB).toFixed(1)}GB`, 594, 7, COL.white, sh);

    // ML bottom bar
    ctx.fillStyle = 'rgba(8,9,16,0.72)'; ctx.fillRect(0, 432, LW, 34);
    const modeTag = g.family === 0 ? '1080p' : g.family === 1 ? `${g.tag} 1:1` : g.family === 2 ? `${g.tag} 1x3` : `${g.hfpsVal}p 3x3`;
    // items keep their usual column but push right if the previous one is wider
    const items = [['Mv', 38, '#dfe2ea'], [modeTag, 92], [`${EXIF.focal}mm`, 192], [`\x81${EXIF.f}`, 268],
      [`\x82${EXIF.shutter}`, 330], [`\x80${EXIF.iso}`, 404], ['AutoWB.G2', 498]];
    let bx = 0;
    for (const [t, x, c] of items) bx = Math.max(x, bx) + mlText(ctx, font, t, Math.max(x, bx), 438, c || COL.white, sh) + 12;
    const batX = Math.max(634, bx);
    ctx.fillStyle = COL.yellow; ctx.strokeStyle = COL.yellow; ctx.lineWidth = 2;
    ctx.fillRect(batX + 3, 441, 6, 2); ctx.strokeRect(batX, 444, 12, 17); ctx.fillRect(batX + 2, 452, 8, 8);
    mlText(ctx, font, '45%', batX + 16, 438, COL.yellow, sh);

    const m = lvMeasure(v);
    // spotmeter
    ctx.strokeStyle = COL.white; ctx.lineWidth = 1.5;
    ctx.strokeRect(355.5, 223.5, 9, 9);
    if (m) mlText(ctx, font, `${m.spot}%`, 360, 236, COL.white, { shadow: true, align: 'center' });

    // RGB histogram (bottom right)
    if (m) drawHistogram(ctx, m.hist, 560, 370, 124, 54);

    // Canon x5 magnification box (1:1 crop modes run in x5)
    if (g.x5 && !target) {
      ctx.fillStyle = 'rgba(0,0,0,0.78)'; ctx.fillRect(600, 262, 106, 96);
      mlText(ctx, font, '*', 608, 268, COL.white);
      mlText(ctx, font, 'X5', 700, 268, COL.white, { align: 'right' });
      ctx.strokeStyle = COL.white; ctx.lineWidth = 2;
      ctx.strokeRect(606, 300, 94, 50);
      const iw = 94 / 5, ih = 50 / 5;
      const ix = 606 + (v.x + v.w / 2) / SENSOR.w * 94 - iw / 2;
      const iy = 300 + (v.y + v.h / 2) / SENSOR.h * 50 - ih / 2;
      ctx.fillStyle = COL.white; ctx.fillRect(ix, iy, iw, ih);
    }

    drawRecIndicator(ctx, now, 44);
  }

  function drawHistogram(ctx, h, x, y, w, hh) {
    ctx.fillStyle = 'rgba(0,0,0,0.85)'; ctx.fillRect(x, y, w, hh);
    const bins = h[0].length;
    let max = 1;
    for (let c = 0; c < 3; c++) for (let i = 2; i < bins - 2; i++) max = Math.max(max, h[c][i]);
    const prev = ctx.globalCompositeOperation;
    ctx.globalCompositeOperation = 'lighter';
    const cols = ['#ff2020', '#20ff20', '#2040ff'];
    for (let c = 0; c < 3; c++) {
      ctx.fillStyle = cols[c];
      ctx.beginPath(); ctx.moveTo(x, y + hh);
      for (let i = 0; i < bins; i++) {
        const v = Math.sqrt(Math.min(1, h[c][i] / max));
        ctx.lineTo(x + (i + 0.5) * w / bins, y + hh - v * (hh - 2));
      }
      ctx.lineTo(x + w, y + hh); ctx.closePath(); ctx.fill();
    }
    ctx.globalCompositeOperation = prev;
    ctx.fillStyle = 'rgba(255,255,255,0.55)';
    for (let i = 1; i < 5; i++) ctx.fillRect(Math.round(x + w - i * w / 10), y, 1, hh);
    ctx.strokeStyle = COL.white; ctx.lineWidth = 1;
    ctx.strokeRect(x + 0.5, y + 0.5, w - 1, hh - 1);
  }

  function drawRecIndicator(ctx, now, y) {
    if (!state.rec) return;
    const s = (now - state.recStart) / 1000;
    const on = Math.floor(s * 2) % 2 === 0;
    ctx.fillStyle = on ? '#ff2a2a' : '#7a1d1d';
    ctx.beginPath(); ctx.arc(22, y + 13, 8, 0, Math.PI * 2); ctx.fill();
    mlText(ctx, 'argnor23', `${tc(s)}  ${fmt(Math.floor(s * geo().fps))} fr`, 38, y + 2, COL.white, { shadow: true });
  }

  const tc = (s) => {
    const m = Math.floor(s / 60), ss = Math.floor(s % 60);
    return `${String(m).padStart(2, '0')}:${String(ss).padStart(2, '0')}`;
  };

  /* ---------- ML menu (Movie tab), layout from src/menu.c ---------- */
  const TABS = ['a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'p', 'i', 'j', 'k', 'q']; // 'd' = Movie
  const ROW_H = 34, ROW_Y0 = 52, MENU_X = 38;

  function tabRows() {
    const g = geo();
    const fam = g.family;
    let help2 = 'Press Q to select Preset and inspect Aspect ratio / Output resolution.';
    if (fam === 1) help2 = 'Center crop on sensor, no pixel binning/skipping. Press Q for Preset.';
    if (fam === 2) help2 = `a.k.a Anamorphic (${resX(g.rawW, g.rawH)} -> ${resX(g.outW, g.outH)}). Press Q for Preset.`;
    if (fam === 3) help2 = `Full-frame 3x3 High Framerate (${resX(g.rawW, g.rawH)} @ ${g.hfpsVal}p). Press Q for Preset.`;
    if (fam === 0) help2 = 'Canon 1080p mode: select 1:1 crop or 1x3 (switch Canon to 720p for 3x3 HFR).';
    const fpsInfo = g.x5 ? '29.776, Rs.85%' : '23.976, Rs.55%';
    const rawVal = fam === 0 ? `ON, ${resX(1920, 1080)}` : `ON, ${g.ctag} (${resX(g.rawW, g.rawH)})`;
    return [
      { name: 'REC indicator', value: 'Elapsed Time', icon: 'dice', help: 'Show time elapsed / remaining while recording.' },
      { name: 'Gradual Exposure', value: 'OFF', icon: 'off', fwd: true, help: 'Smooth exposure changes while recording.' },
      { name: 'FPS override', value: state.override ? 'ON' : 'OFF', rinfo: state.override ? '23.976' : fpsInfo, icon: state.override ? 'on' : 'off', fwd: true, act: 'fps',
        help: g.x5 ? 'x5 LiveView runs at 29.776 fps. Set 23.976 for 24p.' : 'Not needed here: this mode follows Canon’s movie FPS.' },
      { name: 'HDR video', value: 'OFF', icon: 'off', fwd: true, help: 'Alternates ISO between frames.' },
      { name: 'Vignetting', value: 'OFF', icon: 'off', fwd: true, help: 'Vignetting correction / effects.' },
      { name: 'Image Fine-tuning', value: '', icon: 'sub', sub: true, help: 'Subtle image quality tweaks.' },
      { name: 'Crop Mor3', value: fam === 0 ? 'OFF' : `${FAMILIES[fam].name} *`, rinfo: fam === 0 ? '' : fam === 3 ? g.tag : g.tag, icon: fam ? 'on' : 'off', q: true, act: 'crop',
        help: 'Turn your camera into Crop Mor3 modes (select one): 1:1 crop | 1x3 | 3x3.', help2 },
      { name: 'RAW video', value: rawVal, rinfo: `${g.crop.toFixed(2)}x`, icon: 'on', fwd: true, act: 'raw',
        help: 'Record 14-bit RAW video (mlv_lite).', help2: `${state.bits}-bit ${state.lossless ? 'lossless' : 'uncompressed'} · set the format from the panel.` },
      { name: 'Movie Tweaks', value: '', icon: 'subon', sub: true, help: 'Movie recording tweaks.' },
    ];
  }

  function subRows() {
    const g = geo();
    const f = FAMILIES[g.family];
    if (g.family === 0) return [];
    let help = '1:1 center crop (3x), true WYSIWYG LiveView.';
    if (g.hires) help = '1:1 Hi-Res center crop (fast CF recommended). Centered LiveView.';
    if (g.family === 2) help = `1x3 anamorphic -> ${resX(g.outW, g.outH)} desqueezed @ ${state.fps}p, WYSIWYG.`;
    if (g.family === 3) help = `3x3 full-frame High FPS (${resX(g.rawW, g.rawH)} @ ${g.hfpsVal}p).`;
    const outInfo = g.family === 2 ? '1x3 WYSIWYG' : g.family === 3 ? '3x3 FF' : g.hires ? '1:1 Center' : '1:1 WYSIWYG';
    const rinfo = g.family === 3 ? `${resX(g.rawW, g.rawH)} ${g.hfpsVal}p` : resX(g.rawW, g.rawH);
    return [
      { name: 'Preset:', value: f.presets[g.preset].name, rinfo, help },
      { name: 'Aspect ratio:', value: g.ar, help: 'Aspect ratio of the current preset (use Preset / Framing to change).' },
      { name: `${resX(g.outW, g.outH)} @ ${Math.round(g.fps)}p`, value: outInfo, help: 'Final image size after desqueeze.', info: true },
    ];
  }

  function drawItemIcon(ctx, kind, x, y) {
    const cx = x + 18, cy = y + 17;
    if (kind === 'on' || kind === 'off') {
      ctx.fillStyle = kind === 'on' ? COL.green : COL.gray;
      ctx.beginPath(); ctx.arc(cx, cy, 8, 0, Math.PI * 2); ctx.fill();
    } else {
      ctx.fillStyle = kind === 'dice' ? COL.cyan : kind === 'subon' ? COL.green : COL.gray;
      ctx.fillRect(cx - 7, cy - 7, 14, 14);
    }
  }

  function drawMenu(ctx) {
    ctx.fillStyle = COL.bg; ctx.fillRect(0, 0, LW, LH);

    // header: tab name + icons (menu.c: icons from x=150, spacing (720-150)/tabs)
    ctx.fillStyle = COL.head; ctx.fillRect(0, 0, LW, 42);
    const sp = (720 - 150) / TABS.length;
    TABS.forEach((t, i) => {
      const x = 150 + i * sp;
      const sel = t === 'd';
      if (sel) {
        ctx.fillStyle = COL.bg; ctx.fillRect(x - 1, 2, sp + 3, 40);
        ctx.fillRect(x - 4, 40, sp + 9, 2);
      }
      const iw = ML.icons ? ML.icons.w[ML.icons.names.indexOf(t)] : 32;
      mlIcon(ctx, t, x + (sp - iw) / 2 + 1, 2, sel ? COL.white : '#d4d8e4');
    });
    mlText(ctx, 'argnor32', 'Movie', 5, 5, COL.white, { shadow: true, shadowColor: '#3a4054' });

    let helpRow;
    if (state.menu === 'tab') {
      const rows = tabRows();
      rows.forEach((r, i) => drawRow(ctx, r, i === state.tabRow, MENU_X, ROW_Y0 + i * ROW_H, 717, false));
      helpRow = rows[state.tabRow];
    } else {
      const rows = subRows();
      const w = 600, bx = (LW - w) / 2;
      const h = 40 + 25 + rows.length * ROW_H + 20;
      const by = Math.max(3, Math.round((LH - h) / 2 - 30));
      ctx.fillStyle = '#626e8c';
      for (let i = 1; i <= 4; i++) ctx.fillRect(bx - i, by - i, w + i * 2, h + i * 2);
      ctx.fillStyle = COL.head; ctx.fillRect(bx, by, w, 40);
      ctx.fillStyle = COL.bg; ctx.fillRect(bx, by + 40, w, h - 40);
      mlText(ctx, 'argnor32', 'Crop Mor3', bx + 15, by + 4, COL.white, { shadow: true, shadowColor: '#3a4054' });
      mlIcon(ctx, 'n', LW - bx - 45, by + 3, COL.white);
      rows.forEach((r, i) => drawRow(ctx, r, i === state.subRow, bx + 48, by + 65 + i * ROW_H, bx + w - 48, true));
      helpRow = rows[state.subRow];
    }

    // help lines (FONT_MED on the header/footer colour)
    ctx.fillStyle = COL.head; ctx.fillRect(0, 432, LW, 48);
    if (helpRow) {
      mlText(ctx, 'argnor23', helpRow.help || '', 10, 435, COL.white, { fitW: 700 });
      if (helpRow.help2) mlText(ctx, 'argnor23', helpRow.help2, 10, 458, COL.white, { fitW: 700 });
    }
  }

  function drawRow(ctx, r, selected, x, y, xEnd, inSub) {
    const F = 'argnor32';
    const nameW = textWidth(F, r.name);
    let xval = x + Math.max(inSub ? 316 : 324, nameW + 18);
    const valW = textWidth(F, r.value);
    const rW = r.rinfo ? textWidth(F, r.rinfo) : 0;
    const limit = (r.rinfo ? xEnd - rW - (inSub ? 0 : 35) - 16 : xEnd - (inSub ? 0 : 40)) - valW;
    if (xval > limit) xval = Math.max(x + nameW + 24, limit);

    if (selected) {
      const xl = x - 5;
      const xc = inSub && r.value && !r.info ? xval - 15 : xl;
      if (xc > xl) { ctx.fillStyle = COL.barL; ctx.fillRect(xl, y, xc - xl, ROW_H - 1); }
      ctx.fillStyle = COL.bar; ctx.fillRect(xc, y, xEnd - xc, ROW_H - 1);
    }
    if (inSub) drawItemIcon(ctx, 'dice', x - 34, y);
    else drawItemIcon(ctx, r.icon, x - 36, y);

    const sh = { shadow: true };
    mlText(ctx, F, r.name, x, y + 1, COL.white, sh);
    if (r.sub) mlIcon(ctx, 'o', x + 324 - 4, y - 2, COL.white);
    if (r.value) mlText(ctx, F, r.value, xval, y + 1, COL.white, sh);
    if (r.rinfo) mlText(ctx, F, r.rinfo, inSub ? xEnd : xEnd - 35, y + 1, COL.white, { shadow: true, align: 'right' });
    if (!inSub) {
      if (r.q && selected) mlIcon(ctx, 'm', 720 - 40, y - 2, COL.white);
      else if (r.q || r.fwd) mlIcon(ctx, 'o', 720 - 35, y - 2, COL.dim);
    }
  }

  /* ---------- camera buttons ---------- */
  const PAN_STEP = 240;
  function pan(dx, dy) {
    const g = geo();
    if (!g.canPan) {
      toast(g.x5 ? 'This preset records a centered window. Panning works in 1280p, 1080p and 816p.' : 'Panning moves the 1:1 crop window (1280p, 1080p, 816p).');
      return;
    }
    if (state.rec) { toast('Stop recording before moving the crop window.'); return; }
    const before = `${g.sx},${g.sy}`;
    state.pan.x = clamp(state.pan.x + dx * PAN_STEP, -(SENSOR.w - g.sensorW) / 2, (SENSOR.w - g.sensorW) / 2);
    state.pan.y = clamp(state.pan.y + dy * PAN_STEP, -(SENSOR.h - g.sensorH) / 2, (SENSOR.h - g.sensorH) / 2);
    const after = geo();
    if (`${after.sx},${after.sy}` === before) toast('Edge of the sensor.');
    renderCapture();
    setViewTarget(true);
  }

  function press(act, el, ev) {
    if (el) { el.classList.add('press'); setTimeout(() => el.classList.remove('press'), 140); }
    const m = state.menu;
    switch (act) {
      case 'menu':
        toast(m ? 'In the ML menu, MENU hides items you don’t use. Press ERASE (trash) to close.' : 'MENU opens Canon’s own menu. The Magic Lantern menu is on the ERASE (trash) button.', 3200);
        return;
      case 'erase':
        if (state.rec) { toast('Stop recording before opening the menu.'); return; }
        if (m) {
          state.menu = null;
          if (state.steps.menu) state.steps.exit = true;
          const g = geo();
          setViewTarget(true);
          if (g.family === 0) toast('Crop Mor3 OFF · Canon stock 1080p');
          else if (g.x5) toast(`Crop Mor3: entering x5 LiveView → ${g.label}`);
          else toast(`Crop Mor3: sensor set to ${g.label}`);
        } else {
          state.menu = 'tab'; state.tabRow = 6; state.steps.menu = true;
        }
        break;
      case 'up': case 'down': case 'left': case 'right': case 'set': {
        if (!m) {
          if (act === 'set') { toast('Press ERASE (trash) for the ML menu, then SET on Crop Mor3.'); return; }
          const dir = { up: [0, -1], down: [0, 1], left: [-1, 0], right: [1, 0] }[act];
          pan(dir[0], dir[1]);
          return;
        }
        if (act === 'up' || act === 'down') {
          const n = m === 'tab' ? tabRows().length : subRows().length;
          const k = m === 'tab' ? 'tabRow' : 'subRow';
          state[k] = (state[k] + (act === 'up' ? n - 1 : 1)) % n;
          break;
        }
        const d = act === 'left' ? -1 : 1;
        if (m === 'tab') {
          const row = tabRows()[state.tabRow];
          if (row.act === 'crop') cycleFamily(d, true);
          else if (row.act === 'fps') {
            if (!geo().x5) { toast('FPS override only matters in the 1:1 crop (x5) modes.'); return; }
            state.override = !state.override; syncPanel(); renderCapture();
          } else if (row.act === 'raw') toast('mlv_lite stays on in this simulator. Set the format from the panel.');
          else toast('This item is not part of the simulator.');
        } else {
          const row = subRows()[state.subRow];
          if (row.name === 'Preset:' || row.name === 'Aspect ratio:') cyclePreset(d, true);
          else toast('Output is information only.');
        }
        break;
      }
      case 'q':
        if (!m) { toast('Q opens the Crop Mor3 submenu from the ML menu. Press ERASE (trash) first.'); return; }
        if (m === 'tab') {
          if (state.tabRow !== 6) { state.tabRow = 6; }
          if (state.family === 0) { toast('Pick a mode with SET first. Crop Mor3 is OFF.'); break; }
          state.menu = 'sub'; state.subRow = 0;
        } else state.menu = 'tab';
        break;
      case 'rec':
        if (m) { toast('Close the ML menu (ERASE) before recording.'); return; }
        state.rec = !state.rec;
        if (state.rec) {
          state.recStart = performance.now();
          const d = new Date();
          state.recName = `M${String(d.getDate()).padStart(2, '0')}-${String(d.getHours()).padStart(2, '0')}${String(d.getMinutes()).padStart(2, '0')}.MLV`;
          state.steps.rec = true;
        } else {
          toast(`Saved ${state.recName}`);
        }
        $('lamp').classList.toggle('on', state.rec);
        syncPanel();
        break;
      case 'info': state.info = !state.info; break;
      case 'zoom':
        toast(FAMILIES[state.family].x5 ? 'No need to zoom. Crop Mor3 enters x5 by itself.' : 'Zoom is not needed. Crop Mor3 sets the LiveView itself.');
        return;
      case 'play': toast('PLAY opens mlv_play. See the mlv_play section below.'); return;
      case 'rate': toast('Not used by Crop Mor3.'); return;
      case 'joy': {
        if (m) { toast('Use the dial (▲▼◀▶) and SET for menus.'); return; }
        let dx = 0, dy = 0;
        if (ev && el) {
          const r = el.getBoundingClientRect();
          const ox = ev.clientX - (r.left + r.width / 2), oy = ev.clientY - (r.top + r.height / 2);
          if (Math.hypot(ox, oy) > r.width * 0.12) {
            if (Math.abs(ox) > Math.abs(oy)) dx = Math.sign(ox); else dy = Math.sign(oy);
          }
        }
        if (!dx && !dy) { toast(geo().canPan ? 'Click the edge of the joystick (or use the arrow keys) to move the crop window.' : 'The joystick pans the 1:1 crop window in 1280p, 1080p and 816p.'); return; }
        pan(dx, dy);
        return;
      }
      default: return;
    }
    renderSteps();
    requestLcd();
  }

  function cycleFamily(d, fromCamera) {
    if (state.rec) { toast('Stop recording before changing modes.'); return; }
    state.family = (state.family + d + 4) % 4;
    if (fromCamera) state.steps.mode = true;
    if (state.menu === 'sub' && state.family === 0) state.menu = 'tab';
    if (state.menu === 'sub') state.subRow = Math.min(state.subRow, subRows().length - 1);
    afterChange();
  }
  function cyclePreset(d, fromCamera) {
    if (state.rec || state.family === 0) return;
    const n = FAMILIES[state.family].presets.length;
    let p = (state.preset[state.family] + d + n) % n;
    if (state.family === 2 && p === 2 && state.fps !== 24) {
      p = (p + (d >= 0 ? 1 : -1) + n) % n;
    }
    state.preset[state.family] = p;
    if (fromCamera) state.steps.framing = true;
    afterChange();
  }
  function afterChange() {
    if (state.family === 2 && state.preset[2] === 2 && state.fps !== 24) {
      state.preset[2] = 3;
    }
    syncPanel();
    renderCapture();
    if (!state.menu) setViewTarget(true);
    renderSteps();
    requestLcd();
  }

  /* ---------- walkthrough ---------- */
  const STEPS = [
    ['menu', 'Press <b>ERASE</b> (trash). The Magic Lantern menu opens on <em>Movie → Crop Mor3</em>. MENU would open Canon’s menu instead.', 'erase'],
    ['mode', 'Press <b>SET</b> to cycle the mode family: <code>1:1 crop</code>, <code>1x3</code> or <code>3x3</code>.', 'set'],
    ['framing', 'Press <b>Q</b> to open the submenu on <em>Preset:</em>, and use <b>◀ ▶</b> or <b>SET</b> to pick a preset.', 'q'],
    ['exit', 'Press <b>ERASE</b> (trash) again to return to LiveView. Crop Mor3 reconfigures the sensor automatically.', 'erase'],
    ['rec', 'Press <b>START/STOP</b> to record RAW with mlv_lite.', 'rec'],
  ];
  function renderSteps() {
    const ol = $('steps');
    const cur = STEPS.find(([id]) => !state.steps[id]);
    ol.innerHTML = STEPS.map(([id, html]) => `<li class="${state.steps[id] ? 'done' : cur && cur[0] === id ? 'current' : ''}">${html}</li>`).join('');
    document.querySelectorAll('.hs.hint').forEach((e) => e.classList.remove('hint'));
    if (cur) {
      let act = cur[2];
      if (cur[0] === 'framing' && state.menu === 'sub') act = 'right';
      if (cur[0] === 'framing' && !state.menu) act = 'erase';
      if (cur[0] === 'mode' && !state.menu) act = 'erase';
      if (cur[0] === 'mode' && state.menu === 'tab' && state.tabRow !== 6) act = state.tabRow < 6 ? 'down' : 'up';
      const el = document.querySelector(`.hs[data-act="${act}"]`);
      if (el) el.classList.add('hint');
    }
  }

  /* ---------- panel ---------- */
  function segButtons(container, items, isOn, onClick) {
    container.innerHTML = '';
    items.forEach((it, i) => {
      const b = document.createElement('button');
      b.type = 'button';
      b.innerHTML = it;
      b.setAttribute('aria-pressed', String(isOn(i)));
      b.addEventListener('click', () => onClick(i));
      container.append(b);
    });
  }

  function syncPanel() {
    const f = state.family;
    segButtons($('seg-family'), FAMILIES.map((F) => F.short), (i) => i === f, (i) => {
      if (state.rec) { toast('Stop recording before changing modes.'); return; }
      state.family = i; if (state.menu === 'sub' && i === 0) state.menu = 'tab'; afterChange();
    });
    $('field-preset').hidden = f === 0;
    if (f !== 0) {
      segButtons($('seg-preset'), FAMILIES[f].presets.map((p) => p.name), (i) => i === state.preset[f], (i) => {
        if (state.rec) { toast('Stop recording before changing modes.'); return; }
        if (f === 2 && i === 2 && state.fps !== 24) {
          state.fps = 24;
          toast('5.4K (2040p) requires 24p · switched Canon FPS to 24p');
        }
        state.preset[f] = i; afterChange();
      });
    }
    segButtons($('seg-fps'), ['24p', '25p', '30p'], (i) => [24, 25, 30][i] === state.fps, (i) => {
      if (state.rec) { toast('Stop recording first.'); return; }
      state.fps = [24, 25, 30][i]; afterChange();
    });
    $('field-override').hidden = !FAMILIES[f].x5;
    $('chk-override').checked = state.override;
    segButtons($('seg-bits'), ['10-bit', '12-bit', '14-bit'], (i) => [10, 12, 14][i] === state.bits, (i) => {
      if (state.rec) { toast('Stop recording first.'); return; }
      state.bits = [10, 12, 14][i]; afterChange();
    });
    segButtons($('seg-comp'), ['Lossless', 'Uncompressed'], (i) => (i === 0) === state.lossless, (i) => {
      if (state.rec) { toast('Stop recording first.'); return; }
      state.lossless = i === 0; afterChange();
    });
    segButtons($('seg-recview'), ['Final image', 'As recorded'], (i) => (i === 0) === (state.recView === 'final'), (i) => {
      state.recView = i === 0 ? 'final' : 'raw'; syncPanel(); drawRecord(geo());
    });
    document.querySelectorAll('#panel-set .seg button').forEach((b) => { b.disabled = state.rec; });
  }

  /* ---------- capture explainer ---------- */
  function renderCapture() {
    const g = geo();
    const T = TEXT[g.family];
    $('cap-kicker').textContent = g.family === 0 ? 'What the sensor is doing · Crop Mor3 OFF' : `What the sensor is doing · ${g.label}`;
    $('cap-title').textContent = T.title;
    $('cap-body').textContent = T.body;
    $('cap-good').textContent = T.good;
    $('cap-watch').textContent = T.watch;

    drawRecord(g);
    drawSensorMap($('cv-mini'), g, 600, 400);
    drawPixels(g);
    drawStats(g);
  }

  const binTxt = (g) => (g.binX === 3 && g.binY === 3 ? '3×3 binned' : g.binX === 3 ? '3 horizontal photosites → 1 pixel' : '1:1, no binning');

  /* the side panel next to the camera: the frame you get, from the real photo */
  function drawRecord(g) {
    const W = 640, H = 360;
    const ctx = prep($('cv-rec'), W, H);
    ctx.fillStyle = '#080808'; ctx.fillRect(0, 0, W, H);
    const raw = state.recView === 'raw';
    const ar = raw ? g.rawW / g.rawH : g.outW / g.outH;
    const d = fit(ar, W - 24, H - 24); d.x += 12; d.y += 12;
    drawSensor(ctx, g.cap, d.x, d.y, d.w, d.h);
    ctx.strokeStyle = 'rgba(255,255,255,0.35)'; ctx.lineWidth = 1;
    ctx.strokeRect(d.x + 0.5, d.y + 0.5, d.w - 1, d.h - 1);

    const chips = [
      `<b>${g.family === 0 ? 'Canon 1080p' : g.label}</b>`,
      raw ? `${res(g.rawW, g.rawH)} in the MLV${g.family === 2 ? ' (squeezed)' : ''}` : `${res(g.outW, g.outH)} · ${aspectTxt(g.outW, g.outH)}`,
      `${fpsTxt(g.fps)} fps`,
      `${pct(g.hFov)} × ${pct(g.vFov)} of the sensor`,
    ];
    $('rec-tags').innerHTML = chips.map((c) => `<span>${c}</span>`).join('');
    $('rec-note').textContent = raw
      ? (g.family === 2 ? 'Squeezed 3:1. Stretch ×3 in post (automatic via RAWC).' : 'Same as the final image. No desqueeze.')
      : (g.canPan ? 'Pan with the joystick or arrow keys in LiveView.' : g.hires ? 'LiveView shows the center 1920 px. All of this is recorded.' : 'Cut from the 5760×3840 photo at the real sensor position.');
  }

  function dimScene(ctx, d) {
    drawSensor(ctx, { x: 0, y: 0, w: SENSOR.w, h: SENSOR.h }, d.x, d.y, d.w, d.h);
    ctx.fillStyle = 'rgba(8,8,8,0.68)'; ctx.fillRect(d.x, d.y, d.w, d.h);
  }

  function drawSensorMap(cv, g, W, H) {
    const ctx = prep(cv, W, H);
    ctx.clearRect(0, 0, W, H);
    const pad = 14;
    const d = fit(1.5, W - pad * 2, H - pad * 2); d.x += pad; d.y += pad;
    const s = d.w / SENSOR.w;
    dimScene(ctx, d);

    if (state.compare) {
      ctx.setLineDash([4, 4]); ctx.lineWidth = 1;
      const save = { family: state.family, preset: state.preset.slice() };
      for (let f = 0; f < 4; f++) {
        const len = f === 0 ? 1 : FAMILIES[f].presets.length;
        for (let p = 0; p < len; p++) {
          state.family = f; state.preset[f] = p;
          const o = geo();
          if (f === save.family && p === save.preset[f]) continue;
          ctx.strokeStyle = 'rgba(255,255,255,0.28)';
          ctx.strokeRect(d.x + o.sx * s + 0.5, d.y + o.sy * s + 0.5, o.sensorW * s - 1, o.sensorH * s - 1);
        }
      }
      state.family = save.family; state.preset = save.preset;
      ctx.setLineDash([]);
    }

    const rx = d.x + g.sx * s, ry = d.y + g.sy * s, rw = g.sensorW * s, rh = g.sensorH * s;
    drawSensor(ctx, g.cap, rx, ry, rw, rh);
    if (g.binX === 3 && g.binY === 1) {
      ctx.fillStyle = 'rgba(0,0,0,0.13)';
      for (let x = rx; x < rx + rw; x += 6) ctx.fillRect(x, ry, 2, rh);
    }
    ctx.strokeStyle = '#f0a43a'; ctx.lineWidth = 2;
    ctx.strokeRect(rx, ry, rw, rh);
    if (g.hires) {
      ctx.setLineDash([5, 4]); ctx.strokeStyle = 'rgba(255,255,255,0.7)'; ctx.lineWidth = 1;
      ctx.strokeRect(d.x + g.view.x * s, ry + 3, g.view.w * s, rh - 6); ctx.setLineDash([]);
    }

    ctx.font = '600 13px ui-monospace, Menlo, monospace';
    label(ctx, `read: ${res(g.sensorW, g.sensorH)}`, rx + 6, ry + 6, '#f0a43a');
    ctx.fillStyle = 'rgba(255,255,255,0.55)'; ctx.font = '500 12px ui-monospace, Menlo, monospace';
    ctx.textAlign = 'right'; ctx.fillText('sensor 5760×3840', d.x + d.w - 6, d.y + d.h - 8); ctx.textAlign = 'left';
  }

  function label(ctx, text, x, y, color) {
    const w = ctx.measureText(text).width + 10;
    ctx.fillStyle = 'rgba(0,0,0,0.72)'; ctx.fillRect(x, y, w, 19);
    ctx.fillStyle = color; ctx.fillText(text, x + 5, y + 14);
  }

  function drawPixels(g) {
    const cols = 12, rows = 6, c = 24, gap = 2, padTop = 6;
    const bayer = (x, y) => (y % 2 === 0 ? (x % 2 === 0 ? '#b5504c' : '#4f8f55') : (x % 2 === 0 ? '#4f8f55' : '#4a68b5'));
    const W = cols * (c + gap) + 8, H = rows * (c + gap) + padTop + 8;
    let s = `<svg viewBox="0 0 ${W} ${H}" role="img" aria-label="Photosite grouping diagram">`;
    for (let y = 0; y < rows; y++) for (let x = 0; x < cols; x++) {
      s += `<rect x="${4 + x * (c + gap)}" y="${padTop + y * (c + gap)}" width="${c}" height="${c}" rx="3" fill="${bayer(x, y)}" opacity="0.8"/>`;
    }
    const gx = g.binX, gy = g.binY;
    for (let y = 0; y < rows; y += gy) for (let x = 0; x < cols; x += gx) {
      s += `<rect x="${3 + x * (c + gap)}" y="${padTop - 1 + y * (c + gap)}" width="${gx * (c + gap)}" height="${gy * (c + gap)}" rx="4" fill="none" stroke="#f0a43a" stroke-width="1.6"/>`;
    }
    s += '</svg>';
    $('pixels').innerHTML = s;
    $('note-pixels').textContent = gx === 3 && gy === 3
      ? '3×3 photosites → 1 pixel. Wide view, high framerate.'
      : gx === 3 ? '3 side-by-side photosites → 1 pixel. Full lines, squeezed width.'
      : '1 photosite → 1 pixel. Full detail, center crop.';
  }

  function drawStats(g) {
    const r = rates(g);
    const eq = Math.round(state.focal * g.crop);
    const fpsNote = g.family === 3 ? 'Canon 720p HFR mode' : g.x5 ? (state.override ? 'ML FPS override' : 'x5 LiveView native') : 'Canon movie FPS';
    const perMin = state.lossless ? `≈ ${(r.lo * 60 / 1024).toFixed(1)}–${(r.hi * 60 / 1024).toFixed(1)} GB` : `${(r.unc * 60 / 1024).toFixed(1)} GB`;
    const focalSel = `<select id="focal" aria-label="Lens focal length">${[24, 35, 50, 62, 85].map((f) => `<option${f === state.focal ? ' selected' : ''}>${f}</option>`).join('')}</select>`;
    const rows = [
      ['Sensor area', `${res(g.sensorW, g.sensorH)}<small>${pct(g.hFov)} × ${pct(g.vFov)} of sensor</small>`],
      ['Recorded frame', `${res(g.rawW, g.rawH)}<small>${binTxt(g)}</small>`],
      ['Final image', `${res(g.outW, g.outH)}<small>${aspectTxt(g.outW, g.outH)}</small>`],
      ['Horizontal crop', `${g.crop.toFixed(2)}×<small>${focalSel} mm frames like ${eq} mm</small>`],
      ['Frame rate', `${fpsTxt(g.fps)} fps<small>${fpsNote}</small>`],
      ['Data rate', state.lossless
        ? `≈ ${r.lo.toFixed(0)}–${r.hi.toFixed(0)} MB/s<small>lossless, est. (raw ${r.unc.toFixed(0)} MB/s)</small>`
        : `${r.unc.toFixed(0)} MB/s<small>uncompressed</small>`],
      ['One minute', perMin],
    ];
    $('stats').innerHTML = rows.map(([k, v]) => `<dt>${k}</dt><dd>${v}</dd>`).join('');
    $('focal').addEventListener('change', (e) => { state.focal = Number(e.target.value); drawStats(geo()); });
  }

  function updateClip(now) {
    const el = $('clip');
    if (state.rec) {
      const s = (now - state.recStart) / 1000;
      const g = geo();
      const r = rates(g);
      const mb = (state.lossless ? (r.lo + r.hi) / 2 : r.unc) * s;
      el.className = 'clip rec';
      el.textContent = `● ${state.recName} · ${tc(s)} · ${fmt(Math.floor(s * g.fps))} frames · ${state.lossless ? '≈ ' : ''}${mb >= 1024 ? (mb / 1024).toFixed(2) + ' GB' : mb.toFixed(0) + ' MB'}`;
    } else {
      el.className = 'clip';
      el.textContent = `Ready · mlv_lite · ${state.bits}-bit ${state.lossless ? 'lossless' : 'uncompressed'}`;
    }
  }

  /* ---------- wiring ---------- */
  $('hotspots').addEventListener('click', (e) => {
    const b = e.target.closest('.hs');
    if (b) press(b.dataset.act, b, e);
  });
  $('stage').addEventListener('keydown', (e) => {
    if (e.target.closest && e.target.closest('.hs') && (e.key === 'Enter' || e.key === ' ')) return;
    const map = { m: 'erase', M: 'erase', Delete: 'erase', Backspace: 'erase', q: 'q', Q: 'q', r: 'rec', R: 'rec', i: 'info', I: 'info', Enter: 'set', ArrowUp: 'up', ArrowDown: 'down', ArrowLeft: 'left', ArrowRight: 'right' };
    const act = map[e.key];
    if (!act) return;
    e.preventDefault();
    press(act, document.querySelector(`.hs[data-act="${act}"]`));
  });

  /* map the 720x480 LCD box onto the screen quad in the photo (coords in 932x716 photo space) */
  const QUAD = [[146, 322], [572, 321], [572, 607], [146, 607]];
  function adj(m) {
    return [m[4] * m[8] - m[5] * m[7], m[2] * m[7] - m[1] * m[8], m[1] * m[5] - m[2] * m[4],
      m[5] * m[6] - m[3] * m[8], m[0] * m[8] - m[2] * m[6], m[2] * m[3] - m[0] * m[5],
      m[3] * m[7] - m[4] * m[6], m[1] * m[6] - m[0] * m[7], m[0] * m[4] - m[1] * m[3]];
  }
  function mulmm(a, b) {
    const c = [];
    for (let i = 0; i < 3; i++) for (let j = 0; j < 3; j++) {
      let v = 0; for (let k = 0; k < 3; k++) v += a[3 * i + k] * b[3 * k + j]; c[3 * i + j] = v;
    }
    return c;
  }
  const mulmv = (m, v) => [m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2], m[6] * v[0] + m[7] * v[1] + m[8] * v[2]];
  function basis(p) {
    const m = [p[0][0], p[1][0], p[2][0], p[0][1], p[1][1], p[2][1], 1, 1, 1];
    const v = mulmv(adj(m), [p[3][0], p[3][1], 1]);
    return mulmm(m, [v[0], 0, 0, 0, v[1], 0, 0, 0, v[2]]);
  }
  function placeLcd() {
    const st = $('stage');
    const sx = st.clientWidth / 932, sy = st.clientHeight / 716;
    const dst = QUAD.map(([x, y]) => [x * sx, y * sy]);
    const src = [[0, 0], [LW, 0], [LW, LH], [0, LH]];
    const t = mulmm(basis(dst), adj(basis(src)));
    for (let i = 0; i < 9; i++) t[i] /= t[8];
    document.querySelector('.lcd-box').style.transform =
      `matrix3d(${[t[0], t[3], 0, t[6], t[1], t[4], 0, t[7], 0, 0, 1, 0, t[2], t[5], 0, t[8]].join(',')})`;
  }
  placeLcd();
  if ('ResizeObserver' in window) new ResizeObserver(placeLcd).observe($('stage'));

  /* Recording / How it works tabs */
  const tabs = [$('tab-rec'), $('tab-set'), $('tab-how')];
  function selectTab(i, focus) {
    tabs.forEach((t, k) => {
      const on = k === i;
      t.setAttribute('aria-selected', String(on));
      t.tabIndex = on ? 0 : -1;
      const p = $(t.getAttribute('aria-controls'));
      p.classList.toggle('active', on);
      p.setAttribute('aria-hidden', String(!on));
    });
    if (focus) tabs[i].focus();
  }
  tabs.forEach((t, i) => {
    t.addEventListener('click', () => selectTab(i));
    t.addEventListener('keydown', (e) => {
      if (e.key === 'ArrowRight' || e.key === 'ArrowLeft') { e.preventDefault(); selectTab((i + (e.key === 'ArrowRight' ? 1 : tabs.length - 1)) % tabs.length, true); }
    });
  });
  selectTab(0);

  $('chk-override').addEventListener('change', (e) => { state.override = e.target.checked; afterChange(); });
  $('chk-compare').addEventListener('change', (e) => { state.compare = e.target.checked; renderCapture(); });
  $('photo').addEventListener('change', (e) => { if (e.target.files[0]) loadPhoto(e.target.files[0]); e.target.value = ''; });
  $('photo-reset').addEventListener('click', () => { scene = sampleScene; $('photo-reset').hidden = true; renderCapture(); requestLcd(); });
  $('reset-steps').addEventListener('click', () => {
    Object.keys(state.steps).forEach((k) => { state.steps[k] = false; });
    if (state.rec) press('rec');
    state.menu = null; renderSteps(); requestLcd();
  });

  let resizeT = 0;
  window.addEventListener('resize', () => { clearTimeout(resizeT); resizeT = setTimeout(() => { renderCapture(); requestLcd(); }, 150); });

  syncPanel();
  renderSteps();
  setViewTarget(false);
  renderCapture();

  const photoSrc = $('stage').dataset.scene || 'assets/img/scene-5d3.webp';
  loadImg(photoSrc).then((img) => {
    sampleScene = makeScene(img, img.width, Math.round(img.width / 1.5));
    if (!scene) scene = sampleScene;
    renderCapture(); requestLcd();
  }).catch(() => toast('The sample photo could not be loaded.'));
  loadML().then(requestLcd).catch(() => toast('Magic Lantern fonts could not be loaded.'));
})();
