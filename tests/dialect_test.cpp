#include <cstdio>
#include "script/lua.hpp"
int main(int argc, char** argv) {
  lua_State* L = lua_open();
  luaopen_base(L); luaopen_table(L); luaopen_string(L); luaopen_math(L);
  lua_settop(L, 0);
  if (luaL_loadfile(L, argv[1]) || lua_pcall(L, 0, 1, 0)) {
    std::printf("dialect test FAILED: %s\n", lua_tostring(L, -1));
    return 1;
  }
  std::printf("dialect test: %s\n", lua_tostring(L, -1));
  return 0;
}
