#include "sim/blueprints.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#include "core/log.h"
#include "script/script_state.h"
#include "sim/entity.h"
#include "sim/script_object.h"

namespace moho {
namespace {

const char* kRecordKey = "moho64.registered";
const char kBpsKey = 0;
int g_words = 1;  // 64-bit words per category set (fixed once blueprints are loaded)

std::string Lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

BpKind KindFromName(const char* k) {
  if (!std::strcmp(k, "Unit")) return BpKind::Unit;
  if (!std::strcmp(k, "Projectile")) return BpKind::Projectile;
  if (!std::strcmp(k, "Prop")) return BpKind::Prop;
  if (!std::strcmp(k, "Mesh")) return BpKind::Mesh;
  if (!std::strcmp(k, "Emitter")) return BpKind::Emitter;
  if (!std::strcmp(k, "TrailEmitter")) return BpKind::TrailEmitter;
  if (!std::strcmp(k, "Beam")) return BpKind::Beam;
  return BpKind::Other;
}

// Copy a Lua value between two independent states. Tables keep their identity (shared
// sub-tables stay shared); functions and userdata do not cross and become nil.
struct Copier {
  lua_State* from;
  lua_State* to;
  int seen;  // index of a table in `to`: lightuserdata(source table) -> copy
  int sounds = 0;  // index in `from` of the tables made by Sound{} (0: none)
  int dropped = 0;

  void Copy(int idx) {
    switch (lua_type(from, idx)) {
      case LUA_TNUMBER: lua_pushnumber(to, lua_tonumber(from, idx)); break;
      case LUA_TBOOLEAN: lua_pushboolean(to, lua_toboolean(from, idx)); break;
      case LUA_TSTRING: lua_pushlstring(to, lua_tostring(from, idx), lua_strlen(from, idx)); break;
      case LUA_TTABLE: CopyTable(idx); break;
      case LUA_TNIL: lua_pushnil(to); break;
      default:
        ++dropped;
        lua_pushnil(to);
    }
  }

  void CopyTable(int idx) {
    if (idx < 0) idx = lua_gettop(from) + idx + 1;
    lua_checkstack(from, 8);
    lua_checkstack(to, 8);
    const void* key = lua_topointer(from, idx);
    lua_pushlightuserdata(to, const_cast<void*>(key));
    lua_rawget(to, seen);
    if (!lua_isnil(to, -1)) return;
    lua_pop(to, 1);
    bool sound = false;
    if (sounds) {
      lua_pushvalue(from, idx);
      lua_rawget(from, sounds);
      sound = lua_toboolean(from, -1);
      lua_pop(from, 1);
    }
    if (sound) {  // Sound{...} -> a sound object
      lua_newtable(to);
      for (const char* k : {"Bank", "Cue", "LodCutoff"}) {
        lua_pushstring(from, k);
        lua_rawget(from, idx);
        if (lua_isstring(from, -1)) {
          lua_pushstring(to, k);
          lua_pushstring(to, lua_tostring(from, -1));
          lua_rawset(to, -3);
        }
        lua_pop(from, 1);
      }
      PushSound(to, -1);
      lua_remove(to, -2);
      lua_pushlightuserdata(to, const_cast<void*>(key));
      lua_pushvalue(to, -2);
      lua_rawset(to, seen);
      return;
    }
    lua_newtable(to);
    lua_pushlightuserdata(to, const_cast<void*>(key));
    lua_pushvalue(to, -2);
    lua_rawset(to, seen);
    int dst = lua_gettop(to);
    lua_pushnil(from);
    while (lua_next(from, idx)) {
      Copy(-2);
      Copy(-1);
      if (lua_isnil(to, -2) || lua_isnil(to, -1)) lua_pop(to, 2);
      else lua_rawset(to, dst);
      lua_pop(from, 1);
    }
    if (lua_getmetatable(from, idx)) {  // blueprints have none, but keep what is there
      lua_pop(from, 1);
    }
  }
};

uint64_t* NewCat(lua_State* L) {
  auto* w = static_cast<uint64_t*>(lua_newuserdata(L, sizeof(uint64_t) * g_words));
  std::memset(w, 0, sizeof(uint64_t) * g_words);
  PushClassTable(L, "EntityCategory");
  lua_setmetatable(L, -2);
  return w;
}

const uint64_t* CheckCat(lua_State* L, int idx) {
  const uint64_t* c = ToCategory(L, idx);
  if (!c) luaL_argerror(L, idx, "EntityCategory expected");
  return c;
}

SimBlueprints* Bps(lua_State* L) { return SimBlueprints::From(L); }

// The entity index of a unit/entity object, a blueprint table, or a blueprint id.
int EntityIndexOf(lua_State* L, int idx) {
  SimBlueprints* b = Bps(L);
  if (lua_isstring(L, idx)) {
    const BlueprintInfo* bp = b->Find(lua_tostring(L, idx));
    return bp ? bp->entityIndex : -1;
  }
  if (!lua_istable(L, idx)) return -1;
  if (Entity* e = ToObject<Entity>(L, idx)) return e->blueprint ? e->blueprint->entityIndex : -1;
  lua_pushstring(L, "BlueprintId");
  lua_rawget(L, idx < 0 ? idx - 1 : idx);
  int r = -1;
  if (lua_isstring(L, -1)) {
    const BlueprintInfo* bp = b->Find(lua_tostring(L, -1));
    r = bp ? bp->entityIndex : -1;
  }
  lua_pop(L, 1);
  return r;
}

int l_cat_add(lua_State* L) {
  const uint64_t *a = CheckCat(L, 1), *b = CheckCat(L, 2);
  uint64_t* r = NewCat(L);
  for (int i = 0; i < g_words; ++i) r[i] = a[i] | b[i];
  return 1;
}
int l_cat_sub(lua_State* L) {
  const uint64_t *a = CheckCat(L, 1), *b = CheckCat(L, 2);
  uint64_t* r = NewCat(L);
  for (int i = 0; i < g_words; ++i) r[i] = a[i] & ~b[i];
  return 1;
}
int l_cat_mul(lua_State* L) {
  const uint64_t *a = CheckCat(L, 1), *b = CheckCat(L, 2);
  uint64_t* r = NewCat(L);
  for (int i = 0; i < g_words; ++i) r[i] = a[i] & b[i];
  return 1;
}

// "TECH1 LAND" style expressions: names combined left to right with + - * (no precedence),
// spaces mean intersection like '*', parentheses group.
bool ParseExpr(lua_State* L, const char*& p, std::vector<uint64_t>& out);
bool ParseTerm(lua_State* L, const char*& p, std::vector<uint64_t>& out) {
  while (*p == ' ' || *p == '\t') ++p;
  if (*p == '(') {
    ++p;
    if (!ParseExpr(L, p, out)) return false;
    while (*p == ' ') ++p;
    if (*p != ')') return false;
    ++p;
    return true;
  }
  const char* s = p;
  while (std::isalnum(static_cast<unsigned char>(*p)) || *p == '_') ++p;
  if (p == s) return false;
  std::string name(s, p);
  out.assign(g_words, 0);
  auto& cats = Bps(L)->Categories();
  auto it = cats.find(name);
  if (it == cats.end()) {
    // a unit id names that unit's blueprint
    if (const BlueprintInfo* bp = Bps(L)->Find(name); bp && bp->entityIndex >= 0) {
      out[bp->entityIndex >> 6] |= uint64_t(1) << (bp->entityIndex & 63);
      return true;
    }
    return true;  // a name no blueprint uses is an empty category, not an error
  }
  for (int e : it->second) out[e >> 6] |= uint64_t(1) << (e & 63);
  return true;
}
bool ParseExpr(lua_State* L, const char*& p, std::vector<uint64_t>& out) {
  if (!ParseTerm(L, p, out)) return false;
  for (;;) {
    while (*p == ' ' || *p == '\t') ++p;
    char op = *p;
    if (op == 0 || op == ')') return true;
    if (op == '+' || op == '-' || op == '*') ++p;
    else if (op == ',') {  // "A, B": a list of categories = their union
      ++p;
      op = '+';
    } else op = '*';
    std::vector<uint64_t> rhs;
    if (!ParseTerm(L, p, rhs)) return false;
    for (int i = 0; i < g_words; ++i)
      out[i] = op == '+' ? out[i] | rhs[i] : op == '-' ? out[i] & ~rhs[i] : out[i] & rhs[i];
  }
}

int l_ParseEntityCategory(lua_State* L) {
  const char* s = luaL_checkstring(L, 1);
  const char* p = s;
  std::vector<uint64_t> bits;
  if (!ParseExpr(L, p, bits) || *p) return luaL_error(L, "Invalid category expression: %s", s);
  uint64_t* r = NewCat(L);
  for (int i = 0; i < g_words; ++i) r[i] = bits[i];
  return 1;
}

int l_EntityCategoryContains(lua_State* L) {
  const uint64_t* c = CheckCat(L, 1);
  int e = EntityIndexOf(L, 2);
  lua_pushboolean(L, e >= 0 && CategoryHas(c, e));
  return 1;
}

int FilterImpl(lua_State* L, bool keep) {
  const uint64_t* c = CheckCat(L, 1);
  lua_newtable(L);
  if (!lua_istable(L, 2)) return 1;
  int out = lua_gettop(L), n = 0;
  for (int i = 1;; ++i) {
    lua_rawgeti(L, 2, i);
    if (lua_isnil(L, -1)) {
      lua_pop(L, 1);
      break;
    }
    int e = EntityIndexOf(L, -1);
    if ((e >= 0 && CategoryHas(c, e)) == keep) lua_rawseti(L, out, ++n);
    else lua_pop(L, 1);
  }
  return 1;
}
int l_EntityCategoryFilterDown(lua_State* L) { return FilterImpl(L, true); }
int l_EntityCategoryFilterOut(lua_State* L) { return FilterImpl(L, false); }

int l_EntityCategoryCount(lua_State* L) {
  const uint64_t* c = CheckCat(L, 1);
  int n = 0;
  if (lua_istable(L, 2))
    for (int i = 1;; ++i) {
      lua_rawgeti(L, 2, i);
      if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        break;
      }
      int e = EntityIndexOf(L, -1);
      if (e >= 0 && CategoryHas(c, e)) ++n;
      lua_pop(L, 1);
    }
  lua_pushnumber(L, n);
  return 1;
}

int l_EntityCategoryGetUnitList(lua_State* L) {
  const uint64_t* c = CheckCat(L, 1);
  SimBlueprints* b = Bps(L);
  lua_newtable(L);
  int n = 0;
  for (int e = 0; e < b->EntityCount(); ++e) {
    const BlueprintInfo* bp = b->ByEntityIndex(e);
    if (bp->kind == BpKind::Unit && CategoryHas(c, e)) {
      lua_pushstring(L, bp->id.c_str());
      lua_rawseti(L, -2, ++n);
    }
  }
  return 1;
}

int l_EntityCategoryEmpty(lua_State* L) {
  const uint64_t* c = CheckCat(L, 1);
  bool empty = true;
  for (int i = 0; i < g_words; ++i) empty = empty && c[i] == 0;
  lua_pushboolean(L, empty);
  return 1;
}


}  // namespace

uint64_t* PushCategory(lua_State* L) { return NewCat(L); }

const uint64_t* ToCategory(lua_State* L, int idx) {
  if (lua_type(L, idx) != LUA_TUSERDATA) return nullptr;
  if (!lua_getmetatable(L, idx)) return nullptr;
  PushClassTable(L, "EntityCategory");
  bool ok = lua_rawequal(L, -1, -2);
  lua_pop(L, 2);
  return ok ? static_cast<const uint64_t*>(lua_touserdata(L, idx)) : nullptr;
}

bool CategoryHas(const uint64_t* bits, int e) { return (bits[e >> 6] >> (e & 63)) & 1; }

void SimBlueprints::StartRecording(ScriptState& rules) {
  lua_State* L = rules.L();
  lua_pushstring(L, kRecordKey);
  lua_newtable(L);
  lua_rawset(L, LUA_REGISTRYINDEX);
  auto prev = rules.onRegisterBlueprint;
  rules.onRegisterBlueprint = [prev](lua_State* L, const char* kind) {
    if (prev) prev(L, kind);
    lua_pushstring(L, kRecordKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    int n = static_cast<int>(luaL_getn(L, -1));
    lua_newtable(L);
    lua_pushstring(L, kind);
    lua_rawseti(L, -2, 1);
    lua_pushvalue(L, 1);
    lua_rawseti(L, -2, 2);
    lua_rawseti(L, -2, n + 1);
    luaL_setn(L, -1, n + 1);
    lua_pop(L, 1);
  };
}

SimBlueprints* SimBlueprints::From(lua_State* L) {
  lua_pushlightuserdata(L, const_cast<char*>(&kBpsKey));
  lua_rawget(L, LUA_REGISTRYINDEX);
  auto* b = static_cast<SimBlueprints*>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  return b;
}

const BlueprintInfo* SimBlueprints::Find(const std::string& id) const {
  auto it = byId_.find(Lower(id));
  return it == byId_.end() ? nullptr : &all_[it->second];
}

// SpecFootprints { {Name=, SizeX=, SizeZ=, Caps=, MinWaterDepth=, MaxWaterDepth=, MaxSlope=, Flags=}, ... }:
// kept in order; a repeated name is ignored with a warning (as the original).
void SimBlueprints::ReadFootprints(lua_State* L, int list) {
  int n = static_cast<int>(luaL_getn(L, list));
  for (int i = 1; i <= n; ++i) {
    lua_rawgeti(L, list, i);
    lua_rawgeti(L, -1, 1);
    bool isFootprints = lua_isstring(L, -1) && !std::strcmp(lua_tostring(L, -1), "Footprints");
    lua_pop(L, 1);
    if (isFootprints) {
      lua_rawgeti(L, -1, 2);
      int specs = lua_gettop(L);
      for (int k = 1;; ++k) {
        lua_rawgeti(L, specs, k);
        if (!lua_istable(L, -1)) {
          lua_pop(L, 1);
          break;
        }
        auto num = [&](const char* key, double def) {
          lua_pushstring(L, key);
          lua_rawget(L, -2);
          double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : def;
          lua_pop(L, 1);
          return v;
        };
        NamedFootprint f;
        lua_pushstring(L, "Name");
        lua_rawget(L, -2);
        f.name = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        f.sizeX = static_cast<uint8_t>(static_cast<int>(num("SizeX", 0)));
        f.sizeZ = static_cast<uint8_t>(static_cast<int>(num("SizeZ", 0)));
        f.caps = static_cast<uint8_t>(static_cast<int>(num("Caps", 0)));
        f.minWaterDepth = static_cast<float>(num("MinWaterDepth", 0));
        f.maxWaterDepth = static_cast<float>(num("MaxWaterDepth", 0));
        f.maxSlope = static_cast<float>(num("MaxSlope", 0));
        f.flags = static_cast<uint8_t>(static_cast<int>(num("Flags", 0)));
        bool seen = false;
        for (const auto& o : footprints_) seen |= o.name == f.name;
        if (seen) Logf(LogLevel::Warning, "Ignoring duplicate footprint spec %s", f.name.c_str());
        else footprints_.push_back(f);
        lua_pop(L, 1);
      }
      lua_pop(L, 1);
    }
    lua_pop(L, 1);
  }
  Logf(LogLevel::Debug, "moho64: %zu named footprints", footprints_.size());
}

void SimBlueprints::CopyToSim(lua_State* rules, lua_State* sim) {
  lua_pushlightuserdata(sim, const_cast<char*>(&kBpsKey));
  lua_pushlightuserdata(sim, this);
  lua_rawset(sim, LUA_REGISTRYINDEX);

  lua_pushstring(rules, kRecordKey);
  lua_rawget(rules, LUA_REGISTRYINDEX);
  int list = lua_gettop(rules);
  int n = static_cast<int>(luaL_getn(rules, list));
  lua_newtable(sim);  // seen
  Copier cp{rules, sim, lua_gettop(sim)};
  lua_pushstring(rules, "moho64.sounds");
  lua_rawget(rules, LUA_REGISTRYINDEX);
  if (lua_istable(rules, -1)) cp.sounds = lua_gettop(rules);
  else lua_pop(rules, 1);
  lua_pushstring(sim, "__blueprints");
  lua_newtable(sim);
  int bpt = lua_gettop(sim);
  all_.reserve(n);
  ReadFootprints(rules, list);
  int duplicates = 0;
  for (int i = 1; i <= n; ++i) {
    lua_rawgeti(rules, list, i);
    lua_rawgeti(rules, -1, 1);
    BpKind kind = KindFromName(lua_tostring(rules, -1));
    lua_pop(rules, 1);
    if (kind != BpKind::Unit && kind != BpKind::Projectile && kind != BpKind::Prop && kind != BpKind::Mesh) {
      lua_pop(rules, 1);
      continue;
    }
    lua_rawgeti(rules, -1, 2);
    lua_pushstring(rules, "BlueprintId");
    lua_rawget(rules, -2);
    std::string id = lua_isstring(rules, -1) ? lua_tostring(rules, -1) : "";
    lua_pop(rules, 1);
    cp.Copy(-1);  // -> sim
    lua_pop(rules, 2);
    // A blueprint registered again under the same id replaces the earlier one and keeps its
    // ordinal (the original looks the id up and re-initialises the existing blueprint).
    auto dup = id.empty() ? byId_.end() : byId_.find(Lower(id));
    if (dup != byId_.end() && all_[dup->second].kind != kind) dup = byId_.end();
    BlueprintInfo fresh;
    BlueprintInfo& info = dup != byId_.end() ? all_[dup->second] : fresh;
    if (dup == byId_.end()) {
      info.kind = kind;
      info.id = id;
      info.ordinal = static_cast<int>(all_.size());  // 0-based: AssignNextOrdinal is the count so far
    } else {
      // The same engine blueprint is re-read from the new table: engine fields the new table
      // leaves out keep their earlier values (also values derived the first time).
      lua_rawgeti(sim, LUA_REGISTRYINDEX, info.ref);  // the earlier table
      if (reflect) CarryOverEngineFields(sim, lua_gettop(sim) - 1, lua_gettop(sim), kind);
      lua_pop(sim, 1);
      luaL_unref(sim, LUA_REGISTRYINDEX, info.ref);
      ++duplicates;
      Logf(LogLevel::Debug, "moho64: blueprint %s registered again as %s", info.id.c_str(), id.c_str());
      info.id = id;
    }
    lua_pushvalue(sim, -1);
    lua_rawseti(sim, bpt, info.ordinal);
    if (!id.empty()) {  // under the id as now given: an earlier spelling ("FAB4401") keeps the old table
      lua_pushstring(sim, id.c_str());
      lua_pushvalue(sim, -2);
      lua_rawset(sim, bpt);
    }
    // categories
    if (kind != BpKind::Mesh) {
      if (info.entityIndex < 0) {
        info.entityIndex = static_cast<int>(entities_.size());
        entities_.push_back(nullptr);  // fixed up below
      }
      // every entity blueprint id (as registered) is a category of its own
      if (!id.empty()) categories_[id].push_back(info.entityIndex);
      lua_pushstring(sim, "Categories");
      lua_rawget(sim, -2);
      if (lua_istable(sim, -1))
        for (int k = 1;; ++k) {
          lua_rawgeti(sim, -1, k);
          if (lua_isnil(sim, -1)) {
            lua_pop(sim, 1);
            break;
          }
          if (lua_isstring(sim, -1)) categories_[lua_tostring(sim, -1)].push_back(info.entityIndex);
          lua_pop(sim, 1);
        }
      lua_pop(sim, 1);
    }
    if (reflect) ReflectBlueprint(sim, -1, info, *this);
    info.ref = luaL_ref(sim, LUA_REGISTRYINDEX);
    if (dup == byId_.end()) {
      if (!id.empty()) byId_.emplace(Lower(id), all_.size());
      all_.push_back(std::move(info));
    }
  }
  lua_rawset(sim, LUA_GLOBALSINDEX);  // __blueprints
  lua_pop(sim, 1);                    // seen
  lua_settop(rules, list - 1);        // the record list (and the sound set)
  for (const auto& bp : all_)
    if (bp.entityIndex >= 0) entities_[bp.entityIndex] = &bp;
  g_words = std::max(1, (EntityCount() + 63) / 64);
  // Categories the engine defines itself.
  for (const auto& bp : all_) {
    if (bp.kind == BpKind::Unit) {
      categories_["ALLUNITS"].push_back(bp.entityIndex);
    }
    if (bp.kind == BpKind::Projectile) categories_["ALLPROJECTILES"].push_back(bp.entityIndex);
  }
  for (auto& [name, v] : categories_) {  // de-duplicate (a blueprint may list a category twice)
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
  }
  if (duplicates) Logf(LogLevel::Debug, "moho64: %d blueprints registered again under the same id", duplicates);
  if (cp.dropped) Logf(LogLevel::Debug, "moho64: %d non-data blueprint values not copied to the sim", cp.dropped);
  Logf(LogLevel::Debug, "moho64: sim blueprints: %zu (%d entity), %zu categories", all_.size(), EntityCount(),
       categories_.size());
}

void RegisterCategoryBindings(lua_State* L, SimBlueprints* bps) {
  SetMethod(L, "EntityCategory", "__add", l_cat_add);
  SetMethod(L, "EntityCategory", "__sub", l_cat_sub);
  SetMethod(L, "EntityCategory", "__mul", l_cat_mul);
  SetGlobal(L, "ParseEntityCategory", l_ParseEntityCategory);
  SetGlobal(L, "EntityCategoryContains", l_EntityCategoryContains);
  SetGlobal(L, "EntityCategoryFilterDown", l_EntityCategoryFilterDown);
  SetGlobal(L, "EntityCategoryFilterOut", l_EntityCategoryFilterOut);
  SetGlobal(L, "EntityCategoryCount", l_EntityCategoryCount);
  SetGlobal(L, "EntityCategoryGetUnitList", l_EntityCategoryGetUnitList);
  SetGlobal(L, "EntityCategoryEmpty", l_EntityCategoryEmpty);
  // categories.<NAME>
  lua_pushstring(L, "categories");
  lua_newtable(L);
  for (const auto& [name, members] : bps->Categories()) {
    lua_pushstring(L, name.c_str());
    uint64_t* w = NewCat(L);
    for (int e : members) w[e >> 6] |= uint64_t(1) << (e & 63);
    lua_rawset(L, -3);
  }
  lua_rawset(L, LUA_GLOBALSINDEX);
}

}  // namespace moho
