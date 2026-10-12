// The engine's handle to a LazyVar of the game's scripts (/lua/lazyvar.lua).
//
// Layout in this interface is lazy values: a control's Left, Top, Width, ... are LazyVar objects
// that scripts set to numbers or to functions of other LazyVars. The engine makes them (it calls
// the script's own Create), stores them in the control's Lua table and reads them when it needs a
// number. Values the engine knows itself (a texture's size, a text's advance) it sets the same way.
#pragma once
#include "script/lua.hpp"

namespace moho::ui {

class LazyVar {
 public:
  LazyVar() = default;
  ~LazyVar() { Release(); }
  LazyVar(const LazyVar&) = delete;
  LazyVar& operator=(const LazyVar&) = delete;

  // import('/lua/lazyvar.lua').Create(initial). Returns false (and logs) when the script fails.
  bool Create(lua_State* L, float initial = 0.0f);
  void Release();
  bool valid() const { return ref_ != LUA_NOREF; }

  // Push the script object (nil when there is none).
  void Push(lua_State* L) const;

  // The value as a number. A cached value is read directly; otherwise the LazyVar is evaluated.
  // An evaluation error or a value that is not a number is logged and counts as 0.
  float Get() const;
  // The same read for looking only (a dump): nothing is logged or changed when it fails.
  bool TryGet(float* out) const;
  // lazyvar:SetValue(v): the script side tells everything that depends on it.
  void Set(float v);

 private:
  lua_State* L_ = nullptr;  // the main state
  int ref_ = LUA_NOREF;
};

}  // namespace moho::ui
