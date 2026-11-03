// GTA IV PS5 - the hub on the console (see hub_ui.h):
// shown before the game is set up (ReXApp::OnFinalizePaths, asynchronous),
// driven by the DualSense through scePad, with each game's menu music
// (hub/music) and the menu sounds (hub/sfx), and PLAY resumes the start.
#if defined(__PROSPERO__) || defined(__EMSCRIPTEN__)
// [web port] The browser build shows the same hub: the page supplies the pad
// (rex_web_hub_pad) and the fonts are read from the app folder.

#include "ps5_hub.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/pad_dialog_style.h>

#include "hub_sfx.h"
#include "hub_ui.h"
#include "../game/sound.h"
#include "imgui.h"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"

#if defined(__EMSCRIPTEN__)
#include <atomic>
#include <fstream>
#include <iterator>

#include <emscripten/emscripten.h>

#ifndef GTA4PS5_VERSION
#define GTA4PS5_VERSION "1.0web"
#endif

namespace {
// The page's pad: DualSense button bits (kPadCross...), as scePad reports them.
std::atomic<uint32_t> g_web_pad_buttons{0};
std::atomic<uint32_t> g_web_pad_pressed{0};  // (pressed since the hub's last read: a quick tap counts)
}  // namespace
// The page's mouse: position in the overlay's pixels, buttons (bit 0 left),
// wheel notches since the last call.
std::atomic<int32_t> g_web_mouse_x{-1}, g_web_mouse_y{-1}, g_web_mouse_wheel{0};
std::atomic<uint32_t> g_web_mouse_buttons{0}, g_web_mouse_clicks{0};
// (buttons bit 0x100: a left click since the last call, counted)
extern "C" EMSCRIPTEN_KEEPALIVE void rex_web_hub_mouse(int32_t x, int32_t y, uint32_t buttons, int32_t wheel) {
  g_web_mouse_x = x;
  g_web_mouse_y = y;
  g_web_mouse_buttons = buttons & 0xFF;
  g_web_mouse_wheel += wheel;
  if (buttons & 0x100) g_web_mouse_clicks++;
}

// (bit 31: the last input was the keyboard or mouse, so the prompts show keys)
extern "C" EMSCRIPTEN_KEEPALIVE void rex_web_hub_pad(uint32_t buttons) {
  g_web_pad_buttons = buttons;
  g_web_pad_pressed |= buttons & 0x1FFFFFFFu;  // (not the flag bits 29-31)
}

namespace gta4ps5::hub {
bool KeyboardPrompts() { return (g_web_pad_buttons.load() & 0x80000000u) != 0; }
// (bits 29-30: the controller's family, webui.js familyOf)
PadFamily CurrentPadFamily() { return PadFamily((g_web_pad_buttons.load() >> 29) & 3); }
// The button's name on that family's controllers, by position (the bottom
// face button is Cross, Xbox A, Nintendo B), or null for PlayStation's own.
const char* PadLetter(uint32_t button, PadFamily family) {
  if (family == PadFamily::kPlayStation) return nullptr;
  const bool nintendo = family == PadFamily::kNintendo;
  switch (button) {
    case kCross: return nintendo ? "B" : "A";
    case kCircle: return nintendo ? "A" : "B";
    case kSquare: return nintendo ? "Y" : "X";
    case kTriangle: return nintendo ? "X" : "Y";
    case kL1: return nintendo ? "L" : "LB";
    case kR1: return nintendo ? "R" : "RB";
    case kOptions: return nintendo ? "+" : "Menu";
    case kR3: return nintendo ? "R" : "RS";
    default: return nullptr;
  }
}
const char* KeyboardKey(uint32_t button) {
  switch (button) {
    case kCross: return "Enter";
    case kCircle: return "Esc";
    case kSquare: return "X";
    case kTriangle: return "T";
    case kL1: return "Q";
    case kR1: return "E";
    case kOptions: return "O";
    case kR3: return "R";
    default: return nullptr;
  }
}
}  // namespace gta4ps5::hub
#else
extern "C" {
int sceUserServiceInitialize(void* params);
int sceUserServiceGetInitialUser(int32_t* user_id);
int scePadInit(void);
int scePadOpen(int32_t user_id, int32_t type, int32_t index, const void* param);
int scePadGetHandle(int32_t user_id, int32_t type, int32_t index);
int scePadReadState(int32_t handle, void* data);
int sceSystemServicePowerTick(void);
}
#endif

// The fonts, built into the app (fonts/: Oswald and Inter under the SIL Open
// Font License, TeX Gyre Heros Cn under the GUST Font License; Helvetica Neue
// Heavy, the tabs, when the build has it: GTA4PS5_TAB_FONT).
#if defined(__EMSCRIPTEN__)
// (web: read from <app>/fonts when the fonts are set up; WebAssembly objects
// can't take the assembler's .incbin)
#else
#define SVR_INCBIN(name, file)                                                        \
  __asm__(".section .rodata\n.global " #name "\n.balign 16\n" #name ":\n.incbin \"" \
          file "\"\n.global " #name "_end\n" #name "_end:\n.byte 0\n.previous\n");    \
  extern "C" const unsigned char name[];                                              \
  extern "C" const unsigned char name##_end[];
SVR_INCBIN(gta4ps5_font_oswald_bold, GTA4PS5_FONTS_DIR "/oswald-latin-700-normal.ttf")
SVR_INCBIN(gta4ps5_font_oswald, GTA4PS5_FONTS_DIR "/oswald-latin-600-normal.ttf")
SVR_INCBIN(gta4ps5_font_inter, GTA4PS5_FONTS_DIR "/inter-latin-400-normal.ttf")
SVR_INCBIN(gta4ps5_font_inter_bold, GTA4PS5_FONTS_DIR "/inter-latin-600-normal.ttf")
SVR_INCBIN(gta4ps5_font_menu, GTA4PS5_FONTS_DIR "/texgyreheroscn-bold.otf")
SVR_INCBIN(gta4ps5_font_menu_regular, GTA4PS5_FONTS_DIR "/texgyreheroscn-regular.otf")
#if GTA4PS5_TAB_FONT
SVR_INCBIN(gta4ps5_font_tab, GTA4PS5_FONTS_DIR "/HelveticaNeue-Heavy.ttf")
#endif
#endif  // __EMSCRIPTEN__

namespace gta4ps5::hub {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kPadR3 = 0x00000004u, kPadOptions = 0x00000008u, kPadUp = 0x00000010u, kPadRight = 0x00000020u,
                   kPadDown = 0x00000040u, kPadLeft = 0x00000080u, kPadL1 = 0x00000400u, kPadR1 = 0x00000800u,
                   kPadTriangle = 0x00001000u, kPadCircle = 0x00002000u, kPadCross = 0x00004000u,
                   kPadSquare = 0x00008000u;
struct PadData {
  uint32_t buttons;
  uint8_t lx, ly, rx, ry, l2, r2;
  uint16_t padding;
  uint8_t rest[240];
};

Assets g_assets;
Hub::SaveImport g_save_import;
Hub::Display g_display;
// The hub's pictures: kept for the app's life. (The dialog is deleted right
// after the frame START GAME was pressed on, and that frame's draw list,
// rendered afterwards, still uses them.)
std::vector<std::unique_ptr<rex::ui::ImmediateTexture>>* g_textures =
    new std::vector<std::unique_ptr<rex::ui::ImmediateTexture>>();

// The pad: a handle of our own (the input driver later gets the same one).
class Pad5 {
 public:
  Pad read() {
#if defined(__EMSCRIPTEN__)
    PadData d{};
    d.buttons = (g_web_pad_buttons.load() & ~0x60000000u) | g_web_pad_pressed.exchange(0);
    d.lx = d.ly = d.rx = d.ry = 128;
#else
    if (handle_ < 0) {
      sceUserServiceInitialize(nullptr);
      scePadInit();
      int32_t user = -1;
      if (sceUserServiceGetInitialUser(&user) == 0 && user >= 0) {
        handle_ = scePadOpen(user, 0, 0, nullptr);
        if (handle_ < 0) handle_ = scePadGetHandle(user, 0, 0);
      }
      if (handle_ < 0) return {};
    }
    PadData d{};
    if (scePadReadState(handle_, &d) != 0) return {};
#endif
    uint32_t b = 0;
    if ((d.buttons & kPadUp) || d.ly < 64) b |= kUp;
    if ((d.buttons & kPadDown) || d.ly > 192) b |= kDown;
    if ((d.buttons & kPadLeft) || d.lx < 64) b |= kLeft;
    if ((d.buttons & kPadRight) || d.lx > 192) b |= kRight;
    if (d.buttons & kPadCross) b |= kCross;
    if (d.buttons & kPadCircle) b |= kCircle;
    if (d.buttons & kPadSquare) b |= kSquare;
    if (d.buttons & kPadTriangle) b |= kTriangle;
    if (d.buttons & kPadL1) b |= kL1;
    if (d.buttons & kPadR1) b |= kR1;
    if (d.buttons & kPadOptions) b |= kOptions;
    if (d.buttons & kPadR3) b |= kR3;
    Pad p;
    p.held = b;
    p.pressed = b & ~last_;
    // (held directions repeat: after 0.3 s, every 0.06 s)
    const auto now = std::chrono::steady_clock::now();
    constexpr uint32_t kDirs = kUp | kDown | kLeft | kRight;
    if (b & kDirs) {
      if ((b & kDirs) != (last_ & kDirs)) {
        repeat_at_ = now + std::chrono::milliseconds(300);
      } else if (now >= repeat_at_) {
        p.pressed |= b & kDirs;
        repeat_at_ = now + std::chrono::milliseconds(60);
      }
    }
    last_ = b;
    // The first read: ignore what was already held (the button that started the app).
    if (first_) {
      first_ = false;
      p.pressed = 0;
    }
#if defined(__EMSCRIPTEN__)
    // The mouse: where it is, whether it moved, a left click, the wheel.
    const int32_t mx = g_web_mouse_x.load(), my = g_web_mouse_y.load();
    const uint32_t mb = g_web_mouse_buttons.load();
    p.mouse_x = float(mx);
    p.mouse_y = float(my);
    p.mouse_moved = mx != last_mouse_x_ || my != last_mouse_y_;
    p.click = g_web_mouse_clicks.exchange(0) > 0;
    p.wheel = g_web_mouse_wheel.exchange(0);
    last_mouse_x_ = mx, last_mouse_y_ = my, last_mouse_buttons_ = mb;
#endif
    return p;
  }

 private:
#if defined(__EMSCRIPTEN__)
  int32_t last_mouse_x_ = -1, last_mouse_y_ = -1;
  uint32_t last_mouse_buttons_ = 0;
#endif
  int32_t handle_ = -1;
  uint32_t last_ = 0;
  bool first_ = true;
  std::chrono::steady_clock::time_point repeat_at_{};
};

// The menu sounds: three recordings in hub/sfx (click, tab and confirm, as
// .ogg, .wav, .mp3 or .flac), decoded once and kept in memory. Moving, choosing
// and changing a setting click; L1 and R1 beep; PLAY (and choosing the game on
// Home) confirms. Nothing else makes a sound.
class MenuClips {
 public:
  explicit MenuClips(const fs::path& hub) {
    const char* names[] = {"click", "tab", "confirm"};
    for (int c = 0; c < 3; ++c) clips_[c] = Decode(hub / "sfx", names[c]);
  }
  std::shared_ptr<const std::vector<float>> For(Sound s) const {
    switch (s) {
      case Sound::kPage: return clips_[1];
      case Sound::kStart: return clips_[2];
      case Sound::kError: return nullptr;
      default: return clips_[0];
    }
  }

 private:
  static std::shared_ptr<const std::vector<float>> Decode(const fs::path& dir, const char* name) {
    for (const char* ext : {".ogg", ".wav", ".mp3", ".flac"}) {
      const fs::path file = dir / (std::string(name) + ext);
      std::error_code ec;
      if (!fs::is_regular_file(file, ec)) continue;
      std::unique_ptr<gta4ps5::sound::Source> src = gta4ps5::sound::Open(file);
      if (!src) break;
      auto lr = std::make_shared<std::vector<float>>();
      std::vector<float> chunk(4096 * 2);
      while (const size_t n = src->Read(chunk.data(), 4096)) lr->insert(lr->end(), chunk.begin(), chunk.begin() + n * 2);
      return lr;
    }
    REXLOG_WARN("PS5 hub: no {} sound in {}", name, dir.string());
    return nullptr;
  }
  std::array<std::shared_ptr<const std::vector<float>>, 3> clips_;
};

// The hub's sound (ps5/sound.h): each game's menu song (hub/music: "GTA IV
// Menu", "The Lost and Damned Menu", "The Ballad of Gay Tony Menu", any of the
// four types), all three playing from the start and looping, the one of the
// game chosen on Home heard: moving to another fades it over without starting
// either song again. A game without its song has GTA IV's. And the menu
// sounds (MenuClips).
// (web: where the hub is, for the UI thread's stall report in rex_app.cpp)
#if defined(__EMSCRIPTEN__)
extern "C" std::atomic<const char*> g_web_ui_where;
void Where(const char* what) { g_web_ui_where = what; }
#else
void Where(const char*) {}
#endif

class HubAudio {
 public:
  HubAudio(const fs::path& hub, float level) : level_(level), clips_(hub) {
    if (!gta4ps5::sound::StartOutput()) REXLOG_WARN("PS5 hub: no audio port for the hub's sounds");
    for (auto& v : sfx_) v = gta4ps5::sound::NewVoice();
    const char* songs[3] = {"GTA IV Menu", "The Lost and Damned Menu", "The Ballad of Gay Tony Menu"};
    for (int g = 0; g < 3; ++g) {
      for (const char* ext : {".mp3", ".ogg", ".flac", ".wav"}) {
        const fs::path file = hub / "music" / (std::string(songs[g]) + ext);
        std::error_code ec;
        if (fs::is_regular_file(file, ec)) {
          names_[g] = file.stem().string();
          if (auto src = gta4ps5::sound::Open(file)) {
            music_[g] = gta4ps5::sound::NewVoice();
            music_[g]->SetVolume(0);
            music_[g]->Play(std::move(src), true);
          }
          break;
        }
      }
      REXLOG_INFO("PS5 hub: {}: {}", songs[g], music_[g] ? "playing" : "none");
    }
  }
  bool HasMusic() const { return music_[0] || music_[1] || music_[2]; }
  std::string Current() const { return names_[Heard(game_)]; }

  void Sfx(Sound s) {
    auto clip = clips_.For(s);
    if (!clip) return;
    auto* v = sfx_[next_++ % sfx_.size()];  // (a few at once: quick presses overlap)
    v->SetVolume(1);
    v->Play(gta4ps5::sound::Memory(std::move(clip)), false);
  }

  // (each game has one song now: no next one)
  std::string Next() { return {}; }

  // Every frame, with the game chosen on Home: its song up to the Menu Music
  // level, the others down, over about half a second.
  void Tick(float dt, int game) {
    game_ = game;
    const int heard = Heard(game);
    for (int g = 0; g < 3; ++g) {
      if (!music_[g]) continue;
      const float target = !leaving_ && g == heard ? level_ : 0.0f;
      gain_[g] += (target - gain_[g]) * (1.0f - std::exp(-dt * 7.0f));
      if (std::fabs(target - gain_[g]) < 0.002f) gain_[g] = target;
      music_[g]->SetVolume(gain_[g]);
    }
  }

  // Settings' Menu Music level (0..1): the song follows it right away.
  void SetLevel(float level) { level_ = level; }

  void FadeOut() {
    leaving_ = true;
    for (auto* v : music_)
      if (v) v->FadeOut(0.6f);
  }

 private:
  int Heard(int game) const { return game >= 0 && game < 3 && music_[game] ? game : 0; }

  float level_ = 0.6f;
  MenuClips clips_;
  std::array<gta4ps5::sound::Voice*, 3> music_{};
  std::array<float, 3> gain_{};
  std::array<std::string, 3> names_;
  int game_ = 0;
  std::array<gta4ps5::sound::Voice*, 6> sfx_{};
  size_t next_ = 0;
  bool leaving_ = false;
};

// The menu sounds for the game's own dialogs (the on-screen keyboard), after
// the hub has gone: the hub's (MenuClips), a few voices of their own.
class MenuSfx {
 public:
  explicit MenuSfx(const fs::path& hub) : clips_(hub) {
    for (auto& v : voices_) v = gta4ps5::sound::NewVoice();
  }
  void Play(int k) {
    if (k < 0 || k > int(Sound::kError)) return;
    auto clip = clips_.For(Sound(k));
    if (!clip) return;
    auto* v = voices_[next_++ % voices_.size()];
    v->SetVolume(0.85f);
    v->Play(gta4ps5::sound::Memory(std::move(clip)), false);
  }

 private:
  MenuClips clips_;
  std::array<gta4ps5::sound::Voice*, 4> voices_{};
  size_t next_ = 0;
};

// The game's on-screen keyboard in the hub's style (rex/ui/pad_dialog_style.h).
void StyleGameDialogs(const Assets& a, const fs::path& hub) {
  static MenuSfx* sfx = new MenuSfx(hub);  // (for the app's life)
  rex::ui::PadDialogStyle st;
  st.title = a.display;
  st.body = a.body;
  st.bold = a.body_bold;
  const std::pair<uint32_t, uint32_t> map[] = {{kCross, 0x1000}, {kCircle, 0x2000}, {kSquare, 0x4000},
                                               {kTriangle, 0x8000}, {kL1, 0x0100}, {kR1, 0x0200},
                                               {kOptions, 0x0010}, {kLeft | kRight, 0x000C}};
  for (const auto& [hub, xbox] : map)
    if (auto it = a.buttons.find(hub); it != a.buttons.end() && it->second) st.buttons[xbox] = it->second.id;
  st.sound = [](int k) { sfx->Play(k); };
  rex::ui::SetPadDialogStyle(std::move(st));
}

class HubDialog final : public rex::ui::ImGuiDialog {
 public:
  HubDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, const Paths& paths,
            std::function<void()> launch)
      : ImGuiDialog(drawer), launch_(std::move(launch)) {
    const fs::path hub = paths.app / "hub";
    Assets a = g_assets;
    a.logo = Load(immediate, hub / "logo.png");
    a.hero = Load(immediate, hub / "hero.jpg");
    if (!a.hero) a.hero = Load(immediate, hub / "hero.png");
    // Home's panels (hub/home; GTA IV's: the hero and the logo without them)
    const char* games[] = {"gta4", "tlad", "tbogt"};
    for (int g = 0; g < 3; ++g) {
      auto& art = a.home[g];
      // (the logo first: its colour is the glow's, the picture's without one)
      art.logo = Load(immediate, hub / "home" / (std::string(games[g]) + "-logo.png"), &art.color);
      for (const char* ext : {".jpg", ".png"})
        if (!art.picture) LoadHomePicture(immediate, hub / "home" / (std::string(games[g]) + ext), art);
    }
    if (!a.home[0].picture) {
      LoadHomePicture(immediate, hub / "hero.jpg", a.home[0]);
      if (!a.home[0].picture) LoadHomePicture(immediate, hub / "hero.png", a.home[0]);
    }
    if (!a.home[0].logo) a.home[0].logo = a.logo;
    // The Lost and Damned's scuffs (made here)
    a.grunge = Texture(immediate, Grunge(kGrungeW, kGrungeH));
    // the button pictures (hub/buttons/<name>.png; drawn ones where missing)
    const std::pair<uint32_t, const char*> names[] = {
        {kCross, "Cross"}, {kCircle, "Circle"},   {kSquare, "Square"},         {kTriangle, "Triangle"}, {kL1, "L1"},
        {kR1, "R1"},       {kOptions, "Options"}, {kLeft | kRight, "D-Pad"}, {kR3, "R3"}};
    for (const auto& [b, n] : names)
      if (Image img = Load(immediate, hub / "buttons" / (std::string(n) + ".png"))) a.buttons[b] = img;
    hub_.SetImageLoader([this, immediate](const fs::path& file) { return Load(immediate, file); });
    hub_.SetSaveImport(g_save_import);
    hub_.SetDisplay(g_display);
    hub_.Init(paths, a, GTA4PS5_VERSION);
    StyleGameDialogs(a, hub);
    audio_ = std::make_unique<HubAudio>(hub, hub_.MusicVolume());
    Audio hooks;
    hooks.sfx = [this](Sound s) { audio_->Sfx(s); };
    hooks.next_song = [this] { return audio_->Next(); };
    hub_.SetAudio(std::move(hooks));
    hub_.SetHasMusic(audio_->HasMusic());
    hub_.NowPlaying(audio_->Current());
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (done_) return;
    Where("hub: reading the pad");
    const Pad pad = pad_.read();
    Where("hub: music level");
    audio_->SetLevel(hub_.MusicVolume());
    audio_->Tick(io.DeltaTime, hub_.Game());
    Where("hub: drawing");
    const bool go = hub_.Draw(pad, io.DeltaTime);
    Where("hub: drawn");
    // installing: the PS5 stays awake (no rest mode, no screen dimming)
    if (hub_.Busy() && (awake_ -= io.DeltaTime) <= 0) {
#if defined(__PROSPERO__)
      sceSystemServicePowerTick();
#endif
      awake_ = 10;
    }
    if (hub_.Leaving() && !faded_) audio_->FadeOut(), faded_ = true;
    if (go) {
      done_ = true;
      Close();
      launch_();
#if defined(__EMSCRIPTEN__)
      // [web port] The hub's big pictures (Home's art, the logos) once the
      // game has drawn a while: the hub isn't shown again, and they are ~70 MB
      // in the browser's memory. The buttons' small pictures stay (dialogs in
      // the game draw them).
      std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(15));
        auto& textures = *g_textures;
        textures.erase(std::remove_if(textures.begin(), textures.end(),
                                      [](const auto& t) { return t && uint64_t(t->width) * t->height >= 256 * 256; }),
                       textures.end());
      }).detach();
#endif
    }
  }

 private:
  Image Texture(rex::ui::ImmediateDrawer* immediate, const Pixels& p) {
    auto tex = immediate->CreateTexture(uint32_t(p.w), uint32_t(p.h), rex::ui::ImmediateTextureFilter::kLinear, false,
                                        p.rgba.data());
    if (!tex) return {};
    Image img{reinterpret_cast<ImTextureID>(tex.get()), float(p.w), float(p.h)};
    g_textures->push_back(std::move(tex));
    return img;
  }

  // A Home picture, no taller than the 4K screen's band, its black and white copy and its colour.
  void LoadHomePicture(rex::ui::ImmediateDrawer* immediate, const fs::path& file, Assets::HomeArt& art) {
    if (!immediate) return;
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load(file.string().c_str(), &w, &h, &n, 4);
    if (!px) return;
    const Pixels p = Shrink(px, w, h, kHomePictureHeight);
    stbi_image_free(px);
    art.picture = Texture(immediate, p);
    if (art.picture) art.grey = Texture(immediate, Grey(p));
    if (art.picture && !art.color) art.color = Accent(p);
    REXLOG_INFO("PS5 hub: Home picture {} ({}x{}, shown at {}x{})", file.filename().string(), w, h, p.w, p.h);
  }

  // (accent: the picture's colour (Accent), when asked for)
  Image Load(rex::ui::ImmediateDrawer* immediate, const fs::path& file, ImU32* accent = nullptr) {
    if (!immediate) return {};
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load(file.string().c_str(), &w, &h, &n, 4);
    if (!px) return {};
    auto tex = immediate->CreateTexture(uint32_t(w), uint32_t(h), rex::ui::ImmediateTextureFilter::kLinear, false, px);
    Image img{reinterpret_cast<ImTextureID>(tex.get()), float(w), float(h)};
    MeasureFill(img, px, w, h);
    if (accent) {
      Pixels p;
      p.w = w, p.h = h;
      p.rgba.assign(px, px + size_t(w) * size_t(h) * 4);
      *accent = Accent(p);
    }
    stbi_image_free(px);
    if (!tex) return {};
    g_textures->push_back(std::move(tex));
    return img;
  }

  Hub hub_;
  Pad5 pad_;
  std::unique_ptr<HubAudio> audio_;  // (its voices outlive it: the last sounds finish in the game)
  bool faded_ = false;
  bool done_ = false;
  float awake_ = 0;
  std::function<void()> launch_;
};

ImFont* AddFont(ImFontAtlas* atlas, const unsigned char* begin, const unsigned char* end, float size) {
  ImFontConfig cfg;
  cfg.FontDataOwnedByAtlas = false;  // (built into the app)
  cfg.OversampleH = 2;
  return atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(begin), int(end - begin), size, &cfg);
}

}  // namespace

#if defined(__EMSCRIPTEN__)
// The fonts' bytes from the app folder's fonts/ (kept for the atlas's life).
struct FontFile {
  std::vector<unsigned char> bytes;
  const unsigned char* begin() const { return bytes.data(); }
  const unsigned char* end() const { return bytes.data() + bytes.size(); }
};
const FontFile& ReadFont(const char* name) {
  static std::map<std::string, FontFile> fonts;
  FontFile& f = fonts[name];
  if (f.bytes.empty()) {
    std::ifstream in(std::string("/app0/fonts/") + name, std::ios::binary);
    f.bytes.assign(std::istreambuf_iterator<char>(in), {});
    if (f.bytes.empty()) REXLOG_WARN("PS5 hub: font {} missing", name);
    f.bytes.push_back(0);  // (never empty: the atlas reads what it's given)
  }
  return f;
}
#endif

void ConfigureFonts(ImFontAtlas* atlas) {
#if defined(__EMSCRIPTEN__)
  const FontFile& oswald_bold = ReadFont("oswald-latin-700-normal.ttf");
  const FontFile& oswald = ReadFont("oswald-latin-600-normal.ttf");
  const FontFile& inter = ReadFont("inter-latin-400-normal.ttf");
  const FontFile& inter_bold = ReadFont("inter-latin-600-normal.ttf");
  const FontFile& menu = ReadFont("texgyreheroscn-bold.otf");
  const FontFile& menu_regular = ReadFont("texgyreheroscn-regular.otf");
  g_assets.display = AddFont(atlas, oswald_bold.begin(), oswald_bold.end() - 1, 72.0f * 2);
  g_assets.nav = AddFont(atlas, oswald.begin(), oswald.end() - 1, 34.0f * 2);
  g_assets.body = AddFont(atlas, inter.begin(), inter.end() - 1, 26.0f * 2);
  g_assets.body_bold = AddFont(atlas, inter_bold.begin(), inter_bold.end() - 1, 26.0f * 2);
  AddMenuFonts(atlas, menu.begin(), int(menu.bytes.size() - 1), menu_regular.begin(),
               int(menu_regular.bytes.size() - 1), g_assets);
#else
  // Sized for the 4K screen (the hub scales them to the display).
  g_assets.display = AddFont(atlas, gta4ps5_font_oswald_bold, gta4ps5_font_oswald_bold_end, 72.0f * 2);
  g_assets.nav = AddFont(atlas, gta4ps5_font_oswald, gta4ps5_font_oswald_end, 34.0f * 2);
  g_assets.body = AddFont(atlas, gta4ps5_font_inter, gta4ps5_font_inter_end, 26.0f * 2);
  g_assets.body_bold = AddFont(atlas, gta4ps5_font_inter_bold, gta4ps5_font_inter_bold_end, 26.0f * 2);
  // the hub's menu type (GTA IV's: a Helvetica Condensed), at its sizes
  AddMenuFonts(atlas, gta4ps5_font_menu, int(gta4ps5_font_menu_end - gta4ps5_font_menu), gta4ps5_font_menu_regular,
               int(gta4ps5_font_menu_regular_end - gta4ps5_font_menu_regular), g_assets);
#if GTA4PS5_TAB_FONT
  // the tabs (Helvetica Neue Heavy)
  AddTabFonts(atlas, gta4ps5_font_tab, int(gta4ps5_font_tab_end - gta4ps5_font_tab), g_assets);
#endif
#endif  // __EMSCRIPTEN__
}

const Assets& Fonts() { return g_assets; }

void SetSaveImport(Hub::SaveImport import) { g_save_import = std::move(import); }
void SetDisplay(Hub::Display display) { g_display = std::move(display); }

const char* Version() { return GTA4PS5_VERSION; }

void Show(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, const Paths& paths,
          std::function<void()> launch) {
  REXLOG_INFO("PS5 hub: shown (app folder {})", paths.app.string());
  new HubDialog(drawer, immediate, paths, std::move(launch));  // (the dialog owns itself)
}

}  // namespace gta4ps5::hub

#endif  // __PROSPERO__ || __EMSCRIPTEN__
