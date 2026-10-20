#include <cstdio>

#include "core/vfs.h"
#include "user/user_state.h"

int main() {
  moho::Vfs vfs;
  moho::ui::UserState state(&vfs, {});
  lua_State* L = state.L();
  for (const char* name : {"moho64ui_TestHost", "moho64ui_MouseMove", "moho64ui_MouseButton",
                           "moho64ui_MouseWheel", "moho64ui_Key", "moho64ui_Type", "moho64ui_ControlAt"}) {
    lua_getglobal(L, name);
    bool absent = lua_isnil(L, -1);
    lua_pop(L, 1);
    if (!absent) {
      std::printf("production UI state exposes %s\n", name);
      return 1;
    }
  }
  lua_getglobal(L, "PostDragger");
  bool coreBinding = lua_iscfunction(L, -1);
  lua_pop(L, 1);
  if (!coreBinding) return 1;

  moho::ui::UserState::Config config;
  config.registerBindings = [](lua_State* L) {
    lua_pushboolean(L, 1);
    lua_setglobal(L, "host_binding");
  };
  moho::ui::UserState extended(&vfs, config);
  lua_getglobal(extended.L(), "host_binding");
  if (!lua_toboolean(extended.L(), -1)) return 1;
  std::printf("production UI state is isolated; host bindings attach correctly\n");
  return 0;
}
