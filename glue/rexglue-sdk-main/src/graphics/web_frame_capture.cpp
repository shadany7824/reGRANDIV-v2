/**
 * @file        graphics/web_frame_capture.cpp
 * @brief       [web port] First-pixels frame capture for the browser canvas
 *
 * See web_frame_capture.h. Textures are read the way the native renderer
 * reads them: the device keeps each stage's Xenos fetch constant at
 * kTextureFetchBase (big-endian words), which TextureInfo::Prepare turns into
 * format, size, tiling and address. Decoded here: 8_8_8_8 and DXT1/3/5, the
 * formats GTA IV's 2D images use.
 *
 * @license     BSD 3-Clause License
 */

#include "web_frame_capture.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <atomic>
#include <array>
#include <bit>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <emscripten/emscripten.h>
#include <xxhash.h>

#include <rex/graphics/gta4_native/lighting_semantics.h>
#include <rex/graphics/gta4_native/title_commands.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/chrono/clock.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/xmemory.h>

#include "gta4_native/core/draw_state.h"
#include "gta4_native/core/geometry.h"
#include "gta4_native/native_buffer_metadata.h"
#include "gta4_native/native_color_output.h"
#include "gta4_native/native_fixed_function_policy.h"
#include "gta4_native/native_sampler_lod_bias.h"
#include "gta4_native/native_shader_booleans.h"
#include "gta4_native/native_stencil_volume_policy.h"

namespace rex::graphics::web {
namespace {

namespace native = rex::graphics::gta4_native;

// Device layout (gta4_native/graphics_system.cpp).
constexpr uint32_t kTextureFetchBase = 0x480;
constexpr uint32_t kMaxDimension = 4096;

memory::Memory* g_memory = nullptr;
std::mutex g_mutex;

// Textures decoded so far, by fetch base address and format. A slot keeps
// its pixels for the page until the table is full and the slot is reused.
constexpr size_t kMaxTextures = 4096;
struct TextureSlot {
  uint64_t key = 0;
  uint32_t swizzle = 0;
  std::vector<uint8_t> pixels;
  uint32_t decoded_frame = 0;
  // Samples of the guest bytes it was decoded from: GTA IV streams textures
  // into recycled memory, so the same address and size can hold a new image.
  uint32_t address = 0, size = 0, checked_frame = 0;
  uint64_t content = 0;
  uint64_t whole = 0;       // (small textures) a hash of all its base bytes,
  uint32_t whole_frame = 0; // last checked in this frame
  uint32_t used_frame = 0;  // last frame a draw used it
  bool compressed = false;  // pixels are BC blocks (CopyBlocks), not RGBA
  uint32_t levels = 1;      // mips in pixels, one after another
};
std::unordered_map<uint64_t, uint32_t> g_texture_lookup;  // key ^ swizzle -> slot
// Small textures are rewritten by the game (the 32x32 fade texture holds the
// fade's opacity), so each frame that uses them hashes all their bytes and
// decodes them again when those changed (16 KiB at most: cheaper than the
// decode, and the page then uploads only the ones that did change).
constexpr uint32_t kRedecodeArea = 64 * 64;
uint32_t g_frame_number = 0;
TextureSlot g_texture_slots[kMaxTextures];
WebTexture g_textures[kMaxTextures];
uint32_t g_texture_count = 0;
uint32_t g_next_reuse = 0;
uint32_t g_texture_serial = 0;

// This frame's quads, and the two published lists (the page reads one while
// the next frame fills the other).
constexpr size_t kMaxQuads = 4096;
std::vector<WebQuad> g_pending;
// The screen as a console would show it: what was drawn since the last
// colour clear. A frame without draws (the game presents a splash image for
// seconds after drawing it once) keeps the picture instead of blanking it.
std::vector<WebQuad> g_screen;
bool g_cleared = false;
// Resolved render targets by destination base address: the latest copy of
// each, as long as the game keeps sampling it.
struct TargetSnapshot {
  uint32_t width = 0, height = 0, clear_rgba = 0, serial = 0;
  std::vector<WebQuad> quads;
};
std::map<uint32_t, TargetSnapshot> g_targets;
uint32_t g_target_serial = 0;
constexpr size_t kMaxTargets = 1024;
std::vector<WebTarget> g_published_targets[2];
std::vector<WebQuad> g_published_target_quads[2];

// This frame's command counts by type, for the per-present log.
uint32_t g_type_counts[32] = {};
std::vector<WebQuad> g_published[2];
int g_published_list = 0;
uint32_t g_clear_rgba = 0x000000FF;

WebFrame g_frame{};

uint32_t LoadBE32(const uint8_t* p) {
  uint32_t value;
  std::memcpy(&value, p, sizeof(value));
  return __builtin_bswap32(value);
}

uint8_t ToByte(float value) {
  return uint8_t(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// Undoes the fetch constant's endian swap on one block, in place.
void Unswap(uint8_t* data, size_t size, xenos::Endian endian) {
  switch (endian) {
    case xenos::Endian::k8in16:
      for (size_t i = 0; i + 1 < size; i += 2) std::swap(data[i], data[i + 1]);
      break;
    case xenos::Endian::k8in32:
      for (size_t i = 0; i + 3 < size; i += 4) {
        std::swap(data[i], data[i + 3]);
        std::swap(data[i + 1], data[i + 2]);
      }
      break;
    case xenos::Endian::k16in32:
      for (size_t i = 0; i + 3 < size; i += 4) {
        std::swap(data[i], data[i + 2]);
        std::swap(data[i + 1], data[i + 3]);
      }
      break;
    default:
      break;
  }
}

void Rgb565(uint16_t c, uint8_t out[3]) {
  out[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
  out[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
  out[2] = uint8_t((c & 31) * 255 / 31);
}

// Colour part of a DXT block (8 bytes, little-endian after unswapping) into
// 16 RGBA texels; alpha is left at 255.
void DecodeDxtColor(const uint8_t* block, uint8_t texels[16][4], bool dxt1) {
  const uint16_t c0 = uint16_t(block[0] | block[1] << 8);
  const uint16_t c1 = uint16_t(block[2] | block[3] << 8);
  uint8_t palette[4][4] = {};
  Rgb565(c0, palette[0]);
  Rgb565(c1, palette[1]);
  palette[0][3] = palette[1][3] = 255;
  for (int k = 0; k < 3; k++) {
    if (!dxt1 || c0 > c1) {
      palette[2][k] = uint8_t((2 * palette[0][k] + palette[1][k]) / 3);
      palette[3][k] = uint8_t((palette[0][k] + 2 * palette[1][k]) / 3);
    } else {
      palette[2][k] = uint8_t((palette[0][k] + palette[1][k]) / 2);
      palette[3][k] = 0;
    }
  }
  palette[2][3] = 255;
  palette[3][3] = (dxt1 && c0 <= c1) ? 0 : 255;
  const uint32_t bits = uint32_t(block[4]) | uint32_t(block[5]) << 8 | uint32_t(block[6]) << 16 |
                        uint32_t(block[7]) << 24;
  for (int i = 0; i < 16; i++) {
    std::memcpy(texels[i], palette[(bits >> (2 * i)) & 3], 4);
  }
}

// A BC4 block (DXT5's alpha block: 8 bytes) into 16 values.
void DecodeBc4(const uint8_t* block, uint8_t out[16]) {
  uint8_t palette[8] = {block[0], block[1]};
  for (int i = 2; i < 8; i++) {
    palette[i] = palette[0] > palette[1]
                     ? uint8_t(((8 - i) * palette[0] + (i - 1) * palette[1]) / 7)
                     : (i < 6 ? uint8_t(((6 - i) * palette[0] + (i - 1) * palette[1]) / 5)
                              : uint8_t(i == 6 ? 0 : 255));
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; i++) bits |= uint64_t(block[2 + i]) << (8 * i);
  for (int i = 0; i < 16; i++) out[i] = palette[(bits >> (3 * i)) & 7];
}

// Bits [shift, shift + width) of `v` scaled to 0-255.
uint8_t Field(uint32_t v, int shift, int width) {
  const uint32_t max = (1u << width) - 1;
  return uint8_t(((v >> shift) & max) * 255 / max);
}

// One block of the texture at (bx, by) into the RGBA image.
void DecodeBlock(xenos::TextureFormat format, const uint8_t* block, uint32_t bx, uint32_t by,
                 uint32_t width, uint32_t height, uint8_t* image) {
  uint8_t texels[16][4];
  uint32_t size = 4;
  switch (format) {
    case xenos::TextureFormat::k_8_8_8_8: {
      // Components X, Y, Z, W are the word's bytes from the lowest; the fetch
      // swizzle maps them to RGBA (D3D's A8R8G8B8 uses ZYXW).
      size = 1;
      std::memcpy(texels[0], block, 4);
      break;
    }
    case xenos::TextureFormat::k_DXT1:
      DecodeDxtColor(block, texels, true);
      break;
    case xenos::TextureFormat::k_DXT2_3:
      DecodeDxtColor(block + 8, texels, false);
      for (int i = 0; i < 16; i++) {
        const uint8_t nibble = (block[i / 2] >> ((i & 1) * 4)) & 0xF;
        texels[i][3] = uint8_t(nibble * 17);
      }
      break;
    case xenos::TextureFormat::k_DXT4_5: {
      DecodeDxtColor(block + 8, texels, false);
      uint8_t alpha[16];
      DecodeBc4(block, alpha);
      for (int i = 0; i < 16; i++) texels[i][3] = alpha[i];
      break;
    }
    // Uncompressed formats: components X, Y, Z, W from the lowest bits up;
    // the fetch swizzle maps them. Missing components read 0 (W: 1).
    case xenos::TextureFormat::k_8_8_8_8_A:
      size = 1;
      std::memcpy(texels[0], block, 4);
      break;
    // (single-channel formats replicated, as the native renderer's host
    // swizzle does: k_8 RRRR, DXN and CTX1 RGGG - GetNativeHostFormatSwizzle)
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_8_A:
    case xenos::TextureFormat::k_8_B:
      size = 1;
      texels[0][0] = texels[0][1] = texels[0][2] = texels[0][3] = block[0];
      break;
    case xenos::TextureFormat::k_8_8:
      size = 1;
      texels[0][0] = block[0]; texels[0][1] = block[1]; texels[0][2] = 0; texels[0][3] = 255;
      break;
    case xenos::TextureFormat::k_5_6_5: {
      size = 1;
      const uint32_t v = uint32_t(block[0] | block[1] << 8);
      texels[0][0] = Field(v, 0, 5); texels[0][1] = Field(v, 5, 6); texels[0][2] = Field(v, 11, 5); texels[0][3] = 255;
      break;
    }
    case xenos::TextureFormat::k_1_5_5_5: {
      size = 1;
      const uint32_t v = uint32_t(block[0] | block[1] << 8);
      texels[0][0] = Field(v, 0, 5); texels[0][1] = Field(v, 5, 5); texels[0][2] = Field(v, 10, 5); texels[0][3] = Field(v, 15, 1);
      break;
    }
    case xenos::TextureFormat::k_4_4_4_4: {
      size = 1;
      const uint32_t v = uint32_t(block[0] | block[1] << 8);
      for (int c = 0; c < 4; c++) texels[0][c] = Field(v, 4 * c, 4);
      break;
    }
    case xenos::TextureFormat::k_2_10_10_10: {
      size = 1;
      uint32_t v;
      std::memcpy(&v, block, 4);
      texels[0][0] = Field(v, 0, 10); texels[0][1] = Field(v, 10, 10); texels[0][2] = Field(v, 20, 10); texels[0][3] = Field(v, 30, 2);
      break;
    }
    case xenos::TextureFormat::k_DXN: {
      // Two BC4 blocks: X, then Y.
      uint8_t x[16], y[16];
      DecodeBc4(block, x);
      DecodeBc4(block + 8, y);
      for (int i = 0; i < 16; i++) { texels[i][0] = x[i]; texels[i][1] = texels[i][2] = texels[i][3] = y[i]; }
      break;
    }
    case xenos::TextureFormat::k_CTX1: {
      // Two 8:8 endpoints (X, Y) and 2-bit indices: endpoint 0, 1, then the
      // two thirds between them.
      const uint8_t e[4][2] = {{block[0], block[1]}, {block[2], block[3]},
                               {uint8_t((2 * block[0] + block[2]) / 3), uint8_t((2 * block[1] + block[3]) / 3)},
                               {uint8_t((block[0] + 2 * block[2]) / 3), uint8_t((block[1] + 2 * block[3]) / 3)}};
      const uint32_t bits = uint32_t(block[4]) | uint32_t(block[5]) << 8 | uint32_t(block[6]) << 16 |
                            uint32_t(block[7]) << 24;
      for (int i = 0; i < 16; i++) {
        const uint8_t* c = e[(bits >> (2 * i)) & 3];
        texels[i][0] = c[0]; texels[i][1] = texels[i][2] = texels[i][3] = c[1];
      }
      break;
    }
    case xenos::TextureFormat::k_DXT5A: {
      uint8_t x[16];
      DecodeBc4(block, x);
      for (int i = 0; i < 16; i++) { texels[i][0] = texels[i][1] = texels[i][2] = texels[i][3] = x[i]; }
      break;
    }
    case xenos::TextureFormat::k_DXT3A:
      for (int i = 0; i < 16; i++) {
        const uint8_t v = uint8_t(((block[i / 2] >> ((i & 1) * 4)) & 0xF) * 17);
        texels[i][0] = texels[i][1] = texels[i][2] = texels[i][3] = v;
      }
      break;
    default:
      return;
  }
  for (uint32_t ty = 0; ty < size; ty++) {
    for (uint32_t tx = 0; tx < size; tx++) {
      const uint32_t x = bx * size + tx, y = by * size + ty;
      if (x >= width || y >= height) continue;
      std::memcpy(image + (size_t(y) * width + x) * 4, texels[ty * size + tx], 4);
    }
  }
}

// Switches for A/B tests from the page (rex_web_set_option).
int g_option_formats = 1;    // 1: decode the formats beyond 8888 / DXT1-5
int g_option_float_targets = 0;  // 1: decode large float textures too (TextureFor)
int g_option_validate = 1;   // 1: re-check cached textures' bytes each frame
int g_option_buffers = 1;    // 1: re-check cached vertex/index bytes each frame

bool Decodable(xenos::TextureFormat format) {
  using F = xenos::TextureFormat;
  switch (format) {
    case F::k_8_8_8_8: case F::k_DXT1: case F::k_DXT2_3: case F::k_DXT4_5:
      return true;
    case F::k_8_8_8_8_A:
    case F::k_8: case F::k_8_A: case F::k_8_B: case F::k_8_8: case F::k_5_6_5: case F::k_1_5_5_5:
    case F::k_4_4_4_4: case F::k_2_10_10_10: case F::k_DXN: case F::k_DXT5A: case F::k_DXT3A:
    case F::k_CTX1: case F::k_16_16_16_16_FLOAT: case F::k_16_16_FLOAT: case F::k_32_FLOAT:
    case F::k_32_32_FLOAT:
      return g_option_formats != 0;
    default:
      return false;
  }
}

// Float formats are handed over as RGBA16F (16-bit ones) or RGBA32F texels
// (kFloat16Flag / kFloat32Flag in the format word); the rest as RGBA8.
constexpr uint32_t kFloat16Flag = 0x200, kFloat32Flag = 0x400;
uint32_t ChannelBytes(xenos::TextureFormat format) {
  using F = xenos::TextureFormat;
  switch (format) {
    case F::k_16_16_16_16_FLOAT: case F::k_16_16_FLOAT: return 2;
    case F::k_32_FLOAT: case F::k_32_32_FLOAT: return 4;
    default: return 1;
  }
}

// One float texel (its components X, Y, ... little-endian after unswapping)
// as four channels of ChannelBytes each: 32_FLOAT replicated (RRRR, the
// native host swizzle), two-channel ones X Y 0 1 (the RG host formats).
void DecodeFloatTexel(xenos::TextureFormat format, const uint8_t* texel, uint8_t* out) {
  using F = xenos::TextureFormat;
  const uint16_t one16 = 0x3C00;
  const float one32 = 1.0f;
  switch (format) {
    case F::k_16_16_16_16_FLOAT: std::memcpy(out, texel, 8); break;
    case F::k_16_16_FLOAT:
      std::memcpy(out, texel, 4); std::memset(out + 4, 0, 2); std::memcpy(out + 6, &one16, 2);
      break;
    case F::k_32_FLOAT: for (int c = 0; c < 4; c++) std::memcpy(out + 4 * c, texel, 4); break;
    case F::k_32_32_FLOAT:
      std::memcpy(out, texel, 8); std::memset(out + 8, 0, 4); std::memcpy(out + 12, &one32, 4);
      break;
    default: break;
  }
}

// Applies the fetch constant's swizzle (3 bits per output channel: 0-3 pick
// X/Y/Z/W, 4 is 0, 5 is 1) to decoded XYZW texels (channels of `bytes`
// each: 1 unorm, 2 half, 4 float), in place.
void ApplySwizzle(std::vector<uint8_t>& image, uint32_t swizzle, uint32_t bytes = 1) {
  if (swizzle == xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA) return;
  const uint16_t one16 = 0x3C00;
  const float one32 = 1.0f;
  const size_t texel = 4 * bytes;
  for (size_t i = 0; i + texel <= image.size(); i += texel) {
    uint8_t source[16];
    std::memcpy(source, image.data() + i, texel);
    for (int c = 0; c < 4; c++) {
      const uint32_t select = (swizzle >> (3 * c)) & 7;
      uint8_t* to = image.data() + i + c * bytes;
      if (select < 4) {
        std::memcpy(to, source + select * bytes, bytes);
      } else if (select == 5) {
        if (bytes == 1) *to = 255; else if (bytes == 2) std::memcpy(to, &one16, 2); else std::memcpy(to, &one32, 4);
      } else {
        std::memset(to, 0, bytes);
      }
    }
  }
}

// Power-of-two sizes: Xenos mips halve the next power of two, which WebGPU's
// floor(size / 2^level) only matches then; other sizes' mips are decoded at
// the guest's size and resampled to WebGPU's (DecodeTexture).
bool PowerOfTwo(const TextureInfo& info) {
  const uint32_t width = info.width + 1, height = info.height + 1;
  return !(width & (width - 1)) && !(height & (height - 1));
}

// The mip levels this capture hands the page: the chain the fetch constant
// names (WebGPU's mip count for the size at most), mip 0 alone for cube maps.
uint32_t MipLevels(const TextureInfo& info) {
  const uint32_t width = info.width + 1, height = info.height + 1;
  if (!info.memory.mip_address || info.mip_min_level != 0 || info.dimension == xenos::DataDimension::kCube) {
    return 1;
  }
  const uint32_t full = uint32_t(std::bit_width(std::max(width, height)));
  return std::max(1u, std::min(info.mip_levels(), full));
}

// A mip decoded at the guest's size (gw x gh texels of `texel` bytes)
// resampled to WebGPU's (mw x mh): each texel the average of the guest
// texels it covers (RGBA8 channels only; float ones take the nearest).
void Resample(const std::vector<uint8_t>& guest, uint32_t gw, uint32_t gh, uint8_t* out, uint32_t mw,
              uint32_t mh, uint32_t texel) {
  for (uint32_t y = 0; y < mh; y++) {
    const uint32_t y0 = y * gh / mh, y1 = std::max(y0 + 1, (y + 1) * gh / mh);
    for (uint32_t x = 0; x < mw; x++) {
      const uint32_t x0 = x * gw / mw, x1 = std::max(x0 + 1, (x + 1) * gw / mw);
      uint8_t* to = out + (size_t(y) * mw + x) * texel;
      if (texel != 4) {
        std::memcpy(to, guest.data() + (size_t(y0) * gw + x0) * texel, texel);
        continue;
      }
      uint32_t sum[4] = {};
      for (uint32_t gy = y0; gy < y1; gy++)
        for (uint32_t gx = x0; gx < x1; gx++)
          for (int c = 0; c < 4; c++) sum[c] += guest[(size_t(gy) * gw + gx) * 4 + c];
      const uint32_t n = (y1 - y0) * (x1 - x0);
      for (int c = 0; c < 4; c++) to[c] = uint8_t((sum[c] + n / 2) / n);
    }
  }
}

// Calls `block(bx, by, bytes)` for each block of a mip, untiled and with the
// fetch constant's endian swap undone. Packed mips (and small textures' mip
// 0) sit at a block offset inside their tile. False when it isn't readable.
// (`face`: a cube map's face, mip 0 only - faces follow one another, each
// rounded up to 4 KiB)
uint32_t FaceStride(const TextureInfo& info) {
  const auto extent = info.GetMipExtent(0, true);
  const uint32_t bytes = extent.block_pitch_h * extent.block_pitch_v * info.format_info()->bytes_per_block();
  return (bytes + 4095) & ~4095u;
}

template <class F>
bool ForEachBlock(const TextureInfo& info, uint32_t mip, F&& block, uint32_t face = 0) {
  const auto* format_info = info.format_info();
  const uint32_t block_bytes = format_info->bytes_per_block();
  if (!block_bytes) return false;
  uint32_t offset_x = 0, offset_y = 0;
  uint32_t address = info.GetMipLocation(mip, &offset_x, &offset_y, true);
  if (!address) return false;
  if (face) address += face * FaceStride(info);
  const auto extent = info.GetMipExtent(mip, true);
  uint32_t width = 0, height = 0;
  info.GetMipSize(mip, &width, &height);
  const uint32_t bw = format_info->block_width, bh = format_info->block_height;
  const uint32_t blocks_x = (width + bw - 1) / bw, blocks_y = (height + bh - 1) / bh;
  const uint32_t pitch = extent.block_pitch_h;
  const uint32_t log2 = uint32_t(std::bit_width(block_bytes) - 1);
  const uint8_t* source = g_memory->TranslatePhysical<const uint8_t*>(address);
  uint8_t bytes[16];
  for (uint32_t by = 0; by < blocks_y; by++) {
    for (uint32_t bx = 0; bx < blocks_x; bx++) {
      const uint32_t x = bx + offset_x, y = by + offset_y;
      const uint32_t offset =
          info.is_tiled ? uint32_t(texture_util::GetTiledOffset2D(int32_t(x), int32_t(y), pitch, log2))
                        : (y * pitch + x) * block_bytes;
      if (address + uint64_t(offset) + block_bytes > 0x20000000u) return false;
      std::memcpy(bytes, source + offset, block_bytes);
      Unswap(bytes, block_bytes, info.endianness);
      block(bx, by, bytes);
    }
  }
  return true;
}

// Decodes a 2D texture's mips (MipLevels) into `image`, one after another
// (XYZW components as RGBA8, before the fetch swizzle). False when it is not
// one this capture understands.
// A cube map's six faces (mip 0 each) one after another.
uint32_t FaceCount(const TextureInfo& info) {
  return info.dimension == xenos::DataDimension::kCube ? 6 : 1;
}

bool DecodeTexture(const TextureInfo& info, std::vector<uint8_t>& image) {
  const uint32_t width = info.width + 1, height = info.height + 1;
  if (!Decodable(info.format) || !info.format_info()->bytes_per_block() || width > kMaxDimension ||
      height > kMaxDimension) {
    return false;
  }
  image.clear();
  const uint32_t channel = ChannelBytes(info.format), texel = 4 * channel;
  const uint32_t levels = MipLevels(info);
  for (uint32_t face = 0; face < FaceCount(info); face++) {
    for (uint32_t mip = 0; mip < levels; mip++) {
      const uint32_t mw = std::max(1u, width >> mip), mh = std::max(1u, height >> mip);
      const size_t at = image.size();
      image.resize(at + size_t(mw) * mh * texel, 0);
      // (a mip of a size that isn't a power of two: decoded at the guest's
      // size, then resampled)
      uint32_t gw = mw, gh = mh;
      if (mip && !PowerOfTwo(info)) info.GetMipSize(mip, &gw, &gh);
      std::vector<uint8_t> guest;
      if (gw != mw || gh != mh) guest.resize(size_t(gw) * gh * texel, 0);
      uint8_t* level = guest.empty() ? image.data() + at : guest.data();
      const bool read = ForEachBlock(info, mip, [&](uint32_t bx, uint32_t by, const uint8_t* bytes) {
        if (channel == 1) {
          DecodeBlock(info.format, bytes, bx, by, gw, gh, level);
        } else if (bx < gw && by < gh) {
          DecodeFloatTexel(info.format, bytes, level + (size_t(by) * gw + bx) * texel);
        }
      }, face);
      if (read && !guest.empty()) Resample(guest, gw, gh, image.data() + at, mw, mh, texel);
      if (!read) {
        if (mip == 0) return false;
        image.resize(at);  // (the chain stops at the first level it can't read)
        break;
      }
    }
  }
  return true;
}

// Compressed textures the page can upload as they are (BC1/2/3/5 under
// WebGPU's texture-compression-bc, once it says it has it: option 4): the
// blocks untiled and byte-swapped, without decoding. Only with the plain
// swizzle (BC can't reorder channels) and whole blocks on both axes.
int g_option_bc = 0;
constexpr uint32_t kCompressedFlag = 0x100;

bool Compressible(const TextureInfo& info, uint32_t swizzle) {
  using F = xenos::TextureFormat;
  const bool format = info.format == F::k_DXT1 || info.format == F::k_DXT2_3 ||
                      info.format == F::k_DXT4_5 || info.format == F::k_DXN;
  return g_option_bc && format && swizzle == xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA &&
         info.dimension != xenos::DataDimension::kCube && (PowerOfTwo(info) || MipLevels(info) == 1) &&
         (info.width + 1) % 4 == 0 && (info.height + 1) % 4 == 0;
}

// The blocks of each mip (MipLevels), untiled, in rows of whole blocks, one
// mip after another (as WebGPU copies them: a mip smaller than a block is a
// whole block).
bool CopyBlocks(const TextureInfo& info, std::vector<uint8_t>& blocks) {
  const auto* format_info = info.format_info();
  const uint32_t width = info.width + 1, height = info.height + 1;
  const uint32_t block_bytes = format_info->bytes_per_block();
  if (!block_bytes || width > kMaxDimension || height > kMaxDimension) return false;
  blocks.clear();
  const uint32_t levels = MipLevels(info);
  for (uint32_t mip = 0; mip < levels; mip++) {
    const uint32_t blocks_x = (std::max(1u, width >> mip) + 3) / 4;
    const uint32_t blocks_y = (std::max(1u, height >> mip) + 3) / 4;
    const size_t at = blocks.size();
    blocks.resize(at + size_t(blocks_x) * blocks_y * block_bytes, 0);
    uint8_t* level = blocks.data() + at;
    const bool read = ForEachBlock(info, mip, [&](uint32_t bx, uint32_t by, const uint8_t* bytes) {
      if (bx < blocks_x && by < blocks_y) {
        std::memcpy(level + (size_t(by) * blocks_x + bx) * block_bytes, bytes, block_bytes);
      }
    });
    if (!read) {
      if (mip == 0) return false;
      blocks.resize(at);
      break;
    }
  }
  return true;
}

// A hash of three 256-byte samples (start, middle, end) of guest bytes and
// the size: new contents in recycled memory change them (once a frame for
// every buffer and texture drawn, so kept small).
// (`virtual_address`: a buffer's, which the game gives as a virtual address,
// as the native renderer reads it; textures' are physical)
uint64_t ContentSample(uint32_t address, uint32_t size, bool virtual_address = false) {
  if (!virtual_address) address &= 0x1FFFFFFFu;  // the same memory through any physical window
  if (!size || (!virtual_address && address + uint64_t(size) > 0x20000000u) ||
      uint64_t(address) + size > uint64_t(UINT32_MAX) + 1) {
    return 0;
  }
  const uint8_t* bytes = virtual_address ? g_memory->TranslateVirtual<const uint8_t*>(address)
                                         : g_memory->TranslatePhysical<const uint8_t*>(address);
  if (!bytes) return 0;
  const uint32_t chunk = std::min<uint32_t>(size, 256);
  uint64_t hash = XXH3_64bits(bytes, chunk) ^ size;
  if (size > 3 * chunk) {
    hash ^= XXH3_64bits(bytes + (size / 2 & ~3u), chunk) * 31;
    hash ^= XXH3_64bits(bytes + size - chunk, chunk) * 131;
  }
  return hash;
}

// A hash of all of a small texture's guest bytes (physical), 0 when they
// are outside memory.
uint64_t WholeContent(uint32_t address, uint32_t size) {
  address &= 0x1FFFFFFFu;
  if (!size || address + uint64_t(size) > 0x20000000u) return 0;
  return XXH3_64bits(g_memory->TranslatePhysical<const uint8_t*>(address), size);
}

xenos::xe_gpu_texture_fetch_t ReadFetch(uint32_t device, uint32_t stage) {
  const uint8_t* state =
      g_memory->TranslateVirtual<const uint8_t*>(device + kTextureFetchBase + stage * 0x18);
  xenos::xe_gpu_texture_fetch_t fetch{};
  fetch.dword_0 = LoadBE32(state + 0);
  fetch.dword_1 = LoadBE32(state + 4);
  fetch.dword_2 = LoadBE32(state + 8);
  fetch.dword_3 = LoadBE32(state + 12);
  fetch.dword_4 = LoadBE32(state + 16);
  fetch.dword_5 = LoadBE32(state + 20);
  return fetch;
}

// A fetch base (4 KiB page of a physical address) without the window it was
// addressed through: resolves and the draws sampling them name the same
// memory through different windows.
uint32_t PhysicalPage(uint32_t base) {
  return base & 0x1FFFF;
}

// Diagnostics: this frame's texture decodes and buffer conversions (count,
// time, bytes), logged for a frame that spent long on them (OnPresent).
struct FrameWork {
  uint32_t decodes = 0, converts = 0;
  int64_t decode_ns = 0, convert_ns = 0;
  uint64_t decode_bytes = 0, convert_bytes = 0;
};
FrameWork g_frame_work;
int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool FillSlotNow(TextureSlot& entry, const TextureInfo& info, uint32_t swizzle);
// Decodes (or, when the page takes them, copies the compressed blocks of) a
// texture into its slot.
bool FillSlot(TextureSlot& entry, const TextureInfo& info, uint32_t swizzle) {
  const int64_t start = NowNs();
  const bool ok = FillSlotNow(entry, info, swizzle);
  const int64_t took = NowNs() - start;
  g_frame_work.decodes++;
  g_frame_work.decode_ns += took;
  g_frame_work.decode_bytes += entry.pixels.size();
  if (took > 20'000'000) {
    static int logged = 0;
    if (logged++ < 40) {
      REXLOG_INFO("web slow decode: {}x{} format {} dimension {} mips {} at {:08X} -> {} KB {} in {:.1f} ms",
                  info.width + 1, info.height + 1, uint32_t(info.format), uint32_t(info.dimension),
                  info.mip_max_level + 1, info.memory.base_address, entry.pixels.size() >> 10,
                  entry.compressed ? "blocks" : "texels", took / 1e6);
    }
  }
  return ok;
}
bool FillSlotNow(TextureSlot& entry, const TextureInfo& info, uint32_t swizzle) {
  const uint32_t width = info.width + 1, height = info.height + 1;
  if (Compressible(info, swizzle) && CopyBlocks(info, entry.pixels)) {
    entry.compressed = true;
  } else {
    entry.compressed = false;
    if (!DecodeTexture(info, entry.pixels)) return false;
    ApplySwizzle(entry.pixels, swizzle, ChannelBytes(info.format));
  }
  // How many mips that is (a chain can stop early; a cube map's six faces
  // hold the same number).
  const size_t block_bytes = info.format_info()->bytes_per_block();
  const size_t texel = 4 * ChannelBytes(info.format);
  const size_t face_bytes = entry.pixels.size() / FaceCount(info);
  size_t total = 0;
  entry.levels = 0;
  while (total < face_bytes && entry.levels < 16) {
    const uint32_t mw = std::max(1u, width >> entry.levels), mh = std::max(1u, height >> entry.levels);
    total += entry.compressed ? size_t((mw + 3) / 4) * ((mh + 3) / 4) * block_bytes : size_t(mw) * mh * texel;
    entry.levels++;
  }
  entry.levels = std::max(1u, entry.levels);
  return true;
}

// The format word the page reads: Xenos format, compressed / float16 /
// float32 / cube flags, mip count.
constexpr uint32_t kCubeFlag = 0x800;
uint32_t TextureFormatWord(const TextureSlot& entry, const TextureInfo& info) {
  const uint32_t channel = ChannelBytes(info.format);
  return uint32_t(info.format) | (entry.compressed ? kCompressedFlag : 0) |
         (channel == 2 ? kFloat16Flag : channel == 4 ? kFloat32Flag : 0) |
         (info.dimension == xenos::DataDimension::kCube ? kCubeFlag : 0) | entry.levels << 16;
}

// Texture table index + 1 for a stage's texture, decoding it the first
// time; 0 when there is none or it cannot be decoded.
// (`targets`: the 2D compositor's targets, by page, count as textures; the 3D
// path has looked its resolves up already and wants the memory's own texture)
// The keyboard set's Controls picture (tools/xtd_tools/build_controller_diagrams.py):
// frontend_360.xtd holds the on-foot one; the page's labels change in a
// vehicle, so the loaded texture's bytes - found by their hash when first
// decoded - are overwritten with the other picture's, as tiled DXT5 in
// guest memory, whenever the title asks for the other one.
struct ControlsPictures {
  bool loaded = false;
  std::vector<uint8_t> pictures[2];  // controls_foot.bin, controls_vehicle.bin
  uint64_t hashes[2] = {};
  uint32_t address = 0;              // the loaded texture's physical base
  int shown = -1;
};
ControlsPictures g_controls;
std::atomic<int> g_controls_wanted{0};

// (all of it: the pictures' first blocks are the same empty corner)
uint64_t ControlsHash(const uint8_t* bytes) { return XXH3_64bits(bytes, 256 * 1024); }

void LoadControlsPictures() {
  g_controls.loaded = true;
  const char* files[2] = {"/game/button_prompts/keyboard/controls_foot.bin",
                          "/game/button_prompts/keyboard/controls_vehicle.bin"};
  for (int i = 0; i < 2; i++) {
    FILE* file = std::fopen(files[i], "rb");
    if (!file) return;
    std::vector<uint8_t> bytes(256 * 1024);
    const size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    if (read != bytes.size()) return;
    g_controls.pictures[i] = std::move(bytes);
    g_controls.hashes[i] = ControlsHash(g_controls.pictures[i].data());
  }
}

// (a 512x512 DXT5 texture being decoded: one of the pictures?)
void NoteControlsPicture(const xenos::xe_gpu_texture_fetch_t& fetch, uint32_t address) {
  if (xenos::TextureFormat(fetch.format) != xenos::TextureFormat::k_DXT4_5 || fetch.size_2d.width != 511 ||
      fetch.size_2d.height != 511 || address == g_controls.address || (address & 0x1FFFFFFFu) + 256 * 1024 > 0x20000000u) {
    return;
  }
  if (!g_controls.loaded) LoadControlsPictures();
  if (g_controls.pictures[1].empty()) return;
  const uint64_t hash = ControlsHash(g_memory->TranslatePhysical<const uint8_t*>(address));
  for (int i = 0; i < 2; i++) {
    if (hash == g_controls.hashes[i]) {
      g_controls.address = address;
      g_controls.shown = i;
      REXLOG_INFO("web controls picture: the keyboard set's {} picture at {:08X}", i ? "vehicle" : "on-foot", address);
    }
  }
}

uint32_t TextureFor(uint32_t device, uint32_t stage = 0, bool targets = true) {
  const auto fetch = ReadFetch(device, stage);
  // A resolve destination: the page draws that target's quads instead.
  if (targets && fetch.type == xenos::FetchConstantType::kTexture && g_targets.count(PhysicalPage(fetch.base_address))) {
    return kTargetTextureFlag | PhysicalPage(fetch.base_address);
  }
  // Diagnostics: textures this capture can't decode and no resolve produced.
  if (fetch.type == xenos::FetchConstantType::kTexture && !Decodable(xenos::TextureFormat(fetch.format))) {
    static int logged = 0;
    if (logged < 30) {
      logged++;
      auto near = g_targets.lower_bound(PhysicalPage(fetch.base_address));
      REXLOG_INFO("web target miss: stage {} base {:05X} ({}x{} fmt {} dim {}) next target {:05X} of {}",
                  stage, uint32_t(fetch.base_address), fetch.size_2d.width + 1, fetch.size_2d.height + 1,
                  uint32_t(fetch.format), uint32_t(fetch.dimension),
                  near != g_targets.end() ? near->first : 0, g_targets.size());
    }
  }
  if (fetch.type != xenos::FetchConstantType::kTexture ||
      (fetch.dimension != xenos::DataDimension::k2DOrStacked && fetch.dimension != xenos::DataDimension::kCube) ||
      !Decodable(xenos::TextureFormat(fetch.format))) {
    return 0;
  }
  const uint64_t key = uint64_t(fetch.base_address) << 32 | uint64_t(fetch.format) << 26 |
                       uint64_t(fetch.size_2d.width) << 13 | fetch.size_2d.height |
                       (fetch.dimension == xenos::DataDimension::kCube ? 1ull << 63 : 0);
  const uint64_t lookup = key ^ (uint64_t(fetch.swizzle) * 0x9E3779B97F4A7C15ull);
  TextureInfo info;
  if (const auto found = g_texture_lookup.find(lookup); found != g_texture_lookup.end()) {
    const uint32_t i = found->second;
    TextureSlot& entry = g_texture_slots[i];
    if (entry.key == key && entry.swizzle == fetch.swizzle) {
      entry.used_frame = g_frame_number;
      const bool small = uint32_t(fetch.size_2d.width + 1) * (fetch.size_2d.height + 1) <= kRedecodeArea;
      // (once a frame: a texture is looked up for each draw using it)
      uint64_t whole = 0;
      bool redecode = false;
      if (small && entry.decoded_frame != g_frame_number && entry.whole_frame != g_frame_number) {
        entry.whole_frame = g_frame_number;
        whole = WholeContent(entry.address, entry.size);
        redecode = !whole || whole != entry.whole;
      }
      if (!redecode && g_option_validate && entry.checked_frame != g_frame_number) {
        entry.checked_frame = g_frame_number;
        redecode = ContentSample(entry.address, entry.size) != entry.content;
      }
      if (redecode && TextureInfo::Prepare(fetch, &info) && FillSlot(entry, info, fetch.swizzle)) {
        entry.decoded_frame = g_frame_number;
        g_textures[i].format = TextureFormatWord(entry, info);
        entry.content = ContentSample(entry.address, entry.size);
        entry.whole = small ? (whole ? whole : WholeContent(entry.address, entry.size)) : 0;
        g_textures[i].pixels = reinterpret_cast<uint64_t>(entry.pixels.data());
        g_textures[i].serial = ++g_texture_serial;
      }
      return i + 1;
    }
  }
  if (!TextureInfo::Prepare(fetch, &info)) return 0;
  // A large float texture is a render target the game samples (the 2560x2560
  // sun shadow map, a 4096x4096 16_16 float target), met here only in a
  // scene's first frame, before its resolve is known: its memory holds
  // nothing the page drew, and decoding it froze that frame for 100-240 ms
  // and kept 100-130 MB. The draw takes the stand-in for that frame instead.
  // (The game's own textures are block-compressed or 8-bit.)
  switch (xenos::TextureFormat(fetch.format)) {
    case xenos::TextureFormat::k_16_FLOAT:
    case xenos::TextureFormat::k_16_16_FLOAT:
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
    case xenos::TextureFormat::k_32_FLOAT:
    case xenos::TextureFormat::k_32_32_FLOAT:
    case xenos::TextureFormat::k_32_32_32_32_FLOAT:
      if (!g_option_float_targets && uint64_t(info.width + 1) * (info.height + 1) >= 2048ull * 2048) {
        static int logged = 0;
        if (logged++ < 10) {
          REXLOG_INFO("web texture: {}x{} float target (format {}) at {:08X} not decoded", info.width + 1,
                      info.height + 1, uint32_t(fetch.format), info.memory.base_address);
        }
        return 0;
      }
      break;
    default:
      break;
  }
  uint32_t slot = kMaxTextures;
  if (g_texture_count < kMaxTextures) {
    slot = g_texture_count++;
  } else {
    // Reuse a slot no draw used in the last few frames: the frame being
    // recorded (and the one the page shows) may still name the others.
    for (uint32_t tries = 0; tries < kMaxTextures; tries++) {
      const uint32_t candidate = g_next_reuse++ % kMaxTextures;
      if (g_texture_slots[candidate].used_frame + 3 < g_frame_number) {
        slot = candidate;
        break;
      }
    }
    if (slot == kMaxTextures) return 0;
  }
  TextureSlot& entry = g_texture_slots[slot];
  entry.used_frame = g_frame_number;
  if (entry.key) {
    g_texture_lookup.erase(entry.key ^ (uint64_t(entry.swizzle) * 0x9E3779B97F4A7C15ull));
  }
  if (!FillSlot(entry, info, fetch.swizzle)) {
    entry.key = 0;
    return 0;
  }
  entry.key = key;
  entry.swizzle = fetch.swizzle;
  entry.decoded_frame = entry.checked_frame = g_frame_number;
  entry.address = info.memory.base_address;
  entry.size = std::min<uint32_t>(info.memory.base_size ? info.memory.base_size : 4096, 64u << 20);
  entry.content = ContentSample(entry.address, entry.size);
  entry.whole = uint32_t(info.width + 1) * (info.height + 1) <= kRedecodeArea ? WholeContent(entry.address, entry.size) : 0;
  g_texture_lookup[lookup] = slot;
  NoteControlsPicture(fetch, entry.address);
  g_textures[slot] = {info.width + 1, info.height + 1,
                      reinterpret_cast<uint64_t>(entry.pixels.data()), ++g_texture_serial,
                      TextureFormatWord(entry, info)};
  return slot + 1;
}

// GTA IV's immediate-mode vertex: x, y, z in screen pixels, three unused
// words, the colour (0xAARRGGBB), then u, v. Big-endian, 36 bytes.
constexpr uint32_t kImmediateStride = 36;

void RecordQuad(const native::DrawPrimitiveUpCommand& draw) {
  // Xbox 360 D3DPT_TRIANGLESTRIP = 6; four vertices make one quad.
  if (draw.primitive_type != 6 || draw.vertex_count != 4 || draw.stride != kImmediateStride ||
      !draw.vertex_data || g_pending.size() >= kMaxQuads) {
    return;
  }
  const uint8_t* v = g_memory->TranslateVirtual<const uint8_t*>(draw.vertex_data);
  WebQuad quad{};
  for (int i = 0; i < 4; i++) {
    const uint8_t* vertex = v + i * kImmediateStride;
    quad.x[i] = std::bit_cast<float>(LoadBE32(vertex + 0));
    quad.y[i] = std::bit_cast<float>(LoadBE32(vertex + 4));
    quad.u[i] = std::bit_cast<float>(LoadBE32(vertex + 28));
    quad.v[i] = std::bit_cast<float>(LoadBE32(vertex + 32));
  }
  quad.color = LoadBE32(v + 24);
  quad.texture = TextureFor(draw.device);
  // Output state, where the native renderer reads it (graphics_system.cpp:
  // kPackedColorWriteMaskOffset, kBlendControlOffset, kHardwareBlendPackedOffsets).
  const uint8_t* device = g_memory->TranslateVirtual<const uint8_t*>(draw.device);
  quad.output = (LoadBE32(device + 10460) & 0xF) | (LoadBE32(device + 11844) & 0x80000000u);
  quad.blend = LoadBE32(device + 10552);
  // The fade: log the full-screen quad's colour every 10 frames.
  if (quad.x[0] == 0 && quad.y[0] == 0 && quad.x[3] >= 1279 && (g_frame_number % 10) == 0) {
    static uint32_t last_frame = ~0u;
    static int logs = 0;
    if (last_frame != g_frame_number && logs++ < 120) {
      last_frame = g_frame_number;
      REXLOG_INFO("web fade: frame {} full-screen quad colour {:08X}", g_frame_number, quad.color);
    }
  }
  g_pending.push_back(quad);
}

// --- 3D draws ---------------------------------------------------------------

// Resolve destinations by guest texture object (as gta4_metal keys them):
// draws find theirs by the texture bound to the stage (device + 0x30F8).
constexpr uint32_t kBoundTexturesOffset = 0x30F8;
std::unordered_map<uint32_t, uint32_t> g_resolve_ids;

uint32_t ResolveId(uint32_t texture) {
  return g_resolve_ids.try_emplace(texture, uint32_t(g_resolve_ids.size() + 1)).first->second;
}

// Resolve ids by the physical page their destination starts at: some draws
// sample a resolve through another texture object over the same memory.
// (with whether it was a depth resolve: a depth resolve's memory is read as a
// depth texture, k_24_8 / k_24_8_FLOAT; a colour texture the game later keeps
// at the same address - UI and font images - is not that resolve)
// (one of each per page: GTA IV's radar texture shares its page with the scene
// depth resolve, and either may be the latest there)
struct ResolvePage {
  uint32_t color = 0, depth = 0;
  uint32_t color_serial = 0, depth_serial = 0;  // which of them came last
  uint32_t depth_size = 0;                      // the depth resolve's w | h << 16
};
constexpr uint32_t kPackedDepthFlag = 0x40000000u;
uint32_t g_resolve_serial = 0;
bool IsDepthTextureFormat(uint32_t format) {
  return xenos::TextureFormat(format) == xenos::TextureFormat::k_24_8 ||
         xenos::TextureFormat(format) == xenos::TextureFormat::k_24_8_FLOAT;
}
std::unordered_map<uint32_t, ResolvePage> g_resolve_pages;
// Physical pages the game has presented from (its front buffers).
std::unordered_set<uint32_t> g_front_pages;

// (diagnostics: each distinct texture-to-resolve mapping once)
void LogTargetMapping(const char* via, uint32_t stage, uint32_t handle, const xenos::xe_gpu_texture_fetch_t& fetch,
                      uint32_t id) {
  static std::unordered_set<uint64_t> seen;
  const uint64_t key = uint64_t(handle) << 32 ^ uint64_t(fetch.base_address) << 8 ^ id;
  if (seen.size() >= 300 || !seen.insert(key).second) return;
  REXLOG_INFO("web tex map: via {} stage {} handle {:08X} base {:05X} {}x{} fmt {} -> id {}", via, stage, handle,
              uint32_t(fetch.base_address), fetch.size_2d.width + 1, fetch.size_2d.height + 1,
              uint32_t(fetch.format), id);
}

uint32_t TextureFor3D(uint32_t device, uint32_t stage) {
  const uint8_t* bound = g_memory->TranslateVirtual<const uint8_t*>(device + kBoundTexturesOffset);
  const uint32_t handle = LoadBE32(bound + stage * 4);
  const auto fetch = ReadFetch(device, stage);
  const auto found = g_resolve_ids.find(handle);
  if (found != g_resolve_ids.end()) {
    LogTargetMapping("handle", stage, handle, fetch, found->second);
    return kTargetTextureFlag | found->second;
  }
  if (fetch.type == xenos::FetchConstantType::kTexture) {
    auto page = g_resolve_pages.find(PhysicalPage(fetch.base_address));
    // (the game samples a depth resolve one 4 KB page past the base its
    // resolve destination names: look there for the depth when this page
    // has none)
    if (page == g_resolve_pages.end() || !page->second.depth) {
      const auto below = g_resolve_pages.find(PhysicalPage(fetch.base_address) - 1);
      if (below != g_resolve_pages.end() && below->second.depth &&
          (xenos::TextureFormat(fetch.format) == xenos::TextureFormat::k_8_8_8_8 ||
           IsDepthTextureFormat(uint32_t(fetch.format))) &&
          below->second.depth_size == ((fetch.size_2d.width + 1) | (fetch.size_2d.height + 1) << 16)) {
        page = below;
      }
    }
    // An A8R8G8B8 texture of a depth resolve's size over its memory, the
    // latest resolve there (sub_828D9768 reads the scene's D24S8 depth that
    // way): the page packs the resolved depth and stencil into its bytes
    // (packed_depth_alias_ps), with the fetch swizzle in bits 18-29.
    if (page != g_resolve_pages.end() && page->second.depth &&
        xenos::TextureFormat(fetch.format) == xenos::TextureFormat::k_8_8_8_8 &&
        page->second.depth_serial > page->second.color_serial &&
        page->second.depth_size == ((fetch.size_2d.width + 1) | (fetch.size_2d.height + 1) << 16)) {
      LogTargetMapping("packed depth", stage, handle, fetch, page->second.depth);
      return kTargetTextureFlag | kPackedDepthFlag | (uint32_t(fetch.swizzle) & 0xFFF) << 18 |
             (page->second.depth & 0x3FFFF);
    }
    const uint32_t id = page == g_resolve_pages.end() ? 0
                        : IsDepthTextureFormat(uint32_t(fetch.format)) ? page->second.depth
                                                                       : page->second.color;
    if (id) {
      LogTargetMapping("page", stage, handle, fetch, id);
      return kTargetTextureFlag | id;
    }
  }
  return TextureFor(device, stage, false);
}

// Device layout (gta4_metal draw.mm): constants and booleans.
constexpr uint32_t kVertexConstantsOffset = 0x780;
constexpr uint32_t kPixelConstantsOffset = 0x1780;
constexpr uint32_t kConstantsSize = 4096;
constexpr uint32_t kVertexBooleansOffset = 0x2780;
constexpr uint32_t kPixelBooleansOffset = 0x2790;
constexpr uint32_t kDrawConstantsSize = 2 * kConstantsSize + sizeof(native::core::SharedConstants);
constexpr size_t kMaxDraws = 16384;

std::unordered_map<uint32_t, uint64_t> g_shader_hashes;  // shader object -> cache hash
// Texture stages each shader samples, by cache hash, as the page reports them
// from the shaders' WGSL (rex_web_shader_stages); unknown shaders: all.
std::unordered_map<uint64_t, uint32_t> g_shader_stage_masks;

uint32_t StageMask(uint64_t hash) {
  const auto found = g_shader_stage_masks.find(hash);
  return found != g_shader_stage_masks.end() ? found->second : ~0u;
}
// The 256-byte pieces (16 registers each, a bit each) of the vertex and
// pixel constant banks each shader reads, by cache hash, as the page works
// them out from its WGSL (scene3d.js constantNeed, rex_web_shader_constants);
// unknown shaders: all of both.
struct ConstantNeed {
  uint16_t vertex = 0xFFFF, pixel = 0xFFFF;
};
std::unordered_map<uint64_t, ConstantNeed> g_shader_constant_needs;

ConstantNeed NeedOf(uint64_t hash) {
  const auto found = g_shader_constant_needs.find(hash);
  return found != g_shader_constant_needs.end() ? found->second : ConstantNeed{};
}
uint32_t g_vertex_shader = 0, g_pixel_shader = 0, g_declaration = 0, g_index_buffer = 0;
struct StreamState {
  uint32_t buffer = 0, offset = 0, stride = 0;
};
StreamState g_streams[native::kVertexStreamCount];
uint32_t g_target_width = 1280, g_target_height = 720;
// Bound surfaces: colour 0-3, then depth/stencil.
native::SurfaceDescriptor g_surfaces[5];

uint32_t PackSize(const native::SurfaceDescriptor& s) {
  return s.handle ? (s.width & 0xFFFF) | s.height << 16 : 0;
}

// Which surface last wrote each EDRAM base (RB_COLOR_INFO's low 12 bits,
// the packed word at +28): surfaces sharing EDRAM are one memory on the
// console, so a resolve of a surface another one wrote over reads that one's
// pixels (GTA IV resolves its 1280x720 scene through a 640x360 4x MSAA view of
// the same EDRAM, a 2x2 downsample for free).
struct EdramWriter {
  uint32_t handle, width, height, serial;
};
// (colour and depth separately: a depth resolve reads the last depth writer)
std::unordered_map<uint32_t, EdramWriter> g_edram_writers, g_edram_depth_writers;
std::unordered_map<uint32_t, uint32_t> g_handle_writes;  // handle -> serial of its last write
uint32_t g_write_serial = 0;

void NoteWrite(const native::SurfaceDescriptor& s, bool depth) {
  if (!s.handle) return;
  // (diagnostics: each surface's colour info once: format and exponent bias)
  static std::unordered_set<uint32_t> described;
  if (described.size() < 200 && described.insert(s.handle).second) {
    REXLOG_INFO("web surface: {:08X} {} {}x{} info {:08X} format {} exp {} edram {:03X} samples {}", s.handle,
                depth ? "depth" : "color", s.width, s.height, s.address, (s.address >> 16) & 15,
                native::NativeColorExponent(s.address), s.address & 0xFFF, s.sample_type);
  }
  const uint32_t serial = ++g_write_serial;
  (depth ? g_edram_depth_writers : g_edram_writers)[s.address & 0xFFF] = {s.handle, s.width, s.height, serial};
  g_handle_writes[s.handle] = serial;
}

// The bound colour targets in `mask` (and the depth/stencil surface) written.
void NoteWrites(uint32_t mask, bool depth) {
  for (int i = 0; i < 4; i++)
    if (mask & (1u << i)) NoteWrite(g_surfaces[i], false);
  if (depth) NoteWrite(g_surfaces[4], true);
}

// An event with the bound surfaces filled in.
WebDraw BoundEvent(uint32_t kind) {
  WebDraw event{};
  event.kind = kind;
  for (int i = 0; i < 4; i++) event.colors[i] = g_surfaces[i].handle;
  event.depth = g_surfaces[4].handle;
  for (int i = 0; i < 5; i++) event.sizes[i] = PackSize(g_surfaces[i]);
  // Draws and clears, 19: the colour targets' formats (RB_COLOR_INFO's, four
  // bits each) | 0x80000000, so the page stores each surface as the console
  // does (8-bit targets quantize: shaders decode bits packed into them).
  if (kind == kWebDraw || kind == kWebClear) {
    uint32_t formats = 0x80000000u;
    for (int i = 0; i < 4; i++) formats |= ((g_surfaces[i].address >> 16) & 15u) << (4 * i);
    event.params[19] = formats;
  }
  return event;
}

// Declarations by guest object, each with its slot in the published table.
std::vector<WebDecl> g_decls;
std::unordered_map<uint32_t, uint32_t> g_decl_slots;
struct DeclarationView {
  std::vector<native::VertexElement> elements;
};
std::vector<DeclarationView> g_decl_views;

// Converted buffers, by guest buffer and (for vertices) how they were read.
struct ConvertedBuffer {
  uint32_t address = 0, size = 0, flags = 0, id = 0, serial = 0, unlocks = 0;
  // Samples of the guest bytes, checked once a frame: the game also streams
  // geometry into recycled buffers without an unlock this capture sees.
  uint32_t checked_frame = 0;
  uint64_t content = 0;
  std::vector<uint8_t> bytes;
};
std::unordered_map<uint64_t, ConvertedBuffer> g_buffers;
// How often each buffer was unlocked for writing: a capture older than its
// buffer's latest unlock is converted again.
std::unordered_map<uint32_t, uint32_t> g_unlocks;
uint32_t g_buffer_serial = 0, g_buffer_id = 0;
size_t g_buffer_bytes = 0;
// Diagnostics: buffer conversions by reason (new, unlocked, moved or
// resized, contents changed, cache full), count and bytes, logged each 120
// frames.
struct ConvertCounts {
  uint32_t count[5] = {};
  uint64_t bytes[5] = {};
};
ConvertCounts g_converts;
constexpr size_t kMaxBufferBytes = 768u << 20;

// This frame's draws and the bytes they point into (constants, generated
// indices), as offsets until the frame is published.
std::vector<WebDraw> g_draws;
std::vector<uint8_t> g_draw_bytes;
std::vector<WebDraw> g_published_draws[2];
std::vector<uint8_t> g_published_draw_bytes[2];
// Draw fields holding offsets into g_draw_bytes rather than pointers.
constexpr uint64_t kOffsetFlag = 1ull << 63;

// The latest copy of each constant block in this frame's bytes (its first
// `raw.size()` bytes: as much as that draw's shaders read).
struct ConstantBlock {
  std::vector<uint8_t> raw;
  size_t offset = 0;
  bool valid = false;
};
ConstantBlock g_last_shared_constants;
// The vertex and pixel banks' latest copies: the guest bytes of the pieces
// stored (`pieces`, ConstantNeed's bits); the other pieces of the stored
// block are left as they are (no draw using it reads them).
constexpr uint32_t kConstantPiece = 256;
struct ConstantBank {
  std::array<uint8_t, 4096> raw{};
  uint32_t pieces = 0;
  size_t offset = 0;
};
ConstantBank g_last_vertex_constants, g_last_pixel_constants;

size_t AppendDrawBytes(const void* data, size_t size) {
  const size_t offset = (g_draw_bytes.size() + 255) & ~size_t(255);
  g_draw_bytes.resize(offset + size);
  if (data) std::memcpy(g_draw_bytes.data() + offset, data, size);
  return offset;
}

struct VertexInputs {
  struct Input {
    uint32_t location;
    enum class NumericType { kFloat, kUnsignedInteger } numeric_type;
  };
  std::vector<Input> vertex_inputs;
};

const VertexInputs& FloatInputs() {
  static const VertexInputs inputs = [] {
    VertexInputs v;
    for (uint32_t i = 0; i < 40; i++) v.vertex_inputs.push_back({i, VertexInputs::Input::NumericType::kFloat});
    return v;
  }();
  return inputs;
}

// A guest vertex or index buffer, converted (cached until it is unlocked for
// writing or moves). Null when the handle doesn't describe a buffer.
ConvertedBuffer* Capture(uint32_t handle, uint32_t declaration, uint32_t stream, uint32_t offset,
                         uint32_t stride) {
  const uint8_t* header = g_memory->TranslateVirtual<const uint8_t*>(handle);
  if (!handle || !header) return nullptr;
  const uint32_t flags = LoadBE32(header);
  const auto metadata = native::DecodeNativeBufferMetadata(flags, LoadBE32(header + 24), LoadBE32(header + 28));
  if (!metadata || !metadata->HasValidPayload(64u << 20)) return nullptr;
  const bool index = (flags & 0xF) == 2;
  const uint64_t key = index ? uint64_t(handle)
                             : (uint64_t(handle) | uint64_t(declaration) << 32) ^
                                   (uint64_t(stream) << 59 | uint64_t(offset) << 40 | uint64_t(stride) << 24);
  auto found = g_buffers.find(key);
  int reason = 0;  // (g_converts: new)
  if (found != g_buffers.end()) {
    ConvertedBuffer& cached = found->second;
    const uint32_t unlocks = g_unlocks[handle];
    if (cached.address != metadata->guest_address || cached.size != metadata->guest_size || cached.flags != flags) {
      reason = 2;
    } else if (metadata->guest_locked || cached.unlocks != unlocks) {
      reason = 1;
    } else {
      if (!g_option_buffers || cached.checked_frame == g_frame_number) return &cached;
      cached.checked_frame = g_frame_number;
      if (ContentSample(cached.address, cached.size, true) == cached.content) return &cached;
      reason = 3;
    }
  }
  if (g_buffer_bytes > kMaxBufferBytes) {
    g_buffers.clear();
    g_buffer_bytes = 0;
    found = g_buffers.end();
    reason = 4;
  }
  g_converts.count[reason]++;
  g_converts.bytes[reason] += metadata->guest_size;
  ConvertedBuffer& entry = found != g_buffers.end() ? found->second : g_buffers[key];
  if (!entry.id) entry.id = ++g_buffer_id;
  g_buffer_bytes -= std::min(g_buffer_bytes, entry.bytes.size());
  entry.address = metadata->guest_address;
  entry.size = metadata->guest_size;
  entry.flags = flags;
  entry.unlocks = g_unlocks[handle];
  entry.checked_frame = g_frame_number;
  entry.content = ContentSample(entry.address, entry.size, true);
  entry.serial = ++g_buffer_serial;
  const int64_t convert_start = NowNs();
  struct ConvertTime {
    int64_t start;
    size_t size;
    ~ConvertTime() {
      g_frame_work.converts++;
      g_frame_work.convert_ns += NowNs() - start;
      g_frame_work.convert_bytes += size;
    }
  } convert_time{convert_start, entry.size};
  entry.bytes.resize(entry.size);
  // (a virtual address, as the native renderer reads it: GTA IV keeps some
  // geometry - the sky dome - in ordinary virtual memory, which the
  // physical window does not alias)
  const uint8_t* source = g_memory->TranslateVirtual<const uint8_t*>(entry.address);
  if (!source) return nullptr;
  if (index) {
    if (flags & 0x80000000u) {
      for (size_t i = 0; i + 4 <= entry.size; i += 4) {
        const uint32_t value = LoadBE32(source + i) & xenos::kVertexIndexMask;
        std::memcpy(entry.bytes.data() + i, &value, 4);
      }
    } else {
      for (size_t i = 0; i + 2 <= entry.size; i += 2) {
        entry.bytes[i] = source[i + 1];
        entry.bytes[i + 1] = source[i];
      }
    }
  } else {
    native::core::ConvertGuestVertexPayload(entry.bytes.data(), source, entry.size,
                                            g_decl_views[declaration], FloatInputs(), stream, offset,
                                            stride);
  }
  g_buffer_bytes += entry.size;
  return &entry;
}

WebBuffer Describe(const ConvertedBuffer& buffer) {
  return {reinterpret_cast<uint64_t>(buffer.bytes.data()), buffer.id, buffer.serial,
          uint32_t(buffer.bytes.size()), 0, 0, 0};
}

// Immediate-mode vertices (DrawPrimitiveUp) for stream 0, when set.
struct UpVertices {
  uint32_t address = 0, size = 0, stride = 0;
};

void RecordDraw(uint32_t device_address, uint32_t primitive, uint32_t first, uint32_t count,
                int32_t base_vertex, bool indexed, bool restart, uint32_t restart_value,
                const native::LightingContext& lighting, UpVertices up = {}) {
  if (!count || g_draws.size() >= kMaxDraws) return;
  const auto declaration = g_decl_slots.find(g_declaration);
  const auto vs = g_shader_hashes.find(g_vertex_shader);
  const auto ps = g_shader_hashes.find(g_pixel_shader);
  if (declaration == g_decl_slots.end() || vs == g_shader_hashes.end()) {
    static int logged = 0;
    if (logged++ < 10) {
      REXLOG_INFO("web draw skipped: declaration {:08X} {} vertex shader {:08X} {}", g_declaration,
                  declaration != g_decl_slots.end() ? "known" : "unknown", g_vertex_shader,
                  vs != g_shader_hashes.end() ? "known" : "unknown");
    }
    return;
  }
  const uint8_t* device = g_memory->TranslateVirtual<const uint8_t*>(device_address);
  const std::span<const uint8_t> snapshot(device, native::kGuestDeviceSize);
  const auto fixed = native::core::DecodeFixedState(snapshot);
  WebDraw draw = BoundEvent(kWebDraw);
  // Blending per target whose blend control isn't the identity (the native
  // renderer's NativeBlendEnableMask: the D3D enable shadow is not consulted).
  for (int i = 0; i < 4; i++) {
    draw.blend_controls[i] =
        native::IsNativeBlendControlEnabled(fixed.blend_controls[i]) ? fixed.blend_controls[i] | 0x80000000u : 0;
  }
  draw.vs_hash = vs->second;
  draw.ps_hash = ps != g_shader_hashes.end() ? ps->second : 0;
  draw.decl = declaration->second;
  draw.stage = uint32_t(lighting.stage);
  draw.blend_control = draw.blend_controls[0];
  draw.color_write_mask = fixed.color_write_mask;
  draw.depth_enable = fixed.depth_enable;
  draw.depth_function = fixed.depth_function;
  draw.depth_write = fixed.depth_write_enable;
  draw.cull_mode = fixed.cull_mode;
  // The rest of the fixed-function state, in params (see WebDraw).
  auto stencil_ops = [](uint32_t f, uint32_t fail, uint32_t zfail, uint32_t pass) {
    return (f & 7) | (fail & 7) << 4 | (zfail & 7) << 8 | (pass & 7) << 12;
  };
  draw.params[0] = (fixed.stencil_enable ? 1u : 0u) | (fixed.two_sided_stencil ? 2u : 0u);
  draw.params[1] = stencil_ops(fixed.stencil_function, fixed.stencil_fail, fixed.stencil_depth_fail,
                               fixed.stencil_pass);
  draw.params[2] = stencil_ops(fixed.ccw_stencil_function, fixed.ccw_stencil_fail,
                               fixed.ccw_stencil_depth_fail, fixed.ccw_stencil_pass);
  draw.params[3] = (fixed.stencil_reference & 255) | (fixed.stencil_mask & 255) << 8 |
                   (fixed.stencil_write_mask & 255) << 16;
  draw.params[4] = (fixed.back_stencil_reference & 255) | (fixed.back_stencil_mask & 255) << 8 |
                   (fixed.back_stencil_write_mask & 255) << 16;
  const auto cull = native::DecodeNativeCullRasterState(fixed.cull_mode);
  draw.params[5] = (cull.cull_face == native::NativeCullFace::kFront  ? 1u
                    : cull.cull_face == native::NativeCullFace::kBack ? 2u
                                                                     : 0u) |
                   (cull.front_face_clockwise ? 0x10u : 0u) | (cull.valid() ? 0x100u : 0u);
  draw.params[6] = fixed.scissor_enable;
  for (int i = 0; i < 4; i++) draw.params[7 + i] = uint32_t(fixed.scissor[i]);
  draw.params[11] = fixed.depth_bias_enable ? fixed.depth_bias_bits : 0;
  draw.params[12] = fixed.depth_bias_enable ? fixed.slope_scaled_depth_bias_bits : 0;
  for (int i = 0; i < 4; i++) draw.params[13 + i] = std::bit_cast<uint32_t>(fixed.blend_constants[i]);
  // 18: alpha test (bit 0 requested - enabled with a function other than
  // always - and bits 1-3 the function, which the native renderer passes as
  // the pixel shader's alpha-test specialization), bit 4 alpha to mask, bits
  // 8-23 the sample mask.
  const bool alpha_test = fixed.alpha_test_enable && fixed.alpha_function != uint32_t(xenos::CompareFunction::kAlways);
  draw.params[18] = (alpha_test ? 1u : 0u) | (fixed.alpha_function & 7u) << 1 |
                    (fixed.alpha_to_mask_enable ? 0x10u : 0u) | (fixed.sample_mask & 0xFFFFu) << 8;
  // 17 bit 1: OpenGL-style clip space (z from -w to w; PA_CL_CLIP_CNTL bit 19 clear).
  if (fixed.negative_one_to_one_clip_space) draw.params[17] |= 2;
  for (int i = 0; i < 6; i++) draw.viewport[i] = std::bit_cast<float>(fixed.viewport_bits[i]);
  draw.target_width = g_target_width;
  draw.target_height = g_target_height;
  draw.base_vertex = base_vertex;
  // Constants: vertex and pixel banks, then the shared block the shaders
  // read booleans and fixed-function values from.
  // Constants: three blocks, each stored once while consecutive draws share
  // its bytes (most do: a frame has thousands of draws and few changes).
  // Only the part the draw's shaders read is compared and stored (the page
  // reads no more of it: scene3d.js constantNeed, the same rule).
  auto block = [](ConstantBlock& last, const uint8_t* guest, size_t size, bool swap) {
    if (last.valid && last.raw.size() >= size && !std::memcmp(last.raw.data(), guest, size)) return last.offset;
    last.raw.assign(guest, guest + size);
    last.offset = AppendDrawBytes(nullptr, size);
    if (swap) {
      native::core::CopyGuestWordsToHost(g_draw_bytes.data() + last.offset, guest, size);
    } else {
      std::memcpy(g_draw_bytes.data() + last.offset, guest, size);
    }
    last.valid = true;
    return last.offset;
  };
  // The vertex and pixel banks: only the pieces the draw's shaders read
  // compared and stored; the last block serves while it holds all of them
  // unchanged. (Stored as long as up to the last piece read: the page reads
  // that much, scene3d.js constantNeed.)
  auto bank = [](ConstantBank& last, const uint8_t* guest, uint32_t pieces) {
    pieces = pieces & 0xFFFF ? pieces & 0xFFFF : 1;
    if ((last.pieces & pieces) == pieces) {
      bool same = true;
      for (uint32_t p = pieces; p && same; p &= p - 1) {
        const size_t at = size_t(std::countr_zero(p)) * kConstantPiece;
        same = !std::memcmp(last.raw.data() + at, guest + at, kConstantPiece);
      }
      if (same) return last.offset;
    }
    const size_t size = size_t(32 - std::countl_zero(pieces)) * kConstantPiece;
    last.offset = AppendDrawBytes(nullptr, size);
    for (uint32_t p = pieces; p; p &= p - 1) {
      const size_t at = size_t(std::countr_zero(p)) * kConstantPiece;
      std::memcpy(last.raw.data() + at, guest + at, kConstantPiece);
      native::core::CopyGuestWordsToHost(g_draw_bytes.data() + last.offset + at, guest + at, kConstantPiece);
    }
    last.pieces = pieces;
    return last.offset;
  };
  const ConstantNeed vs_need = NeedOf(draw.vs_hash);
  const ConstantNeed ps_need = draw.ps_hash ? NeedOf(draw.ps_hash) : ConstantNeed{0, 0};
  draw.constants = kOffsetFlag | bank(g_last_vertex_constants, device + kVertexConstantsOffset,
                                      vs_need.vertex | ps_need.vertex);
  draw.ps_constants = kOffsetFlag | bank(g_last_pixel_constants, device + kPixelConstantsOffset,
                                         vs_need.pixel | ps_need.pixel);
  // The texture stages the shaders sample (also for the samplers' LOD bias).
  const uint32_t stages = StageMask(draw.vs_hash) | (draw.ps_hash ? StageMask(draw.ps_hash) : 0);
  native::core::SharedConstants shared{};
  // Each sampled stage's fetch-constant LOD bias, which the translated shaders
  // add in their SampleBias instructions (native: shared constants 0x2F0).
  for (uint32_t stage = 0; stage < std::size(shared.sampler_lod_bias); stage++) {
    if (!(stages & (1u << stage))) continue;
    shared.sampler_lod_bias[stage] = native::NativeSamplerLodBias(ReadFetch(device_address, stage).lod_bias, 15.99f);
  }
  shared.booleans = native::PackNativeShaderBooleans(LoadBE32(device + kVertexBooleansOffset),
                                                     LoadBE32(device + kPixelBooleansOffset));
  shared.half_pixel_offset_x = 1.0f / float(g_target_width);
  shared.half_pixel_offset_y = -1.0f / float(g_target_height);
  for (int i = 0; i < 4; i++) shared.clip_plane[i] = std::bit_cast<float>(fixed.clip_plane_bits[i]);
  shared.clip_plane_enabled = (fixed.user_clip_plane_enable_mask & 1u) != 0;
  shared.alpha_threshold = fixed.alpha_reference;
  shared.alpha_to_mask = fixed.alpha_to_mask;
  shared.fragment_coordinate_scale_x = 1.0f;
  shared.fragment_coordinate_scale_y = 1.0f;
  // The colour output each bound target asks for (RB_COLOR_INFO, the packed
  // word at +28): its exponent bias as a scale, and its format's clamp. The
  // native renderer appends this to every pixel shader (native_color_output_spirv.h);
  // the page appends the same to the WGSL ones.
  for (int i = 0; i < 4; i++)
    shared.color_output[i] = native::NativeColorOutput(g_surfaces[i].address, g_surfaces[i].handle != 0);
  draw.shared_constants = kOffsetFlag | block(g_last_shared_constants, reinterpret_cast<const uint8_t*>(&shared),
                                               sizeof(shared), false);
  // Streams the declaration reads.
  if (up.address) {
    // Converted into this frame's bytes as stream 0.
    const uint8_t* source = g_memory->TranslateVirtual<const uint8_t*>(up.address);
    std::vector<uint8_t> vertices(up.size);
    native::core::ConvertGuestVertexPayload(vertices.data(), source, up.size, g_decl_views[draw.decl],
                                            FloatInputs(), 0, 0, up.stride);
    if (primitive == 8) {
      // A rectangle list: three corners per rectangle, the fourth implied
      // (v1 + v2 - v0, for every float component). Two triangles each.
      std::vector<uint8_t> expanded;
      for (uint32_t r = 0; r + 3 <= count; r += 3) {
        const uint8_t* v[3] = {vertices.data() + size_t(r) * up.stride, vertices.data() + size_t(r + 1) * up.stride,
                               vertices.data() + size_t(r + 2) * up.stride};
        std::vector<uint8_t> v3(v[2], v[2] + up.stride);
        for (const auto& e : g_decl_views[draw.decl].elements) {
          const uint32_t floats = e.type == 0x2C83A4 ? 1 : e.type == 0x2C23A5 ? 2 : e.type == 0x2A23B9 ? 3
                                : e.type == 0x1A23A6 ? 4 : 0;
          if (e.stream != 0 || e.offset + floats * 4 > up.stride) continue;
          for (uint32_t k = 0; k < floats; k++) {
            float a, b, c2;
            std::memcpy(&a, v[0] + e.offset + k * 4, 4);
            std::memcpy(&b, v[1] + e.offset + k * 4, 4);
            std::memcpy(&c2, v[2] + e.offset + k * 4, 4);
            const float d = b + c2 - a;
            std::memcpy(v3.data() + e.offset + k * 4, &d, 4);
          }
        }
        for (const uint8_t* corner : {v[0], v[1], v[2], v[1], static_cast<const uint8_t*>(v3.data()), v[2]}) {
          expanded.insert(expanded.end(), corner, corner + up.stride);
        }
      }
      vertices.swap(expanded);
      count = uint32_t(vertices.size() / up.stride);
      if (!count) return;
    }
    const size_t offset = AppendDrawBytes(vertices.data(), vertices.size());
    draw.streams[0] = {kOffsetFlag | offset, 0, 0, uint32_t(vertices.size()), 0, up.stride, 0};
  }
  for (const auto& element : g_decl_views[draw.decl].elements) {
    if (up.address) break;
    if (element.stream >= kWebDrawStreams || draw.streams[element.stream].data) continue;
    const StreamState& state = g_streams[element.stream];
    if (ConvertedBuffer* buffer = Capture(state.buffer, draw.decl, element.stream, state.offset, state.stride)) {
      draw.streams[element.stream] = Describe(*buffer);
      draw.streams[element.stream].offset = state.offset;
      draw.streams[element.stream].stride = state.stride;
    }
  }
  // Topology; fans, quads and strips with restart become triangle lists.
  const bool strip = primitive == 3 || primitive == 6 || primitive == 8;
  switch (primitive) {
    case 1: draw.topology = 0; break;
    case 2: draw.topology = 1; break;
    case 3: draw.topology = 2; break;
    case 4: case 5: case 13: draw.topology = 3; break;
    case 6: draw.topology = 4; break;
    case 8: draw.topology = up.address ? 3 : 4; break;
    default: return;
  }
  const ConvertedBuffer* index_buffer = nullptr;
  if (indexed) {
    index_buffer = Capture(g_index_buffer, 0, 0, 0, 0);
    if (!index_buffer) return;
  }
  const bool index32 = index_buffer && (index_buffer->flags & 0x80000000u);
  auto read = [&](uint32_t i) -> uint32_t {
    if (!indexed) return first + i;
    if (index32) {
      uint32_t value;
      std::memcpy(&value, index_buffer->bytes.data() + size_t(first + i) * 4, 4);
      return value;
    }
    uint16_t value;
    std::memcpy(&value, index_buffer->bytes.data() + size_t(first + i) * 2, 2);
    return value;
  };
  if (indexed && size_t(first + count) * (index32 ? 4 : 2) > index_buffer->bytes.size()) return;
  if (primitive == 8 && !up.address) {
    // A rectangle list from vertex buffers, as the user-pointer one above:
    // each stream's three corners copied out with the fourth implied
    // (v1 + v2 - v0 for float components, v2's otherwise), two triangles a
    // rectangle. (Drawn as a strip, a rectangle would be one triangle: half
    // of a full-screen pass.)
    const uint32_t rects = count / 3;
    if (!rects) return;
    bool streams_done[kWebDrawStreams] = {};
    for (const auto& element : g_decl_views[draw.decl].elements) {
      const uint32_t s = element.stream;
      if (s >= kWebDrawStreams || streams_done[s]) continue;
      streams_done[s] = true;
      const StreamState& state = g_streams[s];
      const ConvertedBuffer* buffer = Capture(state.buffer, draw.decl, s, state.offset, state.stride);
      if (!buffer || !state.stride) return;
      std::vector<uint8_t> expanded;
      expanded.reserve(size_t(rects) * 6 * state.stride);
      for (uint32_t r = 0; r < rects; r++) {
        const uint8_t* v[3];
        for (uint32_t k = 0; k < 3; k++) {
          const size_t at = size_t(state.offset) + size_t(read(r * 3 + k)) * state.stride;
          if (at + state.stride > buffer->bytes.size()) return;
          v[k] = buffer->bytes.data() + at;
        }
        std::vector<uint8_t> v3(v[2], v[2] + state.stride);
        for (const auto& e : g_decl_views[draw.decl].elements) {
          const uint32_t floats = e.type == 0x2C83A4 ? 1 : e.type == 0x2C23A5 ? 2 : e.type == 0x2A23B9 ? 3
                                : e.type == 0x1A23A6 ? 4 : 0;
          if (e.stream != s || e.offset + floats * 4 > state.stride) continue;
          for (uint32_t k = 0; k < floats; k++) {
            float a, b, c2;
            std::memcpy(&a, v[0] + e.offset + k * 4, 4);
            std::memcpy(&b, v[1] + e.offset + k * 4, 4);
            std::memcpy(&c2, v[2] + e.offset + k * 4, 4);
            const float d = b + c2 - a;
            std::memcpy(v3.data() + e.offset + k * 4, &d, 4);
          }
        }
        for (const uint8_t* corner : {v[0], v[1], v[2], v[1], static_cast<const uint8_t*>(v3.data()), v[2]}) {
          expanded.insert(expanded.end(), corner, corner + state.stride);
        }
      }
      const size_t offset = AppendDrawBytes(expanded.data(), expanded.size());
      draw.streams[s] = {kOffsetFlag | offset, 0, 0, uint32_t(expanded.size()), 0, state.stride, 0};
    }
    draw.topology = 3;
    draw.count = rects * 6;
    draw.first = 0;
  } else if (primitive == 5 || primitive == 13 || (strip && restart && indexed)) {
    std::vector<uint32_t> generated;
    if (primitive == 5) {
      // (a restart index starts a new fan, as the native VisitNativeTriangleFan)
      uint32_t start = 0;
      for (uint32_t i = 0; i < count; i++) {
        if (indexed && restart && read(i) == restart_value) {
          start = i + 1;
          continue;
        }
        if (i >= start + 2) generated.insert(generated.end(), {read(start), read(i - 1), read(i)});
      }
    } else if (primitive == 13) {
      for (uint32_t i = 0; i + 3 < count; i += 4) {
        const uint32_t a = read(i), b = read(i + 1), c2 = read(i + 2), d = read(i + 3);
        generated.insert(generated.end(), {a, b, c2, a, c2, d});
      }
    } else {
      for (uint32_t i = 0; i < count; i++) {
        const uint32_t value = read(i);
        generated.push_back(value == restart_value ? UINT32_MAX : value);
      }
    }
    if (generated.empty()) return;
    const size_t offset = AppendDrawBytes(generated.data(), generated.size() * 4);
    draw.indices = {kOffsetFlag | offset, 0, 0, uint32_t(generated.size() * 4), 0, 0, 0};
    draw.index32 = 1;
    draw.count = uint32_t(generated.size());
    draw.first = 0;
  } else if (indexed) {
    draw.indices = Describe(*index_buffer);
    draw.index32 = index32;
    draw.count = count;
    draw.first = first;
  } else {
    draw.count = count;
    draw.first = first;
  }
  for (uint32_t stage = 0; stage < kWebDrawTextures; stage++) {
    if (!(stages & (1u << stage))) continue;
    draw.textures[stage] = TextureFor3D(device_address, stage);
    // The stage's sampler state, from its fetch constant (D3D's sampler
    // states live there on the Xbox 360).
    const auto fetch = ReadFetch(device_address, stage);
    draw.samplers[stage] = uint32_t(fetch.clamp_x) | uint32_t(fetch.clamp_y) << 3 |
                           uint32_t(fetch.clamp_z) << 6 | uint32_t(fetch.mag_filter) << 9 |
                           uint32_t(fetch.min_filter) << 11 | uint32_t(fetch.mip_filter) << 13 |
                           uint32_t(fetch.aniso_filter) << 15 | uint32_t(fetch.mip_min_level) << 19 |
                           uint32_t(fetch.mip_max_level) << 23 | 0x80000000u;
  }
  // What it writes, for the EDRAM owners: the targets its colour write mask
  // lets through (four bits a target), depth when tested and written, stencil
  // when an operation changes it (as the native renderer counts them).
  uint32_t targets = 0, bound_targets = 0;
  for (int i = 0; i < 4; i++) {
    if ((fixed.color_write_mask >> (4 * i)) & 0xF) targets |= 1u << i;
    if (g_surfaces[i].handle) bound_targets |= 1u << i;
  }
  const bool stencil_writes =
      fixed.stencil_enable && (fixed.stencil_write_mask & 255) &&
      (fixed.stencil_fail || fixed.stencil_depth_fail || fixed.stencil_pass ||
       (fixed.two_sided_stencil && (fixed.ccw_stencil_fail || fixed.ccw_stencil_depth_fail || fixed.ccw_stencil_pass)));
  NoteWrites(targets, (fixed.depth_enable && fixed.depth_write_enable) || stencil_writes);
  // 17 bit 0: no clipping at the near and far planes, depth clamped instead -
  // the guest's clip control asks for it (the sky at reversed Z's far plane),
  // or a two-sided z-fail light volume (as the native renderer does).
  const native::NativeStencilVolumeDepthState volume{
      .depth_enable = fixed.depth_enable,
      .depth_write_enable = fixed.depth_write_enable,
      .stencil_enable = fixed.stencil_enable,
      .two_sided_stencil = fixed.two_sided_stencil,
      .cull_mode = fixed.cull_mode,
      // (written targets that are bound, as AnyNativeColorWriteEnabledForTargets)
      .active_color_write = (targets & bound_targets) != 0,
      .stencil_depth_fail = fixed.stencil_depth_fail,
      .stencil_pass = fixed.stencil_pass,
      .stencil_write_mask = fixed.stencil_write_mask,
      .ccw_stencil_depth_fail = fixed.ccw_stencil_depth_fail,
      .ccw_stencil_pass = fixed.ccw_stencil_pass,
      .back_stencil_write_mask = fixed.back_stencil_write_mask,
  };
  // (only in the light setup phase, as the native renderer gates it)
  // (diagnostics: the first Bink movie draws - the boot logos - with what
  // their texture planes resolved to)
  constexpr uint64_t kBinkPixelShaders[] = {0x9E76B68B60127349ull, 0xA6C9E2B8B2A59D7Aull};
  if (draw.ps_hash == kBinkPixelShaders[0] || draw.ps_hash == kBinkPixelShaders[1]) {
    static int logged = 0;
    if (logged++ < 12) {
      std::string planes;
      for (uint32_t stage = 0; stage < 4; stage++) {
        const auto fetch = ReadFetch(device_address, stage);
        planes += fmt::format(" s{}:{:08X} fmt{} {}x{} tiled{}", stage, draw.textures[stage], uint32_t(fetch.format),
                              fetch.size_2d.width + 1, fetch.size_2d.height + 1, uint32_t(fetch.tiled));
      }
      REXLOG_INFO("web bink draw: frame {} prim {} count {} vp {}x{} rt {:08X} blend {:08X}{}", g_frame_number, primitive,
                  draw.count, draw.viewport[2], draw.viewport[3], draw.colors[0], draw.blend_controls[0], planes);
    }
  }
  if (native::ShouldEnableNativeDepthClampForDraw(fixed.depth_clamp_enable != 0,
                                                  native::IsLocalStencilSetup(lighting), volume)) {
    draw.params[17] |= 1;
  }
  g_draws.push_back(draw);
}

}  // namespace

void SetMemory(memory::Memory* memory) {
  g_memory = memory;
}

void OnTitleCommand(uint32_t type, const void* command, size_t command_size) {
  using T = native::CommandType;
  std::lock_guard lock(g_mutex);
  if (type < 32) g_type_counts[type]++;
  // One splash frame's exact command order (frame 100), for the log.
  static std::string sequence_log;
  if (g_frame_number == 100) {
    std::string item = std::to_string(type);
    if (T(type) == T::kDrawPrimitiveUp && command_size >= sizeof(native::DrawPrimitiveUpCommand)) {
      const auto* draw = static_cast<const native::DrawPrimitiveUpCommand*>(command);
      if (g_memory && draw->vertex_data) {
        const uint8_t* v = g_memory->TranslateVirtual<const uint8_t*>(draw->vertex_data);
        item += fmt::format("(x{:.0f} c{:08X})", std::bit_cast<float>(LoadBE32(v)), LoadBE32(v + 24));
      }
    } else if (T(type) == T::kClear && command_size >= sizeof(native::ClearCommand)) {
      item += fmt::format("(f{:X})", static_cast<const native::ClearCommand*>(command)->flags);
    } else if (T(type) == T::kSetRenderTarget && command_size >= sizeof(native::SetRenderTargetCommand)) {
      item += fmt::format("(i{})", static_cast<const native::SetRenderTargetCommand*>(command)->index);
    }
    sequence_log += " " + item;
  } else if (g_frame_number == 101 && !sequence_log.empty()) {
    REXLOG_INFO("web sequence: frame 100:{}", sequence_log);
    sequence_log.clear();
  }
  switch (T(type)) {
    case T::kClear: {
      if (command_size < sizeof(native::ClearCommand)) return;
      const auto* clear = static_cast<const native::ClearCommand*>(command);
      // (Xbox 360 flags: 1-8 the colour targets, 0x10 depth, 0x20 stencil)
      NoteWrites(clear->flags & 0xF, (clear->flags & 0x30) != 0);
      if (g_draws.size() < kMaxDraws) {
        WebDraw event = BoundEvent(kWebClear);
        event.params[0] = clear->flags;
        std::memcpy(&event.params[1], clear->color_bits, 16);
        const float depth = float(std::bit_cast<double>(clear->depth_bits));
        std::memcpy(&event.params[5], &depth, 4);
        event.params[6] = clear->stencil;
        g_draws.push_back(event);
      }
      // Colour clears (D3DCLEAR_TARGET); the colour is four floats.
      if (!(clear->flags & 1)) return;
      uint8_t rgba[4];
      for (int i = 0; i < 4; i++) rgba[i] = ToByte(std::bit_cast<float>(clear->color_bits[i]));
      g_clear_rgba = uint32_t(rgba[0]) << 24 | uint32_t(rgba[1]) << 16 | uint32_t(rgba[2]) << 8 | 255;
      // A full clear starts a new picture; quads drawn before it are gone.
      g_pending.clear();
      g_cleared = true;
      return;
    }
    case T::kDepthSurfaceHandoff: {
      if (command_size < sizeof(native::DepthSurfaceHandoffCommand)) return;
      const auto* handoff = static_cast<const native::DepthSurfaceHandoffCommand*>(command);
      if (g_draws.size() < kMaxDraws) {
        WebDraw event = BoundEvent(kWebDepthHandoff);
        event.params[1] = handoff->source.handle;
        event.params[2] = PackSize(handoff->source);
        event.params[3] = handoff->destination.handle;
        event.params[4] = PackSize(handoff->destination);
        event.params[5] = uint32_t(handoff->stencil_policy);
        // (the resolved scene depth the copy reads, as the native renderer does:
        // the source surface's handle may not be the one the scene drew with)
        event.params[6] = handoff->source_texture ? ResolveId(handoff->source_texture) : 0;
        g_draws.push_back(event);
      }
      NoteWrite(handoff->destination, true);
      return;
    }
    case T::kResolve: {
      if (command_size < sizeof(native::ResolveCommand)) return;
      const auto* resolve = static_cast<const native::ResolveCommand*>(command);
      xenos::xe_gpu_texture_fetch_t fetch{};
      std::memcpy(&fetch, resolve->destination_fetch, sizeof(fetch));
      if (!fetch.base_address) return;
      if (g_draws.size() < kMaxDraws) {
        WebDraw event = BoundEvent(kWebResolve);
        const auto& source = resolve->source;
        event.params[0] = resolve->flags;
        event.params[1] = source.handle;
        event.params[2] = PackSize(source);
        const native::ResolveRectangle rect = resolve->source_rectangle_valid
            ? resolve->source_rectangle
            : native::ResolveRectangle{0, 0, int32_t(source.width), int32_t(source.height)};
        std::memcpy(&event.params[3], &rect, 16);
        event.params[7] = ResolveId(resolve->destination_texture);
        auto& page = g_resolve_pages[PhysicalPage(fetch.base_address)];
        ((resolve->flags & 7) == 4 ? page.depth : page.color) = event.params[7];
        ((resolve->flags & 7) == 4 ? page.depth_serial : page.color_serial) = ++g_resolve_serial;
        if ((resolve->flags & 7) == 4) page.depth_size = (fetch.size_2d.width + 1) | (fetch.size_2d.height + 1) << 16;
        // A resolve into a front buffer's memory: the picture a present shows.
        event.params[17] = g_front_pages.count(PhysicalPage(fetch.base_address)) ? 1 : 0;
        event.params[8] = (fetch.size_2d.width + 1) | (fetch.size_2d.height + 1) << 16;
        event.params[9] = resolve->destination_point_valid ? uint32_t(resolve->destination_point.x) : 0;
        event.params[10] = resolve->destination_point_valid ? uint32_t(resolve->destination_point.y) : 0;
        std::memcpy(&event.params[11], resolve->clear_color_bits, 16);
        const float depth = float(std::bit_cast<double>(resolve->clear_depth_bits));
        std::memcpy(&event.params[15], &depth, 4);
        event.params[16] = resolve->clear_stencil;
        // A source another surface wrote over through the same EDRAM: 19 that
        // surface, 18 bits 1 and 2: it is twice as wide / tall (the page
        // averages its 2x2 blocks, as a 4x MSAA view's samples).
        // (a depth resolve: the last depth writer there, or a colour one when it
        // wrote later: GTA IV draws its scene depth into a colour target over
        // the depth's EDRAM and resolves that as depth)
        const bool depth_resolve = (resolve->flags & 7) == 4;
        auto w = (depth_resolve ? g_edram_depth_writers : g_edram_writers).find(source.address & 0xFFF);
        auto end = depth_resolve ? g_edram_depth_writers.end() : g_edram_writers.end();
        if (depth_resolve) {
          const auto c = g_edram_writers.find(source.address & 0xFFF);
          if (c != g_edram_writers.end() && (w == end || c->second.serial > w->second.serial)) {
            w = c;
            end = g_edram_writers.end();
          }
        }
        if (w != end && w->second.handle != source.handle && source.width && source.height) {
          const auto own = g_handle_writes.find(source.handle);
          const uint32_t sx = w->second.width / source.width, sy = w->second.height / source.height;
          if ((own == g_handle_writes.end() || own->second < w->second.serial) && sx >= 1 && sx <= 2 &&
              sy >= 1 && sy <= 2 && sx * source.width == w->second.width && sy * source.height == w->second.height) {
            event.params[19] = w->second.handle;
            event.params[18] |= (sx - 1) << 1 | (sy - 1) << 2;
          }
        }
        // Bit 3: a depth destination stores 20e4 float depth (k_24_8_FLOAT)
        // rather than 24-bit unorm; bits 8-15 the destination mip level,
        // 16-18 its cube face (or array slice), bit 19 it is a cube map, bits
        // 20-23 its last mip level.
        if (xenos::TextureFormat(fetch.format) == xenos::TextureFormat::k_24_8_FLOAT) event.params[18] |= 8;
        // bits 24-29: the destination's Xenos texture format, so the page
        // stores the resolve as that format does (8-bit channels exactly: the
        // G-buffer packs bits in them, which 16-bit floats can't hold)
        event.params[18] |= (uint32_t(fetch.format) & 63) << 24;
        event.params[18] |= (resolve->destination_level & 0xFF) << 8 | (resolve->destination_slice_or_face & 7) << 16 |
                            (fetch.dimension == xenos::DataDimension::kCube ? 1u << 19 : 0u) |
                            (uint32_t(fetch.mip_max_level) & 15) << 20;
        // Bit 0: the destination is a normalized format (the page clamps the
        // resolve to 0-1, as the destination's own format would).
        switch (xenos::TextureFormat(fetch.format)) {
          case xenos::TextureFormat::k_8:
          case xenos::TextureFormat::k_1_5_5_5:
          case xenos::TextureFormat::k_5_6_5:
          case xenos::TextureFormat::k_6_5_5:
          case xenos::TextureFormat::k_8_8_8_8:
          case xenos::TextureFormat::k_2_10_10_10:
          case xenos::TextureFormat::k_8_A:
          case xenos::TextureFormat::k_8_B:
          case xenos::TextureFormat::k_8_8:
          case xenos::TextureFormat::k_4_4_4_4:
            event.params[18] |= 1;
            break;
          default:
            break;
        }
        g_draws.push_back(event);
      }
      if (!g_targets.count(PhysicalPage(fetch.base_address)) && g_targets.size() >= kMaxTargets) {
        g_targets.erase(g_targets.begin());
      }
      TargetSnapshot& target = g_targets[PhysicalPage(fetch.base_address)];
      target.width = fetch.size_2d.width + 1;
      target.height = fetch.size_2d.height + 1;
      target.clear_rgba = g_clear_rgba;
      target.quads = g_pending;
      target.serial = ++g_target_serial;
      return;
    }
    case T::kDrawPrimitiveUp: {
      if (command_size < sizeof(native::DrawPrimitiveUpCommand) || !g_memory) return;
      const auto* c = static_cast<const native::DrawPrimitiveUpCommand*>(command);
      RecordQuad(*c);
      // The payload size isn't always filled in: the vertices are what count.
      const uint64_t needed = uint64_t(c->stride) * c->vertex_count;
      const uint32_t size = c->vertex_data_size ? c->vertex_data_size : uint32_t(std::min<uint64_t>(needed, 4u << 20));
      static int rejected = 0;
      if (c->vertex_data && c->stride && size <= (4u << 20) && needed <= size) {
        RecordDraw(c->device, c->primitive_type, 0, c->vertex_count, 0, false, false, 0, c->lighting,
                   {c->vertex_data, size, c->stride});
      } else if (rejected++ < 10) {
        REXLOG_INFO("web draw-up rejected: data {:08X} size {} stride {} count {} prim {}", c->vertex_data,
                    c->vertex_data_size, c->stride, c->vertex_count, c->primitive_type);
      }
      return;
    }
    case T::kSetDepthStencil: {
      if (command_size < sizeof(native::SetDepthStencilCommand)) return;
      g_surfaces[4] = static_cast<const native::SetDepthStencilCommand*>(command)->surface;
      return;
    }
    case T::kUpdateEnvironmentalData: {
      // Diagnostics: the game's frame time step against real time between
      // these updates (once a frame), every 2 s. A step much longer than the
      // real interval means game time (cutscenes) runs fast.
      if (command_size < sizeof(native::UpdateEnvironmentalDataCommand)) return;
      const auto* c = static_cast<const native::UpdateEnvironmentalDataCommand*>(command);
      static double game_seconds = 0, gameplay_seconds = 0;
      static uint32_t updates = 0;
      static uint32_t last_frame = 0, frames = 0;
      static auto window_start = std::chrono::steady_clock::now();
      static uint64_t guest_start = rex::chrono::Clock::QueryGuestTickCount();
      if (std::isfinite(c->data.time_step_seconds)) game_seconds += c->data.time_step_seconds;
      updates++;
      // (the gameplay step the PS5 diagnostics read, once per frame shown)
      if (g_frame_number != last_frame) {
        last_frame = g_frame_number;
        frames++;
        constexpr uint32_t kGameplayTimeStep = 0x82C6C2AC;
        const uint32_t bits = rex::memory::load_and_swap<uint32_t>(g_memory->TranslateVirtual(kGameplayTimeStep));
        float step;
        std::memcpy(&step, &bits, sizeof(step));
        if (std::isfinite(step)) gameplay_seconds += step;
      }
      const auto now = std::chrono::steady_clock::now();
      const double real = std::chrono::duration<double>(now - window_start).count();
      if (real >= 2.0) {
        const uint64_t guest = rex::chrono::Clock::QueryGuestTickCount();
        const double guest_s = double(guest - guest_start) / double(rex::chrono::Clock::guest_tick_frequency());
        REXLOG_INFO("web timing: {} updates, {} frames in {:.2f} s real; guest clock {:.2f} s; environment steps "
                    "{:.2f} s ({:.2f}x), gameplay steps {:.2f} s ({:.2f}x), last step {:.1f} ms",
                    updates, frames, real, guest_s, game_seconds, game_seconds / real, gameplay_seconds,
                    gameplay_seconds / real, c->data.time_step_seconds * 1000);
        game_seconds = gameplay_seconds = 0;
        updates = frames = 0;
        window_start = now;
        guest_start = guest;
      }
      return;
    }
    case T::kRegisterShader: {
      if (command_size < sizeof(native::RegisterShaderCommand)) return;
      const auto* c = static_cast<const native::RegisterShaderCommand*>(command);
      g_shader_hashes[c->shader] = c->hash;
      return;
    }
    case T::kRegisterVertexDeclaration: {
      if (command_size < sizeof(native::RegisterVertexDeclarationCommand)) return;
      const auto* c = static_cast<const native::RegisterVertexDeclarationCommand*>(command);
      auto [slot, added] = g_decl_slots.try_emplace(c->declaration, uint32_t(g_decls.size()));
      if (added) {
        g_decls.emplace_back();
        g_decl_views.emplace_back();
        // The page holds a pointer to the table: keep it from moving.
        if (g_decls.capacity() < 8192) g_decls.reserve(8192);
      }
      WebDecl& decl = g_decls[slot->second];
      DeclarationView& view = g_decl_views[slot->second];
      decl.count = std::min<uint32_t>(c->element_count, native::kMaximumVertexElementCount);
      view.elements.assign(c->elements, c->elements + decl.count);
      for (uint32_t i = 0; i < decl.count; i++) {
        const auto& e = c->elements[i];
        decl.elements[i] = {e.stream, e.offset, e.type,
                            native::core::ConvertVertexUsageToLocation(e.usage, e.usage_index)};
      }
      return;
    }
    case T::kSetVertexShader:
    case T::kSetPixelShader: {
      if (command_size < sizeof(native::SetShaderCommand)) return;
      const auto* c = static_cast<const native::SetShaderCommand*>(command);
      (T(type) == T::kSetVertexShader ? g_vertex_shader : g_pixel_shader) = c->shader;
      return;
    }
    case T::kSetVertexDeclaration: {
      if (command_size < sizeof(native::SetVertexDeclarationCommand)) return;
      g_declaration = static_cast<const native::SetVertexDeclarationCommand*>(command)->declaration;
      return;
    }
    case T::kSetVertexStream: {
      if (command_size < sizeof(native::SetVertexStreamCommand)) return;
      const auto* c = static_cast<const native::SetVertexStreamCommand*>(command);
      if (c->stream < native::kVertexStreamCount) g_streams[c->stream] = {c->buffer, c->offset, c->stride};
      return;
    }
    case T::kSetIndexBuffer: {
      if (command_size < sizeof(native::SetIndexBufferCommand)) return;
      g_index_buffer = static_cast<const native::SetIndexBufferCommand*>(command)->buffer;
      return;
    }
    case T::kResourceUnlock: {
      if (command_size < sizeof(native::ResourceUnlockCommand)) return;
      g_unlocks[static_cast<const native::ResourceUnlockCommand*>(command)->resource]++;
      return;
    }
    case T::kSetRenderTarget: {
      if (command_size < sizeof(native::SetRenderTargetCommand)) return;
      const auto* c = static_cast<const native::SetRenderTargetCommand*>(command);
      if (c->index < 4) g_surfaces[c->index] = c->surface;
      if (c->index == 0 && c->surface.width && c->surface.height) {
        g_target_width = c->surface.width;
        g_target_height = c->surface.height;
      }
      return;
    }
    case T::kDrawPrimitive: {
      if (command_size < sizeof(native::DrawPrimitiveCommand) || !g_memory) return;
      const auto* c = static_cast<const native::DrawPrimitiveCommand*>(command);
      RecordDraw(c->device, c->primitive_type, c->start_vertex, c->vertex_count, 0, false, false, 0,
                 c->lighting);
      return;
    }
    case T::kDrawIndexedPrimitive: {
      if (command_size < sizeof(native::DrawIndexedPrimitiveCommand) || !g_memory) return;
      const auto* c = static_cast<const native::DrawIndexedPrimitiveCommand*>(command);
      RecordDraw(c->device, c->primitive_type, c->start_index, c->index_count, c->base_vertex, true,
                 c->primitive_restart_enabled != 0, c->primitive_restart_index, c->lighting);
      return;
    }
    default:
      return;
  }
}

void SetControlsPicture(int picture) {
  if (g_controls_wanted.exchange(picture ? 1 : 0, std::memory_order_relaxed) != (picture ? 1 : 0))
    REXLOG_INFO("web controls picture: now {}", picture ? "in a vehicle" : "on foot");
}

// (the picture asked for over the loaded one, and its slot decoded again)
void SwapControlsPicture() {
  const int wanted = g_controls_wanted.load(std::memory_order_relaxed);
  if (!g_controls.address || g_controls.shown == wanted) return;
  uint8_t* bytes = g_memory->TranslatePhysical<uint8_t*>(g_controls.address);
  // (still the texture it was: a picture's bytes, not whatever reused them)
  if (ControlsHash(bytes) != g_controls.hashes[g_controls.shown]) {
    REXLOG_INFO("web controls picture: the texture at {:08X} changed; looking for it again", g_controls.address);
    g_controls.address = 0;
    return;
  }
  std::memcpy(bytes, g_controls.pictures[wanted].data(), g_controls.pictures[wanted].size());
  g_controls.shown = wanted;
  REXLOG_INFO("web controls picture: showing the {} picture", wanted ? "vehicle" : "on-foot");
  for (uint32_t i = 0; i < g_texture_count; i++) {
    if (g_texture_slots[i].key && g_texture_slots[i].address == g_controls.address) {
      g_texture_slots[i].content = 0;      // (redecoded when next drawn)
      g_texture_slots[i].decoded_frame = 0;
    }
  }
}

void OnPresent(const void* present_command, size_t command_size) {
  std::lock_guard lock(g_mutex);
  SwapControlsPicture();
  // Diagnostics: which device presents, with how many 3D events and which
  // front buffer (the first 40 presents after the first 3D frame).
  if (command_size >= sizeof(native::PresentCommand)) {
    static int logged = 0;
    const auto* p = static_cast<const native::PresentCommand*>(present_command);
    if (g_draws.size() > 500 && logged < 40) {
      logged++;
      std::map<uint32_t, uint32_t> devices;
      for (const auto& e : g_draws) if (e.kind == kWebDraw) devices[e.target_width]++;
      REXLOG_INFO("web present: device {:08X} front {:08X} events {} frame {} submitted {}", p->device,
                  p->frontbuffer_texture, g_draws.size(), g_frame_number, p->submitted_frame);
    }
  }
  // Log when a frame's quad count changes (the first 200 changes).
  static size_t last_count = size_t(-1);
  static int changes = 0;
  if (g_pending.size() != last_count && changes < 200) {
    changes++;
    last_count = g_pending.size();
    std::string types;
    for (uint32_t i = 0; i < 32; i++) {
      if (g_type_counts[i]) types += fmt::format(" t{}={}", i, g_type_counts[i]);
    }
    REXLOG_INFO("web frame: present {} has {} quads{} (commands:{})", g_frame.sequence + 1,
                last_count, g_cleared ? " after a clear" : "", types);
  }
  std::fill(std::begin(g_type_counts), std::end(g_type_counts), 0u);
  if (g_cleared) {
    g_screen.swap(g_pending);
  } else {
    g_screen.insert(g_screen.end(), g_pending.begin(), g_pending.end());
  }
  if (g_screen.size() > kMaxQuads) {
    g_screen.erase(g_screen.begin(), g_screen.end() - kMaxQuads);
  }
  g_pending.clear();
  g_cleared = false;
  g_frame_number++;
  if (g_frame_work.decode_ns + g_frame_work.convert_ns > 20'000'000) {
    const auto& w = g_frame_work;
    REXLOG_INFO("web slow capture: frame {} decoded {} textures ({} KB) in {:.1f} ms, converted {} buffers ({} KB) "
                "in {:.1f} ms",
                g_frame_number, w.decodes, w.decode_bytes >> 10, w.decode_ns / 1e6, w.converts, w.convert_bytes >> 10,
                w.convert_ns / 1e6);
  }
  g_frame_work = {};
  if (g_frame_number % 120 == 0) {
    auto& c = g_converts;
    REXLOG_INFO("web buffers: converted new {} ({} KB), unlocked {} ({} KB), moved {} ({} KB), changed {} ({} KB), "
                "cache full {} ({} KB) in 120 frames; {} cached, {} MB",
                c.count[0], c.bytes[0] >> 10, c.count[1], c.bytes[1] >> 10, c.count[2], c.bytes[2] >> 10,
                c.count[3], c.bytes[3] >> 10, c.count[4], c.bytes[4] >> 10, g_buffers.size(), g_buffer_bytes >> 20);
    c = {};
  }
  // The page reads the latest frame while the game goes on: a frame goes
  // into the other list only when the page isn't reading it - it has
  // finished the latest frame (WebFrame::consumed) or taken the latest one's
  // header and is drawing that (WebFrame::reading) - else this one's 3D
  // draws are dropped. Publishing into the list the page is still reading
  // mixed two frames - the flashes: white or dark screens, other targets, a
  // texture slot's new picture in the radar.
  if (std::atomic_ref<uint32_t>(g_frame.consumed).load(std::memory_order_acquire) != g_frame.sequence &&
      std::atomic_ref<uint32_t>(g_frame.reading).load(std::memory_order_acquire) != g_frame.sequence) {
    g_draws.clear();
    g_draw_bytes.clear();
    g_last_vertex_constants.pieces = g_last_pixel_constants.pieces = 0;
    g_last_shared_constants.valid = false;
    return;
  }
  const int list = g_published_list ^ 1;
  g_published[list] = g_screen;
  g_published_list = list;
  g_frame.clear_rgba = g_clear_rgba;
  g_frame.quad_count = uint32_t(g_published[list].size());
  g_frame.quads = reinterpret_cast<uint64_t>(g_published[list].data());
  g_frame.texture_count = g_texture_count;
  g_frame.textures = reinterpret_cast<uint64_t>(g_textures);
  // Targets: all quads in one array, then each target pointing into it.
  auto& targets = g_published_targets[list];
  auto& target_quads = g_published_target_quads[list];
  targets.clear();
  target_quads.clear();
  for (const auto& [base, target] : g_targets) {
    targets.push_back({base, target.width, target.height, target.clear_rgba,
                       uint32_t(target.quads.size()), target.serial, target_quads.size()});
    target_quads.insert(target_quads.end(), target.quads.begin(), target.quads.end());
  }
  for (auto& target : targets) {
    target.quads = reinterpret_cast<uint64_t>(target_quads.data() + target.quads);
  }
  g_frame.target_count = uint32_t(targets.size());
  g_frame.targets = reinterpret_cast<uint64_t>(targets.data());
  // Draws: offsets into this frame's bytes become pointers.
  g_published_draws[list].swap(g_draws);
  g_published_draw_bytes[list].swap(g_draw_bytes);
  g_draws.clear();
  g_draw_bytes.clear();
  // (the blocks lived in the frame just published)
  g_last_vertex_constants.pieces = g_last_pixel_constants.pieces = 0;
  g_last_shared_constants.valid = false;
  const uint64_t bytes = reinterpret_cast<uint64_t>(g_published_draw_bytes[list].data());
  for (auto& draw : g_published_draws[list]) {
    if (draw.constants & kOffsetFlag) draw.constants = bytes + (draw.constants & ~kOffsetFlag);
    if (draw.ps_constants & kOffsetFlag) draw.ps_constants = bytes + (draw.ps_constants & ~kOffsetFlag);
    if (draw.shared_constants & kOffsetFlag) draw.shared_constants = bytes + (draw.shared_constants & ~kOffsetFlag);
    if (draw.indices.data & kOffsetFlag) draw.indices.data = bytes + (draw.indices.data & ~kOffsetFlag);
    if (draw.streams[0].data & kOffsetFlag) draw.streams[0].data = bytes + (draw.streams[0].data & ~kOffsetFlag);
  }
  g_frame.draw_count = uint32_t(g_published_draws[list].size());
  g_frame.draws = reinterpret_cast<uint64_t>(g_published_draws[list].data());
  g_frame.decl_count = uint32_t(g_decls.size());
  g_frame.decls = reinterpret_cast<uint64_t>(g_decls.data());
  g_frame.screen_base = 0;
  if (command_size >= sizeof(native::PresentCommand)) {
    xenos::xe_gpu_texture_fetch_t fetch{};
    std::memcpy(&fetch, static_cast<const native::PresentCommand*>(present_command)->frontbuffer_fetch,
                sizeof(fetch));
    g_frame.screen_base = PhysicalPage(fetch.base_address);
    const auto* present = static_cast<const native::PresentCommand*>(present_command);
    g_front_pages.insert(PhysicalPage(fetch.base_address));
    const auto by_page = g_resolve_pages.find(PhysicalPage(fetch.base_address));
    const auto front = g_resolve_ids.find(present->frontbuffer_texture);
    g_frame.front_target = by_page != g_resolve_pages.end() && by_page->second.color ? by_page->second.color
                           : front != g_resolve_ids.end() ? front->second : 0;
  }
  std::atomic_ref<uint32_t>(g_frame.sequence).fetch_add(1, std::memory_order_release);
}

}  // namespace rex::graphics::web

// The page's view of the latest frame (index.html reads it every animation
// frame).
// The texture stages a shader samples (bit per stage), from its WGSL.
extern "C" EMSCRIPTEN_KEEPALIVE void rex_web_shader_stages(uint32_t hash_low, uint32_t hash_high,
                                                           uint32_t mask) {
  using namespace rex::graphics::web;
  std::lock_guard lock(g_mutex);
  g_shader_stage_masks[uint64_t(hash_high) << 32 | hash_low] = mask;
}

// The bytes of the vertex and pixel constant banks a shader reads (its
// WGSL's group 0 bindings 0 and 1, scene3d.js constantNeed).
// (as bits, a 256-byte piece each: scene3d.js constantNeed)
extern "C" EMSCRIPTEN_KEEPALIVE void rex_web_shader_constants(uint32_t hash_low, uint32_t hash_high,
                                                              uint32_t vertex_pieces, uint32_t pixel_pieces) {
  using namespace rex::graphics::web;
  std::lock_guard lock(g_mutex);
  g_shader_constant_needs[uint64_t(hash_high) << 32 | hash_low] = {uint16_t(vertex_pieces),
                                                                   uint16_t(pixel_pieces)};
}

// Debug switches: 1 = extra texture formats, 2 = texture byte re-checks,
// 3 = buffer re-checks, 4 = BC blocks to the page, 5 = large float targets decoded.
// Changing either drops the texture cache.
extern "C" EMSCRIPTEN_KEEPALIVE void rex_web_set_option(int option, int value) {
  using namespace rex::graphics::web;
  std::lock_guard lock(g_mutex);
  if (option == 1) g_option_formats = value;
  if (option == 2) g_option_validate = value;
  if (option == 3) g_option_buffers = value;
  if (option == 4) g_option_bc = value;
  if (option == 5) g_option_float_targets = value;
  g_texture_lookup.clear();
  for (auto& slot : g_texture_slots) slot.key = 0;
}

// The game's presents so far (the page's FPS counter: game frames a second).
extern "C" EMSCRIPTEN_KEEPALIVE uint32_t rex_web_present_count() { return rex::graphics::web::g_frame_number; }

// (debugging, from the console: a guest word, big-endian as the game
// stores it, through the virtual mapping; 0 when unmapped)
extern "C" EMSCRIPTEN_KEEPALIVE uint32_t rex_web_peek32(uint32_t address) {
  if (!rex::graphics::web::g_memory) return 0;
  const uint8_t* p = rex::graphics::web::g_memory->TranslateVirtual<const uint8_t*>(address);
  return p ? rex::graphics::web::LoadBE32(p) : 0;
}

extern "C" EMSCRIPTEN_KEEPALIVE const rex::graphics::web::WebFrame* rex_web_frame() {
  return &rex::graphics::web::g_frame;
}
