// GTA IV PS5 - the hub: the screen the app opens on, before the game
// (the PC launcher's settings, driven by the controller), drawn after GTA IV's
// own pause menu: tabs across the top in its condensed type, the chosen one in
// yellow, rows of settings under them, the hints in the footer. Home, Display,
// Game, Saves, Trophies, About and Debug, and menu music (one song after
// another).
//
// This part is plain ImGui + the standard library, so it also builds on a
// PC for previews (tools/hub_preview). ps5_hub.cpp puts it on the PS5's
// screen, reads the DualSense, plays the music and starts the game.
#pragma once

#include <future>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "hub_install.h"
#include "hub_style.h"
#include "hub_trophies.h"
#include "imgui.h"

namespace gta4ps5::hub {

// The controller, as the hub sees it.
enum Button : uint32_t {
  kUp = 1u << 0,
  kDown = 1u << 1,
  kLeft = 1u << 2,
  kRight = 1u << 3,
  kCross = 1u << 4,
  kCircle = 1u << 5,
  kSquare = 1u << 6,
  kTriangle = 1u << 7,
  kL1 = 1u << 8,
  kR1 = 1u << 9,
  kOptions = 1u << 10,
  kR3 = 1u << 11,
};

#if defined(__EMSCRIPTEN__)
// [web port] Whether the hub's prompts show keyboard keys (the last input was
// the keyboard or mouse) rather than the controller's buttons (ps5_hub.cpp).
bool KeyboardPrompts();
// The key a button is on the keyboard ("Enter" for Cross...), or null.
const char* KeyboardKey(uint32_t button);
// The controller last used: its family picks the buttons' names and pictures.
enum class PadFamily : uint32_t { kPlayStation = 0, kXbox = 1, kNintendo = 2 };
PadFamily CurrentPadFamily();
// A button's name on an Xbox or Nintendo controller ("A"...), or null on a
// PlayStation one (its pictures are hub/buttons').
const char* PadLetter(uint32_t button, PadFamily family);
#endif

struct Pad {
  uint32_t held = 0;     // down now
  uint32_t pressed = 0;  // down now, not before (with key repeat for the d-pad)
#if defined(__EMSCRIPTEN__)
  // [web port] The mouse, in the overlay's pixels (-1: none): it focuses what
  // it's over, a click chooses it (Hub::MouseInput).
  float mouse_x = -1, mouse_y = -1;
  bool mouse_moved = false, click = false;
  int wheel = 0;  // notches: + down, - up
#endif
};

struct Image {
  ImTextureID id = 0;
  float width = 0, height = 0;
  // the drawing's share of the picture, across and down (a button picture's
  // drawing is centred in its square: ps5/tools/hub_buttons.py)
  float fill_w = 1, fill_h = 1;
  explicit operator bool() const { return id != 0; }
};

// The part of a picture that isn't clear (alpha above 8), as fill_w / fill_h.
inline void MeasureFill(Image& img, const unsigned char* rgba, int w, int h) {
  int x0 = w, y0 = h, x1 = -1, y1 = -1;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      if (rgba[(size_t(y) * w + x) * 4 + 3] > 8) {
        x0 = x < x0 ? x : x0, x1 = x > x1 ? x : x1;
        y0 = y < y0 ? y : y0, y1 = y > y1 ? y : y1;
      }
  if (x1 >= 0) img.fill_w = float(x1 - x0 + 1) / float(w), img.fill_h = float(y1 - y0 + 1) / float(h);
}

struct Assets {
  ImFont* display = nullptr;    // Oswald Bold (the game's overlay and dialogs)
  ImFont* nav = nullptr;        // Oswald SemiBold
  ImFont* body = nullptr;       // Inter Regular
  ImFont* body_bold = nullptr;  // Inter SemiBold
  // The hub's type, a Helvetica Condensed (TeX Gyre Heros Cn) as in GTA IV's
  // menus: bold for the tabs, rows and titles, regular for text. The atlas is
  // baked once (each font at one size) and a font drawn at another size is
  // scaled, which pinches small text: so each face is baked at the sizes the
  // hub draws it at, and the hub draws from the nearest (AddMenuFonts).
  ImFont* menu = nullptr;          // (a face's handle: its largest size)
  ImFont* menu_regular = nullptr;
  std::vector<ImFont*> menu_sizes, menu_regular_sizes;  // ascending
  // The tabs: Helvetica Neue Heavy (null when the app was built without it:
  // the tabs in the menu type), at its sizes.
  ImFont* tab = nullptr;
  std::vector<ImFont*> tab_sizes;
  // The Lost and Damned's scuffs over its tabs (Grunge): white, the alpha how dark.
  Image grunge;
  Image logo;                  // the game's logo
  Image hero;                   // the picture behind it all
  // Home once an episode is installed: a panel for each game, by the game's
  // numbers (0 GTA IV, 1 The Lost and Damned, 2 The Ballad of Gay Tony): its
  // picture, a black and white copy, and its logo. hub/home/gta4.jpg (or
  // .png), tlad, tbogt, and gta4-logo.png, tlad-logo.png, tbogt-logo.png.
  struct HomeArt {
    Image picture, grey, logo;
    ImU32 color = 0;  // the glow's colour: the logo's (Accent), else the picture's
  };
  HomeArt home[3];
  std::map<uint32_t, Image> buttons;  // the PS5's button pictures, by Button (kLeft | kRight: the d-pad)
};

// Adds the menu type (TeX Gyre Heros Cn bold and regular, the files' bytes)
// to the atlas at the hub's sizes, for a 1080p and a 4K screen.
void AddMenuFonts(ImFontAtlas* atlas, const void* bold, int bold_bytes, const void* regular, int regular_bytes,
                  Assets& a);

// Adds the tabs' type (Helvetica Neue Heavy, the file's bytes) at the sizes
// the tabs are drawn at.
void AddTabFonts(ImFontAtlas* atlas, const void* data, int bytes, Assets& a);

// A picture's pixels (RGBA).
struct Pixels {
  int w = 0, h = 0;
  std::vector<unsigned char> rgba;
};
// The Lost and Damned's scuffed type: blotches, specks and scratches over the
// width of the tabs (white; the alpha is how much darker), the same each time.
Pixels Grunge(int w, int h);
constexpr int kGrungeW = 2048, kGrungeH = 96;
// At most max_h tall (each pixel the average of those it covers): Home's
// pictures need no more than the 4K screen's band.
Pixels Shrink(const unsigned char* rgba, int w, int h, int max_h);
// Black and white.
Pixels Grey(const Pixels& p);
// The picture's strongest colour, brightened (0 for one with none: black and white).
ImU32 Accent(const Pixels& p);
constexpr int kHomePictureHeight = 1400;

struct Paths {
  std::filesystem::path app;        // the app folder (/data/homebrew/PPSA94004)
  std::filesystem::path config;     // native.toml (the game's settings, which the hub also writes)
  std::filesystem::path user_data;  // UserData: the saves are the game's content folders in it
  std::filesystem::path install;    // UserData/LibertyRecomp: the game is installed in its game/
};

// The sounds the hub asks for (ps5_hub.cpp plays them).
enum class Sound { kMove, kSelect, kBack, kPage, kOn, kOff, kStart, kError };

// What the hub plays through (none: silent, as in tools/hub_preview).
struct Audio {
  std::function<void(Sound)> sfx;
  // The menu music: the next song (returns its name; empty: no songs). (Unused:
  // songs follow each other on their own.)
  std::function<std::string()> next_song;
};

// The pages, in the order of the top bar. Display, Game and Debug are the
// settings, by section. Debug is shown only with the debug options (Hub's
// debug_options_).
enum Tab { kHome, kDisplayTab, kGameTab, kEpisodesTab, kSavesTab, kTrophiesTab, kAboutTab, kDebugTab, kTabCount };

class Hub {
 public:
  void Init(const Paths& paths, const Assets& assets, std::string version);
  void SetAudio(Audio audio) { audio_ = std::move(audio); }
  // Draws the hub over the whole display; true when the game should start.
  bool Draw(const Pad& pad, float dt);
  // From PLAY until Draw returns true: the hub fades out (and its music).
  bool Leaving() const { return leaving_; }
  // A song started (not shown).
  void NowPlaying(const std::string& name);
  bool HasMusic() const { return music_; }
  // The game is installed (PLAY starts it; else it opens the installer).
  bool Installed() const { return state_.installed; }
  // The game chosen on Home (0 GTA IV, 1 The Lost and Damned, 2 The Ballad of
  // Gay Tony): the hub has its look and plays its menu music, on every page.
  int Game() const { return HomeGame(); }
  // Installing: the PS5 should stay awake.
  bool Busy() const;
  void SetHasMusic(bool on) { music_ = on; }
  // Settings' Menu Music (hub_music_volume, 50% when not set), 0..1.
  float MusicVolume() const;
  // What loads a picture (a PNG) for the hub: the trophies' (none: drawn without them).
  // Settings' Saves page: Xbox 360 saves waiting in the import folder, and
  // importing them (returns a message for the player).
  struct SaveImport {
    std::function<int()> pending;
    std::function<std::string()> run;
  };
  void SetSaveImport(SaveImport import) { save_import_ = std::move(import); }
  // Display's Refresh Rate, switched as soon as it's chosen: whether the TV is
  // at 120 Hz, and the switch (false: it stayed where it was).
  struct Display {
    std::function<bool()> high_refresh;
    std::function<bool(bool)> set_high_refresh;
    std::function<bool()> vrr;    // (the 120 Hz output a VRR one)
    std::function<bool()> ready;  // (the TV open: a switch can be made)
  };
  void SetDisplay(Display display) { display_ = std::move(display); }
  void SetImageLoader(std::function<Image(const std::filesystem::path&)> load) { load_image_ = std::move(load); }

  struct Setting;

 private:
  struct SaveItem {  // a content folder of the game's (a save)
    std::string name, kind;
    std::filesystem::path folder;
    uint32_t files = 0;
    uint64_t bytes = 0;
    int64_t modified = 0;  // unix time
  };
  struct Backup {
    std::string name;
    uint32_t files = 0;
    uint64_t bytes = 0;
  };
  // The Saves page's lists, made off the hub's thread: every save and backup
  // folder walked for its size (a backup is a whole copy of the saves), which
  // held the hub up each time the page came by.
  struct SavesScan {
    std::vector<SaveItem> saves;
    std::vector<Backup> backups;
    int pending = 0;  // Xbox 360 saves waiting in import/
  };
  static SavesScan ScanSaves(const Paths& paths, const std::function<int()>& pending);
  std::future<SavesScan> saves_scan_;
  int pictures_this_frame_ = 0;  // trophy pictures loaded this frame
  bool saves_again_ = false;  // (asked for again while one was being made)
  // A row of a list: an item, or a heading over the next ones.
  struct Row {
    int item = -1;  // -1: a heading
    std::string heading;
    float y = 0;
  };

  void Load();
  void LoadSaves();
  std::string Value(const std::string& key, const std::string& fallback) const;
  void Save(const std::string& key, const std::string& value);
  int SettingIndex(const Setting& r) const;
  // The Display page's presets (the Preset row): which one every setting it
  // sets matches (kPresetCustom: none), and choosing one.
  static constexpr int kPresetCustom = 3;
  int PresetNow() const;
  void ApplyPreset(int preset);

  // pages
  void DrawBackdrop();
  void DrawTopBar();
  void DrawFooter();
  void DrawHints();
  void DrawConfirm();
  void DrawHome();
  // Home: the game PLAY starts in, by the game's numbers (gta4_start_episode):
  // 0 GTA IV, 1 The Lost and Damned, 2 The Ballad of Gay Tony. A choice once
  // an episode is installed; kept in native.toml for the next time.
  int home_game_ = 0;  // (GTA IV at each start)
  float home_wide_[3] = {1, 0, 0};  // how far each pillar is open (the chosen one)
  float home_from_[3] = {1, 0, 0};  // (where each was when the choice was made)
  int home_shown_ = 0;              // (the choice the pillars are opening to)
  float home_switch_ = 1;           // how far the choice has opened (0 to 1, over kSwitch)
  float home_pop_ = 0;              // the choice's flash (1, fading)
  float home_glare_ = 0;            // seconds since the choice (the glare's sweeps)
  float home_nudge_ = 1;            // a press past the last pillar (0 to 1)
  int home_nudge_dir_ = 0;
  std::vector<int> HomeGames() const;  // GTA IV and the episodes installed, left to right
  int HomeGame() const;                // home_game_, or GTA IV if it's gone
  void DrawHomePanels(const std::vector<int>& games);
  void DrawHomePanel(int game, float x0, float x1, float top, float bottom, float lean, float on);
  void FillPoly(const std::vector<ImVec2>& poly, const std::function<ImU32(ImVec2)>& color);
  void Quad(ImVec2 tl, ImVec2 tr, ImVec2 br, ImVec2 bl, ImU32 ctl, ImU32 ctr, ImU32 cbr, ImU32 cbl);
  void DrawSettings();
  void DrawSaves();
  void DrawEpisodes();
  void DrawAbout();
  // The settings on a page (Display, Game, Debug): indices into Settings().
  std::vector<int> SettingsOn(int tab) const;
  // The debug options (hub_debug_options): the Debug page and the Display
  // page's rarer settings, shown by holding Cross on About for a few seconds
  // (and hidden again the same way). Hidden, they keep their recommended values.
  bool debug_options_ = false;
  float about_hold_ = 0;         // how long Cross has been held on About
  bool about_hold_done_ = false;  // (switched: not again until it's let go)
  static constexpr float kUnlockHold = 3, kUnlockShow = 0.6f;
  int TabsShown() const { return debug_options_ ? kTabCount : kDebugTab; }  // (Debug is the last)
  bool Shown(const Setting& r) const;
  void KeepHiddenSettings();
#if defined(__EMSCRIPTEN__)
  void ApplyWebDeviceDefaults();  // [web port] (hub_ui.cpp)
#endif
  void SetDebugOptions(bool on);
  static std::string VersionLabel(const std::string& v);
  static bool IsSettingsTab(int tab) { return tab == kDisplayTab || tab == kGameTab || tab == kDebugTab; }
  // the hub's two faces (the overlay's when the menu type didn't load)
  ImFont* Bold() const { return a_.menu ? a_.menu : a_.body_bold; }
  ImFont* Regular() const { return a_.menu_regular ? a_.menu_regular : a_.body; }
  // A title over a page of its own (the installer): yellow, as the game's.
  void Heading(const std::string& title, const std::string& subtitle);
  // the installer (hub_installer_page.cpp): over the pages, from PLAY
  // while the game isn't installed
  // The installer: for the game, or (0, 1) one of the episodes.
  void OpenInstaller(int episode = -1);
  void CloseInstaller();
  void DrawInstaller();
  void InstallerInput(uint32_t pressed);
  void InstallerHints(float right);
  void Row3(float y, const char* label, const std::string& name, int status, const std::string& note);
  void Spinner(float cx, float cy, float r, ImU32 col);
  void Tick(float cx, float cy, float size, ImU32 col);
  void XMark(float cx, float cy, float size, ImU32 col);
  // the Trophies page (hub_trophies_page.cpp)
  struct TrophyItem {
    int index;  // in trophies_.trophies; -1: the platinum
    int group;  // 0 earned, 1 not yet
  };
  // The trophies read again (when the page comes by), off the hub's thread:
  // the page takes them when they're there (TakeTrophies).
  void LoadTrophies();
  void TakeTrophies(trophies::List list);
  std::future<trophies::List> trophies_load_;
  void DrawTrophies();
  float TrophySummary();  // returns the y under it
  void TrophyIcon(int index, float x, float y, float size, float over);
  Image TrophyPicture(int index);
  void Empty(const char* title, const std::string& text);
  void GoTo(int tab, float dir);
  // A scrolled list with headings at the page's left: rows' items are
  // 0..n-1 in order; `draw` draws item i's row at y (`over`: how much the
  // focus highlight covers it, 0..1).
  void List(std::vector<Row>& rows, int sel, float rh, const std::function<void(int, float, float)>& draw);
  // The details panel at the right; `draw` from its top, faded in.
  void Panel(const std::function<void(float x, float y, float w)>& draw);
  // Label: value lines of the panel; returns the y after them.
  float Facts(float x, float y, float w, const std::vector<std::pair<std::string, std::string>>& facts);

  void Navigate(const Pad& pad);
#if defined(__EMSCRIPTEN__)
  // [web port] What can be clicked, as this frame draws it: a tab, a list
  // row or a hint's button, in the overlay's pixels (where it was drawn: the
  // pages move the hub's coordinates about as they draw).
  struct HitRegion {
    float x0, y0, x1, y1;
    int kind;  // 0 tab, 1 row (its item), 2 button
    uint32_t value;
  };
  std::vector<HitRegion> hits_, last_hits_;
  void Hit(float x, float y, float w, float h, int kind, uint32_t value) {
    const ImVec2 a = P(x, y), b = P(x + w, y + h);
    hits_.push_back({a.x, a.y, b.x, b.y, kind, value});
  }
  // The mouse over the last frame's regions, as presses for Navigate.
  void MouseInput(Pad& pad);
#endif
  int Items() const;
  void Activate(int dir);
  void Play(Sound s) {
    if (audio_.sfx) audio_.sfx(s);
  }
  void Toast(std::string text) {
    toast_ = std::move(text);
    toast_time_ = 3;
    toast_song_ = false;
  }
  void Confirm(std::string title, std::string text, std::string yes, std::function<void()> action) {
    confirm_title_ = std::move(title), confirm_text_ = std::move(text), confirm_yes_ = std::move(yes);
    confirm_ = std::move(action);
    confirm_no_ = nullptr;
    confirm_no_label_.clear();
    confirm_in_ = 0;
  }
  // ...with Circle doing something too (`no_label` in the hints instead of Cancel).
  void Confirm(std::string title, std::string text, std::string yes, std::function<void()> action, std::string no_label,
               std::function<void()> no_action) {
    Confirm(std::move(title), std::move(text), std::move(yes), std::move(action));
    confirm_no_label_ = std::move(no_label);
    confirm_no_ = std::move(no_action);
  }
  // A message (Cross closes it).
  void Notice(std::string title, std::string text) { Confirm(std::move(title), std::move(text), "OK", [] {}); }
  void BackUpSaves(const std::string& why);
  // Every setting (Display, Game and Debug) back to the recommended one.
  void ResetSettings();
  // The TV to 120 Hz or back to 60, now, and what came of it.
  void SwitchRefresh(bool high);
  // The Refresh Rate at 120: switched at the hub's first frames (Draw).
  bool refresh_checked_ = false;
  void RestoreSaves(const Backup& b);

  Paths paths_;
  Assets a_;
  Audio audio_;
  std::string version_;
  // layout: virtual 1920 x 1080, scaled to the display
  float s_ = 1, ox_ = 0, oy_ = 0;
  ImDrawList* dl_ = nullptr;
  float time_ = 0, dt_ = 0;

  int tab_ = 0;
  int item_[8] = {};  // the focused row on each page
  // trophies
  trophies::List trophies_;
  std::vector<TrophyItem> trophy_items_;
  std::map<uint32_t, Image> trophy_pictures_;  // by image ID (0: the game's own picture, the platinum's)
  std::function<Image(const std::filesystem::path&)> load_image_;
  bool launch_ = false;
  std::string toast_;
  float toast_time_ = 0;
  bool toast_song_ = false;  // (the toast is a song's name)
  std::string pending_song_;  // (shown once the hub has faded in)
  bool music_ = false;
  // a question (restore a backup?): Cross does it, Circle doesn't (or does confirm_no_)
  std::function<void()> confirm_, confirm_no_;
  std::string confirm_title_, confirm_text_, confirm_yes_, confirm_no_label_;
  float confirm_in_ = 0;
  // animation (eased toward their targets)
  float tab_x_ = -1, tab_w_ = 0, focus_y_ = -1, shade_ = 0.3f, page_in_ = 1;
  float tab_lit_[kTabCount] = {};  // each tab's yellow, easing in and out
  float page_dir_ = 1;          // the page slides in from this side (+1: the right)
  float panel_in_ = 1;          // the details panel, after the focus moves
  int panel_item_ = -1;
  float value_in_ = 1, value_dir_ = 0;  // a value, after it changes
  float press_ = 0;             // a flash on the focused thing when it's pressed
  float hero_x_ = -1;           // the picture drifts sideways with the pages
  // Each game's share of the look (easing to the one chosen on Home), and
  // the colours mixed from them for this frame.
  float theme_[3] = {1, 0, 0};
  Palette pal_ = GamePalette(0);
  void Theme();
  void DrawTabs(int game, float alpha);
  float ToastShown() const;  // how much the footer's message shows (0..1)
  bool leaving_ = false;        // PLAY: fading out
  float leave_ = 0;
  float scroll_f_ = 0, scroll_px_ = 0;  // the page's list: its scroll, shown and wanted

  std::vector<std::pair<std::string, std::string>> config_;  // top-level key = value
  std::vector<SaveItem> save_items_;
  SaveImport save_import_;
  int pending_imports_ = 0;
  Display display_;
  // the game's files
  install::State state_;
  // the installer
  std::unique_ptr<install::Job> job_;
  bool installer_ = false;
  install::Job::Phase job_phase_ = install::Job::Phase::kChecking;
  // The game's files looked at again, off the hub's thread (the drive can be
  // slow): state_ takes the answer when it's there.
  std::future<install::State> looking_;
  void LookAgain();
  int installer_episode_ = -1;  // what the installer installs: the game (-1) or an episode
  float shown_progress_ = 0;
  uint64_t free_space_ = 0;
  install::Drive drive_;  // where the game installs
  std::filesystem::path InstallRoot() const;
  std::vector<Backup> backups_;
  // helpers (virtual coordinates)
  ImVec2 P(float x, float y) const { return ImVec2(ox_ + x * s_, oy_ + y * s_); }
  // The face to draw `f` at `size` with: of the menu type's sizes, the nearest
  // baked one at or above it on the screen (one that has all of `t`'s letters).
  ImFont* Pick(ImFont* f, float size, const std::string* t = nullptr) const;
  float Ease(float& v, float target, float speed = 16);
  void Text(ImFont* f, float size, float x, float y, ImU32 col, const std::string& t, float align = 0,
            float spacing = 0);
  float TextWidth(ImFont* f, float size, const std::string& t, float spacing = 0) const;
  // `t` cut to fit `width` (with "...").
  std::string Fit(ImFont* f, float size, const std::string& t, float width) const;
  std::vector<std::string> WrapLines(ImFont* f, float size, float width, const std::string& t);
  float Wrapped(ImFont* f, float size, float x, float y, float width, ImU32 col, const std::string& t,
                float line = 1.4f);
  void Rect(float x, float y, float w, float h, ImU32 col, float r = 0);
  // Text with its capitals centred on `cy` (Text's y is the top of its line).
  void TextMid(ImFont* f, float size, float x, float cy, ImU32 col, const std::string& t, float align = 0,
               float spacing = 0);
  float CapMid(ImFont* f, float size) const;    // the capitals' middle, below the line's top
  float Baseline(ImFont* f, float size) const;  // the baseline, below the line's top
  // A PlayStation button centred on (cx, cy), its picture's square `size`
  // across; returns the width of the button drawn in it.
  float Glyph(uint32_t button, float cx, float cy, ImU32 col, float size);
  float GlyphWidth(uint32_t button, float size) const;
  // One hint in the footer, right-aligned at `right` on line `line` (0, 1):
  // the button, then what it does. Returns its left edge.
  float Hint(float right, uint32_t button, const char* label, int line = 0);
  void Fade(int first_vertex, float alpha);                    // fades what was drawn since
};

}  // namespace gta4ps5::hub
