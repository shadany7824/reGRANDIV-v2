// GTA IV PS5 - the hub (see hub_ui.h): the frame, the Home, Settings,
// Saves and About pages, and the controls.
#include "hub_ui.h"

#include <sys/stat.h>

#include <rex/platform.h>
#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <utility>

#include "hub_lang.h"
#include "hub_style.h"

namespace gta4ps5::hub {

namespace fs = std::filesystem;
using lang::F;
using lang::T;

static const char* const kTabNames[kTabCount] = {"Home", "Display", "Game", "Episodes", "Saves", "Trophies", "About", "Debug"};

namespace {

std::string Unquote(std::string v) {
  if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
  return v;
}

// A folder's entries (none when it can't be read).
std::vector<fs::directory_entry> ListDir(const fs::path& dir) {
  std::vector<fs::directory_entry> out;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) out.push_back(*it);
  return out;
}

bool Hex(const std::string& s, size_t n) {
  if (s.size() != n) return false;
  for (char c : s)
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
  return true;
}

int64_t Modified(const fs::path& p) {
  struct stat st {};
  return stat(p.string().c_str(), &st) == 0 ? int64_t(st.st_mtime) : 0;
}

// The newest change in a folder (its files').
int64_t NewestIn(const fs::path& dir) {
  int64_t t = Modified(dir);
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    if (it->is_regular_file(ec)) t = std::max(t, Modified(it->path()));
  return t;
}

std::string Date(int64_t t) { return lang::Date(t); }

// The profile folders in UserData (16 hex digits: the game's saves are in them).
// The Saves page's first rows: Back Up Now, Import Xbox 360 Saves (and in the
// browser Download Saves, Load Saves From a File), then the backups and saves.
#if defined(__EMSCRIPTEN__)
constexpr int kSaveActions = 4;
#else
constexpr int kSaveActions = 2;
#endif

std::vector<fs::path> Profiles(const fs::path& user_data) {
  std::vector<fs::path> out;
  std::error_code ec;
  for (const auto& e : ListDir(user_data))
    if (e.is_directory(ec) && Hex(e.path().filename().string(), 16)) out.push_back(e.path());
  return out;
}

// Copies a folder's contents (recursively) into another. False on any error.
bool CopyInto(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  fs::create_directories(to, ec);
  if (ec) return false;
  for (const auto& e : ListDir(from)) {
    std::error_code ec2;
    fs::copy(e.path(), to / e.path().filename(), fs::copy_options::recursive | fs::copy_options::overwrite_existing,
             ec2);
    if (ec2) return false;
  }
  return true;
}

}  // namespace

// ----------------------------------------------------------------- settings

struct Hub::Setting {
  const char* section;
  const char* key;
  const char* label;
  const char* short_label;  // (Home's numbers; null: not shown there)
  const char* help;
  bool quoted;                      // a TOML string
  std::vector<const char*> values;  // as written to the file
  std::vector<const char*> labels;
  const char* fallback;             // the value when the file has none
};

// What the PC launcher's Video, Language and Controls pages had that matters on
// the PS5. The PS5 always sends 4K to the TV.
static const std::vector<Hub::Setting>& Settings() {
  static const std::vector<Hub::Setting> rows = {
      // (not a setting of its own: what the ones it sets match, PresetValues)
      {"DISPLAY", "hub_display_preset", "Preset", nullptr,
       "Sets the Display settings below in one go, all with a 60 FPS limit. Recommended is the game as it "
       "comes, in 4K. Performance draws at 1440p with fewer cars and people, for the steadiest 60. Quality "
       "adds sharper and longer shadows, more detail in the distance, SMAA and the enhanced effects, and can "
       "drop below 60 in busy streets. Changing any of them makes it Custom.",
       false, {"recommended", "performance", "quality", "custom"}, {"Recommended", "Performance", "Quality", "Custom"},
       "recommended"},
      {"DISPLAY", "resolution", "Resolution", "RESOLUTION",
       "How sharp the game is drawn. 720p is the Xbox 360's. The PS5's graphics card has room for 4K; "
       "lower only helps if the picture stutters.",
       true, {"720p", "1080p", "1440p", "4k"}, {"720p (Original)", "1080p", "1440p", "4K"}, "4k"},
      // (switched right away, and when the app starts: game/ps5_display.cpp)
      {"DISPLAY", "ps5_refresh_rate", "Refresh Rate", nullptr,
       "How many times a second the TV shows a new picture. 120 Hz needs a TV that takes it and 120 Hz output "
       "on in the PS5's settings; without them the game stays at 60 Hz. It's what lets the 40 and 120 FPS "
       "limits run evenly. It switches as soon as you choose it.",
       false, {"60", "120"}, {"60 Hz", "120 Hz"}, "60"},
      {"DISPLAY", "gta4_draw_distance_scale", "Draw Distance", nullptr,
       "How far out buildings and the city are drawn in detail. Original is the Xbox 360's. Shorter draws "
       "less of the city and runs faster in busy streets, with more pop-in. Farther costs frame rate, and "
       "past 1.5x the game can run short of memory.",
       false, {"0.75", "1.0", "1.5", "2.0", "3.0"}, {"Shorter", "Original", "1.5x", "2x", "3x"}, "1.0"},
      // (the game's four population scales: gta4_ps5_app.cpp)
      {"DISPLAY", "ps5_population", "City Population", nullptr,
       "How many cars and people are out in the city. Original is the Xbox 360's. Fewer takes load off the game "
       "in busy streets and helps it hold its frame rate; More fills the streets and costs frame rate.",
       false, {"0.5", "0.75", "1.0", "1.25"}, {"Fewest", "Fewer", "Original", "More"}, "1.0"},
      {"DISPLAY", "gta4_force_highest_lod", "Highest Detail Models", nullptr,
       "Shows buildings, cars and people at their most detailed as soon as that detail has loaded, near or "
       "far, instead of simpler versions in the distance. Can cost frame rate in busy streets.",
       false, {"false", "true"}, {"Off", "On"}, "false"},
      {"DISPLAY", "gta4_frame_limit", "Frame Limit", nullptr,
       "The most frames a second the game draws. 30 is what the Xbox 360 aimed for and holds steady "
       "everywhere; 60 runs at 50 to 60 in the busiest streets. 40 and 120 are for a 120 Hz Refresh Rate: "
       "40 holds steady and is smoother than 30; 120 is only reached in quieter places.",
       false, {"30", "40", "60", "120"}, {"30 FPS", "40 FPS", "60 FPS", "120 FPS"}, "60"},
      // (game/ps5_overlay.cpp)
      {"DISPLAY", "ps5_fps_counter", "FPS Counter", nullptr,
       "Shows the frame rate at the left edge of the screen while you play. Simple is frames a second and the "
       "frame time. Detailed adds the slowest frames, a graph of the last ones, the resolution, the TV's refresh "
       "rate, the frame limit and how long the renderer takes.",
       true, {"false", "simple", "true"}, {"Off", "Simple", "Detailed"}, "false"},
// (Frame Generation: off and its row gone until it makes frames on the PS5;
// it broke the radar and made no frames in 1.0.1's tests)
#if LIBERTY_HAS_FSR3 && LIBERTY_PS5_FRAME_GENERATION
      {"DISPLAY", "ps5_frame_generation", "Frame Generation", nullptr,
       "AMD FSR 3 adds a frame between each two the game draws. 30 to 60 holds the game at 30 FPS and shows "
       "60, on any TV. 60 to 120 needs a 120 Hz TV and switches to it: the game runs at 60 and shows 120, "
       "and drops to 30 to 60 by itself in places it can't hold 60. Frame Limit doesn't apply while it's on. "
       "Adds a little input lag and turns on FSR 3 anti-aliasing. Experimental: tell us how it feels.",
       true, {"off", "30-60", "60-120"}, {"Off", "30 to 60", "60 to 120"}, "off"},
#endif
      {"DISPLAY", "gta4_native_anti_aliasing", "Anti-Aliasing", nullptr,
       "Smooths jagged edges. FXAA is the lightest; SMAA is sharper and costs a little more. MSAA 4x + SMAA "
       "also cleans up thin edges in the distance, like stairs and wires, but is heavy on the graphics card: "
       "new on the PS5, try 1440p with it if the game slows down.",
       true, {"off", "fxaa", "smaa", "msaa4x_smaa"}, {"Off", "FXAA", "SMAA", "MSAA 4x + SMAA"}, "fxaa"},
      {"DISPLAY", "gta4_anisotropic_filtering", "Texture Filtering", nullptr,
       "Keeps textures seen at an angle sharp, like roads and pavements going into the distance. Original is "
       "the Xbox 360's; 16x is the sharpest and costs very little.",
       true, {"1x", "4x", "8x", "16x"}, {"Original", "4x", "8x", "16x"}, "1x"},
      // (FusionFix's shaders, built in: LibertyRecompLib/shader_overrides)
      {"DISPLAY", "gta4_modern_shaders", "Enhanced Effects", nullptr,
       "The shaders from FusionFix, the PC mod: depth of field, a better sky, bloom and fog, and nicer water "
       "and light beams. Off is the Xbox 360's look. New on the PS5: tell us if something looks wrong.",
       false, {"false", "true"}, {"Off", "On"}, "false"},
      {"DISPLAY", "gta4_motion_blur", "Motion Blur", nullptr, "The blur when driving fast or turning quickly.",
       false, {"false", "true"}, {"Off", "On"}, "true"},
      {"DISPLAY", "gta4_disable_tlad_film_grain", "Film Grain", nullptr,
       "The grainy, film-like look of The Lost and Damned. Off gives it a clean picture; GTA IV and The "
       "Ballad of Gay Tony have none.",
       false, {"false", "true"}, {"On", "Off"}, "false"},
      {"DISPLAY", "gta4_reflection_resolution", "Reflections", nullptr,
       "How sharp water, mirror and car reflections are. Original is the Xbox 360's; 1080p is cleaner.",
       true, {"original", "1080p"}, {"Original", "1080p"}, "1080p"},
      // (the Xbox 360's is 256)
      {"DISPLAY", "gta4_shadow_map_base_size", "Shadow Quality", nullptr,
       "How sharp shadows are. High is twice the Xbox 360's detail; Very High doubles it again and asks more "
       "of the graphics card.",
       false, {"512", "1024"}, {"High", "Very High"}, "512"},
      {"DISPLAY", "gta4_shadow_distance_scale", "Shadow Distance", nullptr,
       "How far out shadows are drawn. Original is the Xbox 360's; 2x reaches further but costs frame "
       "rate in the city.",
       false, {"1.0", "2.0"}, {"Original", "2x"}, "1.0"},
      {"DISPLAY", "gta4_native_shadow_min_indices", "Small Object Shadows", nullptr,
       "Shadows of small things like signs, bins and debris, about a fifth of what is drawn in "
       "busy streets. Off skips them for a higher frame rate.",
       false, {"0", "300"}, {"On", "Off"}, "300"},
      {"DISPLAY", "ps5_environment_reflection_every", "Car Reflections", nullptr,
       "How often the reflections on cars and glass are redrawn. Every other frame looks the same "
       "in motion and leaves more time for the rest of the frame.",
       false, {"1", "2"}, {"Every Frame", "Every Other Frame"}, "2"},
      // (Water Reflections every other frame is held back: the city froze
      // loading with it on, 0.5b7. gta4_native_hooks.cpp ignores the setting.)
      // (the hub's language too; the game is told it as the Xbox 360's language:
      // gta4_ps5_app.cpp)
      {"GAME", "ps5_language", "Language", nullptr,
       "The language of the game's text (menus, subtitles and missions) and of this hub. Auto follows the PS5's "
       "language. Voices stay in English, as on the Xbox 360.",
       true, {"auto", "english", "french", "german", "spanish", "italian"},
       {"Auto", "English", "Fran\u00E7ais", "Deutsch", "Espa\u00F1ol", "Italiano"}, "auto"},
#if !REX_PLATFORM_PS5
      // (rpf_button_prompts.cpp: which pictures the game's button prompts use)
      {"GAME", "gta4_button_prompts", "Button Prompts", nullptr,
       "The pictures in the game's help messages. Auto uses the controller you play with, or the keyboard's "
       "keys when there is none. Applies when the game starts.",
       true, {"auto", "keyboard", "xbox360", "xbox_series_x", "ps5", "switch"},
       {"Auto", "Keyboard & Mouse", "Xbox 360", "Xbox Series", "PlayStation", "Switch"}, "auto"},
#endif
      {"GAME", "gta4_skip_intro", "Skip Intro", nullptr,
       "Goes straight to the game's start screen, past the logos and the legal screens.", false,
       {"false", "true"}, {"Off", "On"}, "false"},
      // (the car camera's sideways offset: gta4_camera_hooks.cpp)
      {"GAME", "gta4_centered_vehicle_camera", "Centered Car Camera", nullptr,
       "Puts the camera straight behind the car instead of a little off to the side, like FusionFix's option "
       "on PC. Off is the Xbox 360's.",
       false, {"false", "true"}, {"Off", "On"}, "false"},
      // (FusionFix's centering delay and turn speed: gta4_camera_hooks.cpp)
      {"GAME", "gta4_vehicle_camera_return_delay", "Car Camera Return", nullptr,
       "How long the car camera stays where you pointed it with the right stick before it swings back "
       "behind the car. Original is the Xbox 360's: it starts swinging back right away.",
       false, {"0", "1000", "2000", "3000"}, {"Original", "After 1 Second", "After 2 Seconds", "After 3 Seconds"},
       "0"},
      {"GAME", "gta4_vehicle_camera_turn_speed", "Car Camera Follow Speed", nullptr,
       "How quickly the car camera swings round to follow the car through turns and back behind it. "
       "Original is the Xbox 360's.",
       false, {"0", "2", "4", "6"}, {"Original", "Faster", "Much Faster", "Fastest"}, "0"},
      {"CONTROLS", "gta4_sony_enabled", "DualSense Features", nullptr,
       "Turns the controller extras below on or off together: the light bar, adaptive triggers and extra rumble.",
       false, {"false", "true"}, {"Off", "On"}, "true"},
      // (rex::input::sony, the PC port's, through the PS5 driver: ps5_input_driver.cpp)
      {"CONTROLS", "gta4_sony_lighting", "Light Bar", nullptr,
       "The controller's light: blue in GTA IV, and flashing red and blue like police lights while you're wanted.",
       false, {"false", "true"}, {"Off", "On"}, "true"},
      {"CONTROLS", "gta4_sony_triggers", "Adaptive Triggers", nullptr,
       "R2 pulls like a real gun's trigger and kicks with each shot, L2 pushes back while you aim, and both feel "
       "like pedals when you drive.",
       false, {"false", "true"}, {"Off", "On"}, "true"},
      {"CONTROLS", "gta4_sony_rumble", "Extra Rumble", nullptr,
       "Adds rumble when you shoot, get hit or are near an explosion, on top of the game's own.", false,
       {"false", "true"}, {"Off", "On"}, "true"},
      {"CONTROLS", "gta4_sony_intensity", "Feedback Strength", nullptr,
       "How strong the adaptive triggers and the extra rumble feel.", false, {"0.25", "0.5", "0.75", "1.0"},
       {"Light", "Medium", "Strong", "Max"}, "0.5"},
      {"AUDIO", "ps5_audio_frames", "Audio Delay", nullptr,
       "How far the sound runs behind the picture. Lower is closer to the action; if the sound crackles or cuts "
       "out, pick the next one up. Wireless headsets add a little delay of their own.",
       false, {"6", "12", "24", "64"}, {"Lowest (32 ms)", "Low (64 ms)", "Steady (128 ms)", "Original (341 ms)"},
       "12"},
      {"AUDIO", "hub_music_volume", "Menu Music", nullptr,
       "How loud the music is here in the hub. It changes right away.", false,
       {"0", "10", "20", "30", "40", "50", "60", "70", "80", "90", "100"},
       {"Off", "10%", "20%", "30%", "40%", "50%", "60%", "70%", "80%", "90%", "100%"}, "50"},
      {"DEBUG", "ps5_save_logs", "Save Logs", nullptr,
       "Writes what the game does to /app0/logs while it runs. Send these with a bug report. Takes effect "
       "the next time the app starts.",
       false, {"false", "true"}, {"Off", "On"}, "true"},
      {"DEBUG", "log_level", "Log Detail", nullptr,
       "How much goes into the log. Normal is enough for most reports; Detailed and Everything help with "
       "crashes and freezes but make big files and can slow the game.",
       true, {"info", "debug", "trace"}, {"Normal", "Detailed", "Everything"}, "info"},
      {"DEBUG", "ps5_renderer_cpus", "Renderer Threads", nullptr,
       "Own Cores gives the game's render thread and the two renderer threads a CPU core each and "
       "moves everything else off them (about 10% faster than Anywhere). Compare switches between "
       "two versions of Own Cores every 10 seconds and notes each in the log.",
       true, {"cores", "any", "compare"}, {"Own Cores", "Anywhere", "Compare"}, "cores"},
      {"DEBUG", "ps5_lean_capture", "Game Thread Capture", nullptr,
       "How the game's render thread hands each draw to the renderer. Lean holds every buffer and "
       "texture once for two frames instead of once per draw and fetches what a capture reads "
       "ahead of time. Same picture either way. Compare stays on Lean and switches its fetching "
       "ahead off and on every 10 seconds, noting each in the log.",
       true, {"on", "off", "compare"}, {"Lean", "Classic", "Compare"}, "on"},
      {"DEBUG", "ps5_frametime_capture", "Frame Time Capture", nullptr,
       "Records every frame's time to a CSV file in /app0/logs, for checking stutter and slowdowns.", false,
       {"false", "true"}, {"Off", "On"}, "false"},
      {"DEBUG", "ps5_hang_reports", "Freeze Reports", nullptr,
       "When the game stops drawing for a few seconds, writes where every thread is to the log.", false,
       {"false", "true"}, {"Off", "On"}, "true"},
      {"DEBUG", "gta4_native_gpu_detile", "GPU Texture Unpacking", nullptr,
       "Unpacks the game's textures on the graphics card. Off does it on the CPU the old way: slower, "
       "but a test for broken or missing textures.",
       false, {"true", "false"}, {"On", "Off"}, "true"},
      {"DEBUG", "gta4_native_frame_timeline", "Frame Timeline", nullptr,
       "Logs when each stage of every frame finished, to find what holds the frame rate back. The log "
       "grows quickly with it on.",
       false, {"false", "true"}, {"Off", "On"}, "false"},
      // (game/ps5_overlay.cpp: the overlay repainting every vblank, as up to 0.5b6)
      {"DEBUG", "ps5_overlay_every_vblank", "Frame Delivery", nullptr,
       "How the game's frames reach the TV. As Finished sends each frame as soon as it's ready. Every Vblank "
       "redraws the screen at every refresh, as Beta 6 did, with two refreshes more input lag. For comparing "
       "which feels smoother.",
       false, {"false", "true"}, {"As Finished", "Every Vblank"}, "false"},
      {"DEBUG", "gta4_native_texture_watch", "Texture Watch", nullptr,
       "Re-reads textures that change in memory without the game saying so. Off brings back the wrong map and "
       "radar tiles. Only for comparing performance.",
       false, {"true", "false"}, {"On", "Off"}, "true"},
      // (gta4_native/graphics_system.cpp: lights that go dark near the camera)
      {"DEBUG", "ps5_light_test", "Light Test", nullptr,
       "For lights that go dark close to the camera, like in tunnels or over the pool table. No Stencil lets "
       "every lamp light all it reaches, even through walls. No Clamp changes how a lamp's area is marked. "
       "Tell us which one brings the light back. Applies at the next start.",
       true, {"off", "no-stencil", "no-clamp"}, {"Off", "No Stencil", "No Clamp"}, "off"},
  };
#if defined(__EMSCRIPTEN__)
  // [web port] The browser's settings: without what's the PS5's own (the
  // DualSense, the TV's refresh rate, its audio port) or only the native
  // renderer's (the web renderer draws at the game's resolution, without
  // FusionFix or its anti-aliasing, frame limit and counter yet), and with
  // the help text for a browser.
  static const std::vector<Hub::Setting> web_rows = [] {
    static const std::string_view kHidden[] = {
        "resolution", "ps5_refresh_rate", "gta4_frame_limit", "ps5_fps_counter", "ps5_frame_generation",
        "gta4_native_anti_aliasing", "gta4_anisotropic_filtering", "gta4_modern_shaders", "ps5_audio_frames",
        "gta4_sony_enabled", "gta4_sony_lighting", "gta4_sony_triggers", "gta4_sony_rumble", "gta4_sony_intensity"};
    std::vector<Hub::Setting> out;
    for (Hub::Setting r : rows) {
      const std::string_view key = r.key;
      if (std::find(std::begin(kHidden), std::end(kHidden), key) != std::end(kHidden)) continue;
      if (key == "hub_display_preset") {
        r.help = "Sets the Display settings below in one go. Recommended is the game as it comes. Performance "
                 "has fewer cars and people, for the steadiest frame rate in the browser. Quality adds longer "
                 "shadows and more detail in the distance. Changing any of them makes it Custom.";
      } else if (key == "ps5_language") {
        r.help = "The language of the game's text (menus, subtitles and missions) and of this menu. Auto is "
                 "English in the browser. Voices stay in English, as on the Xbox 360.";
      }
      out.push_back(r);
    }
    return out;
  }();
  return web_rows;
#endif
  return rows;
}

// --------------------------------------------------------------------- data

void Hub::Init(const Paths& paths, const Assets& assets, std::string version) {
  paths_ = paths;
  a_ = assets;
  version_ = std::move(version);
  Load();
}

void Hub::Load() {
  config_.clear();
  std::ifstream in(paths_.config);
  for (std::string line; std::getline(in, line);) {
    const size_t a = line.find_first_not_of(" \t");
    if (a == std::string::npos || line[a] == '#') continue;
    if (line[a] == '[') break;  // (top-level keys only)
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = line.substr(a, eq - a), v = line.substr(eq + 1);
    k.erase(k.find_last_not_of(" \t") + 1);
    v.erase(0, v.find_first_not_of(" \t"));
    if (const size_t hash = v.find(" #"); hash != std::string::npos && v[0] != '"') v.erase(hash);
    v.erase(v.find_last_not_of(" \t\r") + 1);
    config_.emplace_back(k, v);
  }
  lang::Set(lang::FromSetting(Value("ps5_language", "auto")));
  debug_options_ = Value("hub_debug_options", "false") == "true";
  KeepHiddenSettings();
#if defined(__EMSCRIPTEN__)
  ApplyWebDeviceDefaults();
#endif
  LoadSaves();
  LoadTrophies();
  // (the game's files come from Recomp Store: only whether they're there)
  state_ = install::Look(paths_.app);
}

Pixels Shrink(const unsigned char* rgba, int w, int h, int max_h) {
  Pixels out;
  if (h <= max_h) {
    out.w = w, out.h = h;
    out.rgba.assign(rgba, rgba + size_t(w) * size_t(h) * 4);
    return out;
  }
  const double scale = double(h) / max_h;
  out.h = max_h, out.w = std::max(1, int(w / scale + 0.5));
  out.rgba.resize(size_t(out.w) * size_t(out.h) * 4);
  for (int y = 0; y < out.h; ++y) {
    const int y0 = int(y * scale), y1 = std::min(h, std::max(y0 + 1, int((y + 1) * scale)));
    for (int x = 0; x < out.w; ++x) {
      const int x0 = int(x * scale), x1 = std::min(w, std::max(x0 + 1, int((x + 1) * scale)));
      uint32_t sum[4] = {};
      for (int sy = y0; sy < y1; ++sy)
        for (int sx = x0; sx < x1; ++sx)
          for (int c = 0; c < 4; ++c) sum[c] += rgba[(size_t(sy) * w + sx) * 4 + c];
      const uint32_t n = uint32_t((y1 - y0) * (x1 - x0));
      for (int c = 0; c < 4; ++c) out.rgba[(size_t(y) * out.w + x) * 4 + c] = uint8_t(sum[c] / n);
    }
  }
  return out;
}

Pixels Grey(const Pixels& p) {
  Pixels out = p;
  for (size_t i = 0; i + 3 < out.rgba.size(); i += 4) {
    const uint8_t v = uint8_t((out.rgba[i] * 54 + out.rgba[i + 1] * 183 + out.rgba[i + 2] * 19) >> 8);
    out.rgba[i] = out.rgba[i + 1] = out.rgba[i + 2] = v;
  }
  return out;
}

ImU32 Accent(const Pixels& p) {
  // the hue most of its vivid pixels share (by 10 degrees, with their
  // neighbours), its pixels' average, made bright and strong. Skin's hues
  // count for less: the people would make every picture's colour the same.
  constexpr int kBins = 36;
  double weight[kBins] = {}, sum[kBins][3] = {};
  const size_t n = p.rgba.size() / 4, stride = std::max<size_t>(1, n / 60000);
  for (size_t k = 0; k < n; k += stride) {
    if (p.rgba[k * 4 + 3] < 128) continue;  // (a logo's clear parts)
    const float r = p.rgba[k * 4] / 255.0f, g = p.rgba[k * 4 + 1] / 255.0f, b = p.rgba[k * 4 + 2] / 255.0f;
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
    const float s = mx > 0 ? (mx - mn) / mx : 0;
    if (s < 0.2f || mx < 0.2f) continue;
    float h = mx == r ? (g - b) / (mx - mn) : mx == g ? 2 + (b - r) / (mx - mn) : 4 + (r - g) / (mx - mn);
    h = std::fmod(h * 60 + 360, 360.0f);
    const int bin = std::min(kBins - 1, int(h / (360.0f / kBins)));
    const bool skin = h < 50 || h > 350;
    const double w = double(s) * s * s * mx * (skin ? 0.05 : 1.0);
    weight[bin] += w, sum[bin][0] += r * w, sum[bin][1] += g * w, sum[bin][2] += b * w;
  }
  int best = -1;
  double best_w = 0;
  for (int i = 0; i < kBins; ++i) {
    const double w = weight[(i + kBins - 1) % kBins] + weight[i] + weight[(i + 1) % kBins];
    if (w > best_w) best_w = w, best = i;
  }
  if (best < 0) return 0;
  double c[3] = {};
  for (int d = -1; d <= 1; ++d)
    for (int k = 0; k < 3; ++k) c[k] += sum[(best + d + kBins) % kBins][k];
  for (double& v : c) v /= best_w;
  // (as HSV: the hue kept, strong and full bright)
  const double mx = std::max({c[0], c[1], c[2]}), mn = std::min({c[0], c[1], c[2]});
  if (mx <= 0) return 0;
  const double s = std::clamp((mx - mn) / mx * 1.3, 0.55, 0.9);
  int out[3];
  for (int k = 0; k < 3; ++k) {
    const double t = mx > mn ? (c[k] - mn) / (mx - mn) : 1;  // 0 at the weakest channel, 1 at the strongest
    out[k] = int(std::lround(255 * (1 - s * (1 - t))));
  }
  return Rgba(out[0], out[1], out[2]);
}

std::vector<int> Hub::HomeGames() const {
  // (as the Complete Edition's cover: The Lost and Damned, GTA IV, The Ballad of Gay Tony)
  std::vector<int> games;
  if (state_.installed && state_.episode[0]) games.push_back(1);
  games.push_back(0);
  if (state_.installed && state_.episode[1]) games.push_back(2);
  return games;
}

int Hub::HomeGame() const {
  const std::vector<int> games = HomeGames();
  return std::find(games.begin(), games.end(), home_game_) != games.end() ? home_game_ : 0;
}

Hub::SavesScan Hub::ScanSaves(const Paths& paths, const std::function<int()>& pending) {
  SavesScan out;
  std::error_code ec;
  // UserData/<profile>/<title>/<content type>/<name>: one save each
  for (const fs::path& profile : Profiles(paths.user_data)) {
    for (const auto& title : ListDir(profile)) {
      if (!title.is_directory(ec) || !Hex(title.path().filename().string(), 8)) continue;
      for (const auto& type : ListDir(title.path())) {
        const std::string t = type.path().filename().string();
        if (!type.is_directory(ec) || !Hex(t, 8)) continue;
        for (const auto& pkg : ListDir(type.path())) {
          if (!pkg.is_directory(ec)) continue;
          SaveItem s;
          s.folder = pkg.path();
          s.name = pkg.path().filename().string();
          // (the game's folder names, for people: "0Saved Game" -> "Saved Game", "exdata" -> "Extra Data")
          if (s.name.size() > 1 && std::isdigit(static_cast<unsigned char>(s.name[0])) &&
              std::isalpha(static_cast<unsigned char>(s.name[1])))
            s.name.erase(0, 1);
          if (s.name == "exdata") s.name = "Extra Data";
          s.kind = t == "00000001" ? "Saved game" : "Game data";
          s.bytes = FolderBytes(pkg.path(), &s.files);
          s.modified = NewestIn(pkg.path());
          out.saves.push_back(std::move(s));
        }
      }
    }
  }
  std::sort(out.saves.begin(), out.saves.end(), [](const SaveItem& a, const SaveItem& b) { return a.name < b.name; });
  for (const auto& e : ListDir(paths.app / "SaveBackups")) {
    if (!e.is_directory(ec)) continue;
    Backup b;
    b.name = e.path().filename().string();
    b.bytes = FolderBytes(e.path(), &b.files);
    out.backups.push_back(std::move(b));
  }
  // newest first (their names start with the date)
  std::sort(out.backups.begin(), out.backups.end(), [](const Backup& a, const Backup& b) { return a.name > b.name; });
  out.pending = pending ? pending() : 0;
  return out;
}

// (in the background: Draw takes the lists when they're made)
void Hub::LoadSaves() {
  if (saves_scan_.valid()) {
    saves_again_ = true;
    return;
  }
  saves_scan_ = std::async(std::launch::async,
                           [paths = paths_, pending = save_import_.pending] { return ScanSaves(paths, pending); });
}

void Hub::BackUpSaves(const std::string& why) {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_r(&now, &tm);
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H-%M-%S", &tm);
  const fs::path root = paths_.app / "SaveBackups";
  fs::path dest = root / (std::string(stamp) + why);
  std::error_code ec;
  for (int k = 2; fs::exists(dest, ec); ++k) dest = root / (std::string(stamp) + why + " " + std::to_string(k));
  bool ok = true;
  for (const fs::path& profile : Profiles(paths_.user_data)) ok = CopyInto(profile, dest / profile.filename()) && ok;
  if (ok) {
    if (why.empty()) Toast(T("Saves backed up"));
  } else {
    Toast(T("The backup couldn't be made"));
    Play(Sound::kError);
  }
  LoadSaves();
}

void Hub::RestoreSaves(const Backup& b) {
  BackUpSaves(" (before restore)");
  for (const fs::path& profile : Profiles(paths_.user_data)) {
    std::error_code ec;
    fs::remove_all(profile, ec);
  }
  if (CopyInto(paths_.app / "SaveBackups" / b.name, paths_.user_data)) {
    Toast(T("Backup restored"));
    Play(Sound::kOn);
  } else {
    Toast(T("Restoring didn't finish: your saves are in the newest backup"));
    Play(Sound::kError);
  }
  LoadSaves();
  item_[kSavesTab] = 0;
}

float Hub::MusicVolume() const {
  const int pct = std::atoi(Value("hub_music_volume", "50").c_str());
  return std::clamp(pct, 0, 100) / 100.0f;
}

std::string Hub::Value(const std::string& key, const std::string& fallback) const {
  for (const auto& [k, v] : config_)
    if (k == key) return Unquote(v);
  return fallback;
}

void Hub::Save(const std::string& key, const std::string& value) {
  bool found = false;
  for (auto& [k, v] : config_)
    if (k == key) v = value, found = true;
  if (!found) config_.emplace_back(key, value);
  // The file: that key's line replaced (or added before the first [section]).
  std::vector<std::string> lines;
  {
    std::ifstream in(paths_.config);
    for (std::string line; std::getline(in, line);) lines.push_back(line);
  }
  const std::string entry = key + " = " + value;
  size_t insert_at = lines.size();
  bool done = false;
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string& l = lines[i];
    const size_t first = l.find_first_not_of(" \t");
    if (first == std::string::npos) continue;
    if (l[first] == '[') {
      insert_at = std::min(insert_at, i);
      break;
    }
    const size_t eq = l.find('=');
    if (eq == std::string::npos || l[first] == '#') continue;
    std::string k = l.substr(first, eq - first);
    k.erase(k.find_last_not_of(" \t") + 1);
    if (k == key) {
      lines[i] = entry;
      done = true;
      break;
    }
  }
  if (!done) {
    // (before the comment lines over the first [section])
    while (insert_at > 0 && insert_at < lines.size() &&
           lines[insert_at - 1].find_first_not_of(" \t") != std::string::npos &&
           lines[insert_at - 1][lines[insert_at - 1].find_first_not_of(" \t")] == '#')
      --insert_at;
    lines.insert(lines.begin() + std::ptrdiff_t(insert_at), entry);
  }
#if defined(__EMSCRIPTEN__)
  // [web port] Written in place: a rename in the browser's storage (OPFS)
  // left a directory where the file was.
  {
    std::ofstream out(paths_.config, std::ios::trunc);
    for (const std::string& l : lines) out << l << "\n";
  }
#else
  const fs::path tmp = paths_.config.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    for (const std::string& l : lines) out << l << "\n";
  }
  std::error_code ec;
  fs::rename(tmp, paths_.config, ec);
#endif
}

void Hub::NowPlaying(const std::string&) {
  // (songs change quietly: no name on the screen)
}

// The menu type, baked at the sizes the hub draws it at (on a 1080p screen
// and on a 4K one, where everything is twice as big): the tabs and titles,
// the rows, the headings and hints. The largest sizes hold only plain ASCII,
// and one only what's drawn that big (a percentage), to keep the atlas small;
// 100 and 120 have the accented letters too (the tabs and titles at 4K, in
// French, German, Spanish and Italian).
// The condensed letters get a little air between them (kBoldTrack and
// kRegularTrack, of the size): set as tight as the font comes, rows of it
// read cramped on a TV.
void AddMenuFonts(ImFontAtlas* atlas, const void* bold, int bold_bytes, const void* regular, int regular_bytes,
                  Assets& a) {
  constexpr float kBoldTrack = 0.03f, kRegularTrack = 0.02f;
  static const ImWchar kLatin[] = {0x0020, 0x00FF, 0x0152, 0x0153, 0x2010, 0x2027, 0};
  static const ImWchar kLetters[] = {0x0020, 0x007E, 0x00A0, 0x00FF, 0x0152, 0x0153, 0};
  static const ImWchar kAscii[] = {0x0020, 0x007E, 0};
  static const ImWchar kDigits[] = {'%', '%', '0', '9', 'H', 'H', 0};
  auto add = [atlas](const void* data, int bytes, float px, const ImWchar* ranges, float track) {
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;  // (built into the app; shared by every size)
    cfg.OversampleH = px < 40 ? 2 : 1;
    cfg.OversampleV = 1;
    cfg.GlyphRanges = ranges;
    cfg.GlyphExtraAdvanceX = px * track;
    return atlas->AddFontFromMemoryTTF(const_cast<void*>(data), bytes, px, &cfg);
  };
  a.menu_sizes.clear();
  a.menu_regular_sizes.clear();
  for (float px : {24.0f, 32.0f, 42.0f, 52.0f, 62.0f, 72.0f, 84.0f})
    if (ImFont* f = add(bold, bold_bytes, px, kLatin, kBoldTrack)) a.menu_sizes.push_back(f);
  for (float px : {100.0f, 120.0f})
    if (ImFont* f = add(bold, bold_bytes, px, kLetters, kBoldTrack)) a.menu_sizes.push_back(f);
  if (ImFont* f = add(bold, bold_bytes, 144.0f, kAscii, kBoldTrack)) a.menu_sizes.push_back(f);
  if (ImFont* f = add(bold, bold_bytes, 244.0f, kDigits, kBoldTrack)) a.menu_sizes.push_back(f);
  for (float px : {24.0f, 30.0f, 36.0f, 42.0f, 48.0f, 56.0f, 64.0f})
    if (ImFont* f = add(regular, regular_bytes, px, kLatin, kRegularTrack)) a.menu_regular_sizes.push_back(f);
  a.menu = a.menu_sizes.size() < 2 ? nullptr : a.menu_sizes[a.menu_sizes.size() - 2];  // (144: a full one)
  a.menu_regular = a.menu_regular_sizes.empty() ? nullptr : a.menu_regular_sizes.back();
}

// Helvetica Neue Heavy for the tabs, at the sizes they're drawn at
// (66 or less on a 1080p screen, twice that at 4K). The handle is the largest.
void AddTabFonts(ImFontAtlas* atlas, const void* data, int bytes, Assets& a) {
  static const ImWchar kLetters[] = {0x0020, 0x007E, 0x00A0, 0x00FF, 0x0152, 0x0153, 0};
  a.tab_sizes.clear();
  for (float px : {40.0f, 52.0f, 66.0f, 84.0f, 104.0f, 132.0f}) {
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;  // (built into the app; shared by every size)
    cfg.OversampleH = px < 60 ? 2 : 1;
    cfg.OversampleV = 1;
    cfg.GlyphRanges = kLetters;
    if (ImFont* f = atlas->AddFontFromMemoryTTF(const_cast<void*>(data), bytes, px, &cfg)) a.tab_sizes.push_back(f);
  }
  a.tab = a.tab_sizes.empty() ? nullptr : a.tab_sizes.back();
}

// The Lost and Damned's scuffs (see hub_ui.h): soft blotches where the paint
// is thinner, specks, and short scratches, as its menu's type has. A fixed
// seed: the tabs look the same each time.
Pixels Grunge(int w, int h) {
  Pixels p;
  p.w = w, p.h = h;
  std::vector<float> dark(size_t(w) * size_t(h), 0.0f);
  uint32_t seed = 0x2C1B3C6Du;
  auto rnd = [&] {
    seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
    return float(seed & 0xFFFFFF) / float(0x1000000);
  };
  // blotches: value noise on two scales, only its peaks
  auto lattice = [](int x, int y, uint32_t salt) {
    uint32_t v = uint32_t(x) * 0x8DA6B343u ^ uint32_t(y) * 0xD8163841u ^ salt * 0xCB1AB31Fu;
    v ^= v >> 13, v *= 0x5BD1E995u, v ^= v >> 15;
    return float(v & 0xFFFF) / 65535.0f;
  };
  auto noise = [&](float x, float y, float cell, uint32_t salt) {
    const float fx = x / cell, fy = y / cell;
    const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
    const float tx = Smooth(fx - float(ix)), ty = Smooth(fy - float(iy));
    const float a = lattice(ix, iy, salt), b = lattice(ix + 1, iy, salt);
    const float c = lattice(ix, iy + 1, salt), d = lattice(ix + 1, iy + 1, salt);
    return (a + (b - a) * tx) * (1 - ty) + (c + (d - c) * tx) * ty;
  };
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float n = 0.6f * noise(float(x), float(y), 22, 1) + 0.4f * noise(float(x), float(y), 7, 2);
      // (and a fine grit all over)
      const float grit = noise(float(x), float(y), 2.2f, 3);
      dark[size_t(y) * w + x] = 1 - (1 - std::clamp((n - 0.5f) / 0.3f, 0.0f, 1.0f) * 0.55f) *
                                        (1 - std::clamp((grit - 0.55f) / 0.45f, 0.0f, 1.0f) * 0.35f);
    }
  auto blend = [&](int x, int y, float v) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    float& d = dark[size_t(y) * w + x];
    d = 1 - (1 - d) * (1 - std::clamp(v, 0.0f, 1.0f));
  };
  // specks
  const int specks = w * h / 110;
  for (int i = 0; i < specks; ++i) {
    const float cx = rnd() * float(w), cy = rnd() * float(h), r = 0.5f + rnd() * rnd() * 2.2f, v = 0.45f + 0.5f * rnd();
    for (int y = int(cy - r - 1); y <= int(cy + r + 1); ++y)
      for (int x = int(cx - r - 1); x <= int(cx + r + 1); ++x) {
        const float dist = std::hypot(float(x) + 0.5f - cx, float(y) + 0.5f - cy);
        blend(x, y, v * std::clamp(r + 0.5f - dist, 0.0f, 1.0f));
      }
  }
  // scratches: short, thin and wandering
  const int scratches = w / 6;
  for (int i = 0; i < scratches; ++i) {
    float x = rnd() * float(w), y = rnd() * float(h), angle = rnd() * 6.2832f;
    const int steps = 6 + int(rnd() * 34);
    const float v = 0.5f + 0.45f * rnd();
    for (int k = 0; k < steps; ++k) {
      blend(int(x), int(y), v);
      if (rnd() < 0.35f) blend(int(x) + 1, int(y), v * 0.5f);
      angle += (rnd() - 0.5f) * 0.5f;
      x += std::cos(angle), y += std::sin(angle);
    }
  }
  p.rgba.resize(size_t(w) * size_t(h) * 4);
  for (size_t i = 0; i < dark.size(); ++i) {
    p.rgba[i * 4 + 0] = p.rgba[i * 4 + 1] = p.rgba[i * 4 + 2] = 255;
    p.rgba[i * 4 + 3] = static_cast<unsigned char>(std::clamp(dark[i], 0.0f, 1.0f) * 255.0f + 0.5f);
  }
  return p;
}

// ------------------------------------------------------------------ drawing

float Hub::Ease(float& v, float target, float speed) {
  v = v < -0.5f ? target : v + (target - v) * (1.0f - std::exp(-dt_ * speed));
  return v;
}

void Hub::Fade(int first_vertex, float alpha) {
  if (alpha >= 0.999f) return;
  for (int i = first_vertex; i < dl_->VtxBuffer.Size; ++i) {
    ImU32& c = dl_->VtxBuffer[i].col;
    c = WithAlpha(c, alpha);
  }
}

ImFont* Hub::Pick(ImFont* f, float size, const std::string* t) const {
  const std::vector<ImFont*>* sizes = f == a_.menu           ? &a_.menu_sizes
                                     : f == a_.menu_regular ? &a_.menu_regular_sizes
                                     : f == a_.tab          ? &a_.tab_sizes
                                                            : nullptr;
  if (!sizes || sizes->empty()) return f;
  const float want = size * s_ * 0.94f;  // (a touch smaller is still sharp)
  auto has_all = [&](ImFont* face) {
    if (!t) return true;
    ImFontBaked* baked = face->GetFontBaked(face->LegacySize);
    if (!baked) return false;
    for (size_t i = 0; i < t->size();) {  // (UTF-8: up to three bytes a letter here)
      const unsigned char c = static_cast<unsigned char>((*t)[i]);
      unsigned int cp = c;
      size_t n = 1;
      if (c >= 0xE0 && i + 2 < t->size()) cp = ((c & 0x0Fu) << 12) | ((((*t)[i + 1]) & 0x3Fu) << 6) | (((*t)[i + 2]) & 0x3Fu), n = 3;
      else if (c >= 0xC0 && i + 1 < t->size()) cp = ((c & 0x1Fu) << 6) | (((*t)[i + 1]) & 0x3Fu), n = 2;
      i += n;
      if (cp >= 0x20 && cp != '\n' && !baked->FindGlyphNoFallback(ImWchar(cp))) return false;
    }
    return true;
  };
  for (ImFont* face : *sizes)
    if (face->LegacySize >= want && has_all(face)) return face;
  for (auto it = sizes->rbegin(); it != sizes->rend(); ++it)  // (bigger than any: the largest with the letters)
    if (has_all(*it)) return *it;
  return f;
}

void Hub::Text(ImFont* f, float size, float x, float y, ImU32 col, const std::string& t, float align, float spacing) {
  if (!f) f = ImGui::GetFont();
  f = Pick(f, size, &t);
  float px = x - TextWidth(f, size, t, spacing) * align;
  if (spacing <= 0) {
    dl_->AddText(f, size * s_, P(px, y), col, t.c_str());
    return;
  }
  // (letter by letter: a UTF-8 letter's bytes stay together)
  for (size_t i = 0; i < t.size();) {
    size_t n = 1;
    const unsigned char c = static_cast<unsigned char>(t[i]);
    if (c >= 0xF0) n = 4;
    else if (c >= 0xE0) n = 3;
    else if (c >= 0xC0) n = 2;
    const char* a = t.c_str() + i;
    const char* b = t.c_str() + std::min(t.size(), i + n);
    dl_->AddText(f, size * s_, P(px, y), col, a, b);
    px += f->CalcTextSizeA(size, FLT_MAX, 0, a, b).x + spacing;
    i += n;
  }
}

float Hub::TextWidth(ImFont* f, float size, const std::string& t, float spacing) const {
  if (!f) f = ImGui::GetFont();
  f = Pick(f, size, &t);
  if (spacing <= 0) return f->CalcTextSizeA(size, FLT_MAX, 0, t.c_str()).x;
  float w = 0;
  for (size_t i = 0; i < t.size();) {
    size_t n = 1;
    const unsigned char c = static_cast<unsigned char>(t[i]);
    if (c >= 0xF0) n = 4;
    else if (c >= 0xE0) n = 3;
    else if (c >= 0xC0) n = 2;
    w += f->CalcTextSizeA(size, FLT_MAX, 0, t.c_str() + i, t.c_str() + std::min(t.size(), i + n)).x + spacing;
    i += n;
  }
  return t.empty() ? 0 : w - spacing;
}

std::string Hub::Fit(ImFont* f, float size, const std::string& t, float width) const {
  if (TextWidth(f, size, t) <= width) return t;
  std::string s = t;
  while (!s.empty() && TextWidth(f, size, s + "...") > width) {
    s.pop_back();
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();  // (whole UTF-8 letters)
  }
  return s + "...";
}

// Text broken into lines of at most `width`; returns the height used.
// Text wrapped to a width, a line per '\n' at least.
std::vector<std::string> Hub::WrapLines(ImFont* f, float size, float width, const std::string& t) {
  std::vector<std::string> lines;
  size_t start = 0;
  for (;;) {
    const size_t nl = t.find('\n', start);
    std::istringstream words(t.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
    std::string word, cur;
    while (words >> word) {
      const std::string next = cur.empty() ? word : cur + " " + word;
      if (!cur.empty() && TextWidth(f, size, next) > width) {
        lines.push_back(cur);
        cur = word;
      } else {
        cur = next;
      }
    }
    if (!cur.empty()) lines.push_back(Fit(f, size, cur, width));
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  return lines;
}

float Hub::Wrapped(ImFont* f, float size, float x, float y, float width, ImU32 col, const std::string& t, float line) {
  float yy = y;
  for (const std::string& l : WrapLines(f, size, width, t)) Text(f, size, x, yy, col, l), yy += size * line;
  return yy - y;
}

void Hub::Rect(float x, float y, float w, float h, ImU32 col, float r) {
  dl_->AddRectFilled(P(x, y), P(x + w, y + h), col, r * s_);
}

namespace {
const ImFontGlyph* CapGlyph(ImFont* f) {
  ImFontBaked* baked = f->GetFontBaked(f->LegacySize);
  return baked ? baked->FindGlyphNoFallback('H') : nullptr;
}
}  // namespace

float Hub::CapMid(ImFont* f, float size) const {
  if (!f) f = ImGui::GetFont();
  f = Pick(f, size);
  const ImFontGlyph* g = CapGlyph(f);
  return g ? (g->Y0 + g->Y1) * 0.5f * size / f->LegacySize : size * 0.5f;
}

float Hub::Baseline(ImFont* f, float size) const {
  if (!f) f = ImGui::GetFont();
  f = Pick(f, size);
  const ImFontGlyph* g = CapGlyph(f);
  return g ? g->Y1 * size / f->LegacySize : size * 0.8f;
}

void Hub::TextMid(ImFont* f, float size, float x, float cy, ImU32 col, const std::string& t, float align,
                  float spacing) {
  Text(f, size, x, cy - CapMid(f, size), col, t, align, spacing);
}

#if defined(__EMSCRIPTEN__)
namespace {
// The prompts the hub shows, as the game's will (the Button Prompts setting,
// gta4_button_prompts): Auto follows the last input - the keyboard's keys, or
// the controller's own buttons - and a chosen set shows that set's whatever
// is used.
struct PromptStyle {
  bool keyboard;
  PadFamily family;
};
PromptStyle StyleFor(const std::string& chosen) {
  if (chosen == "keyboard") return {true, PadFamily::kXbox};
  if (chosen.rfind("xbox", 0) == 0 || chosen == "steam_deck" || chosen == "steam_controller")
    return {false, PadFamily::kXbox};
  if (chosen.rfind("ps", 0) == 0) return {false, PadFamily::kPlayStation};
  if (chosen == "switch") return {false, PadFamily::kNintendo};
  return {KeyboardPrompts(), CurrentPadFamily()};
}
}  // namespace
#endif

// The DualSense's buttons: the pictures in hub/buttons (each drawn centred in
// its square, ps5/tools/hub_buttons.py), else drawn in one colour. `size` is
// the button's longer side: a face button's width, the options button's height.
float Hub::GlyphWidth(uint32_t button, float size) const {
  if (button == (kUp | kDown)) return size * 0.62f;  // (drawn: the menu's arrows)
  if (button == (kLeft | kRight)) return size * 1.05f;
#if defined(__EMSCRIPTEN__)
  // (a keycap: the key's name and a margin)
  const PromptStyle style = StyleFor(Value("gta4_button_prompts", "auto"));
  if (style.keyboard) {
    if (const char* key = KeyboardKey(button)) {
      return std::max(size, TextWidth(a_.body_bold, size * 0.5f, key) + size * 0.55f);
    }
  } else if (const char* letter = PadLetter(button, style.family)) {
    // (an Xbox or Nintendo button: a ring, or a pill for the longer names)
    return std::max(size, TextWidth(a_.body_bold, size * 0.46f, letter) + size * 0.5f);
  }
#endif
  if (auto it = a_.buttons.find(button); it != a_.buttons.end() && it->second)
    return size * it->second.fill_w / std::max(it->second.fill_w, it->second.fill_h);
  switch (button) {
    case kL1:
    case kR1: return size;
    case kOptions: return size * 26 / 22;
    default: return size;
  }
}

float Hub::Glyph(uint32_t button, float cx, float cy, ImU32 col, float size) {
  // (the d-pad's directions are drawn, as the game's arrows: its picture is the whole pad)
  const bool arrows = button == (kUp | kDown) || button == (kLeft | kRight);
#if defined(__EMSCRIPTEN__)
  // The keyboard: a keycap with the key's name.
  const PromptStyle style = StyleFor(Value("gta4_button_prompts", "auto"));
  if (!arrows && style.keyboard) {
    if (const char* key = KeyboardKey(button)) {
      const float w = GlyphWidth(button, size), h = size * 0.8f;
      dl_->AddRect(P(cx - w * 0.5f, cy - h * 0.5f), P(cx + w * 0.5f, cy + h * 0.5f), col, 5 * s_, 0, 1.6f * s_);
      TextMid(a_.body_bold, size * 0.5f, cx, cy, col, key, 0.5f);
      return w;
    }
  }
  // An Xbox or Nintendo controller: its buttons' names, by position.
  if (!arrows && !style.keyboard) {
    if (const char* letter = PadLetter(button, style.family)) {
      const float w = GlyphWidth(button, size), h = size * 0.8f;
      dl_->AddRect(P(cx - w * 0.5f, cy - h * 0.5f), P(cx + w * 0.5f, cy + h * 0.5f), col, h * 0.5f * s_, 0, 1.6f * s_);
      TextMid(a_.body_bold, size * 0.46f, cx, cy, col, letter, 0.5f);
      return w;
    }
  }
#endif
  if (auto it = a_.buttons.find(button); !arrows && it != a_.buttons.end() && it->second) {
    const Image& img = it->second;
    const float h = size * 0.5f / std::max(img.fill_w, img.fill_h);
    dl_->AddImage(img.id, P(cx - h, cy - h), P(cx + h, cy + h), ImVec2(0, 0), ImVec2(1, 1), col);
    return GlyphWidth(button, size);
  }
  const ImVec2 c = P(cx, cy);
  // (drawn: a face button 30 across, L1 / R1 40, the options button 22 tall)
  const float u = size / (button == kL1 || button == kR1 ? 40 : button == kOptions ? 22 : 30) * s_;
  const float R = 15 * u, k = 6.2f * u, t = 2.2f * u;
  switch (button) {
    case kCross:
    case kCircle:
    case kSquare:
    case kTriangle:
      dl_->AddCircle(c, R, col, 40, 1.6f * u);
      if (button == kCross) {
        dl_->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), col, t);
        dl_->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), col, t);
      } else if (button == kCircle) {
        dl_->AddCircle(c, k * 1.1f, col, 28, t);
      } else if (button == kSquare) {
        dl_->AddRect(ImVec2(c.x - k * 0.95f, c.y - k * 0.95f), ImVec2(c.x + k * 0.95f, c.y + k * 0.95f), col, 0, 0, t);
      } else {
        dl_->AddTriangle(ImVec2(c.x, c.y - k * 1.15f), ImVec2(c.x + k * 1.1f, c.y + k * 0.8f),
                         ImVec2(c.x - k * 1.1f, c.y + k * 0.8f), col, t);
      }
      break;
    case kLeft | kRight: {
      const float a = 8 * u, gap = 3.5f * u;
      dl_->AddTriangleFilled(ImVec2(c.x - gap, c.y - a), ImVec2(c.x - gap, c.y + a), ImVec2(c.x - gap - a * 1.2f, c.y), col);
      dl_->AddTriangleFilled(ImVec2(c.x + gap, c.y - a), ImVec2(c.x + gap, c.y + a), ImVec2(c.x + gap + a * 1.2f, c.y), col);
      break;
    }
    case kUp | kDown: {
      const float a = 8 * u, gap = 3.5f * u;
      dl_->AddTriangleFilled(ImVec2(c.x - a, c.y - gap), ImVec2(c.x + a, c.y - gap), ImVec2(c.x, c.y - gap - a * 1.2f), col);
      dl_->AddTriangleFilled(ImVec2(c.x - a, c.y + gap), ImVec2(c.x + a, c.y + gap), ImVec2(c.x, c.y + gap + a * 1.2f), col);
      break;
    }
    case kOptions:
      dl_->AddRect(ImVec2(c.x - 13 * u, c.y - 11 * u), ImVec2(c.x + 13 * u, c.y + 11 * u), col, 6 * u, 0, 1.6f * u);
      for (int i = -1; i <= 1; ++i)
        dl_->AddLine(ImVec2(c.x - 6 * u, c.y + i * 4.5f * u), ImVec2(c.x + 6 * u, c.y + i * 4.5f * u), col, 1.8f * u);
      break;
    case kR3:
      dl_->AddCircle(c, R, col, 40, 1.6f * u);
      dl_->AddCircleFilled(c, R - 4 * u, col, 40);
      TextMid(a_.body_bold, 13 * u / s_, cx, cy, Black(1), "R3", 0.5f);
      break;
    case kL1:
    case kR1:
      dl_->AddRect(ImVec2(c.x - 20 * u, c.y - 13 * u), ImVec2(c.x + 20 * u, c.y + 13 * u), col, 7 * u, 0, 1.6f * u);
      TextMid(a_.body_bold, 15 * u / s_, cx, cy, col, button == kL1 ? "L1" : "R1", 0.5f);
      break;
  }
  return GlyphWidth(button, size);
}

// One hint in the footer, right-aligned at `right`: the button, then what it
// does in capitals, both centred on the line (as the game's MOVE / ADJUST / BACK).
float Hub::Hint(float right, uint32_t button, const char* label, int line) {
  constexpr float kSize = 23, kButton = 31, kGap = 12, kBetween = 46, kSpacing = 0.8f;
  const float cy = kHintsY + 40 * float(line);
  const std::string text = Upper(T(label));
  const float tw = TextWidth(Regular(), kSize, text, kSpacing);
  const float gw = GlyphWidth(button, kButton);
  const float x = right - tw;
  TextMid(Regular(), kSize, x, cy, pal_.hint, text, 0, kSpacing);
  Glyph(button, x - kGap - gw * 0.5f, cy, White(0.92f), kButton);
#if defined(__EMSCRIPTEN__)
  // (the left / right hint clicks as right: the next value)
  Hit(x - kGap - gw - 6, cy - 20, right - (x - kGap - gw - 6), 40, 2, button == (kLeft | kRight) ? kRight : button);
#endif
  return x - kGap - gw - kBetween;
}

// A title over a page of its own (the installer), in the menu's yellow.
void Hub::Heading(const std::string& title, const std::string& subtitle) {
  TextMid(Bold(), 54, kPageL, kListY + 30, pal_.title, title);
  if (!subtitle.empty()) TextMid(Regular(), 28, kPageL, kListY + 84, kMenuGray, subtitle);
}

// A scrolled list between the rules: rows of the page's items (the focused
// one in yellow, the colour following the focus as it moves), and small grey
// headings over groups of them.
void Hub::List(std::vector<Row>& rows, int sel, float rh, const std::function<void(int, float, float)>& draw) {
  const float hh = 44, view = kListView, x = kListX, y0 = kListY;
  float yy = 0;
  int sel_row = -1;
  for (size_t k = 0; k < rows.size(); ++k) {
    Row& r = rows[k];
    if (r.item < 0) {
      yy += k ? 14 : 0;
      r.y = yy;
      yy += hh;
    } else {
      r.y = yy;
      if (r.item == sel) sel_row = int(k);
      yy += rh;
    }
  }
  const float total = yy;
  if (sel_row < 0) return;
  // keep the focused row in view (with its heading, when it's the first under it)
  {
    float top = rows[size_t(sel_row)].y, bottom = top + rh;
    if (sel_row > 0 && rows[size_t(sel_row) - 1].item < 0) top = rows[size_t(sel_row) - 1].y;
    float target = scroll_px_;
    if (top < target) target = top;
    if (bottom > target + view) {
      target = bottom - view;
      for (const Row& r : rows)
        if (r.y >= target - 0.5f) {
          target = r.y;
          break;
        }
    }
    scroll_px_ = std::clamp(target, 0.0f, std::max(0.0f, total - view));
  }
  Ease(scroll_f_, scroll_px_, 28);
  Ease(focus_y_, rows[size_t(sel_row)].y, 36);
  dl_->PushClipRect(P(0, y0 - 6), P(kW, y0 + view), true);
  for (size_t k = 0; k < rows.size(); ++k) {
    const Row& row = rows[k];
    const float ry = y0 + row.y - scroll_f_;
    if (ry < y0 - rh - 20 || ry > y0 + view + 20) continue;
    if (row.item < 0) {
      TextMid(Bold(), 21, x, ry + hh * 0.5f, kDimGray, Upper(row.heading), 0, 1.6f);
      continue;
    }
    const float over = std::clamp(1 - std::fabs(focus_y_ - row.y) / (rh * 0.85f), 0.0f, 1.0f);
    draw(row.item, ry, over);
#if defined(__EMSCRIPTEN__)
    if (ry >= y0 - 2 && ry + rh <= y0 + view + 2) Hit(x - 24, ry, kPanelX - 60 - (x - 24), rh, 1, uint32_t(row.item));
#endif
  }
  dl_->PopClipRect();
  if (total > view) {  // where the list is: a thin bar at the list's right
    const float bx = kPanelX - 56;
    const float bh = std::max(40.0f, view * view / total);
    const float by = y0 + (view - bh) * scroll_f_ / (total - view);
    Rect(bx, y0, 2, view, kRuleGray);
    Rect(bx - 1, by, 4, bh, kMenuGray);
  }
}

void Hub::Panel(const std::function<void(float, float, float)>& draw) {
  const int v0 = dl_->VtxBuffer.Size;
  draw(kPanelX + (1 - Smooth(panel_in_)) * 12, kPanelY, kPanelW);
  Fade(v0, Smooth(panel_in_));
}

float Hub::Facts(float x, float y, float w, const std::vector<std::pair<std::string, std::string>>& facts) {
  for (const auto& [k, v] : facts) {
    TextMid(Bold(), 20, x, y + 17, kDimGray, Upper(k), 0, 1.6f);
    y += std::max(46.0f, Wrapped(Bold(), 28, x + 190, y, w - 190, kText, v, 1.25f) + 14);
  }
  return y;
}

// Black, as the game's menu: the pages draw on it.
void Hub::DrawBackdrop() {
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  dl_->AddRectFilled(ImVec2(0, 0), size, Black(1));
}

// The tabs across the top, as the game's: the chosen one in yellow (easing
// over as L1 and R1 change it), the others grey; in an episode's look, as that
// episode's menu (DrawTabs). Over the installer, its one.
void Hub::DrawTopBar() {
  for (int i = 0; i < kTabCount; ++i) Ease(tab_lit_[i], i == tab_ ? 1.0f : 0.0f, 26);
  for (int g = 0; g < 3; ++g)
    if (theme_[g] > 0.004f) DrawTabs(g, Smooth(theme_[g]));
}

// The tabs in one game's look, faded by `alpha` (two of them while the look
// changes), all three in Helvetica Neue Heavy: GTA IV's grey and the chosen
// one gold; The Lost and Damned's grey and the chosen one dark red, scuffed
// (Grunge); The Ballad of Gay Tony's each in its colour along the rainbow, the
// chosen one white.
void Hub::DrawTabs(int game, float alpha) {
  constexpr float kSize = 66, kGap = 60;
  ImFont* const f = a_.tab ? a_.tab : Bold();
  const Palette& p = GamePalette(game);
  std::vector<std::string> names;
  if (installer_) {
    names.push_back(T("Install|tab"));
  } else {
    for (int i = 0; i < TabsShown(); ++i) names.push_back(T(kTabNames[i]));
  }
  // (longer names in some languages, a wider type: smaller, to fit the page's width)
  float total = 0;
  for (size_t i = 0; i < names.size(); ++i)
    total += TextWidth(f, kSize, names[i]) + (i ? kGap : 0);
  const float fit = std::min(1.0f, (kPageR - kPageL) / std::max(1.0f, total));
  const float size = kSize * fit, gap = kGap * fit;
  float x = kPageL;
  for (size_t i = 0; i < names.size(); ++i) {
    const float lit = installer_ ? 1.0f : tab_lit_[i];
    const ImU32 off = game == 2 ? Along(kTbogtTabs, names.size() > 1 ? float(i) / float(names.size() - 1) : 0.0f)
                                : p.tab_off;
    const float w = TextWidth(f, size, names[i]);
    TextMid(f, size, x, kTabsY, WithAlpha(Mix(off, p.tab_on, lit), alpha), names[i]);
#if defined(__EMSCRIPTEN__)
    if (!installer_ && alpha > 0.5f) Hit(x - gap * 0.4f, kTabsY - size * 0.65f, w + gap * 0.8f, size * 1.2f, 0, uint32_t(i));
#endif
    if (game == 1 && a_.grunge) {
      // (black over black outside the letters: only they show it)
      const float x0 = x - 4, x1 = x + w + 4, y0 = kTabsY - size * 0.62f, y1 = kTabsY + size * 0.46f;
      const float u0 = (x0 - kPageL) / (kPageR - kPageL + 40), u1 = (x1 - kPageL) / (kPageR - kPageL + 40);
      dl_->AddImage(a_.grunge.id, P(x0, y0), P(x1, y1), ImVec2(u0, 0), ImVec2(u1, 1), Black(0.92f * alpha));
    }
    x += w + gap;
  }
}

// Each game's share of the look eases to the one chosen on Home (quickly: the
// pillars open as fast), and this frame's colours are mixed from them.
void Hub::Theme() {
  const int game = HomeGames().size() > 1 ? HomeGame() : 0;
  for (int g = 0; g < 3; ++g) Ease(theme_[g], g == game ? 1.0f : 0.0f, 9);
  float sum = 0;
  for (float w : theme_) sum += w;
  auto mix = [&](ImU32 Palette::*field) {
    float c[4] = {};
    for (int g = 0; g < 3; ++g) {
      const ImU32 v = GamePalette(g).*field;
      for (int k = 0; k < 4; ++k) c[k] += float((v >> (8 * k)) & 0xFF) * theme_[g] / std::max(sum, 1e-4f);
    }
    ImU32 out = 0;
    for (int k = 0; k < 4; ++k) out |= ImU32(std::clamp(c[k] + 0.5f, 0.0f, 255.0f)) << (8 * k);
    return out;
  };
  for (ImU32 Palette::*field : {&Palette::tab_off, &Palette::tab_on, &Palette::row, &Palette::focus, &Palette::title,
                                &Palette::hint, &Palette::toast, &Palette::rule_top, &Palette::rule_bottom})
    pal_.*field = mix(field);
}

// The rules under the tabs and over the footer (across the whole screen; The
// Ballad of Gay Tony's in its rainbow, the lower one darker), and the clock.
void Hub::DrawFooter() {
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  for (const float y : {kRuleTop, kRuleBottom}) {
    const float y0 = P(0, y).y, y1 = P(0, y + 2).y;
    dl_->AddRectFilled(ImVec2(0, y0), ImVec2(size.x, y1), y == kRuleTop ? pal_.rule_top : pal_.rule_bottom);
    if (theme_[2] > 0.004f) {
      constexpr int kStops = int(sizeof(kTbogtRule) / sizeof(kTbogtRule[0]));
      const float a = Smooth(theme_[2]) * (y == kRuleTop ? 1.0f : 0.62f);
      for (int k = 0; k + 1 < kStops; ++k) {
        const float xa = size.x * float(k) / float(kStops - 1), xb = size.x * float(k + 1) / float(kStops - 1);
        const ImU32 ca = WithAlpha(kTbogtRule[k], a), cb = WithAlpha(kTbogtRule[k + 1], a);
        dl_->AddRectFilledMultiColor(ImVec2(xa, y0), ImVec2(xb, y1), ca, cb, cb, ca);
      }
    }
  }
  // At the footer's left, as the game's pause menu has its day, time and
  // money: the day, the date and the time. A message takes its place for a
  // while (Toast).
  // (a touch heavier than the regular type: drawn twice, half a pixel apart)
  const float message = ToastShown();
  if (message < 0.999f) {
    const std::string clock = lang::Clock(int64_t(std::time(nullptr)));
    for (const float dx : {0.0f, 0.5f})
      TextMid(Regular(), 32, kPageL + dx, kHintsY, WithAlpha(pal_.toast, 1 - message), clock, 0, 0.6f);
  }
}

float Hub::ToastShown() const {
  return toast_time_ > 0 ? Smooth(std::min(toast_time_ * 3, (3.0f - toast_time_) * 6)) : 0.0f;
}

void Hub::DrawHints() {
  float r = kPageR;
  if (confirm_) return;  // (the question has its own, under it)
  if (installer_) {
    InstallerHints(r);
  } else {
    switch (tab_) {
      case kHome:
        r = Hint(r, kCross, state_.installed ? "Play" : "Install");
        if (HomeGames().size() > 1) r = Hint(r, kLeft | kRight, "Move");
        break;
      case kDisplayTab:
      case kGameTab:
      case kDebugTab:
        r = Hint(r, kCircle, "Back");
        r = Hint(r, kTriangle, "Reset All");
        r = Hint(r, kLeft | kRight, "Adjust");
        r = Hint(r, kUp | kDown, "Move");
        break;
      case kSavesTab:
        r = Hint(r, kCircle, "Back");
        // (the actions, then the backups; the saves themselves have none)
        if (const int it = item_[tab_]; it < kSaveActions + int(backups_.size()))
          r = Hint(r, kCross, it == 0   ? "Back Up"
                              : it == 1 ? "Import"
                              : it == 2 && kSaveActions > 2 ? "Download"
                              : it == 3 && kSaveActions > 3 ? "Load"
                                                            : "Restore");
        r = Hint(r, kUp | kDown, "Move");
        break;
      case kTrophiesTab:
        r = Hint(r, kCircle, "Back");
        if (!trophy_items_.empty()) r = Hint(r, kUp | kDown, "Move");
        break;
      case kEpisodesTab:
        r = Hint(r, kCircle, "Back");
        if (!state_.episode[std::clamp(item_[tab_], 0, install::kEpisodeCount - 1)]) r = Hint(r, kCross, "Install");
        r = Hint(r, kUp | kDown, "Move");
        break;
      case kAboutTab: r = Hint(r, kCircle, "Back"); break;
    }
    // the second line: what works on every page
    float r2 = kPageR;
    if (tab_ != kHome) r2 = Hint(r2, kOptions, "Play", 1);
    {  // L1 and R1 together: the tabs
      constexpr float kSize = 23, kButton = 31, kGap = 12, kSpacing = 0.8f;
      const float cy = kHintsY + 40;
      const std::string text = Upper(T("Tab"));
      const float tw = TextWidth(Regular(), kSize, text, kSpacing);
      const float x = r2 - tw;
      TextMid(Regular(), kSize, x, cy, pal_.hint, text, 0, kSpacing);
      const float r1 = GlyphWidth(kR1, kButton), l1 = GlyphWidth(kL1, kButton);
      Glyph(kR1, x - kGap - r1 * 0.5f, cy, White(0.92f), kButton);
      Glyph(kL1, x - kGap - r1 - 8 - l1 * 0.5f, cy, White(0.92f), kButton);
    }
  }
  // a message: capitals at the footer's left, in the clock's place (as the
  // game's PAUSED)
  if (const float a = ToastShown(); a > 0)
    TextMid(Bold(), 28, kPageL + (1 - a) * 10, kHintsY, WithAlpha(pal_.toast, a), Upper(toast_), 0, 0.8f);
}

// A question over everything (restore a backup?): the screen goes dark and it
// sits in the middle, as the game's warnings, with its buttons under it.
void Hub::DrawConfirm() {
  Ease(confirm_in_, confirm_ ? 1.0f : 0.0f, 14);
  if (confirm_in_ < 0.01f) return;
  const float a = Smooth(confirm_in_);
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  dl_->AddRectFilled(ImVec2(0, 0), size, Black(0.97f * a));
  const int v0 = dl_->VtxBuffer.Size;
  const float w = 1080, cx = kW * 0.5f;
  const std::vector<std::string> lines = WrapLines(Regular(), 31, w, confirm_text_);
  const float line_h = 31 * 1.45f;
  const float h = 74 + float(lines.size()) * line_h + 40 + 40;
  float y = (kH - h) * 0.5f - 20 + (1 - a) * 16;
  TextMid(Bold(), 56, cx, y + 26, pal_.title, confirm_title_, 0.5f);
  y += 74;
  for (const std::string& l : lines) Text(Regular(), 31, cx, y, kText, l, 0.5f), y += line_h;
  y += 40;
  // the buttons, centred: Cross and what it does, then Circle's
  constexpr float kSize = 25, kButton = 33, kGap = 12, kBetween = 56, kSpacing = 0.8f;
  struct Choice {
    uint32_t button;
    std::string text;
  };
  std::vector<Choice> choices = {{kCross, Upper(confirm_yes_)}};
  if (confirm_yes_ != "OK") choices.push_back({kCircle, Upper(confirm_no_label_.empty() ? T("Cancel") : confirm_no_label_)});
  float total = 0;
  for (const Choice& c : choices) total += GlyphWidth(c.button, kButton) + kGap + TextWidth(Regular(), kSize, c.text, kSpacing);
  total += kBetween * float(choices.size() - 1);
  float x = cx - total * 0.5f;
  for (const Choice& c : choices) {
    const float gw = GlyphWidth(c.button, kButton);
    Glyph(c.button, x + gw * 0.5f, y + 16, White(0.92f), kButton);
    x += gw + kGap;
    TextMid(Regular(), kSize, x, y + 16, pal_.hint, c.text, 0, kSpacing);
    x += TextWidth(Regular(), kSize, c.text, kSpacing) + kBetween;
  }
  Fade(v0, a);
}

// ------------------------------------------------------------------- pages

void Hub::DrawHome() {
  const std::vector<int> games = HomeGames();
  if (games.size() > 1) {
    DrawHomePanels(games);
    return;
  }
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  // The picture fills the space between the rules (as the Map tab's map),
  // a little closer than it fits and drifting slowly from side to side, as
  // the game's own loading art does. X plays (the hint at the bottom).
  const float top = P(0, kRuleTop + 2).y, bottom = P(0, kRuleBottom).y;
  const Image& hero = a_.hero;
  if (hero) {
    const float breath = 0.5f - 0.5f * std::cos(time_ * 6.2832f / 90.0f);
    const float zoom = 1.10f + 0.04f * breath;
    const float band = size.x / std::max(1.0f, bottom - top), img = hero.width / hero.height;
    float uw = 1, vh = img / band;  // (the band is wider than the picture: its full width, a strip of its height)
    if (vh > 1) uw = 1 / vh, vh = 1;
    uw /= zoom, vh /= zoom;
    // (across most of the room the zoom leaves, there and back in a minute)
    const float cx = 0.5f + (1 - uw) * 0.5f * 0.9f * std::sin(time_ * 6.2832f / 60.0f);
    const float cy = 0.27f;  // (the face and the skyline, room over the head)
    const ImVec2 uv0(cx - uw * 0.5f, std::clamp(cy - vh * 0.5f, 0.0f, 1 - vh));
    const ImVec2 uv1(uv0.x + uw, uv0.y + vh);
    dl_->AddImage(hero.id, ImVec2(0, top), ImVec2(size.x, bottom), uv0, uv1);
    dl_->AddRectFilled(ImVec2(0, top), ImVec2(size.x, bottom), Black(0.22f));
    const float fade_to = P(1000, 0).x;
    dl_->AddRectFilledMultiColor(ImVec2(0, top), ImVec2(fade_to, bottom), Black(0.7f), Black(0), Black(0), Black(0.7f));
    dl_->AddRectFilledMultiColor(ImVec2(0, P(0, kRuleBottom - 200).y), ImVec2(size.x, bottom), Black(0), Black(0),
                                 Black(0.6f), Black(0.6f));
  }
  if (!state_.installed) {
    const float in = Smooth((time_ - 0.2f) / 0.6f);
    const int v0 = dl_->VtxBuffer.Size;
    TextMid(Bold(), 40, kPageL, kRuleBottom - 104, Rgba(255, 255, 255), T("Not installed yet"));
    TextMid(Regular(), 30, kPageL, kRuleBottom - 58, Rgba(200, 200, 200),
            T("Press X to set it up from your disc image and title update."));
    Fade(v0, in);
  }
}

namespace {
// Where the person is in each game's Home picture (hub/home: gta4, tlad,
// tbogt), across and down: the face, nudged toward the middle of the figure.
// A closed pillar is centred on it.
constexpr ImVec2 kHomeFocus[3] = {{0.468f, 0.16f}, {0.485f, 0.07f}, {0.452f, 0.13f}};
// Each logo's size (The Ballad of Gay Tony's is wide and heavy: smaller).
constexpr float kHomeLogoScale[3] = {1.0f, 1.0f, 0.8f};

// The fade up from the chosen pillar's foot, under its logo: dark under GTA
// IV's white logo, white under The Lost and Damned's red one (its picture is
// light), a deep blue under The Ballad of Gay Tony's neon.
ImU32 FootColor(int game) {
  return game == 1 ? Rgba(255, 255, 255) : game == 2 ? Rgba(10, 16, 60) : Rgba(0, 0, 0);
}

// Each game's colour, for the glow round its pillar when its logo and
// picture have none.
ImU32 GameColor(int game) {
  return game == 1 ? Rgba(214, 92, 38)     // The Lost and Damned: rust
         : game == 2 ? Rgba(232, 72, 150)  // The Ballad of Gay Tony: pink
                     : kAccent;            // GTA IV: the menu's gold
}

// Quick, then settling (a choice opening).
float EaseOut(float t) {
  const float u = 1 - std::clamp(t, 0.0f, 1.0f);
  return 1 - u * u * u * u;
}

// The part of a convex polygon inside a convex quad, both clockwise on the
// screen (Sutherland-Hodgman).
std::vector<ImVec2> ClipToQuad(std::vector<ImVec2> poly, const ImVec2 (&quad)[4]) {
  for (int e = 0; e < 4 && !poly.empty(); ++e) {
    const ImVec2 a = quad[e], b = quad[(e + 1) % 4];
    auto side = [&](ImVec2 p) { return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x); };
    std::vector<ImVec2> out;
    for (size_t i = 0; i < poly.size(); ++i) {
      const ImVec2 p = poly[i], q = poly[(i + 1) % poly.size()];
      const float sp = side(p), sq = side(q);
      if (sp >= 0) out.push_back(p);
      if ((sp >= 0) != (sq >= 0)) {
        const float t = sp / (sp - sq);
        out.emplace_back(p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t);
      }
    }
    poly = std::move(out);
  }
  return poly;
}
}  // namespace

// Home once an episode is installed: a pillar for each game, side by side
// across the band between the rules, all leaning the same way (as the
// Complete Edition's cover). The chosen one opens to twice the others' width,
// in colour, its logo over a fade at its foot, a glow in the logo's colour at
// its edges that breathes, and now and then a glare across it; the others are
// narrower, black and white and dimmer, each centred on its person. The
// chosen one's picture moves as the game's loading art does (closer and back,
// side to side, slowly). A choice opens quickly and settles (EaseOut), its
// glow flashing; a press past the last pillar nudges it. Left and Right
// choose, X plays.
void Hub::DrawHomePanels(const std::vector<int>& games) {
  constexpr float kOpen = 1.0f;     // (the chosen one's extra width, in the others' widths)
  constexpr float kLean = 70.0f;    // (how far a pillar's top leans right, at 1080p: about 6 degrees)
  constexpr float kSwitch = 0.42f;  // (seconds a choice takes to open)
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  const float top = P(0, kRuleTop + 2).y, bottom = P(0, kRuleBottom).y;
  const float gap = std::round(10 * s_), lean = kLean * s_;
  const int chosen = HomeGame();
  if (chosen != home_shown_) {  // (a new choice: each pillar from where it is now)
    for (int g = 0; g < 3; ++g) home_from_[g] = home_wide_[g];
    home_shown_ = chosen, home_switch_ = 0, home_pop_ = 1, home_glare_ = 0;
  }
  home_switch_ = std::min(1.0f, home_switch_ + dt_ / kSwitch);
  home_nudge_ = std::min(1.0f, home_nudge_ + dt_ / 0.32f);
  home_glare_ += dt_;
  Ease(home_pop_, 0, 4);
  const float open = EaseOut(home_switch_);
  float shares = 0;
  for (const int g : games) {
    home_wide_[g] = home_from_[g] + ((g == chosen ? 1.0f : 0.0f) - home_from_[g]) * open;
    shares += 1 + home_wide_[g] * kOpen;
  }
  // (the first pillar's foot starts off the screen, so its top meets the screen's edge)
  const float room = size.x + lean - gap * float(games.size() - 1);
  dl_->PushClipRect(ImVec2(0, top), ImVec2(size.x, bottom), true);
  float x = -lean;
  for (const int g : games) {
    const float w = room * (1 + home_wide_[g] * kOpen) / shares;
    DrawHomePanel(g, x, x + w, top, bottom, lean, Smooth(home_wide_[g]));
    x += w + gap;
  }
  dl_->PopClipRect();
}

// A quad in four colours (top left, top right, bottom right, bottom left).
void Hub::Quad(ImVec2 tl, ImVec2 tr, ImVec2 br, ImVec2 bl, ImU32 ctl, ImU32 ctr, ImU32 cbr, ImU32 cbl) {
  const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  const ImDrawIdx i = ImDrawIdx(dl_->_VtxCurrentIdx);
  dl_->PrimReserve(6, 4);
  dl_->PrimWriteIdx(i), dl_->PrimWriteIdx(ImDrawIdx(i + 1)), dl_->PrimWriteIdx(ImDrawIdx(i + 2));
  dl_->PrimWriteIdx(i), dl_->PrimWriteIdx(ImDrawIdx(i + 2)), dl_->PrimWriteIdx(ImDrawIdx(i + 3));
  dl_->PrimWriteVtx(tl, uv, ctl), dl_->PrimWriteVtx(tr, uv, ctr);
  dl_->PrimWriteVtx(br, uv, cbr), dl_->PrimWriteVtx(bl, uv, cbl);
}

// A convex polygon, each corner its colour.
void Hub::FillPoly(const std::vector<ImVec2>& poly, const std::function<ImU32(ImVec2)>& color) {
  const int n = int(poly.size());
  if (n < 3) return;
  const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  const ImDrawIdx i = ImDrawIdx(dl_->_VtxCurrentIdx);
  dl_->PrimReserve((n - 2) * 3, n);
  for (int k = 1; k + 1 < n; ++k)
    dl_->PrimWriteIdx(i), dl_->PrimWriteIdx(ImDrawIdx(i + k)), dl_->PrimWriteIdx(ImDrawIdx(i + k + 1));
  for (const ImVec2& p : poly) dl_->PrimWriteVtx(p, uv, color(p));
}

// A pillar: its foot from x0 to x1 on the lower rule, its top `lean` over.
void Hub::DrawHomePanel(int game, float x0, float x1, float top, float bottom, float lean, float on) {
  const Assets::HomeArt& art = a_.home[game];
  const float h = bottom - top;
  const ImVec2 tl(x0 + lean, top), tr(x1 + lean, top), br(x1, bottom), bl(x0, bottom);
  const ImVec2 pillar[4] = {tl, tr, br, bl};
  // (a point up its sides: t 0 at the foot, 1 at the top)
  auto side = [&](float x, float t) { return ImVec2(x + lean * t, bottom - h * t); };
  // a press past the last pillar: the chosen one's picture and logo nudge that way and back
  const float nudge = on * float(home_nudge_dir_) * 16 * s_ * std::sin(3.1416f * home_nudge_) * (1 - home_nudge_);
  if (art.picture) {
    // the picture covers the pillar (upright, the pillar cut from it), a
    // little closer than it fits, the person in the middle (kHomeFocus);
    // the chosen one moving in the room that leaves
    const float left = std::min(x0, x0 + lean), width = x1 - x0 + std::fabs(lean);
    const float phase = float(game) * 2.1f;
    const float breath = 0.5f - 0.5f * std::cos(time_ * 6.2832f / 26.0f + phase);
    const float zoom = 1.08f + on * (0.04f + 0.05f * breath + 0.03f * home_pop_);
    const float shape = width / std::max(1.0f, h), picture = art.picture.width / std::max(1.0f, art.picture.height);
    float uw = 1, vh = 1;
    if (picture > shape) uw = shape / picture;
    else vh = picture / shape;
    uw /= zoom, vh /= zoom;
    const ImVec2 focus = kHomeFocus[game];
    // down: the face a fifth of the way down
    const float v0 = std::clamp(focus.y - 0.2f * vh + on * 0.012f * std::sin(time_ * 6.2832f / 41.0f + phase), 0.0f,
                                1 - vh);
    // across: the face in the middle of the pillar's part on the screen, at
    // the face's height (the pillar leans)
    const float face = (focus.y - v0) / vh;  // (0 at the top)
    const float screen_w = ImGui::GetIO().DisplaySize.x;
    const float middle = (std::max(0.0f, x0 + lean * (1 - face)) + std::min(screen_w, x1 + lean * (1 - face))) * 0.5f;
    const float sway = on * 0.06f * std::sin(time_ * 6.2832f / 34.0f + phase) - nudge / width;
    const float u0 = std::clamp(focus.x - (middle - left) / width * uw + sway * uw, 0.0f, 1 - uw);
    const ImVec2 uv0(u0, v0);
    auto uv = [&](ImVec2 p) { return ImVec2(uv0.x + (p.x - left) / width * uw, uv0.y + (p.y - top) / h * vh); };
    if (!art.grey || on < 0.999f)
      dl_->AddImageQuad((art.grey ? art.grey : art.picture).id, tl, tr, br, bl, uv(tl), uv(tr), uv(br), uv(bl));
    if (art.grey && on > 0.001f)
      dl_->AddImageQuad(art.picture.id, tl, tr, br, bl, uv(tl), uv(tr), uv(br), uv(bl), White(on));
  } else {
    // (no picture: a dark pillar)
    Quad(tl, tr, br, bl, Rgba(38, 38, 42), Rgba(38, 38, 42), Rgba(12, 12, 14), Rgba(12, 12, 14));
  }
  // the others dimmer; under the chosen one's logo, a fade up from its foot
  dl_->AddQuadFilled(tl, tr, br, bl, Black(0.5f * (1 - on)));
  const ImU32 foot = FootColor(game);
  Quad(side(x0, 0.55f), side(x1, 0.55f), br, bl, WithAlpha(foot, 0), WithAlpha(foot, 0), WithAlpha(foot, 0.82f * on),
       WithAlpha(foot, 0.82f * on));
  // the logo (the game's name without one), at the foot of the chosen one,
  // a touch bigger as it's chosen
  if (on > 0.01f) {
    const float cx = ((x0 + x1) * 0.5f + lean * 0.22f + nudge - ox_) / s_, hw = (x1 - x0) / s_;
    const float foot_y = (bottom - oy_) / s_ - 56;
    const float scale = kHomeLogoScale[game] * (1 + 0.05f * home_pop_);
    if (art.logo) {
      const float lw = std::min(hw * 0.72f, 620.0f) * scale;
      const float lh = std::min(lw * art.logo.height / art.logo.width, 230.0f * scale);
      const float fw = lh * art.logo.width / art.logo.height;
      dl_->AddImage(art.logo.id, P(cx - fw * 0.5f, foot_y - lh), P(cx + fw * 0.5f, foot_y), ImVec2(0, 0),
                    ImVec2(1, 1), White(on));
    } else {
      const std::string name = game ? T(install::EpisodeName(game - 1)) : "Grand Theft Auto IV";
      TextMid(Bold(), 52, cx, foot_y - 30, White(on), Fit(Bold(), 52, name, hw - 80), 0.5f);
    }
  }
  // a glare across the chosen one: just after it's chosen, then every few
  // seconds, a soft band of light steeper than the pillar, cut by its sides
  if (on > 0.01f) {
    constexpr float kEvery = 5.5f, kSweep = 0.9f;
    const float t = std::fmod(std::max(0.0f, home_glare_ - 0.12f), kEvery) / kSweep;
    if (t < 1) {
      // (a wide soft band and a bright narrow one in its middle)
      const float g = h * 0.55f, wide = 0.12f * (x1 - x0) + 40 * s_, core = 0.022f * (x1 - x0) + 8 * s_;
      const float c = (x0 - g - wide) + (x1 + lean + 2 * wide + g - x0) * Smooth(t);
      auto u = [&](ImVec2 p) { return p.x - g * (bottom - p.y) / h; };
      for (const auto& [half, peak] : {std::pair{wide, 0.2f}, std::pair{core, 0.3f}}) {
        auto light = [&](ImVec2 p) { return White(peak * on * std::max(0.0f, 1 - std::fabs(u(p) - c) / half)); };
        for (const float a : {c - half, c}) {
          const float b = a + half;
          FillPoly(ClipToQuad({ImVec2(a + g, top), ImVec2(b + g, top), ImVec2(b, bottom), ImVec2(a, bottom)}, pillar),
                   light);
        }
      }
    }
  }
  // the glow round the chosen one, in its colour: bright at the edge, fading
  // in, breathing, and flashing as it's chosen
  if (on > 0.01f) {
    const ImU32 col = art.color ? art.color : GameColor(game);
    const float pulse = 0.85f + 0.15f * std::sin(time_ * 6.2832f / 2.6f);
    const float strength = on * std::min(1.0f, pulse + 0.6f * home_pop_);
    const float step = std::max(1.0f, 1.5f * s_);
    const int rings = 22 + int(10 * home_pop_);
    for (int k = 0; k < rings; ++k) {
      const float t = 1 - float(k) / float(rings), in = k * step, up = in / h;
      auto at = [&](float x, float t, float dx) { const ImVec2 p = side(x, t); return ImVec2(p.x + dx, p.y); };
      const ImVec2 ring[4] = {at(x0, 1 - up, in), at(x1, 1 - up, -in), at(x1, up, -in), at(x0, up, in)};
      dl_->AddPolyline(ring, 4, WithAlpha(col, strength * (k ? 0.75f * t * t : 1.0f)), ImDrawFlags_Closed,
                       k ? step : 2 * step);
    }
  }
}

namespace {
bool IsPreset(const Hub::Setting& r) { return std::string_view(r.key) == "hub_display_preset"; }

// What each preset sets (Recommended, Performance, Quality). Recommended is
// every one's own default. Frame Generation and the Debug options' Display
// rows aren't in them: hidden, those stay at their recommended values.
struct PresetValue {
  const char* key;
  const char* value;
};
const std::vector<PresetValue>& PresetValues(int preset) {
  static const std::vector<PresetValue> kPresets[3] = {
      {{"resolution", "4k"},
       {"gta4_draw_distance_scale", "1.0"},
       {"ps5_population", "1.0"},
       {"gta4_force_highest_lod", "false"},
       {"gta4_frame_limit", "60"},
       {"gta4_native_anti_aliasing", "fxaa"},
       {"gta4_anisotropic_filtering", "1x"},
       {"gta4_modern_shaders", "false"},
       {"gta4_reflection_resolution", "1080p"},
       {"gta4_shadow_map_base_size", "512"},
       {"gta4_shadow_distance_scale", "1.0"}},
      {{"resolution", "1440p"},
       {"gta4_draw_distance_scale", "1.0"},
       {"ps5_population", "0.75"},
       {"gta4_force_highest_lod", "false"},
       {"gta4_frame_limit", "60"},
       {"gta4_native_anti_aliasing", "fxaa"},
       {"gta4_anisotropic_filtering", "1x"},
       {"gta4_modern_shaders", "false"},
       {"gta4_reflection_resolution", "original"},
       {"gta4_shadow_map_base_size", "512"},
       {"gta4_shadow_distance_scale", "1.0"}},
      {{"resolution", "4k"},
       {"gta4_draw_distance_scale", "1.5"},
       {"ps5_population", "1.0"},
       {"gta4_force_highest_lod", "true"},
       {"gta4_frame_limit", "60"},
       {"gta4_native_anti_aliasing", "smaa"},
       {"gta4_anisotropic_filtering", "16x"},
       {"gta4_modern_shaders", "true"},
       {"gta4_reflection_resolution", "1080p"},
       {"gta4_shadow_map_base_size", "1024"},
       {"gta4_shadow_distance_scale", "2.0"}},
  };
  return kPresets[std::clamp(preset, 0, 2)];
}
const Hub::Setting* FindSetting(std::string_view key) {
  for (const Hub::Setting& r : Settings())
    if (key == r.key) return &r;
  return nullptr;
}
}  // namespace

int Hub::PresetNow() const {
  for (int p = 0; p < kPresetCustom; ++p) {
    bool all = true;
    for (const PresetValue& pv : PresetValues(p)) {
      const Setting* r = FindSetting(pv.key);
      if (!r) continue;
      const std::string want = pv.value;
      const int k = SettingIndex(*r);
      if (k < 0 || k >= int(r->values.size()) || want != r->values[size_t(k)]) {
        all = false;
        break;
      }
    }
    if (all) return p;
  }
  return kPresetCustom;
}

void Hub::ApplyPreset(int preset) {
  for (const PresetValue& pv : PresetValues(preset)) {
    const Setting* r = FindSetting(pv.key);
    if (!r) continue;
    const std::string v = pv.value;
    Save(r->key, r->quoted ? "\"" + v + "\"" : v);
  }
}

int Hub::SettingIndex(const Setting& r) const {
  if (IsPreset(r)) return PresetNow();
  const std::string v = Value(r.key, r.fallback);
  for (size_t k = 0; k < r.values.size(); ++k)
    if (v == r.values[k]) return int(k);
  // (a number the game wrote itself: "1.000000" for "1.0")
  if (char* end = nullptr; !r.quoted && !v.empty()) {
    const double n = std::strtod(v.c_str(), &end);
    if (end && *end == '\0')
      for (size_t k = 0; k < r.values.size(); ++k)
        if (std::fabs(std::strtod(r.values[k], nullptr) - n) < 1e-6 && std::isdigit((unsigned char)r.values[k][0]))
          return int(k);
  }
  for (size_t k = 0; k < r.values.size(); ++k)
    if (std::string(r.fallback) == r.values[k]) return int(k);
  return 0;
}

std::vector<int> Hub::SettingsOn(int tab) const {
  std::vector<int> out;
  const auto& set = Settings();
  for (int i = 0; i < int(set.size()); ++i) {
    const std::string_view section = set[size_t(i)].section;
    const int t = section == "DISPLAY" ? kDisplayTab : section == "DEBUG" ? kDebugTab : kGameTab;
    if (t == tab && Shown(set[size_t(i)])) out.push_back(i);
  }
  return out;
}

namespace {
// A level (Menu Music): drawn as a bar, as the game's SFX and music levels.
bool IsLevel(const Hub::Setting& r) { return std::string_view(r.key) == "hub_music_volume"; }
bool IsLanguage(const Hub::Setting& r) { return std::string_view(r.key) == "ps5_language"; }
// A value as shown: Auto says which language it is.
std::string ValueLabel(const Hub::Setting& r, int k) {
  if (IsLanguage(r) && std::string_view(r.values[size_t(k)]) == "auto")
    return std::string(T("Auto")) + " (" + lang::Name(lang::System()) + ")";
  return T(r.labels[size_t(k)]);
}
}  // namespace

// Display, Game and Debug: the game's settings page - a label, its value
// across from it - and the focused one explained at the right.
void Hub::DrawSettings() {
  constexpr float kRowH = 46, kSize = 34;
  const auto& set = Settings();
  const std::vector<int> on = SettingsOn(tab_);
  if (on.empty()) return;
  const int sel = item_[tab_] = std::clamp(item_[tab_], 0, int(on.size()) - 1);
  std::vector<Row> rows;
  for (int i = 0; i < int(on.size()); ++i) rows.push_back({i});
  List(rows, sel, kRowH, [&](int i, float ry, float over) {
    const auto& r = set[size_t(on[size_t(i)])];
    const float cy = ry + kRowH * 0.5f;
    const ImU32 col = Mix(pal_.row, pal_.focus, over);
    // (a long name, in some languages: smaller, to stay clear of its value)
    const std::string label = T(r.label);
    const float room = kValueX - kListX - 28, lw = TextWidth(Bold(), kSize, label);
    TextMid(Bold(), lw > room ? std::max(24.0f, kSize * room / lw) : kSize, kListX, cy, col, label);
    const int idx = SettingIndex(r);
    if (IsLevel(r)) {
      const float bw = 280, bh = 18, by = cy - bh * 0.5f;
      const float f = r.values.size() > 1 ? float(idx) / float(r.values.size() - 1) : 0;
      Rect(kValueX, by, bw, bh, kBarBack);
      if (f > 0) Rect(kValueX, by, bw * f, bh, Mix(Rgba(206, 206, 206), pal_.focus, over));
      return;
    }
    // the value just changed: it slides in from the side it was changed toward
    const float in = i == sel ? Smooth(value_in_) : 1;
    const int v0 = dl_->VtxBuffer.Size;
    TextMid(Bold(), kSize, kValueX + (1 - in) * 16 * value_dir_, cy, col, ValueLabel(r, idx));
    Fade(v0, 0.2f + 0.8f * in);
  });
  // the focused setting, explained, and its choices
  const auto& r = set[size_t(on[size_t(sel)])];
  Panel([&](float px, float py, float pw) {
    py += Wrapped(Regular(), 29, px, py + 6, pw, Rgba(196, 196, 196), T(r.help), 1.32f) + 34;
    if (std::string_view(r.key) == "ps5_refresh_rate" && display_.high_refresh) {
      const char* now = !display_.high_refresh()               ? "60 Hz"
                        : display_.vrr && display_.vrr() ? "120 Hz with VRR"
                                                         : "120 Hz";
      TextMid(Bold(), 29, px, py + 18, kText, F("Right now: {}", {T(now)}));
      py += 44 + 20;
    }
    if (IsLevel(r) || r.labels.size() > 6) return;
    const int cur = SettingIndex(r);
    for (int k = 0; k < int(r.labels.size()); ++k) {
      TextMid(Bold(), 29, px, py + 18, k == cur ? pal_.focus : kDimGray, ValueLabel(r, k));
      py += 44;
    }
  });
}

#if defined(__EMSCRIPTEN__)
// [web port] the saves in and out of the browser (web/web_saves_transfer.cpp)
namespace web_saves {
bool Zip(const fs::path& root, const std::vector<fs::path>& folders, std::vector<uint8_t>& zip, int* files);
bool Unzip(const std::vector<uint8_t>& zip, const fs::path& root, int* files, std::string* error);
void Offer(const std::vector<uint8_t>& zip, const std::string& name);
void Pick();
bool TakeUpload(std::vector<uint8_t>& bytes);
void PickXbox360();
bool TakeXbox360Picked();
}  // namespace web_saves
#endif

void Hub::DrawSaves() {
  constexpr float kRowH = 46, kSize = 33, kSavesValueX = 840;
  const int nb = int(backups_.size()), nf = int(save_items_.size());
  const int n = kSaveActions + nb + nf;
#if defined(__EMSCRIPTEN__)
  // (a file the player picked for Load Saves From a File: the saves now
  // backed up, then the file's unpacked over them)
  if (std::vector<uint8_t> upload; web_saves::TakeUpload(upload)) {
    BackUpSaves(" (before loading a file)");
    int files = 0;
    std::string error;
    if (web_saves::Unzip(upload, paths_.user_data, &files, &error)) {
      LoadSaves();
      Play(Sound::kSelect);
      Notice(T("Saves loaded"), F("{} files from the zip are in your saves. The ones you had are in SaveBackups.",
                                  {std::to_string(files)}));
    } else {
      Play(Sound::kError);
      Notice(T("The saves couldn't be loaded"), error);
    }
  }
  // (Xbox 360 save packages the player picked for Import Xbox 360 Saves)
  if (web_saves::TakeXbox360Picked() && save_import_.run) {
    Play(Sound::kSelect);
    BackUpSaves(" (before import)");
    const std::string message = save_import_.run();
    LoadSaves();
    Notice(T("Xbox 360 saves"), message);
  }
#endif
  const int sel = item_[tab_] = std::clamp(item_[tab_], 0, n - 1);
  std::vector<Row> rows;
  for (int i = 0; i < kSaveActions; ++i) rows.push_back({i});
  if (nb) {
    rows.push_back({-1, T("Restore a backup")});
    for (int i = 0; i < nb; ++i) rows.push_back({kSaveActions + i});
  }
  if (nf) {
    rows.push_back({-1, F("Your saves  {}", {std::to_string(nf)})});
    for (int i = 0; i < nf; ++i) rows.push_back({kSaveActions + nb + i});
  }
  List(rows, sel, kRowH, [&](int i, float ry, float over) {
    std::string name, value;
    if (i == 0) {
      name = T("Back Up Now");
    } else if (i == 1) {
      name = T("Import Xbox 360 Saves");
#if defined(__EMSCRIPTEN__)
      value = T("Choose Files");
#else
      value = pending_imports_ ? F("{} waiting", {std::to_string(pending_imports_)}) : T("None waiting");
#endif
    } else if (i == 2 && kSaveActions > 2) {
      name = T("Download Saves");
    } else if (i == 3 && kSaveActions > 3) {
      name = T("Load Saves From a File");
    } else if (i < kSaveActions + nb) {
      const Backup& b = backups_[size_t(i - kSaveActions)];
      name = b.name, value = Size(b.bytes);
    } else {
      const SaveItem& f = save_items_[size_t(i - kSaveActions - nb)];
      name = T(f.name.c_str()), value = T(f.kind.c_str());
    }
    const float cy = ry + kRowH * 0.5f;
    const ImU32 col = Mix(pal_.row, pal_.focus, over);
    TextMid(Bold(), kSize, kListX, cy, col, Fit(Bold(), kSize, name, kSavesValueX - kListX - 30));
    if (!value.empty()) TextMid(Bold(), kSize, kSavesValueX, cy, col, Fit(Bold(), kSize, value, kPanelX - 50 - kSavesValueX));
  });
  Panel([&](float px, float py, float pw) {
    const ImU32 text = Rgba(196, 196, 196);
    if (sel == 0) {
      Wrapped(Regular(), 29, px, py + 6, pw, text,
              T("Copies all of your GTA IV saves into SaveBackups, named by the date and time."), 1.38f);
    } else if (sel == 1) {
#if defined(__EMSCRIPTEN__)
      Wrapped(Regular(), 29, px, py + 6, pw, text,
              T("Bring your Xbox 360 progress over. Copy the save files off your 360 (on its drive they're in "
                "Content/<your profile>/545407F2/00000001) onto this computer, then press Enter and pick them. "
                "They're imported into this browser only, and your saves are backed up first."), 1.38f);
      return;
#endif
      py += Wrapped(Regular(), 29, px, py + 6, pw, text,
                    T("Bring your Xbox 360 progress over. Copy the save files from your 360 (on its drive they're "
                      "in Content/<your profile>/545407F2/00000001) into this folder on the PS5:"), 1.38f) + 22;
      py += Wrapped(Bold(), 28, px, py, pw, pal_.title, T("/data/homebrew/PPSA94004/import/")) + 26;
      Wrapped(Regular(), 27, px, py, pw, kMenuGray,
              T("Press Cross to import them. Your saves are backed up first, and imported files move to "
                "import/done."), 1.38f);
    } else if (sel == 2 && kSaveActions > 2) {
      Wrapped(Regular(), 29, px, py + 6, pw, text,
              T("Saves your GTA IV saves into one zip file your browser downloads. Your saves live in this "
                "browser's storage, so keep a copy: clearing the site's data removes them, backups too."), 1.38f);
    } else if (sel == 3 && kSaveActions > 3) {
      Wrapped(Regular(), 29, px, py + 6, pw, text,
              T("Puts the saves from a zip made with Download Saves back, on this browser or another. The saves "
                "you have now are backed up first."), 1.38f);
    } else if (sel < kSaveActions + nb) {
      const Backup& b = backups_[size_t(sel - kSaveActions)];
      py += Wrapped(Bold(), 36, px, py, pw, kText, b.name, 1.15f) + 22;
      py = Facts(px, py, pw, {{T("Files"), std::to_string(b.files)}, {T("Size"), Size(b.bytes)}});
      Wrapped(Regular(), 27, px, py + 16, pw, kMenuGray,
              T("Restoring puts these saves back. The saves you have now are backed up first."), 1.38f);
    } else {
      const SaveItem& f = save_items_[size_t(sel - kSaveActions - nb)];
      py += Wrapped(Bold(), 36, px, py, pw, kText, T(f.name.c_str()), 1.15f) + 22;
      Facts(px, py, pw,
            {{T("Type"), T(f.kind.c_str())},
             {T("Files"), std::to_string(f.files)},
             {T("Size"), Size(f.bytes)},
             {T("Changed"), Date(f.modified)}});
    }
  });
}

// The episodes: each, and whether it's installed; the focused one explained,
// and where its package goes. Cross installs it (the installer).
void Hub::DrawEpisodes() {
  constexpr float kRowH = 46, kSize = 33, kEpisodeValueX = 760;
  const int n = install::kEpisodeCount;
  const int sel = item_[tab_] = std::clamp(item_[tab_], 0, n - 1);
  std::vector<Row> rows;
  for (int i = 0; i < n; ++i) rows.push_back({i});
  auto status = [&](int e) {
    return T(state_.episode[e] ? "Installed" : state_.episode_found[e] ? "Ready to install|episode" : "Not installed");
  };
  List(rows, sel, kRowH, [&](int i, float ry, float over) {
    const float cy = ry + kRowH * 0.5f;
    const ImU32 col = Mix(pal_.row, pal_.focus, over);
    TextMid(Bold(), kSize, kListX, cy, col, Fit(Bold(), kSize, T(install::EpisodeName(i)), kEpisodeValueX - kListX - 30));
    TextMid(Bold(), kSize, kEpisodeValueX, cy, col, Fit(Bold(), kSize, status(i), kPanelX - 50 - kEpisodeValueX));
  });
  Panel([&](float px, float py, float pw) {
    const ImU32 text = Rgba(196, 196, 196);
    py += Wrapped(Bold(), 36, px, py, pw, kText, T(install::EpisodeName(sel)), 1.15f) + 18;
    py += Wrapped(Regular(), 29, px, py, pw, text,
                  T(sel == 0 ? "Johnny Klebitz and The Lost MC. The first of the two episodes."
                             : "Luis Lopez and nightclub owner Tony Prince. The second of the two episodes."),
                  1.38f) + 26;
    if (state_.episode[sel]) {
      Wrapped(Regular(), 27, px, py, pw, kMenuGray,
              T("Installed. When the game starts it asks if you want to play it, the first time."), 1.38f);
      return;
    }
    if (!state_.installed) {
      Wrapped(Regular(), 27, px, py, pw, kMenuGray,
              T("Install GTA IV first. The episodes are added to the installed game."), 1.38f);
      return;
    }
    py += Wrapped(Regular(), 27, px, py, pw, kMenuGray,
                  T("Copy the episode's package from your Xbox 360 (on its drive it's in "
                    "Content/0000000000000000/545407F2/00000002) into this folder on the PS5:"),
                  1.38f) + 22;
    py += Wrapped(Bold(), 28, px, py, pw, pal_.title, T("/data/homebrew/PPSA94004/install/")) + 26;
    Wrapped(Regular(), 27, px, py, pw, kMenuGray,
            T(state_.episode_found[sel] ? "Its package is there. Press Cross to install it."
                                        : "Then press Cross to install it."),
            1.38f);
  });
}

void Hub::DrawAbout() {
  struct Credit {
    const char* who;
    const char* what;
  };
  const Credit people[] = {
      {"OZORDI", "Created Liberty Recompiled, the GTA IV recompilation this is built on, with its contributors."},
      {"@elripalda", "Brought it to PlayStation 5: the PS5 port and this hub."},
      {"vaduur", "LibertyRecompAndroid: the fixes for running it on handheld hardware."},
      {"ReXGlue", "The static recompiler and runtime that turn the Xbox 360 game into a native program."},
#if defined(__EMSCRIPTEN__)
      {"itzzace", "The web port: GTA IV in a browser with WebAssembly and WebGPU, its renderer, controls, prompts, "
                  "saves and this page."},
#endif
  };
  float y = kListY + 6;
  for (const Credit& c : people) {
    TextMid(Bold(), 35, kListX, y + 18, kText, c.who);
    y += 46;
    y += Wrapped(Regular(), 28, kListX, y, 860, kMenuGray, T(c.what), 1.35f) + 30;
  }
  const float px = kPanelX;
  float py = kPanelY + 4;
  TextMid(Bold(), 21, px, py + 12, kDimGray, Upper(T("Built With")), 0, 1.6f);
  py += 44;
#if defined(__EMSCRIPTEN__)
  const char* parts[] = {"Emscripten (WebAssembly)", "WebGPU", "naga (shaders to WGSL)", "Dear ImGui",
                         "stb_image, stb_vorbis, dr_mp3 and dr_flac", "TeX Gyre Heros (GUST Font License)",
                         "Oswald and Inter (SIL Open Font License)"};
#else
  const char* parts[] = {"PS5 Payload SDK", "Mesa RADV (Vulkan)", "Dear ImGui", "stb_image, stb_vorbis, dr_mp3 and dr_flac",
                         "TeX Gyre Heros (GUST Font License)", "Oswald and Inter (SIL Open Font License)"};
#endif
  for (const char* part : parts) {
    TextMid(Regular(), 28, px, py + 14, Rgba(196, 196, 196), T(part));
    py += 40;
  }
  py += 26;
  TextMid(Bold(), 21, px, py + 12, kDimGray, Upper(T("Please Note")), 0, 1.6f);
  py += 44;
  Wrapped(Regular(), 26, px, py, kPanelW, kMenuGray,
          T("An unofficial fan project, not affiliated with or endorsed by Rockstar Games, Take-Two "
            "Interactive, Microsoft or Sony. Grand Theft Auto and all related names belong to their owners. No game "
            "files are included: you need your own copy of GTA IV for Xbox 360."),
          1.36f);
  if (!version_.empty()) TextMid(Regular(), 24, px, kRuleBottom - 34, kDimGray, F("Version {}", {VersionLabel(version_)}));
  // Cross held here: a line filling under the version until the debug
  // options switch (Navigate), once it has been held a moment.
  if (about_hold_ > kUnlockShow && !about_hold_done_) {
    const float f = std::clamp((about_hold_ - kUnlockShow) / (kUnlockHold - kUnlockShow), 0.0f, 1.0f);
    Rect(px, kRuleBottom - 16, 240, 3, Rgba(58, 58, 58));
    Rect(px, kRuleBottom - 16, 240 * f, 3, pal_.focus);
  }
}

// --------------------------------------------------------------- behaviour

namespace {
// The Display page's rarer settings: shown with the debug options (Frame
// Generation is experimental; the other two only trade a little picture for
// frame rate, and are set for it).
bool Advanced(const Hub::Setting& r) {
  const std::string_view key = r.key;
  return key == "ps5_frame_generation" || key == "gta4_native_shadow_min_indices" ||
         key == "ps5_environment_reflection_every";
}
}  // namespace

bool Hub::Shown(const Setting& r) const {
  return debug_options_ || (!Advanced(r) && std::string_view(r.section) != "DEBUG");
}

#if defined(__EMSCRIPTEN__)
// [web port] A slower computer starts with less to draw: the first time the
// hub runs in a browser, the settings that cut what each frame draws (and
// that the player hasn't set) get values for this machine's CPU threads and
// memory, as the browser reports them. Once a browser (web_device_defaults
// records the tier); the Display page changes them as any other.
// GTA4_WEB_DEVICE_TIER (low, medium, high) chooses the tier instead.
void Hub::ApplyWebDeviceDefaults() {
  if (!Value("web_device_defaults", "").empty()) return;
  const int threads = EM_ASM_INT({ return navigator.hardwareConcurrency | 0; });
  const int memory_tenths = EM_ASM_INT({ return Math.round((navigator.deviceMemory || 0) * 10); });
  std::string tier = "high";
  if ((threads && threads <= 4) || (memory_tenths && memory_tenths <= 40)) {
    tier = "low";
  } else if (threads && threads <= 8) {
    tier = "medium";
  }
  if (const char* forced = std::getenv("GTA4_WEB_DEVICE_TIER"); forced && *forced) tier = forced;
  struct Default {
    const char* tier;
    const char* key;
    const char* value;
  };
  // (Draw Distance Shorter - the city's skyline stays, from its distant
  // models: the arrival shot drew 2,000 things instead of 7,700 - and fewer
  // cars and people, the fewest for low: each draws less of every frame,
  // the web build's cost)
  static const Default kDefaults[] = {
      {"low", "gta4_draw_distance_scale", "0.75"},
      {"low", "ps5_population", "0.5"},
      {"medium", "gta4_draw_distance_scale", "0.75"},
      {"medium", "ps5_population", "0.75"},
  };
  for (const Default& d : kDefaults) {
    if (tier != d.tier || !Value(d.key, "").empty()) continue;
    Save(d.key, d.value);
  }
  Save("web_device_defaults", "\"" + tier + "\"");
  std::printf("web hub: %d threads, %.1f GB: %s settings\n", threads, memory_tenths / 10.0, tier.c_str());
}
#endif

// What isn't shown is at its recommended value: the Debug page's and the rarer
// Display settings (as Reset All sets them), and Light Color Fix, which no
// longer has a row (always on). Run at the start and when the debug options
// are hidden again.
void Hub::KeepHiddenSettings() {
  for (const Setting& r : Settings()) {
    if (Shown(r) || Value(r.key, r.fallback) == r.fallback) continue;
    Save(r.key, r.quoted ? std::string("\"") + r.fallback + "\"" : std::string(r.fallback));
    if (std::string_view(r.key) == "ps5_frame_generation") {
      Save("gta4_native_frame_generation", "\"off\"");
      Save("gta4_native_upscaler", "\"native\"");
      Save("gta4_temporal_upscaler_quality", "\"native\"");
    }
  }
  // (frame generation off for everyone, whatever turned it on before)
  if (Value("ps5_frame_generation", "off") != "off" || Value("gta4_native_frame_generation", "off") != "off" ||
      Value("gta4_native_upscaler", "native") != "native") {
    Save("ps5_frame_generation", "\"off\"");
    Save("gta4_native_frame_generation", "\"off\"");
    Save("gta4_native_upscaler", "\"native\"");
    Save("gta4_temporal_upscaler_quality", "\"native\"");
  }
  if (Value("gta4_native_guest_texture_swizzle", "false") != "false")
    Save("gta4_native_guest_texture_swizzle", "false");
}

void Hub::SetDebugOptions(bool on) {
  debug_options_ = on;
  Save("hub_debug_options", on ? "true" : "false");
  if (!on) KeepHiddenSettings();
  value_in_ = 0;
  Play(on ? Sound::kOn : Sound::kOff);
  Toast(T(on ? "Debug options on" : "Debug options off"));
}

// "1.0t1" -> "1.0 Test 1", "0.5b7" -> "0.5 Beta 7".
std::string Hub::VersionLabel(const std::string& v) {
  const size_t at = v.find_first_of("tb");
  if (at == std::string::npos || at == 0 || at + 1 >= v.size()) return v;
  return v.substr(0, at) + (v[at] == 't' ? " Test " : " Beta ") + v.substr(at + 1);
}

int Hub::Items() const {
  switch (tab_) {
    case kDisplayTab:
    case kGameTab:
    case kDebugTab: return int(SettingsOn(tab_).size());
    case kSavesTab: return kSaveActions + int(backups_.size()) + int(save_items_.size());
    case kEpisodesTab: return install::kEpisodeCount;
    case kTrophiesTab: return int(trophy_items_.size());
    default: return 0;
  }
}

void Hub::ResetSettings() {
  for (const Setting& r : Settings()) {
    if (IsLanguage(r)) continue;  // (a choice, not a recommendation: it stays)
    if (IsPreset(r)) continue;    // (the rest's defaults are Recommended)
    Save(r.key, r.quoted ? std::string("\"") + r.fallback + "\"" : std::string(r.fallback));
  }
  // (what frame generation's setting sets with it: off)
  Save("gta4_native_frame_generation", "\"off\"");
  Save("gta4_native_upscaler", "\"native\"");
  Save("gta4_temporal_upscaler_quality", "\"native\"");
  value_in_ = 0;
  Play(Sound::kSelect);
  Toast(T("Settings reset"));
  if (display_.high_refresh && display_.high_refresh()) display_.set_high_refresh(false);
}

void Hub::SwitchRefresh(bool high) {
  if (display_.set_high_refresh(high)) {
    Toast(high ? T("Now at 120 Hz") : T("Back to 60 Hz"));
    return;
  }
  if (!high) return;
  Save("ps5_refresh_rate", "60");  // (it would only be refused again at the next start)
  if (Value("ps5_frame_generation", "off") == "60-120") Save("ps5_frame_generation", "\"30-60\"");
  value_in_ = 0;
  Play(Sound::kError);
  Notice(T("Still at 60 Hz"),
         T("The TV didn't take 120 Hz. Check that 120 Hz output is on in the PS5's settings (Screen and Video, "
           "Video Output) and that the TV runs 120 Hz at the resolution the PS5 sends it. 4K at 120 Hz needs "
           "HDMI 2.1."));
}

void Hub::Activate(int dir) {
  const int item = item_[tab_];
  switch (tab_) {
    case kDisplayTab:
    case kGameTab:
    case kDebugTab: {
      const std::vector<int> on = SettingsOn(tab_);
      if (item < 0 || item >= int(on.size())) break;
      const auto& r = Settings()[size_t(on[size_t(item)])];
      if (IsPreset(r)) {
        // Recommended, Performance and Quality in turn (Custom only shows)
        const int now = PresetNow();
        ApplyPreset(now == kPresetCustom ? (dir > 0 ? 0 : kPresetCustom - 1)
                                         : (now + dir + kPresetCustom) % kPresetCustom);
        value_in_ = 0;
        value_dir_ = float(dir);
        Play(Sound::kSelect);
        break;
      }
      const int n = int(r.values.size());
      const int idx = (SettingIndex(r) + dir + n) % n;
      Save(r.key, r.quoted ? std::string("\"") + r.values[size_t(idx)] + "\"" : std::string(r.values[size_t(idx)]));
      if (IsLanguage(r)) lang::Set(lang::FromSetting(r.values[size_t(idx)]));  // (the hub, right away)
      if (std::string_view(r.key) == "ps5_refresh_rate" && display_.set_high_refresh) {
        if (std::string_view(r.values[size_t(idx)]) == "120") {
          Confirm(T("Switch to 120 Hz?"), T("The screen goes dark for a moment while the TV changes over."),
                  T("Switch"), [this] { SwitchRefresh(true); }, T("Later"),
                  [this] { Toast(T("120 Hz from the next start")); });
        } else {
          SwitchRefresh(false);
        }
      }
      if (std::string_view(r.key) == "ps5_refresh_rate" && std::string_view(r.values[size_t(idx)]) == "60" &&
          Value("ps5_frame_generation", "off") == "60-120")
        Save("ps5_frame_generation", "\"30-60\"");  // (60 to 120 needs 120 Hz)
      if (std::string_view(r.key) == "ps5_frame_generation") {
        // Frame generation needs FSR 3.1 reconstruction; its Native preset
        // keeps the chosen resolution (anti-aliasing only). 60 to 120 takes
        // the TV to 120 Hz; 30 to 60 works at either rate.
        const std::string_view mode = r.values[size_t(idx)];
        const bool on = mode != "off";
        Save("gta4_native_frame_generation", on ? "\"fsr3\"" : "\"off\"");
        Save("gta4_native_upscaler", on ? "\"fsr3\"" : "\"native\"");
        Save("gta4_temporal_upscaler_quality", "\"native\"");
        if (mode == "60-120" && Value("ps5_refresh_rate", "60") != "120") {
          Save("ps5_refresh_rate", "120");
          if (display_.set_high_refresh)
            Confirm(T("Switch to 120 Hz?"), T("The screen goes dark for a moment while the TV changes over."),
                    T("Switch"), [this] { SwitchRefresh(true); }, T("Later"),
                    [this] { Toast(T("120 Hz from the next start")); });
        }
      }
      value_in_ = 0;
      value_dir_ = float(dir);
      Play(Sound::kSelect);
      break;
    }
    case kEpisodesTab: {
      const int e = std::clamp(item, 0, install::kEpisodeCount - 1);
      if (state_.episode[e]) {
        Play(Sound::kError);
        Toast(F("{} is installed", {T(install::EpisodeName(e))}));
        break;
      }
      Play(Sound::kSelect);
      OpenInstaller(e);
      break;
    }
    case kSavesTab: {
      if (item == 0) {
        BackUpSaves("");
        Play(Sound::kSelect);
      } else if (item == 1) {
        if (!save_import_.run) break;
#if defined(__EMSCRIPTEN__)
        // (the player picks their files; DrawSaves imports them once they're in)
        Play(Sound::kSelect);
        web_saves::PickXbox360();
        break;
#endif
        pending_imports_ = save_import_.pending ? save_import_.pending() : 0;
        if (!pending_imports_) {
          Play(Sound::kError);
          Notice(T("Nothing to import"), T("Copy your Xbox 360 save files into /data/homebrew/PPSA94004/import/ on "
                                           "the PS5 first, then come back here."));
          break;
        }
        Play(Sound::kSelect);
        BackUpSaves(" (before import)");
        const std::string message = save_import_.run();
        pending_imports_ = save_import_.pending ? save_import_.pending() : 0;
        LoadSaves();
        Notice(T("Xbox 360 saves"), message);
#if defined(__EMSCRIPTEN__)
      } else if (item == 2) {
        std::vector<uint8_t> zip;
        int files = 0;
        if (!web_saves::Zip(paths_.user_data, Profiles(paths_.user_data), zip, &files) || !files) {
          Play(Sound::kError);
          Notice(T("Nothing to download"), T("There are no GTA IV saves yet: play the game and save first."));
          break;
        }
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_r(&now, &tm);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H-%M", &tm);
        web_saves::Offer(zip, std::string("GTA IV saves ") + stamp + ".zip");
        Play(Sound::kSelect);
        Toast(F("{} save files downloaded", {std::to_string(files)}));
      } else if (item == 3) {
        Play(Sound::kSelect);
        web_saves::Pick();
#endif
      } else if (item - kSaveActions < int(backups_.size())) {
        const Backup b = backups_[size_t(item - kSaveActions)];
        Play(Sound::kSelect);
        Confirm(T("Restore this backup?"),
                F("Your saves are replaced by the ones from {}. The saves you have now are backed up first, so "
                  "nothing is lost.",
                  {b.name}),
                T("Restore"), [this, b] { RestoreSaves(b); });
      }
      break;
    }
  }
}

void Hub::Navigate(const Pad& pad) {
  const uint32_t p = pad.pressed;
  if (leaving_) return;
  if (confirm_) {  // a question: Cross does it, Circle doesn't (or does its own thing)
    if ((p & kCross) && confirm_in_ > 0.6f) {  // (not before it has been seen)
      auto action = std::move(confirm_);
      confirm_ = nullptr;
      action();
    } else if ((p & kCircle) && confirm_in_ > 0.6f) {
      auto no = std::move(confirm_no_);
      confirm_ = nullptr, confirm_no_ = nullptr;
      Play(Sound::kBack);
      if (no) no();
    }
    return;
  }
  // the installer, over everything
  if (installer_) {
    InstallerInput(p);
    return;
  }
  // PLAY: the game, once installed; before that, the installer
  auto start = [&] {
    state_ = install::Look(paths_.app);
    if (state_.installed) {
      // (the game reads it at its start: gta4_start_episode, -1 the game's own way)
      Save("gta4_start_episode", HomeGames().size() > 1 ? std::to_string(HomeGame()) : "-1");
      press_ = 1, launch_ = true;
    } else {
      Play(Sound::kSelect);
      OpenInstaller();
    }
  };
  if (p & kOptions) {
    start();
    return;
  }
  // About: Cross held for a few seconds shows (or hides) the Debug page and
  // the Display page's rarer settings.
  if (tab_ == kAboutTab && (pad.held & kCross)) {
    if (!about_hold_done_ && (about_hold_ += dt_) >= kUnlockHold) {
      about_hold_done_ = true;
      SetDebugOptions(!debug_options_);
    }
  } else {
    about_hold_ = 0, about_hold_done_ = false;
  }
  if (p & (kL1 | kR1)) {
    const int n = TabsShown();
    GoTo((tab_ + ((p & kR1) ? 1 : n - 1)) % n, (p & kR1) ? 1.0f : -1.0f);
    Play(Sound::kPage);
    return;
  }
  if ((p & kCircle) && tab_ != kHome) {
    GoTo(kHome, -1);
    Play(Sound::kBack);
    return;
  }
  if (tab_ == kHome) {
    const std::vector<int> games = HomeGames();
    if (games.size() > 1 && (p & (kLeft | kRight))) {
      const int at = int(std::find(games.begin(), games.end(), HomeGame()) - games.begin());
      const int to = at + ((p & kRight) ? 1 : -1);
      if (to >= 0 && to < int(games.size())) {
        home_game_ = games[size_t(to)];
        Play(Sound::kMove);
      } else {  // (past the last one: a nudge)
        home_nudge_ = 0, home_nudge_dir_ = (p & kRight) ? 1 : -1;
      }
    }
    if (p & kCross) start();
    return;
  }
  const int n = Items();
  if (n == 0) return;
  int& sel = item_[tab_];
  const int before = sel;
  if (p & kUp) sel = (sel + n - 1) % n;
  if (p & kDown) sel = (sel + 1) % n;
  if (sel != before) Play(Sound::kMove);
  switch (tab_) {
    case kDisplayTab:
    case kGameTab:
    case kDebugTab:
      if (p & kTriangle) {
        Play(Sound::kSelect);
        Confirm(T("Reset all settings?"),
                T("Everything on the Display, Game and Debug pages goes back to the recommended settings. The "
                  "language stays as it is."),
                T("Reset"), [this] { ResetSettings(); });
        return;
      }
      if (p & kLeft) Activate(-1);
      if (p & (kRight | kCross)) Activate(1);
      if (p & (kLeft | kRight | kCross)) press_ = 0.6f;
      break;
    case kSavesTab:
    case kEpisodesTab:
      if (p & kCross) press_ = 1, Activate(0);
      break;
  }
}

void Hub::GoTo(int tab, float dir) {
  if (tab == tab_) return;
  tab_ = tab;
  scroll_f_ = -1;
  scroll_px_ = 0;
  focus_y_ = -1;  // (the highlight starts where the page's row is)
  page_in_ = 0;
  page_dir_ = dir;
  panel_in_ = 1;
  panel_item_ = item_[tab_];
  // (what these pages show can change while the hub is open: looked at again)
  if (tab == kSavesTab) LoadSaves();
  if (tab == kTrophiesTab) LoadTrophies();
  if (tab == kEpisodesTab) LookAgain();
}

void Hub::LookAgain() {
  if (looking_.valid()) return;  // (one at a time)
  looking_ = std::async(std::launch::async, [app = paths_.app] { return install::Look(app); });
}

#if defined(__EMSCRIPTEN__)
void Hub::MouseInput(Pad& pad) {
  if (pad.wheel) pad.pressed |= pad.wheel > 0 ? kDown : kUp;
  if (pad.mouse_x < 0 || (!pad.mouse_moved && !pad.click)) return;
  const float x = pad.mouse_x, y = pad.mouse_y;  // (the regions are in the overlay's pixels too)
  for (const HitRegion& r : last_hits_) {
    if (x < r.x0 || x > r.x1 || y < r.y0 || y > r.y1) continue;
    if (r.kind == 0) {
      if (pad.click && int(r.value) != tab_) GoTo(int(r.value), int(r.value) > tab_ ? 1.0f : -1.0f);
    } else if (r.kind == 1) {
      item_[tab_] = int(r.value);
      if (pad.click) pad.pressed |= kCross;
    } else if (pad.click) {
      pad.pressed |= r.value;
    }
    return;
  }
}
#endif

bool Hub::Draw(const Pad& pad, float dt) {
  if (looking_.valid() && looking_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) state_ = looking_.get();
  if (saves_scan_.valid() && saves_scan_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    SavesScan scan = saves_scan_.get();
    save_items_ = std::move(scan.saves);
    backups_ = std::move(scan.backups);
    pending_imports_ = scan.pending;
    if (saves_again_) saves_again_ = false, LoadSaves();
  }
  if (trophies_load_.valid() && trophies_load_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    TakeTrophies(trophies_load_.get());
  pictures_this_frame_ = 0;
  dt_ = std::clamp(dt, 0.0f, 0.1f);
  time_ += dt_;
  toast_time_ = std::max(0.0f, toast_time_ - dt_);
  // The Refresh Rate at 120: the TV switched once it's open, from here (no
  // frame is flipped while the switch measures the vblanks: ps5_display.h).
  if (!refresh_checked_ && display_.ready && display_.ready()) {
    refresh_checked_ = true;
    if (Value("ps5_refresh_rate", "60") == "120" && display_.high_refresh && !display_.high_refresh())
      SwitchRefresh(true);
  }
#if defined(__EMSCRIPTEN__)
  last_hits_.swap(hits_);
  hits_.clear();
  Pad input = pad;
  MouseInput(input);
  Navigate(input);
#else
  Navigate(pad);
#endif
  if (launch_ && !leaving_) {
    leaving_ = true, press_ = 1;
    Play(Sound::kStart);
  }
  if (item_[tab_] != panel_item_) panel_item_ = item_[tab_], panel_in_ = 0;
  Ease(panel_in_, 1, 16);
  Ease(value_in_, 1, 18);
  Ease(press_, 0, 9);
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  s_ = std::min(size.x / kW, size.y / kH);
  ox_ = (size.x - kW * s_) * 0.5f;
  oy_ = (size.y - kH * s_) * 0.5f;
  dl_ = ImGui::GetForegroundDrawList();
  Theme();
  DrawBackdrop();
  DrawTopBar();
  // the page comes up quickly (the game's menu just switches), from a touch
  // to the side it came from
  Ease(page_in_, 1, 24);
  const int first = dl_->VtxBuffer.Size;
  const float shift = (1 - page_in_) * 14 * page_dir_;
  ox_ += shift * s_;
  if (installer_) {
    DrawInstaller();
  } else {
    switch (tab_) {
      case kHome: DrawHome(); break;
      case kDisplayTab:
      case kGameTab:
      case kDebugTab: DrawSettings(); break;
      case kSavesTab: DrawSaves(); break;
      case kEpisodesTab: DrawEpisodes(); break;
      case kTrophiesTab: DrawTrophies(); break;
      case kAboutTab: DrawAbout(); break;
    }
  }
  ox_ -= shift * s_;
  Fade(first, Smooth(page_in_));
  DrawFooter();
  DrawHints();
  DrawConfirm();
  // opening: up from black; PLAY: down to black
  const float open = Smooth(time_ / 0.7f);
  if (leaving_) leave_ = std::min(1.0f, leave_ + dt_ / 0.6f);
  const float black = std::max(1 - open, Smooth(leave_));
  if (black > 0.002f) dl_->AddRectFilled(ImVec2(0, 0), size, Black(black));
  return leaving_ && leave_ >= 1;
}

}  // namespace gta4ps5::hub
