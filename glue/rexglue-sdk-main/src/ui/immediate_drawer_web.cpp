/**
 * @file        ui/immediate_drawer_web.cpp
 * @brief       [web port] Immediate drawer for the browser page
 *
 * See immediate_drawer_web.h. Textures keep their pixels until the page has
 * uploaded them (it uploads each once, by id, and says so:
 * rex_web_ui_texture_uploaded); a frame's vertices, indices and draws are built
 * between Begin and End and published as one of two buffers, so the page
 * reads a whole frame while the next is recorded.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/ui/immediate_drawer_web.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include <emscripten/emscripten.h>

namespace rex::ui {
namespace {

std::mutex g_mutex;

struct TextureData {
  WebUiTexture info{};
  std::vector<uint8_t> pixels;
};
// Live textures by id; the published table lists them.
std::map<uint32_t, TextureData> g_textures;
std::vector<WebUiTexture> g_texture_table[2];
uint32_t g_next_texture_id = 1;
// Ids the page has uploaded (webui.js), their pixels let go at the next End:
// the hub's pictures and ImGui's font atlas are 130 MB of copies otherwise.
constexpr uint32_t kUploadRing = 1024;
std::atomic<uint32_t> g_uploaded[kUploadRing];
std::atomic<uint32_t> g_uploaded_head{0};
uint32_t g_uploaded_tail = 0;

struct FrameData {
  std::vector<ImmediateVertex> vertices;
  std::vector<uint16_t> indices;
  std::vector<WebUiDraw> draws;
};
FrameData g_building, g_published[2];
int g_published_index = 0;
WebUiFrame g_frame{};
// The current batch's start in the frame being built.
uint32_t g_batch_vertex_base = 0, g_batch_index_base = 0;
bool g_batch_indexed = false;

class WebImmediateTexture final : public ImmediateTexture {
 public:
  WebImmediateTexture(uint32_t width, uint32_t height, uint32_t id)
      : ImmediateTexture(width, height), id(id) {}
  ~WebImmediateTexture() override {
    std::lock_guard lock(g_mutex);
    g_textures.erase(id);
  }
  const uint32_t id;
};

}  // namespace

std::unique_ptr<ImmediateTexture> WebImmediateDrawer::CreateTexture(uint32_t width, uint32_t height,
                                                                    ImmediateTextureFilter filter,
                                                                    bool is_repeated,
                                                                    const uint8_t* data) {
  (void)is_repeated;
  std::lock_guard lock(g_mutex);
  const uint32_t id = g_next_texture_id++;
  TextureData& t = g_textures[id];
  t.pixels.assign(size_t(width) * height * 4, 0);
  if (data) std::memcpy(t.pixels.data(), data, t.pixels.size());
  t.info = {id, width, height, filter == ImmediateTextureFilter::kLinear ? 1u : 0u,
            reinterpret_cast<uint64_t>(t.pixels.data())};
  return std::make_unique<WebImmediateTexture>(width, height, id);
}

void WebImmediateDrawer::Begin(UIDrawContext& ui_draw_context, float coordinate_space_width,
                               float coordinate_space_height) {
  ImmediateDrawer::Begin(ui_draw_context, coordinate_space_width, coordinate_space_height);
  g_building.vertices.clear();
  g_building.indices.clear();
  g_building.draws.clear();
}

void WebImmediateDrawer::BeginDrawBatch(const ImmediateDrawBatch& batch) {
  g_batch_vertex_base = uint32_t(g_building.vertices.size());
  g_batch_index_base = uint32_t(g_building.indices.size());
  g_batch_indexed = batch.indices != nullptr;
  if (batch.vertices && batch.vertex_count > 0) {
    g_building.vertices.insert(g_building.vertices.end(), batch.vertices,
                               batch.vertices + batch.vertex_count);
  }
  if (batch.indices && batch.index_count > 0) {
    g_building.indices.insert(g_building.indices.end(), batch.indices,
                              batch.indices + batch.index_count);
  }
}

void WebImmediateDrawer::Draw(const ImmediateDraw& draw) {
  if (draw.count <= 0) return;
  WebUiDraw d{};
  d.texture = draw.texture ? static_cast<WebImmediateTexture*>(draw.texture)->id : 0;
  d.lines = draw.primitive_type == ImmediatePrimitiveType::kLines ? 1 : 0;
  d.count = uint32_t(draw.count);
  if (g_batch_indexed) {
    d.index_offset = g_batch_index_base + uint32_t(draw.index_offset);
    d.base_vertex = int32_t(g_batch_vertex_base) + draw.base_vertex;
  } else {
    // Unindexed: indices of its own, 0..count-1 from the draw's first vertex.
    d.index_offset = uint32_t(g_building.indices.size());
    d.base_vertex = int32_t(g_batch_vertex_base) + draw.index_offset;
    for (int i = 0; i < draw.count; i++) g_building.indices.push_back(uint16_t(i));
  }
  d.scissor = draw.scissor ? 1 : 0;
  d.scissor_rect[0] = draw.scissor_left;
  d.scissor_rect[1] = draw.scissor_top;
  d.scissor_rect[2] = draw.scissor_right;
  d.scissor_rect[3] = draw.scissor_bottom;
  g_building.draws.push_back(d);
}

void WebImmediateDrawer::EndDrawBatch() {}

void WebImmediateDrawer::End() {
  {
    std::lock_guard lock(g_mutex);
    const int index = g_published_index ^ 1;
    std::swap(g_published[index], g_building);
    for (; g_uploaded_tail != g_uploaded_head.load(std::memory_order_acquire); g_uploaded_tail++) {
      const uint32_t id = g_uploaded[g_uploaded_tail % kUploadRing].exchange(0, std::memory_order_acq_rel);
      if (auto it = g_textures.find(id); it != g_textures.end()) {
        std::vector<uint8_t>().swap(it->second.pixels);
        it->second.info.pixels = 0;
      }
    }
    auto& table = g_texture_table[index];
    table.clear();
    for (const auto& [id, t] : g_textures) table.push_back(t.info);
    const FrameData& f = g_published[index];
    g_frame.vertex_count = uint32_t(f.vertices.size());
    g_frame.index_count = uint32_t(f.indices.size());
    g_frame.draw_count = uint32_t(f.draws.size());
    g_frame.vertices = reinterpret_cast<uint64_t>(f.vertices.data());
    g_frame.indices = reinterpret_cast<uint64_t>(f.indices.data());
    g_frame.draws = reinterpret_cast<uint64_t>(f.draws.data());
    g_frame.width = coordinate_space_width();
    g_frame.height = coordinate_space_height();
    g_frame.texture_count = uint32_t(table.size());
    g_frame.textures = reinterpret_cast<uint64_t>(table.data());
    g_published_index = index;
    std::atomic_ref<uint32_t>(g_frame.sequence).fetch_add(1, std::memory_order_release);
  }
  ImmediateDrawer::End();
}

}  // namespace rex::ui

// The page has a texture on the GPU: its pixels can go (at the next End).
extern "C" EMSCRIPTEN_KEEPALIVE void rex_web_ui_texture_uploaded(uint32_t id) {
  using namespace rex::ui;
  const uint32_t at = g_uploaded_head.load(std::memory_order_relaxed);
  g_uploaded[at % kUploadRing].store(id, std::memory_order_relaxed);
  g_uploaded_head.store(at + 1, std::memory_order_release);
}

// The page's view of the latest overlay frame.
extern "C" EMSCRIPTEN_KEEPALIVE const rex::ui::WebUiFrame* rex_web_ui_frame() {
  return &rex::ui::g_frame;
}
