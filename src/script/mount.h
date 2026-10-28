// Mounting the game data the way the original does: its init file (init.lua, init_faf.lua, ...) is Lua that
// fills a `path` table with the folders and archives to mount, and a `hook` table with the hook folders.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace moho {

class Vfs;

// Run `initFile` (a host path) in a state of its own, mount every entry of its `path` table into `vfs`
// ("dir\*.scd" mounts every match) and append its `hook` table to `hookDirs`. The init file sees
// InitFileDir and SHGetFolderPath(name); `folders` holds answers that replace the system's for a name
// (LOCAL_APPDATA, APPDATA, PERSONAL, COMMON_APPDATA, COMMON_DOCUMENTS). skip (may be null): host folders of the
// `path` table that are left out (compared without case, / and \ alike, a trailing slash ignored), as moho64ui
// leaves the original M28AI out when it mounts our variant. Errors are logged; returns success.
bool MountFromInitFile(const std::string& initFile, const std::map<std::string, std::string>& folders, Vfs& vfs,
                       std::vector<std::string>& hookDirs, const std::vector<std::string>* skip = nullptr);

}  // namespace moho
