#include "core/vfs.h"

#include <algorithm>
#include <filesystem>

#include "core/hostfs.h"
#include "core/log.h"

namespace fs = std::filesystem;

namespace moho {

std::string Vfs::Normalize(std::string_view path) {
  std::string clean = hostfs::CleanPath(path);
  clean = hostfs::ToLower(clean);
  if (clean.empty() || clean[0] != '/') clean.insert(clean.begin(), '/');
  return clean;
}

void Vfs::AddFile(const std::string& vpath, FileRef ref) {
  if (files_.count(vpath)) return;  // an earlier search path already provides it
  files_.emplace(vpath, std::move(ref));
  // Register parent directories.
  size_t p = vpath.rfind('/');
  while (p != std::string::npos && p > 0) {
    std::string dir = vpath.substr(0, p);
    if (!dirs_.insert(dir).second) break;
    p = dir.rfind('/');
  }
}

bool Vfs::Mount(const std::string& hostPath, const std::string& mountPoint) {
  std::string resolved = hostfs::Resolve(hostPath);
  std::string mp = Normalize(mountPoint);
  if (mp.back() != '/') mp += '/';
  std::string display = hostfs::DisplayPath(hostPath);
  if (resolved.empty()) return false;
  Logf(LogLevel::Info, "DISK: AddSearchPath: '%s', mounted as '%s'", display.c_str(), mp.c_str());
  MountPoint m;
  m.display = display;
  m.hostPath = resolved;
  m.mountPoint = mp;
  uint32_t idx = static_cast<uint32_t>(mounts_.size());
  std::error_code ec;
  if (fs::is_directory(fs::u8path(resolved), ec)) {
    mounts_.push_back(std::move(m));
    std::vector<std::pair<std::string, FileRef>> found;
    for (auto it = fs::recursive_directory_iterator(fs::u8path(resolved), fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) break;
      if (!it->is_regular_file(ec)) continue;
      auto relu = fs::relative(it->path(), fs::u8path(resolved), ec).generic_u8string();
      std::string rel(relu.begin(), relu.end());
      FileRef ref;
      ref.mount = idx;
      ref.relHost = rel;
      ref.size = it->file_size(ec);
      found.emplace_back(Normalize(mp + rel), std::move(ref));
    }
    std::sort(found.begin(), found.end(), [](auto& a, auto& b) { return a.first < b.first; });
    for (auto& f : found) AddFile(f.first, std::move(f.second));
    if (mp != "/") dirs_.insert(mp.substr(0, mp.size() - 1));
    return true;
  }
  m.zip = ZipArchive::Open(resolved);
  if (!m.zip) {
    Logf(LogLevel::Warning, "DISK: unable to open archive '%s'", display.c_str());
    return false;
  }
  mounts_.push_back(std::move(m));
  const auto& entries = mounts_.back().zip->Entries();
  for (uint32_t i = 0; i < entries.size(); ++i) {
    if (entries[i].isDirectory) continue;
    FileRef ref;
    ref.mount = idx;
    ref.entry = i;
    ref.size = entries[i].size;
    AddFile(Normalize(mp + entries[i].name), std::move(ref));
  }
  if (mp != "/") dirs_.insert(mp.substr(0, mp.size() - 1));
  return true;
}

namespace {
// The original resolves only rooted virtual paths: "mods/x/y.sca" is not found.
bool IsRooted(std::string_view p) { return !p.empty() && (p[0] == '/' || p[0] == '\\'); }
}  // namespace

bool Vfs::Exists(std::string_view vpath) const {
  if (!IsRooted(vpath)) return false;
  std::string p = Normalize(vpath);
  return files_.count(p) || dirs_.count(p);
}

bool Vfs::IsFolder(std::string_view vpath) const { return dirs_.count(Normalize(vpath)) > 0; }

std::optional<Vfs::FileInfo> Vfs::GetFileInfo(std::string_view vpath) const {
  if (!IsRooted(vpath)) return std::nullopt;
  std::string p = Normalize(vpath);
  FileInfo info;
  if (dirs_.count(p) || p == "/") {
    info.isFolder = true;
    return info;
  }
  auto it = files_.find(p);
  if (it == files_.end()) return std::nullopt;
  info.size = it->second.size;
  info.isReadOnly = mounts_[it->second.mount].zip != nullptr;
  info.writeTime = it->second.writeTime;
  return info;
}

std::optional<std::string> Vfs::ReadFile(std::string_view vpath) const {
  auto it = files_.find(Normalize(vpath));
  if (it == files_.end()) return std::nullopt;
  const MountPoint& m = mounts_[it->second.mount];
  if (m.zip) return m.zip->Read(m.zip->Entries()[it->second.entry]);
  return hostfs::ReadFile(m.hostPath + "/" + it->second.relHost);
}

std::vector<std::string> Vfs::FindFiles(std::string_view dir, std::string_view pattern) const {
  std::vector<std::string> out;
  std::string d = Normalize(dir);
  if (d.back() != '/') d += '/';
  for (auto it = files_.lower_bound(d); it != files_.end(); ++it) {
    if (it->first.compare(0, d.size(), d) != 0) break;
    size_t slash = it->first.rfind('/');
    std::string_view name(it->first.c_str() + slash + 1);
    if (hostfs::WildcardMatch(pattern, name)) out.push_back(it->first);
  }
  return out;
}

std::string Vfs::ToLocal(std::string_view path) const {
  std::string p(path);
  if (!p.empty() && p[0] == '@') p.erase(0, 1);
  std::string display = hostfs::DisplayPath(p);
  for (const auto& m : mounts_) {
    if (display.size() > m.display.size() && display.compare(0, m.display.size(), m.display) == 0 &&
        display[m.display.size()] == '\\') {
      std::string rel = display.substr(m.display.size() + 1);
      std::replace(rel.begin(), rel.end(), '\\', '/');
      return Normalize(m.mountPoint + rel);
    }
  }
  return Normalize(p);
}

std::string Vfs::ChunkName(std::string_view vpath) const {
  std::string p = Normalize(vpath);
  auto it = files_.find(p);
  if (it == files_.end()) return "@" + p;
  const MountPoint& m = mounts_[it->second.mount];
  std::string rel = p.substr(m.mountPoint.size());
  std::replace(rel.begin(), rel.end(), '/', '\\');
  return "@" + m.display + "\\" + rel;
}

}  // namespace moho
