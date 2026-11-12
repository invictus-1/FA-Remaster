// Check that nothing the reclaim cells' cache (src/script/fastlib_reclaim.cpp, Remastered.ReclaimCellTotals) keeps
// about one Lua state reaches the next state of the same program run (review of 2026-11-09, R7).
//
// The cache recognises a cell and its entries by ADDRESS (the cell's table, each id string, each prop's table, the
// prop handle's box) and by the tables' serials and write stamps, which start again in every state. A second game
// meets the first game's entries when its state's objects land on the same addresses. The game's allocator (mimalloc)
// did not do that when tried (16 games in one moho64 run, 2026-11-10: the same table at 16 different addresses), so this test
// brings its own: Lua is compiled here with l_realloc / l_free given by this file, a bump allocator that starts again
// at the same address for each state. Two "games" then make the same objects in the same order at the same
// addresses, with one difference: each prop's ReclaimLeft (1, then 0.5; the same number of table writes).
//
// Build and run: py tests/reclaim_state_cache_test.py   (about 20 s; what it does:)
//   py -m ziglang c++ -O2 -std=c++20 -ffp-contract=off -I src -I third_party/lua
//      "-Dl_realloc(b,os,s)=test_realloc(b,os,s)" "-Dl_free(b,os)=test_free(b,os)" -include tests/reclaim_state_cache_test.h
//      tests/reclaim_state_cache_test.cpp src/script/fastlib_reclaim.cpp src/script/script_handle.cpp
//      -x c -std=c99 -w third_party/lua/*.c -o reclaim_state_cache_test.exe
// Prints "reclaim state cache test: ok" or the failed check; the exit
// code is the number of failures. With the cache as one map for the process (before 2026-11-10) it prints
// "FAILED: the second state's totals are 8 / 80 / 24, expected 8 / 40 / 12": the first state's.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/log.h"
#include "script/fastlib.h"
#include "script/script_handle.h"
#include "script/script_state.h"

// ---- the allocator Lua is built with here: one block of memory, handed out from its start again for each state ----
namespace {
constexpr size_t kArena = 8u << 20;
alignas(16) unsigned char g_arena[kArena];
size_t g_used = 0;
}  // namespace
extern "C" void* test_realloc(void* block, size_t oldSize, size_t size) {
  if (size == 0) return nullptr;
  const size_t need = (size + 15) & ~static_cast<size_t>(15);
  if (g_used + need > kArena) {
    std::fprintf(stderr, "reclaim state cache test: the arena is full\n");
    std::abort();
  }
  void* p = g_arena + g_used;
  g_used += need;
  if (block) std::memcpy(p, block, oldSize < size ? oldSize : size);
  return p;
}
extern "C" void test_free(void*, size_t) {}  // (nothing is given back: the next state starts the block again)

// ---- what the two engine files need from the rest of the engine, as small as the test can have it ----
namespace moho {
ScriptState::ScriptState(Kind kind, Vfs* vfs) : kind_(kind), vfs_(vfs) {
  L_ = lua_open();
  lua_setextra(L_, 0, this);
}
ScriptState::~ScriptState() {
  if (L_) lua_close(L_);
}
ScriptState* ScriptState::From(lua_State* L) { return static_cast<ScriptState*>(lua_getextra(L, 0)); }
void Logf(LogLevel, const char*, ...) {}  // (core/log.h: the verify switch's line; the test has no log)
// IsDestroyed's test (script/core_bindings.cpp IsDestroyedValue), without the collector's check
bool IsDestroyedValue(lua_State* L, int idx, bool) {
  bool destroyed = true;
  if (lua_istable(L, idx)) lua_rawgetcobject(L, idx);
  else lua_pushvalue(L, idx);
  if (lua_type(L, -1) == LUA_TUSERDATA && lua_getmetatable(L, -1)) {
    if (IsObjectHandleMeta(L, -1)) {
      void** box = static_cast<void**>(lua_touserdata(L, -2));
      destroyed = !box || !*box;
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  return destroyed;
}
}  // namespace moho

namespace {
using namespace moho;

struct Totals {
  double count = -1, mass = -1, energy = -1;
  const void* cell = nullptr;
};

void SetNumber(lua_State* L, int table, const char* key, double v) {
  lua_pushstring(L, key);
  lua_pushnumber(L, static_cast<lua_Number>(v));
  lua_rawset(L, table);
}

// One "game": a new state at the start of the arena, a cell of 8 live props whose ReclaimLeft is `left`, and the
// cell's totals from the engine's helper.
Totals Game(double left) {
  g_used = 0;
  Totals t;
  ScriptObject props[8];
  {
    ScriptState state(ScriptState::Kind::Sim, nullptr);
    lua_State* L = state.L();
    RegisterObjectHandle(L);
    lua_newtable(L);  // 1: Remastered
    RegisterRemasteredReclaim(L);
    lua_newtable(L);  // 2: the cell
    t.cell = lua_topointer(L, 2);
    for (int i = 0; i < 8; ++i) {
      lua_newtable(L);  // 3: the prop
      BindObject(L, 3, &props[i]);
      SetNumber(L, 3, "ReclaimLeft", left);
      SetNumber(L, 3, "MaxMassReclaim", 10);
      SetNumber(L, 3, "MaxEnergyReclaim", 3);
      char id[16];
      std::snprintf(id, sizeof id, "%d", 1048576 + i);
      lua_pushstring(L, id);
      lua_pushvalue(L, 3);
      lua_rawset(L, 2);
      lua_settop(L, 2);
    }
    lua_pushstring(L, "ReclaimCellTotals");
    lua_rawget(L, 1);
    lua_pushvalue(L, 2);
    if (lua_pcall(L, 1, 3, 0) == 0 && lua_isnumber(L, -3)) {
      t.count = lua_tonumber(L, -3);
      t.mass = lua_tonumber(L, -2);
      t.energy = lua_tonumber(L, -1);
    }
    for (ScriptObject& p : props) p.UnbindLua();
  }
  return t;
}

}  // namespace

int main() {
  int fails = 0;
  const Totals a = Game(1.0);
  const Totals b = Game(0.5);
  if (!(a.count == 8 && a.mass == 80 && a.energy == 24)) {
    std::printf("reclaim state cache test FAILED: the first state's totals are %g / %g / %g, expected 8 / 80 / 24\n",
                a.count, a.mass, a.energy);
    ++fails;
  }
  if (a.cell != b.cell) {  // (the test's own premise: without it the second state cannot meet the first's entries)
    std::printf("reclaim state cache test FAILED: the two states' cells are not at one address (%p, %p)\n", a.cell,
                b.cell);
    ++fails;
  }
  if (!(b.count == 8 && b.mass == 40 && b.energy == 12)) {
    std::printf("reclaim state cache test FAILED: the second state's totals are %g / %g / %g, expected 8 / 40 / 12\n",
                b.count, b.mass, b.energy);
    ++fails;
  }
  if (!fails) std::printf("reclaim state cache test: ok (both states' cell at %p)\n", a.cell);
  return fails;
}
