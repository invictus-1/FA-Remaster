#include <cstdio>
#include <cstdlib>
#include <string>
#include "script/lua.hpp"

// Runs the checks (dialect_test.lua) in a new state, which it leaves open; false if they fail.
static bool Run(const char* file, lua_State*& L, std::string& order) {
  L = lua_open();
  luaopen_base(L); luaopen_table(L); luaopen_string(L); luaopen_math(L);
  lua_settop(L, 0);
  if (luaL_loadfile(L, file) || lua_pcall(L, 0, 1, 0)) {
    std::printf("dialect test FAILED: %s\n", lua_tostring(L, -1));
    return false;
  }
  std::printf("dialect test: %s\n", lua_tostring(L, -1));
  lua_getglobal(L, "PAIRS_ORDER");
  order = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
  lua_settop(L, 0);
  return true;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: dialect_test dialect_test.lua\n");
    return 2;
  }
  // Twice, in two states open at once and with blocks of odd sizes taken in between, so that the second state's
  // objects are at other addresses: the order next() walks object keys in must not depend on them, as two copies
  // of a game in lockstep must agree on it (third_party/lua/ltable.c luaH_mainposition).
  lua_State *a = nullptr, *b = nullptr;
  std::string oa, ob;
  bool ok = Run(argv[1], a, oa);
  for (int i = 0; ok && i < 64; ++i) (void)std::malloc(24 + 40 * i);  // (kept to the end)
  ok = ok && Run(argv[1], b, ob);
  if (ok && oa != ob) {
    std::printf("dialect test FAILED: pairs() over object keys differs between two states:\n %s\n %s\n", oa.c_str(),
                ob.c_str());
    ok = false;
  }
  if (a) lua_close(a);
  if (b) lua_close(b);
  return ok ? 0 : 1;
}
