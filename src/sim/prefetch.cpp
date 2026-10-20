#include "sim/prefetch.h"

#include "core/vfs.h"
#include "sim/script_object.h"
#include "sim/sim.h"

namespace moho {
namespace {

int Create(lua_State* L) {
  auto* set = Sim::From(L)->Own(std::make_unique<PrefetchSet>());
  CreateObject(L, set, "CPrefetchSet");
  return 1;
}

int Update(lua_State* L) {
  auto* set = CheckObject<PrefetchSet>(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);
  set->Update(L, 2, *Sim::From(L)->vfs());
  return 0;
}

int Reset(lua_State* L) {
  CheckObject<PrefetchSet>(L, 1)->resources.clear();
  return 0;
}

}  // namespace

void PrefetchSet::Update(lua_State* L, int data, const Vfs& vfs) {
  if (data < 0)
    data = lua_gettop(L) + data + 1;
  std::map<std::string, std::string> next;
  for (const char* field : {"models", "anims", "d3d_textures", "batch_textures"}) {
    lua_getfield(L, data, field);
    if (lua_istable(L, -1)) {
      lua_pushnil(L);
      while (lua_next(L, -2)) {
        if (lua_isstring(L, -1)) {
          const std::string path = Vfs::Normalize(lua_tostring(L, -1));
          if (!next.count(path)) {
            auto old = resources.find(path);
            if (old != resources.end())
              next.emplace(path, std::move(old->second));
            else if (auto bytes = vfs.ReadFile(path))
              next.emplace(path, std::move(*bytes));
          }
        }
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);
  }
  resources = std::move(next);
}

void RegisterPrefetchBindings(lua_State* L) {
  SetGlobal(L, "CreatePrefetchSet", Create);
  SetMethod(L, "CPrefetchSet", "Update", Update);
  SetMethod(L, "CPrefetchSet", "Reset", Reset);
}

}  // namespace moho
