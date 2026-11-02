// [web port] The hub's install folder: a real folder on the
// computer the page is served from (the test server's /host routes, see
// serve.py), whose files are mounted read-only in the browser with WasmFS's
// fetch backend, each read in chunks when it's read. Looked at again whenever
// the hub looks for what's there, so files copied in while it's open are found.
#include "web_host_files.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <emscripten/em_js.h>
#include <emscripten/wasmfs.h>
#include <unistd.h>

#include <rex/logging.h>

namespace fs = std::filesystem;

// (a synchronous request: fine on a worker, which everything here runs on.
// The URL is made whole from the page's origin: the workers run gta4.js from a
// blob: URL (index.html), against which "/host/..." isn't an address.)
EM_JS(int, rex_web_host_request, (const char* method, const char* url, char* out, int capacity), {
  try {
    const xhr = new XMLHttpRequest();
    xhr.open(UTF8ToString(Number(method)), new URL(UTF8ToString(Number(url)), self.location.origin).toString(), false);
    xhr.send();
    if (xhr.status < 200 || xhr.status >= 300) return -1;
    const text = xhr.responseText || "";
    if (capacity > 0) stringToUTF8(text, Number(out), capacity);
    return lengthBytesUTF8(text);
  } catch (e) {
    return -1;
  }
});

namespace gta4web {

namespace {

constexpr uint32_t kChunkSize = 4u << 20;

std::mutex g_mutex;
std::map<std::string, std::string> g_shown;                   // folder -> its path on the computer
std::map<std::string, std::map<std::string, uint64_t>> g_mounted;  // folder -> its files mounted (path -> size)

// (a path's characters as a URL's: the server unquotes them)
std::string Escape(const std::string& path) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : path) {
    if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
      out += char(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 15];
    }
  }
  return out;
}

}  // namespace

bool Request(const char* method, const std::string& url, std::string* body) {
  std::vector<char> buf(64 << 10);
  int n = rex_web_host_request(method, url.c_str(), buf.data(), int(buf.size()));
  if (n < 0) return false;
  if (body && size_t(n) + 1 > buf.size()) {
    buf.resize(size_t(n) + 1);
    n = rex_web_host_request(method, url.c_str(), buf.data(), int(buf.size()));
    if (n < 0) return false;
  }
  if (body) body->assign(buf.data(), size_t(n));
  return true;
}

namespace {

// "<name>\t<value>" lines
std::vector<std::pair<std::string, std::string>> Lines(const std::string& text) {
  std::vector<std::pair<std::string, std::string>> out;
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(at, end - at);
    if (const size_t tab = line.find('\t'); tab != std::string::npos)
      out.emplace_back(line.substr(0, tab), line.substr(tab + 1));
    at = end + 1;
  }
  return out;
}

}  // namespace

std::string HostFolderShown(const std::string& folder) {
  std::lock_guard lock(g_mutex);
  if (g_shown.empty()) {
    std::string info;
    if (Request("GET", "/host/info", &info))
      for (auto& [name, path] : Lines(info)) g_shown[name] = path;
  }
  auto it = g_shown.find(folder);
  return it != g_shown.end() ? it->second : std::string();
}

bool SyncHostFolder(const std::string& folder, const fs::path& at) {
  std::string text;
  if (!Request("GET", "/host/list/" + folder, &text)) return false;
  std::map<std::string, uint64_t> listed;
  for (auto& [size, path] : Lines(text)) listed[path] = std::strtoull(size.c_str(), nullptr, 10);

  std::lock_guard lock(g_mutex);
  auto& mounted = g_mounted[folder];
  std::error_code ec;
  fs::create_directories(at, ec);
  // gone, or changed: taken away (a changed one is mounted again below)
  for (auto it = mounted.begin(); it != mounted.end();) {
    auto found = listed.find(it->first);
    if (found != listed.end() && found->second == it->second) {
      ++it;
      continue;
    }
    ::unlink((at / it->first).c_str());
    it = mounted.erase(it);
  }
  for (const auto& [path, size] : listed) {
    if (mounted.count(path)) continue;
    const fs::path file = at / path;
    if (fs::exists(file, ec)) continue;  // (one of the app's own, as the disc image)
    fs::create_directories(file.parent_path(), ec);
    const std::string url = "/host/file/" + folder + "/" + Escape(path);
    if (wasmfs_create_file(file.c_str(), 0444, wasmfs_create_fetch_backend(url.c_str(), kChunkSize)) < 0) {
      REXLOG_WARN("web: couldn't put {} from the {} folder in the browser", path, folder);
      continue;
    }
    mounted[path] = size;
    REXLOG_INFO("web: {} folder: {} ({} MB)", folder, path, size >> 20);
  }
  return true;
}

}  // namespace gta4web

extern "C" const char* rex_web_host_folder_shown(const char* folder) {
  // (kept for the hub's text: one string per folder, made once)
  static std::mutex m;
  static std::map<std::string, std::string> kept;
  std::lock_guard lock(m);
  auto [it, made] = kept.try_emplace(folder);
  if (made) it->second = gta4web::HostFolderShown(folder);
  return it->second.c_str();
}

extern "C" bool rex_web_sync_host_folder(const char* folder, const char* at) {
  return gta4web::SyncHostFolder(folder, at);
}


extern "C" uint64_t rex_web_host_free_space() {
  std::string text;
  return gta4web::Request("GET", "/host/free", &text) ? std::strtoull(text.c_str(), nullptr, 10) : 0;
}
