#include <map>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/vfs.h"
#include "script/script_state.h"

namespace moho {

// Build __active_mods the way the game's mod manager does: each selected mod's mod_info.lua
// run in an environment of defaults, `location` = its folder.
void SetActiveMods(ScriptState& st, const std::vector<std::string>& uids) {
  lua_State* L = st.L();
  Vfs* vfs = st.vfs();
  std::map<std::string, int> refByUid;  // uid -> stack index
  lua_newtable(L);                      // __active_mods
  int modsIdx = lua_gettop(L);
  std::map<std::string, std::string> fileByUid;
  for (const auto& file : vfs->FindFiles("/mods", "*mod_info.lua")) {
    std::string location = file.substr(0, file.rfind('/'));
    std::string code =
        "return function(file, location)\n"
        "local env = { location = location, name = file, description = '', author = '', copyright = '',\n"
        "  exclusive = false, selectable = true, hookdir = '/hook', shadowdir = '/shadow', uid = file }\n"
        "doscript(file, env)\n"
        "env.location = location\n"
        "return env\n"
        "end\n";
    lua_pushcfunction(L, ScriptTraceback);
    int eh = lua_gettop(L);
    if (luaL_loadbuffer(L, code.data(), code.size(), "=modinfo") != 0) {
      LogScriptError(lua_tostring(L, -1));
      lua_settop(L, modsIdx);
      continue;
    }
    lua_call(L, 0, 1);  // -> the loader function
    lua_pushstring(L, file.c_str());
    lua_pushstring(L, location.c_str());
    if (lua_pcall(L, 2, 1, eh) != 0) {
      LogScriptError(std::string("Problem loading ") + file + ":\n" + (lua_tostring(L, -1) ? lua_tostring(L, -1) : "?"));
      lua_settop(L, modsIdx);
      continue;
    }
    lua_getfield(L, -1, "uid");
    std::string uid = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    for (size_t i = 0; i < uids.size(); ++i) {
      if (uids[i] == uid) {
        lua_pushvalue(L, -1);
        lua_rawseti(L, modsIdx, static_cast<int>(i) + 1);
      }
    }
    lua_settop(L, modsIdx);
  }
  lua_setglobal(L, "__active_mods");
}

}  // namespace moho
