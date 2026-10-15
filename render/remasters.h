// Remastered units' files made for this engine (four levels, meshes of more than 65,535 vertices,
// their LabPBR textures and lights list), as the visuals track lays them out: one folder per unit,
//   <root>/<ID>/export/moho64/<ID>_lod0.scm ... , textures/<ID>_lab_*.dds, <ID>_lights.bp
// A host mounts them before it opens the world, and ModelCache then uses them in place of the
// unit's own model (the same place a single previewed unit is put: /preview/<id>).
#pragma once
#include <string>

namespace moho {
class Vfs;
}

namespace render {

// Also mounts the remastered effects' folder there (_effects/export/moho64, effect_files.h) at
// /remastered, unless it holds a file named HOLD.
// Mounts every unit's folder found under `root` that has a first level and no file named HOLD
// (the visuals track puts one into a folder that must not be used). Returns how many.
int MountRemasters(moho::Vfs* vfs, const std::string& root);

}  // namespace render
