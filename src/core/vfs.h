// The game's virtual file system ("DISK" in the original): an ordered list of search paths,
// each a host directory or a zip archive mounted at a virtual directory. Virtual paths are
// lower case with forward slashes ("/lua/system/blueprints.lua"). When several search paths
// hold the same virtual file, the one mounted first wins.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/zip.h"

namespace moho {

class Vfs {
 public:
  struct FileInfo {
    bool isFolder = false;
    bool isReadOnly = false;
    uint64_t size = 0;
    int64_t writeTime = 0;
  };

  // Normalize a virtual path: lower case, '/', "." and ".." collapsed, leading '/'.
  static std::string Normalize(std::string_view path);

  // Add a search path. hostPath is a directory or an archive; mountPoint is a virtual dir.
  // Returns false when the host path does not exist.
  bool Mount(const std::string& hostPath, const std::string& mountPoint);

  // Add a search path that holds one file, `vpath`, whose bytes the host keeps in memory: nothing is written to
  // disk, so programs running side by side each see their own. It takes its place in the search order as Mount
  // does: mounted before the init file's paths, it wins over a file of the same path in them, and the other files
  // of their folder stay visible. Returns false when an earlier search path already provides `vpath`.
  bool MountMemoryFile(std::string_view vpath, std::string text);
  // Replace the bytes of a file MountMemoryFile mounted; false when `vpath` is no such file. Not while another
  // thread may read that file.
  bool SetMemoryFile(std::string_view vpath, std::string text);

  bool Exists(std::string_view vpath) const;
  bool IsFolder(std::string_view vpath) const;
  std::optional<FileInfo> GetFileInfo(std::string_view vpath) const;
  std::optional<std::string> ReadFile(std::string_view vpath) const;

  // All files under `dir` (recursively) whose name matches `pattern`, sorted.
  std::vector<std::string> FindFiles(std::string_view dir, std::string_view pattern) const;

  // Host path -> virtual path ("DiskToLocal"). Virtual paths are returned normalized.
  std::string ToLocal(std::string_view sysOrLocalPath) const;

  // The chunk name the original gives a script: "@" + display host path of the file.
  std::string ChunkName(std::string_view vpath) const;

  size_t MountCount() const { return mounts_.size(); }
  size_t FileCount() const { return files_.size(); }

 private:
  struct MountPoint {
    std::string display;     // "c:\users\...\lua.nx2" (empty for a memory file)
    std::string hostPath;    // resolved host path
    std::string mountPoint;  // "/" or "/mods/x/"
    std::unique_ptr<ZipArchive> zip;
    bool memory = false;     // MountMemoryFile: the one file's bytes are `text`
    std::string text;
  };
  struct FileRef {
    uint32_t mount = 0;
    uint32_t entry = 0;   // zip entry index (archives)
    std::string relHost;  // relative host path (directories), original case
    uint64_t size = 0;
    int64_t writeTime = 0;
  };
  std::vector<MountPoint> mounts_;
  std::map<std::string, FileRef> files_;  // first mount wins
  std::set<std::string> dirs_;

  void AddFile(const std::string& vpath, FileRef ref);
};

}  // namespace moho
