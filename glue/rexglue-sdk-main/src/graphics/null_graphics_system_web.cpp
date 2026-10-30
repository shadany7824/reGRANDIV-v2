/**
 * @file        graphics/null_graphics_system_web.cpp
 * @brief       [web port] Stand-in renderer until a WebGPU one exists
 *
 * Accepts every set-up call and draws nothing. It speaks GTA IV's native
 * title-command ABI (the game's own D3D calls, forwarded by the glue's
 * hooks), so the game takes its native-renderer path instead of programming
 * the Xbox GPU, which nothing here would answer. Every command is accepted
 * and dropped; queries get neutral answers. Each present completes its frame
 * at once (paced to 60 Hz), as the real renderer does when a frame is done:
 * the game allows only two frames in flight (sub_82A46D70). Commands are
 * counted per type and summarised in the log, and clears and textured draws
 * feed the first-pixels capture the page shows (web_frame_capture.h).
 *
 * @license     BSD 3-Clause License
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include <emscripten/emscripten.h>

#include <rex/cvar.h>
#include <rex/graphics/gta4_native/title_commands.h>

#include "web_frame_capture.h"
#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(web_uncapped_frames, false, "Web",
                    "Complete frames as fast as the game presents them (no 60 Hz pacing), "
                    "to measure the game's own frame rate");
// The PS5 build's draw savings, read by gta4_native_hooks.cpp (the native
// renderer, which defines them there, is not in the web build). Defaults as
// on the PS5: in busy streets about a fifth of the draws are small objects'
// shadows, and the car reflections look the same redrawn every other frame;
// each draw left out saves the capture and the page about 5 us.
REXCVAR_DEFINE_UINT32(gta4_native_shadow_min_indices, 300, "GTA IV/Graphics/Shadows",
                      "Skip indexed draws with fewer indices than this in the shadow map passes "
                      "(0 = draw everything)");
REXCVAR_DEFINE_UINT32(ps5_environment_reflection_every, 2, "PS5",
                      "Redraw the environment reflection map (cars, glass) every Nth frame")
    .range(1, 4);
REXCVAR_DEFINE_UINT32(ps5_water_reflection_every, 1, "PS5", "Redraw the water reflection every Nth frame")
    .range(1, 4);
// (web only: small objects in the water's 320x180 reflection, gta4_native_hooks.cpp)
REXCVAR_DEFINE_UINT32(web_water_reflection_min_indices, 300, "Web",
                      "Skip indexed draws with fewer indices than this in the water reflection "
                      "(0 = draw everything)");

namespace rex::system {
namespace {

namespace native = rex::graphics::gta4_native;

// The device's completed-frame counter (graphics_system.cpp's
// kCompletedFrameOffset): the game compares it with the frame it submitted.
constexpr uint32_t kCompletedFrameOffset = 16552;
constexpr auto kFrameInterval = std::chrono::microseconds(16667);

// Matches the WebGPU default maxTextureDimension2D.
constexpr uint32_t kMaxImageDimension2D = 8192;
constexpr size_t kCommandTypeCount = size_t(native::CommandType::kQueryTemporalUpscaler) + 1;

// Per-frame timings for the page's diagnostics (rex_web_frame_timings): for
// each of the last 256 presents, by present number, the game's own time for
// the frame and the capture's time publishing it, in microseconds.
constexpr uint32_t kTimingFrames = 256;
struct FrameTimings {
  std::atomic<uint32_t> presents{0};
  uint32_t work_us[kTimingFrames] = {};
  uint32_t publish_us[kTimingFrames] = {};
};
FrameTimings g_frame_timings;

class NullGraphicsSystem final : public IGraphicsSystem {
 public:
  X_STATUS SetupPresentation(ui::WindowedAppContext* app_context) override {
    (void)app_context;
    REXLOG_WARN("web: no renderer yet; presentation is disabled");
    return X_STATUS_SUCCESS;
  }

  X_STATUS SetupGuestGpu(runtime::FunctionDispatcher* function_dispatcher,
                         KernelState* kernel_state) override {
    (void)function_dispatcher;
    memory_ = kernel_state ? kernel_state->memory() : nullptr;
    graphics::web::SetMemory(memory_);
    REXLOG_WARN("web: no renderer yet; GTA IV's draw stream is accepted and dropped");
    start_ = std::chrono::steady_clock::now();
    return X_STATUS_SUCCESS;
  }

  bool has_presentation() const override { return false; }

  uint32_t GetTitleCommandAbi(uint32_t title_id) const override {
    return title_id == native::kTitleId ? native::kTitleCommandAbi : 0;
  }

  bool SubmitTitleCommand(uint32_t title_id, uint32_t abi_version, const void* command,
                          size_t command_size) override {
    if (title_id != native::kTitleId || !command) {
      return false;
    }
    if (Count(abi_version, command, command_size) == native::CommandType::kPresent &&
        command_size >= sizeof(native::PresentCommand)) {
      CompleteFrame(*static_cast<const native::PresentCommand*>(command));
    }
    return true;
  }

  bool ExecuteTitleCommand(uint32_t title_id, uint32_t abi_version, const void* command,
                           size_t command_size, void* result, size_t result_size) override {
    if (title_id != native::kTitleId || !command) {
      return false;
    }
    const auto type = Count(abi_version, command, command_size);
    if (result && result_size) {
      std::memset(result, 0, result_size);
    }
    if (type == native::CommandType::kPresent && command_size >= sizeof(native::PresentCommand)) {
      CompleteFrame(*static_cast<const native::PresentCommand*>(command));
    }
    if (type == native::CommandType::kQueryDeviceCapabilities &&
        result_size >= sizeof(native::DeviceCapabilitiesResult)) {
      auto* capabilities = static_cast<native::DeviceCapabilitiesResult*>(result);
      capabilities->max_image_dimension_2d = kMaxImageDimension2D;
    }
    // No temporal upscaler: the game falls back to its own image path.
    return type != native::CommandType::kQueryTemporalUpscaler;
  }

  void Shutdown() override {}

 private:
  // Waits out the rest of a 60 Hz frame, then publishes the frame as done.
  void CompleteFrame(const native::PresentCommand& present) {
    const auto arrived = std::chrono::steady_clock::now();
    // The game's own time for the frame: from the last frame's completion to
    // this present (the pacing sleep below is not the game's).
    const auto work = last_completed_.time_since_epoch().count() ? arrived - last_completed_
                                                                  : std::chrono::steady_clock::duration{};
    if (last_completed_.time_since_epoch().count()) {
      work_total_ += work;
      work_frames_++;
    }
    graphics::web::OnPresent(&present, sizeof(present));
    const auto now = std::chrono::steady_clock::now();
    {
      auto us = [](auto d) {
        return uint32_t(std::min<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(d).count(), UINT32_MAX));
      };
      const uint32_t n = g_frame_timings.presents.load(std::memory_order_relaxed);
      g_frame_timings.work_us[n % kTimingFrames] = us(work);
      g_frame_timings.publish_us[n % kTimingFrames] = us(now - arrived);
      g_frame_timings.presents.store(n + 1, std::memory_order_release);
    }
    if (next_frame_ > now && !REXCVAR_GET(web_uncapped_frames)) {
      std::this_thread::sleep_until(next_frame_);
    }
    next_frame_ = std::max(now, next_frame_) + kFrameInterval;
    last_completed_ = std::chrono::steady_clock::now();
    if (!memory_ || !present.device) {
      return;
    }
    auto* completed = memory_->TranslateVirtual<uint32_t*>(present.device + kCompletedFrameOffset);
    const uint32_t current = __builtin_bswap32(*completed);
    if (int32_t(present.submitted_frame - current) > 0 || current == 0) {
      *completed = __builtin_bswap32(present.submitted_frame);
    }
  }

  // Counts a command and returns its type (or kQueryTemporalUpscaler + 1 for
  // other ABIs, whose envelopes carry trace data).
  native::CommandType Count(uint32_t abi_version, const void* command, size_t command_size) {
    if (abi_version != native::kTitleCommandAbi || command_size < sizeof(native::CommandHeader)) {
      other_abi_.fetch_add(1, std::memory_order_relaxed);
      return native::CommandType(kCommandTypeCount);
    }
    const auto type = static_cast<const native::CommandHeader*>(command)->type;
    graphics::web::OnTitleCommand(uint32_t(type), command, command_size);
    const size_t index = size_t(type);
    if (index >= kCommandTypeCount) {
      return type;
    }
    if (counts_[index].fetch_add(1, std::memory_order_relaxed) == 0) {
      REXLOG_INFO("web: first title command {} ({})", index, Name(type));
    }
    if (type == native::CommandType::kPresent) {
      const uint64_t presents = counts_[index].load(std::memory_order_relaxed);
      if (presents == 1 || presents % 120 == 0) {
        LogSummary(presents);
      }
    }
    return type;
  }

  void LogSummary(uint64_t presents) {
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    std::string line;
    for (size_t i = 0; i < kCommandTypeCount; i++) {
      const uint64_t count = counts_[i].load(std::memory_order_relaxed);
      if (count) {
        line += " " + std::string(Name(native::CommandType(i))) + "=" + std::to_string(count);
      }
    }
    const double work_ms =
        work_frames_ ? std::chrono::duration<double, std::milli>(work_total_).count() / work_frames_ : 0;
    work_total_ = {};
    work_frames_ = 0;
    REXLOG_INFO("web: frame {} at {:.1f} s ({:.1f} frames/s, game {:.2f} ms per frame = {:.0f} fps "
                "possible):{}",
                presents, seconds, double(presents) / std::max(seconds, 0.001), work_ms,
                work_ms > 0 ? 1000.0 / work_ms : 0.0, line);
  }

  static const char* Name(native::CommandType type) {
    using T = native::CommandType;
    switch (type) {
      case T::kDeviceCreated: return "device-created";
      case T::kDeviceDestroyed: return "device-destroyed";
      case T::kRegisterShader: return "register-shader";
      case T::kRegisterVertexDeclaration: return "register-vertex-decl";
      case T::kSetRenderState: return "set-render-state";
      case T::kSetPixelShader: return "set-pixel-shader";
      case T::kSetVertexShader: return "set-vertex-shader";
      case T::kSetVertexDeclaration: return "set-vertex-decl";
      case T::kSetTexture: return "set-texture";
      case T::kResourceUnlock: return "resource-unlock";
      case T::kSetDepthStencil: return "set-depth-stencil";
      case T::kSetRenderTarget: return "set-render-target";
      case T::kSetVertexStream: return "set-vertex-stream";
      case T::kSetIndexBuffer: return "set-index-buffer";
      case T::kDrawPrimitive: return "draw";
      case T::kDrawPrimitiveUp: return "draw-up";
      case T::kDrawIndexedPrimitive: return "draw-indexed";
      case T::kResolve: return "resolve";
      case T::kTextureLock: return "texture-lock";
      case T::kClear: return "clear";
      case T::kRenderPhaseMarker: return "phase-marker";
      case T::kPresent: return "present";
      case T::kQueryDeviceCapabilities: return "query-caps";
      case T::kRegisterReflectionTarget: return "register-reflection";
      case T::kReleaseResource: return "release-resource";
      case T::kUpdateEnvironmentalData: return "environment";
      case T::kDepthSurfaceHandoff: return "depth-handoff";
      case T::kRegisterVirtualResource: return "register-virtual-resource";
      case T::kTemporalUpdate: return "temporal-update";
      case T::kQueryTemporalUpscaler: return "query-upscaler";
    }
    return "unknown";
  }

  std::array<std::atomic<uint64_t>, kCommandTypeCount> counts_{};
  std::atomic<uint64_t> other_abi_{0};
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point next_frame_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point last_completed_{};
  std::chrono::steady_clock::duration work_total_{};
  uint64_t work_frames_ = 0;
  memory::Memory* memory_ = nullptr;
};

}  // namespace
}  // namespace rex::system

// The web build links its renderer in, as the PS5 build does.
extern "C" rex::system::IGraphicsSystem* rex_gpu_create(uint32_t abi_version,
                                                        const rex::system::GpuCreateInfo* info) {
  (void)info;
  if (abi_version != rex::system::kGpuPluginAbiVersion) {
    return nullptr;
  }
  return new rex::system::NullGraphicsSystem();
}

// [web port] The page's diagnostics: the address of the per-frame timings
// (u32 presents, then u32 work_us[256], then u32 publish_us[256]).
extern "C" EMSCRIPTEN_KEEPALIVE void* rex_web_frame_timings() {
  return &rex::system::g_frame_timings;
}
