// Touch controls: a floating left joystick (-> W/A/S/D), a drag-to-look camera
// on the right half (-> mouse movement), and on-screen buttons (-> keys / mouse
// buttons). It feeds the same entry points as gameinput.js, so the game code
// needs no changes. Only shown on touch devices, once the game has started.
(() => {
  if (!matchMedia('(pointer: coarse)').matches) return;

  // ---- settings ---------------------------------------------------------
  const LOOK_SENS = 1.6;      // camera speed: raise for faster turning
  const STICK_RADIUS = 60;    // px the thumb can move from the stick's centre
  const DEADZONE = 0.3;       // 0..1, how far to push before a key registers

  // Windows virtual-key codes. CHECK these against the pause menu's Controls
  // page and change any that don't match your copy's bindings.
  const BUTTONS = [
    { label: 'FIRE',   vk: 0x01, cls: 'big' },   // left mouse button
    { label: 'AIM',    vk: 0x02 },               // right mouse button
    { label: 'SPRINT', vk: 0x20 },               // Space (also handbrake)
    { label: 'ACTION', vk: 0x46 },               // F: enter / exit vehicle
    { label: 'JUMP',   vk: 0x12 },               // Alt (verify)
    { label: 'COVER',  vk: 0x51 },               // Q (verify)
    { label: 'PHONE',  vk: 0x26 },               // Up arrow (verify)
    { label: 'ENTER',  vk: 0x0D, cls: 'small' }, // menus
    { label: 'ESC',    vk: 0x1B, cls: 'small' }, // pause
  ];
  // -----------------------------------------------------------------------

  const key = (vk, down) => Module._rex_web_game_key?.(vk, down ? 1 : 0);
  const held = new Set();
  const press = (vk, down) => {
    if (down === held.has(vk)) return;
    down ? held.add(vk) : held.delete(vk);
    key(vk, down);
  };
  const releaseAll = () => { for (const vk of [...held]) press(vk, false); };

  const css = document.createElement('style');
  css.textContent = `
    #touch-ui{position:fixed;inset:0;z-index:50;display:none;user-select:none;-webkit-user-select:none;
      -webkit-touch-callout:none;touch-action:none;pointer-events:none}
    #touch-ui.on{display:block}
    #touch-ui .zone{position:absolute;top:0;bottom:0;pointer-events:auto;touch-action:none}
    #touch-ui .left{left:0;width:42%}
    #touch-ui .right{right:0;width:58%}
    #touch-ui .base,#touch-ui .knob{position:absolute;border-radius:50%;display:none;pointer-events:none;
      transform:translate(-50%,-50%)}
    #touch-ui .base{width:${STICK_RADIUS * 2}px;height:${STICK_RADIUS * 2}px;border:2px solid rgba(255,255,255,.45);
      background:rgba(255,255,255,.08)}
    #touch-ui .knob{width:56px;height:56px;background:rgba(255,255,255,.4)}
    #touch-ui .btn{position:absolute;width:62px;height:62px;border-radius:50%;pointer-events:auto;touch-action:none;
      border:2px solid rgba(255,255,255,.5);background:rgba(0,0,0,.35);color:#fff;font:600 11px system-ui,sans-serif;
      display:flex;align-items:center;justify-content:center;letter-spacing:.03em}
    #touch-ui .btn.big{width:84px;height:84px;font-size:13px}
    #touch-ui .btn.small{width:44px;height:30px;border-radius:8px;font-size:10px}
    #touch-ui .btn.down{background:rgba(255,255,255,.35)}
  `;
  document.head.appendChild(css);

  const ui = document.createElement('div');
  ui.id = 'touch-ui';
  const left = Object.assign(document.createElement('div'), { className: 'zone left' });
  const right = Object.assign(document.createElement('div'), { className: 'zone right' });
  const base = Object.assign(document.createElement('div'), { className: 'base' });
  const knob = Object.assign(document.createElement('div'), { className: 'knob' });
  ui.append(left, right, base, knob);

  // Button layout (right side, thumb arc). Positions are [right px, bottom px].
  const pos = {
    FIRE: [28, 40], AIM: [130, 28], SPRINT: [118, 120], ACTION: [20, 140],
    JUMP: [210, 100], COVER: [210, 180], PHONE: [118, 205],
    ENTER: [20, 0], ESC: [74, 0],
  };
  for (const b of BUTTONS) {
    const el = Object.assign(document.createElement('div'), { className: 'btn ' + (b.cls || ''), textContent: b.label });
    const [r, bt] = pos[b.label] || [20, 20];
    el.style.right = r + 'px';
    el.style.bottom = (bt + (b.cls === 'small' ? 8 : 0)) + 'px';
    if (b.cls === 'small') { el.style.top = '8px'; el.style.bottom = 'auto'; }
    el.addEventListener('pointerdown', e => {
      e.preventDefault(); el.setPointerCapture(e.pointerId); el.classList.add('down'); press(b.vk, true);
    });
    const up = e => { el.classList.remove('down'); press(b.vk, false); };
    el.addEventListener('pointerup', up);
    el.addEventListener('pointercancel', up);
    ui.appendChild(el);
  }
  document.body.appendChild(ui);

  // ---- left stick -> WASD ------------------------------------------------
  let stickId = null, ox = 0, oy = 0;
  const setKeys = (dx, dy) => {
    press(0x57, dy < -DEADZONE);  // W
    press(0x53, dy > DEADZONE);   // S
    press(0x41, dx < -DEADZONE);  // A
    press(0x44, dx > DEADZONE);   // D
  };
  const stickEnd = () => {
    stickId = null; base.style.display = knob.style.display = 'none'; setKeys(0, 0);
  };
  left.addEventListener('pointerdown', e => {
    if (stickId !== null) return;
    e.preventDefault(); left.setPointerCapture(e.pointerId);
    stickId = e.pointerId; ox = e.clientX; oy = e.clientY;
    for (const el of [base, knob]) { el.style.left = ox + 'px'; el.style.top = oy + 'px'; el.style.display = 'block'; }
  });
  left.addEventListener('pointermove', e => {
    if (e.pointerId !== stickId) return;
    let dx = e.clientX - ox, dy = e.clientY - oy;
    const len = Math.hypot(dx, dy);
    if (len > STICK_RADIUS) { dx *= STICK_RADIUS / len; dy *= STICK_RADIUS / len; }
    knob.style.left = (ox + dx) + 'px'; knob.style.top = (oy + dy) + 'px';
    setKeys(dx / STICK_RADIUS, dy / STICK_RADIUS);
  });
  left.addEventListener('pointerup', e => { if (e.pointerId === stickId) stickEnd(); });
  left.addEventListener('pointercancel', e => { if (e.pointerId === stickId) stickEnd(); });

  // ---- right side drag -> camera (mouse movement) ------------------------
  let lookId = null, lx = 0, ly = 0, remX = 0, remY = 0;
  right.addEventListener('pointerdown', e => {
    if (lookId !== null) return;
    e.preventDefault(); right.setPointerCapture(e.pointerId);
    lookId = e.pointerId; lx = e.clientX; ly = e.clientY;
  });
  right.addEventListener('pointermove', e => {
    if (e.pointerId !== lookId) return;
    remX += (e.clientX - lx) * LOOK_SENS; remY += (e.clientY - ly) * LOOK_SENS;
    lx = e.clientX; ly = e.clientY;
    const mx = remX | 0, my = remY | 0;   // keep the fractions so slow drags aren't lost
    remX -= mx; remY -= my;
    if (mx || my) Module._rex_web_game_mouse?.(mx, my, 0);
  });
  const lookEnd = e => { if (e.pointerId === lookId) lookId = null; };
  right.addEventListener('pointerup', lookEnd);
  right.addEventListener('pointercancel', lookEnd);

  // ---- housekeeping ------------------------------------------------------
  // Fullscreen + landscape on the first touch (needs a user gesture).
  ui.addEventListener('pointerdown', async () => {
    if (document.fullscreenElement) return;
    try { await document.documentElement.requestFullscreen(); } catch {}
    try { await screen.orientation.lock('landscape'); } catch {}
  }, { once: true });
  document.addEventListener('visibilitychange', () => { if (document.hidden) { releaseAll(); stickEnd(); } });
  addEventListener('blur', () => { releaseAll(); stickEnd(); });
  ui.addEventListener('contextmenu', e => e.preventDefault());

  // Show the overlay only once the game has started (before that the hub owns input).
  const timer = setInterval(() => {
    if (window.rexGameRunning?.()) { ui.classList.add('on'); clearInterval(timer); }
  }, 500);
})();