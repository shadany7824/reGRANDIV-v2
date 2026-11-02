/**
 * @file        graphics/web_frame_capture.h
 * @brief       [web port] 2D frame capture for the browser canvas
 *
 * A stand-in until the WebGPU renderer draws GTA IV's whole command stream:
 * each frame records the game's immediate-mode quads (DrawPrimitiveUp: four
 * screen-space vertices with colour and texture coordinates, which is how
 * splash screens, loading screens and much of the frontend are drawn) with
 * the texture bound to stage 0, decodes each new texture once from guest
 * memory to RGBA, and publishes the quads in a mailbox the page reads
 * (rex_web_frame) and draws with WebGPU, in order, alpha blended.
 *
 * Render targets are emulated as quad lists: the quads drawn since a clear
 * are what a resolve copies out, kept under the resolve's destination
 * texture address. A quad sampling such an address uses that target's image
 * (the page renders it to a texture first), and present shows the target its
 * front buffer names.
 *
 * @license     BSD 3-Clause License
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace rex::memory {
class Memory;
}

namespace rex::graphics::web {

// One quad as the game drew it: a triangle strip in the guest's 1280x720
// screen space, texture multiplied by the colour.
struct WebQuad {
  float x[4], y[4], u[4], v[4];
  uint32_t color;    // 0xAARRGGBB, as the game wrote it
  uint32_t texture;  // texture table index + 1; 0 = untextured; with the top
                     // bit set, the low 20 bits are a target's base address
  uint32_t output;   // bits 0-3: colour write mask (RGBA); bit 31: blending on
  uint32_t blend;    // Xenos RB_BLENDCONTROL: factors and operations
};

constexpr uint32_t kTargetTextureFlag = 0x80000000u;

// A resolved render target: its clear colour and quads, at a texture address.
struct WebTarget {
  uint32_t base;        // fetch base address (>> 12) of the resolve destination
  uint32_t width;
  uint32_t height;
  uint32_t clear_rgba;  // 0xRRGGBBAA
  uint32_t quad_count;
  uint32_t serial;      // changes when the target is resolved again
  uint64_t quads;       // WebQuad[quad_count]
};

// A decoded texture. `serial` changes when the slot is reused.
struct WebTexture {
  uint32_t width;
  uint32_t height;
  uint64_t pixels;  // RGBA8 rows, width * 4 bytes each
  uint32_t serial;
  uint32_t format;
};

// A vertex stream or index buffer as the page uploads it: little-endian bytes
// (vertex words and 16-bit components swapped as the declaration says). `id`
// names the buffer for the page's cache; `serial` changes with the contents.
struct WebBuffer {
  uint64_t data;
  uint32_t id;
  uint32_t serial;
  uint32_t size;
  uint32_t offset;  // vertex streams: the stream's start in the buffer
  uint32_t stride;  // vertex streams: bytes per vertex
  uint32_t pad;
};

constexpr uint32_t kWebDrawStreams = 4;
constexpr uint32_t kWebDrawTextures = 26;

// One 3D draw (DrawPrimitive / DrawIndexedPrimitive) with what its shaders
// read. The shaders are the recompiled cache's, by hash (shaders/<hash>.wgsl).
struct WebDraw {
  uint64_t vs_hash;
  uint64_t ps_hash;
  uint64_t constants;  // 4096 bytes of vertex constants (see ps_constants, shared_constants)
  WebBuffer indices;   // data 0: not indexed
  uint32_t index32;
  uint32_t topology;   // 0 points, 1 lines, 2 line strip, 3 triangles, 4 strip
  uint32_t count;      // indices (or vertices when not indexed)
  uint32_t first;
  int32_t base_vertex;
  uint32_t decl;       // index into WebFrame::decls
  uint32_t stage;      // gta4_native::RenderExecutionStage
  uint32_t blend_control;
  uint32_t color_write_mask;
  uint32_t depth_enable;
  uint32_t depth_function;
  uint32_t depth_write;
  uint32_t cull_mode;
  float viewport[6];   // x, y, width, height, min depth, max depth
  uint32_t target_width;
  uint32_t target_height;
  uint32_t textures[kWebDrawTextures];  // texture table index + 1 per stage
  uint32_t pad;
  WebBuffer streams[kWebDrawStreams];
  // The frame is a list of these in command order: draws, and also clears
  // and resolves (`kind`), with the surfaces bound when each happened.
  uint32_t kind;             // WebEventKind
  uint32_t colors[4];        // bound colour surfaces (guest handles), 0 = none
  uint32_t depth;            // bound depth/stencil surface
  uint32_t sizes[5];         // colours then depth: width | height << 16
  uint32_t blend_controls[4];
  // kDraw: 0 stencil enable (bit 0) and two-sided (bit 1); 1 front and 2 back
  //   (counter-clockwise) stencil function | fail << 4 | depth fail << 8 |
  //   pass << 12 (D3D order, as WebGPU's); 3 front and 4 back reference |
  //   read mask << 8 | write mask << 16; 5 cull face (0 none, 1 front,
  //   2 back) | 0x10 clockwise front | 0x100 valid; 6 scissor enable; 7-10
  //   scissor left, top, right, bottom; 11 depth bias, 12 slope bias (float
  //   bits, 0 when off); 13-16 blend constant (floats); 17 bit 0 depth clamp
  //   (no near/far clipping).
  // kClear: flags (1 colour, 2 depth, 4 stencil), colour (4 floats), depth
  //   (float), stencil.
  // kDepthHandoff: see kWebDepthHandoff.
  // kResolve: flags (bits 0-2 source: colour index or 4 = depth; 0x100 /
  //   0x200 clear colour / depth afterwards), source handle, source size
  //   (w | h << 16), rectangle left, top, right, bottom, destination id (per guest texture; draws name it in textures[] with the top bit set),
  //   destination size (w | h << 16), destination x, y, then the clear
  //   colour (4 floats), depth (float), stencil.
  uint32_t params[20];
  // The pixel constants (4096 bytes) and shared constants (1280 bytes). Draws
  // with the same bytes point at the same block.
  uint64_t ps_constants;
  uint64_t shared_constants;
  // Per texture stage the shaders use: the fetch constant's sampler state,
  // clamp x | y << 3 | z << 6 (xenos::ClampMode), mag filter << 9, min << 11,
  // mip << 13 (xenos::TextureFilter), aniso << 15, 0x80000000 = present.
  uint32_t samplers[kWebDrawTextures];
};

// kWebDepthHandoff (the title's kDepthSurfaceHandoff: the scene depth copied
// into the forward pass's depth surface): params 1 source handle, 2 its size,
// 3 destination handle, 4 its size, 5 stencil policy (forward_stencil.h), 6 the
// resolve id of the scene depth texture it copies.
enum WebEventKind : uint32_t { kWebDraw = 0, kWebClear = 1, kWebResolve = 2, kWebDepthHandoff = 3 };

// A vertex declaration: elements with the shader input location each feeds.
struct WebDeclElement {
  uint16_t stream;
  uint16_t offset;
  uint32_t type;      // Xenos vertex format (as in gta4_metal VertexFormat)
  uint32_t location;  // shader input location, UINT32_MAX = none
};
struct WebDecl {
  uint32_t count;
  WebDeclElement elements[64];
};

// What the page reads. Written by the presenting thread; `sequence` changes
// last, after everything else is in place.
struct WebFrame {
  uint32_t sequence;     // bumped on every present
  uint32_t clear_rgba;   // latest colour clear, 0xRRGGBBAA
  uint32_t quad_count;
  uint32_t texture_count;
  uint64_t quads;        // WebQuad[quad_count]: drawn since the last clear
  uint64_t textures;     // WebTexture[texture_count]
  uint32_t target_count;
  uint32_t screen_base;  // front buffer's fetch base address, 0 = none
  uint64_t targets;      // WebTarget[target_count]
  uint32_t draw_count;
  uint32_t decl_count;
  uint64_t draws;        // WebDraw[draw_count], this frame's 3D draws
  uint64_t decls;        // WebDecl[decl_count]
  uint32_t front_target; // resolve id (kResolve params[7]) of the front buffer
  uint32_t consumed;     // the sequence the page has finished reading (it writes this)
  uint32_t reading;      // the sequence whose header the page has read and is drawing (it writes this)
  uint32_t pad;
};

void SetMemory(memory::Memory* memory);
// Called for each native title command the game submits.
void OnTitleCommand(uint32_t type, const void* command, size_t command_size);
// Called on present, after the frame's commands.
void OnPresent(const void* present_command, size_t command_size);
// The pause menu's Controls picture the keyboard set shows: 0 on foot, 1 in
// a vehicle (as the page's labels; set by the title every frame).
void SetControlsPicture(int picture);
// A menu open (the pause menu, the map, the phone), for the page's touch controls.
void SetMenuOpen(bool open);

}  // namespace rex::graphics::web
