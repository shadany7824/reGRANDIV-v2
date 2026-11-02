#include "rpf_button_prompts.h"

#include "gta4_init.h"

#include <algorithm>
#include <functional>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/crypto/sha256.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/lzx.h>

extern "C" {
#include <rijndael-alg-fst.h>
}

#ifndef GTA4_BUTTON_PROMPT_ASSET_ROOT
#error "GTA4_BUTTON_PROMPT_ASSET_ROOT must point to the button prompt assets"
#endif

REXCVAR_DEFINE_STRING(gta4_button_prompts, "auto", "GTA IV/Controls",
                      "Button prompt pictures: auto (the controller found, else keyboard keys in the "
                      "browser), keyboard, touch, xbox360, xbox_one, xbox_series_x, ps3, ps4, ps5, switch, "
                      "steam_deck or steam_controller");

namespace gta4::button_prompts {
namespace {

constexpr uint32_t kRpf2Magic = 0x32465052;
constexpr uint32_t kResourceMagic = 0x05435352;
constexpr uint32_t kTextureDictionaryType = 0x07;
constexpr uint64_t kTocOffset = 0x800;
constexpr uint64_t kBlockSize = 0x800;
constexpr uint32_t kDirectoryBit = 0x80000000;
constexpr uint32_t kResourceMask = 0xC0000000;
constexpr uint32_t kResourceOffsetMask = 0x7FFFFF00;
constexpr std::string_view kPromptPath = "/textures/buttons_360.xtd";
// The pause menu's Controls picture (controller.dds), replaced when the set
// has one (tools/xtd_tools/build_controller_diagrams.py).
constexpr std::string_view kFrontendPath = "/textures/frontend_360.xtd";
constexpr size_t kNoEntry = static_cast<size_t>(-1);
constexpr std::string_view kPromptMount = "\\Device\\LibertyRecomp\\ButtonPrompts";

struct RpfHeader {
  uint32_t toc_size = 0;
  uint32_t entry_count = 0;
  bool encrypted = false;
};

struct TocEntry {
  size_t toc_offset = 0;
  uint32_t name_offset = 0;
  bool is_directory = false;
  uint32_t content_index = 0;
  uint32_t content_count = 0;
  uint32_t size = 0;
  uint32_t offset = 0;
  uint32_t size_in_archive = 0;
  uint32_t resource_type = 0;
  uint32_t resource_flags = 0;
  bool is_resource = false;
};

struct ParsedArchive {
  RpfHeader header;
  std::vector<uint8_t> toc;
  std::vector<TocEntry> entries;
  size_t string_table_offset = 0;
  size_t prompt_entry_index = 0;
  size_t frontend_entry_index = kNoEntry;
  uint64_t file_size = 0;
  uint64_t max_data_end = 0;
};

uint32_t ReadU32(std::span<const uint8_t> data, size_t offset) {
  return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
         (static_cast<uint32_t>(data[offset + 2]) << 16) |
         (static_cast<uint32_t>(data[offset + 3]) << 24);
}

void WriteU32(std::span<uint8_t> data, size_t offset, uint32_t value) {
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8);
  data[offset + 2] = static_cast<uint8_t>(value >> 16);
  data[offset + 3] = static_cast<uint8_t>(value >> 24);
}

uint64_t AlignUp(uint64_t value, uint64_t alignment) {
  const uint64_t remainder = value % alignment;
  if (remainder == 0) {
    return value;
  }
  return value + (alignment - remainder);
}

std::optional<std::vector<uint8_t>> ReadFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    return std::nullopt;
  }
  const auto end = stream.tellg();
  if (end < 0 || static_cast<uint64_t>(end) > std::numeric_limits<size_t>::max()) {
    return std::nullopt;
  }
  std::vector<uint8_t> data(static_cast<size_t>(end));
  stream.seekg(0, std::ios::beg);
  if (!data.empty() && !stream.read(reinterpret_cast<char*>(data.data()), data.size())) {
    return std::nullopt;
  }
  return data;
}

std::string HashBytes(std::span<const uint8_t> data) {
  return rex::crypto::sha256(
      std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
}

bool CryptToc(std::span<uint8_t> toc, std::span<const uint8_t> key, bool decrypt) {
  if (key.size() != 32 || toc.size() % 16 != 0) {
    return false;
  }

  std::array<u32, 4 * (MAXNR + 1)> round_keys{};
  const int rounds = decrypt ? rijndaelKeySetupDec(round_keys.data(), key.data(), 256)
                             : rijndaelKeySetupEnc(round_keys.data(), key.data(), 256);
  if (rounds <= 0) {
    return false;
  }

  std::array<uint8_t, 16> output{};
  for (int pass = 0; pass < 16; ++pass) {
    for (size_t offset = 0; offset < toc.size(); offset += output.size()) {
      if (decrypt) {
        rijndaelDecrypt(round_keys.data(), rounds, toc.data() + offset, output.data());
      } else {
        rijndaelEncrypt(round_keys.data(), rounds, toc.data() + offset, output.data());
      }
      std::memcpy(toc.data() + offset, output.data(), output.size());
    }
  }
  return true;
}

std::optional<std::string_view> EntryName(const ParsedArchive& archive, uint32_t name_offset) {
  const size_t offset = archive.string_table_offset + name_offset;
  if (offset >= archive.toc.size()) {
    return std::nullopt;
  }
  const auto* begin = reinterpret_cast<const char*>(archive.toc.data() + offset);
  const size_t available = archive.toc.size() - offset;
  const auto* end = static_cast<const char*>(std::memchr(begin, '\0', available));
  if (!end) {
    return std::nullopt;
  }
  return std::string_view(begin, static_cast<size_t>(end - begin));
}

bool FindPromptEntry(const ParsedArchive& archive, size_t index, std::string_view parent,
                     std::vector<bool>& active, size_t& result, std::string_view target = kPromptPath) {
  if (index >= archive.entries.size() || active[index]) {
    return false;
  }
  const auto& entry = archive.entries[index];
  if (!entry.is_directory) {
    return false;
  }

  active[index] = true;
  const uint64_t end = static_cast<uint64_t>(entry.content_index) + entry.content_count;
  if (end > archive.entries.size()) {
    active[index] = false;
    return false;
  }

  for (uint64_t child_index = entry.content_index; child_index < end; ++child_index) {
    const auto& child = archive.entries[static_cast<size_t>(child_index)];
    auto name = EntryName(archive, child.name_offset);
    if (!name) {
      active[index] = false;
      return false;
    }
    std::string path(parent);
    path.push_back('/');
    path.append(*name);
    if (!child.is_directory && path == target) {
      result = static_cast<size_t>(child_index);
      active[index] = false;
      return true;
    }
    if (child.is_directory &&
        FindPromptEntry(archive, static_cast<size_t>(child_index), path, active, result, target)) {
      active[index] = false;
      return true;
    }
  }
  active[index] = false;
  return false;
}

std::optional<ParsedArchive> ParseArchive(const std::filesystem::path& path,
                                          std::span<const uint8_t> key, std::string& error) {
  std::error_code fs_error;
  const uint64_t file_size = std::filesystem::file_size(path, fs_error);
  if (fs_error || file_size < kTocOffset) {
    error = "archive is missing or shorter than its RPF header";
    return std::nullopt;
  }

  std::ifstream stream(path, std::ios::binary);
  std::array<uint8_t, 20> header_data{};
  if (!stream.read(reinterpret_cast<char*>(header_data.data()), header_data.size())) {
    error = "could not read the RPF header";
    return std::nullopt;
  }
  if (ReadU32(header_data, 0) != kRpf2Magic) {
    error = "archive is not an RPF2 file";
    return std::nullopt;
  }

  ParsedArchive archive;
  archive.file_size = file_size;
  archive.header.toc_size = ReadU32(header_data, 4);
  archive.header.entry_count = ReadU32(header_data, 8);
  archive.header.encrypted = ReadU32(header_data, 16) != 0;

  const uint64_t entry_bytes = static_cast<uint64_t>(archive.header.entry_count) * 16;
  if (archive.header.entry_count == 0 || entry_bytes > archive.header.toc_size ||
      static_cast<uint64_t>(archive.header.toc_size) + kTocOffset > file_size) {
    error = "RPF TOC dimensions are invalid";
    return std::nullopt;
  }
  if (archive.header.encrypted && key.size() != 32) {
    error = "encrypted RPF requires a 32-byte AES key";
    return std::nullopt;
  }

  archive.toc.resize(archive.header.toc_size);
  stream.seekg(static_cast<std::streamoff>(kTocOffset), std::ios::beg);
  if (!stream.read(reinterpret_cast<char*>(archive.toc.data()), archive.toc.size())) {
    error = "could not read the RPF TOC";
    return std::nullopt;
  }
  if (archive.header.encrypted && !CryptToc(archive.toc, key, true)) {
    error = "could not decrypt the RPF TOC";
    return std::nullopt;
  }

  archive.string_table_offset = static_cast<size_t>(entry_bytes);
  archive.entries.reserve(archive.header.entry_count);
  for (uint32_t i = 0; i < archive.header.entry_count; ++i) {
    TocEntry entry;
    entry.toc_offset = static_cast<size_t>(i) * 16;
    entry.name_offset = ReadU32(archive.toc, entry.toc_offset);
    const uint32_t word1 = ReadU32(archive.toc, entry.toc_offset + 4);
    const uint32_t word2 = ReadU32(archive.toc, entry.toc_offset + 8);
    const uint32_t word3 = ReadU32(archive.toc, entry.toc_offset + 12);
    entry.is_directory = (word2 & kDirectoryBit) != 0;
    if (entry.is_directory) {
      entry.content_index = word2 & ~kDirectoryBit;
      entry.content_count = word3 & 0x0FFFFFFF;
    } else {
      entry.size = word1;
      entry.is_resource = (word3 & kResourceMask) == kResourceMask;
      if (entry.is_resource) {
        entry.resource_type = word2 & 0xFF;
        entry.offset = word2 & kResourceOffsetMask;
        entry.size_in_archive = entry.size;
        entry.resource_flags = word3;
      } else {
        entry.offset = word2;
        entry.size_in_archive = word3 & 0xBFFFFFFF;
      }
      const uint64_t allocation = AlignUp(entry.size_in_archive, kBlockSize);
      const uint64_t data_end = static_cast<uint64_t>(entry.offset) + allocation;
      if (data_end > file_size) {
        error = "RPF entry points outside the archive";
        return std::nullopt;
      }
      archive.max_data_end = std::max(archive.max_data_end, data_end);
    }
    archive.entries.push_back(entry);
  }

  if (!archive.entries.front().is_directory) {
    error = "RPF root entry is not a directory";
    return std::nullopt;
  }
  std::vector<bool> active(archive.entries.size());
  if (!FindPromptEntry(archive, 0, "", active, archive.prompt_entry_index)) {
    error = "RPF does not contain /textures/buttons_360.xtd";
    return std::nullopt;
  }
  {
    std::vector<bool> frontend_active(archive.entries.size());
    size_t frontend = 0;
    if (FindPromptEntry(archive, 0, "", frontend_active, frontend, kFrontendPath) &&
        archive.entries[frontend].is_resource &&
        archive.entries[frontend].resource_type == kTextureDictionaryType) {
      archive.frontend_entry_index = frontend;
    }
  }
  const auto& prompt = archive.entries[archive.prompt_entry_index];
  if (!prompt.is_resource || prompt.resource_type != kTextureDictionaryType) {
    error = "buttons_360.xtd is not the expected texture-dictionary resource";
    return std::nullopt;
  }
  return archive;
}

std::optional<std::vector<uint8_t>> ReadEntryData(const std::filesystem::path& path,
                                                  const TocEntry& entry) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return std::nullopt;
  }
  std::vector<uint8_t> data(entry.size_in_archive);
  stream.seekg(entry.offset, std::ios::beg);
  if (!data.empty() && !stream.read(reinterpret_cast<char*>(data.data()), data.size())) {
    return std::nullopt;
  }
  return data;
}

bool WriteZeros(std::fstream& stream, uint64_t offset, uint64_t size) {
  std::array<char, 4096> zeros{};
  stream.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
  while (size != 0) {
    const size_t chunk = static_cast<size_t>(std::min<uint64_t>(size, zeros.size()));
    if (!stream.write(zeros.data(), chunk)) {
      return false;
    }
    size -= chunk;
  }
  return true;
}

bool IsTextureDictionary(std::span<const uint8_t> data) {
  return data.size() >= 12 && ReadU32(data, 0) == kResourceMagic &&
         ReadU32(data, 4) == kTextureDictionaryType && data.size() <= std::numeric_limits<uint32_t>::max();
}

// A copy of `source` with its buttons_360.xtd - and, when `frontend` isn't
// empty, its frontend_360.xtd - replaced: each written over its old
// allocation when it fits there, else after the archive's last data.
bool BuildShadowArchive(const std::filesystem::path& source,
                        const std::filesystem::path& destination, std::span<const uint8_t> key,
                        std::span<const uint8_t> replacement, std::span<const uint8_t> frontend,
                        std::string& error) {
  auto archive = ParseArchive(source, key, error);
  if (!archive) {
    return false;
  }
  if (!IsTextureDictionary(replacement) || (!frontend.empty() && !IsTextureDictionary(frontend))) {
    error = "replacement is not a valid GTA IV texture dictionary";
    return false;
  }
  if (!frontend.empty() && archive->frontend_entry_index == kNoEntry) {
    error = "RPF does not contain /textures/frontend_360.xtd";
    return false;
  }

  std::error_code fs_error;
  std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing,
                             fs_error);
  if (fs_error) {
    error = "could not copy the source RPF: " + fs_error.message();
    return false;
  }

  std::fstream stream(destination, std::ios::binary | std::ios::in | std::ios::out);
  auto toc = archive->toc;
  uint64_t append_at = AlignUp(std::max(archive->file_size, archive->max_data_end), kBlockSize);
  auto place = [&](const TocEntry& old_entry, std::span<const uint8_t> data) {
    const uint64_t old_allocation = AlignUp(old_entry.size_in_archive, kBlockSize);
    const uint64_t new_allocation = AlignUp(data.size(), kBlockSize);
    uint64_t new_offset = old_entry.offset;
    if (new_allocation > old_allocation) {
      new_offset = append_at;
      append_at += new_allocation;
    }
    if (new_offset > kResourceOffsetMask) {
      error = "replacement resource offset exceeds the RPF2 field width";
      return false;
    }
    if (!stream || !WriteZeros(stream, old_entry.offset, old_allocation)) {
      error = "could not clear the old resource allocation";
      return false;
    }
    stream.seekp(static_cast<std::streamoff>(new_offset), std::ios::beg);
    if (!stream.write(reinterpret_cast<const char*>(data.data()), data.size()) ||
        !WriteZeros(stream, new_offset + data.size(), new_allocation - data.size())) {
      error = "could not write the replacement resource";
      return false;
    }
    const size_t entry_offset = old_entry.toc_offset;
    WriteU32(toc, entry_offset + 4, static_cast<uint32_t>(data.size()));
    WriteU32(toc, entry_offset + 8, static_cast<uint32_t>(new_offset) | kTextureDictionaryType);
    WriteU32(toc, entry_offset + 12, ReadU32(data, 8));
    return true;
  };
  if (!place(archive->entries[archive->prompt_entry_index], replacement) ||
      (!frontend.empty() && !place(archive->entries[archive->frontend_entry_index], frontend))) {
    return false;
  }
  if (archive->header.encrypted && !CryptToc(toc, key, false)) {
    error = "could not encrypt the updated RPF TOC";
    return false;
  }
  stream.seekp(static_cast<std::streamoff>(kTocOffset), std::ios::beg);
  if (!stream.write(reinterpret_cast<const char*>(toc.data()), toc.size())) {
    error = "could not write the updated RPF TOC";
    return false;
  }
  stream.flush();
  if (!stream) {
    error = "failed while flushing the shadow RPF";
    return false;
  }
  return true;
}

std::string Lowercase(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return result;
}

std::optional<std::filesystem::path> FindAsset(std::string_view variant,
                                               std::string_view file = "buttons_360.xtd");

std::string SelectVariant() {
  // (the hub's Button Prompts setting: a set chosen by hand, when it exists)
  const std::string chosen = REXCVAR_GET(gta4_button_prompts);
  if (chosen != "auto" && FindAsset(chosen)) {
    REXLOG_INFO("Button prompts: chosen -> {}", chosen);
    return chosen;
  }
#if REX_PLATFORM_PS5
  // [ps5 port] Always the DualSense's (SDL has no PS5 pads to look at).
  REXLOG_INFO("Button prompts: PS5 -> ps5");
  return "ps5";
#endif
  SDL_UpdateGamepads();
  int count = 0;
  SDL_JoystickID* gamepads = SDL_GetGamepads(&count);
  if (!gamepads || count == 0) {
    SDL_free(gamepads);
#if defined(__EMSCRIPTEN__)
    // [web port] Keyboard and mouse: keycaps (build_keyboard_prompts.py),
    // with the help text's actions shown as their PC keys
    // (gta4_keyboard_prompts.cpp).
    if (FindAsset("keyboard")) {
      REXLOG_INFO("Button prompts: no gamepad; using keyboard");
      return "keyboard";
    }
#endif
    REXLOG_INFO("Button prompts: no SDL gamepad detected; using xbox360");
    return "xbox360";
  }

  const SDL_JoystickID id = gamepads[0];
  const char* raw_name = SDL_GetGamepadNameForID(id);
  const std::string name = raw_name ? raw_name : "Unknown";
  const std::string lower_name = Lowercase(name);
  const SDL_GamepadType type = SDL_GetGamepadTypeForID(id);
  SDL_free(gamepads);

  std::string variant = "xbox360";
  if (lower_name.contains("touch controls") && FindAsset("touch")) {
    // [web port] The page's on-screen controller (touchinput.js): the Xbox
    // 360's buttons, and a Controls picture of the touch layout.
    variant = "touch";
  } else if (lower_name.contains("steam deck")) {
    variant = "steam_deck";
  } else if (lower_name.contains("steam controller")) {
    variant = "steam_controller";
  } else if (lower_name.contains("series")) {
    variant = "xbox_series_x";
  } else {
    switch (type) {
      case SDL_GAMEPAD_TYPE_XBOXONE:
        variant = "xbox_one";
        break;
      case SDL_GAMEPAD_TYPE_PS3:
        variant = "ps3";
        break;
      case SDL_GAMEPAD_TYPE_PS4:
        variant = "ps4";
        break;
      case SDL_GAMEPAD_TYPE_PS5:
        variant = "ps5";
        break;
      case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
      case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
      case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
      case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        variant = "switch";
        break;
      default:
        break;
    }
  }

  REXLOG_INFO("Button prompts: SDL controller '{}' type={} -> {}", name, static_cast<int>(type),
              variant);
  return variant;
}

std::optional<std::filesystem::path> FindAsset(std::string_view variant, std::string_view file) {
  const auto executable_assets = rex::filesystem::GetResourcesFolder() / "button_prompts" / variant / file;
  std::error_code error;
  if (std::filesystem::is_regular_file(executable_assets, error)) {
    return executable_assets;
  }
  const auto source_assets =
      std::filesystem::path(GTA4_BUTTON_PROMPT_ASSET_ROOT) / variant / file;
  error.clear();
  if (std::filesystem::is_regular_file(source_assets, error)) {
    return source_assets;
  }
  return std::nullopt;
}

std::optional<std::vector<uint8_t>> LoadKey(const std::filesystem::path& game_data_root) {
  const std::array candidates = {
      game_data_root.parent_path() / "aes_key.bin", game_data_root / "aes_key.bin",
      rex::filesystem::GetResourcesFolder() / "aes_key.bin"};
  for (const auto& candidate : candidates) {
    auto key = ReadFile(candidate);
    if (key && key->size() == 32) {
      return key;
    }
  }
  return std::nullopt;
}

bool VerifyShadow(const std::filesystem::path& path, std::span<const uint8_t> key,
                  std::string_view expected_hash, std::string_view frontend_hash, std::string& error) {
  auto archive = ParseArchive(path, key, error);
  if (!archive) {
    return false;
  }
  auto data = ReadEntryData(path, archive->entries[archive->prompt_entry_index]);
  if (!data) {
    error = "could not read the prompt resource from the shadow RPF";
    return false;
  }
  if (HashBytes(*data) != expected_hash) {
    error = "shadow RPF prompt resource hash does not match the selected asset";
    return false;
  }
  if (!frontend_hash.empty()) {
    auto frontend = archive->frontend_entry_index == kNoEntry
                        ? std::nullopt
                        : ReadEntryData(path, archive->entries[archive->frontend_entry_index]);
    if (!frontend || HashBytes(*frontend) != frontend_hash) {
      error = "shadow RPF controls picture does not match the selected asset";
      return false;
    }
  }
  return true;
}

// The Controls picture put into the player's own frontend_360.xtd when the
// archive is built, so no copy of the game's menu textures has to ship: the
// resource unpacked (its XMem frames of LZX), the set's controller texture
// (button_prompts/<set>/controller.bin: tiled DXT5, from
// tools/xtd_tools/build_controller_diagrams.py) written over the stock one
// (button_prompts/controller-place.txt: where, and the stock picture's SHA-256,
// so any other version of the file is left as it is), and packed again with
// LZX's stored blocks, which any LZX decoder reads (xtd_store.c's pack).
constexpr uint32_t kXMemFrame = 0x8000;
constexpr uint32_t kResourceWindow = 1u << 17;

std::optional<std::vector<uint8_t>> UnpackResource(std::span<const uint8_t> resource) {
  std::vector<uint8_t> lzx;
  size_t total = 0;
  for (size_t at = 20; at < resource.size();) {
    uint32_t raw = kXMemFrame, packed = 0;
    if (resource[at] == 0xFF) {
      if (at + 5 > resource.size()) break;
      raw = uint32_t(resource[at + 1]) << 8 | resource[at + 2];
      packed = uint32_t(resource[at + 3]) << 8 | resource[at + 4];
      at += 5;
    } else {
      if (at + 2 > resource.size()) break;
      packed = uint32_t(resource[at]) << 8 | resource[at + 1];
      at += 2;
    }
    if (packed == 0 || at + packed > resource.size()) break;
    lzx.insert(lzx.end(), resource.begin() + at, resource.begin() + at + packed);
    total += raw;
    at += packed;
  }
  std::vector<uint8_t> data(total);
  if (total == 0 ||
      lzx_decompress(lzx.data(), lzx.size(), data.data(), data.size(), kResourceWindow, nullptr, 0) != 0) {
    return std::nullopt;
  }
  return data;
}

std::vector<uint8_t> PackResource(std::span<const uint8_t> header, std::span<const uint8_t> data) {
  std::vector<uint8_t> out(header.begin(), header.begin() + 16);
  out.resize(20);
  // (one stored block per frame: the first frame's E8 bit off, type 3, the
  // 24-bit length, padding to a 16-bit word, R0-R2, the bytes, and a pad byte
  // after an odd length)
  for (size_t offset = 0; offset < data.size(); offset += kXMemFrame) {
    const uint32_t raw = uint32_t(std::min<size_t>(data.size() - offset, kXMemFrame));
    std::vector<uint8_t> frame;
    uint32_t word = 0;
    int used = 0;
    auto bits = [&](uint32_t value, int count) {
      while (count--) {
        word = word << 1 | ((value >> count) & 1);
        if (++used == 16) {
          frame.push_back(uint8_t(word));
          frame.push_back(uint8_t(word >> 8));
          word = 0;
          used = 0;
        }
      }
    };
    if (offset == 0) bits(0, 1);
    bits(3, 3);
    bits(raw, 24);
    if (used) bits(0, 16 - used);
    for (int r = 0; r < 3; ++r) frame.insert(frame.end(), {1, 0, 0, 0});
    frame.insert(frame.end(), data.begin() + offset, data.begin() + offset + raw);
    if (raw & 1) frame.push_back(0);
    const uint32_t packed = uint32_t(frame.size());
    if (raw == kXMemFrame) {
      out.insert(out.end(), {uint8_t(packed >> 8), uint8_t(packed)});
    } else {
      out.insert(out.end(), {0xFF, uint8_t(raw >> 8), uint8_t(raw), uint8_t(packed >> 8), uint8_t(packed)});
    }
    out.insert(out.end(), frame.begin(), frame.end());
  }
  const uint32_t body = uint32_t(out.size() - 20);
  out[16] = uint8_t(body >> 24), out[17] = uint8_t(body >> 16), out[18] = uint8_t(body >> 8), out[19] = uint8_t(body);
  return out;
}

std::optional<std::vector<uint8_t>> BuildFrontend(const std::filesystem::path& source, std::span<const uint8_t> key,
                                                  std::string_view variant) {
  const auto picture_path = FindAsset(variant, "controller.bin");
  const auto place_path = FindAsset(".", "controller-place.txt");
  auto picture = picture_path ? ReadFile(*picture_path) : std::nullopt;
  auto place = place_path ? ReadFile(*place_path) : std::nullopt;
  if (!picture || picture->empty() || !place || place->empty()) {
    return std::nullopt;
  }
  uint64_t offset = 0;
  char stock_hash[65] = {};
  const std::string place_text(place->begin(), place->end());
  if (std::sscanf(place_text.c_str(), "%llu %64s", reinterpret_cast<unsigned long long*>(&offset), stock_hash) != 2) {
    REXLOG_WARN("Button prompts: controller-place.txt is not '<offset> <sha256>'");
    return std::nullopt;
  }
  std::string error;
  auto archive = ParseArchive(source, key, error);
  if (!archive || archive->frontend_entry_index == kNoEntry) {
    REXLOG_WARN("Button prompts: no frontend_360.xtd to put the Controls picture in ({})", error);
    return std::nullopt;
  }
  auto resource = ReadEntryData(source, archive->entries[archive->frontend_entry_index]);
  auto data = resource && IsTextureDictionary(*resource) ? UnpackResource(*resource) : std::nullopt;
  if (!data || offset + picture->size() > data->size()) {
    REXLOG_WARN("Button prompts: the game's frontend_360.xtd could not be unpacked");
    return std::nullopt;
  }
  const std::span<uint8_t> stock(data->data() + offset, picture->size());
  if (HashBytes(stock) != stock_hash) {
    REXLOG_WARN("Button prompts: the game's Controls picture isn't the one expected; left as it is");
    return std::nullopt;
  }
  std::copy(picture->begin(), picture->end(), stock.begin());
  REXLOG_INFO("Button prompts: '{}' Controls picture put into the game's frontend_360.xtd", variant);
  return PackResource(*resource, *data);
}

std::optional<std::filesystem::path> PrepareArchive(const std::filesystem::path& game_data_root,
                                                    const std::filesystem::path& cache_root,
                                                    std::string_view variant) {
  const auto source = game_data_root / "xbox360.rpf";
  auto asset_path = FindAsset(variant);
  auto key = LoadKey(game_data_root);
  if (!asset_path) {
    REXLOG_WARN("Button prompts: asset for '{}' was not found", variant);
    return std::nullopt;
  }
  if (!key) {
    REXLOG_WARN("Button prompts: aes_key.bin is missing or is not 32 bytes");
    return std::nullopt;
  }

  auto replacement = ReadFile(*asset_path);
  const std::string source_hash = rex::crypto::sha256_file(source);
  const std::string replacement_hash = replacement ? HashBytes(*replacement) : "";
  // (the set's Controls picture, when it has one: a ready frontend_360.xtd, or
  // the picture alone, put into the game's own file only when the archive is
  // built - the archive's name has the picture's hash, and the file beside it
  // the built dictionary's, for VerifyShadow)
  const auto frontend_path = FindAsset(variant, "frontend_360.xtd");
  auto frontend = frontend_path ? ReadFile(*frontend_path) : std::nullopt;
  if (frontend && frontend->empty()) frontend.reset();
  std::string frontend_hash = frontend ? HashBytes(*frontend) : "";
  const auto picture_path = frontend ? std::nullopt : FindAsset(variant, "controller.bin");
  const auto picture = picture_path ? ReadFile(*picture_path) : std::nullopt;
  const std::string picture_hash = picture && !picture->empty() ? HashBytes(*picture) : "";
  if (!replacement || source_hash.empty() || replacement_hash.empty()) {
    REXLOG_WARN("Button prompts: could not read or hash the archive inputs");
    return std::nullopt;
  }

  const auto prompt_cache = cache_root / "button_prompts";
  std::error_code fs_error;
  std::filesystem::create_directories(prompt_cache, fs_error);
  if (fs_error) {
    REXLOG_WARN("Button prompts: could not create cache '{}': {}", prompt_cache.string(),
                fs_error.message());
    return std::nullopt;
  }
  const std::string frontend_tag = !frontend_hash.empty() ? "-" + frontend_hash.substr(0, 16)
                                   : !picture_hash.empty() ? "-p" + picture_hash.substr(0, 15)
                                                           : "";
  const std::string cache_name =
      "xbox360-" + source_hash.substr(0, 16) + "-" + replacement_hash.substr(0, 16) + frontend_tag + ".rpf";
  const auto destination = prompt_cache / cache_name;
  const auto built_hash_path = destination.string() + ".frontend";

  std::string error;
  if (std::filesystem::is_regular_file(destination, fs_error)) {
    std::string cached_frontend_hash = frontend_hash;
    if (!picture_hash.empty()) {
      const auto built = ReadFile(built_hash_path);
      cached_frontend_hash = built ? std::string(built->begin(), built->end()) : "";
    }
    if (VerifyShadow(destination, *key, replacement_hash, cached_frontend_hash, error)) {
      REXLOG_INFO("Button prompts: using cached '{}' archive", variant);
      return destination;
    }
  }
  if (!picture_hash.empty()) {
    frontend = BuildFrontend(source, *key, variant);
    frontend_hash = frontend ? HashBytes(*frontend) : "";
  }

  const auto temporary = destination.string() + ".tmp";
  fs_error.clear();
  std::filesystem::remove(temporary, fs_error);
  const std::span<const uint8_t> frontend_bytes =
      frontend ? std::span<const uint8_t>(*frontend) : std::span<const uint8_t>();
  if (!BuildShadowArchive(source, temporary, *key, *replacement, frontend_bytes, error) ||
      !VerifyShadow(temporary, *key, replacement_hash, frontend_hash, error)) {
    std::filesystem::remove(temporary, fs_error);
    REXLOG_WARN("Button prompts: could not build shadow archive: {}", error);
    return std::nullopt;
  }

  fs_error.clear();
  std::filesystem::remove(destination, fs_error);
  fs_error.clear();
  std::filesystem::rename(temporary, destination, fs_error);
  if (fs_error) {
    const std::string rename_error = fs_error.message();
    std::filesystem::remove(temporary, fs_error);
    REXLOG_WARN("Button prompts: could not publish shadow archive: {}", rename_error);
    return std::nullopt;
  }
  if (!picture_hash.empty()) {
    std::ofstream(built_hash_path, std::ios::binary | std::ios::trunc) << frontend_hash;
  }
  REXLOG_INFO("Button prompts: built '{}' shadow archive at '{}'", variant, destination.string());
  // (the archives built for other sets or earlier assets: a 60 MB copy of
  // xbox360.rpf each, which would pile up - in the browser, in its storage)
  for (auto it = std::filesystem::directory_iterator(prompt_cache, fs_error);
       !fs_error && it != std::filesystem::directory_iterator(); it.increment(fs_error)) {
    const std::string name = it->path().filename().string();
    if (it->path() != destination && it->path().string() != built_hash_path && name.starts_with("xbox360-") &&
        (name.ends_with(".rpf") || name.ends_with(".rpf.frontend"))) {
      std::error_code remove_error;
      std::filesystem::remove(it->path(), remove_error);
    }
  }
  return destination;
}

bool MountArchive(rex::Runtime& runtime, const std::filesystem::path& archive) {
  auto* file_system = runtime.file_system();
  auto device =
      std::make_unique<rex::filesystem::HostPathDevice>(kPromptMount, archive.parent_path(), true);
  if (!device->Initialize() || !file_system->RegisterDevice(std::move(device))) {
    REXLOG_WARN("Button prompts: could not mount shadow archive directory");
    return false;
  }

  std::string target(kPromptMount);
  target.push_back('\\');
  target.append(archive.filename().string());
  file_system->RegisterSymbolicLink("game:\\xbox360.rpf", target);
  file_system->RegisterSymbolicLink("d:\\xbox360.rpf", target);
  REXLOG_INFO("Button prompts: redirected game:/xbox360.rpf to '{}'", archive.string());
  return true;
}

// The Controls page's "XBOX GUIDE" label for a set whose controller has no
// Xbox button: the button's own name, the same in every language
// (PatchLoadedGuideLabel writes it over the loaded text).
std::optional<std::string> GuideLabel(std::string_view variant) {
  if (variant == "ps3" || variant == "ps4" || variant == "ps5") return "PS";
  if (variant == "switch") return "HOME";
  if (variant == "keyboard" || variant == "touch") return "";  // (no line on their pictures)
  return std::nullopt;
}

}  // namespace

bool g_keyboard_prompts = false;
std::string g_variant;
// (the loaded strings patched, PatchLoadedGuideLabel)
std::vector<uint8_t*> g_guide_strings;

bool PrepareAndMount(rex::Runtime& runtime, const std::filesystem::path& game_data_root,
                     const std::filesystem::path& cache_root) {
  const std::string variant = SelectVariant();
  auto archive = PrepareArchive(game_data_root, cache_root, variant);
  const bool mounted = archive && MountArchive(runtime, *archive);
  if (mounted) {
    // (text copies an earlier build wrote beside the archive, unused)
    std::error_code error;
    for (auto it = std::filesystem::directory_iterator(archive->parent_path(), error);
         !error && it != std::filesystem::directory_iterator(); it.increment(error)) {
      if (it->path().extension() == ".gxt") {
        std::error_code remove_error;
        std::filesystem::remove(it->path(), remove_error);
      }
    }
  }
  g_keyboard_prompts = mounted && variant == "keyboard";
  if (mounted) g_variant = variant;
  return mounted;
}

bool KeyboardPromptsMounted() { return g_keyboard_prompts; }

// The game reads its text from common.rpf (common:/text/<LANGUAGE>.GXT), so
// the guide label is patched where the loaded table holds it instead: each
// "XBOX GUIDE" string in guest memory, found once with the pause menu open
// and checked again on later pauses (a language change loads it anew).
void PatchLoadedGuideLabel(uint8_t* base) {
  const auto label = GuideLabel(g_variant);
  if (!label) return;
  bool intact = !g_guide_strings.empty();
  for (uint8_t* at : g_guide_strings) {
    if (std::memcmp(at, label->c_str(), label->size() + 1) != 0) intact = false;
  }
  if (intact) return;
  static constexpr char kNeedle[] = "\0XBOX GUIDE\0";
  const std::string_view needle(kNeedle, sizeof(kNeedle) - 1);
  const std::boyer_moore_horspool_searcher searcher(needle.begin(), needle.end());
  g_guide_strings.clear();
  // (the game's heaps: the physical window and the virtual heap)
  const std::pair<uint32_t, uint32_t> regions[] = {{0xA0000000u, 0x20000000u}, {0x40000000u, 0x40000000u}};
  for (const auto& [start, size] : regions) {
    const char* first = reinterpret_cast<const char*>(REX_RAW_ADDR(start));
    const char* last = first + size;
    for (const char* it = std::search(first, last, searcher); it != last && g_guide_strings.size() < 8;
         it = std::search(it + 1, last, searcher)) {
      uint8_t* text = reinterpret_cast<uint8_t*>(const_cast<char*>(it + 1));
      for (size_t k = 0; k < 10; ++k) text[k] = k < label->size() ? uint8_t((*label)[k]) : 0;
      g_guide_strings.push_back(text);
    }
  }
  REXLOG_INFO("Button prompts: guide label '{}' written over {} loaded strings", *label, g_guide_strings.size());
}

}  // namespace gta4::button_prompts
