// Host file system access.
//
// The game's init and Lua files name host paths the Windows way ("C:/Program Files (x86)/...",
// backslashes or slashes, any letter case). This layer accepts those paths everywhere:
//  - on Windows they are used directly;
//  - on other hosts a drive mapping turns "C:/" into a host directory, and every lookup is
//    case-insensitive, so Linux test machines see the same files the game would.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace moho::hostfs {

// Map a Windows drive letter to a host directory (non-Windows hosts; ignored on Windows).
void MapDrive(char drive, const std::string& hostRoot);
// Map a Windows path prefix ("C:/Users/chris/Downloads/SupComLab") to a host directory.
// Longest matching prefix wins; checked before drive mappings (non-Windows hosts).
void MapPrefix(const std::string& windowsPrefix, const std::string& hostDir);

// The original engine's display form of a host path: lower case, backslashes, "." and ".."
// collapsed, no trailing separator. Example: "c:\users\chris\downloads\supcomlab\gamedata".
std::string DisplayPath(std::string_view path);

// Forward-slash form with "." and ".." collapsed, original letter case kept.
std::string CleanPath(std::string_view path);

// Resolve to an existing host path (case-insensitive on non-Windows). Empty if missing.
std::string Resolve(std::string_view path);

bool IsDirectory(std::string_view path);
bool IsFile(std::string_view path);

struct FileStat {
  bool isFolder = false;
  bool isReadOnly = false;
  uint64_t size = 0;
  int64_t writeTime = 0;  // seconds since the Unix epoch
};
std::optional<FileStat> Stat(std::string_view path);

// Names of the entries of a directory (not "." or ".."), sorted the way NTFS lists them.
std::vector<std::string> ListDirectory(std::string_view dir);

// Win32 FindFirstFile semantics: "dir/*", "dir/*.nx2", "dir/mod_info.lua". Returns entry names
// (with "." and ".." when the pattern matches them, as Windows does), sorted like NTFS.
std::vector<std::string> Glob(std::string_view pattern);

// Case-insensitive Windows wildcard match ('*', '?'); "*.*" matches every name.
bool WildcardMatch(std::string_view pattern, std::string_view name);

std::optional<std::string> ReadFile(std::string_view path);
bool WriteFile(std::string_view path, std::string_view data);

std::string ToLower(std::string_view s);

}  // namespace moho::hostfs
