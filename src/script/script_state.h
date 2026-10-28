// A Lua state as the engine sets it up: GPG-dialect Lua, standard libraries, and the engine
// functions every state has (the original's "core" binding set: logging, doscript, disk access,
// string/vector helpers, blueprint registration).
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "script/lua.hpp"

namespace moho {

class Vfs;

// A Vector ({x, y, z} with the vector metatable): core_bindings.cpp.
void PushVector(lua_State* L, float x, float y, float z);

// The colour parser (func_ParseColor 0x4b2b90): one of the colour table's names (case does not matter), else six or
// eight hex digits (six: alpha 0xff). False for anything else. core_bindings.cpp.
bool DecodeColor(const char* text, uint32_t* argb);

class ScriptState {
 public:
  int vectorMetaRef = -2;  // LUA_NOREF until first use (core_bindings.cpp)
  enum class Kind { Init, Rules, Sim, User };

  ScriptState(Kind kind, Vfs* vfs);
  ~ScriptState();
  ScriptState(const ScriptState&) = delete;
  ScriptState& operator=(const ScriptState&) = delete;

  lua_State* L() const { return L_; }
  Kind kind() const { return kind_; }
  Vfs* vfs() const { return vfs_; }

  // Hook directories from the init file's `hook` table (e.g. "/schook").
  void SetHookDirs(std::vector<std::string> dirs) { hookDirs_ = std::move(dirs); }
  const std::vector<std::string>& HookDirs() const { return hookDirs_; }

  // Hook files from outside the game's file system: the engine's own script fixes (Sim::fixes, docs/deviations.md),
  // keyed by the normalized virtual path of the script they hook. Each is joined to its script after the hook
  // folders' files and before the mods' hooks. `where` is the host file, for the log.
  struct ExtraHook {
    std::string where;
    std::string source;
  };
  void SetExtraHooks(std::map<std::string, ExtraHook> hooks) { extraHooks_ = std::move(hooks); }
  const std::map<std::string, ExtraHook>& ExtraHooks() const { return extraHooks_; }
  // More of them (our own AI's, src/ai/scripts.cpp): a script both hook sets touch gets both, these after the ones it
  // has, as one more hook folder after it.
  void AddExtraHooks(const std::map<std::string, ExtraHook>& hooks);

  // Scripts from outside the game's file system that only this state sees: our own AI's (data/ai, src/ai/scripts.cpp),
  // keyed by normalized virtual path. doscript / import and exists() find them where the file system has no file of
  // that path (they never replace a game file); DiskFindFiles does not list them. They live and go with the state,
  // so nothing of them stays in a file system the host shares with its other states or its next game.
  void SetOwnFiles(std::map<std::string, ExtraHook> files) { ownFiles_ = std::move(files); }
  const ExtraHook* OwnFile(std::string_view vpath) const;
  size_t OwnFileCount() const { return ownFiles_.size(); }

  // doscript semantics (with hooks), run protected; errors are logged. Returns success.
  bool DoScript(const std::string& vpath);
  // Run a chunk of source, protected; errors are logged.
  bool DoString(const std::string& source, const std::string& chunkName);

  static ScriptState* From(lua_State* L);

  // Called for every Register*Blueprint(bp) with the blueprint table at stack index 1.
  std::function<void(lua_State*, const char* kind)> onRegisterBlueprint;
  std::function<void()> onLoaderProgress;
  // Called by print(...) with the text tostring gave each argument; true = the line is kept (the log gets none of
  // it). Unset or false: print logs as LOG does. It must not touch the Lua state. (Sim: Sim::collectPrints.)
  std::function<bool(const std::vector<std::string>& args)> onPrint;

 private:
  lua_State* L_ = nullptr;
  Kind kind_;
  Vfs* vfs_;
  std::vector<std::string> hookDirs_;
  std::map<std::string, ExtraHook> extraHooks_;
  std::map<std::string, ExtraHook> ownFiles_;
};

// Push the error message plus a traceback (used as the pcall error handler).
int ScriptTraceback(lua_State* L);

// Log a script error the way the original does (WARN with the message and traceback).
void LogScriptError(const std::string& message);

// Load the concatenation of a script and its hooks as one chunk and push it.
// Returns 0 on success (function on the stack) or a Lua error code (message on the stack).
int LoadScriptWithHooks(lua_State* L, const std::string& vpath);

void RegisterCoreBindings(ScriptState& state);

// Set __active_mods from the selected mod uids (in that order), as the mod manager does.
void SetActiveMods(ScriptState& st, const std::vector<std::string>& uids);

}  // namespace moho
