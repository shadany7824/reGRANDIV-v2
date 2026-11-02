// The game's keyboard and mouse: GTA IV PC's controls, sent straight to the
// runtime's keyboard/mouse driver (rex_web_game_key / rex_web_game_mouse in
// mnk_input_driver.cpp) as Windows virtual-key codes, the way the PC game
// reads them. Only once the game has started: before that the hub owns them.
// Clicking the picture locks the pointer, so the mouse turns the camera.
(() => {
  const named = {
    Backspace: 0x08, Tab: 0x09, Enter: 0x0D, NumpadEnter: 0x0D, ShiftLeft: 0x10, ShiftRight: 0x10,
    ControlLeft: 0x11, ControlRight: 0x11, AltLeft: 0x12, AltRight: 0x12, CapsLock: 0x14, Escape: 0x1B,
    Space: 0x20, PageUp: 0x21, PageDown: 0x22, End: 0x23, Home: 0x24, ArrowLeft: 0x25, ArrowUp: 0x26,
    ArrowRight: 0x27, ArrowDown: 0x28, Insert: 0x2D, Delete: 0x2E, Backquote: 0xC0, Minus: 0xBD,
    Equal: 0xBB, BracketLeft: 0xDB, BracketRight: 0xDD, Backslash: 0xDC, Semicolon: 0xBA, Quote: 0xDE,
    Comma: 0xBC, Period: 0xBE, Slash: 0xBF, NumpadAdd: 0x6B, NumpadSubtract: 0x6D, NumpadMultiply: 0x6A,
    NumpadDivide: 0x6F, NumpadDecimal: 0x6E,
  };
  const vkFor = code => {
    if (named[code]) return named[code];
    let m = /^Key([A-Z])$/.exec(code);
    if (m) return m[1].charCodeAt(0);
    m = /^Digit([0-9])$/.exec(code);
    if (m) return 0x30 + +m[1];
    m = /^Numpad([0-9])$/.exec(code);
    if (m) return 0x60 + +m[1];
    m = /^F([0-9]{1,2})$/.exec(code);
    if (m && +m[1] >= 1 && +m[1] <= 12) return 0x6F + +m[1];
    return 0;
  };
  let started = false;
  const gameRunning = () => {
    if (!started && Module._rex_web_game_key && (window.bootLog || []).some(l => l.includes('START GAME'))) started = true;
    return started;
  };
  // Shared with touchinput.js, which shows its overlay once the game is running.
  window.rexGameRunning = gameRunning;

  const key = (e, down) => {
    if (!gameRunning()) return;
    const vk = vkFor(e.code);
    if (!vk) return;
    e.preventDefault();  // (Tab, Space and the arrows would move the page)
    if (down && e.repeat) return;
    Module._rex_web_game_key(vk, down ? 1 : 0);
  };
  addEventListener('keydown', e => key(e, true));
  addEventListener('keyup', e => key(e, false));
  // Releasing everything when the page loses focus (keys held then never come up).
  addEventListener('blur', () => {
    if (!started) return;
    for (const vk of [0x01, 0x02, 0x04, 0x10, 0x11, 0x20, 0x41, 0x44, 0x53, 0x57]) Module._rex_web_game_key(vk, 0);
  });

  const stage = document.getElementById('stage') || document.getElementById('scene');
  const buttonVk = [0x01, 0x04, 0x02, 0x05, 0x06];  // left, middle, right, back, forward
  stage.addEventListener('mousedown', e => {
    if (!gameRunning()) return;
    if (document.pointerLockElement !== stage) stage.requestPointerLock?.();
    const vk = buttonVk[e.button];
    if (vk) Module._rex_web_game_key(vk, 1);
    e.preventDefault();
  });
  addEventListener('mouseup', e => {
    if (!started) return;
    const vk = buttonVk[e.button];
    if (vk) Module._rex_web_game_key(vk, 0);
  });
  stage.addEventListener('contextmenu', e => { if (started) e.preventDefault(); });
  addEventListener('mousemove', e => {
    if (!started || document.pointerLockElement !== stage) return;
    if (e.movementX || e.movementY) Module._rex_web_game_mouse(e.movementX | 0, e.movementY | 0, 0);
  });
  stage.addEventListener('wheel', e => {
    if (!gameRunning()) return;
    e.preventDefault();
    // (one detent a notch; the wheel's down is the game's next weapon / station)
    Module._rex_web_game_mouse(0, 0, e.deltaY < 0 ? 1 : e.deltaY > 0 ? -1 : 0);
  }, { passive: false });
})();