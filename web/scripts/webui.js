// The game's overlays (rex/ui/immediate_drawer_web.h): the PS5 hub before the
// game, dialogs during it. Each frame is ImGui-style triangles (x, y, u, v,
// colour) with 16-bit indices and per-draw texture and scissor, drawn over the
// game's picture in the scene canvas, alpha blended.
//
// Also the hub's pad: keyboard and gamepad as DualSense button bits for
// rex_web_hub_pad (arrows / left stick: D-pad, Enter: Cross, Esc or
// Backspace: Circle, X: Square, T: Triangle, Q / E: L1 / R1, O: Options), and
// the mouse over the game's canvas (rex_web_hub_mouse: pointing focuses, a
// click chooses, the wheel scrolls; on a touch screen a tap clicks and a swipe
// scrolls). The prompts show keys while the keyboard
// or mouse was used last, the controller's buttons after a gamepad.
'use strict';
window.WebUi = (() => {
  let device, format, pipelines = {}, sampler, nearest, white, layout;
  const textures = new Map();  // id -> { texture, view, binds: {linear, nearest} }
  let lastSequence = -1, lastGameFrame = 0;
  window.webUiStats = {};

  const PAD = { up: 0x10, right: 0x20, down: 0x40, left: 0x80, l1: 0x400, r1: 0x800,
                triangle: 0x1000, circle: 0x2000, cross: 0x4000, square: 0x8000, options: 0x8, r3: 0x4 };
  const keys = { ArrowUp: PAD.up, ArrowDown: PAD.down, ArrowLeft: PAD.left, ArrowRight: PAD.right,
                 Enter: PAD.cross, Space: PAD.cross, Escape: PAD.circle, Backspace: PAD.circle,
                 KeyX: PAD.square, KeyT: PAD.triangle, KeyQ: PAD.l1, KeyE: PAD.r1, KeyO: PAD.options };
  const held = new Set();
  // (pressed since the hub last read the pad: a tap shorter than its 16 ms
  // reads still counts once)
  const tapped = new Set();
  let keyboardLast = true;  // (no gamepad yet: the keyboard's prompts)
  addEventListener('keydown', e => { keyboardLast = true; if (keys[e.code]) held.add(e.code), tapped.add(e.code); });
  addEventListener('keyup', e => held.delete(e.code));
  addEventListener('blur', () => { held.clear(); tapped.clear(); });
  // The mouse, in the overlay's 1280x720 pixels over the scene canvas.
  const mouse = { x: -1, y: -1, buttons: 0, wheel: 0 };
  const canvasPoint = e => {
    const canvas = document.getElementById('scene'), r = canvas.getBoundingClientRect();
    // (an unlaid-out canvas, as in a hidden window, measures 0: its own size then)
    return [(e.clientX - r.left) * 1280 / (r.width || 1280), (e.clientY - r.top) * 720 / (r.height || 720)];
  };
  addEventListener('mousemove', e => {
    const [x, y] = canvasPoint(e);
    const inside = x >= 0 && y >= 0 && x < 1280 && y < 720;
    mouse.x = inside ? Math.round(x) : -1;
    mouse.y = inside ? Math.round(y) : -1;
    if (inside) keyboardLast = true;
  });
  addEventListener('mousedown', e => {
    const [x, y] = canvasPoint(e);
    if (x >= 0 && y >= 0 && x < 1280 && y < 720) {
      keyboardLast = true;
      // (where it was clicked: a tap needn't come after a move over the canvas)
      mouse.x = Math.round(x), mouse.y = Math.round(y);
      mouse.buttons |= 1 << e.button;
      if (e.button === 0) mouse.clicked = true;  // (latched: a quick click isn't missed)
    }
  });
  addEventListener('mouseup', e => { mouse.buttons &= ~(1 << e.button); });
  addEventListener('wheel', e => {
    const [x, y] = canvasPoint(e);
    if (x >= 0 && y >= 0 && x < 1280 && y < 720) {
      mouse.wheel += Math.sign(e.deltaY);
      e.preventDefault();
    }
  }, { passive: false });
  // A touch screen: a tap is a click already (the browser's mouse events for
  // it); a finger dragged up or down scrolls, a wheel notch per 40 pixels of
  // the overlay's, as the wheel would.
  let swipe = null;
  addEventListener('pointerdown', e => {
    // (not over the game's touch controls: their sticks aren't swipes)
    if (e.pointerType !== 'touch' || swipe || document.querySelector('#touch.on')) return;
    const [x, y] = canvasPoint(e);
    if (x >= 0 && y >= 0 && x < 1280 && y < 720) swipe = { id: e.pointerId, y };
  });
  addEventListener('pointermove', e => {
    if (!swipe || e.pointerId !== swipe.id) return;
    const [, y] = canvasPoint(e);
    while (Math.abs(y - swipe.y) >= 40) {
      const step = y < swipe.y ? 1 : -1;  // (the finger going up: the list moves on, as a wheel turned down)
      mouse.wheel += step;
      swipe.y -= step * 40;
    }
  });
  const swipeEnd = e => { if (swipe && e.pointerId === swipe.id) swipe = null; };
  addEventListener('pointerup', swipeEnd);
  addEventListener('pointercancel', swipeEnd);
  function padBits() {
    let bits = 0;
    for (const code of held) bits |= keys[code];
    for (const code of tapped) bits |= keys[code];
    tapped.clear();
    for (const pad of navigator.getGamepads ? navigator.getGamepads() : []) {
      if (!pad) continue;
      const b = i => pad.buttons[i] && pad.buttons[i].pressed;
      if (pad.buttons.some(x => x.pressed) || pad.axes.some(a => Math.abs(a) > 0.5)) {
        keyboardLast = false;
        padFamily = familyOf(pad.id);
      }
      if (b(0)) bits |= PAD.cross; if (b(1)) bits |= PAD.circle; if (b(2)) bits |= PAD.square;
      if (b(3)) bits |= PAD.triangle; if (b(4)) bits |= PAD.l1; if (b(5)) bits |= PAD.r1;
      if (b(9)) bits |= PAD.options; if (b(11)) bits |= PAD.r3;
      if (b(12) || pad.axes[1] < -0.5) bits |= PAD.up; if (b(13) || pad.axes[1] > 0.5) bits |= PAD.down;
      if (b(14) || pad.axes[0] < -0.5) bits |= PAD.left; if (b(15) || pad.axes[0] > 0.5) bits |= PAD.right;
    }
    // (bit 31: the keyboard or mouse was last; bits 29-30: the pad's family,
    // for the hub's button pictures - 0 PlayStation, 1 Xbox, 2 Nintendo)
    return (bits | (keyboardLast ? 0x80000000 : 0) | padFamily << 29) >>> 0;
  }
  // The family from the browser's id for the pad ("DualSense Wireless
  // Controller (STANDARD GAMEPAD Vendor: 054c Product: 0ce6)", "Xbox Wireless
  // Controller (... Vendor: 045e ...)"): Sony's, Nintendo's, else Xbox-style
  // (what most other pads copy).
  let padFamily = 1;
  function familyOf(id) {
    const text = (id || '').toLowerCase();
    if (/054c|dualsense|dualshock|playstation|ps[345]/.test(text)) return 0;
    if (/057e|nintendo|switch|pro controller|joy-con/.test(text)) return 2;
    return 1;
  }

  function pipeline(lines) {
    const key = lines ? 'lines' : 'triangles';
    if (pipelines[key]) return pipelines[key];
    const module = device.createShaderModule({ code: `
      struct Screen { size: vec2f, pad: vec2f }
      @group(0) @binding(0) var<uniform> screen: Screen;
      @group(0) @binding(1) var image: texture_2d<f32>;
      @group(0) @binding(2) var image_sampler: sampler;
      struct Out { @builtin(position) pos: vec4f, @location(0) uv: vec2f, @location(1) color: vec4f }
      @vertex fn vs(@location(0) pos: vec2f, @location(1) uv: vec2f, @location(2) color: vec4f) -> Out {
        var out: Out;
        out.pos = vec4f(pos.x / screen.size.x * 2.0 - 1.0, 1.0 - pos.y / screen.size.y * 2.0, 0.0, 1.0);
        out.uv = uv;
        out.color = color;
        return out;
      }
      @fragment fn fs(in: Out) -> @location(0) vec4f {
        return textureSample(image, image_sampler, in.uv) * in.color;
      }` });
    return pipelines[key] = device.createRenderPipeline({
      layout: device.createPipelineLayout({ bindGroupLayouts: [layout] }),
      vertex: { module, entryPoint: 'vs', buffers: [{ arrayStride: 20, attributes: [
        { shaderLocation: 0, offset: 0, format: 'float32x2' },
        { shaderLocation: 1, offset: 8, format: 'float32x2' },
        { shaderLocation: 2, offset: 16, format: 'unorm8x4' }] }] },
      fragment: { module, entryPoint: 'fs', targets: [{ format, blend: {
        color: { srcFactor: 'src-alpha', dstFactor: 'one-minus-src-alpha' },
        alpha: { srcFactor: 'one', dstFactor: 'one-minus-src-alpha' } } }] },
      primitive: { topology: lines ? 'line-list' : 'triangle-list' },
    });
  }

  // A texture of the overlay's table, uploaded once (by id).
  function texture(m, table, count, id) {
    if (!id) return white;
    const cached = textures.get(id);
    if (cached) return cached;
    for (let i = 0; i < count; i++) {
      const at = table + i * 24;
      if (m.getUint32(at, true) !== id) continue;
      const w = m.getUint32(at + 4, true), h = m.getUint32(at + 8, true);
      const pixels = Number(m.getBigUint64(at + 16, true));
      const linear = m.getUint32(at + 12, true) !== 0;
      if (!w || !h || !pixels) return white;
      const tex = device.createTexture({ size: [w, h], format: 'rgba8unorm',
        usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
      device.queue.writeTexture({ texture: tex }, new Uint8Array(wasmMemory.buffer, pixels, w * h * 4).slice(),
        { bytesPerRow: w * 4 }, [w, h]);
      const entry = { texture: tex, view: tex.createView(), linear };
      textures.set(id, entry);
      // (the game lets its copy of the pixels go)
      Module._rex_web_ui_texture_uploaded?.(id);
      return entry;
    }
    return white;
  }

  return {
    init(gpuDevice, canvasFormat) {
      device = gpuDevice;
      format = canvasFormat;
      layout = device.createBindGroupLayout({ entries: [
        { binding: 0, visibility: GPUShaderStage.VERTEX, buffer: {} },
        { binding: 1, visibility: GPUShaderStage.FRAGMENT, texture: {} },
        { binding: 2, visibility: GPUShaderStage.FRAGMENT, sampler: {} }] });
      sampler = device.createSampler({ magFilter: 'linear', minFilter: 'linear' });
      nearest = device.createSampler({ magFilter: 'nearest', minFilter: 'nearest' });
      const t = device.createTexture({ size: [1, 1], format: 'rgba8unorm',
        usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
      device.queue.writeTexture({ texture: t }, new Uint8Array([255, 255, 255, 255]), { bytesPerRow: 4 }, [1, 1]);
      white = { texture: t, view: t.createView(), linear: true };
      // The pad, and the overlay on its own while the game draws nothing
      // (the hub runs before the game starts).
      setInterval(() => {
        try {
          if (Module._rex_web_hub_pad) Module._rex_web_hub_pad(padBits());
          if (Module._rex_web_hub_mouse) {
            Module._rex_web_hub_mouse(mouse.x, mouse.y, mouse.buttons | (mouse.clicked ? 0x100 : 0), mouse.wheel);
            mouse.wheel = 0;
            mouse.clicked = false;
          }
          if (performance.now() - lastGameFrame > 100) this.drawAlone();
        } catch (e) {
          if (!window.webUiError) console.error('webui', window.webUiError = e.stack || e);
        }
      }, 16);
    },
    // The page drew a game frame (the overlay goes on top of it, in that pass).
    noteGameFrame() { lastGameFrame = performance.now(); },
    drawAlone() {
      if (!Module._rex_web_ui_frame) return;
      const m = new DataView(wasmMemory.buffer);
      const frame = Number(Module._rex_web_ui_frame());
      const sequence = m.getUint32(frame, true);
      if (sequence === lastSequence) return;
      const encoder = device.createCommandEncoder();
      const transient = [];
      this.draw(encoder, Scene3D.canvasTexture().createView(), true, transient);
      const shot = Scene3D.captureCanvas(encoder);  // (Scene3D.shotName: a test capture)
      device.queue.submit([encoder.finish()]);
      if (shot) shot();
      device.queue.onSubmittedWorkDone().then(() => transient.forEach(b => b.destroy()));
    },
    // Records the latest overlay frame onto `view` (cleared first if `clear`).
    draw(encoder, view, clear, transient) {
      if (!device || !Module._rex_web_ui_frame) return;
      const m = new DataView(wasmMemory.buffer);
      const frame = Number(Module._rex_web_ui_frame());
      lastSequence = m.getUint32(frame, true);
      const vertexCount = m.getUint32(frame + 4, true), indexCount = m.getUint32(frame + 8, true);
      const drawCount = m.getUint32(frame + 12, true);
      const vertices = Number(m.getBigUint64(frame + 16, true)), indices = Number(m.getBigUint64(frame + 24, true));
      const draws = Number(m.getBigUint64(frame + 32, true));
      const width = m.getFloat32(frame + 40, true) || 1280, height = m.getFloat32(frame + 44, true) || 720;
      const textureCount = m.getUint32(frame + 48, true), table = Number(m.getBigUint64(frame + 56, true));
      // Now and then: textures the game has let go of (the hub's pictures once
      // the game starts) leave the GPU too.
      if (lastSequence % 120 === 0 && textures.size > textureCount) {
        const live = new Set();
        for (let i = 0; i < textureCount; i++) live.add(m.getUint32(table + i * 24, true));
        for (const [id, t] of textures) if (!live.has(id)) { t.texture.destroy(); textures.delete(id); }
      }
      Object.assign(webUiStats, { sequence: lastSequence, draws: drawCount, vertices: vertexCount, textures: textureCount });
      if (!clear && !drawCount) return;
      const pass = encoder.beginRenderPass({ colorAttachments: [{ view, loadOp: clear ? 'clear' : 'load',
        storeOp: 'store', clearValue: { r: 0, g: 0, b: 0, a: 1 } }] });
      if (drawCount && vertexCount && indexCount) {
        const vb = device.createBuffer({ size: Math.ceil(vertexCount * 20 / 4) * 4, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
        device.queue.writeBuffer(vb, 0, new Uint8Array(wasmMemory.buffer, vertices, vertexCount * 20).slice());
        const ibBytes = new Uint8Array(Math.ceil(indexCount * 2 / 4) * 4);
        ibBytes.set(new Uint8Array(wasmMemory.buffer, indices, indexCount * 2));
        const ib = device.createBuffer({ size: ibBytes.byteLength, usage: GPUBufferUsage.INDEX | GPUBufferUsage.COPY_DST });
        device.queue.writeBuffer(ib, 0, ibBytes);
        const ub = device.createBuffer({ size: 16, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
        device.queue.writeBuffer(ub, 0, new Float32Array([width, height, 0, 0]));
        transient.push(vb, ib, ub);
        pass.setVertexBuffer(0, vb);
        pass.setIndexBuffer(ib, 'uint16');
        const W = 1280, H = 720, sx = W / width, sy = H / height;
        for (let i = 0; i < drawCount; i++) {
          const at = draws + i * 40;
          const tex = texture(m, table, textureCount, m.getUint32(at, true));
          const lines = m.getUint32(at + 4, true) !== 0;
          pass.setPipeline(pipeline(lines));
          pass.setBindGroup(0, device.createBindGroup({ layout, entries: [
            { binding: 0, resource: { buffer: ub } }, { binding: 1, resource: tex.view },
            { binding: 2, resource: tex.linear ? sampler : nearest }] }));
          if (m.getUint32(at + 20, true)) {
            const l = Math.max(0, Math.floor(m.getFloat32(at + 24, true) * sx));
            const t = Math.max(0, Math.floor(m.getFloat32(at + 28, true) * sy));
            const r = Math.min(W, Math.ceil(m.getFloat32(at + 32, true) * sx));
            const b = Math.min(H, Math.ceil(m.getFloat32(at + 36, true) * sy));
            if (r <= l || b <= t) continue;
            pass.setScissorRect(l, t, r - l, b - t);
          } else {
            pass.setScissorRect(0, 0, W, H);
          }
          pass.drawIndexed(m.getUint32(at + 12, true), 1, m.getUint32(at + 8, true), m.getInt32(at + 16, true));
        }
      }
      pass.end();
    },
  };
})();
