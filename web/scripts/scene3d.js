// GTA IV's 3D draws (web_frame_capture.h WebDraw) rendered with the
// recompiled shader cache as WGSL (wgsl/<hash>.wgsl, from spv2wgsl).
// The frame is replayed as the game issued it: draws into the bound colour
// and depth surfaces (one GPU texture per guest surface: rgba16float colour,
// depth32float-stencil8 depth, as the native D32_SFLOAT_S8), clears, and
// resolves, which copy a surface
// into a texture at a guest address that later draws sample. The canvas
// shows the texture the present names (the front buffer).
//
// Bindings, as spv2wgsl lays them out: group 0 = uniform blocks (0 vertex
// constants, 1 pixel constants, 2 shared constants), group 1 = textures at
// kind * 32 + stage (kind 0 2D, 1 2D array, 2 3D, 3 cube), group 2 = samplers
// at the stage.
'use strict';
window.Scene3D = (() => {
  const W = 1280, H = 720;
  const DRAW_SIZE = 640, DECL_SIZE = 772;
  const COLOR = 'rgba16float';
  // (depth24plus-stencil8 when the device lacks depth32float-stencil8: init)
  let DEPTH = 'depth32float-stencil8';
  const CONSTANTS_SIZE = 4096 * 2 + 1280;
  // (a draw's blend constant words read as floats, reused every draw)
  const blendWords = new Uint32Array(4), blendFloats = new Float32Array(blendWords.buffer);
  // (one word read as a float, without allocating)
  const wordScratch = new Uint32Array(1), wordFloat = new Float32Array(wordScratch.buffer);
  const asFloat = w => { wordScratch[0] = w; return wordFloat[0]; };
  // An event's first bound colour size (the pass's size), else its depth's.
  function firstSize(e) {
    const s = e.sizes;
    return s[0] || s[1] || s[2] || s[3] || s[4];
  }
  // Which draws to show; replaceable from the console (window.scene3dFilter).
  // Default: full-screen targets outside deferred lighting (stage tags are
  // mostly unknown, so the target size picks the scene passes).
  window.scene3dFilter = () => true;
  let device, canvasContext, canvasFormat, float32Targets = false;
  const surfaces = new Map();     // guest surface handle -> { texture, view, width, height, depth }
  const resolved = new Map();     // resolve id (per guest texture) -> { texture, view, width, height }
  const pendingClears = new Map();  // surface handle -> { color } | { depth, stencil }
  const shaders = new Map();      // hash -> { state: 'loading'|'ready'|'failed', module, info }
  const pipelines = new Map();    // key -> pipeline | 'pending' | 'failed'
  const buffers = new Map();      // buffer id -> { serial, buffer }
  // Frames replayed: every buffer, surface and resolved texture notes the
  // frame that last used it, and ones unused for a while are destroyed
  // (evictUnused). The game makes new ones as it streams the city and makes
  // targets; kept forever they grew the GPU's memory without end.
  let frameSerial = 0;
  const sampleSelect = flags => { const encoded = (flags >>> 4) & 7; return Math.min(6, encoded ? encoded - 1 : 0); };
  const useResolved = id => { const r = resolved.get(id); if (r) r.used = frameSerial; return r; };
  const dummies = {};
  let samplers, stats = {};
  window.scene3dStats = stats;

  function hex(lo, hi) { return (hi >>> 0).toString(16).toUpperCase().padStart(8, '0') + (lo >>> 0).toString(16).toUpperCase().padStart(8, '0'); }

  // What spv2wgsl's output declares: resource bindings, vertex inputs and
  // fragment outputs.
  function parse(code) {
    const bindings = [];
    for (const m of code.matchAll(/@group\((\d+)\) @binding\((\d+)\)\s*\nvar(?:<(\w+)>)? \w+: ([^;]+);/g)) {
      bindings.push({ group: +m[1], binding: +m[2], space: m[3] || '', type: m[4].trim() });
    }
    const inputs = [];
    const main = code.match(/@vertex\s*fn shaderMain\(([\s\S]*?)\)\s*->/);
    if (main) for (const m of main[1].matchAll(/@location\((\d+)\) \w+: ([\w<>]+)/g)) inputs.push({ location: +m[1], type: m[2] });
    return { bindings, inputs };
  }

  // The colour output epilogue the native renderer appends to every pixel
  // shader (gta4_native/native_color_output_spirv.h): each target's output
  // times its exponent bias scale, NaN to 0, then clamped to the target
  // format's range when its lower bound isn't above its upper. The capture
  // puts the parameters in the shared constants at 0x360 (three vec4s a
  // target: scale, lower, upper).
  function withColorOutput(code) {
    if (!code.includes('@fragment')) return code;
    const out = code.match(/struct FragmentOutput \{([\s\S]*?)\n\}/);
    if (!out) return code;
    const targets = [...out[1].matchAll(/@location\((\d)\) (\w+): vec4<f32>/g)].map(m => ({ location: +m[1], name: m[2] }));
    if (!targets.length || !/return FragmentOutput\(.*\);/.test(code)) return code;
    let shared = code.match(/@group\(0\) @binding\(2\)\s*\nvar<uniform> (\w+): \w+;/)?.[1];
    let head = '';
    if (!shared) {
      shared = 'rexShared';
      head = 'struct RexShared {\n    member: array<vec4<u32>, 80>,\n}\n@group(0) @binding(2) \nvar<uniform> rexShared: RexShared;\n';
    }
    const fields = targets.map(t => '    r.' + t.name + ' = rexOutput(r.' + t.name + ', ' + t.location + 'u);').join('\n');
    const tail = `
fn rexOutput(v: vec4<f32>, slot: u32) -> vec4<f32> {
    let at = 54u + slot * 3u;
    let scale = bitcast<vec4<f32>>(${shared}.member[at]);
    let lower = bitcast<vec4<f32>>(${shared}.member[at + 1u]);
    let upper = bitcast<vec4<f32>>(${shared}.member[at + 2u]);
    let scaled = v * scale;
    let nan = (bitcast<vec4<u32>>(scaled) & vec4<u32>(0x7fffffffu)) > vec4<u32>(0x7f800000u);
    let finite = select(scaled, vec4<f32>(0.0), nan);
    let raised = select(finite, lower, finite < lower);
    let clamped = select(raised, upper, raised > upper);
    return select(scaled, clamped, lower <= upper);
}
fn rexColorOutput(o: FragmentOutput) -> FragmentOutput {
    var r = o;
${fields}
    return r;
}
`;
    const lead = code.match(/^(diagnostic\([^)]*\);\n)?/)[0];
    return lead + head + code.slice(lead.length)
      .replace(/return FragmentOutput\((.*)\);/, 'return rexColorOutput(FragmentOutput($1));') + tail;
  }

  // Alpha test: the translated pixel shaders carry it behind specialization
  // constant 0 (bit 1 enable, bits 8-10 the compare function), which the
  // native renderer sets per draw (PackAlphaTestSpecialization) and the WGSL
  // conversion froze at 0 - leaving `if (0u != 0u) { let f = ((0u >> 8u) & 7u);`.
  // That constant becomes the pipeline-overridable rex_alpha_test.
  function withAlphaTest(code) {
    if (!code.includes('@fragment')) return { code, alphaTest: false };
    const pattern = /if \(0u != 0u\) \{(\s*let \w+ = )\(\(0u >> bitcast<u32>\(8u\)\) & 7u\);/g;
    if (!pattern.test(code)) return { code, alphaTest: false };
    code = code.replace(pattern, 'if ((rex_alpha_test & 2u) != 0u) {$1((rex_alpha_test >> bitcast<u32>(8u)) & 7u);');
    const lead = code.match(/^(diagnostic\([^)]*\);\n)?/)[0];
    return { code: lead + 'override rex_alpha_test: u32 = 0u;\n' + code.slice(lead.length), alphaTest: true };
  }

  // Shaders the native renderer always replaces (modern_shader_policy.h
  // AllowModernShader with gta4_modern_shaders off): motion-blur composites,
  // Bink video, and the final composite whose stock form overflows on black.
  const kOverrides = new Set(['F6AEB9A606561C54', 'EE75C9F6AA1AB16A', 'E51D9DD95A333D92', 'D3B2B2125BC24911',
    '94CC5AF0E2FF5B08', '535ACDAB8AE84D82', '156BAD4A9EE62726', '9E76B68B60127349', 'A6C9E2B8B2A59D7A']);
  function loadShader(hash) {
    const entry = { state: 'loading' };
    shaders.set(hash, entry);
    // (the native renderer's always-on replacements first, when built:
    // overrides.sh, wgsl/override/<hash>.wgsl)
    const stock = () => fetch('wgsl/' + hash + '.wgsl').then(r => r.ok ? r.text() : Promise.reject(r.status));
    const source = kOverrides.has(hash)
      ? fetch('wgsl/override/' + hash + '.wgsl').then(r => r.ok ? (entry.override = true, r.text()) : stock(), stock)
      : stock();
    source.then(code => {
      // (window.s3dShaderPatch, debugging: { hash: code => code } applied to a
      // shader's WGSL when it first loads)
      if (window.s3dShaderPatch && window.s3dShaderPatch[hash]) code = window.s3dShaderPatch[hash](code);
      code = withColorOutput(code);
      ({ code, alphaTest: entry.alphaTest } = withAlphaTest(code));
      entry.code = code;
      entry.constantNeed = constantNeed(code);
      entry.outputs = new Set([...(code.match(/struct FragmentOutput \{([\s\S]*?)\n\}/)?.[1] || '')
        .matchAll(/@location\((\d)\)/g)].map(m => +m[1]));
      entry.module = device.createShaderModule({ code });
      entry.info = parse(code);
      entry.state = 'ready';
      // Tell the capture which texture stages this shader samples, so it
      // looks up only those for its draws.
      let mask = 0;
      for (const b of entry.info.bindings) if (b.group === 1) mask |= 1 << (b.binding & 31);
      if (Module._rex_web_shader_stages) {
        Module._rex_web_shader_stages(parseInt(hash.slice(8), 16) >>> 0, parseInt(hash.slice(0, 8), 16) >>> 0, mask >>> 0);
      }
      // ...and how much of each constant bank it reads, so the capture
      // compares and copies only that much for its draws.
      if (Module._rex_web_shader_constants) {
        Module._rex_web_shader_constants(parseInt(hash.slice(8), 16) >>> 0, parseInt(hash.slice(0, 8), 16) >>> 0,
                                         entry.constantNeed[2], entry.constantNeed[3]);
      }
    }).catch(e => { entry.state = 'failed'; stats.missingShaders = (stats.missingShaders || 0) + 1; });
    return entry;
  }
  // The bytes of each constant block (group 0 bindings 0 and 1, 4 KiB
  // each) a shader reads: up to its highest register when every index is a
  // literal, all of it when one is computed (skinning's bone palettes),
  // none when it has no such binding. render uploads only that much of each
  // block.
  // need[2] and need[3]: the 256-byte pieces (16 registers each) of the two
  // blocks it reads, a bit each, for the capture (all of them when an index
  // is computed): it compares and copies only those.
  function constantNeed(code) {
    const need = [0, 0, 0, 0];
    for (const [, binding, name] of code.matchAll(/@group\(0\) @binding\(([01])\)\s*var<uniform> (\w+)/g)) {
      let max = -1, dynamic = false, pieces = 0;
      // (the index: brackets balanced one level deep, `member[a[1]]`)
      for (const m of code.matchAll(new RegExp(name + '\\.member\\[((?:[^\\[\\]]|\\[[^\\[\\]]*\\])*)\\]', 'g'))) {
        const index = m[1];
        let register = -1;
        let r = /^(\d+)u$/.exec(index);
        if (r) register = +r[1];
        else if ((r = /^\(u32\((\d+)u\) >> bitcast<u32>\(4u\)\)$/.exec(index))) register = +r[1] >> 4;
        if (register < 0 || register > 255) { dynamic = true; break; }
        max = Math.max(max, register);
        pieces |= 1 << (register >> 4);
      }
      need[+binding] = dynamic ? 4096 : Math.max(256, Math.ceil((max + 1) * 16 / 256) * 256);
      need[2 + +binding] = dynamic ? 0xFFFF : pieces || 1;
    }
    return need;
  }

  function shader(hash) {
    const entry = shaders.get(hash) || loadShader(hash);
    return entry.state === 'ready' ? entry : null;
  }

  // A colour target's storage, from its Xenos format (RB_COLOR_INFO, the
  // capture's params[19], four bits a target): 8-bit and 10-bit targets as
  // the console stores them (they quantize, and shaders decode bits packed
  // into them), float ones as 16-bit floats.
  function targetFormat(d, i) {
    const p = d.params[19];
    if (!(p & 0x80000000)) return COLOR;
    switch ((p >>> (4 * i)) & 15) {
      case 0: case 1: return 'rgba8unorm';
      case 2: case 10: return 'rgb10a2unorm';
      // (32-bit float targets at full precision, as the native R32/R32G32
      // ones: GTA IV draws its scene depth into one; blending them needs
      // float32-blendable)
      case 14: return float32Targets ? 'r32float' : COLOR;
      case 15: return float32Targets ? 'rg32float' : COLOR;
      default: return COLOR;
    }
  }

  // Xenos vertex formats (gta4_metal pipelines.mm VertexFormat) for a float,
  // unsigned or signed shader input; null when WebGPU has no match.
  function vertexFormat(type, kind) {
    const f = kind === 'f', u = kind === 'u', i = kind === 'i';
    switch (type) {
      case 0x2C83A4: return f ? 'float32' : null;
      case 0x2C23A5: return f ? 'float32x2' : null;
      case 0x2A23B9: return f ? 'float32x3' : null;
      case 0x1A23A6: return f ? 'float32x4' : null;
      case 0x182886: return u ? 'uint8x4' : f ? 'unorm8x4-bgra' : null;
      case 0x1A2286: case 0x1A2386: return u ? 'uint8x4' : f ? 'unorm8x4' : null;
      case 0x2C2359: return i ? 'sint16x2' : f ? 'snorm16x2' : null;
      case 0x1A235A: return i ? 'sint16x4' : f ? 'snorm16x4' : null;
      case 0x1A2086: case 0x1A2186: return u ? 'uint8x4' : f ? 'unorm8x4' : null;
      case 0x2C2159: return i ? 'sint16x2' : f ? 'snorm16x2' : null;
      case 0x1A215A: return i ? 'sint16x4' : f ? 'snorm16x4' : null;
      case 0x2C2059: return u ? 'uint16x2' : f ? 'unorm16x2' : null;
      case 0x1A205A: return u ? 'uint16x4' : f ? 'unorm16x4' : null;
      case 0x2C82A1: case 0x2A2287: case 0x2A2187: case 0x2A2190: case 0x2A2390: return u ? 'uint32' : i ? 'sint32' : null;
      case 0x2C235F: return f ? 'float16x2' : null;
      case 0x1A2360: return f ? 'float16x4' : null;
      default: return null;
    }
  }
  const formatSize = { float32: 4, float32x2: 8, float32x3: 12, float32x4: 16, uint8x4: 4, unorm8x4: 4,
    'unorm8x4-bgra': 4, sint16x2: 4, snorm16x2: 4, sint16x4: 8, snorm16x4: 8, uint16x2: 4, unorm16x2: 4,
    uint16x4: 8, unorm16x4: 8, uint32: 4, sint32: 4, float16x2: 4, float16x4: 8 };
  const kindOf = type => type.includes('u32') ? 'u' : type.includes('i32') ? 'i' : 'f';
  const zeroFormat = { f: 'float32x4', u: 'uint32x4', i: 'sint32x4' };

  const compares = ['never', 'less', 'equal', 'less-equal', 'greater', 'not-equal', 'greater-equal', 'always'];
  const factors = { 0: 'zero', 1: 'one', 4: 'src', 5: 'one-minus-src', 6: 'src-alpha', 7: 'one-minus-src-alpha',
    8: 'dst', 9: 'one-minus-dst', 10: 'dst-alpha', 11: 'one-minus-dst-alpha', 12: 'constant',
    13: 'one-minus-constant', 14: 'constant', 15: 'one-minus-constant', 16: 'src-alpha-saturated' };
  const operations = { 0: 'add', 1: 'subtract', 2: 'min', 3: 'max', 4: 'reverse-subtract' };
  function blendComponent(src, op, dst) {
    const operation = operations[op] || 'add';
    return operation === 'min' || operation === 'max' ? { operation, srcFactor: 'one', dstFactor: 'one' }
      : { operation, srcFactor: factors[src] || 'one', dstFactor: factors[dst] || 'zero' };
  }

  // Float shader inputs fed formats WebGPU can't convert to floats itself:
  // the vertex format, the type the input is declared as instead, and the
  // expression turning it back into the vec4<f32> the shader expects.
  // DEC3N is signed 10:10:10:2; SHORT2/4 and UBYTE4 are integers read as
  // unnormalized floats (1000 -> 1000.0), as gta4_metal's Short2/4 and UChar4.
  const conversions = {
    [0x1A2187]: { format: 'uint32', type: 'u32', expr: x => 'dec3n(' + x + ')' },
    [0x2C2359]: { format: 'sint16x2', type: 'vec2<i32>', expr: x => 'vec4<f32>(vec2<f32>(' + x + '), 0.0, 1.0)' },
    [0x1A235A]: { format: 'sint16x4', type: 'vec4<i32>', expr: x => 'vec4<f32>(' + x + ')' },
    [0x1A2286]: { format: 'uint8x4', type: 'vec4<u32>', expr: x => 'vec4<f32>(' + x + ')' },
    [0x1A2386]: { format: 'uint8x4', type: 'vec4<u32>', expr: x => 'vec4<f32>(' + x + ')' },
  };
  // A vertex shader with those inputs retyped and converted on entry.
  // `packed` lists [location, xenos type] pairs. `vt` (vertexTransform), on
  // the position the shader returns: gl - OpenGL clip space, z from -w to w,
  // to WebGPU's 0 to w (the native negativeOneToOne); flipZ - depth mirrored
  // (z = w - z), for a viewport whose depth range runs from 1 down to 0
  // (WebGPU viewports need min <= max, so the draw gets the range the right
  // way round and its depths flipped here instead); fit - x and y scaled and
  // offset (overridable rex_vp_*), for a viewport reaching outside its target
  // (WebGPU's must lie inside it; the native renderer keeps the guest's).
  function packedVariant(vs, packed, vt = {}) {
    const key = packed.map(p => p.join(':')).join('.') + (vt.gl ? '.gl' : '') + (vt.flipZ ? '.fz' : '') + (vt.fit ? '.fit' : '') +
      (vt.bias ? '.bias' : '');
    vs.variants ??= {};
    if (vs.variants[key]) return vs.variants[key];
    let code = vs.code, moved = false;
    code = code.replace(/(@vertex\s*fn shaderMain\()([\s\S]*?)(\)\s*->[^{]*\{)([\s\S]*?\n\})/, (all, head, params, arrow, body) => {
      for (const [location, type] of packed) {
        const m = params.match(new RegExp('@location\\(' + location + '\\) (\\w+): vec4<f32>'));
        if (!m) continue;
        const c = conversions[type];
        params = params.replace(m[0], '@location(' + location + ') ' + m[1] + ': ' + c.type);
        body = body.replace(new RegExp('= ' + m[1] + ';', 'g'), '= ' + c.expr(m[1]) + ';');
      }
      const position = (vt.gl || vt.flipZ || vt.fit || vt.bias) && vs.code.match(/@builtin\(position\) (\w+):/);
      if (position) {
        const at = 'moved.' + position[1];
        let change = '';
        if (vt.gl) change += at + '.z = (' + at + '.z + ' + at + '.w) * 0.5; ';
        if (vt.flipZ) change += at + '.z = ' + at + '.w - ' + at + '.z; ';
        if (vt.fit) change += at + '.x = ' + at + '.x * rex_vp_sx + rex_vp_ox * ' + at + '.w; ' +
                              at + '.y = ' + at + '.y * rex_vp_sy + rex_vp_oy * ' + at + '.w; ';
        // (the constant depth bias, in depth units as D3D and Xenos give it:
        // added to z/w after any flip, whatever the depth format stores)
        if (vt.bias) change += at + '.z = ' + at + '.z + rex_depth_bias * ' + at + '.w; ';
        const before = body;
        body = body.replace(/return VertexOutput\(([^;]*)\);/, 'var moved = VertexOutput($1); ' + change + 'return moved;');
        moved = body !== before;
      }
      return head + params + arrow + body;
    });
    // (the overrides only where the position was rewritten to read them: an
    // unreferenced one set from the pipeline is an error)
    if (vt.bias && moved) code += `
override rex_depth_bias: f32 = 0.0;
`;
    if (vt.fit && moved) code += `
override rex_vp_sx: f32 = 1.0;
override rex_vp_sy: f32 = 1.0;
override rex_vp_ox: f32 = 0.0;
override rex_vp_oy: f32 = 0.0;
`;
    code += `
fn dec3n(v: u32) -> vec4<f32> {
    let c = vec3<i32>(i32(v << 22u) >> 22u, i32(v << 12u) >> 22u, i32(v << 2u) >> 22u);
    return vec4<f32>(max(vec3<f32>(c) / 511.0, vec3<f32>(-1.0)), 1.0);
}
`;
    const module = device.createShaderModule({ code });
    module.rexFit = vt.fit && moved;
    module.rexBias = vt.bias && moved;
    return vs.variants[key] = module;
  }

  // Stencil, culling and depth bias from a draw's params (web_frame_capture.h
  // WebDraw): pipeline state in WebGPU, so part of the pipeline key.
  const stencilOps = ['keep', 'zero', 'replace', 'increment-clamp', 'decrement-clamp', 'invert',
                      'increment-wrap', 'decrement-wrap'];
  // How a draw's position is placed (packedVariant's vt, in d.vt / d.vtKey),
  // and the viewport rectangle to set: null when it covers nothing. WebGPU's
  // viewport must lie inside the surface; one reaching outside (a guard band,
  // an atlas tile) is cut to the surface and the shader scales and offsets
  // the position so it lands where the guest's full viewport puts it.
  function vertexTransform(d, PW, PH) {
    const [vx, vy, vw, vh] = d.viewport;
    if (!(vw > 0 && vh > 0)) return null;
    const x0 = Math.max(0, vx), y0 = Math.max(0, vy), x1 = Math.min(PW, vx + vw), y1 = Math.min(PH, vy + vh);
    if (x1 <= x0 || y1 <= y0) return null;
    // (window.s3dExp, debugging: 'nogl' ignores the GL clip-space flag)
    const vt = { gl: !!(d.params[17] & 2) && !(window.s3dExp || '').includes('nogl'), flipZ: d.viewport[4] > d.viewport[5], fit: null };
    const cw = x1 - x0, ch = y1 - y0;
    if (Math.abs(cw - vw) > 1e-3 || Math.abs(ch - vh) > 1e-3 || Math.abs(x0 - vx) > 1e-3 || Math.abs(y0 - vy) > 1e-3) {
      // x' = x * vw/cw + (2 (vx - x0) + vw - cw) / cw (times w); y likewise,
      // with WebGPU's y up and the framebuffer's down.
      const r = v => Math.round(v * 1e5) / 1e5;
      vt.fit = [r(vw / cw), r(vh / ch), r((2 * (vx - x0) + vw - cw) / cw), r((ch - vh - 2 * (vy - y0)) / ch)];
    }
    // (the constant depth bias, params 11, as an NDC offset: the depth range
    // scales depth units; none for points and lines, as rasterState)
    const f32 = asFloat(d.params[11]);
    const range = Math.abs(d.viewport[5] - d.viewport[4]);
    // (none for a zero-width depth range: every depth is the same there)
    vt.bias = d.topology >= 3 && Number.isFinite(f32) && f32 !== 0 && range > 1e-6 &&
              window.s3dBiasMode !== 1 && window.s3dBiasMode !== 2
      ? Math.round(f32 / range * 1e9) / 1e9 : 0;  // (added to the final depth, flipped range or not)
    d.vt = vt;
    d.vtKey = (window.s3dExp || '') + (vt.gl ? 'g' : '') + (vt.flipZ ? 'f' : '') + (vt.fit ? vt.fit.join(':') : '') + (vt.bias ? 'b' + vt.bias : '');
    return { rect: [x0, y0, cw, ch] };
  }

  // The stencil reference and masks (params 3, or 4 for back faces): WebGPU
  // has one set for both, so a draw culling front faces - only back faces
  // drawn - takes the back ones (the native SelectNativeStencilMaskRef).
  function stencilWord(d) {
    const p = d.params, culled = p[5] & 0x100 ? p[5] & 3 : 0;
    return (p[0] & 2) && culled === 1 ? p[4] : p[3];
  }
  function rasterState(d) {
    const p = d.params, f32 = i => new Float32Array(new Uint32Array([p[i]]).buffer)[0];
    const face = w => ({ compare: compares[w & 7], failOp: stencilOps[(w >> 4) & 7],
                         depthFailOp: stencilOps[(w >> 8) & 7], passOp: stencilOps[(w >> 12) & 7] });
    // (window.s3dNoStencil, debugging: pixel shaders whose draws ignore the stencil test)
    const enabled = (p[0] & 1) && !(window.s3dNoStencil && window.s3dNoStencil.has(d.ps)), twoSided = p[0] & 2;
    const off = { compare: 'always', failOp: 'keep', depthFailOp: 'keep', passOp: 'keep' };
    const front = enabled ? face(p[1]) : off;
    const back = enabled ? (twoSided ? face(p[2]) : front) : off;
    // (window.s3dNoCull, debugging: no draw culls faces)
    let cullFace = window.s3dNoCull ? 0 : p[5] & 0x100 ? (p[5] & 3) : 0;
    // (window.s3dFlipCull, debugging: pixel shaders whose draws cull the other faces)
    if (window.s3dFlipCull && window.s3dFlipCull.has(d.ps) && cullFace) cullFace = 3 - cullFace;
    // Depth bias as the native renderer scales it: the constant in units of
    // the 24-bit depth's smallest step (2^24, kD3D10PolygonOffsetFactorFloat24),
    // the slope per 1/16 pixel (kPolygonOffsetScaleSubpixelUnit); none for
    // points and lines (WebGPU allows it only on triangles).
    const bias = f32(11), slope = f32(12), triangles = d.topology >= 3;
    const r = {
      cullMode: ['none', 'front', 'back'][cullFace] || 'none',
      frontFace: p[5] & 0x10 ? 'cw' : 'ccw',
      stencilFront: front, stencilBack: back,
      stencilReadMask: enabled ? (stencilWord(d) >> 8) & 255 : 0xFF,
      stencilWriteMask: enabled ? (stencilWord(d) >> 16) & 255 : 0,
      // (window.s3dBiasMode, debugging: 1 the earlier unscaled bias, 2 none)
      // (the constant part is applied in the vertex shader, vertexTransform:
      // a float depth format's bias unit depends on the depth itself)
      depthBias: triangles && Number.isFinite(bias) && window.s3dBiasMode === 1 ? Math.round(bias) : 0,
      depthBiasSlopeScale: triangles && Number.isFinite(slope) && window.s3dBiasMode !== 2 ? (window.s3dBiasMode === 1 ? slope : slope / 16) : 0,
    };
    // (no depth surface bound: no depth or stencil test, as the native renderer)
    if (d.noDepth) { r.stencilFront = r.stencilBack = off; r.stencilReadMask = 0xFF; r.stencilWriteMask = 0; }
    r.key = [enabled && !d.noDepth ? p[1] : 0, enabled && twoSided && !d.noDepth ? p[2] : 0, r.stencilReadMask, r.stencilWriteMask,
             cullFace, r.frontFace, r.depthBias, r.depthBiasSlopeScale, p[17] & 1, d.noDepth ? 'nd' : ''].join(',');
    return r;
  }

  // The pipeline for a draw, or null while it is being built (or failed).
  // A pipeline's identity from the draw's numbers (no layouts built): the
  // shader pair, declaration, streams and fixed-function state.
  // (the last draw's key and the values it was made from: consecutive draws
  // mostly share them, and comparing numbers is far cheaper than making the
  // string again)
  const qkValues = new Float64Array(40), qkLast = new Float64Array(40);
  let qkLastKey = null, qkLastVs = null, qkLastPs = null, qkLastVt = null;
  // (any of quickKeyString's debugging switches set: read once a frame, render)
  let qkDebugOn = false;
  const qkDebug = () => !!(window.s3dNoCull || window.s3dNoStencil || window.s3dFlipCull || window.s3dSolid ||
                           window.s3dNoDepth || window.s3dUnclipped);
  function quickKey(d) {
    const st = d.streams, p = d.params, v = qkValues;
    let n = 0;
    v[n++] = d.decl;
    for (let s = 0; s < 4; s++) v[n++] = st[s].data ? st[s].stride : -1;
    v[n++] = d.topology; v[n++] = d.index32; v[n++] = d.indexed ? 1 : 0; v[n++] = d.viewport[4] === d.viewport[5] ? 1 : 0;
    v[n++] = d.noDepth ? 1 : 0; v[n++] = d.alphaSpec; v[n++] = d.targetMask; v[n++] = p[19];
    for (let i = 0; i < 4; i++) v[n++] = d.blends[i];
    v[n++] = d.writeMask; v[n++] = d.depthEnable; v[n++] = d.depthFunction; v[n++] = d.depthWrite;
    for (let i = 0; i < 6; i++) v[n++] = p[i];
    v[n++] = p[11]; v[n++] = p[12]; v[n++] = p[17] & 1;
    if (qkLastKey !== null && d.vs === qkLastVs && d.ps === qkLastPs && d.vtKey === qkLastVt && !qkDebugOn) {
      let same = true;
      for (let i = 0; i < n; i++) if (v[i] !== qkLast[i]) { same = false; break; }
      if (same) return qkLastKey;
    }
    qkLast.set(v);
    qkLastVs = d.vs; qkLastPs = d.ps; qkLastVt = d.vtKey;
    return qkLastKey = quickKeyString(d);
  }
  function quickKeyString(d) {
    const st = d.streams, p = d.params;
    return d.vs + d.ps + d.decl + ',' + (st[0].data ? st[0].stride : -1) + ',' + (st[1].data ? st[1].stride : -1) + ',' +
      (st[2].data ? st[2].stride : -1) + ',' + (st[3].data ? st[3].stride : -1) + ',' + d.topology + ',' + d.index32 +
      ',' + d.indexed + ',' + d.vtKey + ',' + (d.viewport[4] === d.viewport[5]) + ',' + d.noDepth + ',' + d.alphaSpec + ',' + d.targetMask + ',' + (p[19] >>> 0) + ',' + d.blends[0] + ',' + d.blends[1] + ',' + d.blends[2] + ',' +
      d.blends[3] + ',' + d.writeMask + ',' + d.depthEnable + ',' + d.depthFunction + ',' + d.depthWrite + ',' +
      p[0] + ',' + p[1] + ',' + p[2] + ',' + p[3] + ',' + p[4] + ',' + p[5] + ',' + p[11] + ',' + p[12] + ',' + (p[17] & 1) + (window.s3dNoCull ? ',nc' : '') + (window.s3dNoStencil && window.s3dNoStencil.has(d.ps) ? ',ns' : '') + (window.s3dFlipCull && window.s3dFlipCull.has(d.ps) ? ',fc' : '') + (window.s3dSolid && window.s3dSolid.has(d.ps) ? ',so' : '') + (window.s3dNoDepth && window.s3dNoDepth.has(d.ps) ? ',nd' : '') + (window.s3dUnclipped && window.s3dUnclipped.has(d.ps) ? ',uc' : '');
  }
  const quickPipelines = new Map();  // quickKey -> { pipeline, layouts, groups }

  // Explicit bind group layouts for a shader pair (from the WGSL bindings):
  // the uniform blocks with dynamic offsets, so one bind group per frame
  // serves every draw of the pair. groups[g] = { layout, bindings } or null.
  const textureLayouts = {
    'texture_2d<f32>': { sampleType: 'float', viewDimension: '2d' },
    'texture_2d_array<f32>': { sampleType: 'float', viewDimension: '2d-array' },
    'texture_3d<f32>': { sampleType: 'float', viewDimension: '3d' },
    'texture_cube<f32>': { sampleType: 'float', viewDimension: 'cube' },
    'texture_cube_array<f32>': { sampleType: 'float', viewDimension: 'cube-array' },
    'texture_depth_2d': { sampleType: 'depth', viewDimension: '2d' },
    'texture_depth_2d_array': { sampleType: 'depth', viewDimension: '2d-array' },
    'texture_depth_cube': { sampleType: 'depth', viewDimension: 'cube' },
  };
  let layoutSerial = 0;
  function groupLayouts(vs, ps) {
    vs.groupLayouts ??= {};
    const cacheKey = ps ? ps.code.length + ':' + ps.info.bindings.length + ':' + (ps.id ??= ++layoutSerial) : '-';
    if (vs.groupLayouts[cacheKey]) return vs.groupLayouts[cacheKey];
    const byGroup = [new Map(), new Map(), new Map()];
    for (const info of [vs.info, ps && ps.info]) {
      if (!info) continue;
      for (const b of info.bindings) if (b.group < 3 && !byGroup[b.group].has(b.binding)) byGroup[b.group].set(b.binding, b);
    }
    const visibility = GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT;
    const groups = byGroup.map((bindings, g) => {
      const sorted = [...bindings.values()].sort((a, b) => a.binding - b.binding);
      const entries = [];
      for (const b of sorted) {
        if (g === 0) entries.push({ binding: b.binding, visibility, buffer: { type: 'uniform', hasDynamicOffset: true } });
        else if (g === 1) {
          const t = textureLayouts[b.type.replace(/\s/g, '')];
          if (!t) return undefined;  // (a texture type this renderer doesn't bind: use the auto layout)
          entries.push({ binding: b.binding, visibility, texture: t });
        } else entries.push({ binding: b.binding, visibility, sampler: { type: b.type === 'sampler_comparison' ? 'comparison' : 'filtering' } });
      }
      return { layout: device.createBindGroupLayout({ entries }), bindings: sorted, id: ++layoutSerial };
    });
    if (groups.includes(undefined)) return vs.groupLayouts[cacheKey] = null;
    const used = groups[2].bindings.length ? 3 : groups[1].bindings.length ? 2 : 1;
    const result = { groups: groups.slice(0, used),
                     pipelineLayout: device.createPipelineLayout({ bindGroupLayouts: groups.slice(0, used).map(x => x.layout) }) };
    return vs.groupLayouts[cacheKey] = result;
  }

  // The pipelines this browser has built, kept between visits (in the
  // site's storage, OPFS) as the draw state they were built for, and built
  // again in the background when the page starts: a scene's first frames
  // otherwise wait on each new pipeline's compile (the intro's stutters).
  const kPipelineFile = 'scene3d-pipelines-v1.json';
  const recordedPipelines = new Map();  // fullKey -> draw state
  let recordedDirty = false;
  function recordPipeline(fullKey, d, decl) {
    if (d.prewarm || recordedPipelines.has(fullKey) || recordedPipelines.size >= 6000) return;
    recordedPipelines.set(fullKey, {
      key: fullKey, vs: d.vs, ps: d.ps, psHash: !!d.psHash, decl: d.decl, declElements: decl,
      streams: d.streams.map(st => ({ data: st.data ? 1 : 0, stride: st.stride })),
      topology: d.topology, index32: d.index32, indexed: d.indexed, viewport: [...d.viewport],
      blends: [...d.blends], params: [...d.params], writeMask: d.writeMask, depthEnable: d.depthEnable,
      depthFunction: d.depthFunction, depthWrite: d.depthWrite, noDepth: d.noDepth, alphaSpec: d.alphaSpec,
      targetMask: d.targetMask, vt: d.vt, vtKey: d.vtKey,
    });
    recordedDirty = true;
  }
  async function pipelineFile(create) {
    const root = await navigator.storage.getDirectory();
    return root.getFileHandle(kPipelineFile, { create });
  }
  setInterval(async () => {
    if (!recordedDirty) return;
    recordedDirty = false;
    try {
      const writable = await (await pipelineFile(true)).createWritable();
      await writable.write(JSON.stringify([...recordedPipelines.values()]));
      await writable.close();
    } catch (e) {}
  }, 15000);
  async function prewarmPipelines() {
    let saved = [];
    try { saved = JSON.parse(await (await (await pipelineFile(false)).getFile()).text()); } catch (e) {
      // (a first visit: the record shipped with the page, scene3d-pipelines.json)
      try { saved = await (await fetch('scene3d-pipelines.json')).json(); } catch (e2) { return; }
    }
    stats.prewarm = { saved: saved.length, built: 0 };
    // (kept: this visit's new ones are added to them)
    for (const d of saved) if (d.key && !recordedPipelines.has(d.key)) recordedPipelines.set(d.key, d);
    // (every shader they use loading at once)
    for (const d of saved) { shader(d.vs); if (d.psHash) shader(d.ps); }
    let next = 0;
    const step = () => {
      // (a few each turn, once their shaders have loaded: the page stays responsive)
      let started = 0;
      while (next < saved.length && started < 24) {
        const d = saved[next];
        const vs = shader(d.vs), ps = d.psHash ? shader(d.ps) : null;
        const waiting = (!vs && shaders.get(d.vs)?.state === 'loading') ||
                        (d.psHash && !ps && shaders.get(d.ps)?.state === 'loading');
        if (waiting) break;
        next++;
        if (!vs || (d.psHash && !ps)) continue;
        d.prewarm = true;
        d.params = Uint32Array.from(d.params);
        d.viewport = Float32Array.from(d.viewport);
        try { pipelineFor(d, vs, ps, d.declElements); stats.prewarm.built++; } catch (e) {}
        started++;
      }
      if (next < saved.length) setTimeout(step, 4);
    };
    step();
  }

  function pipelineFor(d, vs, ps, decl) {
    const quick = quickKey(d);
    const hit = quickPipelines.get(quick);
    if (hit) return hit.pipeline ? hit : null;
    // Vertex buffers: one per stream the shader's inputs read, then one
    // zero-stride buffer per input the declaration doesn't supply.
    const layouts = [], streamSlots = {};
    let zeroInputs = [];
    const packed = [];  // [location, xenos type] of float inputs converted in the shader
    for (const input of vs.info.inputs) {
      const kind = kindOf(input.type);
      const element = decl.find(e => e.location === input.location);
      const convert = element && kind === 'f' && conversions[element.type];
      if (convert) packed.push([input.location, element.type]);
      const format = element && d.streams[element.stream] && d.streams[element.stream].data &&
        (convert ? convert.format : vertexFormat(element.type, kind));
      const stride = element ? d.streams[element.stream]?.stride : 0;
      if (!format || !stride || stride % 4 || element.offset + formatSize[format] > stride) {
        zeroInputs.push({ shaderLocation: input.location, offset: 0, format: zeroFormat[kind] });
        continue;
      }
      if (streamSlots[element.stream] === undefined) {
        streamSlots[element.stream] = layouts.length;
        layouts.push({ arrayStride: stride, attributes: [], stream: element.stream });
      }
      layouts[streamSlots[element.stream]].attributes.push({ shaderLocation: input.location, offset: element.offset, format });
    }
    if (zeroInputs.length) layouts.push({ arrayStride: 0, attributes: zeroInputs, stream: -1 });
    const topology = ['point-list', 'line-list', 'line-strip', 'triangle-list', 'triangle-strip'][d.topology];
    // (the position's transform: see packedVariant, render's vertexTransform)
    const vt = d.vt;
    const strip = topology.endsWith('strip');
    const key = [d.vs, d.ps, layouts.map(l => l.arrayStride + ':' + l.attributes.map(a => a.shaderLocation + '/' + a.offset + '/' + a.format).join(',')).join(';'),
      packed.map(p => p.join(':')).join('.'), d.vtKey, d.viewport[4] === d.viewport[5], d.noDepth, d.alphaSpec, topology, strip && d.indexed ? d.index32 : '', d.targetMask, d.params[19] >>> 0, d.blends.join(','), d.writeMask, d.depthEnable, d.depthFunction, d.depthWrite].join('|');
    const raster = rasterState(d);
    const fullKey = key + '|' + (window.s3dExp || '') + raster.key + (window.s3dSolid && window.s3dSolid.has(d.ps) ? '|solid' : '') + (window.s3dNoDepth && window.s3dNoDepth.has(d.ps) ? '|nd' : '') + (window.s3dUnclipped && window.s3dUnclipped.has(d.ps) ? '|uc' : '');
    const explicit = groupLayouts(vs, ps);
    const entry = { pipeline: null, layouts, explicit, fullKey };
    quickPipelines.set(quick, entry);
    const cached = pipelines.get(fullKey);
    if (cached) {
      if (typeof cached !== 'string') entry.pipeline = cached;
      return entry.pipeline ? entry : null;
    }
    pipelines.set(fullKey, 'pending');
    recordPipeline(fullKey, d, decl);
    // One target per bound colour surface (null where none is bound).
    const targets = [];
    for (let i = 0; i < 4; i++) {
      if (!(d.targetMask >> i & 1)) { if (d.targetMask >> i) targets.push(null); continue; }
      // (only targets the pixel shader writes, as the native renderer masks
      // with its colour output mask: WebGPU rejects a written target the
      // shader has no output for)
      const format = targetFormat(d, i);
      const channels = format === 'r32float' ? 0x1 : format === 'rg32float' ? 0x3 : 0xF;
      const target = { format, writeMask: ps && ps.outputs && ps.outputs.has(i) ? (d.writeMask >> (4 * i)) & channels : 0 };
      const b = d.blends[i];
      if ((b & 0x80000000) && target.writeMask) {
        target.blend = { color: blendComponent(b & 0x1F, (b >>> 5) & 7, (b >>> 8) & 0x1F),
                         alpha: blendComponent((b >>> 16) & 0x1F, (b >>> 21) & 7, (b >>> 24) & 0x1F) };
      }
      targets.push(target);
    }
    const vertexModule = packed.length || vt.gl || vt.flipZ || vt.fit || vt.bias ? packedVariant(vs, packed, vt) : vs.module;
    const descriptor = {
      layout: explicit ? explicit.pipelineLayout : 'auto',
      vertex: { module: vertexModule, entryPoint: 'shaderMain',
                constants: vertexModule.rexFit || vertexModule.rexBias ? Object.assign({},
                  vertexModule.rexFit ? { rex_vp_sx: vt.fit[0], rex_vp_sy: vt.fit[1], rex_vp_ox: vt.fit[2], rex_vp_oy: vt.fit[3] } : {},
                  vertexModule.rexBias ? { rex_depth_bias: vt.bias } : {}) : undefined,
                buffers: layouts.map(l => ({ arrayStride: l.arrayStride, stepMode: 'vertex', attributes: l.attributes })) },
      primitive: { topology, stripIndexFormat: strip && d.indexed ? (d.index32 ? 'uint32' : 'uint16') : undefined,
                   // (depth clamp: no near/far clipping, web_frame_capture.h params 17)
                   // (also for a viewport depth range of zero width - the sky
                   // and cloud dome, pinned to the far plane: their depth is
                   // the same whatever z is, so clipping by z can only cut
                   // pieces out of them, which it did, leaving cloud blocks)
                   // (window.s3dUnclipped, debugging: pixel shaders drawn without depth clipping)
                   unclippedDepth: !(window.s3dExp || '').includes('clip') && !!((d.params[17] & 1) || d.viewport[4] === d.viewport[5] ||
                                      (window.s3dUnclipped && window.s3dUnclipped.has(d.ps))) && device.features.has('depth-clip-control'),
                   cullMode: raster.cullMode, frontFace: raster.frontFace },
      depthStencil: { format: DEPTH, depthWriteEnabled: !!(d.depthEnable && d.depthWrite) && !d.noDepth,
                      depthCompare: d.depthEnable && !d.noDepth && !(window.s3dNoDepth && window.s3dNoDepth.has(d.ps)) ? compares[d.depthFunction] : 'always',
                      stencilFront: raster.stencilFront, stencilBack: raster.stencilBack,
                      stencilReadMask: raster.stencilReadMask, stencilWriteMask: raster.stencilWriteMask,
                      depthBias: raster.depthBias, depthBiasSlopeScale: raster.depthBiasSlopeScale },
    };
    // Depth-only draws still need the pass's colour targets, written by nothing.
    dummies.noPixel ??= device.createShaderModule({ code: '@fragment fn main() {}' });
    // (window.s3dSolid, debugging: pixel shaders drawn as solid red instead)
    if (ps && window.s3dSolid && window.s3dSolid.has(d.ps)) {
      const outs = targets.map((t, i) => t ? '@location(' + i + ') o' + i + ': vec4<f32>' : '').filter(Boolean).join(', ');
      const sets = targets.map((t, i) => t ? 'o.o' + i + ' = vec4<f32>(1.0, 0.0, 0.0, 1.0);' : '').join(' ');
      ps = { module: device.createShaderModule({ code: 'struct O { ' + outs + ' } @fragment fn shaderMain() -> O { var o: O; ' + sets + ' return o; }' }) };
    }
    // (the alpha test: see withAlphaTest)
    descriptor.fragment = ps ? { module: ps.module, entryPoint: 'shaderMain', targets,
                                 constants: ps.alphaTest ? { rex_alpha_test: d.alphaSpec } : undefined }
      : { module: dummies.noPixel, entryPoint: 'main', targets };
    device.createRenderPipelineAsync(descriptor).then(p => {
      pipelines.set(fullKey, p);
      // (every quick key sharing this pipeline picks it up)
      for (const q of quickPipelines.values()) if (q.fullKey === fullKey) q.pipeline = p;
    }).catch(e => {
      pipelines.set(fullKey, 'failed');
      stats.pipelineFailures = (stats.pipelineFailures || 0) + 1;
      if (stats.pipelineFailures <= 8) console.warn('scene3d pipeline', d.vs, d.ps, e.message);
      (window.scene3dErrors ??= []).push(d.vs + '/' + d.ps + ': ' + e.message.slice(0, 300));
    });
    return null;
  }

  // Guest bytes padded to a multiple of 4 (what writeBuffer takes).
  function padded(heap, at, size) {
    const out = new Uint8Array((size + 3) & ~3);
    out.set(new Uint8Array(heap, at, size));
    return out;
  }
  // A converted guest buffer on the GPU, uploaded again when its serial changes.
  // (this frame's own bytes - user-pointer vertices, generated indices - in
  // one buffer, render's arena: buffer.arena its offset there)
  let arenaBuffer = null;
  function gpuBuffer(heap, buffer, usage) {
    if (!buffer.id && buffer.arena !== undefined && arenaBuffer) return arenaBuffer;
    if (!buffer.id) {
      const b = device.createBuffer({ size: Math.max(16, (buffer.size + 3) & ~3), usage: usage | GPUBufferUsage.COPY_DST });
      device.queue.writeBuffer(b, 0, padded(heap, buffer.data, buffer.size));
      stats.transient.push(b);
      return b;
    }
    let entry = buffers.get(buffer.id);
    if (!entry || entry.serial !== buffer.serial || entry.size < buffer.size) {
      if (entry) entry.buffer.destroy();
      const b = device.createBuffer({ size: Math.max(16, (buffer.size + 3) & ~3),
        usage: GPUBufferUsage.VERTEX | GPUBufferUsage.INDEX | GPUBufferUsage.COPY_DST });
      device.queue.writeBuffer(b, 0, padded(heap, buffer.data, buffer.size));
      entry = { serial: buffer.serial, buffer: b, size: buffer.size };
      buffers.set(buffer.id, entry);
      stats.uploadedBytes += buffer.size;
    }
    entry.used = frameSerial;
    return entry.buffer;
  }

  // (word views of the heap, made again when it grows; draw records are
  // 8-byte aligned, so `at / 4` is exact - and stays right past 4 GiB)
  let heapWords = null, heapFloats = null;
  const hexCache = new Map();
  function hexOf(lo, hi) {
    let row = hexCache.get(hi);
    if (!row) hexCache.set(hi, row = new Map());
    let text = row.get(lo);
    if (!text) row.set(lo, text = hex(lo, hi));
    return text;
  }
  function readBuffer(u32, w) {
    return { data: u32[w] + u32[w + 1] * 4294967296, id: u32[w + 2], serial: u32[w + 3],
             size: u32[w + 4], offset: u32[w + 5], stride: u32[w + 6] };
  }
  // (params as floats, read where the capture left them)
  function paramFloat(i) { return heapFloats[this.paramsAt + i]; }
  // A WebDraw (web_frame_capture.h), by its byte offsets / 4. Its arrays are
  // views of the capture's memory, valid while this frame replays (kept
  // records copy them: recordPipeline).
  function readDraw(m, at) {
    if (!heapWords || heapWords.buffer !== m.buffer) {
      heapWords = new Uint32Array(m.buffer);
      heapFloats = new Float32Array(m.buffer);
    }
    const u32 = heapWords, w = at / 4;
    const d = {
      vs: hexOf(u32[w], u32[w + 1]),
      ps: hexOf(u32[w + 2], u32[w + 3]),
      psHash: (u32[w + 2] | u32[w + 3]) !== 0,
      constants: u32[w + 4] + u32[w + 5] * 4294967296,
      psConstants: u32[w + 130] + u32[w + 131] * 4294967296,
      sharedConstants: u32[w + 132] + u32[w + 133] * 4294967296,
      indices: readBuffer(u32, w + 6),
      index32: u32[w + 14], topology: u32[w + 15],
      count: u32[w + 16], first: u32[w + 17], baseVertex: u32[w + 18] | 0,
      decl: u32[w + 19], stage: u32[w + 20], blend: u32[w + 21],
      writeMask: u32[w + 22], depthEnable: u32[w + 23],
      depthFunction: u32[w + 24], depthWrite: u32[w + 25],
      viewport: heapFloats.subarray(w + 27, w + 33),
      targetWidth: u32[w + 33], targetHeight: u32[w + 34],
      textures: u32.subarray(w + 35, w + 61),
      samplerWords: u32.subarray(w + 134, w + 160),
      streams: [readBuffer(u32, w + 62), readBuffer(u32, w + 70), readBuffer(u32, w + 78), readBuffer(u32, w + 86)],
      kind: u32[w + 94],
      colors: [u32[w + 95], u32[w + 96], u32[w + 97], u32[w + 98]],
      depth: u32[w + 99],
      sizes: [u32[w + 100], u32[w + 101], u32[w + 102], u32[w + 103], u32[w + 104]],
      blends: [u32[w + 105], u32[w + 106], u32[w + 107], u32[w + 108]],
      params: u32.subarray(w + 109, w + 129),
      paramsAt: w + 109,
      paramFloat,
    };
    d.indexed = d.indices.data !== 0;
    return d;
  }
  const declCache = new Map();
  function readDecl(m, table, index) {
    const at = table + index * DECL_SIZE;
    const count = m.getUint32(at, true), elements = [];
    for (let i = 0; i < count; i++) {
      const e = at + 4 + i * 12;
      elements.push({ stream: m.getUint16(e, true), offset: m.getUint16(e + 2, true),
                      type: m.getUint32(e + 4, true), location: m.getUint32(e + 8, true) });
    }
    return elements;
  }

  // A sampler for a stage's state (web_frame_capture.h WebDraw::samplers):
  // Xenos clamp modes and filters in WebGPU's terms (no border colours or
  // half-way clamps there; only mip 0 is uploaded, so no mip filtering).
  const samplerCache = new Map();
  const addressModes = ['repeat', 'mirror-repeat', 'clamp-to-edge', 'mirror-repeat',
                        'clamp-to-edge', 'mirror-repeat', 'clamp-to-edge', 'mirror-repeat'];
  function samplerFor(word, comparison) {
    if (!(word & 0x80000000)) return comparison ? samplers.sampler_comparison : samplers.sampler;
    const key = (word & 0x7FFFFFFF) * 4 + (comparison ? 1 : 0) + (window.s3dMip0 ? 2 : 0);
    let sampler = samplerCache.get(key);
    if (!sampler) {
      // (TextureFilter: 0 point, 1 linear, 2 base map, 3 use the fetch
      // constant's - point, as the native renderer takes it)
      const filter = f => f === 1 ? 'linear' : 'nearest';
      const mag = filter((word >> 9) & 3), min = filter((word >> 11) & 3), mipMode = (word >> 13) & 3;
      // (mip filter 2 = the base map only; aniso 1-5 = 1x..16x, when all filters are linear)
      const aniso = (word >> 15) & 7;
      const mipmapFilter = mipMode === 1 ? 'linear' : 'nearest';
      // (the fetch constant's mip range, bits 19-22 and 23-26)
      const mipMin = (word >>> 19) & 15, mipMax = Math.max(mipMin, (word >>> 23) & 15);
      const maxAnisotropy = aniso >= 1 && aniso <= 5 && mag === 'linear' && min === 'linear' && mipmapFilter === 'linear'
        ? 1 << (aniso - 1) : 1;
      sampler = device.createSampler({ addressModeU: addressModes[word & 7], addressModeV: addressModes[(word >> 3) & 7],
        addressModeW: addressModes[(word >> 6) & 7], magFilter: mag, minFilter: min, mipmapFilter,
        lodMinClamp: mipMode === 2 || window.s3dMip0 ? 0 : window.s3dNoMipClamp ? 0 : mipMin, lodMaxClamp: mipMode === 2 || window.s3dMip0 ? 0 : window.s3dNoMipClamp ? 32 : mipMax, maxAnisotropy,
        compare: comparison ? 'less-equal' : undefined });
      samplerCache.set(key, sampler);
    }
    return sampler;
  }

  // The view a texture binding gets for a draw (web_frame_capture textures:
  // table index + 1, or the top bit and a resolve id), or a stand-in.
  let textureViewFn = null;
  // (kind 0 2D views, kind 3 cube ones: a cube map's view is marked rexCube;
  // the top bit and bit 30: a packed-depth alias of a depth resolve, the id
  // in the low 18 bits - Scene3D.packedAlias made it)
  function bindingView(b, d) {
    const kind = b.binding >> 5, t = d.textures[b.binding & 31];
    let view = null;
    const cube = kind === 3 && b.type === 'texture_cube<f32>';
    if (t && ((kind === 0 && b.type === 'texture_2d<f32>') || cube)) {
      if ((t >>> 30) === 3) view = packedAliases.get(t >>> 0)?.view;
      else view = t & 0x80000000 ? useResolved(t & 0x3FFFF)?.view : textureViewFn(t - 1);
      if (view && !!view.rexCube !== cube) view = null;
      if (view && (t & 0x80000000)) stats.sampledTargets = (stats.sampledTargets || 0) + 1;
    }
    return view || dummyTexture(b.type.replace(/<.*>/, '').replace(/\s/g, ''));
  }
  const packedAliases = new Map();  // texture word -> { texture, view, serial }
  const viewIds = new WeakMap();
  let nextViewId = 1;
  function viewId(v) {
    let id = viewIds.get(v);
    if (!id) viewIds.set(v, id = nextViewId++);
    return id;
  }
  const cachedGroups = new Map();
  let frameConstantGroups = new Map();

  // A stand-in for textures this renderer doesn't have (transparent black, so
  // blended effects sampling one add nothing).
  function dummyTexture(type) {
    if (dummies[type]) return dummies[type];
    const depth = type.startsWith('texture_depth');
    const dimension = type.includes('3d') ? '3d' : '2d';
    const layers = type.includes('cube') ? 6 : 1;
    const texture = device.createTexture({ size: [1, 1, layers], dimension, format: depth ? 'depth32float' : 'rgba8unorm',
      usage: GPUTextureUsage.TEXTURE_BINDING | (depth ? GPUTextureUsage.RENDER_ATTACHMENT : GPUTextureUsage.COPY_DST) });
    if (!depth) for (let l = 0; l < layers; l++)
      device.queue.writeTexture({ texture, origin: [0, 0, l] }, new Uint8Array([0, 0, 0, 0]), { bytesPerRow: 4 }, [1, 1, 1]);
    const viewDimension = type.includes('cube_array') ? 'cube-array' : type.includes('cube') ? 'cube'
      : type.includes('2d_array') ? '2d-array' : type.includes('3d') ? '3d' : '2d';
    return dummies[type] = texture.createView({ dimension: viewDimension });
  }

  return {
    // Device options the converted shaders need.
    deviceDescriptor(adapter) {
      const requiredLimits = {};
      for (const name of ['maxVertexAttributes', 'maxVertexBuffers', 'maxInterStageShaderVariables',
        'maxInterStageShaderComponents', 'maxSampledTexturesPerShaderStage', 'maxSamplersPerShaderStage',
        'maxUniformBuffersPerShaderStage', 'maxBindingsPerBindGroup', 'maxColorAttachmentBytesPerSample']) {
        if (name in adapter.limits) requiredLimits[name] = adapter.limits[name];
      }
      const requiredFeatures = ['clip-distances', 'depth32float-stencil8', 'texture-compression-bc', 'depth-clip-control',
                                'float32-filterable', 'float32-blendable']
        .filter(f => adapter.features.has(f));
      return { requiredLimits, requiredFeatures };
    },
    writtenThisFrame: new Set(),
    lastFrontResolve: 0,
    frontIds: new Set(),

    init(gpuDevice) {
      device = gpuDevice;
      DEPTH = device.features.has('depth32float-stencil8') ? 'depth32float-stencil8' : 'depth24plus-stencil8';
      float32Targets = device.features.has('float32-blendable') && device.features.has('float32-filterable');
      // Kept for reloading this script into a running game:
      //   eval(await (await fetch('scene3d.js')).text()); Scene3D.init(scene3dDevice)
      window.scene3dDevice = gpuDevice;
      const canvas = document.getElementById('scene');
      canvasContext = canvas.getContext('webgpu');
      canvasFormat = navigator.gpu.getPreferredCanvasFormat();
      canvasContext.configure({ device, format: canvasFormat, alphaMode: 'opaque',
                                usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_DST | GPUTextureUsage.COPY_SRC });
      samplers = {
        sampler: device.createSampler({ magFilter: 'linear', minFilter: 'linear', addressModeU: 'repeat', addressModeV: 'repeat', addressModeW: 'repeat' }),
        sampler_comparison: device.createSampler({ compare: 'less-equal', magFilter: 'linear', minFilter: 'linear' }),
      };
      // Copies a rectangle of a surface into a texture: `offset` is the source
      // origin minus the destination's. Depth surfaces copy their depth to red.
      // Colour resolves scale by the resolve's exponent (pad.x) and clamp to 0-1
      // into a normalized destination (pad.y), as the native resolve does.
      const copyCode = (source, load) => `
        struct Copy { offset: vec2i, pad: vec2i, scale: vec2i, pad2: vec2i }
        @group(0) @binding(0) var image: ${source};
        @group(0) @binding(1) var<uniform> copy: Copy;
        @vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
          let p = array(vec2f(-1, -1), vec2f(3, -1), vec2f(-1, 3));
          return vec4f(p[i], 0, 1);
        }
        @fragment fn fs(@builtin(position) p: vec4f) -> @location(0) vec4f {
          let size = vec2i(textureDimensions(image));
          let base = (vec2i(p.xy) + copy.offset) * copy.scale;
          let at = clamp(base, vec2i(0), size - 1);
          ${load}
        }`;
      const copyPipeline = (code, format) => {
        const module = device.createShaderModule({ code });
        return device.createRenderPipeline({ layout: 'auto', vertex: { module, entryPoint: 'vs' },
          fragment: { module, entryPoint: 'fs', targets: [{ format }] } });
      };
      // (a source twice as wide or tall, copy.scale: an MSAA view's samples,
      // sample k at (k % scale.x, k / scale.x), averaged as the resolve's
      // sample select asks - pad2.x: 0-3 one sample, 4 samples 0 and 1, 5
      // samples 2 and 3, 6 all; pad2.y: into a 16-bit float destination, NaN
      // to 0 and the rest clamped to +-65504, as resolve_convert_ps does)
      const colorLoad = `var sum = vec4f(0);
          var count = 0;
          let n = copy.scale.x * copy.scale.y;
          let sel = copy.pad2.x;
          for (var round = 0; round < 2 && count == 0; round++) {
            for (var k = 0; k < n; k++) {
              let chosen = round == 1 || n == 1 || sel >= 6 || (sel < 4 && k == sel) || (sel == 4 && k < 2) || (sel == 5 && k >= 2);
              if (!chosen) { continue; }
              sum += textureLoad(image, clamp(base + vec2i(k % copy.scale.x, k / copy.scale.x), vec2i(0), size - 1), 0);
              count++;
            }
          }
          var c = sum / f32(max(count, 1)) * exp2(f32(copy.pad.x));
          if (copy.pad2.y != 0) {
            let nan = (bitcast<vec4u>(c) & vec4u(0x7fffffffu)) > vec4u(0x7f800000u);
            c = clamp(select(c, vec4f(0), nan), vec4f(-65504), vec4f(65504));
          }
          return select(c, clamp(c, vec4f(0), vec4f(1)), copy.pad.y != 0);`;
      const colorPipelines = new Map();
      this.copyColorFor = format => {
        if (!colorPipelines.has(format)) colorPipelines.set(format, copyPipeline(copyCode('texture_2d<f32>', colorLoad), format));
        return colorPipelines.get(format);
      };
      this.copyColor = this.copyColorFor(COLOR);
      // Depth to red. What counts as empty (the sky, at reversed Z's far
      // plane: the final composite tests exactly 0) is decided on the depth
      // as the guest stores it, 24-bit unorm or 20e4 float (pad.y: float),
      // like the native scene_depth_handoff / packed_depth_alias shaders:
      // depths that store as 0 become 0. (pad.x, debugging: 1 inverted)
      const packDepth = `
        fn depth20e4(depth: f32) -> u32 {
          if (!(depth > 0.0)) { return 0u; }
          var bits = bitcast<u32>(depth);
          if (bits >= 0x3FFFFFF8u) { return 0xFFFFFFu; }
          if (bits < 0x38800000u) {
            let shift = min(113u - (bits >> 23u), 24u);
            bits = (0x800000u | (bits & 0x7FFFFFu)) >> shift;
          } else {
            bits += 0xC8000000u;
          }
          return (bits >> 3u) & 0xFFFFFFu;
        }
        fn packDepth(depth: f32, float24: bool) -> u32 {
          return select(u32(round(clamp(depth, 0.0, 1.0) * 16777215.0)), depth20e4(depth), float24);
        }`;
      this.packDepthCode = packDepth;
      const depthLoad = read => `var d = ${read};
          if (copy.pad.x == 1) { d = 1.0 - d; }
          if (packDepth(d, copy.pad.y != 0) == 0u) { d = 0.0; }
          return vec4f(d, d, d, 1);`;
      this.copyDepth = copyPipeline(packDepth + copyCode('texture_depth_2d', depthLoad('textureLoad(image, at, 0)')), COLOR);
      if (device.features.has('float32-filterable')) {
        this.copyDepth32 = copyPipeline(packDepth + copyCode('texture_depth_2d', depthLoad('textureLoad(image, at, 0)')), 'r32float');
        // (a depth resolve of a colour surface: GTA IV's scene depth drawn into one, in red)
        this.copyColor32 = copyPipeline(packDepth + copyCode('texture_2d<f32>', depthLoad('textureLoad(image, at, 0).r')), 'r32float');
      }
      // A depth resolve's stencil too, for textures that read the resolve as
      // packed D24S8 bytes (packed_depth_alias): the stencil-only view to red.
      this.copyStencil = copyPipeline(copyCode('texture_2d<u32>', 'return vec4f(f32(textureLoad(image, at, 0).r), 0, 0, 1);'), 'r32float');
      // The packed alias itself: depth << 8 | stencil as four bytes, through
      // the fetch swizzle (pad2.x; 0x60A puts stencil in blue), pad.y float depth.
      this.packAlias = (() => {
        const module = device.createShaderModule({ code: packDepth + `
          struct Alias { pad: vec2i, swizzle: u32, float24: u32 }
          @group(0) @binding(0) var depthImage: texture_2d<f32>;
          @group(0) @binding(1) var stencilImage: texture_2d<f32>;
          @group(0) @binding(2) var<uniform> packing: Alias;
          @vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
            let p = array(vec2f(-1, -1), vec2f(3, -1), vec2f(-1, 3));
            return vec4f(p[i], 0, 1);
          }
          @fragment fn fs(@builtin(position) p: vec4f) -> @location(0) vec4f {
            let at = vec2i(p.xy);
            let d = textureLoad(depthImage, clamp(at, vec2i(0), vec2i(textureDimensions(depthImage)) - 1), 0).r;
            let st = u32(textureLoad(stencilImage, clamp(at, vec2i(0), vec2i(textureDimensions(stencilImage)) - 1), 0).r) & 255u;
            let packed = (packDepth(d, packing.float24 != 0u) << 8u) | st;
            let raw = vec4f(vec4u(packed, packed >> 8u, packed >> 16u, packed >> 24u) & vec4u(255u)) / 255.0;
            var o = vec4f(0);
            for (var c = 0u; c < 4u; c++) {
              let k = (packing.swizzle >> (3u * c)) & 7u;
              o[c] = select(f32(k & 1u), raw[min(k, 3u)], k < 4u);
            }
            return o;
          }` });
        return device.createRenderPipeline({ layout: 'auto', vertex: { module, entryPoint: 'vs' },
          fragment: { module, entryPoint: 'fs', targets: [{ format: 'rgba8unorm' }] } });
      })();
      // (stretched to 1280x720, and brightened by 2^pad.x: for window.s3dShow)
      this.present = copyPipeline(copyCode('texture_2d<f32>', `let fit = clamp(vec2i(p.xy * vec2f(size) / vec2f(1280, 720)), vec2i(0), size - 1);
          let c = textureLoad(image, fit, 0);
          return vec4f(select(c.rgb, c.aaa, copy.pad.y != 0) * exp2(f32(copy.pad.x)), 1);`), canvasFormat);
      prewarmPipelines();
      // The Xbox 360's display gamma: D3D builds a 1024-entry, 10-bit ramp
      // from the display type the kernel reports (VdGetCurrentDisplayGamma:
      // TV, power 2.22) and the console applies it to every pixel it sends
      // out (sub_82A4CB88 fills it at 0x831CF6E8); the presented picture
      // goes through it as well (the shadows' depth the reference shows).
      {
        const module = device.createShaderModule({ code: `
          struct Ramp { v: array<vec4f, 256> }
          @group(0) @binding(0) var image: texture_2d<f32>;
          @group(0) @binding(2) var<uniform> ramp: Ramp;
          @vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
            let p = array(vec2f(-1, -1), vec2f(3, -1), vec2f(-1, 3));
            return vec4f(p[i], 0, 1);
          }
          fn entry(i: u32) -> f32 { return ramp.v[i / 4u][i % 4u]; }
          fn curve(x: f32) -> f32 {
            let f = clamp(x, 0.0, 1.0) * 1023.0;
            let i = u32(floor(f));
            return mix(entry(i), entry(min(i + 1u, 1023u)), fract(f)) / 1023.0;
          }
          @fragment fn fs(@builtin(position) p: vec4f) -> @location(0) vec4f {
            let size = vec2i(textureDimensions(image));
            let fit = clamp(vec2i(p.xy * vec2f(size) / vec2f(1280, 720)), vec2i(0), size - 1);
            let c = textureLoad(image, fit, 0).rgb;
            return vec4f(curve(c.r), curve(c.g), curve(c.b), 1);
          }` });
        this.presentRamped = device.createRenderPipeline({ layout: 'auto', vertex: { module, entryPoint: 'vs' },
          fragment: { module, entryPoint: 'fs', targets: [{ format: canvasFormat }] } });
        this.rampBuffer = device.createBuffer({ size: 4096, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
        this.rampValid = false;
        this.rampCheckedFrame = -1000;
      }
      device.addEventListener('uncapturederror', e => {
        stats.gpuErrors = (stats.gpuErrors || 0) + 1;
        if (stats.gpuErrors <= 12) (window.scene3dErrors ??= []).push('uncaptured: ' + e.error.message.slice(0, 300));
      });
    },

    // A guest surface's GPU texture, made (again) when its size or (when
    // given) its format changes.
    surface(handle, packedSize, depth, format) {
      const width = packedSize & 0xFFFF, height = packedSize >>> 16;
      if (!handle || !width || !height || width > 8192 || height > 8192) return null;
      let s = surfaces.get(handle);
      if (s && s.width === width && s.height === height && s.depth === depth && (!format || s.format === format)) {
        s.used = frameSerial;
        return s;
      }
      if (s) { s.texture.label += ' remade@' + frameSerial; stats.retired.push(s.texture); }
      format = depth ? DEPTH : format || (s && s.format) || COLOR;
      const texture = device.createTexture({ size: [width, height], format, label: 'surface ' + handle.toString(16),
        usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING });
      s = { texture, view: texture.createView(), width, height, depth, format, used: frameSerial,
            sampleView: depth ? texture.createView({ aspect: 'depth-only' }) : null,
            stencilView: depth ? texture.createView({ aspect: 'stencil-only' }) : null };
      surfaces.set(handle, s);
      return s;
    },

    // Replays the frame's events. `textureView(index)` gives the 2D view of
    // texture table entry `index` (0-based), or null.
    render(m, heap, frame, textureView, encoder) {
      const profStart = performance.now();
      qkDebugOn = qkDebug();
      const eventCount = m.getUint32(frame + 48, true);
      const declTable = Number(m.getBigUint64(frame + 64, true));
      const eventTable = Number(m.getBigUint64(frame + 56, true));
      const screenBase = m.getUint32(frame + 72, true);  // the front buffer's resolve id
      // (the header read: the game may now publish its next frame into the
      // other list - index.html, WebFrame::reading)
      if (this.onHeaderRead) this.onHeaderRead();
      textureViewFn = textureView;
      frameConstantGroups = new Map();  // (this frame's constants buffer)
      Object.assign(stats, { events: eventCount, drawn: 0, skippedShader: 0, skippedPipeline: 0, pending: 0, passes: 0,
        resolves: 0, clears: 0, uploadedBytes: 0, transient: [], retired: [] });
      if (!eventCount) return;
      const events = [];
      for (let i = 0; i < eventCount; i++) {
        const e = readDraw(m, eventTable + i * DRAW_SIZE);
        if (e.kind !== 0 || window.scene3dFilter(e, i)) events.push(e);
      }
      const profRead = performance.now();
      // Constants for every draw in one buffer, 256-byte aligned.
      // Each distinct block (vertex, pixel, shared constants) once: draws
      // sharing a block's bytes point at the same one (web_frame_capture.h).
      const drawEvents = events.filter(e => e.kind === 0);
      // (each block only as much as the shaders that read it need,
      // constantNeed: bindings still name 4 KiB, which may run on over the
      // next blocks - they are read-only - and past the end into padding)
      const blockNeeds = new Map();
      const need = (pointer, size) => blockNeeds.set(pointer, Math.max(blockNeeds.get(pointer) || 0, size));
      for (const d of drawEvents) {
        const v = shaders.get(d.vs), f = d.psHash ? shaders.get(d.ps) : null;
        const vn = v && v.constantNeed, fn = d.psHash ? f && f.constantNeed : [0, 0];
        need(d.constants, vn && fn ? Math.max(vn[0], fn[0]) : 4096);
        need(d.psConstants, vn && fn ? Math.max(vn[1], fn[1]) : 4096);
        need(d.sharedConstants, 1280);
      }
      const blockOffsets = new Map();
      let constantSize = 0;
      const place = pointer => {
        let block = blockOffsets.get(pointer);
        if (!block) {
          const size = blockNeeds.get(pointer);
          blockOffsets.set(pointer, block = { offset: constantSize, size });
          constantSize += Math.ceil(size / 256) * 256;
        }
        return block.offset;
      };
      for (const d of drawEvents) {
        d.cOff = [place(d.constants), place(d.psConstants), place(d.sharedConstants)];
      }
      const constantBytes = new Uint8Array(constantSize + 4096);
      for (const [pointer, block] of blockOffsets) {
        // (a block pointer outside memory - one the capture didn't resolve -
        // stays zeros rather than ending the frame's replay)
        if (!(pointer > 0 && pointer + block.size <= heap.byteLength)) { stats.badConstants = (stats.badConstants || 0) + 1; continue; }
        constantBytes.set(new Uint8Array(heap, pointer, block.size), block.offset);
      }
      // (window.s3dNoClip, debugging: no draw's user clip plane enabled, the
      // shared constants' byte at 0x240)
      if (window.s3dNoClip) for (const d of drawEvents) constantBytes[d.cOff[2] + 0x240] = 0;
      // (window.s3dNoLodBias, debugging: the samplers' LOD bias, shared 0x2F0, zeroed)
      if (window.s3dNoLodBias) for (const d of drawEvents) constantBytes.fill(0, d.cOff[2] + 0x2F0, d.cOff[2] + 0x2F0 + 26 * 4);
      // (window.s3dPsConst, debugging: { ps: hash, reg, value: [x, y, z, w] } replaces a
      // pixel constant register for that pixel shader's draws)
      const ov = window.s3dPsConst;
      if (ov) for (const d of drawEvents) {
        if (d.ps !== ov.ps) continue;
        new Float32Array(constantBytes.buffer, d.cOff[1] + ov.reg * 16, 4).set(ov.value);
      }
      stats.constantBlocks = blockOffsets.size;
      // This frame's own vertex and index bytes, one buffer and one upload (a
      // buffer each cost a createBuffer and a writeBuffer per draw).
      {
        const parts = [];
        let arenaSize = 0;
        const placeOwn = b => {
          if (!b || !b.data || b.id) return;
          b.arena = arenaSize;
          arenaSize += (b.size + 15) & ~15;
          parts.push(b);
        };
        for (const d of drawEvents) { if (d.indexed) placeOwn(d.indices); for (const st of d.streams) placeOwn(st); }
        arenaBuffer = null;
        if (arenaSize) {
          arenaBuffer = device.createBuffer({ size: arenaSize, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.INDEX | GPUBufferUsage.COPY_DST });
          const own = new Uint8Array(arenaSize);
          for (const b of parts) {
            if (b.data > 0 && b.data + b.size <= heap.byteLength) own.set(new Uint8Array(heap, b.data, b.size), b.arena);
          }
          device.queue.writeBuffer(arenaBuffer, 0, own);
          stats.transient.push(arenaBuffer);
          stats.ownBytes = arenaSize;
          stats.ownBuffers = parts.length;
        }
      }
      const constants = device.createBuffer({ size: constantBytes.byteLength, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
      const profCopy = performance.now();
      device.queue.writeBuffer(constants, 0, constantBytes);
      stats.transient.push(constants);
      const profUpload = performance.now();

      let pass = null, passKey = '';
      const endPass = () => { if (pass) { pass.end(); pass = null; passKey = ''; } };
      // A pass over the bound surfaces, applying their pending clears.
      const beginPass = e => {
        // (the pass already open over these surfaces: most draws - compared
        // field by field, the key made only for a new pass)
        const formats = e.kind === 0 || e.kind === 1 ? e.params[19] >>> 0 : -1;
        if (pass && pass.formats === formats && pass.depthHandle === e.depth) {
          let same = true;
          for (let i = 0; i < 4 && same; i++) same = pass.colorHandles[i] === e.colors[i];
          for (let i = 0; i < 5 && same; i++) same = pass.sizeWords[i] === e.sizes[i];
          if (same) return pass.mask;
        }
        const key = e.colors.join() + '|' + e.depth + '|' + e.sizes.join() + (formats >= 0 ? '|' + formats : '');
        if (pass && key === passKey) return pass.mask;
        const colors = [], first = firstSize(e);
        let mask = 0;
        for (let i = 0; i < 4; i++) {
          const s = e.colors[i] && e.sizes[i] === first
            ? this.surface(e.colors[i], e.sizes[i], false, e.kind === 0 || e.kind === 1 ? targetFormat(e, i) : undefined) : null;
          if (s) mask |= 1 << i;
          colors.push(s);
        }
        let depth = e.depth && e.sizes[4] === first ? this.surface(e.depth, e.sizes[4], true) : null;
        if (!depth) {
          // Attachments must match in size: a stand-in depth buffer.
          const width = first & 0xFFFF, height = first >>> 16;
          if (!width || !height) return null;
          // (asked for each time, not kept: one no pass uses for a while is
          // evicted, and a kept one would be a destroyed texture)
          depth = this.surface(-first, first, true);
        }
        endPass();
        const colorAttachments = [];
        for (let i = 0; i < 4; i++) {
          if (!colors[i]) { if (mask >> i) colorAttachments.push(null); continue; }
          const clear = pendingClears.get(e.colors[i]);
          pendingClears.delete(e.colors[i]);
          colorAttachments.push({ view: colors[i].view, loadOp: clear ? 'clear' : 'load', storeOp: 'store',
            clearValue: clear ? clear.color : undefined });
        }
        const dclear = pendingClears.get(e.depth);
        pendingClears.delete(e.depth);
        pass = encoder.beginRenderPass({ colorAttachments, depthStencilAttachment: { view: depth.view,
          depthLoadOp: dclear ? 'clear' : 'load', depthStoreOp: 'store', depthClearValue: dclear ? dclear.depth : 1,
          stencilLoadOp: dclear ? 'clear' : 'load', stencilStoreOp: 'store', stencilClearValue: dclear ? dclear.stencil : 0 } });
        pass.mask = mask; pass.width = first & 0xFFFF; pass.height = first >>> 16;
        pass.formats = formats; pass.depthHandle = e.depth;
        pass.colorHandles = [...e.colors]; pass.sizeWords = [...e.sizes];
        // (the state set in this pass, so a draw sets only what changed: each
        // call into WebGPU costs about a microsecond on a slow CPU, and most
        // draws repeat most of the last one's state)
        pass.s = { pipeline: null, bg: [null, null, null], off0: '', vb: [], vbOff: [], ib: null, ibOff: -1, ibFormat: '',
                   viewport: '', scissor: '', stencil: -1, blend: '' };
        passKey = key;
        stats.passes++;
        return mask;
      };
      // Pending clears of one surface, applied now (a resolve reads it).
      const flushClear = (handle, size, depth) => {
        const clear = pendingClears.get(handle);
        const s = this.surface(handle, size, depth);
        if (!clear || !s) return;
        pendingClears.delete(handle);
        const p = depth
          ? encoder.beginRenderPass({ colorAttachments: [], depthStencilAttachment: { view: s.view, depthLoadOp: 'clear',
              depthStoreOp: 'store', depthClearValue: clear.depth, stencilLoadOp: 'clear', stencilStoreOp: 'store', stencilClearValue: clear.stencil } })
          : encoder.beginRenderPass({ colorAttachments: [{ view: s.view, loadOp: 'clear', storeOp: 'store', clearValue: clear.color }] });
        p.end();
      };
      const floats = (e, at) => [0, 1, 2, 3].map(i => e.paramFloat(at + i));

      const profLoop = performance.now();
      for (const e of events) {
        if (e.kind === 1) {
          // Clear: of the bound surfaces, applied by the next pass over them.
          endPass();
          stats.clears++;
          const [r, g, b, a] = floats(e, 1);
          // (Xbox 360 flags: 1, 2, 4, 8 the colour targets, 0x10 depth, 0x20 stencil)
          e.colors.forEach((h, i) => { if (h && (e.params[0] & (1 << i))) pendingClears.set(h, { color: { r, g, b, a } }); });
          if ((e.params[0] & 0x30) && e.depth) pendingClears.set(e.depth, { depth: Math.min(1, Math.max(0, e.paramFloat(5))), stencil: e.params[6] & 255 });
          continue;
        }
        if (e.kind === 2) {
          endPass();
          stats.resolves++;
          this.resolve(e, encoder, flushClear, floats);
          continue;
        }
        if (e.kind === 3) {
          endPass();
          this.depthHandoff(e, encoder, flushClear);
          continue;
        }
        const d = e;
        // Debugging: window.s3dSkip, a set of shader hashes (vertex or pixel)
        // whose draws are left out.
        if (window.s3dSkip && (window.s3dSkip.has(d.vs) || window.s3dSkip.has(d.ps))) continue;
        // (window.s3dSkipIf, debugging: a predicate on the draw)
        if (window.s3dSkipIf && window.s3dSkipIf(d)) continue;
        // (window.s3dSkipNoPs: vertex shaders whose draws without a pixel shader are left out)
        if (window.s3dSkipNoPs && !d.psHash && window.s3dSkipNoPs.has(d.vs)) continue;
        const vs = shader(d.vs), ps = d.psHash ? shader(d.ps) : null;
        if (!vs || (d.psHash && !ps)) {
          stats.skippedShader++;
          // (still loading, not failed: render holds the last whole frame)
          if (shaders.get(d.vs)?.state === 'loading' || (d.psHash && shaders.get(d.ps)?.state === 'loading')) stats.pending++;
          continue;
        }
        // (the exposure adaptation's constants, for the frame log)
        if (d.ps === '12AA6B4951968E28' && d.psConstants > 0) {
          const c = new Float32Array(constantBytes.buffer, d.cOff[1] + 208 * 16, 16);
          stats.adaptation = [c[0], c[4], c[8], c[12]].map(v => Math.round(v * 1000) / 1000);
        }
        const p = d.params;
        // Draws the native renderer drops: point lists (no point-sprite
        // translation), a sample mask without sample 0 (one sample here), and
        // a viewport that isn't finite.
        if (d.topology === 0 || !((p[18] >>> 8) & 1) || !d.viewport.every(Number.isFinite)) {
          stats.droppedDraws = (stats.droppedDraws || 0) + 1;
          continue;
        }
        // (packed-depth aliases it samples, made before its pass opens)
        if (d.textures.some(t => (t >>> 30) === 3)) {
          for (const t of d.textures) if ((t >>> 30) === 3) this.packedAlias(t, encoder, endPass);
        }
        const mask = beginPass(d);
        if (mask === null) continue;
        d.targetMask = mask;
        d.noDepth = !(d.depth && d.sizes[4] === firstSize(d));
        // (the alpha test's specialization: enable, bit 1, and the function at 8)
        d.alphaSpec = p[18] & 1 && !window.s3dNoAlphaTest ? 2 | ((p[18] >>> 1) & 7) << 8 : 0;
        const fit = vertexTransform(d, pass.width, pass.height);
        if (!fit) { stats.droppedDraws = (stats.droppedDraws || 0) + 1; continue; }
        if (!declCache.has(d.decl)) declCache.set(d.decl, readDecl(m, declTable, d.decl));
        const profA = window.s3dProf ? performance.now() : 0;
        const built = pipelineFor(d, vs, ps, declCache.get(d.decl));
        const profB = window.s3dProf ? performance.now() : 0;
        if (!built) {
          stats.skippedPipeline++;
          const q = quickPipelines.get(quickKey(d));
          if (q && pipelines.get(q.fullKey) === 'pending') stats.pending++;
          continue;
        }
        const { pipeline, layouts, explicit } = built;
        const ps0 = pass.s;
        if (ps0.pipeline !== pipeline) { pass.setPipeline(pipeline); ps0.pipeline = pipeline; }
        if (explicit) {
          // Explicit layouts: the constants by dynamic offset (one bind group
          // per layout per frame), textures and samplers cached by contents.
          explicit.groups.forEach((g, gi) => {
            if (gi === 0) {
              let bg = frameConstantGroups.get(g.id);
              if (!bg) {
                bg = device.createBindGroup({ layout: g.layout, entries: g.bindings.map(b => ({ binding: b.binding,
                  resource: { buffer: constants, offset: 0, size: b.binding === 2 ? 1280 : 4096 } })) });
                frameConstantGroups.set(g.id, bg);
              }
              const offsets = g.bindings.map(b => d.cOff[b.binding] || 0);
              const off = offsets.join();
              if (ps0.bg[0] !== bg || ps0.off0 !== off) { pass.setBindGroup(0, bg, offsets); ps0.bg[0] = bg; ps0.off0 = off; }
            } else if (gi === 1) {
              const views = g.bindings.map(b => bindingView(b, d));
              let key = g.id + ':';
              for (const v of views) key += viewId(v) + ',';
              let bg = cachedGroups.get(key);
              if (!bg) {
                bg = device.createBindGroup({ layout: g.layout, entries: g.bindings.map((b, i) => ({ binding: b.binding, resource: views[i] })) });
                if (cachedGroups.size > 8000) cachedGroups.clear();
                cachedGroups.set(key, bg);
              }
              if (ps0.bg[1] !== bg) { pass.setBindGroup(1, bg); ps0.bg[1] = bg; }
            } else {
              let key = 's' + g.id + ':';
              for (const b of g.bindings) key += d.samplerWords[b.binding & 31] + ',';
              let bg = cachedGroups.get(key);
              if (!bg) {
                bg = device.createBindGroup({ layout: g.layout, entries: g.bindings.map(b => ({ binding: b.binding,
                  resource: samplerFor(d.samplerWords[b.binding & 31], b.type === 'sampler_comparison') })) });
                cachedGroups.set(key, bg);
              }
              if (ps0.bg[2] !== bg) { pass.setBindGroup(2, bg); ps0.bg[2] = bg; }
            }
          });
        }
        // Bind groups 0-2 as the shaders declare them (auto layouts).
        const groups = [[], [], []];
        if (!explicit) {
        for (const info of [vs.info, ps && ps.info]) {
          if (!info) continue;
          for (const b of info.bindings) {
            if (groups[b.group].some(x => x.binding === b.binding)) continue;
            let resource;
            if (b.group === 0) {
              resource = { buffer: constants, offset: d.cOff[b.binding] || 0, size: b.binding === 2 ? 1280 : 4096 };
            } else if (b.group === 1) {
              resource = bindingView(b, d);
            } else {
              resource = samplerFor(d.samplerWords[b.binding & 31], b.type === 'sampler_comparison');
            }
            groups[b.group].push({ binding: b.binding, resource });
          }
        }
        const maxGroup = groups[2].length ? 2 : groups[1].length ? 1 : 0;
        for (let g = 0; g <= maxGroup; g++) {
          pass.setBindGroup(g, device.createBindGroup({ layout: pipeline.getBindGroupLayout(g), entries: groups[g] }));
        }
        ps0.bg = [null, null, null];
        ps0.off0 = '';
        }
        const profC = window.s3dProf ? performance.now() : 0;
        for (let slot = 0; slot < layouts.length; slot++) {
          const l = layouts[slot];
          let buffer, offset = 0;
          if (l.stream < 0) {
            buffer = dummies.zeroVertex ??= device.createBuffer({ size: 64, usage: GPUBufferUsage.VERTEX });
          } else {
            const st = d.streams[l.stream];
            buffer = gpuBuffer(heap, st, GPUBufferUsage.VERTEX);
            offset = (st.offset & ~3) + (st.arena || 0);
          }
          if (ps0.vb[slot] !== buffer || ps0.vbOff[slot] !== offset) {
            pass.setVertexBuffer(slot, buffer, offset);
            ps0.vb[slot] = buffer; ps0.vbOff[slot] = offset;
          }
        }
        // The viewport: the part inside the surface (vertexTransform keeps the
        // guest's transform for the rest), and its depth range as the guest
        // set it (one from 1 down to 0 set the right way round: the vertex
        // shader flips its depths).
        const PW = pass.width, PH = pass.height;
        const [x, y, w, h] = fit.rect;
        const z0 = Math.min(1, Math.max(0, d.viewport[4] || 0)), z1 = Math.min(1, Math.max(0, d.viewport[5] || 0));
        const viewportKey = x + ',' + y + ',' + w + ',' + h + ',' + z0 + ',' + z1;
        if (ps0.viewport !== viewportKey) {
          pass.setViewport(x, y, w, h, Math.min(z0, z1), Math.max(z0, z1));
          ps0.viewport = viewportKey;
        }
        // Dynamic state: scissor, stencil reference, blend constant.
        // (the scissor always: the hardware's window scissor, which the XDK
        // narrows to the D3D one when that is on, as the native renderer)
        const sl = Math.max(0, Math.min(PW, p[7] | 0)), st = Math.max(0, Math.min(PH, p[8] | 0));
        const sr = Math.max(sl, Math.min(PW, p[9] | 0)), sb = Math.max(st, Math.min(PH, p[10] | 0));
        if (sr <= sl || sb <= st) continue;  // nothing would be drawn
        const scissorKey = sl + ',' + st + ',' + sr + ',' + sb;
        if (ps0.scissor !== scissorKey) { pass.setScissorRect(sl, st, sr - sl, sb - st); ps0.scissor = scissorKey; }
        const stencil = stencilWord(d) & 255;
        if (ps0.stencil !== stencil) { pass.setStencilReference(stencil); ps0.stencil = stencil; }
        // (blend factors 14 and 15, constant alpha, read the constant's alpha
        // in WebGPU's 'constant': when a draw blends with those and not the
        // constant colour, its RGB is the alpha)
        let alphaFactor = false, colorFactor = false;
        for (let i = 0; i < 4; i++) {
          const b = d.blends[i];
          if (!(b & 0x80000000)) continue;
          for (let k = 0; k < 32; k += 8) {
            const f = (b >>> k) & 0x1F;
            if (f === 14 || f === 15) alphaFactor = true;
            if (f === 12 || f === 13) colorFactor = true;
          }
        }
        const constantAlpha = alphaFactor && !colorFactor;
        const blendKey = (constantAlpha ? 'a' : 'c') + p[13] + ',' + p[14] + ',' + p[15] + ',' + p[16];
        if (ps0.blend !== blendKey) {
          blendWords[0] = p[13]; blendWords[1] = p[14]; blendWords[2] = p[15]; blendWords[3] = p[16];
          const bc = blendFloats;
          pass.setBlendConstant(constantAlpha ? { r: bc[3], g: bc[3], b: bc[3], a: bc[3] } : { r: bc[0], g: bc[1], b: bc[2], a: bc[3] });
          ps0.blend = blendKey;
        }
        if (d.indexed) {
          const ib = gpuBuffer(heap, d.indices, GPUBufferUsage.INDEX), ibFormat = d.index32 ? 'uint32' : 'uint16';
          const ibOff = d.indices.arena || 0;
          if (ps0.ib !== ib || ps0.ibOff !== ibOff || ps0.ibFormat !== ibFormat) {
            pass.setIndexBuffer(ib, ibFormat, ibOff);
            ps0.ib = ib; ps0.ibOff = ibOff; ps0.ibFormat = ibFormat;
          }
          pass.drawIndexed(d.count, 1, d.first, d.baseVertex);
        } else {
          pass.draw(d.count, 1, d.first);
        }
        stats.drawn++;
        if (window.s3dProf) {
          const now = performance.now(), prof = window.s3dProf;
          prof.pipe = (prof.pipe || 0) + profB - profA;
          prof.binds = (prof.binds || 0) + profC - profB;
          prof.rest = (prof.rest || 0) + now - profC;
        }
      }
      endPass();
      // (window.s3dProf, debugging: ms before the draw loop, in it, and after)
      if (window.s3dProf) {
        const now = performance.now(), prof = window.s3dProf;
        prof.frames = (prof.frames || 0) + 1;
        prof.setup = (prof.setup || 0) + profLoop - profStart;
        prof.read = (prof.read || 0) + profRead - profStart;
        prof.copy = (prof.copy || 0) + profCopy - profRead;
        prof.upload = (prof.upload || 0) + profUpload - profCopy;
        prof.kb = (prof.kb || 0) + constantBytes.byteLength / 1024;
        prof.loop = (prof.loop || 0) + now - profLoop;
        this.profEnd = now;
      }
      // The front buffer to the canvas. The game rotates three; the page
      // replays only the latest frame, so the one it names may have been
      // filled by a frame the page skipped (an old picture). Then show this
      // frame's last full-size resolve instead.
      // So: the last resolve this frame into any of the game's front buffers
      // (the textures presents have named).
      if (screenBase) this.frontIds.add(screenBase);
      let front = this.lastFrontResolve ? resolved.get(this.lastFrontResolve) : null;
      stats.frontId = this.lastFrontResolve ? 'r' + this.lastFrontResolve.toString(16) : 's' + screenBase.toString(16);
      if (!front) front = resolved.get(screenBase);
      // (window.s3dShow, debugging: { resolved: id } or { surface: handle } shown
      // instead, brightened by 2^exponent; alpha: true shows its alpha)
      const show = window.s3dShow;
      const shown = show && (show.resolved ? resolved.get(show.resolved) : surfaces.get(show.surface));
      if (shown && !shown.depth) front = shown;
      // ({ texture: binding word } shows a guest texture, the draw's word as captured)
      if (show && show.texture) { const v = textureViewFn(show.texture - 1); if (v) front = { view: v, width: 1280, height: 720 }; }
      this.writtenThisFrame.clear();
      this.lastFrontResolve = 0;
      stats.front = front ? front.width + 'x' + front.height : 'none ' + screenBase.toString(16);
      // A frame some of whose draws were skipped while their shaders or
      // pipelines still compiled (a new scene's first frames) misses whole
      // passes - the exposure, the composite - and flashes white or black:
      // the last whole frame is shown again instead, for up to 2 s. So is a
      // frame with no front buffer found (a single black frame mid-shot;
      // screens drawn only in 2D replace it after this).
      const held = (stats.pending > 0 || !front || window.s3dForceHold) && this.heldFrame && (this.heldCount | 0) < 120 && !shown;
      if (held) {
        this.heldCount++;
        stats.held = (stats.held || 0) + 1;
        encoder.copyTextureToTexture({ texture: this.heldFrame }, { texture: canvasContext.getCurrentTexture() }, [1280, 720]);
      } else if (front) {
        this.heldCount = 0;
        this.lastFront = front;
        const p = encoder.beginRenderPass({ colorAttachments: [{ view: canvasContext.getCurrentTexture().createView(),
          loadOp: 'clear', storeOp: 'store', clearValue: { r: 0, g: 0, b: 0, a: 1 } }] });
        this.updateDisplayRamp();
        const ramped = this.rampValid && !shown && !window.s3dNoDisplayGamma;
        p.setPipeline(ramped ? this.presentRamped : this.present);
        if (ramped) {
          p.setBindGroup(0, device.createBindGroup({ layout: this.presentRamped.getBindGroupLayout(0), entries: [
            { binding: 0, resource: front.view }, { binding: 2, resource: { buffer: this.rampBuffer } }] }));
        } else {
          p.setBindGroup(0, this.copyBind(this.present, front.view, 0, 0, shown && show.exponent || 0, shown && show.alpha ? 1 : 0));
        }
        p.setViewport(0, 0, 1280, 720, 0, 1);
        p.draw(3);
        p.end();
        this.heldFrame ??= device.createTexture({ size: [1280, 720], format: canvasFormat,
                                                  usage: GPUTextureUsage.COPY_DST | GPUTextureUsage.COPY_SRC });
        encoder.copyTextureToTexture({ texture: canvasContext.getCurrentTexture() }, { texture: this.heldFrame }, [1280, 720]);
      }
      this.evictUnused();
      // A ring of the last 600 frames for finding flashes: draws skipped (a
      // pipeline still compiling), GPU errors so far, and the exposure
      // adaptation's inputs (12AA6B's pixel constants c208-c211 x).
      const frameLog = window.s3dFrameLog ??= [];
      frameLog.push({ serial: frameSerial, at: performance.now() | 0, drawn: stats.drawn, skipped: stats.skippedPipeline,
                      skippedShader: stats.skippedShader, gpuErrors: stats.gpuErrors | 0, adapt: stats.adaptation || null,
                      created: stats.created || null, front: stats.front, frontId: stats.frontId, pending: stats.pending, held: held });
      if (frameLog.length > 4000) frameLog.shift();
      stats.adaptation = null;
      stats.created = null;
      stats.transient.push(...stats.retired);
    },

    // Once a second: buffers, surfaces and resolved textures no frame has used
    // for 5 s destroyed (after this frame's work, through stats.retired), and
    // the counts in stats.live.
    evictUnused() {
      frameSerial++;
      if (frameSerial % 60) return;
      const old = frameSerial - 300;
      for (const [id, b] of buffers) if (!(b.used > old)) { stats.retired.push(b.buffer); buffers.delete(id); }
      for (const [h, t] of surfaces) if (!(t.used > old)) { t.texture.label += ' evicted@' + frameSerial; stats.retired.push(t.texture); surfaces.delete(h); }
      for (const [id, t] of resolved) {
        if (t === this.lastFront || this.frontIds.has(id) || t.used > old) continue;
        stats.retired.push(t.texture);
        if (t.stencil) stats.retired.push(t.stencil.texture);
        resolved.delete(id);
      }
      for (const [key, a] of packedAliases) if (!(a.used > old)) { stats.retired.push(a.texture); packedAliases.delete(key); }
      let bytes = 0;
      for (const t of surfaces.values()) bytes += t.width * t.height * 8;
      for (const t of resolved.values()) bytes += t.width * t.height * 8;
      for (const b of buffers.values()) bytes += b.size;
      stats.live = { buffers: buffers.size, surfaces: surfaces.size, resolved: resolved.size, mb: Math.round(bytes / 1048576) };
    },

    canvasTexture: () => canvasContext.getCurrentTexture(),

    // What the canvas shows this frame, saved as uploads/<name> (PNG): ask with
    // Scene3D.shotName = name; the page calls captureCanvas before submitting.
    captureCanvas(encoder) {
      const name = this.shotName;
      if (!name) return null;
      this.shotName = null;
      const readback = device.createBuffer({ size: 1280 * 720 * 4, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
      encoder.copyTextureToBuffer({ texture: canvasContext.getCurrentTexture() }, { buffer: readback, bytesPerRow: 5120 }, [1280, 720]);
      return async () => {
        await readback.mapAsync(GPUMapMode.READ);
        const px = new Uint8ClampedArray(readback.getMappedRange().slice(0));
        readback.destroy();
        if (canvasFormat === 'bgra8unorm') for (let i = 0; i < px.length; i += 4) { const b = px[i]; px[i] = px[i + 2]; px[i + 2] = b; }
        const canvas = new OffscreenCanvas(1280, 720);
        canvas.getContext('2d').putImageData(new ImageData(px, 1280, 720), 0, 0);
        await fetch('/upload/' + name, { method: 'POST', body: await canvas.convertToBlob({ type: 'image/jpeg', quality: 0.8 }) });
        (window.canvasShots ??= []).push(name);
      };
    },

    // A packed-depth alias (web_frame_capture TextureFor3D): the depth
    // resolve's depth and stencil as D24S8 bytes through the fetch swizzle
    // (bits 18-29), made again when the resolve is (closing the open pass).
    packedAlias(t, encoder, endPass) {
      const source = useResolved(t & 0x3FFFF);
      if (!source || source.format !== 'r32float') return;
      let alias = packedAliases.get(t >>> 0);
      if (alias) alias.used = frameSerial;
      if (alias && alias.serial === source.serial && alias.width === source.width && alias.height === source.height) return;
      endPass();
      if (!alias || alias.width !== source.width || alias.height !== source.height) {
        if (alias) stats.retired.push(alias.texture);
        const texture = device.createTexture({ size: [source.width, source.height], format: 'rgba8unorm',
          usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING });
        alias = { texture, view: texture.createView(), width: source.width, height: source.height };
        packedAliases.set(t >>> 0, alias);
      }
      alias.serial = source.serial;
      alias.used = frameSerial;
      const uniform = device.createBuffer({ size: 16, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
      device.queue.writeBuffer(uniform, 0, new Uint32Array([0, 0, (t >>> 18) & 0xFFF, source.float24 ? 1 : 0]));
      stats.transient.push(uniform);
      const stencil = source.stencil ? source.stencil.view : dummyTexture('texture_2d');
      const pass = encoder.beginRenderPass({ colorAttachments: [{ view: alias.view, loadOp: 'clear', storeOp: 'store',
        clearValue: { r: 0, g: 0, b: 0, a: 0 } }] });
      pass.setPipeline(this.packAlias);
      pass.setBindGroup(0, device.createBindGroup({ layout: this.packAlias.getBindGroupLayout(0), entries: [
        { binding: 0, resource: source.view }, { binding: 1, resource: stencil }, { binding: 2, resource: { buffer: uniform } }] }));
      pass.draw(3);
      pass.end();
      stats.packedAliases = (stats.packedAliases || 0) + 1;
    },

    // A picture the page drew itself (the 2D compositor's: splash, legal
    // and loading screens) to the canvas, through the display ramp too.
    presentTexture(encoder, texture) {
      this.updateDisplayRamp();
      if (!this.rampValid || window.s3dNoDisplayGamma) {
        encoder.copyTextureToTexture({ texture }, { texture: canvasContext.getCurrentTexture() }, [1280, 720]);
        return;
      }
      const p = encoder.beginRenderPass({ colorAttachments: [{ view: canvasContext.getCurrentTexture().createView(),
        loadOp: 'clear', storeOp: 'store', clearValue: { r: 0, g: 0, b: 0, a: 1 } }] });
      p.setPipeline(this.presentRamped);
      p.setBindGroup(0, device.createBindGroup({ layout: this.presentRamped.getBindGroupLayout(0), entries: [
        { binding: 0, resource: texture.createView() }, { binding: 2, resource: { buffer: this.rampBuffer } }] }));
      p.setViewport(0, 0, 1280, 720, 0, 1);
      p.draw(3);
      p.end();
    },

    // (the display ramp read from the game every 2 s: kept only when it is
    // one - rising, from 0 to 1023)
    updateDisplayRamp() {
      if (frameSerial - this.rampCheckedFrame < 120 || typeof Module === 'undefined' || !Module._rex_web_peek32) return;
      this.rampCheckedFrame = frameSerial;
      const values = new Float32Array(1024);
      for (let i = 0; i < 1024; i += 2) {
        const w = Module._rex_web_peek32(0x831CF6E8 + i * 2) >>> 0;
        values[i] = w >>> 16; values[i + 1] = w & 0xFFFF;
      }
      let rising = values[0] <= 8 && values[1023] === 1023;
      for (let i = 1; i < 1024 && rising; i++) rising = values[i] >= values[i - 1];
      this.rampValid = rising;
      if (rising) device.queue.writeBuffer(this.rampBuffer, 0, values);
    },

    copyBind(pipeline, view, dx, dy, exponent = 0, clampTo01 = 0, sx = 1, sy = 1, sampleSelect = 6, cleanup = 0) {
      const uniform = device.createBuffer({ size: 32, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
      device.queue.writeBuffer(uniform, 0, new Int32Array([dx, dy, exponent, clampTo01, sx, sy, sampleSelect, cleanup]));
      stats.transient.push(uniform);
      return device.createBindGroup({ layout: pipeline.getBindGroupLayout(0),
        entries: [{ binding: 0, resource: view }, { binding: 1, resource: { buffer: uniform } }] });
    },

    // (the resolve flags' sample select, bits 4-6 encoded + 1, as the native
    // DecodeResolveSampleSelect: 0-3 one sample, 4 samples 0-1, 5 samples 2-3,
    // 6 all four)
    // A resolve: the source surface's rectangle into the texture at the
    // destination base (made at its size), then any source clears.
    resolve(e, encoder, flushClear, floats) {
      const p = e.params, depth = (p[0] & 7) === 4;
      const sourceHandle = p[1], sourceSize = p[2];
      flushClear(sourceHandle, sourceSize, depth);
      let source = surfaces.get(sourceHandle);
      // A surface that wrote over the source through the same EDRAM (params
      // 19, and 18 bits 1-2: twice as wide / tall): read from it instead.
      let sx = 1, sy = 1;
      // (a depth resolve may read a colour surface: its red channel is the depth)
      const alias = p[19] ? surfaces.get(p[19]) : null;
      let fromColor = false;
      if (alias && (!!alias.depth === depth || (depth && !alias.depth))) {
        fromColor = depth && !alias.depth;
        flushClear(p[19], alias.width | alias.height << 16, !!alias.depth);
        source = alias;
        sx = ((p[18] >> 1) & 1) + 1;
        sy = ((p[18] >> 2) & 1) + 1;
        stats.aliasResolves = (stats.aliasResolves || 0) + 1;
      }
      const base = p[7], width = p[8] & 0xFFFF, height = p[8] >>> 16;
      const [left, top, right, bottom] = [p[3], p[4], p[5], p[6]].map(v => v | 0);
      if (source && width && height && width <= 8192 && height <= 8192 && (source.depth === depth || fromColor)) {
        // (depth resolves at full precision: with reversed Z the sky and the
        // distance sit near 0, where 16-bit floats band the depth into steps;
        // 32-bit float colour surfaces resolve into 32-bit float textures)
        // (params 18 bits 24-29: the destination's texture format - 8-bit and
        // 10-bit ones stored as such, as the native resolve does: the G-buffer
        // packs bits into its 8-bit channels, which a 16-bit float texture
        // rounds, and the lights unpacking them then drew dashes)
        const destFormat = (p[18] >>> 24) & 63;
        const format = depth && this.copyDepth32 ? 'r32float'
          : !depth && (source.format === 'r32float' || source.format === 'rg32float') ? source.format
          : !depth && destFormat === 6 ? 'rgba8unorm' : !depth && destFormat === 7 ? 'rgb10a2unorm' : COLOR;
        // (params 18: bits 8-15 the destination mip, 16-18 its cube face, bit
        // 19 a cube map, 20-23 its last mip; bit 3 depth stored as 20e4 float)
        const level = (p[18] >>> 8) & 0xFF, face = (p[18] >>> 16) & 7, cube = !!(p[18] & 0x80000);
        const mipCount = Math.max(level + 1, ((p[18] >>> 20) & 15) + 1);
        const layered = cube || level > 0 || mipCount > 1;
        const levels = layered ? Math.min(mipCount, 1 + Math.floor(Math.log2(Math.max(width, height)))) : 1;
        let dest = resolved.get(base);
        if (!dest || dest.width !== width || dest.height !== height || dest.format !== format || dest.cube !== cube ||
            dest.mipCount < levels) {
          if (dest) stats.retired.push(dest.texture);
          (stats.created ??= []).push(base.toString(16) + ':' + width + 'x' + height + ':' + format);
          const texture = device.createTexture({ size: [width, height, cube ? 6 : 1], format, mipLevelCount: levels,
            usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_SRC });
          const view = cube ? Object.assign(texture.createView({ dimension: 'cube' }), { rexCube: true }) : texture.createView();
          dest = { texture, view, width, height, format, cube, mipCount: levels };
          resolved.set(base, dest);
        }
        dest.used = frameSerial;
        dest.float24 = !!(p[18] & 8);
        dest.serial = (dest.serial || 0) + 1;
        this.writtenThisFrame.add(base);
        if (this.frontIds.has(base) || p[17]) this.lastFrontResolve = base;
        const mw = Math.max(1, width >> level), mh = Math.max(1, height >> level);
        const dx = p[9] | 0, dy = p[10] | 0;
        const w = Math.min(right - left, mw - dx), h = Math.min(bottom - top, mh - dy);
        if (w > 0 && h > 0 && level < dest.mipCount) {
          const pipeline = dest.format === 'r32float' && depth ? (fromColor ? this.copyColor32 : this.copyDepth32)
            : depth && !fromColor ? this.copyDepth : this.copyColorFor(dest.format);
          const target = dest.cube || dest.mipCount > 1 ? dest.texture.createView({ dimension: '2d', baseMipLevel: level, mipLevelCount: 1,
                                                             baseArrayLayer: cube ? face : 0, arrayLayerCount: 1 }) : dest.view;
          const pass = encoder.beginRenderPass({ colorAttachments: [{ view: target, loadOp: 'load', storeOp: 'store' }] });
          pass.setPipeline(pipeline);
          // (the exponent: signed six bits at 26 of the flags; params[18] bit 0:
          // the destination is a normalized format; for depth, pad.y: float depth)
          const exponent = depth ? (window.s3dDepthMode | 0) : (((p[0] >>> 26) & 31) - ((p[0] >>> 26) & 32));
          pass.setBindGroup(0, this.copyBind(pipeline, depth && !fromColor ? source.sampleView : source.view, left - dx, top - dy,
                                             exponent, depth ? (dest.float24 ? 1 : 0) : p[18] & 1, sx, sy,
                                             sampleSelect(p[0]), !depth && dest.format === COLOR ? 1 : 0));
          pass.setViewport(dx, dy, w, h, 0, 1);
          pass.draw(3);
          pass.end();
          // (a depth resolve's stencil, kept for packed-depth aliases of it)
          // (a depth resolve of a colour surface - GTA IV's scene depth - takes
          // the stencil of the depth surface bound with it)
          const stencilSource = fromColor ? (e.depth ? surfaces.get(e.depth) : null) : source;
          if (depth && stencilSource && stencilSource.stencilView && !layered &&
              stencilSource.width === source.width && stencilSource.height === source.height) {
            if (!dest.stencil || dest.stencil.width !== width || dest.stencil.height !== height) {
              if (dest.stencil) stats.retired.push(dest.stencil.texture);
              const texture = device.createTexture({ size: [width, height], format: 'r32float',
                usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING });
              dest.stencil = { texture, view: texture.createView(), width, height };
            }
            const sp = encoder.beginRenderPass({ colorAttachments: [{ view: dest.stencil.view, loadOp: 'load', storeOp: 'store' }] });
            sp.setPipeline(this.copyStencil);
            sp.setBindGroup(0, this.copyBind(this.copyStencil, stencilSource.stencilView, left - dx, top - dy));
            sp.setViewport(dx, dy, w, h, 0, 1);
            sp.draw(3);
            sp.end();
          }
        }
      } else {
        stats.badResolves = (stats.badResolves || 0) + 1;
      }
      // Clears the resolve asked for on its source.
      if (p[0] & 0x100) {
        const [r, g, b, a] = floats(e, 11);
        const index = p[0] & 7;
        const handle = index < 4 ? e.colors[index] || sourceHandle : sourceHandle;
        pendingClears.set(handle, { color: { r, g, b, a } });
      }
      if ((p[0] & 0x200) && e.depth) {
        pendingClears.set(e.depth, { depth: Math.min(1, Math.max(0, e.paramFloat(15))), stencil: p[16] & 255 });
      }
    },

    // A depth handoff (web_frame_capture.h kWebDepthHandoff): the source depth
    // surface's depth into the destination's, as the native renderer's
    // RecordDepthSurfaceHandoff does. Policy 1 (rebuild scene coverage) also sets
    // the destination stencil to 0x80 where the scene depth is empty (0), else
    // 0xFF (forward_stencil.h); policy 0 keeps the destination's stencil.
    depthHandoff(e, encoder, flushClear) {
      const p = e.params;
      if (!p[3]) return;
      // (the surfaces as the draws made them: asking with the command's sizes
      // could make them again, losing their contents)
      // (from the resolved scene depth texture, params[6], depth in red, as the
      // native renderer copies from the handoff's source texture)
      const source = p[6] ? resolved.get(p[6]) : null, dest = surfaces.get(p[3]);
      if (!source || !dest || !dest.depth) { stats.handoffMisses = (stats.handoffMisses || 0) + 1; return; }
      flushClear(p[3], dest.width | dest.height << 16, true);
      if (!this.handoffPipelines) {
        // (empty = the depth stores as 0 in the guest's own encoding, mode.w:
        // 20e4 float, as the native scene_depth_handoff_ps)
        const module = device.createShaderModule({ code: this.packDepthCode + `
          @group(0) @binding(0) var source: texture_2d<f32>;
          @group(0) @binding(1) var<uniform> mode: vec4u;
          @vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
            let p = array(vec2f(-1, -1), vec2f(3, -1), vec2f(-1, 3));
            return vec4f(p[i], 0, 1);
          }
          @fragment fn fs(@builtin(position) p: vec4f) -> @builtin(frag_depth) f32 {
            let size = vec2f(textureDimensions(source));
            let at = vec2i(p.xy * size / vec2f(f32(mode.y), f32(mode.z)));
            let d = textureLoad(source, clamp(at, vec2i(0), vec2i(size) - 1), 0).x;
            let empty = packDepth(d, mode.w != 0u) == 0u;
            if (mode.x == 1u && !empty) { discard; }
            if (mode.x == 2u && empty) { discard; }
            return select(d, 0.0, empty);
          }` });
        const make = passOp => device.createRenderPipeline({ layout: 'auto', vertex: { module, entryPoint: 'vs' },
          fragment: { module, entryPoint: 'fs', targets: [] },
          depthStencil: { format: DEPTH, depthWriteEnabled: true, depthCompare: 'always',
            stencilFront: { compare: 'always', passOp }, stencilBack: { compare: 'always', passOp },
            stencilWriteMask: 0xFF } });
        this.handoffPipelines = { keep: make('keep'), replace: make('replace') };
      }
      const pass = encoder.beginRenderPass({ colorAttachments: [], depthStencilAttachment: { view: dest.view,
        depthLoadOp: 'load', depthStoreOp: 'store', stencilLoadOp: 'load', stencilStoreOp: 'store' } });
      const run = (pipeline, mode, reference) => {
        const uniform = device.createBuffer({ size: 16, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
        device.queue.writeBuffer(uniform, 0, new Uint32Array([mode, dest.width, dest.height, source.float24 ? 1 : 0]));
        stats.transient.push(uniform);
        pass.setPipeline(pipeline);
        pass.setBindGroup(0, device.createBindGroup({ layout: pipeline.getBindGroupLayout(0),
          entries: [{ binding: 0, resource: source.view }, { binding: 1, resource: { buffer: uniform } }] }));
        pass.setStencilReference(reference);
        pass.draw(3);
      };
      // (window.s3dHandoffMode, debugging: 1 keeps the stencil whatever the
      // policy, 2 skips handoffs)
      if (window.s3dHandoffMode === 2) { pass.end(); return; }
      if (p[5] === 1 && window.s3dHandoffMode !== 1) {
        run(this.handoffPipelines.replace, 1, 0x80);
        run(this.handoffPipelines.replace, 2, 0xFF);
      } else {
        run(this.handoffPipelines.keep, 0, 0);
      }
      pass.end();
      stats.handoffs = (stats.handoffs || 0) + 1;
    },

    // Saves the front buffer as a PNG on the test server (uploads/<name>).
    // Debugging: a compact description of the latest frame's events (for the
    // boot screens): kind, surfaces, shaders and what each texture stage holds.
    describeFrame(maxEvents = 40, from = 0) {
      const m = new DataView(wasmMemory.buffer);
      const frame = Number(Module._rex_web_frame());
      const n = m.getUint32(frame + 48, true), table = Number(m.getBigUint64(frame + 56, true));
      const textures = Number(m.getBigUint64(frame + 24, true));
      const out = ['events ' + n + ' front ' + m.getUint32(frame + 72, true) + ' quads2d ' + m.getUint32(frame + 8, true)];
      if (from < 0) from = Math.max(0, n + from);
      for (let i = from; i < Math.min(n, from + maxEvents); i++) {
        const e = readDraw(m, table + i * DRAW_SIZE);
        const size = x => (x & 0xFFFF) + 'x' + (x >>> 16);
        if (e.kind === 1) { out.push(i + ' clear f' + e.params[0] + ' c0 ' + e.colors[0].toString(16) + ' depth ' + e.depth.toString(16) + ' z ' + e.paramFloat(5) + ' s ' + e.params[6]); continue; }
        if (e.kind === 3) { out.push(i + ' depth handoff ' + e.params[1].toString(16) + ' -> ' + e.params[3].toString(16) + ' policy ' + e.params[5]); continue; }
        if (e.kind === 2) { out.push(i + ' resolve src ' + e.params[1].toString(16) + ' ' + size(e.params[2]) + ' -> id ' + e.params[7] + ((e.params[0] & 7) === 4 ? ' DEPTH' : '') + (e.params[19] ? ' alias ' + e.params[19].toString(16) : '') + (e.params[17] ? ' FRONT' : '')); continue; }
        const tex = [];
        e.textures.forEach((t, s) => {
          if (!t) return;
          if (t & 0x80000000) { tex.push(s + ((t & 0x40000000) ? ':packed' : ':target') + (t & 0x3FFFF)); return; }
          const at = textures + (t - 1) * 24;
          tex.push(s + ':' + m.getUint32(at, true) + 'x' + m.getUint32(at + 4, true) + 'f' + m.getUint32(at + 20, true));
        });
        const ready = (shaders.get(e.vs)?.state || 'new') + '/' + (e.psHash ? shaders.get(e.ps)?.state || 'new' : '-');
        const st = ' z' + e.depthEnable + '/' + e.depthFunction + '/' + e.depthWrite + ' st ' + e.params[0] + ':' + e.params[1].toString(16) + ':' + e.params[3].toString(16) + ' ds ' + e.depth.toString(16);
        out.push(i + ' draw ' + e.vs + '/' + e.ps + ' [' + ready + '] topo' + e.topology + ' n' + e.count + st +
                 ' rt ' + e.colors[0].toString(16) + ' ' + size(e.sizes[0]) + ' tex ' + tex.join(' '));
      }
      return out.join('\n');
    },

    // Debugging: pixel (or vertex, vertex = true) constant registers [from, from + count) of the latest
    // frame's event `index`, as floats.
    constants(index, from, count = 1, vertex = false) {
      const m = new DataView(wasmMemory.buffer);
      const frame = Number(Module._rex_web_frame());
      const table = Number(m.getBigUint64(frame + 56, true));
      const e = readDraw(m, table + index * DRAW_SIZE);
      const base = vertex ? e.constants : e.psConstants;
      const out = [];
      for (let r = from; r < from + count; r++) out.push([0, 1, 2, 3].map(c => +m.getFloat32(base + r * 16 + c * 4, true).toPrecision(4)));
      return out;
    },

    // Debugging: the resolved textures (base -> size), and saving any one.
    resolvedList() { return [...resolved].map(([b, t]) => b.toString(16) + ':' + t.width + 'x' + t.height); },
    // The first `count` texels of a resolved texture, as floats.
    async texels(base, count = 4, row = 0) {
      const t = resolved.get(base);
      if (!t) return null;
      const readback = device.createBuffer({ size: 256, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
      const encoder = device.createCommandEncoder();
      const n = Math.min(count, t.width, t.format === 'r32float' ? 64 : 32);
      encoder.copyTextureToBuffer({ texture: t.texture, origin: [0, Math.min(row, t.height - 1)] },
                                  { buffer: readback, bytesPerRow: 256 }, [n, 1]);
      device.queue.submit([encoder.finish()]);
      await readback.mapAsync(GPUMapMode.READ);
      if (t.format === 'r32float') {
        const f = new Float32Array(readback.getMappedRange().slice(0, n * 4));
        readback.destroy();
        return [...f].map(v => +v.toPrecision(4));
      }
      const half = new Uint16Array(readback.getMappedRange().slice(0, n * 8));
      readback.destroy();
      const toFloat = h => { const e = (h >> 10) & 31, f = h & 1023, s = h >> 15 ? -1 : 1;
        return e === 0 ? s * f * 2 ** -24 : e === 31 ? (f ? NaN : s * Infinity) : s * (1 + f / 1024) * 2 ** (e - 15); };
      return [...half].map(toFloat).map(v => +v.toPrecision(4));
    },
    async snapshotBase(base, name) { return this.snapshot(name, resolved.get(base)); },
    async snapshot(name, entry) {
      const front = entry || this.lastFront;
      if (!front) return 'no front buffer';
      const W2 = front.width, H2 = front.height, row = Math.ceil(W2 * 8 / 256) * 256;
      const readback = device.createBuffer({ size: row * H2, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
      const encoder = device.createCommandEncoder();
      encoder.copyTextureToBuffer({ texture: front.texture }, { buffer: readback, bytesPerRow: row }, [W2, H2]);
      device.queue.submit([encoder.finish()]);
      await readback.mapAsync(GPUMapMode.READ);
      const half = new Uint16Array(readback.getMappedRange().slice(0));
      readback.destroy();
      const toFloat = h => { const e = (h >> 10) & 31, f = h & 1023, s = h >> 15 ? -1 : 1;
        return e === 0 ? s * f * 2 ** -24 : e === 31 ? 0 : s * (1 + f / 1024) * 2 ** (e - 15); };
      const rgba = new Uint8ClampedArray(W2 * H2 * 4);
      for (let y = 0; y < H2; y++) for (let x = 0; x < W2; x++) for (let c = 0; c < 4; c++)
        rgba[(y * W2 + x) * 4 + c] = c === 3 ? 255 : toFloat(half[y * row / 2 + x * 4 + c]) * 255 * (window.snapScale || 1);
      const canvas = new OffscreenCanvas(W2, H2);
      canvas.getContext('2d').putImageData(new ImageData(rgba, W2, H2), 0, 0);
      await fetch('/upload/' + name, { method: 'POST', body: await canvas.convertToBlob({ type: 'image/png' }) });
      return name;
    },
  };
})();
