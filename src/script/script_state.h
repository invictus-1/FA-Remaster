// A Lua state as the engine sets it up: GPG-dialect Lua, standard libraries, and the engine
// functions every state has (the original's "core" binding set: logging, doscript, disk access,
// string/vector helpers, blueprint registration).
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "script/lua.hpp"

namespace moho {

class Vfs;

class ScriptState {
 public:
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

  // doscript semantics (with hooks), run protected; errors are logged. Returns success.
  bool DoScript(const std::string& vpath);
  // Run a chunk of source, protected; errors are logged.
  bool DoString(const std::string& source, const std::string& chunkName);

  static ScriptState* From(lua_State* L);

  // Called for every Register*Blueprint(bp) with the blueprint table at stack index 1.
  std::function<void(lua_State*, const char* kind)> onRegisterBlueprint;
  std::function<void()> onLoaderProgress;

 private:
  lua_State* L_ = nullptr;
  Kind kind_;
  Vfs* vfs_;
  std::vector<std::string> hookDirs_;
};

// Push the error message plus a traceback (used as the pcall error handler).
int ScriptTraceback(lua_State* L);

// Log a script error the way the original does (WARN with the message and traceback).
void LogScriptError(const std::string& message);

// Load the concatenation of a script and its hooks as one chunk and push it.
// Returns 0 on success (function on the stack) or a Lua error code (message on the stack).
int LoadScriptWithHooks(lua_State* L, const std::string& vpath);

void RegisterCoreBindings(ScriptState& state);

}  // namespace moho
