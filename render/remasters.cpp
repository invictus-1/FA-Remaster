#include "remasters.h"

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include "core/hostfs.h"
#include "core/vfs.h"
#include "effect_files.h"

namespace render {

namespace {

// Whether a folder's files (ListDirectory) hold one of this name, in any case (`name` in lower case).
bool Lists(const std::vector<std::string>& files, const std::string& name) {
  for (const std::string& file : files) {
    std::string low = file;
    for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (low == name) return true;
  }
  return false;
}

}  // namespace

int MountRemasters(moho::Vfs* vfs, const std::string& root, bool effectsDespiteHold, bool terrainDespiteHold) {
  int mounted = 0;
  {  // the remastered ground: its remap.json and the materials it names, below one folder
    const std::string ground = root + "/_terrain/export/moho64";
    if (moho::hostfs::IsDirectory(ground)) {
      const bool hold = Lists(moho::hostfs::ListDirectory(ground), "hold");
      if ((!hold || terrainDespiteHold) && vfs->Mount(ground, kRemasteredTerrain))
        printf("render: remastered ground from %s%s\n", ground.c_str(), hold ? " (despite its HOLD)" : "");
    }
  }
  {  // [grime] the units' shared grime layers (unit_grime.h): one folder of the sets' files
    const std::string grime = root + "/_unitgrime/export/moho64";
    if (moho::hostfs::IsDirectory(grime)) {
      const bool hold = Lists(moho::hostfs::ListDirectory(grime), "hold");
      if (!hold && vfs->Mount(grime, kRemasteredUnitGrime)) printf("render: units' grime layers from %s\n", grime.c_str());
    }
  }
  {  // the remastered effects (effect_files.h): one folder with the game's paths below it
    const std::string effects = root + "/_effects/export/moho64";
    if (moho::hostfs::IsDirectory(effects)) {
      const bool hold = !effectsDespiteHold && Lists(moho::hostfs::ListDirectory(effects), "hold");
      if (!hold && vfs->Mount(effects, kRemasteredEffects)) printf("render: remastered effects from %s\n", effects.c_str());
      // remastered projectiles' meshes: projectiles/<ID>/<ID>_lod0.scm and its LabPBR textures beside
      // it, each mounted where a remastered model is looked for (/preview/<id>, models.h); the
      // original look asks for the game's own model instead (ModelCache::Blueprint's `original`)
      const std::string shots = effects + "/projectiles";
      if (!hold && moho::hostfs::IsDirectory(shots))
        for (const std::string& name : moho::hostfs::ListDirectory(shots)) {
          const std::string folder = shots + "/" + name;
          std::string id = name;
          for (char& c : id) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
          if (Lists(moho::hostfs::ListDirectory(folder), id + "_lod0.scm")) vfs->Mount(folder, "/preview/" + id);
        }
    }
  }
  for (const std::string& name : moho::hostfs::ListDirectory(root)) {
    const std::string folder = root + "/" + name + "/export/moho64";
    if (!moho::hostfs::IsDirectory(folder)) continue;
    // (a unit's folder is named by its id, in any case; its first level tells that it is one; an empty file named HOLD
    // in it: the visuals track's word to leave it out)
    std::string id = name;
    for (char& c : id) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::vector<std::string> files = moho::hostfs::ListDirectory(folder);
    if (!Lists(files, id + "_lod0.scm") || Lists(files, "hold")) continue;
    if (vfs->Mount(folder, "/preview/" + id)) ++mounted;
  }
  return mounted;
}

}  // namespace render
