/**
 * @file        web/web_startup.cpp
 * @brief       [web port] Disc, title update and install before the app starts
 *
 * Runs at the start of main(), on the game's own worker thread:
 *
 * 1. The disc image, Title Update 8 and the RPF key are mounted read-only
 *    from the host (/iso/gta4.iso, /iso/tu8, /iso/aes_key) with WasmFS's
 *    fetch backend, which reads
 *    them in chunks with HTTP range requests. The browser's main thread may
 *    not block on the network, so this cannot happen there.
 * 2. The Liberty Recompiled folder (game, saves, caches) lives in the
 *    browser's origin-private file system (OPFS), so the multi-GB install is
 *    kept on disk between visits instead of in memory.
 * 3. If the game is not installed yet, the installer runs without its dialog
 *    (which needs a renderer), from the mounted disc and update, and reports
 *    progress to the console.
 *
 * @license     BSD 3-Clause License
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <unistd.h>
#include <thread>

#include <emscripten/console.h>
#include <emscripten/wasmfs.h>
#include <sys/stat.h>

#include "install/gta4_installer.h"

// Reports guest threads stuck in a kernel wait (core/ps5_diag_web.cpp).
extern "C" void rex_web_start_wait_monitor();

namespace {

constexpr uint32_t kChunkSize = 4u << 20;
// (the hub's install folder: these two, and the files in the install folder
// on the computer the page is served from, web_host_files.cpp)
constexpr const char* kInstallFolder = "/home/web_user/install";
constexpr const char* kImportFolder = "/home/web_user/import";
constexpr const char* kDiscPath = "/home/web_user/install/gta4.iso";
constexpr const char* kUpdatePath = "/home/web_user/install/TU_1A581VI_000000K000000.0000000000205";
constexpr const char* kLibertyRoot = "/home/web_user/.local/share/LibertyRecomp";

void Log(const std::string& message) {
  emscripten_out(("web: " + message).c_str());
}

bool MountFetched(const char* url, const char* path) {
  backend_t fetch = wasmfs_create_fetch_backend(url, kChunkSize);
  if (wasmfs_create_file(path, 0444, fetch) < 0) {
    Log(std::string("could not mount ") + path);
    return false;
  }
  Log(std::string("mounted ") + path + " from " + url);
  return true;
}

// A folder of files each fetched when read, from the folder's URL plus the
// file's path in it: one backend, so one fetching thread for all of them
// (a backend per file started a thread per file, each loading the engine's
// script over the network first).
void MountFetchedFolder(const char* url, const char* path, std::initializer_list<std::string> files) {
  backend_t fetch = wasmfs_create_fetch_backend(url, kChunkSize);
  if (wasmfs_create_directory(path, 0777, fetch) < 0) {
    Log(std::string("could not mount ") + path);
    return;
  }
  int mounted = 0;
  for (const std::string& file : files) {
    const std::string full = std::string(path) + "/" + file;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(full).parent_path(), ec);
    const int fd = open(full.c_str(), O_CREAT | O_RDONLY, 0444);
    if (fd < 0) {
      Log("could not mount " + full);
      continue;
    }
    close(fd);
    mounted++;
  }
  Log(std::string("mounted ") + path + " from " + url + " (" + std::to_string(mounted) + " files)");
}

void InstallIfNeeded() {
  const std::filesystem::path liberty_root = kLibertyRoot;
  std::string reason;
  if (gta4::install::IsInstallReady(liberty_root / "game", &reason)) {
    Log("GTA IV is already installed in browser storage");
    return;
  }
  Log("GTA IV is not installed (" + reason + "); installing from the disc and Title Update 8");

  gta4::install::Selection selection;
  selection.game_source = kDiscPath;
  selection.update_source = kUpdatePath;
  gta4::install::Progress progress;
  gta4::install::Result result;
  std::atomic<bool> done = false;
  const auto start = std::chrono::steady_clock::now();
  std::thread worker([&] {
    result = gta4::install::Install(selection, liberty_root, progress);
    done.store(true, std::memory_order_release);
  });

  auto seconds = [&] {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  };
  while (!done.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    const double copied = double(progress.copied_bytes.load()) / (1 << 20);
    const double total = double(progress.total_bytes.load()) / (1 << 20);
    char line[160];
    std::snprintf(line, sizeof(line), "install: %.0f / %.0f MiB (%.0f%%), %.1f MiB/s", copied,
                  total, total > 0 ? copied * 100.0 / total : 0.0,
                  copied / std::max(seconds(), 0.001));
    Log(line);
  }
  worker.join();

  char line[96];
  std::snprintf(line, sizeof(line), "install %s after %.0f s", result.success ? "finished" : "FAILED",
                seconds());
  Log(std::string(line) + (result.error.empty() ? "" : ": " + result.error));
}

// The Display settings the Xbox 360 reference video has, once per profile:
// Subtitles on, Brightness 7 and Contrast 6 (read off its sliders; the
// profile the game makes has off, 5 and 5). Brightness and contrast are
// only changed while they still hold those defaults, so a player's own
// choice stays. (They suit the picture once it goes through the console's
// display gamma, scene3d.js presentRamped; an earlier build set them
// without it, found them too bright and put 5 and 5 back - those profiles
// get them again here.) Patched in the settings blob (setting 0x63E83FFF,
// 8-byte big-endian key / value pairs) before the game reads it; a marker
// beside it keeps the player's choices after that.
void SeedDisplaySettings() {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path saves = fs::path(kLibertyRoot) / "saves";
  for (auto title = fs::directory_iterator(saves, ec); !ec && title != fs::directory_iterator(); title.increment(ec)) {
    const fs::path profiles = title->path() / "profile";
    std::error_code ec2;
    for (auto user = fs::directory_iterator(profiles, ec2); !ec2 && user != fs::directory_iterator();
         user.increment(ec2)) {
      const fs::path blob = user->path() / "63E83FFF";
      const fs::path marker = user->path() / "63E83FFF.web-display-seeded-v3";
      std::error_code ec3;
      if (!fs::is_regular_file(blob, ec3) || fs::exists(marker, ec3)) continue;
      FILE* file = std::fopen(blob.c_str(), "r+b");
      if (!file) continue;
      uint8_t bytes[1024];
      const size_t size = std::fread(bytes, 1, sizeof(bytes), file);
      auto be32 = [&](size_t at) {
        return uint32_t(bytes[at]) << 24 | uint32_t(bytes[at + 1]) << 16 | uint32_t(bytes[at + 2]) << 8 | bytes[at + 3];
      };
      for (size_t at = 0; at + 8 <= size; at += 8) {
        const uint32_t key = be32(at), value = be32(at + 4);
        // (0xC8 brightness, 0xC9 contrast, 0xCB subtitles)
        const int seeded = key == 0xCB                ? 1
                           : key == 0xC8 && value == 5 ? 7
                           : key == 0xC9 && value == 5 ? 6
                                                       : -1;
        if (seeded < 0) continue;
        const uint8_t word[4] = {0, 0, 0, uint8_t(seeded)};
        std::fseek(file, long(at + 4), SEEK_SET);
        std::fwrite(word, 1, 4, file);
      }
      std::fclose(file);
      if (FILE* done = std::fopen(marker.c_str(), "wb")) std::fclose(done);
      Log("display settings as in the reference (subtitles on, brightness 7, contrast 6): " + blob.string());
    }
  }
}

}  // namespace

// Called by main(), which runs on a worker.
extern "C" void rex_web_main_thread_init() {
  // Logs go to the browser console line by line, not when a buffer fills.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  rex_web_start_wait_monitor();

  backend_t root = wasmfs_get_backend_by_path("/");
  for (const char* dir : {"/home", "/home/web_user", "/home/web_user/.local",
                          "/home/web_user/.local/share", kInstallFolder, kImportFolder}) {
    wasmfs_create_directory(dir, 0777, root);
  }
  // The installer looks for the RPF key at GTA4_RPF_AES_KEY_SOURCE (/game/...).
  wasmfs_create_directory("/game", 0777, root);
  const bool mounted = MountFetched("/iso/gta4.iso", kDiscPath) &&
                       MountFetched("/iso/tu8", kUpdatePath) &&
                       MountFetched("/iso/aes_key", GTA4_RPF_AES_KEY_SOURCE);
  // The button-prompt sets (rpf_button_prompts: the keyboard's made by
  // tools/xtd_tools/build_keyboard_prompts.py, the PlayStation and Switch
  // sets' Controls pictures by build_controller_diagrams.py), where it looks
  // for them; each file fetched only when the chosen set's is read. Each
  // set's controller.bin is its Controls picture, put into the player's own
  // frontend_360.xtd when the archive is built.
  MountFetchedFolder("/prompts", "/game/button_prompts",
                     {"keyboard/buttons_360.xtd", "xbox360/buttons_360.xtd", "xbox_one/buttons_360.xtd",
                      "xbox_series_x/buttons_360.xtd", "ps3/buttons_360.xtd", "ps4/buttons_360.xtd",
                      "ps5/buttons_360.xtd", "switch/buttons_360.xtd", "steam_deck/buttons_360.xtd",
                      "steam_controller/buttons_360.xtd", "keyboard/controls_foot.bin",
                      "keyboard/controls_vehicle.bin", "keyboard/controller.bin", "ps3/controller.bin",
                      "ps4/controller.bin", "ps5/controller.bin", "switch/controller.bin",
                      "xbox_one/controller.bin", "xbox_series_x/controller.bin", "controller-place.txt"});

  // The PS5 hub's app folder (ps5/package/hub and ps5/assets/fonts, which the
  // test server serves at /app0): the files it looks for, each fetched when read.
  wasmfs_create_directory("/app0", 0777, root);
  MountFetchedFolder("/app0/hub", "/app0/hub",
                     {"buttons/Circle.png", "buttons/Create.png", "buttons/Cross.png", "buttons/D-Pad.png",
                      "buttons/L1.png", "buttons/L2.png", "buttons/L3.png", "buttons/Options.png",
                      "buttons/R1.png", "buttons/R2.png", "buttons/R3.png", "buttons/Square.png",
                      "buttons/Touch Pad.png", "buttons/Triangle.png", "hero.jpg", "home/gta4.jpg",
                      "home/tbogt-logo.png", "home/tbogt.jpg", "home/tlad-logo.png", "home/tlad.jpg", "logo.png",
                      "music/GTA IV Menu.mp3", "music/The Ballad of Gay Tony Menu.mp3",
                      "music/The Lost and Damned Menu.mp3", "sfx/click.ogg", "sfx/confirm.ogg", "sfx/tab.ogg"});
  MountFetchedFolder("/app0/fonts", "/app0/fonts",
                     {"inter-latin-400-normal.ttf", "inter-latin-600-normal.ttf", "oswald-latin-600-normal.ttf",
                      "oswald-latin-700-normal.ttf", "texgyreheroscn-bold.otf", "texgyreheroscn-regular.otf"});
  // The hub finds the game as on the PS5: <app>/UserData/LibertyRecomp, the
  // disc image, title update and episodes in <app>/install, and Xbox 360
  // saves in <app>/import.
  symlink("/home/web_user/.local/share", "/app0/UserData");
  symlink(kInstallFolder, "/app0/install");
  symlink(kImportFolder, "/app0/import");

  if (wasmfs_create_directory(kLibertyRoot, 0777, wasmfs_create_opfs_backend()) < 0) {
    Log(std::string("could not put ") + kLibertyRoot + " in browser storage (OPFS)");
    return;
  }
  Log(std::string(kLibertyRoot) + " is in browser storage (OPFS)");
  // (an earlier build's settings save could leave an empty directory where
  // native.toml goes, which startup would then fail to read: removed)
  {
    std::error_code ec;
    const std::filesystem::path config = std::filesystem::path(kLibertyRoot) / "native.toml";
    if (std::filesystem::is_directory(config, ec) && std::filesystem::is_empty(config, ec)) {
      std::filesystem::remove(config, ec);
      Log("removed a directory left where native.toml goes");
    }
  }

  SeedDisplaySettings();

  if (mounted) {
    InstallIfNeeded();
  }
}
