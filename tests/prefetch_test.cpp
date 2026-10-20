#include "sim/prefetch.h"

#include <cstdio>

#include "core/vfs.h"
#include "script/script_state.h"
#include "sim/script_object.h"
#include "sim/sim.h"

int main() {
  moho::Vfs vfs;
  vfs.MountMemoryFile("/model.scm", "model bytes");
  vfs.MountMemoryFile("/animation.sca", "animation bytes");
  moho::ScriptState state(moho::ScriptState::Kind::Sim, &vfs);
  lua_State* L = state.L();
  {
    moho::Sim sim(&vfs, {}, {});
    lua_setextra(L, 1, &sim);
    moho::RegisterSimClasses(L);
    moho::RegisterPrefetchBindings(L);
    if (!state.DoString(
            "moho.CPrefetchSet.__index = moho.CPrefetchSet\n"
            "prefetch = CreatePrefetchSet()\n"
            "assert(prefetch:Update({models={'/MODEL.scm', '/model.scm'}, anims={'/animation.sca'},"
            "d3d_textures={'/missing.dds'}}) == nil)\n",
            "=prefetch"))
      return 1;
    lua_getglobal(L, "prefetch");
    auto* set = moho::CheckObject<moho::PrefetchSet>(L, -1);
    lua_pop(L, 1);
    if (set->resources.size() != 2 || set->resources.at("/model.scm") != "model bytes" ||
        set->resources.at("/animation.sca") != "animation bytes")
      return 1;
    if (!state.DoString("prefetch:Update({models={'/model.scm'}})", "=replace prefetch"))
      return 1;
    if (set->resources.size() != 1 || set->resources.count("/animation.sca"))
      return 1;
    if (!state.DoString("prefetch:Reset()", "=reset prefetch") || !set->resources.empty())
      return 1;
    vfs.SetMemoryFile("/model.scm", "replacement bytes");
    if (!state.DoString("prefetch:Update({models={'/model.scm'}})", "=reload prefetch"))
      return 1;
    if (set->resources.at("/model.scm") != "replacement bytes")
      return 1;
    if (!state.DoString("assert(not pcall(function() prefetch:Update(false) end))", "=prefetch arguments"))
      return 1;
  }
  if (!state.DoString("assert(not pcall(function() prefetch:Reset() end))", "=prefetch lifetime"))
    return 1;
  std::puts("prefetch resource and lifetime checks passed");
  return 0;
}
