// Read-only zip archives (.scd, .nx2, .zip): the game's packed data files.
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace moho {

class ZipArchive {
 public:
  struct Entry {
    std::string name;  // path inside the archive, forward slashes, original case
    uint32_t method = 0;
    uint64_t compressedSize = 0;
    uint64_t size = 0;
    uint64_t localHeaderOffset = 0;
    uint32_t dosTime = 0;
    bool isDirectory = false;
  };

  static std::unique_ptr<ZipArchive> Open(const std::string& hostPath);
  ~ZipArchive();

  const std::vector<Entry>& Entries() const { return entries_; }
  std::optional<std::string> Read(const Entry& e) const;
  const std::string& HostPath() const { return hostPath_; }

 private:
  ZipArchive() = default;
  std::string hostPath_;
  std::vector<Entry> entries_;
};

}  // namespace moho
