// GTA IV PS5 - Xbox 360 saves into the PS5 app (see save_import.h).
#include "save_import.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <random>
#include <span>

#include <fmt/format.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/logging.h>
#include <rex/string/utf8.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xcontent.h>

#if defined(__EMSCRIPTEN__)
// (in the browser the import folder holds the files the player picked, in
// this browser's storage: web/web_saves_transfer.cpp)
extern "C" const char* rex_web_xbox360_import_folder();
#endif

namespace gta4ps5 {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kGta4TitleId = 0x545407F2;
constexpr uint64_t kDefaultProfile = 0xB13EBABEBABEBABEull;  // the runtime's profile

struct FileDestroyer {
  void operator()(rex::filesystem::File* f) const {
    if (f) f->Destroy();
  }
};

bool IsDir(const rex::filesystem::Entry* e) {
  return e && (e->attributes() & rex::filesystem::kFileAttributeDirectory) != 0;
}

bool SafeName(const std::string& n) {
  return !n.empty() && n != "." && n != ".." && n.find_first_of("/\\:") == std::string::npos;
}

bool CopyEntry(rex::filesystem::Entry* entry, const fs::path& to, std::string& error) {
  if (IsDir(entry)) {
    std::error_code ec;
    fs::create_directories(to, ec);
    for (const auto& child : entry->children()) {
      if (!SafeName(child->name())) continue;
      if (!CopyEntry(child.get(), to / child->name(), error)) return false;
    }
    return true;
  }
  rex::filesystem::File* raw = nullptr;
  if (entry->Open(rex::filesystem::FileAccess::kGenericRead, &raw) != 0 || !raw) {
    error = "couldn't open " + entry->path();
    return false;
  }
  std::unique_ptr<rex::filesystem::File, FileDestroyer> file(raw);
  std::ofstream out(to, std::ios::binary | std::ios::trunc);
  if (!out) {
    error = "couldn't write " + to.string();
    return false;
  }
  std::vector<uint8_t> buf(1 << 20);
  size_t offset = 0;
  while (offset < entry->size()) {
    const size_t want = std::min(buf.size(), size_t(entry->size() - offset));
    size_t got = 0;
    if (file->ReadSync(std::span<uint8_t>(buf).first(want), offset, &got) != 0 || got == 0) {
      error = "couldn't read " + entry->path();
      return false;
    }
    out.write(reinterpret_cast<const char*>(buf.data()), std::streamsize(got));
    offset += got;
  }
  return bool(out);
}

// The runtime's multiplayer identity (live_compatibility.cpp IdentityDisk):
// its XUID replaces the default profile's, so it's the profile the game
// lists saves for. Made here as the runtime would when the game hasn't
// started yet, so saves imported before the first start are found too.
struct IdentityDisk {
  std::array<uint8_t, 8> magic{};
  uint32_t version = 0;
  uint64_t xuid = 0;
  uint64_t machine_id = 0;
  std::array<uint8_t, 6> ethernet_address{};
  std::array<uint8_t, 32> install_secret{};
  std::array<char, 16> player_name{};
};
constexpr std::array<uint8_t, 8> kIdentityMagic = {'L', 'B', 'R', 'L', 'I', 'V', 'E', 0};

uint64_t IdentityXuid(const fs::path& saves_root) {
  const fs::path path = saves_root / "liberty_live_identity.bin";
  IdentityDisk disk{};
  {
    std::ifstream in(path, std::ios::binary);
    if (in && in.read(reinterpret_cast<char*>(&disk), sizeof(disk)) && disk.magic == kIdentityMagic &&
        disk.version == 1 && disk.xuid && disk.machine_id)
      return disk.xuid;
    if (in.gcount() > 0) return 0;  // (someone else's format: leave it alone)
  }
  std::random_device rd;
  auto random64 = [&] { return (uint64_t(rd()) << 32) | rd(); };
  disk = {};
  disk.magic = kIdentityMagic;
  disk.version = 1;
  do disk.xuid = random64() & ~0x00C0000000000000ULL; while (!disk.xuid);
  do disk.machine_id = random64(); while (!disk.machine_id);
  for (auto& b : disk.ethernet_address) b = uint8_t(rd());
  disk.ethernet_address.front() = uint8_t((disk.ethernet_address.front() & 0xFC) | 0x02);
  for (auto& b : disk.install_secret) b = uint8_t(rd());
  std::memcpy(disk.player_name.data(), "Player", 6);
  std::error_code ec;
  fs::create_directories(saves_root, ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(&disk), sizeof(disk));
  return out ? disk.xuid : 0;
}

// The header the runtime lists a save by (content_manager.cpp
// WriteContentHeaderFile): without one it lists the folder's name as the
// display name, which the game can't read a date from, so it skips the save.
bool WriteContentHeader(const fs::path& profile, const std::string& name, const std::u16string& display) {
  rex::system::xam::XCONTENT_AGGREGATE_DATA data{};
  data.device_id = static_cast<uint32_t>(rex::system::xam::DummyDeviceId::HDD);
  data.content_type = rex::system::XContentType::kSavedGame;
  data.set_display_name(display);
  data.set_file_name(name);
  data.xuid = std::strtoull(profile.filename().string().c_str(), nullptr, 16);
  data.title_id = kGta4TitleId;
  const fs::path path = profile / "545407F2" / "Headers" / "00000001" / (name + ".header");
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(&data), sizeof(data));
  return bool(out);
}

// The profile folders saves go in: the ones the game already uses and the
// identity's, else the runtime's default profile.
std::vector<fs::path> ProfileFolders(const fs::path& saves_root) {
  std::vector<fs::path> out;
  std::error_code ec;
  for (fs::directory_iterator it(saves_root, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string n = it->path().filename().string();
    if (!it->is_directory(ec) || n.size() != 16) continue;
    bool hex = true;
    for (char c : n) hex &= std::isxdigit(static_cast<unsigned char>(c)) != 0;
    if (hex) out.push_back(it->path());
  }
  const uint64_t xuid = IdentityXuid(saves_root);
  const fs::path own = saves_root / fmt::format("{:016X}", xuid ? xuid : kDefaultProfile);
  if (std::find(out.begin(), out.end(), own) == out.end()) out.push_back(own);
  return out;
}

}  // namespace

fs::path SaveImportFolder(const fs::path& app) {
#if defined(__EMSCRIPTEN__)
  return rex_web_xbox360_import_folder();
#else
  return app / "import";
#endif
}

int PendingSaveImports(const fs::path& app) {
  int n = 0;
  std::error_code ec;
  for (fs::directory_iterator it(SaveImportFolder(app), ec), end; !ec && it != end; it.increment(ec))
    if (it->is_regular_file(ec) && it->path().filename().string()[0] != '.') ++n;
  return n;
}

SaveImportResult ImportXbox360Saves(const fs::path& app, const fs::path& saves_root) {
  SaveImportResult result;
  const fs::path folder = SaveImportFolder(app);
  std::error_code ec;
  fs::create_directories(folder, ec);
  std::vector<fs::path> files;
  for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
    if (it->is_regular_file(ec) && it->path().filename().string()[0] != '.') files.push_back(it->path());

  for (const fs::path& file : files) {
    const std::string name = file.filename().string();
    auto header = rex::filesystem::StfsContainerDevice::ReadPackageHeader(file);
    if (!header) {
      result.errors.push_back(name + ": not an Xbox 360 save package");
      continue;
    }
    const uint32_t title = header->metadata.execution_info.title_id;
    const auto type = header->metadata.content_type;
    if (title != kGta4TitleId) {
      result.errors.push_back(fmt::format("{}: another game's ({:08X})", name, title));
      continue;
    }
    if (type != rex::filesystem::XContentType::kSavedGame) {
      result.errors.push_back(name + ": not a saved game");
      continue;
    }
    rex::filesystem::StfsContainerDevice device("import:", file, false);
    if (!device.Initialize()) {
      result.errors.push_back(name + ": the package is damaged");
      continue;
    }
    auto* root = device.ResolvePath("");
    if (!root) {
      result.errors.push_back(name + ": the package is empty");
      continue;
    }
    // The game dates each save (and picks the newest) from its display name
    // ("<mission> - dd/mm/yy hh:mm:ss"): the package's, in the first language
    // it has one in (a Spanish 360 has only the Spanish one).
    std::u16string display = header->metadata.display_name(rex::system::XLanguage::kEnglish);
    for (uint32_t lang = 1; display.empty() && lang < uint32_t(rex::system::XLanguage::kMaxLanguages); ++lang)
      display = header->metadata.display_name(rex::system::XLanguage(lang));
    bool ok = true;
    for (const fs::path& profile : ProfileFolders(saves_root)) {
      const fs::path dest = profile / "545407F2" / "00000001" / name;
      fs::remove_all(dest, ec);
      std::string error;
      if (!CopyEntry(root, dest, error)) {
        result.errors.push_back(name + ": " + error);
        ok = false;
        break;
      }
      if (!WriteContentHeader(profile, name, display.empty() ? rex::string::to_utf16(name) : display)) {
        result.errors.push_back(name + ": couldn't write its header");
        ok = false;
        break;
      }
      REXLOG_INFO("PS5 save import: {} -> {}", name, dest.string());
    }
    if (!ok) continue;
    ++result.imported;
    result.names.push_back(name);
#if !defined(__EMSCRIPTEN__)
    fs::create_directories(folder / "done", ec);
    fs::rename(file, folder / "done" / name, ec);
#endif
  }
#if defined(__EMSCRIPTEN__)
  // (the picked copies are done with, imported or not: the player picks again)
  fs::remove_all(folder, ec);
#endif
  for (const auto& e : result.errors) REXLOG_WARN("PS5 save import: {}", e);
  return result;
}

}  // namespace gta4ps5
