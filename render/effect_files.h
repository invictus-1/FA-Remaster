// The visuals track's remastered effects: emitter, beam and trail blueprints and their textures, laid
// out below one folder with the same paths as the game's (visuals/work/_effects/export/moho64), and
// mounted at /remastered by MountRemasters. In the remastered look a file is read from there when the
// visuals track has made one, and from the game otherwise; the original look always reads the game's.
#pragma once

#include <string>

#include "core/vfs.h"

namespace render {

inline constexpr const char* kRemasteredEffects = "/remastered";

// The path to read for `path` (as the game names it): the remastered file of that path when
// `remastered` and one exists, `path` itself otherwise.
inline std::string EffectFile(const moho::Vfs& vfs, const std::string& path, bool remastered) {
  if (!remastered || path.empty()) return path;
  std::string ours = std::string(kRemasteredEffects) + moho::Vfs::Normalize(path);
  return vfs.Exists(ours) ? ours : path;
}

}  // namespace render
