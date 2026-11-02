ObjC.import('AppKit');
// The pause menu's Controls picture (frontend_360.xtd controller.dds) drawn
// for another controller: its own body and layout, with the leader lines
// starting at the rows the game prints its labels at (the Xbox 360
// picture's: left edge LT LB guide back LS L3 up left down right, right edge
// RT RB Y B A X RS R3 start) and running to that controller's control.
// Usage: osascript -l JavaScript controller_draw.js <out.png> <ps|xbox|switch|keys_foot|keys_vehicle|touch>
function run(argv) {
  const [output, style] = argv;
  const size = 512;
  const rep = $.NSBitmapImageRep.alloc.initWithBitmapDataPlanesPixelsWidePixelsHighBitsPerSampleSamplesPerPixelHasAlphaIsPlanarColorSpaceNameBytesPerRowBitsPerPixel(null, size, size, 8, 4, true, false, $.NSDeviceRGBColorSpace, 0, 0);
  const ctx = $.NSGraphicsContext.graphicsContextWithBitmapImageRep(rep);
  $.NSGraphicsContext.saveGraphicsState; $.NSGraphicsContext.setCurrentContext(ctx);
  const P = (x, y) => $.NSMakePoint(x, size - y);
  const rgb = (r, g, b, a = 1) => $.NSColor.colorWithDeviceRedGreenBlueAlpha(r / 255, g / 255, b / 255, a);
  const black = rgb(8, 8, 8), light = rgb(232, 232, 232), lineGrey = rgb(125, 123, 125);
  const fillStroke = (path, fill, edge, width = 3) => {
    if (fill) { fill.setFill; path.fill; }
    if (edge) { edge.setStroke; path.setLineWidth(width); path.stroke; }
  };
  const oval = (x, y, rx, ry = rx) => $.NSBezierPath.bezierPathWithOvalInRect($.NSMakeRect(x - rx, size - y - ry, 2 * rx, 2 * ry));
  const rounded = (x, y, w, h, r) => $.NSBezierPath.bezierPathWithRoundedRectXRadiusYRadius($.NSMakeRect(x - w / 2, size - y - h / 2, w, h), r, r);
  const poly = (points, closed = true) => {
    const p = $.NSBezierPath.bezierPath;
    p.moveToPoint(P(points[0][0], points[0][1]));
    for (const [x, y] of points.slice(1)) p.lineToPoint(P(x, y));
    if (closed) p.closePath;
    p.setLineJoinStyle(1); p.setLineCapStyle(1);
    return p;
  };
  // A smooth closed outline through points (Catmull-Rom as cubic Béziers).
  const smooth = points => {
    const p = $.NSBezierPath.bezierPath, n = points.length;
    p.moveToPoint(P(points[0][0], points[0][1]));
    for (let i = 0; i < n; i++) {
      const p0 = points[(i - 1 + n) % n], p1 = points[i], p2 = points[(i + 1) % n], p3 = points[(i + 2) % n];
      p.curveToPointControlPoint1ControlPoint2(P(p2[0], p2[1]),
        P(p1[0] + (p2[0] - p0[0]) / 6, p1[1] + (p2[1] - p0[1]) / 6),
        P(p2[0] - (p3[0] - p1[0]) / 6, p2[1] - (p3[1] - p1[1]) / 6));
    }
    p.closePath;
    return p;
  };
  const mirror = half => half.concat(half.slice().reverse().map(([x, y]) => [size - x, y]));
  const text = (s, x, y, fontSize, color) => {
    const font = $.NSFont.fontWithNameSize('HelveticaNeue-Bold', fontSize) || $.NSFont.boldSystemFontOfSize(fontSize);
    const attrs = $.NSDictionary.dictionaryWithObjectsForKeys($([font, color]), $([$.NSFontAttributeName, $.NSForegroundColorAttributeName]));
    const ts = $(s).sizeWithAttributes(attrs);
    $(s).drawAtPointWithAttributes($.NSMakePoint(x - ts.width / 2, size - y - ts.height / 2), attrs);
  };
  const stick = (x, y, body) => {
    fillStroke(oval(x, y, 30), rgb(22, 22, 24), black, 3);
    fillStroke(oval(x, y, 21), body, rgb(70, 70, 74), 2);
    fillStroke(oval(x, y, 13), rgb(46, 46, 50), null);
  };
  const dpad = (x, y, body, arm = 15, half = 8) => {
    const cross = poly([[x - half, y - arm - half], [x + half, y - arm - half], [x + half, y - half], [x + arm + half, y - half],
      [x + arm + half, y + half], [x + half, y + half], [x + half, y + arm + half], [x - half, y + arm + half], [x - half, y + half],
      [x - arm - half, y + half], [x - arm - half, y - half], [x - half, y - half]]);
    fillStroke(cross, body, black, 3);
  };

  // The keyboard and mouse: each row's line ends at a keycap with the PC key
  // its action is bound to (gta4_input_hooks.cpp kButtonBindings; A, B, Y and
  // the d-pad are the keyboard's controller buttons, gta4_keyboard_controller.h:
  // Enter, Backspace, F and the arrows), on foot or
  // in a vehicle as the page shows (the web build swaps the two pictures);
  // a keyboard and mouse drawn between.
  if (style === 'keys_foot' || style === 'keys_vehicle') {
    const foot = style === 'keys_foot';
    const leftKeys = foot ? ['RMB', 'E', null, 'V', 'WASD', 'CTRL', '\u2191', '\u2190', '\u2193', '\u2192']
                          : ['S', 'LMB', null, 'V', 'A / D', 'CAPS', '\u2191', '\u2190', '\u2193', '\u2192'];
    const rightKeys = foot ? ['LMB', 'Q', 'F', 'R', 'SHIFT', 'SPACE', 'MOUSE', 'C', 'ESC']
                           : ['W', 'SPACE', 'F', 'BKSP', 'ENTER', 'H', 'MOUSE', 'C', 'ESC'];
    const leftRows = [53, 80, 109.5, 135.5, 162, 189, 216, 243, 270, 297.5];
    const rightRows = [53, 80, 134.5, 162, 189, 216, 243, 270, 297.5];
    // keyboard
    fillStroke(rounded(256, 150, 250, 118, 12), rgb(40, 40, 44), black, 4);
    const keyRow = (y, count, w, x0) => { for (let i = 0; i < count; i++) fillStroke(rounded(x0 + i * (w + 4) + w / 2, y, w, 18, 3), rgb(62, 62, 68), black, 1.5); };
    keyRow(108, 11, 16, 145); keyRow(130, 11, 16, 151); keyRow(152, 10, 16, 157); keyRow(174, 9, 16, 163);
    fillStroke(rounded(256, 196, 110, 18, 3), rgb(62, 62, 68), black, 1.5);
    // (W A S D lit)
    for (const [x, y] of [[191, 130], [177, 152], [197, 152], [217, 152]]) fillStroke(rounded(x, y, 16, 18, 3), rgb(150, 150, 158), black, 1.5);
    // mouse
    fillStroke(rounded(256, 262, 54, 78, 26), rgb(40, 40, 44), black, 4);
    fillStroke(poly([[229, 252], [283, 252]], false), null, black, 2.5); fillStroke(poly([[256, 224], [256, 252]], false), null, black, 2.5);
    fillStroke(rounded(256, 238, 6, 14, 3), rgb(150, 150, 158), black, 1.5);
    const cap = (x, y, label) => {
      const w = Math.max(30, label.length * 9 + 14);
      fillStroke(rounded(x, y, w, 21, 5), rgb(232, 232, 232), black, 2.5);
      text(label, x, y + 0.5, label.length > 3 ? 11 : 13, rgb(20, 20, 20));
      return w;
    };
    const leads = [];
    leftRows.forEach((row, i) => { if (leftKeys[i]) leads.push([[0, row], [58, row], leftKeys[i], true]); });
    rightRows.forEach((row, i) => { if (rightKeys[i]) leads.push([[size, row], [size - 58, row], rightKeys[i], false]); });
    for (const [a, b] of leads) fillStroke(poly([a, b], false), null, black, 6);
    for (const [a, b] of leads) fillStroke(poly([a, b], false), null, lineGrey, 2);
    for (const [, [x, y], label, leftSide] of leads) {
      const w = Math.max(30, label.length * 9 + 14);
      cap(leftSide ? x + w / 2 : x - w / 2, y, label);
    }
    $.NSGraphicsContext.restoreGraphicsState;
    rep.representationUsingTypeProperties($.NSBitmapImageFileTypePNG, $({})).writeToFileAtomically(output, true);
    return 'ok';
  }

  // The touch controls (web/scripts/touchinput.js): a phone on its side with
  // the buttons the page shows on foot, where it shows them, each row's line
  // ending at that button. Positions as touchinput.js has them, in hundredths
  // of the screen's height from its edges (x < 0: from the right, a string:
  // from the middle).
  if (style === 'touch') {
    // (the pause menu shows the picture down to about y 325: the phone fits above that)
    const sx = 91, sy = 118, sw = 330, sh = 330 * 9 / 16, u = sh / 100;
    const at = ([x, y]) => [typeof x === 'string' ? sx + sw / 2 + +x * u : x < 0 ? sx + sw + x * u : sx + x * u, sy + sh - y * u];
    // the phone and its screen
    fillStroke(rounded(sx + sw / 2, sy + sh / 2, sw + 34, sh + 22, 26), rgb(30, 30, 33), black, 5);
    fillStroke(rounded(sx + sw / 2, sy + sh / 2, sw, sh, 8), rgb(70, 78, 86), black, 2);
    fillStroke(rounded(sx + sw / 2, sy + sh * 0.78, sw - 4, sh * 0.42, 6), rgb(58, 62, 64), null);
    fillStroke(oval(sx - 9, sy + sh / 2, 3), rgb(60, 60, 64), null);
    const stick = at([46, 24]), stickR = 14 * u, look = at([-62, 34]);
    const C = {
      rt: at([-16, 21]), lt: at([-39, 13]), x: at([-12, 46]), y: at([-31, 38]), b: at([-29, 57]),
      rb: at([-12, 63]), lb: at([23, 80]), r3: at([-46, 44]), l3: at([64, 9]), up: at([9, 80]),
      left: at([9, 66]), right: at([23, 66]), back: at(['-9', 88]), start: at(['9', 88]),
      run: [stick[0] + stickR * 0.7, stick[1] - stickR * 0.7],
    };
    const left = [[53, C.lt], [80, C.lb], [135.5, C.back], [162, stick], [189, C.l3],
      [216, C.up], [243, C.left], [297.5, C.right]];
    const right = [[53, C.rt], [80, C.rb], [134.5, C.y], [162, C.b], [189, C.run], [216, C.x], [243, look],
      [270, C.r3], [297.5, C.start]];
    const lead = (row, anchor, leftSide) => {
      const edge = leftSide ? 0 : size, elbow = leftSide ? 38 : size - 38;
      // (AIM's row runs level above the phone and drops to it past the
      // others; PAUSE's goes round the phone's right end and over its top)
      if (anchor === C.lt || anchor === C.rt) return [[edge, row], [anchor[0], row], anchor];
      if (anchor === C.start) return [[edge, row], [size - 24, row], [size - 24, 104], [anchor[0], 104], anchor];
      return [[edge, row], [elbow, row], anchor];
    };
    const leads = left.map(([row, a]) => lead(row, a, true)).concat(right.map(([row, a]) => lead(row, a, false)));
    for (const points of leads) fillStroke(poly(points, false), null, black, 6);
    for (const points of leads) fillStroke(poly(points, false), null, lineGrey, 2);
    // the stick, its edge (running) in green; the camera's drag
    fillStroke(oval(...stick, stickR), rgb(20, 20, 22, 0.55), light, 2);
    fillStroke(oval(...stick, 6 * u), rgb(200, 200, 205), null);
    fillStroke(oval(...C.run, 2.4), rgb(143, 220, 90), black, 1);
    fillStroke(oval(...look, 9 * u), null, rgb(200, 200, 205, 0.7), 1.5);
    text('↔', look[0], look[1] - 1, 13, light);
    const round = (p, d, label, fontSize) => {
      fillStroke(oval(...p, d * u / 2), rgb(16, 16, 18), light, 2);
      text(label, p[0], p[1] + 0.5, fontSize, light);
    };
    const flat = (p, w, label, fontSize) => {
      fillStroke(rounded(...p, w * u, Math.min(w, 9) * 0.8 * u, 2 * u), rgb(16, 16, 18), light, 1.6);
      text(label, p[0], p[1] + 0.5, fontSize, light);
    };
    round(C.rt, 20, 'FIRE', 9); round(C.lt, 15, 'AIM', 8); round(C.x, 13, 'JUMP', 6.5);
    round(C.y, 12, 'ENTER', 5.5); round(C.b, 10, 'RELOAD', 4.2); round(C.r3, 9, 'BACK', 4.5);
    round(C.l3, 10, 'CROUCH', 4.2);
    flat(C.rb, 11, 'COVER', 5.5); flat(C.lb, 9, 'PICK UP', 4); flat(C.up, 9, 'PHONE', 4.5);
    flat(C.left, 9, '◀ GUN', 4.5); flat(C.right, 9, 'GUN ▶', 4.5);
    flat(C.back, 8, 'CAMERA', 3.8); flat(C.start, 8, 'PAUSE', 4);
    $.NSGraphicsContext.restoreGraphicsState;
    rep.representationUsingTypeProperties($.NSBitmapImageFileTypePNG, $({})).writeToFileAtomically(output, true);
    return 'ok';
  }

  // Layout: the body and the point each label row's line ends on.
  let L;
  if (style === 'ps') {
    L = {
      body: mirror([[256, 64], [150, 60], [98, 74], [70, 112], [52, 200], [44, 280], [62, 322], [100, 326], [132, 290], [168, 240], [210, 232]]),
      bodyFill: rgb(40, 40, 44), triggers: { l2: [112, 34, 54, 26], l1: [112, 58, 70, 14] },
      ls: [196, 214], rs: [316, 214], dpad: [118, 148], face: { y: [394, 116], b: [424, 148], a: [394, 180], x: [364, 148] },
      guide: [256, 194], back: [172, 92], start: [340, 92],
    };
  } else if (style === 'switch') {
    L = {
      body: mirror([[256, 74], [160, 66], [104, 80], [72, 118], [52, 200], [48, 270], [70, 318], [108, 318], [146, 276], [182, 238], [214, 228]]),
      bodyFill: rgb(48, 48, 52), triggers: { l2: [118, 36, 56, 24], l1: [118, 60, 74, 14] },
      ls: [130, 150], rs: [320, 224], dpad: [192, 224], face: { y: [410, 122], b: [438, 150], a: [410, 178], x: [382, 150] },
      guide: [286, 178], back: [208, 112], start: [304, 112], capture: [226, 178],
    };
  } else {  // xbox one / series
    L = {
      body: mirror([[256, 70], [168, 64], [110, 76], [76, 108], [56, 172], [46, 250], [60, 306], [98, 322], [136, 290], [176, 248], [214, 236]]),
      bodyFill: rgb(42, 42, 46), triggers: { l2: [118, 36, 52, 26], l1: [118, 62, 78, 14] },
      ls: [130, 150], rs: [322, 222], dpad: [192, 222], face: { y: [384, 124], b: [412, 152], a: [384, 180], x: [356, 152] },
      guide: [256, 104], back: [218, 148], start: [294, 148], share: [256, 168],
    };
  }
  const mirrorX = ([x, y, w, h]) => [size - x, y, w, h];

  // Triggers and bumpers (behind the body), the body, then the controls.
  for (const t of [L.triggers.l2, mirrorX(L.triggers.l2)]) fillStroke(rounded(t[0], t[1], t[2], t[3], 9), rgb(30, 30, 33), black, 3);
  for (const t of [L.triggers.l1, mirrorX(L.triggers.l1)]) fillStroke(rounded(t[0], t[1], t[2], t[3], 7), rgb(56, 56, 60), black, 3);
  fillStroke(smooth(L.body), L.bodyFill, black, 5);
  if (style === 'ps') fillStroke(rounded(256, 112, 128, 76, 14), rgb(52, 52, 58), black, 3);  // touchpad
  // The leader lines: from the label's row at the picture's edge, level to
  // just outside the body, then straight to the control (drawn over the body
  // as the Xbox 360 picture's are: black edge, grey core).
  const [dx, dy] = L.dpad, [lx, ly] = L.ls, [rx, ry] = L.rs;
  const left = [[53, [L.triggers.l2[0] - 20, L.triggers.l2[1] + 4]], [80, [L.triggers.l1[0] - 26, L.triggers.l1[1] + 2]],
    [109.5, L.guide], [135.5, L.back], [162, [lx - 12, ly - 8]], [189, [lx, ly + 6]],
    [216, [dx, dy - 16]], [243, [dx - 16, dy]], [270, [dx, dy + 16]], [297.5, [dx + 16, dy]]];
  const right = [[53, [size - L.triggers.l2[0] + 20, L.triggers.l2[1] + 4]], [80, [size - L.triggers.l1[0] + 26, L.triggers.l1[1] + 2]],
    [134.5, L.face.y], [162, L.face.b], [189, L.face.a], [216, L.face.x], [243, [rx + 12, ry - 8]], [270, [rx, ry + 6]],
    [297.5, L.start]];
  const lead = (row, anchor, leftSide) => {
    const edge = leftSide ? 0 : size, elbow = leftSide ? 38 : size - 38;
    const points = [[edge, row], [elbow, row], anchor];
    poly(points, false);
    fillStroke(poly(points, false), null, black, 6);
    return points;
  };
  const leads = left.map(([row, a]) => lead(row, a, true)).concat(right.map(([row, a]) => lead(row, a, false)));
  for (const points of leads) fillStroke(poly(points, false), null, lineGrey, 2);
  // The controls over the lines' ends, as on the Xbox 360 picture.
  const keyFill = rgb(26, 26, 28);
  if (style === 'ps') {
    stick(...L.ls, rgb(34, 34, 38)); stick(...L.rs, rgb(34, 34, 38));
    // the d-pad: four separate arrows
    const [dx, dy] = L.dpad;
    for (const [ox, oy, r] of [[0, -18, 0], [18, 0, 90], [0, 18, 180], [-18, 0, 270]]) {
      const a = r * Math.PI / 180, rot = (px, py) => [dx + ox + px * Math.cos(a) - py * Math.sin(a), dy + oy + px * Math.sin(a) + py * Math.cos(a)];
      fillStroke(poly([rot(-8, -9), rot(8, -9), rot(8, 4), rot(0, 11), rot(-8, 4)]), keyFill, black, 2.5);
    }
    const green = rgb(64, 226, 160), red = rgb(255, 102, 102), blue = rgb(124, 178, 232), pink = rgb(255, 105, 248);
    for (const k of Object.keys(L.face)) fillStroke(oval(...L.face[k], 15), keyFill, black, 2.5);
    { const [x, y] = L.face.y; fillStroke(poly([[x, y - 7], [x + 7, y + 5], [x - 7, y + 5]]), null, green, 2.5); }
    { const [x, y] = L.face.b; fillStroke(oval(x, y, 6.5), null, red, 2.5); }
    { const [x, y] = L.face.a; fillStroke(poly([[x - 6, y - 6], [x + 6, y + 6]], false), null, blue, 2.5); fillStroke(poly([[x + 6, y - 6], [x - 6, y + 6]], false), null, blue, 2.5); }
    { const [x, y] = L.face.x; fillStroke(poly([[x - 6, y - 6], [x + 6, y - 6], [x + 6, y + 6], [x - 6, y + 6]]), null, pink, 2.5); }
    fillStroke(rounded(...L.back, 10, 20, 5), keyFill, black, 2); fillStroke(rounded(...L.start, 10, 20, 5), keyFill, black, 2);
    fillStroke(oval(...L.guide, 11), keyFill, black, 2.5); text('PS', L.guide[0], L.guide[1] + 0.5, 9, light);
  } else {
    stick(...L.ls, rgb(34, 34, 38)); stick(...L.rs, rgb(34, 34, 38));
    fillStroke(oval(...L.dpad, 30), rgb(30, 30, 33), black, 2.5);
    dpad(...L.dpad, keyFill);
    const letters = style === 'switch'
      // (by position, as controllers map them: Nintendo's X is the top one)
      ? { y: ['X', light], b: ['A', light], a: ['B', light], x: ['Y', light] }
      : { y: ['Y', rgb(250, 200, 40)], b: ['B', rgb(240, 70, 60)], a: ['A', rgb(110, 200, 70)], x: ['X', rgb(60, 140, 240)] };
    for (const k of Object.keys(L.face)) {
      fillStroke(oval(...L.face[k], 15), keyFill, black, 2.5);
      text(letters[k][0], L.face[k][0], L.face[k][1] + 1, 16, letters[k][1]);
    }
    if (style === 'switch') {
      fillStroke(oval(...L.back, 9), keyFill, black, 2); fillStroke(poly([[L.back[0] - 5, L.back[1]], [L.back[0] + 5, L.back[1]]], false), null, light, 2.5);
      fillStroke(oval(...L.start, 9), keyFill, black, 2);
      fillStroke(poly([[L.start[0] - 5, L.start[1]], [L.start[0] + 5, L.start[1]]], false), null, light, 2.5);
      fillStroke(poly([[L.start[0], L.start[1] - 5], [L.start[0], L.start[1] + 5]], false), null, light, 2.5);
      fillStroke(oval(...L.guide, 11), keyFill, black, 2.5);
      { const [x, y] = L.guide; fillStroke(poly([[x - 6, y + 1], [x, y - 5], [x + 6, y + 1], [x + 4, y + 1], [x + 4, y + 6], [x - 4, y + 6], [x - 4, y + 1]]), null, light, 1.8); }
      fillStroke(rounded(...L.capture, 14, 14, 3), keyFill, black, 2); fillStroke(oval(...L.capture, 4), null, rgb(90, 90, 96), 1.5);
    } else {
      fillStroke(oval(...L.back, 8), keyFill, black, 2); fillStroke(oval(...L.start, 8), keyFill, black, 2);
      fillStroke(rounded(...L.share, 12, 7, 3), keyFill, black, 1.5);
      for (const dy of [-2.5, 0, 2.5]) fillStroke(poly([[L.start[0] - 3.5, L.start[1] + dy], [L.start[0] + 3.5, L.start[1] + dy]], false), null, light, 1.2);
      fillStroke(rounded(L.back[0] - 1, L.back[1] + 1, 6, 5, 1), null, light, 1.2);
      fillStroke(oval(...L.guide, 15), rgb(30, 30, 33), black, 2.5);
      { const [x, y] = L.guide; fillStroke(oval(x, y, 9), null, light, 2); fillStroke(poly([[x - 6, y - 6], [x + 6, y + 6]], false), null, light, 2.2); fillStroke(poly([[x + 6, y - 6], [x - 6, y + 6]], false), null, light, 2.2); }
    }
  }


  $.NSGraphicsContext.restoreGraphicsState;
  rep.representationUsingTypeProperties($.NSBitmapImageFileTypePNG, $({})).writeToFileAtomically(output, true);
  return 'ok';
}
