#include "script/script_state.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "core/hostfs.h"
#include "core/log.h"
#include "core/vfs.h"

extern "C" {
extern FILE* (*lua_gpg_fopen_hook)(const char* filename, const char* mode);
extern int (*lua_gpg_remove_hook)(const char* filename);
extern int (*lua_gpg_rename_hook)(const char* from, const char* to);
}

namespace moho {
namespace {
const char kStateKey = 0;  // address used as a registry key

// Scripts name files the Windows way; reads go through the host mapping. Scripts may not
// modify files outside the engine's own data folder (none is configured yet), so the
// original game's files (shader cache, preferences) are never touched by our engine.
FILE* ScriptFopen(const char* filename, const char* mode) {
  bool write = std::strpbrk(mode, "wa+") != nullptr;
  if (write) {
    Logf(LogLevel::Debug, "script file write blocked: %s", filename);
    return nullptr;
  }
  std::string host = hostfs::Resolve(filename);
  return host.empty() ? nullptr : std::fopen(host.c_str(), mode);
}
int ScriptRemove(const char* filename) {
  Logf(LogLevel::Debug, "script file remove blocked: %s", filename);
  return -1;
}
int ScriptRename(const char* from, const char*) {
  Logf(LogLevel::Debug, "script file rename blocked: %s", from);
  return -1;
}
}  // namespace

ScriptState::ScriptState(Kind kind, Vfs* vfs) : kind_(kind), vfs_(vfs) {
  lua_gpg_fopen_hook = ScriptFopen;
  lua_gpg_remove_hook = ScriptRemove;
  lua_gpg_rename_hook = ScriptRename;
  L_ = lua_open();
  luaopen_base(L_);
  luaopen_table(L_);
  luaopen_io(L_);
  luaopen_string(L_);
  luaopen_math(L_);
  luaopen_debug(L_);
  lua_settop(L_, 0);
  lua_pushlightuserdata(L_, const_cast<char*>(&kStateKey));
  lua_pushlightuserdata(L_, this);
  lua_rawset(L_, LUA_REGISTRYINDEX);
  RegisterCoreBindings(*this);
}

ScriptState::~ScriptState() {
  if (L_) lua_close(L_);
}

ScriptState* ScriptState::From(lua_State* L) {
  lua_pushlightuserdata(L, const_cast<char*>(&kStateKey));
  lua_rawget(L, LUA_REGISTRYINDEX);
  auto* s = static_cast<ScriptState*>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  return s;
}

int ScriptTraceback(lua_State* L) {
  // msg -> msg .. "\n" .. debug.traceback()
  const char* msg = lua_tostring(L, 1);
  lua_getglobal(L, "debug");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return 1;
  }
  lua_pushstring(L, "traceback");
  lua_rawget(L, -2);
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 2);
    return 1;
  }
  lua_pushstring(L, msg ? msg : "(error object is not a string)");
  lua_call(L, 1, 1);
  return 1;
}

void LogScriptError(const std::string& message) { LogWrite(LogLevel::Warning, message); }

int LoadScriptWithHooks(lua_State* L, const std::string& name) {
  ScriptState* st = ScriptState::From(L);
  Vfs* vfs = st->vfs();
  if (!vfs->Exists(name) || vfs->IsFolder(name)) {
    lua_pushfstring(L, "Unable to find file %s", name.c_str());
    return LUA_ERRFILE;
  }
  std::vector<std::string> parts{name};
  // Engine-wide hook folders (the init file's `hook` table, e.g. /schook).
  for (const auto& dir : st->HookDirs()) {
    std::string h = dir + name;
    if (vfs->Exists(h)) {
      Logf(LogLevel::Info, "Hooked %s with %s", name.c_str(), h.c_str());
      parts.push_back(h);
    }
  }
  // Each active mod's /hook folder, in __active_mods order.
  lua_pushstring(L, "__active_mods");
  lua_rawget(L, LUA_GLOBALSINDEX);
  if (lua_istable(L, -1)) {
    for (int i = 1;; ++i) {
      lua_rawgeti(L, -1, i);
      if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        break;
      }
      if (lua_istable(L, -1)) {
        lua_pushstring(L, "location");
        lua_gettable(L, -2);
        if (lua_isstring(L, -1)) {
          std::string h = std::string(lua_tostring(L, -1)) + "/hook" + name;
          if (vfs->Exists(h)) {
            Logf(LogLevel::Info, "Hooked %s with %s", name.c_str(), h.c_str());
            parts.push_back(h);
          }
        }
        lua_pop(L, 1);
      }
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);
  // The original concatenates the script and its hooks into one chunk, so hooks see the
  // script's file-level locals.
  std::string source;
  for (const auto& p : parts) {
    auto data = vfs->ReadFile(p);
    if (!data) {
      lua_pushfstring(L, "Unable to read file %s", p.c_str());
      return LUA_ERRFILE;
    }
    source += *data;
    // Parts are joined as they are; a newline is added only where a file lacks one (so line
    // numbers in the hooked chunk match the original's).
    if (!data->empty() && data->back() != '\n') source += '\n';
  }
  std::string chunk = vfs->ChunkName(name);
  return luaL_loadbuffer(L, source.data(), source.size(), chunk.c_str());
}

bool ScriptState::DoScript(const std::string& vpath) {
  int top = lua_gettop(L_);
  lua_pushcfunction(L_, ScriptTraceback);
  int rc = LoadScriptWithHooks(L_, vpath);
  if (rc == 0) rc = lua_pcall(L_, 0, 0, top + 1);
  bool ok = rc == 0;
  if (!ok) LogScriptError(lua_isstring(L_, -1) ? lua_tostring(L_, -1) : "(error object is not a string)");
  lua_settop(L_, top);
  return ok;
}

bool ScriptState::DoString(const std::string& source, const std::string& chunkName) {
  int top = lua_gettop(L_);
  lua_pushcfunction(L_, ScriptTraceback);
  int rc = luaL_loadbuffer(L_, source.data(), source.size(), chunkName.c_str());
  if (rc == 0) rc = lua_pcall(L_, 0, 0, top + 1);
  bool ok = rc == 0;
  if (!ok) LogScriptError(lua_isstring(L_, -1) ? lua_tostring(L_, -1) : "(error object is not a string)");
  lua_settop(L_, top);
  return ok;
}

}  // namespace moho
