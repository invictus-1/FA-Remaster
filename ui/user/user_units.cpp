// Units as the interface sees them: one script object per unit of the running simulation, the
// selection, and what the panels ask about units.
//
// FIRST VERSION, not read from the original yet. After every beat the list of units is taken from
// the engine's view functions (sim/sim_view.h) and each unit gets an object whose methods are the
// class table moho.UserUnit's; an object stays the same table for as long as its unit lives, so
// scripts can keep units as table keys. What a method cannot know yet (the command queue, shields,
// fuel, what is being built) answers with the plain case. SelectUnits tells the game script at
// once (gamemain.OnSelectionChanged(old, new, added, removed), FA exe 0x869418).
#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/log.h"
#include "script/script_handle.h"
#include "script/script_state.h"
#include "sim/blueprints.h"
#include "sim/sim.h"
#include "sim/sim_view.h"
#include "user/bindings.h"
#include "user/user_state.h"

namespace moho::ui {
namespace {

class UserUnit : public ScriptObject {
 public:
  UnitView view;
  bool gone = false;  // the simulation no longer has it
  std::string customName;
  std::vector<std::string> selectionSets;
};

struct UnitStore : UserState::Extra {
  std::map<uint32_t, std::unique_ptr<UserUnit>> units;
  std::vector<UserUnit*> selection;
  bool reporting = false;  // inside OnSelectionChanged
};

UnitStore& Store(lua_State* L) {
  UserState* user = UserState::From(L);
  if (!user->units) user->units = std::make_unique<UnitStore>();
  return static_cast<UnitStore&>(*user->units);
}
Sim* TheSim(lua_State* L) { return UserState::From(L)->config().sim; }

// The unit behind a script value; nullptr for anything else.
UserUnit* ToUnit(lua_State* L, int idx) {
  if (!lua_istable(L, idx)) return nullptr;
  return dynamic_cast<UserUnit*>(GetObject(L, idx));
}
UserUnit* CheckUnit(lua_State* L, int idx) { return CheckObject<UserUnit>(L, idx); }

void MakeTable(lua_State* L, UserUnit* u) {
  lua_newtable(L);
  lua_newtable(L);
  lua_pushstring(L, "__index");
  PushUserClassTable(L, "UserUnit");
  lua_rawset(L, -3);
  lua_setmetatable(L, -2);
  BindObject(L, lua_gettop(L), u);
  lua_pop(L, 1);
}

// The unit's blueprint table (__blueprints[id]) on the stack, or nil.
void PushBlueprint(lua_State* L, const std::string& id) {
  lua_getglobal(L, "__blueprints");
  if (!lua_istable(L, -1)) return;
  lua_pushstring(L, id.c_str());
  lua_rawget(L, -2);
  lua_remove(L, -2);
}

bool InCategory(lua_State* L, const UserUnit* u, const std::string& category) {
  SimBlueprints* bps = SimBlueprints::From(L);
  if (!bps) return false;
  const BlueprintInfo* bp = bps->Find(u->view.blueprint);
  auto it = bps->Categories().find(category);
  if (!bp || it == bps->Categories().end()) return false;
  return std::binary_search(it->second.begin(), it->second.end(), bp->entityIndex);
}

// A list of units as scripts get it: an array, or nil for none.
void PushUnitList(lua_State* L, const std::vector<UserUnit*>& list, bool nilWhenEmpty) {
  if (list.empty() && nilWhenEmpty) {
    lua_pushnil(L);
    return;
  }
  lua_newtable(L);
  int n = 0;
  for (UserUnit* u : list) {
    PushObject(L, u);
    lua_rawseti(L, -2, ++n);
  }
}

void ReadUnitList(lua_State* L, int idx, std::vector<UserUnit*>* out) {
  if (!lua_istable(L, idx)) return;
  lua_pushnil(L);
  while (lua_next(L, idx) != 0) {
    UserUnit* u = ToUnit(L, lua_gettop(L));
    if (u && !u->gone && std::find(out->begin(), out->end(), u) == out->end()) out->push_back(u);
    lua_pop(L, 1);
  }
}

// gamemain.OnSelectionChanged(oldSelection, newSelection, added, removed)
void ReportSelection(lua_State* L, const std::vector<UserUnit*>& before, const std::vector<UserUnit*>& now) {
  UnitStore& store = Store(L);
  if (store.reporting || !UserState::From(L)->gameInterface) return;
  std::vector<UserUnit*> added, removed;
  for (UserUnit* u : now)
    if (std::find(before.begin(), before.end(), u) == before.end()) added.push_back(u);
  for (UserUnit* u : before)
    if (std::find(now.begin(), now.end(), u) == now.end()) removed.push_back(u);
  int top = lua_gettop(L);
  lua_pushcfunction(L, ScriptTraceback);
  lua_getglobal(L, "import");
  lua_pushstring(L, "/lua/ui/game/gamemain.lua");
  if (lua_pcall(L, 1, 1, top + 1) == 0 && lua_istable(L, -1)) {
    lua_pushstring(L, "OnSelectionChanged");
    lua_gettable(L, -2);
    if (lua_isfunction(L, -1)) {
      PushUnitList(L, before, false);
      PushUnitList(L, now, false);
      PushUnitList(L, added, false);
      PushUnitList(L, removed, false);
      store.reporting = true;
      if (lua_pcall(L, 4, 0, top + 1) != 0)
        Logf(LogLevel::Warning, "Error running '/lua/ui/game/gamemain.lua:OnSelectionChanged': %s",
             lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
      store.reporting = false;
    }
  }
  lua_settop(L, top);
}

void SetSelection(lua_State* L, std::vector<UserUnit*> now) {
  UnitStore& store = Store(L);
  if (now == store.selection) return;
  std::vector<UserUnit*> before = store.selection;
  store.selection = std::move(now);
  ReportSelection(L, before, store.selection);
}

// --- globals ---

// GetSelectedUnits(): the selected units, or nil when there are none
int l_GetSelectedUnits(lua_State* L) {
  PushUnitList(L, Store(L).selection, true);
  return 1;
}
// SelectUnits(units): nil or an empty table selects nothing
int l_SelectUnits(lua_State* L) {
  std::vector<UserUnit*> list;
  ReadUnitList(L, 1, &list);
  SetSelection(L, list);
  return 0;
}
// ValidateUnitsList(units): the units of the list that still exist
int l_ValidateUnitsList(lua_State* L) {
  std::vector<UserUnit*> list;
  ReadUnitList(L, 1, &list);
  PushUnitList(L, list, false);
  return 1;
}
// GetUnitById(id): the unit of an entity id (a string, as GetEntityId gives it)
int l_GetUnitById(lua_State* L) {
  UnitStore& store = Store(L);
  auto it = store.units.find(static_cast<uint32_t>(std::strtoul(luaL_checkstring(L, 1), nullptr, 10)));
  if (it == store.units.end() || it->second->gone) lua_pushnil(L);
  else PushObject(L, it->second.get());
  return 1;
}

// The focus army's units of a category that fit a test.
template <class Test>
void FocusUnits(lua_State* L, const char* category, Test test, std::vector<UserUnit*>* out) {
  int army = TheSim(L)->focusArmy + 1;
  for (auto& [id, u] : Store(L).units)
    if (!u->gone && !u->view.dead && u->view.army == army && InCategory(L, u.get(), category) && test(*u)) out->push_back(u.get());
}
// GetArmyAvatars(): the focus army's commanders
int l_GetArmyAvatars(lua_State* L) {
  std::vector<UserUnit*> list;
  FocusUnits(L, "COMMAND", [](const UserUnit&) { return true; }, &list);
  PushUnitList(L, list, false);
  return 1;
}
// GetIdleEngineers() / GetIdleFactories(): the focus army's finished engineers / factories with
// nothing to do, or nil
int l_GetIdleEngineers(lua_State* L) {
  std::vector<UserUnit*> list;
  FocusUnits(L, "ENGINEER", [](const UserUnit& u) { return !u.view.beingBuilt && u.view.queueLength == 0; }, &list);
  PushUnitList(L, list, true);
  return 1;
}
int l_GetIdleFactories(lua_State* L) {
  std::vector<UserUnit*> list;
  FocusUnits(L, "FACTORY", [](const UserUnit& u) { return !u.view.beingBuilt && u.view.queueLength == 0; }, &list);
  PushUnitList(L, list, true);
  return 1;
}

// GetUnitCommandData(units): what the selected units can do, as the orders and construction panels
// need it: the names of their orders (General.CommandCaps), of their toggles (General.ToggleCaps),
// and the category of everything they can build (Economy.BuildableCategory). Each is the union
// over the units. (First version: build restrictions and upgrades a unit does not have yet are
// not taken off.)
int l_GetUnitCommandData(lua_State* L) {
  std::vector<UserUnit*> list;
  ReadUnitList(L, 1, &list);
  lua_settop(L, 1);
  lua_newtable(L);  // 2: orders
  lua_newtable(L);  // 3: toggles
  lua_pushnil(L);   // 4: buildable category
  std::vector<std::string> seen;
  auto collect = [&](const char* caps, int out) {
    lua_pushstring(L, "General");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
      lua_pushstring(L, caps);
      lua_gettable(L, -2);
      if (lua_istable(L, -1)) {
        int t = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, t) != 0) {
          if (lua_type(L, -2) == LUA_TSTRING && lua_toboolean(L, -1)) {
            std::string name = lua_tostring(L, -2);
            if (std::find(seen.begin(), seen.end(), name) == seen.end()) {
              seen.push_back(name);
              lua_pushstring(L, name.c_str());
              lua_rawseti(L, out, static_cast<int>(luaL_getn(L, out)) + 1);
            }
          }
          lua_pop(L, 1);
        }
      }
      lua_pop(L, 1);
    }
    lua_pop(L, 1);
  };
  for (UserUnit* u : list) {
    PushBlueprint(L, u->view.blueprint);
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      continue;
    }
    collect("CommandCaps", 2);
    collect("ToggleCaps", 3);
    lua_pushstring(L, "Economy");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
      lua_pushstring(L, "BuildableCategory");
      lua_gettable(L, -2);
      if (lua_istable(L, -1)) {
        int t = lua_gettop(L);
        for (int i = 1;; ++i) {
          lua_rawgeti(L, t, i);
          if (!lua_isstring(L, -1)) {
            lua_pop(L, 1);
            break;
          }
          lua_getglobal(L, "ParseEntityCategory");
          lua_insert(L, -2);
          if (lua_pcall(L, 1, 1, 0) != 0) {
            lua_pop(L, 1);
            continue;
          }
          if (lua_isnil(L, 4)) {
            lua_replace(L, 4);
          } else if (lua_getmetatable(L, -1)) {  // sum = sum + this one
            lua_pushstring(L, "__add");
            lua_rawget(L, -2);
            lua_pushvalue(L, 4);
            lua_pushvalue(L, -4);
            if (lua_pcall(L, 2, 1, 0) == 0) lua_replace(L, 4);
            else lua_pop(L, 1);
            lua_pop(L, 2);
          } else {
            lua_pop(L, 1);
          }
        }
      }
      lua_pop(L, 1);
    }
    lua_pop(L, 2);
  }
  if (lua_isnil(L, 4)) {  // nothing can be built: the empty category
    lua_getglobal(L, "ParseEntityCategory");
    lua_pushstring(L, "");
    if (lua_pcall(L, 1, 1, 0) == 0) lua_replace(L, 4);
    else lua_pop(L, 1);
  }
  lua_settop(L, 4);
  return 3;
}

// What the order buttons ask about the selected units' states. The interface does not get the
// states from the simulation yet, so each answers with the state a new unit is in: fire state 0
// (return fire), no script bit set, not paused, no automatic mode; nothing attached or assisting.
int l_ListZero(lua_State* L) {
  lua_pushnumber(L, 0);
  return 1;
}
int l_ListFalse(lua_State* L) {
  lua_pushboolean(L, 0);
  return 1;
}
int l_ListEmpty(lua_State* L) {
  lua_newtable(L);
  return 1;
}

// The engine's category functions know the simulation's own units; these stand in front of them
// so that the interface's units count by their blueprint.
int l_EntityCategoryContains(lua_State* L) {
  lua_settop(L, 2);
  if (UserUnit* u = ToUnit(L, 2)) {
    lua_pushstring(L, u->view.blueprint.c_str());
    lua_replace(L, 2);
  }
  lua_pushvalue(L, lua_upvalueindex(1));
  lua_insert(L, 1);
  lua_call(L, 2, 1);
  return 1;
}
// EntityCategoryFilterDown(category, units) keeps the units in the category, FilterOut the others.
int FilterUnits(lua_State* L, bool keepMembers) {
  lua_settop(L, 2);
  lua_newtable(L);
  if (!lua_istable(L, 2)) return 1;
  int n = 0;
  lua_pushnil(L);
  while (lua_next(L, 2) != 0) {
    int v = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushvalue(L, 1);
    if (UserUnit* u = ToUnit(L, v)) lua_pushstring(L, u->view.blueprint.c_str());
    else lua_pushvalue(L, v);
    bool member = lua_pcall(L, 2, 1, 0) == 0 && lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    if (member == keepMembers) {
      lua_pushvalue(L, v);
      lua_rawseti(L, 3, ++n);
    }
    lua_pop(L, 1);
  }
  return 1;
}
int l_EntityCategoryFilterDown(lua_State* L) { return FilterUnits(L, true); }
int l_EntityCategoryFilterOut(lua_State* L) { return FilterUnits(L, false); }

// --- a unit's methods ---

int l_GetEntityId(lua_State* L) {
  lua_pushstring(L, std::to_string(CheckUnit(L, 1)->view.id).c_str());
  return 1;
}
int l_GetUnitId(lua_State* L) {
  lua_pushstring(L, CheckUnit(L, 1)->view.blueprint.c_str());
  return 1;
}
int l_GetBlueprint(lua_State* L) {
  PushBlueprint(L, CheckUnit(L, 1)->view.blueprint);
  return 1;
}
int l_GetArmy(lua_State* L) {
  lua_pushnumber(L, CheckUnit(L, 1)->view.army);
  return 1;
}
int l_GetPosition(lua_State* L) {
  const UnitView& v = CheckUnit(L, 1)->view;
  PushVector(L, v.position[0], v.position[1], v.position[2]);
  return 1;
}
int l_GetHealth(lua_State* L) {
  lua_pushnumber(L, CheckUnit(L, 1)->view.health);
  return 1;
}
int l_GetMaxHealth(lua_State* L) {
  lua_pushnumber(L, CheckUnit(L, 1)->view.maxHealth);
  return 1;
}
int l_GetWorkProgress(lua_State* L) {
  lua_pushnumber(L, CheckUnit(L, 1)->view.workProgress);
  return 1;
}
int l_IsDead(lua_State* L) {
  UserUnit* u = ToUnit(L, 1);  // a unit that is gone is dead, and no error
  lua_pushboolean(L, !u || u->gone || u->view.dead);
  return 1;
}
int l_IsIdle(lua_State* L) {
  lua_pushboolean(L, CheckUnit(L, 1)->view.queueLength == 0);
  return 1;
}
int l_IsInCategory(lua_State* L) {
  UserUnit* u = CheckUnit(L, 1);
  lua_pushboolean(L, InCategory(L, u, luaL_checkstring(L, 2)));
  return 1;
}
int l_UnitFalse(lua_State* L) {
  CheckUnit(L, 1);
  lua_pushboolean(L, 0);
  return 1;
}
int l_UnitNil(lua_State* L) {
  CheckUnit(L, 1);
  lua_pushnil(L);
  return 1;
}
int l_UnitZero(lua_State* L) {
  CheckUnit(L, 1);
  lua_pushnumber(L, 0);
  return 1;
}
int l_UnitEmptyTable(lua_State* L) {
  CheckUnit(L, 1);
  lua_newtable(L);
  return 1;
}
// GetFuelRatio(): -1 for a unit without fuel
int l_GetFuelRatio(lua_State* L) {
  CheckUnit(L, 1);
  lua_pushnumber(L, -1);
  return 1;
}
int l_GetBuildRate(lua_State* L) {
  PushBlueprint(L, CheckUnit(L, 1)->view.blueprint);
  lua_Number rate = 0;
  if (lua_istable(L, -1)) {
    lua_pushstring(L, "Economy");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
      lua_pushstring(L, "BuildRate");
      lua_gettable(L, -2);
      rate = lua_tonumber(L, -1);
    }
  }
  lua_pushnumber(L, rate);
  return 1;
}
int l_GetCustomName(lua_State* L) {
  UserUnit* u = CheckUnit(L, 1);
  if (u->customName.empty()) lua_pushnil(L);
  else lua_pushstring(L, u->customName.c_str());
  return 1;
}
int l_SetCustomName(lua_State* L) {
  CheckUnit(L, 1)->customName = lua_isstring(L, 2) ? lua_tostring(L, 2) : "";
  return 0;
}
// GetStat(name [, default]): a statistic the unit's script keeps; {Value = default} while the
// interface gets none of them.
int l_GetStat(lua_State* L) {
  CheckUnit(L, 1);
  bool hasDefault = lua_gettop(L) >= 3;
  lua_newtable(L);
  lua_pushstring(L, "Value");
  if (hasDefault) lua_pushvalue(L, 3);
  else lua_pushnumber(L, 0);
  lua_rawset(L, -3);
  return 1;
}
// GetEconData(): what the unit makes and uses; zeros while the interface gets none of it.
int l_GetEconData(lua_State* L) {
  CheckUnit(L, 1);
  lua_newtable(L);
  for (const char* key : {"energyConsumed", "energyProduced", "energyRequested", "massConsumed", "massProduced", "massRequested"}) {
    lua_pushstring(L, key);
    lua_pushnumber(L, 0);
    lua_rawset(L, -3);
  }
  return 1;
}
int l_GetMissileInfo(lua_State* L) {
  CheckUnit(L, 1);
  lua_newtable(L);
  for (const char* key : {"nukeSiloBuildCount", "nukeSiloMaxStorageCount", "nukeSiloStorageCount", "tacticalSiloBuildCount",
                          "tacticalSiloMaxStorageCount", "tacticalSiloStorageCount"}) {
    lua_pushstring(L, key);
    lua_pushnumber(L, 0);
    lua_rawset(L, -3);
  }
  return 1;
}
int l_GetSelectionSets(lua_State* L) {
  UserUnit* u = CheckUnit(L, 1);
  lua_newtable(L);
  int n = 0;
  for (const std::string& s : u->selectionSets) {
    lua_pushstring(L, s.c_str());
    lua_rawseti(L, -2, ++n);
  }
  return 1;
}
int l_AddSelectionSet(lua_State* L) {
  UserUnit* u = CheckUnit(L, 1);
  std::string s = luaL_checkstring(L, 2);
  if (std::find(u->selectionSets.begin(), u->selectionSets.end(), s) == u->selectionSets.end()) u->selectionSets.push_back(s);
  return 0;
}
int l_RemoveSelectionSet(lua_State* L) {
  UserUnit* u = CheckUnit(L, 1);
  std::string s = luaL_checkstring(L, 2);
  u->selectionSets.erase(std::remove(u->selectionSets.begin(), u->selectionSets.end(), s), u->selectionSets.end());
  return 0;
}
int l_HasSelectionSet(lua_State* L) {
  UserUnit* u = CheckUnit(L, 1);
  std::string s = luaL_checkstring(L, 2);
  lua_pushboolean(L, std::find(u->selectionSets.begin(), u->selectionSets.end(), s) != u->selectionSets.end());
  return 1;
}
int l_UnitNothing(lua_State* L) {
  CheckUnit(L, 1);
  return 0;
}

}  // namespace

// After a beat: the interface's units follow the simulation's.
void RefreshUserUnits(lua_State* L) {
  Sim* sim = TheSim(L);
  if (!sim) return;
  UnitStore& store = Store(L);
  std::vector<UnitView> views;
  UnitViews(*sim, &views);
  for (auto& [id, u] : store.units) u->gone = true;
  for (const UnitView& v : views) {
    auto& slot = store.units[v.id];
    if (slot && slot->view.blueprint != v.blueprint) {  // the id went to another unit
      slot->UnbindLua();
      slot.reset();
    }
    if (!slot) {
      slot = std::make_unique<UserUnit>();
      slot->view = v;
      MakeTable(L, slot.get());
    }
    slot->view = v;
    slot->gone = false;
  }
  // Units that are gone leave the selection (the script hears of it) and then the store.
  std::vector<UserUnit*> kept;
  for (UserUnit* u : store.selection)
    if (!u->gone && !u->view.dead) kept.push_back(u);
  SetSelection(L, kept);
  for (auto it = store.units.begin(); it != store.units.end();) {
    if (it->second->gone) {
      it->second->UnbindLua();
      it = store.units.erase(it);
    } else {
      ++it;
    }
  }
}

void RegisterUserUnitBindings(lua_State* L) {
  if (!TheSim(L)) return;
  SetGlobal(L, "GetSelectedUnits", l_GetSelectedUnits);
  SetGlobal(L, "SelectUnits", l_SelectUnits);
  SetGlobal(L, "ValidateUnitsList", l_ValidateUnitsList);
  SetGlobal(L, "GetUnitById", l_GetUnitById);
  SetGlobal(L, "GetArmyAvatars", l_GetArmyAvatars);
  SetGlobal(L, "GetIdleEngineers", l_GetIdleEngineers);
  SetGlobal(L, "GetIdleFactories", l_GetIdleFactories);
  SetGlobal(L, "GetUnitCommandData", l_GetUnitCommandData);
  SetGlobal(L, "GetFireState", l_ListZero);
  for (const char* name : {"GetScriptBit", "GetIsPaused", "GetIsAutoMode", "GetIsAutoSurfaceMode"}) SetGlobal(L, name, l_ListFalse);
  for (const char* name : {"GetAttachedUnitsList", "GetAssistingUnitsList"}) SetGlobal(L, name, l_ListEmpty);

  const char* cls = "UserUnit";
  SetMethod(L, cls, "GetEntityId", l_GetEntityId);
  SetMethod(L, cls, "GetUnitId", l_GetUnitId);
  SetMethod(L, cls, "GetBlueprint", l_GetBlueprint);
  SetMethod(L, cls, "GetArmy", l_GetArmy);
  SetMethod(L, cls, "GetPosition", l_GetPosition);
  SetMethod(L, cls, "GetHealth", l_GetHealth);
  SetMethod(L, cls, "GetMaxHealth", l_GetMaxHealth);
  SetMethod(L, cls, "GetWorkProgress", l_GetWorkProgress);
  SetMethod(L, cls, "IsDead", l_IsDead);
  SetMethod(L, cls, "IsIdle", l_IsIdle);
  SetMethod(L, cls, "IsInCategory", l_IsInCategory);
  SetMethod(L, cls, "GetFuelRatio", l_GetFuelRatio);
  SetMethod(L, cls, "GetShieldRatio", l_UnitZero);
  SetMethod(L, cls, "GetBuildRate", l_GetBuildRate);
  SetMethod(L, cls, "GetCustomName", l_GetCustomName);
  SetMethod(L, cls, "SetCustomName", l_SetCustomName);
  SetMethod(L, cls, "GetStat", l_GetStat);
  SetMethod(L, cls, "GetEconData", l_GetEconData);
  SetMethod(L, cls, "GetMissileInfo", l_GetMissileInfo);
  SetMethod(L, cls, "GetSelectionSets", l_GetSelectionSets);
  SetMethod(L, cls, "AddSelectionSet", l_AddSelectionSet);
  SetMethod(L, cls, "RemoveSelectionSet", l_RemoveSelectionSet);
  SetMethod(L, cls, "HasSelectionSet", l_HasSelectionSet);
  SetMethod(L, cls, "GetCommandQueue", l_UnitEmptyTable);
  for (const char* name : {"GetFocus", "GetCreator", "GetGuardedEntity"}) SetMethod(L, cls, name, l_UnitNil);
  for (const char* name : {"IsAutoMode", "IsAutoSurfaceMode", "IsOverchargePaused", "IsRepeatQueue", "IsStunned",
                           "HasUnloadCommandQueuedUp", "CanAttackTarget"})
    SetMethod(L, cls, name, l_UnitFalse);
  SetMethod(L, cls, "ProcessInfo", l_UnitNothing);
}

// In front of the engine's category functions: call this once those are registered.
void InstallUnitCategoryFunctions(lua_State* L) {
  for (auto [name, fn] : {std::pair<const char*, lua_CFunction>{"EntityCategoryContains", l_EntityCategoryContains},
                          {"EntityCategoryFilterDown", l_EntityCategoryFilterDown},
                          {"EntityCategoryFilterOut", l_EntityCategoryFilterOut}}) {
    lua_getglobal(L, "EntityCategoryContains");
    if (lua_isfunction(L, -1)) {
      lua_pushcclosure(L, fn, 1);
      lua_setglobal(L, name);
    } else {
      lua_pop(L, 1);
    }
  }
}

}  // namespace moho::ui
