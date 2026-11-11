// The reclaim grid's helpers in `Remastered` (script/fastlib.h): one cell's update of FAF's AI reclaim grid
// (/lua/AI/GridReclaim.lua, AIGridReclaim.UpdateCell) as the engine's script fix data/fixes/hook/lua/AI/GridReclaim.lua
// (docs/deviations.md 10) does it, in C; and the grid's sort of its cells by TotalMass (FAF's SortLambda, GridReclaim.lua
// 59-61) with table.sort's own routine and the comparison in C.
//
// What it follows: FAF's UpdateCell (GridReclaim.lua 134-155), whose sum loop is by Willem 'Jip' Wijnia under the MIT
// licence; its notice, as the file carries it:
//
//   Copyright (c) 2022  Willem 'Jip' Wijnia
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy
//   of this software and associated documentation files (the "Software"), to deal
//   in the Software without restriction, including without limitation the rights
//   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//   copies of the Software, and to permit persons to whom the Software is
//   furnished to do so, subject to the following conditions:
//
//   The above copyright notice and this permission notice shall be included in all
//   copies or substantial portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
//   SOFTWARE.
//
// and the fix's own re-sum in EntityId order without destroyed props (FA Remastered, MIT).
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include "script/fastlib.h"
#include "script/script_handle.h"
#include "script/script_state.h"

namespace moho {
namespace {

// A value read as `v or 0` and then used in float arithmetic: nil / false give 0; a number itself. Anything else (true,
// a string the VM would convert, a table with __mul, ...) is not handled here: false, and the caller leaves the cell
// to the Lua code, which does what the VM does with it (its result or its error).
bool OrZero(lua_State* L, int idx, lua_Number* out) {
  const int t = lua_type(L, idx);
  if (t == LUA_TNIL || (t == LUA_TBOOLEAN && !lua_toboolean(L, idx))) {
    *out = 0;
    return true;
  }
  if (t != LUA_TNUMBER) return false;
  *out = lua_tonumber(L, idx);
  return true;
}

// The three fields of the reclaim object at `obj`, read through lua_gettable (metamethods as the VM's `reclaim.X`) in
// the Lua's order: ReclaimLeft, MaxMassReclaim, MaxEnergyReclaim. Keys at stack slots k..k+2. Leaves the stack as it was.
bool ReadReclaim(lua_State* L, int obj, int k, lua_Number* fraction, lua_Number* mass, lua_Number* energy) {
  bool ok = true;
  for (int i = 0; i < 3 && ok; ++i) {
    lua_pushvalue(L, k + i);
    lua_gettable(L, obj);
    ok = OrZero(L, -1, i == 0 ? fraction : i == 1 ? mass : energy);
    lua_pop(L, 1);
  }
  return ok;
}

// One entry of a cell: its id's number and what the re-sum takes from it.
struct CellEntry {
  uint64_t id;  // the id's digits as a number (no leading zero, at most 12 digits: below 2^40)
  bool live;    // not destroyed (IsDestroyed's test)
  lua_Number fraction, mass, energy;  // ReclaimLeft or 0, MaxMassReclaim or 0, MaxEnergyReclaim or 0
};

// The id key at `idx` as the fix's SortPadded takes it: a string of digits without a leading zero, at most 12 long.
// Its order, the fix's (shorter first, then by text), is then the numbers' order. Anything else: false (the Lua code
// sorts it with KeyLess).
bool ParseId(lua_State* L, int idx, uint64_t* id) {
  if (lua_type(L, idx) != LUA_TSTRING) return false;
  const char* s = lua_tostring(L, idx);
  const size_t n = lua_strlen(L, idx);
  if (n == 0 || n > 12 || s[0] < '1' || s[0] > '9') return false;
  uint64_t v = 0;
  for (size_t i = 0; i < n; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + static_cast<uint64_t>(s[i] - '0');
  }
  *id = v;
  return true;
}

// Remastered.ReclaimCellTotals(reclaims): count, totalMass, totalEnergy of a reclaim cell's table `reclaims` (id ->
// reclaim object), as the fix's GridReclaim.UpdateCell leaves them with FAF's debug view off:
//   count: the entries, as FAF's loop counts them (count + 1 in floats);
//   totalMass, totalEnergy: over the ids in EntityId order, each entry not destroyed (IsDestroyed's test), with
//     fraction = ReclaimLeft or 0: totalMass + fraction * (MaxMassReclaim or 0) and totalEnergy + fraction *
//     (MaxEnergyReclaim or 0), the same float operations in the same order as the Lua (lua_Number is float;
//     -ffp-contract=off). FAF's own totals are not made: the fix replaces them.
// The table is walked once (lua_next). Each value's three fields are read once, through lua_gettable (metamethods as
// the VM's `reclaim.X`), in the Lua's order ReclaimLeft, MaxMassReclaim, MaxEnergyReclaim; FAF's loop and the re-sum
// read them twice and the re-sum reads reclaims[id] again: the same values, as nothing in between writes, and a
// prop's fields are its own or its class table's (no __index function).
// Returns nothing (the caller runs its Lua code) when `reclaims` is not a table, a value is not a table, an id is not
// one SortPadded takes, or a field is not nil / false / a number: those cases stay with the Lua code, which gives what
// it always gave (or raises its error). Makes no Lua object and calls no function that collects.
int FullCellTotals(lua_State* L) {
  if (lua_type(L, 1) != LUA_TTABLE) return 0;
  lua_settop(L, 1);
  lua_checkstack(L, 8);
  lua_pushstring(L, "ReclaimLeft");       // 2
  lua_pushstring(L, "MaxMassReclaim");    // 3
  lua_pushstring(L, "MaxEnergyReclaim");  // 4
  static thread_local std::vector<CellEntry> entries;
  entries.clear();
  lua_Number count = 0;
  lua_pushnil(L);
  while (lua_next(L, 1)) {  // key -2, value -1
    count = count + 1;
    CellEntry c;
    if (lua_type(L, -1) != LUA_TTABLE || !ParseId(L, -2, &c.id)) return 0;
    const int v = lua_gettop(L);
    if (!ReadReclaim(L, v, 2, &c.fraction, &c.mass, &c.energy)) return 0;
    c.live = !IsDestroyedValue(L, v, false);
    entries.push_back(c);
    lua_pop(L, 1);
  }
  std::sort(entries.begin(), entries.end(), [](const CellEntry& a, const CellEntry& b) { return a.id < b.id; });
  lua_Number totalMass = 0, totalEnergy = 0;
  for (const CellEntry& c : entries) {
    if (!c.live) continue;
    totalMass = totalMass + c.fraction * c.mass;
    totalEnergy = totalEnergy + c.fraction * c.energy;
  }
  lua_settop(L, 0);
  lua_pushnumber(L, count);
  lua_pushnumber(L, totalMass);
  lua_pushnumber(L, totalEnergy);
  return 3;
}

// ---- The cells' cache (e1d 2026-11-07) ----
// A cell is updated (GridReclaim.UpdateCell) when one of its props was added, damaged, reclaimed or destroyed, and the
// full re-sum above then reads every entry again: three fields and the handle of each prop, ~430 ns an entry (B1:
// 15,461 updates of 527 entries on average, 99.3% of them the same as at the cell's last update; FullCellTotals' walk
// 3.5 s of 3.7 s). Kept per cell (the `reclaims` table, by its address and serial): each entry's id, prop, the prop's
// write stamp (lobject.h Table::wstamp: every store into a table bumps it) and its handle's box, and the three values
// read. At the next update an entry whose key is the same string (its text compared), whose prop is the same table
// (address and serial) with the same stamp, and whose three fields were the prop's own (found by a raw read: then
// lua_gettable gives that value and calls no metamethod) is taken from the cache; anything else is read as the full
// re-sum reads it, in lua_next's order. Liveness is read again for every entry (its box: IsDestroyed's test). The
// totals are then summed as before (EntityId order, live entries), so they are the full re-sum's bit for bit.
// MOHO64_VERIFY_RECLAIM=1: the full re-sum runs too at every call, and any difference stops the program.
struct CachedEntry {
  const void* key;   // the id string (its text kept in keyText to compare)
  const void* prop;  // the value's table
  unsigned propSerial = 0, propStamp = 0;
  void** box = nullptr;  // its _c_object's payload (nullptr: no handle: destroyed for IsDestroyed)
  bool cacheable = false;  // the three fields were the prop's own (raw) values
  uint8_t keyLen = 0;
  char keyText[12];
  uint64_t id = 0;
  lua_Number fraction = 0, mass = 0, energy = 0;
};
struct CellCache {
  unsigned serial = 0;
  std::vector<CachedEntry> entries;  // in the table's slot order at the last call (lua_gpg_tablepairs)
  std::vector<uint32_t> order;       // entries' indices by id
  std::vector<uint32_t> slots;       // a flat hash of the entries by key address (index + 1; 0 empty), power of 2
  void Index() {
    size_t cap = 16;
    while (cap < 2 * entries.size()) cap <<= 1;
    slots.assign(cap, 0);
    for (size_t i = 0; i < entries.size(); ++i) {
      size_t h = Hash(entries[i].key) & (cap - 1);
      while (slots[h]) h = (h + 1) & (cap - 1);
      slots[h] = static_cast<uint32_t>(i + 1);
    }
  }
  int32_t Find(const void* key) const {
    if (slots.empty()) return -1;
    const size_t m = slots.size() - 1;
    for (size_t h = Hash(key) & m;; h = (h + 1) & m) {
      const uint32_t v = slots[h];
      if (!v) return -1;
      if (entries[v - 1].key == key) return static_cast<int32_t>(v - 1);
    }
  }
  static size_t Hash(const void* p) { return static_cast<size_t>((reinterpret_cast<uintptr_t>(p) >> 4) * 0x9E3779B97F4A7C15ull >> 20); }
};
// The cells' caches of one Lua state, owned by its ScriptState (reclaimCells). Until 2026-11-10 this was one map for
// the process, never cleared: an entry is recognised by addresses (the cell's table, the id string, the prop's table)
// and by serials and write stamps, which start again in every state. A second game of the same program run whose
// state lands on the first game's addresses then takes entries of the first: its three values, and a handle box in
// the first state's freed memory (review of 2026-11-09, R7). Not seen with the game's allocator (16 games in one run
// put the same table at 16 addresses); tests/reclaim_state_cache_test.cpp forces the addresses and shows it.
// A state runs on one thread at a time and the Lua API is never called from a pool task, so there is no lock.
using CellMap = std::unordered_map<const void*, CellCache>;
CellMap* Cells(lua_State* L) {
  ScriptState* st = ScriptState::From(L);
  if (!st) return nullptr;  // (a state the engine did not make: no cache, the full re-sum)
  if (!st->reclaimCells) st->reclaimCells = std::make_shared<CellMap>();
  return static_cast<CellMap*>(st->reclaimCells.get());
}

// The box of the handle the value at idx carries (IsDestroyedValue's reads, without the collector's check: the full
// re-sum calls it with checkGc false). nullptr: no handle.
void** HandleBox(lua_State* L, int idx) {
  void** box = nullptr;
  if (lua_istable(L, idx)) lua_rawgetcobject(L, idx);
  else lua_pushvalue(L, idx);
  if (lua_type(L, -1) == LUA_TUSERDATA && lua_getmetatable(L, -1)) {
    if (IsObjectHandleMeta(L, -1)) box = static_cast<void**>(lua_touserdata(L, -2));
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  return box;
}

// An entry read as the full re-sum reads it (ReadReclaim, IsDestroyedValue's box), the value at `v`, its key at `k`.
// False: the full re-sum would leave this cell to the Lua (an id or a field it does not take).
bool ReadEntry(lua_State* L, int k, int v, int keys, CachedEntry* e) {
  if (lua_type(L, v) != LUA_TTABLE || !ParseId(L, k, &e->id)) return false;
  if (!ReadReclaim(L, v, keys, &e->fraction, &e->mass, &e->energy)) return false;
  bool own = true;
  for (int i = 0; i < 3 && own; ++i) {
    lua_pushvalue(L, keys + i);
    lua_rawget(L, v);
    own = !lua_isnil(L, -1);
    lua_pop(L, 1);
  }
  e->cacheable = own;
  e->box = HandleBox(L, v);
  e->prop = lua_topointer(L, v);
  lua_gpg_tablestamp(e->prop, &e->propSerial, &e->propStamp);
  size_t len = 0;
  const char* text = lua_tostring(L, k);
  len = lua_strlen(L, k);
  e->keyLen = static_cast<uint8_t>(len);
  std::memcpy(e->keyText, text, len);  // (ParseId took it: 1..12 digits)
  return true;
}

bool SameKey(const CachedEntry& c, const void* key) {
  size_t len = 0;
  const char* text = lua_gpg_strptr(key, &len);
  return len == c.keyLen && !std::memcmp(text, c.keyText, len);
}

// MOHO64_DEBUG_RECLAIM=1 (statistics only): calls, entries, how many were taken from the cache, the time, at exit
struct CacheStats {
  bool on = std::getenv("MOHO64_DEBUG_RECLAIM") != nullptr;
  uint64_t calls = 0, entries = 0, reused = 0;
  double ms = 0;
  ~CacheStats() {
    if (on)
      std::fprintf(stderr, "moho64 reclaim totals (cached): %llu calls, %llu entries, %llu taken from the cache, %.1f ms\n",
                   static_cast<unsigned long long>(calls), static_cast<unsigned long long>(entries),
                   static_cast<unsigned long long>(reused), ms);
  }
};
CacheStats g_cacheStats;
double NowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

int CachedCellTotalsImpl(lua_State* L);
int CachedCellTotals(lua_State* L) {
  if (!g_cacheStats.on) return CachedCellTotalsImpl(L);
  const double t0 = NowMs();
  const int r = CachedCellTotalsImpl(L);
  g_cacheStats.ms += NowMs() - t0;
  ++g_cacheStats.calls;
  return r;
}

int CachedCellTotalsImpl(lua_State* L) {
  if (lua_type(L, 1) != LUA_TTABLE) return 0;
  lua_settop(L, 1);
  CellMap* cells = Cells(L);
  if (!cells) return FullCellTotals(L);
  static thread_local std::vector<lua_GpgPair> pairs;
  if (pairs.size() < 64) pairs.resize(64);
  int n = lua_gpg_tablepairs(L, 1, pairs.data(), static_cast<int>(pairs.size()));
  if (n > static_cast<int>(pairs.size())) {
    pairs.resize(static_cast<size_t>(n));
    n = lua_gpg_tablepairs(L, 1, pairs.data(), n);
  }
  for (int i = 0; i < n; ++i)
    if (pairs[i].kt != LUA_TSTRING || pairs[i].vt != LUA_TTABLE) return FullCellTotals(L);
  const void* cell = lua_topointer(L, 1);
  unsigned cellSerial = 0, cellStamp = 0;
  lua_gpg_tablestamp(cell, &cellSerial, &cellStamp);
  CellCache& cache = (*cells)[cell];
  if (cache.serial != cellSerial) {
    cache = CellCache{};
    cache.serial = cellSerial;
  }
  const std::vector<CachedEntry>& old = cache.entries;
  // which cached entry each slot's pair may be: the same slot's first (the table's slots move only when it grows or a
  // key is added in a colliding place), else by the key's address
  static thread_local std::vector<int32_t> match;
  match.resize(static_cast<size_t>(n));
  bool sameSlots = n == static_cast<int>(old.size());
  for (int i = 0; i < n; ++i) {
    int32_t j = -1;
    if (i < static_cast<int>(old.size()) && old[static_cast<size_t>(i)].key == pairs[i].k) j = i;
    else {
      j = cache.Find(pairs[i].k);
      sameSlots = false;
    }
    if (j >= 0 && old[static_cast<size_t>(j)].prop != pairs[i].v) j = -1;
    match[static_cast<size_t>(i)] = j;
  }
  constexpr int kAhead = 8;  // (each entry's prop, key and box fetched this many entries ahead)
  auto fetch = [&](int i) {
    const int32_t j = match[static_cast<size_t>(i)];
    if (j < 0) return;
    __builtin_prefetch(pairs[i].v);
    __builtin_prefetch(static_cast<const char*>(pairs[i].v) + 56);
    __builtin_prefetch(pairs[i].k);
    if (void** b = old[static_cast<size_t>(j)].box) __builtin_prefetch(b);
  };
  for (int i = 0; i < n && i < kAhead; ++i) fetch(i);
  static thread_local std::vector<CachedEntry> cur;
  cur.resize(static_cast<size_t>(n));
  lua_pushstring(L, "ReclaimLeft");       // 2
  lua_pushstring(L, "MaxMassReclaim");    // 3
  lua_pushstring(L, "MaxEnergyReclaim");  // 4
  for (int i = 0; i < n; ++i) {
    if (i + kAhead < n) fetch(i + kAhead);
    const int32_t j = match[static_cast<size_t>(i)];
    CachedEntry& e = cur[static_cast<size_t>(i)];
    if (j >= 0) {
      const CachedEntry& c = old[static_cast<size_t>(j)];
      unsigned serial = 0, stamp = 0;
      lua_gpg_tablestamp(pairs[i].v, &serial, &stamp);
      if (c.cacheable && serial == c.propSerial && stamp == c.propStamp && SameKey(c, pairs[i].k)) {
        e = c;
        ++g_cacheStats.reused;
        continue;
      }
    }
    lua_gpg_pushgc(L, pairs[i].k, LUA_TSTRING);  // 5
    lua_gpg_pushgc(L, pairs[i].v, LUA_TTABLE);   // 6
    const bool ok = ReadEntry(L, 5, 6, 2, &e);
    lua_settop(L, 4);
    e.key = pairs[i].k;
    if (!ok) {
      cache = CellCache{};
      cache.serial = cellSerial;
      lua_settop(L, 1);
      return FullCellTotals(L);
    }
    // (the order holds while each slot keeps its id: an entry read again in its slot may carry another string, if the
    // cached one was collected and a new one made at its address)
    if (j < 0 || e.id != old[static_cast<size_t>(j)].id) sameSlots = false;
  }
  // the order by id: kept while every slot holds the key it held (the same keys, so the same ids), else sorted again
  if (!sameSlots || cache.order.size() != static_cast<size_t>(n)) {
    cache.order.resize(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) cache.order[static_cast<size_t>(i)] = static_cast<uint32_t>(i);
    std::sort(cache.order.begin(), cache.order.end(),
              [](uint32_t a, uint32_t b) { return cur[a].id < cur[b].id; });
  }
  g_cacheStats.entries += static_cast<uint64_t>(n);
  lua_Number count = 0, totalMass = 0, totalEnergy = 0;
  for (int i = 0; i < n; ++i) count = count + 1;
  for (const uint32_t k : cache.order) {
    const CachedEntry& c = cur[k];
    if (!c.box || !*c.box) continue;  // (IsDestroyed)
    totalMass = totalMass + c.fraction * c.mass;
    totalEnergy = totalEnergy + c.fraction * c.energy;
  }
  cache.entries.swap(cur);
  if (!sameSlots) cache.Index();
  lua_settop(L, 0);
  lua_pushnumber(L, count);
  lua_pushnumber(L, totalMass);
  lua_pushnumber(L, totalEnergy);
  return 3;
}

struct VerifyCounts {
  uint64_t calls = 0, left = 0;
  ~VerifyCounts() {
    if (calls) std::fprintf(stderr, "moho64 verify reclaim totals: %llu calls, all equal to the full re-sum (%llu left to the Lua)\n",
                            static_cast<unsigned long long>(calls), static_cast<unsigned long long>(left));
  }
};
VerifyCounts g_verify;

// Remastered.ReclaimCellTotals(reclaims): FullCellTotals' results, from the cells' cache.
int l_ReclaimCellTotals(lua_State* L) {
  static const bool verify = std::getenv("MOHO64_VERIFY_RECLAIM") != nullptr;
  if (!verify) return CachedCellTotals(L);
  // the full re-sum first, then the cached one on the same table (pushed again by its address: the caller still holds
  // it; nothing is made, so the collector's counts stay as without the switch); both must agree in every bit
  lua_settop(L, 1);
  const bool isTable = lua_type(L, 1) == LUA_TTABLE;
  const void* arg = isTable ? lua_topointer(L, 1) : nullptr;
  const int nf = FullCellTotals(L);  // (leaves only its results)
  lua_Number f[3] = {0, 0, 0};
  for (int i = 0; i < nf; ++i) f[i] = lua_tonumber(L, -nf + i);
  lua_settop(L, 0);
  if (isTable) lua_gpg_pushgc(L, arg, LUA_TTABLE);
  else lua_pushnil(L);
  const int nc = CachedCellTotals(L);
  ++g_verify.calls;
  if (!nf) ++g_verify.left;
  bool same = nf == nc;
  for (int i = 0; i < nc && same; ++i) {
    const lua_Number c = lua_tonumber(L, -nc + i);
    same = !std::memcmp(&c, &f[i], sizeof c);
  }
  if (!same) {
    std::fprintf(stderr, "moho64 verify reclaim totals: call %llu: the cache gives %d results, the full re-sum %d (%g %g %g)\n",
                 static_cast<unsigned long long>(g_verify.calls), nc, nf, f[0], f[1], f[2]);
    std::abort();
  }
  return nc;
}

// FAF's SortLambda(a, b), `return a.TotalMass > b.TotalMass`, as table.sort's comparison for the values at a and b:
// a.TotalMass, then b.TotalMass, each through lua_gettable (metamethods as the VM's `a.TotalMass`; the key, the string
// "TotalMass", at stack index 2), then the VM's `>`: OP_LT with the operands swapped (lua_lessthan, numbers, strings and
// __lt as the VM's). lua_gpg_checkgc: the collector's check that luaD_call makes when the script comparison returns.
int TotalMassGreater(lua_State* L, int a, int b) {
  lua_pushvalue(L, 2);
  lua_gettable(L, a - 1);
  lua_pushvalue(L, 2);
  lua_gettable(L, b - 2);
  const int r = lua_lessthan(L, -1, -2);
  lua_pop(L, 2);
  lua_gpg_checkgc(L);
  return r;
}

// Remastered.SortCellsByTotalMass(t): table.sort(t, SortLambda) with SortLambda in C: the same routine (ltablib.c
// luaB_sortwith: the length as table.sort takes it, the same quicksort, the same comparisons in the same order, the
// same swaps), so the same order, ties included. The key string is the closure's upvalue (made once, when Remastered is
// made): nothing is made while sorting.
int l_SortCellsByTotalMass(lua_State* L) {
  lua_settop(L, 1);
  lua_pushvalue(L, lua_upvalueindex(1));
  luaB_sortwith(L, TotalMassGreater);
  return 0;
}

}  // namespace

void RegisterRemasteredReclaim(lua_State* L) {
  lua_pushstring(L, "ReclaimCellTotals");
  lua_pushcfunction(L, l_ReclaimCellTotals);
  lua_rawset(L, -3);
  lua_pushstring(L, "SortCellsByTotalMass");
  lua_pushstring(L, "TotalMass");
  lua_pushcclosure(L, l_SortCellsByTotalMass, 1);
  lua_rawset(L, -3);
}

}  // namespace moho
