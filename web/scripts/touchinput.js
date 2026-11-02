// Touch controls for phones and tablets. They are a controller to the game:
// this page's browser reports them as a gamepad (navigator.getGamepads), so
// SDL and the hub read them like any pad and the game gets its console
// controls, its own help text and the "touch" button prompt set (the Xbox
// 360's buttons, and a Controls picture of this layout: rpf_button_prompts.cpp).
// Made for thumbs, as phone games do it:
// - left half: a stick wherever the thumb lands; pushed to its edge on foot
//   it runs (A held)
// - right half: drag to look; the camera turns as fast as the finger moves
// - a few big buttons named for what they do, which change with where the
//   player is: on foot, in a vehicle, or in a menu or on the phone (the
//   button's letter in a corner, for the game's "press Y" help)
// - a thumb can slide from one button onto the next
// Loaded before the game so SDL finds the pad when it starts. On by itself on
// touch screens; the Touch button by Fullscreen turns them on or off (kept in
// this browser), and ?touch=1 / ?touch=0 overrides both. Shown only once the
// game runs: the hub takes taps and swipes itself (webui.js).
(() => {
  const asked = /[?&]touch=([01])/.exec(location.search);
  const touchScreen = navigator.maxTouchPoints > 0;
  let kept = null;
  try { kept = localStorage.getItem('gta4-touch'); } catch {}
  let enabled = asked ? asked[1] === '1'
              : kept !== null ? kept === '1'
              : touchScreen && matchMedia('(pointer: coarse)').matches;
  // (no button on a computer without a touch screen, unless asked for)
  if (!touchScreen && !asked) return;

  // ---- the gamepad ----------------------------------------------------------
  // The standard layout: buttons A B X Y, LB RB, LT RT, Back Start, L3 R3, up
  // down left right, guide; axes left x y, right x y.
  const B = { a: 0, b: 1, x: 2, y: 3, lb: 4, rb: 5, lt: 6, rt: 7, back: 8, start: 9, l3: 10, r3: 11,
              up: 12, down: 13, left: 14, right: 15 };
  const buttons = Array.from({ length: 17 }, () => ({ pressed: false, touched: false, value: 0 }));
  const axes = [0, 0, 0, 0];
  // (in its own slot after the browser's four, so a real controller keeps its
  // place; no vendor in the name: SDL's default mapping for a standard pad)
  const SLOT = 4;
  let lastBuzz = 0;
  const pad = {
    id: 'Touch Controls (STANDARD GAMEPAD)', index: SLOT, connected: true, mapping: 'standard',
    timestamp: performance.now(), axes, buttons,
    // The game's rumble, as a short buzz where the browser has one (Android).
    vibrationActuator: {
      effects: ['dual-rumble'],
      playEffect: (type, p = {}) => {
        const strength = Math.max(p.strongMagnitude || 0, p.weakMagnitude || 0);
        const now = performance.now();
        if (strength > 0.25 && now - lastBuzz > 150) { lastBuzz = now; navigator.vibrate?.(Math.round(40 + 60 * strength)); }
        return Promise.resolve('complete');
      },
      reset: () => Promise.resolve('complete'),
    },
  };
  const changed = () => { pad.timestamp = performance.now(); };
  const browserPads = navigator.getGamepads ? navigator.getGamepads.bind(navigator) : () => [];
  Object.defineProperty(navigator, 'getGamepads', {
    configurable: true,
    value: () => {
      const list = Array.from(browserPads());
      while (list.length < SLOT) list.push(null);
      list[SLOT] = enabled ? pad : null;
      return list;
    },
  });
  // Plugged in or pulled out, as a controller is (SDL takes both while the
  // game runs; the prompts the game shows are picked when it starts).
  const announce = () => {
    pad.connected = enabled;
    const e = new Event(enabled ? 'gamepadconnected' : 'gamepaddisconnected');
    Object.defineProperty(e, 'gamepad', { value: pad });
    dispatchEvent(e);
  };
  // (for a listener added after the page has loaded, as SDL's may be)
  addEventListener('load', () => { if (enabled) announce(); });

  // Each button can be held by several things at once (a finger on it, the
  // stick at its edge): it's down while any of them holds it.
  const holders = buttons.map(() => new Set());
  const hold = (i, who, down) => {
    down ? holders[i].add(who) : holders[i].delete(who);
    const now = holders[i].size > 0;
    if (buttons[i].pressed === now) return;
    buttons[i].pressed = buttons[i].touched = now;
    buttons[i].value = now ? 1 : 0;
    changed();
  };
  const setAxes = (first, x, y) => {
    if (axes[first] === x && axes[first + 1] === y) return;
    axes[first] = x; axes[first + 1] = y;
    changed();
  };

  // ---- the overlay ------------------------------------------------------------
  // Sizes in vmin, so a phone and a tablet both get thumb-sized buttons.
  const css = document.createElement('style');
  css.textContent = `
    #touch { position: fixed; inset: 0; z-index: 2; display: none; pointer-events: none; user-select: none;
      -webkit-user-select: none; -webkit-touch-callout: none; touch-action: none; font-family: system-ui, sans-serif; }
    #touch.on { display: block; }
    #touch .zone { position: absolute; top: 0; bottom: 0; pointer-events: auto; touch-action: none; }
    #touch .zone.left { left: 0; width: 45%; }
    #touch .zone.right { right: 0; width: 55%; }
    #touch .ring, #touch .knob { position: absolute; border-radius: 50%; pointer-events: none; transform: translate(-50%, -50%); }
    #touch .ring { width: 28vmin; height: 28vmin; border: 0.5vmin solid rgba(255,255,255,.35); background: rgba(0,0,0,.18); }
    #touch .knob { width: 12vmin; height: 12vmin; background: rgba(255,255,255,.42); }
    #touch .ring.idle, #touch .knob.idle { opacity: .4; }
    #touch .ring.run { border-color: rgba(143,220,90,.85); }
    /* (in a menu the d-pad is where the stick was) */
    #touch.inmenu .zone.left, #touch.inmenu .ring, #touch.inmenu .knob { display: none; }
    #touch .btn { position: absolute; pointer-events: auto; touch-action: none; box-sizing: border-box;
      border-radius: 50%; border: 0.45vmin solid rgba(255,255,255,.55); background: rgba(0,0,0,.4); color: #fff;
      display: none; align-items: center; justify-content: center; transform: translate(-50%, 50%);
      font-weight: 700; letter-spacing: .02em; line-height: 1.05; text-align: center; white-space: pre; }
    #touch .btn.shown { display: flex; }
    /* (a touch a little outside the ring still counts) */
    #touch .btn::after { content: ''; position: absolute; inset: -1.6vmin; border-radius: inherit; }
    /* (the letter just outside the button's top right, clear of its name) */
    #touch .btn i { position: absolute; top: -1.6vmin; right: -1.2vmin; font-style: normal; font-size: 2.2vmin;
      text-shadow: 0 0 .5vmin #000, 0 0 .3vmin #000; }
    #touch .btn i:empty { display: none; }
    #touch .btn.flat { border-radius: 2vmin; }
    #touch .btn.down { background: rgba(255,255,255,.42); }
    #touch .btn.a i { color: #8fdc5a; } #touch .btn.b i { color: #ff6b5e; }
    #touch .btn.x i { color: #5aa6ff; } #touch .btn.y i { color: #ffd23c; }
  `;
  document.head.appendChild(css);

  const ui = document.createElement('div');
  ui.id = 'touch';
  const zone = side => Object.assign(document.createElement('div'), { className: 'zone ' + side });
  const leftZone = zone('left'), rightZone = zone('right');
  ui.append(leftZone, rightZone);

  // The buttons, and where each is in each mode: [x, y, size, name] in vmin,
  // x from the left edge (from the right when negative, from the middle as a
  // string), y up from the bottom; a mode it isn't in: not shown there. The
  // names are what the pause menu's Controls page calls them.
  const layout = [
    // button, its letter, colour class, round or flat
    [B.rt, 'RT', '', '', {
      foot: [-16, 21, 20, 'FIRE'], vehicle: [-16, 21, 20, 'GAS'] }],
    [B.lt, 'LT', '', '', {
      foot: [-39, 13, 15, 'AIM'], vehicle: [-39, 13, 17, 'BRAKE'] }],
    [B.x, 'X', 'x', '', {
      foot: [-12, 46, 13, 'JUMP'], menu: [-35, 15, 12, ''] }],
    [B.y, 'Y', 'y', '', {
      foot: [-31, 38, 12, 'ENTER'], vehicle: [-34, 40, 12, 'EXIT'], menu: [-35, 37, 12, ''] }],
    [B.b, 'B', 'b', '', {
      foot: [-29, 57, 10, 'RELOAD'], menu: [-14, 39, 15, 'BACK'] }],
    [B.a, 'A', 'a', '', {
      menu: [-14, 16, 17, 'SELECT'] }],
    [B.rb, 'RB', '', 'flat', {
      foot: [-12, 63, 11, 'COVER'], vehicle: [-14, 46, 13, 'HAND\nBRAKE'], menu: [-12, 62, 11, 'TAB ▶'] }],
    [B.lb, 'LB', '', 'flat', {
      foot: [23, 80, 9, 'PICK\nUP'], vehicle: [-14, 64, 10, 'SHOOT'], menu: [12, 62, 11, '◀ TAB'] }],
    [B.r3, 'R3', '', '', {
      foot: [-46, 44, 9, 'LOOK\nBACK'], vehicle: [-32, 59, 9, 'LOOK\nBACK'] }],
    [B.l3, 'L3', '', '', {
      foot: [64, 9, 10, 'CROUCH'] }],
    [B.up, '', '', 'flat', {
      foot: [9, 80, 9, 'PHONE'], vehicle: [9, 80, 9, 'PHONE'], menu: [20, 44, 11, '▲'] }],
    [B.down, '', '', 'flat', {
      menu: [20, 16, 11, '▼'] }],
    [B.left, '', '', 'flat', {
      foot: [9, 66, 9, '◀ GUN'], vehicle: [9, 66, 9, '◀ RADIO'], menu: [7, 30, 11, '◀'] }],
    [B.right, '', '', 'flat', {
      foot: [23, 66, 9, 'GUN ▶'], vehicle: [23, 66, 9, 'RADIO ▶'], menu: [33, 30, 11, '▶'] }],
    [B.back, '', '', 'flat', {
      foot: ['-9', 88, 8, 'CAMERA'], vehicle: ['-9', 88, 8, 'CAMERA'] }],
    [B.start, '', '', 'flat', {
      foot: ['9', 88, 8, 'PAUSE'], vehicle: ['9', 88, 8, 'PAUSE'], menu: ['0', 88, 8, 'RESUME'] }],
  ];
  const all = [];
  for (const [index, letter, colour, shape, modes] of layout) {
    const el = document.createElement('div');
    el.className = `btn ${colour} ${shape}`;
    const word = document.createElement('span');
    const badge = document.createElement('i');
    badge.textContent = letter;
    el.append(word, badge);
    ui.appendChild(el);
    all.push({ el, word, index, modes });
  }
  const byElement = new Map(all.map(b => [b.el, b]));

  // A finger on the buttons: it presses the one it's on, and slides onto
  // another (fire while aiming, from one to the next) without lifting.
  const fingers = new Map();  // pointer id -> the button it holds
  const press = (pointer, button) => {
    const old = fingers.get(pointer);
    if (old === button) return;
    if (old) { old.el.classList.remove('down'); hold(old.index, pointer, false); }
    if (button) {
      button.el.classList.add('down');
      hold(button.index, pointer, true);
      navigator.vibrate?.(8);
      fingers.set(pointer, button);
    } else fingers.delete(pointer);
  };
  const buttonAt = (x, y) => {
    const el = document.elementFromPoint(x, y)?.closest?.('#touch .btn.shown');
    return el ? byElement.get(el) : null;
  };
  for (const b of all) {
    b.el.addEventListener('pointerdown', e => {
      e.preventDefault();
      e.stopPropagation();
      b.el.setPointerCapture(e.pointerId);
      press(e.pointerId, b);
    });
    b.el.addEventListener('pointermove', e => {
      if (!fingers.has(e.pointerId)) return;
      const under = buttonAt(e.clientX, e.clientY);
      if (under) press(e.pointerId, under);
    });
    const up = e => press(e.pointerId, null);
    b.el.addEventListener('pointerup', up);
    b.el.addEventListener('pointercancel', up);
  }

  let mode = 'foot';
  const vmin = () => Math.min(innerWidth, innerHeight) / 100;
  const place = () => {
    const u = vmin();
    for (const b of all) {
      const at = b.modes[mode];
      if (!at) {
        // (a button going away lets go of what held it)
        for (const [id, held] of [...fingers]) if (held === b) press(id, null);
        b.el.classList.remove('shown');
        continue;
      }
      const [x, y, size, name] = at;
      b.el.classList.add('shown');
      b.el.style.left = `${typeof x === 'string' ? innerWidth / 2 + +x * u : x < 0 ? innerWidth + x * u : x * u}px`;
      b.el.style.bottom = `${y * u}px`;
      b.el.style.width = `${size * u}px`;
      b.el.style.height = `${(b.el.classList.contains('flat') ? Math.min(size, 9) * 0.8 : size) * u}px`;
      b.word.textContent = name;
      // (the name as big as fits: its longest line across the button)
      const longest = Math.max(1, ...name.split('\n').map(s => s.length));
      b.word.style.fontSize = `${Math.min(size * 0.3, size * 1.25 / longest) * u}px`;
    }
  };
  addEventListener('resize', place);

  // ---- the sticks -------------------------------------------------------------
  const ring = Object.assign(document.createElement('div'), { className: 'ring idle' });
  const knob = Object.assign(document.createElement('div'), { className: 'knob idle' });
  ui.append(ring, knob);
  const put = (el, x, y) => { el.style.left = x + 'px'; el.style.top = y + 'px'; };
  // Moving: from where the thumb lands, out to the ring's edge; at rest it
  // waits at its usual place. Pushed to the edge on foot, it runs.
  const REACH = 12, RUN_AT = 0.97;
  let moveId = null, ox = 0, oy = 0;
  const rest = () => {
    // (right of the game's radar, which is in the corner)
    const x = 46 * vmin(), y = innerHeight - 24 * vmin();
    put(ring, x, y); put(knob, x, y);
    ring.classList.add('idle'); knob.classList.add('idle'); ring.classList.remove('run');
  };
  leftZone.addEventListener('pointerdown', e => {
    if (moveId !== null) return;
    e.preventDefault();
    leftZone.setPointerCapture(e.pointerId);
    moveId = e.pointerId; ox = e.clientX; oy = e.clientY;
    put(ring, ox, oy); put(knob, ox, oy);
    ring.classList.remove('idle'); knob.classList.remove('idle');
  });
  leftZone.addEventListener('pointermove', e => {
    if (e.pointerId !== moveId) return;
    const r = REACH * vmin();
    let dx = e.clientX - ox, dy = e.clientY - oy;
    // (a thumb gone well past the ring drags it along, so coming back slows
    // it at once)
    const far = Math.hypot(dx, dy) - r * 1.4;
    if (far > 0) {
      const f = far / Math.hypot(dx, dy);
      ox += dx * f; oy += dy * f; put(ring, ox, oy);
      dx = e.clientX - ox; dy = e.clientY - oy;
    }
    const length = Math.hypot(dx, dy);
    if (length > r) { dx *= r / length; dy *= r / length; }
    put(knob, ox + dx, oy + dy);
    setAxes(0, +(dx / r).toFixed(3), +(dy / r).toFixed(3));
    const run = mode === 'foot' && length >= r * RUN_AT;
    ring.classList.toggle('run', run);
    hold(B.a, 'stick', run);
  });
  const moveEnd = e => {
    if (e && e.pointerId !== moveId) return;
    moveId = null;
    setAxes(0, 0, 0);
    hold(B.a, 'stick', false);
    rest();
  };
  leftZone.addEventListener('pointerup', moveEnd);
  leftZone.addEventListener('pointercancel', moveEnd);
  addEventListener('resize', () => { if (moveId === null) rest(); });
  rest();

  // Looking: the finger's movement since the last frame is the right stick
  // for that frame, so the camera turns with the finger and stops with it.
  const LOOK = 2.2;  // (vmin a frame for the stick's full turn)
  let lookId = null, lx = 0, ly = 0, sumX = 0, sumY = 0, lookX = 0, lookY = 0;
  rightZone.addEventListener('pointerdown', e => {
    if (lookId !== null) return;
    e.preventDefault();
    rightZone.setPointerCapture(e.pointerId);
    lookId = e.pointerId; lx = e.clientX; ly = e.clientY;
  });
  rightZone.addEventListener('pointermove', e => {
    if (e.pointerId !== lookId) return;
    sumX += e.clientX - lx; sumY += e.clientY - ly;
    lx = e.clientX; ly = e.clientY;
  });
  const lookEnd = e => { if (e && e.pointerId !== lookId) return; lookId = null; sumX = sumY = 0; };
  rightZone.addEventListener('pointerup', lookEnd);
  rightZone.addEventListener('pointercancel', lookEnd);
  const frame = () => {
    const full = LOOK * vmin();
    const clamp = v => Math.max(-1, Math.min(1, v));
    // (partly the last frame's: a drag doesn't stutter between pointer events)
    lookX = clamp(sumX / full) * 0.6 + lookX * 0.4;
    lookY = clamp(sumY / full) * 0.6 + lookY * 0.4;
    if (Math.abs(lookX) < 0.02) lookX = 0;
    if (Math.abs(lookY) < 0.02) lookY = 0;
    sumX = sumY = 0;
    setAxes(2, +lookX.toFixed(3), +lookY.toFixed(3));
    requestAnimationFrame(frame);
  };
  requestAnimationFrame(frame);

  const releaseAll = () => {
    for (const id of [...fingers.keys()]) press(id, null);
    moveEnd(); lookEnd();
    for (let i = 0; i < buttons.length; i++) { holders[i].clear(); hold(i, null, false); }
  };
  addEventListener('blur', releaseAll);
  document.addEventListener('visibilitychange', () => { if (document.hidden) releaseAll(); });
  ui.addEventListener('contextmenu', e => e.preventDefault());
  // Full screen, sideways, on the first touch in the game (it needs a touch).
  const fill = async e => {
    if (e.pointerType !== 'touch') return;
    ui.removeEventListener('pointerdown', fill, true);
    if (document.fullscreenElement) return;
    try { await document.documentElement.requestFullscreen({ navigationUI: 'hide' }); } catch {}
    try { await screen.orientation.lock('landscape'); } catch {}
  };
  ui.addEventListener('pointerdown', fill, true);

  // ---- shown in the game, with the buttons for where the player is ----------
  let placed = false;
  const update = () => {
    const state = window.Module?._rex_web_play_state ? Module._rex_web_play_state() : 0;
    const now = state & 2 ? 'menu' : state & 1 ? 'vehicle' : 'foot';
    if (now === mode && placed) return;
    if (now === 'menu') moveEnd();
    mode = now;
    placed = true;
    ui.classList.toggle('inmenu', mode === 'menu');
    place();
  };
  let running = false;
  const show = () => ui.classList.toggle('on', enabled && running);
  const gameRunning = () => !!(window.Module && Module._rex_web_game_key &&
                               (window.bootLog || []).some(l => l.includes('START GAME')));
  // The Touch button, by Log and Fullscreen.
  const toggle = Object.assign(document.createElement('button'), { type: 'button', id: 'touch-button' });
  const name = () => { toggle.textContent = enabled ? 'Touch: On' : 'Touch: Off'; };
  toggle.addEventListener('click', () => {
    enabled = !enabled;
    try { localStorage.setItem('gta4-touch', enabled ? '1' : '0'); } catch {}
    if (!enabled) releaseAll();
    announce();
    name();
    show();
  });
  name();
  const attach = () => {
    document.body.appendChild(ui);
    const bar = document.getElementById('bar');
    bar ? bar.prepend(toggle) : document.body.appendChild(toggle);
  };
  if (document.body) attach(); else addEventListener('DOMContentLoaded', attach);
  const timer = setInterval(() => {
    if (!gameRunning()) return;
    clearInterval(timer);
    running = true;
    show();
    update();
    setInterval(update, 200);
  }, 500);
})();
