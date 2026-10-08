#include "core/hostfs.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <unordered_map>

namespace fs = std::filesystem;

namespace moho::hostfs {
namespace {

std::mutex g_mutex;
std::map<char, std::string> g_drives;                         // 'c' -> "/home/x/c"
std::vector<std::pair<std::string, std::string>> g_prefixes;  // lowercase clean prefix -> host dir
std::unordered_map<std::string, std::string> g_resolveCache;  // lowercase clean path -> host

bool IsDriveSpec(std::string_view p) {
  return p.size() >= 2 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':';
}

// NTFS orders names by their upper-cased UTF-16 code units; for ASCII that is toupper().
bool NtfsLess(const std::string& a, const std::string& b) {
  size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    unsigned char ca = static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(a[i])));
    unsigned char cb = static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(b[i])));
    if (ca != cb) return ca < cb;
  }
  return a.size() < b.size();
}

#ifndef _WIN32
// Host path of a clean (forward slash) path, before case resolution.
std::string HostSpelling(const std::string& clean) {
  std::string lower = ToLower(clean);
  for (const auto& [prefix, host] : g_prefixes) {  // sorted longest first
    if (lower.compare(0, prefix.size(), prefix) == 0 && (lower.size() == prefix.size() || lower[prefix.size()] == '/'))
      return host + clean.substr(prefix.size());
  }
  if (IsDriveSpec(clean)) {
    char d = static_cast<char>(std::tolower(static_cast<unsigned char>(clean[0])));
    auto it = g_drives.find(d);
    if (it == g_drives.end()) return {};
    std::string rest = clean.substr(2);
    if (rest.empty() || rest[0] != '/') rest = "/" + rest;
    return it->second + rest;
  }
  return clean;
}

std::string ResolveCaseInsensitive(const std::string& host) {
  if (host.empty()) return {};
  std::error_code ec;
  if (fs::exists(host, ec)) return host;
  // Walk component by component, matching names without regard to case.
  std::string cur = host[0] == '/' ? "/" : ".";
  size_t i = host[0] == '/' ? 1 : 0;
  while (i <= host.size()) {
    size_t j = host.find('/', i);
    if (j == std::string::npos) j = host.size();
    std::string comp = host.substr(i, j - i);
    i = j + 1;
    if (comp.empty()) continue;
    std::string next = (cur == "/" ? "/" : cur + "/") + comp;
    if (!fs::exists(next, ec)) {
      bool found = false;
      for (auto& e : fs::directory_iterator(cur, ec)) {
        std::string name = e.path().filename().string();
        if (ToLower(name) == ToLower(comp)) {
          next = (cur == "/" ? "/" : cur + "/") + name;
          found = true;
          break;
        }
      }
      if (!found) return {};
    }
    cur = next;
  }
  return cur;
}
#endif

}  // namespace

std::string ToLower(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

void MapDrive(char drive, const std::string& hostRoot) {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::string root = hostRoot;
  while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) root.pop_back();
  g_drives[static_cast<char>(std::tolower(static_cast<unsigned char>(drive)))] = root;
  g_resolveCache.clear();
}

void MapPrefix(const std::string& windowsPrefix, const std::string& hostDir) {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::string host = hostDir;
  while (host.size() > 1 && (host.back() == '/' || host.back() == '\\')) host.pop_back();
  g_prefixes.emplace_back(ToLower(CleanPath(windowsPrefix)), host);
  std::sort(g_prefixes.begin(), g_prefixes.end(), [](auto& a, auto& b) { return a.first.size() > b.first.size(); });
  g_resolveCache.clear();
}

std::string CleanPath(std::string_view path) {
  std::string p(path);
  std::replace(p.begin(), p.end(), '\\', '/');
  std::string prefix;
  size_t start = 0;
  if (IsDriveSpec(p)) {
    prefix = p.substr(0, 2);
    start = 2;
  }
  bool absolute = start < p.size() && p[start] == '/';
  std::vector<std::string> parts;
  size_t i = start;
  while (i <= p.size()) {
    size_t j = p.find('/', i);
    if (j == std::string::npos) j = p.size();
    std::string comp = p.substr(i, j - i);
    i = j + 1;
    if (comp.empty() || comp == ".") continue;
    if (comp == "..") {
      if (!parts.empty() && parts.back() != "..") parts.pop_back();
      else if (!absolute) parts.push_back("..");
      continue;
    }
    parts.push_back(comp);
  }
  std::string out = prefix;
  if (absolute) out += "/";
  for (size_t k = 0; k < parts.size(); ++k) {
    if (k) out += "/";
    out += parts[k];
  }
  return out;
}

std::string DisplayPath(std::string_view path) {
  std::string clean = ToLower(CleanPath(path));
  std::replace(clean.begin(), clean.end(), '/', '\\');
  return clean;
}

std::string Resolve(std::string_view path) {
  std::string clean = CleanPath(path);
#ifdef _WIN32
  std::error_code ec;
  return fs::exists(fs::u8path(clean), ec) ? clean : std::string();
#else
  std::string key = ToLower(clean);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_resolveCache.find(key);
    if (it != g_resolveCache.end()) return it->second;
  }
  std::string host;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    host = HostSpelling(clean);
  }
  std::string resolved = ResolveCaseInsensitive(host);
  if (!resolved.empty()) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_resolveCache[key] = resolved;
  }
  return resolved;
#endif
}

bool IsDirectory(std::string_view path) {
  std::string r = Resolve(path);
  std::error_code ec;
  return !r.empty() && fs::is_directory(fs::u8path(r), ec);
}

bool IsFile(std::string_view path) {
  std::string r = Resolve(path);
  std::error_code ec;
  return !r.empty() && fs::is_regular_file(fs::u8path(r), ec);
}

std::optional<FileStat> Stat(std::string_view path) {
  std::string r = Resolve(path);
  if (r.empty()) return std::nullopt;
  std::error_code ec;
  fs::path p = fs::u8path(r);
  FileStat st;
  st.isFolder = fs::is_directory(p, ec);
  if (!st.isFolder) st.size = fs::file_size(p, ec);
  auto perms = fs::status(p, ec).permissions();
  st.isReadOnly = (perms & fs::perms::owner_write) == fs::perms::none;
  auto ft = fs::last_write_time(p, ec);
  st.writeTime = std::chrono::duration_cast<std::chrono::seconds>(
                     ft.time_since_epoch() -
                     std::chrono::duration_cast<fs::file_time_type::duration>(
                         fs::file_time_type::clock::now().time_since_epoch() -
                         std::chrono::system_clock::now().time_since_epoch()))
                     .count();
  return st;
}

std::vector<std::string> ListDirectory(std::string_view dir) {
  std::vector<std::string> names;
  std::string r = Resolve(dir);
  if (r.empty()) return names;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(fs::u8path(r), ec)) { auto u = e.path().filename().u8string(); names.emplace_back(u.begin(), u.end()); }
  std::sort(names.begin(), names.end(), NtfsLess);
  return names;
}

bool WildcardMatch(std::string_view pattern, std::string_view name) {
  if (pattern == "*" || pattern == "*.*") return true;
  // Iterative glob with backtracking on the last '*'.
  size_t p = 0, n = 0, star = std::string_view::npos, mark = 0;
  auto eq = [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
  };
  while (n < name.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || eq(pattern[p], name[n]))) {
      ++p;
      ++n;
    } else if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      mark = n;
    } else if (star != std::string_view::npos) {
      p = star + 1;
      n = ++mark;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') ++p;
  // Win32: a trailing ".*" also matches names without an extension.
  if (p + 2 == pattern.size() && pattern[p] == '.' && pattern[p + 1] == '*') return true;
  return p == pattern.size();
}

std::vector<std::string> Glob(std::string_view pattern) {
  std::string clean(pattern);
  std::replace(clean.begin(), clean.end(), '\\', '/');
  size_t slash = clean.rfind('/');
  std::string dir = slash == std::string::npos ? std::string(".") : clean.substr(0, slash);
  std::string mask = slash == std::string::npos ? clean : clean.substr(slash + 1);
  if (dir.empty()) dir = "/";
  std::vector<std::string> out;
  if (!IsDirectory(dir)) return out;
  std::vector<std::string> names = ListDirectory(dir);
  names.insert(names.begin(), {".", ".."});
  for (auto& n : names) {
    if (WildcardMatch(mask, n)) out.push_back(n);
  }
  return out;
}

std::optional<std::string> ReadFile(std::string_view path) {
  std::string r = Resolve(path);
  if (r.empty()) return std::nullopt;
  FILE* f = std::fopen(r.c_str(), "rb");
  if (!f) return std::nullopt;
  std::string data;
  char buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) data.append(buf, n);
  std::fclose(f);
  return data;
}

bool WriteFile(std::string_view path, std::string_view data) {
  std::string clean = CleanPath(path);
#ifndef _WIN32
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    clean = HostSpelling(clean);
  }
#endif
  FILE* f = std::fopen(clean.c_str(), "wb");
  if (!f) return false;
  std::fwrite(data.data(), 1, data.size(), f);
  std::fclose(f);
  return true;
}

}  // namespace moho::hostfs
