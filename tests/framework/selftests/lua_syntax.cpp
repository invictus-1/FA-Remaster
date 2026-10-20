#include "script/lua.hpp"
#include <cstdio>

int main(int argc, char** argv) {
  int failed = 0;
  lua_State* L = lua_open();
  for (int i = 1; i < argc; ++i) {
    bool ok = luaL_loadfile(L, argv[i]) == 0;
    if (!ok) {
      ++failed;
      std::printf("%s: %s\n", argv[i], lua_tostring(L, -1));
    }
    lua_settop(L, 0);
  }
  lua_close(L);
  return failed ? 1 : 0;
}
