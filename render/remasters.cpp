#include "remasters.h"

#include <cctype>
#include <cstdio>

#include "core/hostfs.h"
#include "core/vfs.h"
#include "effect_files.h"

namespace render {

int MountRemasters(moho::Vfs* vfs, const std::string& root) {
  int mounted = 0;
  {  // the remastered effects (effect_files.h): one folder with the game's paths below it
    const std::string effects = root + "/_effects/export/moho64";
    bool hold = false;
    if (moho::hostfs::IsDirectory(effects)) {
      for (const std::string& file : moho::hostfs::ListDirectory(effects)) {
        std::string low = file;
        for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (low == "hold") hold = true;
      }
      if (!hold && vfs->Mount(effects, kRemasteredEffects)) printf("render: remastered effects from %s\n", effects.c_str());
    }
  }
  for (const std::string& name : moho::hostfs::ListDirectory(root)) {
    const std::string folder = root + "/" + name + "/export/moho64";
    if (!moho::hostfs::IsDirectory(folder)) continue;
    // (a unit's folder is named by its id, in any case; its first level tells that it is one)
    bool first = false, hold = false;  // (an empty file named HOLD in it: the visuals track's word to leave it out)
    for (const std::string& file : moho::hostfs::ListDirectory(folder)) {
      std::string low = file, want = name + "_lod0.scm";
      for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      for (char& c : want) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (low == want) first = true;
      if (low == "hold") hold = true;
    }
    if (!first || hold) continue;
    std::string id = name;
    for (char& c : id) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (vfs->Mount(folder, "/preview/" + id)) ++mounted;
  }
  return mounted;
}

}  // namespace render
