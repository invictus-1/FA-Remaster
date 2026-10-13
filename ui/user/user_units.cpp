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
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/log.h"
#include "maui/world_source.h"
#include "script/script_handle.h"
#include "script/script_state.h"
#include "sim/blueprints.h"
#include "sim/command_writer.h"
#include "sim/session.h"
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
  std::unique_ptr<CommandWriter> writer;  // the player's orders since the last beat
  uint32_t queueFactory = 0xffffffffu;    // the factory whose queue the construction panel shows
  std::string queueKey;                   // that queue as it was last handed to the script
  // The buildings the player's units have been ordered to build and have not begun, as of beat
  // `orderedTick` (worked out once a beat, for the world's picture).
  std::vector<WorldGhost> ordered;
  uint32_t orderedTick = 0xffffffffu;
};

UnitStore& Store(lua_State* L) {
  UserState* user = UserState::From(L);
  if (!user->units) user->units = std::make_unique<UnitStore>();
  return static_cast<UnitStore&>(*user->units);
}
Sim* TheSim(lua_State* L) { return UserState::From(L)->config().sim; }
bool StateOf(lua_State* L, const UserUnit* u, UnitStateView* out);  // further down: a unit's state from the simulation

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

// AddSelectUnits(units) (FA exe 0x8bdc30): these units join the selection.
int l_AddSelectUnits(lua_State* L) {
  std::vector<UserUnit*> now = Store(L).selection;
  ReadUnitList(L, 1, &now);
  SetSelection(L, now);
  return 0;
}

// Whether a unit is in the original's state "BeingUpgraded" (0x25): the building that an upgrade
// is putting up in place of the old one. INFERRED from what the engine's view gives: unfinished,
// a structure, and made by a structure (a factory's units are mobile).
bool BeingUpgraded(lua_State* L, Sim* sim, const UserUnit* u) {
  if (!u->view.beingBuilt || !InCategory(L, u, "STRUCTURE")) return false;
  UnitStateView s;
  if (!UnitStateOf(*sim, u->view.id, &s) || s.creator == 0xffffffffu) return false;
  auto it = Store(L).units.find(s.creator);
  return it != Store(L).units.end() && !it->second->gone && InCategory(L, it->second.get(), "STRUCTURE");
}

// The box around a unit for the camera: the original takes the model's box; here the
// blueprint's SizeX and SizeZ about the unit's place.
void UnitBox(lua_State* L, const UserUnit* u, float lo[3], float hi[3]) {
  float sx = 1, sz = 1;
  int top = lua_gettop(L);
  PushBlueprint(L, u->view.blueprint);
  if (lua_istable(L, -1)) {
    lua_pushstring(L, "SizeX");
    lua_rawget(L, -2);
    if (lua_isnumber(L, -1)) sx = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_pushstring(L, "SizeZ");
    lua_rawget(L, -2);
    if (lua_isnumber(L, -1)) sz = static_cast<float>(lua_tonumber(L, -1));
  }
  lua_settop(L, top);
  const float* p = u->view.position;
  lo[0] = p[0] - sx * 0.5f, lo[1] = p[1], lo[2] = p[2] - sz * 0.5f;
  hi[0] = p[0] + sx * 0.5f, hi[1] = p[1], hi[2] = p[2] + sz * 0.5f;
}
// The world camera shows one unit: its box and 20 more on every side across (the camera's slot
// 11, FA exe 0x7a8580).
void ShowUnit(lua_State* L, const UserUnit* u, float seconds) {
  float lo[3], hi[3];
  UnitBox(L, u, lo, hi);
  lo[0] -= 20, lo[2] -= 20, hi[0] += 20, hi[2] += 20;
  WorldCameraShowBox(L, lo, hi, seconds);
}

// UISelectionByCategory(expression, addToCurSel, inViewFrustum, nearestToMouse, mustBeIdle)
// (FA exe 0x866db0) and the console command UI_SelectByCategory (0x866a30), the work of both in
// 0x8662b0: the keys for "select all engineers", "nearest idle engineer", "all on screen" and
// their like. As read there:
// - the units asked: those the world camera has in view, or all;
// - of them the player's own selectable ones in the category; with mustBeIdle only finished
//   ones with no orders (the unit's byte +0x1a2 clear and its command list empty);
// - with nearestToMouse only the one nearest to the place of the world under the mouse;
//   without it every one that is not a building being upgraded (unit state 0x25);
// - with addToCurSel they join the selection, else they are the selection (none found: nothing
//   is selected);
// - the command's two more: without engineers and commanders (+excludeengineers), and the world
//   camera goes to what was selected (+goto: one unit as UISelectAndZoomTo shows it, several by
//   the box around them all). With +idle the original's camera goes from one idle unit to the
//   next with each use (0x865590, not read): here it goes to the selection as without.
// False when the expression cannot be read.
bool SelectUnitsByCategory(lua_State* L, const char* expr, bool add, bool inView, bool nearest, bool idle, bool goTo,
                           bool noEngineers) {
  Sim* sim = TheSim(L);
  std::vector<uint64_t> bits;
  SimBlueprints* bps = SimBlueprints::From(L);
  if (!sim || !bps || !ParseCategory(L, expr, bits)) return false;
  UnitStore& store = Store(L);
  std::vector<UserUnit*> asked;
  std::vector<uint32_t> ids;
  if (inView && UnitsOnScreen(L, &ids)) {
    std::sort(ids.begin(), ids.end());
    for (uint32_t id : ids) {
      auto it = store.units.find(id);
      if (it != store.units.end()) asked.push_back(it->second.get());
    }
  } else {
    for (auto& [id, u] : store.units)
      if (!inView || PlaceInCameraView(L, u->view.position)) asked.push_back(u.get());
  }
  std::vector<UserUnit*> now;
  if (add) now = store.selection;
  float mouse[3] = {0, 0, 0};
  if (nearest) MouseWorldPlace(L, mouse);
  UserUnit* best = nullptr;
  float bestDistance = std::numeric_limits<float>::max();
  for (UserUnit* u : asked) {
    if (u->gone || u->view.dead || !u->view.selectable || u->view.army != sim->focusArmy + 1) continue;
    if (idle && (u->view.beingBuilt || u->view.queueLength != 0)) continue;
    const BlueprintInfo* bp = bps->Find(u->view.blueprint);
    if (!bp || bp->entityIndex < 0 || static_cast<size_t>(bp->entityIndex >> 6) >= bits.size() ||
        !(bits[bp->entityIndex >> 6] >> (bp->entityIndex & 63) & 1))
      continue;
    if (noEngineers && (InCategory(L, u, "ENGINEER") || InCategory(L, u, "COMMAND"))) continue;
    if (nearest) {
      float dx = u->view.position[0] - mouse[0], dy = u->view.position[1] - mouse[1], dz = u->view.position[2] - mouse[2];
      float d = std::sqrt(dx * dx + dy * dy + dz * dz);
      if (d < bestDistance) bestDistance = d, best = u;
    } else if (!BeingUpgraded(L, sim, u) && std::find(now.begin(), now.end(), u) == now.end()) {
      now.push_back(u);
    }
  }
  if (best && std::find(now.begin(), now.end(), best) == now.end()) now.push_back(best);
  if (goTo && now.size() == 1) {
    ShowUnit(L, now[0], 0);
  } else if (goTo && !now.empty()) {
    float lo[3], hi[3];
    for (size_t i = 0; i < now.size(); ++i) {
      float a[3], b[3];
      UnitBox(L, now[i], a, b);
      for (int k = 0; k < 3; ++k) lo[k] = i ? std::min(lo[k], a[k]) : a[k], hi[k] = i ? std::max(hi[k], b[k]) : b[k];
    }
    WorldCameraShowBox(L, lo, hi, 0);
  }
  SetSelection(L, now);
  return true;
}
int l_UISelectionByCategory(lua_State* L) {
  const char* expr = luaL_checkstring(L, 1);
  if (!SelectUnitsByCategory(L, expr, lua_toboolean(L, 2) != 0, lua_toboolean(L, 3) != 0, lua_toboolean(L, 4) != 0,
                             lua_toboolean(L, 5) != 0, false, false))
    return luaL_error(L, "Invalid category expression: %s", expr);
  return 0;
}

// UIZoomTo(units, [seconds]) (FA exe 0x867240): the world camera shows these units. The box it
// is given: across, from the least to the greatest x and z of the units' places, 20 more on
// every side; in height, their mean height with half the larger of the two spans and 20 above
// and below. (So one unit alone is seen from a zoom of 40.)
int l_UIZoomTo(lua_State* L) {
  std::vector<UserUnit*> list;
  ReadUnitList(L, 1, &list);
  if (list.empty()) return 0;
  float lo[3], hi[3], height = 0;
  for (size_t i = 0; i < list.size(); ++i) {
    const float* p = list[i]->view.position;
    for (int k = 0; k < 3; k += 2) {
      lo[k] = i ? std::min(lo[k], p[k]) : p[k];
      hi[k] = i ? std::max(hi[k], p[k]) : p[k];
    }
    height += p[1];
  }
  height /= static_cast<float>(list.size());
  float half = std::max(hi[0] - lo[0], hi[2] - lo[2]) * 0.5f;
  lo[0] -= 20, lo[2] -= 20, hi[0] += 20, hi[2] += 20;
  lo[1] = height - half - 20, hi[1] = height + half + 20;
  WorldCameraShowBox(L, lo, hi, lua_isnumber(L, 2) ? static_cast<float>(lua_tonumber(L, 2)) : 0);
  return 0;
}

// UISelectAndZoomTo(unit, [seconds]) (FA exe 0x866fc0): the unit becomes the selection and the
// world camera shows it.
int l_UISelectAndZoomTo(lua_State* L) {
  UserUnit* u = ToUnit(L, 1);
  if (!u || u->gone) return 0;
  SetSelection(L, {u});
  ShowUnit(L, u, lua_isnumber(L, 2) ? static_cast<float>(lua_tonumber(L, 2)) : 0);
  return 0;
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
  lua_pushnil(L);   // 4: buildable category (made at the end, from `total`)
  std::vector<uint64_t> total(static_cast<size_t>(CategoryWordCount()), 0);
  // In the blueprints as the engine keeps them, a set of orders or toggles is a bit set written
  // as text: the one name when a single bit is set, else the number.
  static const char* const kOrders[] = {"RULEUCC_Move", "RULEUCC_Stop", "RULEUCC_Attack", "RULEUCC_Guard", "RULEUCC_Patrol",
                                        "RULEUCC_RetaliateToggle", "RULEUCC_Repair", "RULEUCC_Capture", "RULEUCC_Transport",
                                        "RULEUCC_CallTransport", "RULEUCC_Nuke", "RULEUCC_Tactical", "RULEUCC_Teleport",
                                        "RULEUCC_Ferry", "RULEUCC_SiloBuildTactical", "RULEUCC_SiloBuildNuke",
                                        "RULEUCC_Sacrifice", "RULEUCC_Pause", "RULEUCC_Overcharge", "RULEUCC_Dive",
                                        "RULEUCC_Reclaim", "RULEUCC_SpecialAction", "RULEUCC_Dock", "RULEUCC_Script"};
  static const char* const kToggles[] = {"RULEUTC_ShieldToggle", "RULEUTC_WeaponToggle", "RULEUTC_JammingToggle",
                                         "RULEUTC_IntelToggle", "RULEUTC_ProductionToggle", "RULEUTC_StealthToggle",
                                         "RULEUTC_GenericToggle", "RULEUTC_SpecialToggle", "RULEUTC_CloakToggle"};
  uint32_t masks[4] = {0, 0, 0, 0};  // the bits for the tables at stack index 2 (orders) and 3 (toggles)
  auto collect = [&](const char* caps, int out, const char* const* names, int count) {
    lua_pushstring(L, "General");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
      lua_pushstring(L, caps);
      lua_gettable(L, -2);
      if (lua_isstring(L, -1)) {
        std::string text = lua_tostring(L, -1);
        bool named = false;
        for (int i = 0; i < count; ++i)
          if (text == names[i]) masks[out] |= 1u << i, named = true;
        if (!named) masks[out] |= static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 10));
      } else if (lua_istable(L, -1)) {  // the form of the blueprint files: {NAME = true, ...}
        for (int i = 0; i < count; ++i) {
          lua_pushstring(L, names[i]);
          lua_rawget(L, -2);
          if (lua_toboolean(L, -1)) masks[out] |= 1u << i;
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
    UnitStateView state;
    if (StateOf(L, u, &state)) {  // what the unit can do right now
      masks[2] |= state.commandCaps;
      masks[3] |= state.toggleCaps;
    } else {
      collect("CommandCaps", 2, kOrders, 24);
      collect("ToggleCaps", 3, kToggles, 9);
    }
    // What it can build: the sets its blueprint names, less what the unit itself may not build now
    // (its script's AddBuildRestriction: a commander without its upgrades, a factory before its
    // own; the engine's UnitRestrictedBlueprints).
    std::vector<uint64_t> mine(total.size(), 0), part;
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
          if (ParseCategory(L, lua_tostring(L, -1), part))
            for (size_t w = 0; w < mine.size() && w < part.size(); ++w) mine[w] |= part[w];
          lua_pop(L, 1);
        }
      }
      lua_pop(L, 1);
    }
    lua_pop(L, 2);
    if (Sim* sim = TheSim(L)) {
      for (const std::string& id : UnitRestrictedBlueprints(*sim, u->view.id))
        if (ParseCategory(L, id.c_str(), part))
          for (size_t w = 0; w < mine.size() && w < part.size(); ++w) mine[w] &= ~part[w];
    }
    for (size_t w = 0; w < total.size(); ++w) total[w] |= mine[w];
  }
  for (int i = 0, n = 0; i < 24; ++i)
    if (masks[2] & (1u << i)) {
      lua_pushstring(L, kOrders[i]);
      lua_rawseti(L, 2, ++n);
    }
  for (int i = 0, n = 0; i < 9; ++i)
    if (masks[3] & (1u << i)) {
      lua_pushstring(L, kToggles[i]);
      lua_rawseti(L, 3, ++n);
    }
  // What the player's army may not build is taken off (the engine's list of restricted blueprints).
  if (Sim* sim = TheSim(L)) {
    std::vector<uint64_t> part;
    for (const std::string& id : RestrictedBlueprints(*sim, sim->focusArmy + 1))
      if (ParseCategory(L, id.c_str(), part))
        for (size_t w = 0; w < total.size() && w < part.size(); ++w) total[w] &= ~part[w];
  }
  // (Always a category, also when nothing can be built: the game's script hands it to
  // EntityCategoryContains without looking, gamemain.lua 658.)
  uint64_t* words = PushCategory(L);
  for (size_t w = 0; w < total.size(); ++w) words[w] = total[w];
  lua_replace(L, 4);
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

// --- orders ---
//
// The player's orders are written as the command stream's messages (the engine's CommandWriter)
// and handed to the host, which applies them before the next beat.

CommandWriter& Writer(lua_State* L) {
  UnitStore& store = Store(L);
  if (!store.writer) store.writer = std::make_unique<CommandWriter>(UserState::From(L)->config().commandSource);
  return *store.writer;
}

std::string TakeOrders(lua_State* L) {
  UserState* user = UserState::From(L);
  if (!user->units) return std::string();
  UnitStore& store = static_cast<UnitStore&>(*user->units);
  return store.writer ? store.writer->Take() : std::string();
}

void UserUnitIds(lua_State* L, bool selectedOnly, std::vector<uint32_t>* ids) {
  UserState* user = UserState::From(L);
  if (!user->units) return;
  UnitStore& store = static_cast<UnitStore&>(*user->units);
  if (selectedOnly) {
    for (UserUnit* u : store.selection)
      if (!u->gone && !u->view.dead) ids->push_back(u->view.id);
  } else {
    for (auto& [id, u] : store.units)
      if (!u->gone && !u->view.dead) ids->push_back(id);
  }
}

bool UserUnitFacts(lua_State* L, uint32_t id, std::string* blueprint, float position[3], int* army) {
  UserState* user = UserState::From(L);
  if (!user->units) return false;
  UnitStore& store = static_cast<UnitStore&>(*user->units);
  auto it = store.units.find(id);
  if (it == store.units.end() || it->second->gone || it->second->view.dead) return false;
  *blueprint = it->second->view.blueprint;
  for (int i = 0; i < 3; ++i) position[i] = it->second->view.position[i];
  *army = it->second->view.army;
  return true;
}

void RequestSessionEnd(lua_State* L) {
  if (TheSim(L)) Writer(L).EndGame();
}

void RequestSessionPause(lua_State* L, bool pause) {
  if (!TheSim(L)) return;
  if (pause) Writer(L).RequestPause();
  else Writer(L).Resume();
}

// commandmode.OnCommandIssued(command) (FA exe 0x83e770): the script hears of every order the
// engine issued for the player, as a table. First version of that table: CommandType (the
// order's name), Clear, Target = {Type = "Position", Position} or {Type = "Entity", EntityId,
// Position}, Units, Blueprint.
void ReportCommandIssued(lua_State* L, const char* type, const std::vector<UserUnit*>& units, float x, float y, float z,
                         bool clear, const std::string& blueprint, uint32_t entity = 0xffffffffu) {
  int top = lua_gettop(L);
  lua_pushcfunction(L, ScriptTraceback);
  lua_getglobal(L, "import");
  lua_pushstring(L, "/lua/ui/game/commandmode.lua");
  if (lua_pcall(L, 1, 1, top + 1) == 0 && lua_istable(L, -1)) {
    lua_pushstring(L, "OnCommandIssued");
    lua_gettable(L, -2);
    if (lua_isfunction(L, -1)) {
      lua_newtable(L);
      lua_pushstring(L, "CommandType");
      lua_pushstring(L, type);
      lua_rawset(L, -3);
      lua_pushstring(L, "Clear");
      lua_pushboolean(L, clear);
      lua_rawset(L, -3);
      lua_pushstring(L, "Blueprint");
      if (blueprint.empty()) lua_pushnil(L);
      else lua_pushstring(L, blueprint.c_str());
      lua_rawset(L, -3);
      lua_pushstring(L, "Units");
      PushUnitList(L, units, false);
      lua_rawset(L, -3);
      lua_pushstring(L, "Target");
      lua_newtable(L);
      lua_pushstring(L, "Type");
      lua_pushstring(L, entity != 0xffffffffu ? "Entity" : "Position");
      lua_rawset(L, -3);
      if (entity != 0xffffffffu) {
        lua_pushstring(L, "EntityId");
        lua_pushstring(L, std::to_string(entity).c_str());
        lua_rawset(L, -3);
      }
      lua_pushstring(L, "Position");
      PushVector(L, x, y, z);
      lua_rawset(L, -3);
      lua_rawset(L, -3);
      if (lua_pcall(L, 1, 0, top + 1) != 0)
        Logf(LogLevel::Warning, "Error running '/lua/ui/game/commandmode.lua:OnCommandIssued': %s",
             lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
    }
  }
  lua_settop(L, top);
}

namespace {
// An order for the selected units of the player's army, with a place or a unit as its target.
bool IssueOrder(lua_State* L, const std::string& order, float x, float y, float z, uint32_t entity, bool clear,
                const std::string& blueprint) {
  Sim* sim = TheSim(L);
  if (!sim) return false;
  static const struct {
    const char* name;
    CommandType type;
    bool atUnit;  // takes a unit as its target only
  } kOrders[] = {{"Move", CommandType::Move, false},
                 {"Attack", CommandType::Attack, false},
                 {"Patrol", CommandType::Patrol, false},
                 {"AggressiveMove", CommandType::AggressiveMove, false},
                 {"BuildMobile", CommandType::BuildMobile, false},
                 {"Guard", CommandType::Guard, true},
                 {"Repair", CommandType::Repair, true},
                 {"Reclaim", CommandType::Reclaim, true},
                 {"Capture", CommandType::Capture, true}};
  const char* name = nullptr;
  CommandType type = CommandType::None;
  for (const auto& o : kOrders)
    if (order == o.name && (entity != 0xffffffffu || !o.atUnit)) name = o.name, type = o.type;
  if (!name) return false;
  std::vector<uint32_t> ids;
  std::vector<UserUnit*> units;
  for (UserUnit* u : Store(L).selection)
    if (!u->gone && !u->view.dead && u->view.army == sim->focusArmy + 1) ids.push_back(u->view.id), units.push_back(u);
  if (ids.empty()) return false;
  StreamIssueData d;
  d.type = static_cast<int>(type);
  d.target.type = entity != 0xffffffffu ? 1 : 2;  // a unit, or a position
  d.target.entity = entity != 0xffffffffu ? entity : d.target.entity;
  d.target.pos.x = x, d.target.pos.y = y, d.target.pos.z = z;
  d.blueprint = blueprint;
  // An order given with a factory selected is the factory's rally order: what the units it builds
  // are told to do (the command stream's IssueFactoryCommand, FA exe 0x6f14d0), kept in a queue
  // of its own. INFERRED: which units count (here: buildings that are factories; a carrier moves
  // itself) and that every order of the world's clicks but a building order goes that way; where
  // the original's interface decides is not read.
  std::vector<uint32_t> factories, others;
  for (UserUnit* u : units)
    (type != CommandType::BuildMobile && InCategory(L, u, "FACTORY") && InCategory(L, u, "STRUCTURE") ? factories : others)
        .push_back(u->view.id);
  if (!others.empty()) Writer(L).IssueCommand(others, d, clear);
  if (!factories.empty()) Writer(L).IssueFactoryCommand(factories, d, clear);
  ReportCommandIssued(L, name, units, x, y, z, clear, blueprint, entity);
  return true;
}
}  // namespace

bool IssueToSelection(lua_State* L, const std::string& order, float x, float y, float z, bool clear, const std::string& blueprint) {
  return IssueOrder(L, order, x, y, z, 0xffffffffu, clear, blueprint);
}

// What the player's army knows of a unit now (the engine's ReconOf bits; its own and its allies'
// units: everything). The picture shows a unit by the same answer: the unit while it is seen, a
// blip while it is on radar, sonar or omni alone, nothing otherwise.
uint32_t ReconBitsOf(lua_State* L, const UserUnit* u) {
  Sim* sim = TheSim(L);
  if (!sim || sim->focusArmy < 0) return 0xf;
  return ReconOf(*sim, sim->focusArmy + 1, u->view.id);
}

int HoverKindOf(lua_State* L, uint32_t entity) {
  Sim* sim = TheSim(L);
  UserState* user = UserState::From(L);
  if (!sim || !user->units) return 0;
  UnitStore& store = static_cast<UnitStore&>(*user->units);
  auto it = store.units.find(entity);
  if (it == store.units.end() || it->second->gone || it->second->view.dead) return 0;
  UserUnit* u = it->second.get();
  if (std::find(store.selection.begin(), store.selection.end(), u) != store.selection.end()) return 0;
  int army = sim->focusArmy + 1;
  if (u->view.army == army) return 1;
  int stance = 1;  // neutral unless the armies say otherwise
  for (const ArmyView& v : ArmyViews(*sim))
    if (v.index == u->view.army && army >= 1 && army <= static_cast<int>(v.alliance.size())) stance = v.alliance[static_cast<size_t>(army - 1)];
  return stance == 2 ? 1 : stance == 0 ? 2 : 3;
}

uint32_t UnitNearPlace(lua_State* L, float x, float z, float radius) {
  UserState* user = UserState::From(L);
  if (!user->units) return 0xffffffffu;
  uint32_t best = 0xffffffffu;
  float bestD = radius * radius;
  for (const auto& entry : static_cast<UnitStore&>(*user->units).units) {
    const UnitView& v = entry.second->view;
    if (entry.second->gone || v.dead) continue;
    if (ReconBitsOf(L, entry.second.get()) == 0) continue;  // the player's army knows nothing of it
    float d = (v.position[0] - x) * (v.position[0] - x) + (v.position[2] - z) * (v.position[2] - z);
    if (d <= bestD) best = v.id, bestD = d;
  }
  return best;
}

bool IssueToSelectionAt(lua_State* L, const std::string& order, uint32_t entity, bool clear) {
  UnitStore& store = Store(L);
  auto it = store.units.find(entity);
  if (it == store.units.end() || it->second->gone) return false;
  const float* p = it->second->view.position;
  return IssueOrder(L, order, p[0], p[1], p[2], entity, clear, std::string());
}

// What a right click orders (func_GetRightMouseButtonAction, FA exe 0x81ec00), by the unit under
// the cursor and by what the selected units can do between them (their command bits together):
//   an enemy: Attack if they can attack (and it lives);
//   else an enemy, or a thing of no army, or one of the category RECLAIMFRIENDLY: Reclaim, if it
//     can be reclaimed (the category RECLAIMABLE, or it is still being built) and they can;
//   else, not an enemy: Repair a unit that is being built (a building of the categories FACTORY
//     or SILO left out); else Guard, unless the selection is that unit alone or it is a
//     CAMPAIGNGATE; else Repair an ally's damaged unit or one that is upgrading;
//   else Move.
// Left out so far: Capture (the original asks a flag of the unit, +0x69 of its attributes, that
// the simulation's view does not give; without it engineers reclaim, as players know it), the
// transport and ferry cases (0x81d280, 0x81d660, 0x81da20), whether the selection's weapons reach
// the target's layer (CanAttackTarget 0x8c0d30: when they do not, the original gives no order),
// an attack order's marker under the cursor, the option "switch_right_click_behavior", and things
// that are not units (wreckage, trees): the picture answers for units only.
std::string RightClickOrder(lua_State* L, uint32_t under) {
  Sim* sim = TheSim(L);
  if (!sim) return std::string();
  UnitStore& store = Store(L);
  int army = sim->focusArmy + 1;
  uint32_t caps = 0;
  std::vector<UserUnit*> own;
  for (UserUnit* u : store.selection) {
    if (u->gone || u->view.dead || u->view.army != army) continue;
    UnitStateView state;
    if (StateOf(L, u, &state)) caps |= state.commandCaps;
    own.push_back(u);
  }
  if (own.empty()) return std::string();
  const uint32_t kMove = 1, kAttack = 4, kGuard = 8, kRepair = 0x40, kReclaim = 0x100000;
  const std::string move = caps & kMove ? "RULEUCC_Move" : "";
  auto it = store.units.find(under);
  UserUnit* e = it != store.units.end() && !it->second->gone ? it->second.get() : nullptr;
  if (!e) return move;
  int stance = e->view.army == army ? 2 : 0;
  for (const ArmyView& v : ArmyViews(*sim))
    if (v.index == e->view.army && e->view.army != army && army <= static_cast<int>(v.alliance.size()))
      stance = v.alliance[static_cast<size_t>(army - 1)];
  bool enemy = stance == 0, ally = stance == 2, noArmy = e->view.army <= 0;
  if (enemy && (caps & kAttack) && !e->view.dead) return "RULEUCC_Attack";
  UnitStateView target;
  StateOf(L, e, &target);
  // (The unit's own switch, SetReclaimable, on top of the original's test: a unit a script has
  // made unreclaimable is not offered for reclaim. Inferred, not read.)
  bool reclaimable = (InCategory(L, e, "RECLAIMABLE") || e->view.beingBuilt) && target.reclaimable;
  if ((enemy || noArmy || InCategory(L, e, "RECLAIMFRIENDLY")) && reclaimable && (caps & kReclaim)) return "RULEUCC_Reclaim";
  if (enemy) return move;
  if ((caps & kRepair) && e->view.beingBuilt &&
      (InCategory(L, e, "MOBILE") || !(InCategory(L, e, "FACTORY") || InCategory(L, e, "SILO"))))
    return "RULEUCC_Repair";
  bool alone = own.size() == 1 && own[0] == e;
  if ((caps & kGuard) && !InCategory(L, e, "CAMPAIGNGATE") && !alone) return "RULEUCC_Guard";
  if ((caps & kRepair) && ((ally && e->view.maxHealth > e->view.health) || e->view.headCommand == static_cast<int>(CommandType::Upgrade)))
    return "RULEUCC_Repair";
  return move;
}

namespace {

// --- orders that need no place ---

// The engine's command numbers by the names scripts use ("UNITCOMMAND_Stop" or "Stop").
const struct {
  const char* name;
  CommandType type;
} kCommandNames[] = {
    {"Stop", CommandType::Stop}, {"Move", CommandType::Move}, {"Dive", CommandType::Dive}, {"FormMove", CommandType::FormMove},
    {"BuildSiloTactical", CommandType::BuildSiloTactical}, {"BuildSiloNuke", CommandType::BuildSiloNuke},
    {"BuildFactory", CommandType::BuildFactory}, {"BuildMobile", CommandType::BuildMobile}, {"BuildAssist", CommandType::BuildAssist},
    {"Attack", CommandType::Attack}, {"FormAttack", CommandType::FormAttack}, {"Nuke", CommandType::Nuke},
    {"Tactical", CommandType::Tactical}, {"Teleport", CommandType::Teleport}, {"Guard", CommandType::Guard},
    {"Patrol", CommandType::Patrol}, {"Ferry", CommandType::Ferry}, {"FormPatrol", CommandType::FormPatrol},
    {"Reclaim", CommandType::Reclaim}, {"Repair", CommandType::Repair}, {"Capture", CommandType::Capture},
    {"TransportLoadUnits", CommandType::TransportLoadUnits}, {"TransportReverseLoadUnits", CommandType::TransportReverseLoadUnits},
    {"TransportUnloadUnits", CommandType::TransportUnloadUnits},
    {"TransportUnloadSpecificUnits", CommandType::TransportUnloadSpecificUnits},
    {"DetachFromTransport", CommandType::DetachFromTransport}, {"Upgrade", CommandType::Upgrade}, {"Script", CommandType::Script},
    {"AssistCommander", CommandType::AssistCommander}, {"KillSelf", CommandType::KillSelf}, {"DestroySelf", CommandType::DestroySelf},
    {"Sacrifice", CommandType::Sacrifice}, {"Pause", CommandType::Pause}, {"OverCharge", CommandType::OverCharge},
    {"AggressiveMove", CommandType::AggressiveMove}, {"FormAggressiveMove", CommandType::FormAggressiveMove},
    {"AssistMove", CommandType::AssistMove}, {"SpecialAction", CommandType::SpecialAction}, {"Dock", CommandType::Dock},
};
bool CommandByName(std::string name, CommandType* type, const char** plain) {
  if (name.rfind("UNITCOMMAND_", 0) == 0) name = name.substr(12);
  for (const auto& c : kCommandNames)
    if (name == c.name) {
      *type = c.type;
      *plain = c.name;
      return true;
    }
  return false;
}

// The entity ids of the player's own living units in a list.
void OwnIds(lua_State* L, const std::vector<UserUnit*>& list, std::vector<uint32_t>* ids, std::vector<UserUnit*>* units) {
  int army = TheSim(L)->focusArmy + 1;
  for (UserUnit* u : list)
    if (!u->gone && !u->view.dead && u->view.army == army) ids->push_back(u->view.id), units->push_back(u);
}

// One order without a place for a list of units: `params` (a stack index or 0) is the order's Lua
// value, as a Script order carries one.
bool IssuePlain(lua_State* L, const std::vector<UserUnit*>& list, const std::string& command, int params, bool clear,
                const std::string& blueprint, int count) {
  CommandType type;
  const char* plain;
  if (!CommandByName(command, &type, &plain)) {
    Logf(LogLevel::Warning, "moho64ui: unknown order '%s'", command.c_str());
    return false;
  }
  std::vector<uint32_t> ids;
  std::vector<UserUnit*> units;
  OwnIds(L, list, &ids, &units);
  if (ids.empty()) return false;
  StreamIssueData d;
  d.type = static_cast<int>(type);
  d.blueprint = blueprint;
  d.count = d.maxCount = count;
  if (params && lua_istable(L, params)) SerializeLua(L, params, &d.lua);
  // Scripts issue Stop without saying "clear" and expect the units to stop at once, so a Stop
  // empties the queue. (Inferred: where the original decides that.)
  if (type == CommandType::Stop) clear = true;
  Writer(L).IssueCommand(ids, d, clear);
  ReportCommandIssued(L, plain, units, 0, 0, 0, clear, blueprint);
  return true;
}

// IssueCommand(command [, luaParams [, clear]]): an order for the selected units
int l_IssueCommand(lua_State* L) {
  IssuePlain(L, Store(L).selection, luaL_checkstring(L, 1), lua_istable(L, 2) ? 2 : 0, lua_toboolean(L, 3) != 0, std::string(), 1);
  return 0;
}
// IssueUnitCommand(units, command [, luaParams [, clear]])
int l_IssueUnitCommand(lua_State* L) {
  std::vector<UserUnit*> list;
  ReadUnitList(L, 1, &list);
  IssuePlain(L, list, luaL_checkstring(L, 2), lua_istable(L, 3) ? 3 : 0, lua_toboolean(L, 4) != 0, std::string(), 1);
  return 0;
}
// IssueBlueprintCommand(command, blueprintId, count [, clear]): for the selected units (a factory's
// build order). It goes behind what is queued unless `clear` says otherwise.
int l_IssueBlueprintCommand(lua_State* L) {
  IssuePlain(L, Store(L).selection, luaL_checkstring(L, 1), 0, lua_toboolean(L, 4) != 0, luaL_checkstring(L, 2),
             static_cast<int>(luaL_optnumber(L, 3, 1)));
  return 0;
}
// IssueBlueprintCommandToUnit(unit, command, blueprintId, count [, clear])
int l_IssueBlueprintCommandToUnit(lua_State* L) {
  std::vector<UserUnit*> list;
  if (UserUnit* u = ToUnit(L, 1)) list.push_back(u);
  IssuePlain(L, list, luaL_checkstring(L, 2), 0, lua_toboolean(L, 5) != 0, luaL_checkstring(L, 3), static_cast<int>(luaL_optnumber(L, 4, 1)));
  return 0;
}

// GetUnitCommandFromCommandCap(cap): the order's name for an order cap ("RULEUCC_Stop" ->
// "UNITCOMMAND_Stop"). (Inferred pairs where the two names differ.)
int l_GetUnitCommandFromCommandCap(lua_State* L) {
  std::string cap = luaL_checkstring(L, 1);
  if (cap.rfind("RULEUCC_", 0) == 0) cap = cap.substr(8);
  static const char* const kOther[][2] = {{"Overcharge", "OverCharge"}, {"SiloBuildTactical", "BuildSiloTactical"},
                                          {"SiloBuildNuke", "BuildSiloNuke"}, {"Transport", "TransportUnloadUnits"},
                                          {"CallTransport", "TransportLoadUnits"}, {"RetaliateToggle", "None"}};
  for (const auto& o : kOther)
    if (cap == o[0]) cap = o[1];
  lua_pushstring(L, ("UNITCOMMAND_" + cap).c_str());
  return 1;
}

// SimCallback({Func = name, Args = table} [, addUnitSelection]): the simulation's script runs the
// named callback with the arguments, and with the selected units when asked.
int l_SimCallback(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_pushstring(L, "Func");
  lua_gettable(L, 1);
  std::string name = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
  lua_pop(L, 1);
  lua_pushstring(L, "Args");
  lua_gettable(L, 1);
  std::string args;
  if (!lua_isnil(L, -1)) SerializeLua(L, lua_gettop(L), &args);
  lua_pop(L, 1);
  std::vector<uint32_t> ids;
  std::vector<UserUnit*> units;
  if (lua_toboolean(L, 2)) OwnIds(L, Store(L).selection, &ids, &units);
  if (!name.empty()) Writer(L).LuaSimCallback(name, args, ids);
  return 0;
}

// A switch on every own unit of a list: the simulation is told by key and value.
void InfoPair(lua_State* L, int listIdx, const char* key, const std::string& value) {
  std::vector<UserUnit*> list;
  ReadUnitList(L, listIdx, &list);
  std::vector<uint32_t> ids;
  std::vector<UserUnit*> units;
  OwnIds(L, list, &ids, &units);
  for (uint32_t id : ids) Writer(L).ProcessInfoPair(id, key, value);
}
// SetPaused(units, paused), SetAutoMode(units, on), SetFireState(units, state),
// ToggleFireState(units, currentState), ToggleScriptBit(units, bit, currentState)
int l_SetPaused(lua_State* L) {
  InfoPair(L, 1, "SetPaused", lua_toboolean(L, 2) ? "true" : "false");
  return 0;
}
int l_SetAutoMode(lua_State* L) {
  InfoPair(L, 1, "SetAutoMode", lua_toboolean(L, 2) ? "true" : "false");
  return 0;
}
int l_SetFireState(lua_State* L) {
  InfoPair(L, 1, "SetFireState", std::to_string(static_cast<int>(luaL_checknumber(L, 2))));
  return 0;
}
int l_ToggleFireState(lua_State* L) {  // the next of the three states
  int current = static_cast<int>(luaL_optnumber(L, 2, 0));
  InfoPair(L, 1, "SetFireState", std::to_string(current < 0 ? 0 : (current + 1) % 3));
  return 0;
}
int l_ToggleScriptBit(lua_State* L) {
  InfoPair(L, 1, "ToggleScriptBit", std::to_string(static_cast<int>(luaL_checknumber(L, 2))));
  return 0;
}


// --- unit states (the engine's UnitStateOf, sim/sim_view.h) ---
//
// What the panels show of a unit beyond where it is: its queue, what it works on, its switches
// and meters. Asked of the simulation when a script asks, for the units it asks about.

bool StateOf(lua_State* L, const UserUnit* u, UnitStateView* out) {
  Sim* sim = TheSim(L);
  return sim && u && !u->gone && UnitStateOf(*sim, u->view.id, out);
}

void PushUnitById(lua_State* L, uint32_t id) {
  UnitStore& store = Store(L);
  auto it = id == 0xffffffffu ? store.units.end() : store.units.find(id);
  if (it == store.units.end() || it->second->gone) lua_pushnil(L);
  else PushObject(L, it->second.get());
}

const char* CommandName(int type) {
  for (const auto& c : kCommandNames)
    if (static_cast<int>(c.type) == type) return c.name;
  return "None";
}

// unit:GetCommandQueue(): the unit's orders, first to last, each {ID, type, position}.
// GetCommandQueue() (FA exe 0x8c5480): the unit's orders, each {ID, type, position}. A factory
// gives its rally queue instead (the original asks the unit for its factory command queue first,
// slot +0x20, and takes the unit's own, slot +0x18, only when there is none): the game's script
// places the rally flag by its last entry.
int l_GetCommandQueue(lua_State* L) {
  UnitStateView s;
  lua_newtable(L);
  if (!StateOf(L, CheckUnit(L, 1), &s)) return 1;
  int n = 0;
  // (hasFactoryQueue: the simulation keeps a rally queue for the unit; a new factory has the
  // engine's first rally in it.)
  for (const CommandView& c : s.hasFactoryQueue ? s.factoryQueue : s.queue) {
    lua_newtable(L);
    lua_pushstring(L, "ID");
    lua_pushnumber(L, static_cast<lua_Number>(c.id));
    lua_rawset(L, -3);
    lua_pushstring(L, "type");
    lua_pushstring(L, CommandName(c.type));
    lua_rawset(L, -3);
    if (c.hasPosition) {
      lua_pushstring(L, "position");
      PushVector(L, c.position[0], c.position[1], c.position[2]);
      lua_rawset(L, -3);
    }
    lua_rawseti(L, -2, ++n);
  }
  return 1;
}
template <uint32_t UnitStateView::*Field>
int l_UnitEntity(lua_State* L) {
  UnitStateView s;
  if (StateOf(L, CheckUnit(L, 1), &s)) PushUnitById(L, s.*Field);
  else lua_pushnil(L);
  return 1;
}
template <bool UnitStateView::*Field>
int l_UnitSwitch(lua_State* L) {
  UnitStateView s;
  lua_pushboolean(L, StateOf(L, CheckUnit(L, 1), &s) && s.*Field);
  return 1;
}
int l_GetShieldRatioNow(lua_State* L) {
  UnitStateView s;
  lua_pushnumber(L, StateOf(L, CheckUnit(L, 1), &s) ? s.shieldRatio : 0);
  return 1;
}
int l_GetFuelRatioNow(lua_State* L) {
  UnitStateView s;
  lua_pushnumber(L, StateOf(L, CheckUnit(L, 1), &s) ? s.fuelRatio : -1);
  return 1;
}
int l_GetBuildRateNow(lua_State* L) {
  UnitStateView s;
  if (!StateOf(L, CheckUnit(L, 1), &s)) return l_GetBuildRate(L);
  lua_pushnumber(L, s.buildRate);
  return 1;
}
// unit:GetEconData(): what the unit makes and uses, per second.
int l_GetEconDataNow(lua_State* L) {
  UnitStateView s;
  StateOf(L, CheckUnit(L, 1), &s);
  lua_newtable(L);
  auto set = [&](const char* key, float v) {
    lua_pushstring(L, key);
    lua_pushnumber(L, v);
    lua_rawset(L, -3);
  };
  set("energyProduced", s.produced[0]);
  set("energyConsumed", s.consumed[0]);
  set("energyRequested", s.consumptionPerSecond[0]);
  set("massProduced", s.produced[1]);
  set("massConsumed", s.consumed[1]);
  set("massRequested", s.consumptionPerSecond[1]);
  return 1;
}
int l_GetMissileInfoNow(lua_State* L) {
  UnitStateView s;
  StateOf(L, CheckUnit(L, 1), &s);
  lua_newtable(L);
  auto set = [&](const char* key, int v) {
    lua_pushstring(L, key);
    lua_pushnumber(L, v);
    lua_rawset(L, -3);
  };
  set("nukeSiloStorageCount", s.nukeSiloCount);
  set("tacticalSiloStorageCount", s.tacticalSiloCount);
  set("nukeSiloBuildCount", s.nukeSiloBuildCount);
  set("nukeSiloMaxStorageCount", s.nukeSiloMaxStorage);
  set("tacticalSiloBuildCount", s.tacticalSiloBuildCount);
  set("tacticalSiloMaxStorageCount", s.tacticalSiloMaxStorage);
  return 1;
}

// GetRolloverInfo() (FA exe 0x842920; a unit's table 0x8421f0): what the mouse is over in the
// world, nil for nothing. A unit: blueprintId, health, maxHealth, kills, the economy and silo
// numbers, fuelRatio, shieldRatio, workProgress, userUnit, entityId (a string), armyIndex (from
// 0), teamColor, customName; and `focus`, the same table for what it works on. (The original
// also tells of things that are not units, and of `focusUpgrade` / `focusProp`.)
void PushRollover(lua_State* L, UserUnit* u, bool withFocus) {
  UnitStateView s;
  StateOf(L, u, &s);
  lua_newtable(L);
  auto num = [&](const char* key, lua_Number v) {
    lua_pushstring(L, key);
    lua_pushnumber(L, v);
    lua_rawset(L, -3);
  };
  auto str = [&](const char* key, const std::string& v) {
    lua_pushstring(L, key);
    lua_pushstring(L, v.c_str());
    lua_rawset(L, -3);
  };
  str("blueprintId", u->view.blueprint);
  num("health", u->view.health);
  num("maxHealth", u->view.maxHealth);
  num("kills", s.kills);
  num("energyConsumed", s.consumed[0]);
  num("massConsumed", s.consumed[1]);
  num("energyRequested", s.consumptionPerSecond[0]);
  num("massRequested", s.consumptionPerSecond[1]);
  num("energyProduced", s.produced[0]);
  num("massProduced", s.produced[1]);
  num("tacticalSiloBuildCount", s.tacticalSiloBuildCount);
  num("tacticalSiloStorageCount", s.tacticalSiloCount);
  num("tacticalSiloMaxStorageCount", s.tacticalSiloMaxStorage);
  num("nukeSiloBuildCount", s.nukeSiloBuildCount);
  num("nukeSiloStorageCount", s.nukeSiloCount);
  num("nukeSiloMaxStorageCount", s.nukeSiloMaxStorage);
  num("fuelRatio", s.fuelRatio);
  num("shieldRatio", s.shieldRatio);
  num("workProgress", u->view.workProgress);
  lua_pushstring(L, "userUnit");
  PushObject(L, u);
  lua_rawset(L, -3);
  str("entityId", std::to_string(u->view.id));
  num("armyIndex", u->view.army - 1);
  str("teamColor", ArmyColorOf(L, u->view.army));
  if (!u->customName.empty()) str("customName", u->customName);
  if (withFocus && s.focus != 0xffffffffu) {
    UnitStore& store = Store(L);
    auto it = store.units.find(s.focus);
    if (it != store.units.end() && !it->second->gone) {
      lua_pushstring(L, "focus");
      PushRollover(L, it->second.get(), false);
      lua_rawset(L, -3);
    }
  }
}
int l_GetRolloverInfo(lua_State* L) {
  UnitStore& store = Store(L);
  auto it = store.units.find(UnitUnderMouse(L));
  if (it == store.units.end() || it->second->gone || it->second->view.dead) return 0;
  UserUnit* u = it->second.get();
  uint32_t recon = ReconBitsOf(L, u);
  if (recon == 0) return 0;
  if (!(recon & kReconSeenNow)) {
    // A blip (radar or sonar alone): the player is told whose it is, not what it is. (The
    // original's table for an entity without a blueprint, 0x8426f0: blueprintId "unknown",
    // teamColor, armyIndex. That a blip is such an entity there is inferred.)
    lua_newtable(L);
    lua_pushstring(L, "blueprintId");
    lua_pushstring(L, "unknown");
    lua_rawset(L, -3);
    lua_pushstring(L, "teamColor");
    lua_pushstring(L, ArmyColorOf(L, u->view.army).c_str());
    lua_rawset(L, -3);
    lua_pushstring(L, "armyIndex");
    lua_pushnumber(L, u->view.army - 1);
    lua_rawset(L, -3);
    return 1;
  }
  PushRollover(L, u, true);
  return 1;
}

// The switches of a list of units, as the order buttons ask. GetFireState(units): the state they
// share, -1 when they differ. The others: true when any of them has it on.
int l_GetFireStateNow(lua_State* L) {
  std::vector<UserUnit*> list;
  ReadUnitList(L, 1, &list);
  int state = -2;
  for (UserUnit* u : list) {
    UnitStateView s;
    if (!StateOf(L, u, &s)) continue;
    state = state == -2 || state == s.fireState ? s.fireState : -1;
  }
  lua_pushnumber(L, state == -2 ? 0 : state);
  return 1;
}
template <bool UnitStateView::*Field>
int l_AnySwitch(lua_State* L) {
  std::vector<UserUnit*> list;
  ReadUnitList(L, 1, &list);
  bool any = false;
  for (UserUnit* u : list) {
    UnitStateView s;
    any = any || (StateOf(L, u, &s) && s.*Field);
  }
  lua_pushboolean(L, any);
  return 1;
}
// GetScriptBit(units, bit): whether any of them has the toggle on
int l_GetScriptBitNow(lua_State* L) {
  std::vector<UserUnit*> list;
  ReadUnitList(L, 1, &list);
  int bit = static_cast<int>(luaL_checknumber(L, 2));
  bool any = false;
  for (UserUnit* u : list) {
    UnitStateView s;
    any = any || (StateOf(L, u, &s) && bit >= 0 && bit < 32 && (s.scriptBits >> bit & 1u));
  }
  lua_pushboolean(L, any);
  return 1;
}

// --- a factory's queue as the construction panel shows it ---
//
// SetCurrentFactoryForQueueDisplay(unit) names the factory and returns its build queue as
// {{id = blueprint, count = n}, ...}; while one is named, the game script hears of a change
// (gamemain.OnQueueChanged(queue), which the original calls by name, FA exe 0x836270);
// ClearCurrentFactoryForQueueDisplay() ends that. DecreaseBuildCountInQueue(index, count) and
// IncreaseBuildCountInQueue(index, count) change an entry (counted from 1).

void BuildQueue(lua_State* L, const UserUnit* factory, std::vector<CommandView>* out) {
  UnitStateView s;
  if (!StateOf(L, factory, &s)) return;
  for (const CommandView& c : s.queue)
    if (c.type == static_cast<int>(CommandType::BuildFactory) || c.type == static_cast<int>(CommandType::BuildMobile) ||
        c.type == static_cast<int>(CommandType::Upgrade))
      if (!c.blueprint.empty()) out->push_back(c);
}
void PushBuildQueue(lua_State* L, const std::vector<CommandView>& queue) {
  lua_newtable(L);
  int n = 0;
  for (const CommandView& c : queue) {
    lua_newtable(L);
    lua_pushstring(L, "id");
    lua_pushstring(L, c.blueprint.c_str());
    lua_rawset(L, -3);
    lua_pushstring(L, "count");
    lua_pushnumber(L, c.count);
    lua_rawset(L, -3);
    lua_rawseti(L, -2, ++n);
  }
}
std::string QueueKey(const std::vector<CommandView>& queue) {
  std::string key;
  for (const CommandView& c : queue) key += c.blueprint + "*" + std::to_string(c.count) + ",";
  return key;
}
int l_SetCurrentFactoryForQueueDisplay(lua_State* L) {
  UnitStore& store = Store(L);
  UserUnit* u = ToUnit(L, 1);
  store.queueFactory = u ? u->view.id : 0xffffffffu;
  std::vector<CommandView> queue;
  BuildQueue(L, u, &queue);
  store.queueKey = QueueKey(queue);
  PushBuildQueue(L, queue);
  return 1;
}
int l_ClearCurrentFactoryForQueueDisplay(lua_State* L) {
  Store(L).queueFactory = 0xffffffffu;
  return 0;
}
int ChangeBuildCount(lua_State* L, bool more) {
  UnitStore& store = Store(L);
  auto it = store.units.find(store.queueFactory);
  if (it == store.units.end()) return 0;
  std::vector<CommandView> queue;
  BuildQueue(L, it->second.get(), &queue);
  int index = static_cast<int>(luaL_checknumber(L, 1)), count = static_cast<int>(luaL_optnumber(L, 2, 1));
  if (index < 1 || index > static_cast<int>(queue.size())) return 0;
  if (more) Writer(L).IncreaseCommandCount(queue[static_cast<size_t>(index - 1)].id, count);
  else Writer(L).DecreaseCommandCount(queue[static_cast<size_t>(index - 1)].id, count);
  return 0;
}
int l_IncreaseBuildCountInQueue(lua_State* L) { return ChangeBuildCount(L, true); }
int l_DecreaseBuildCountInQueue(lua_State* L) { return ChangeBuildCount(L, false); }

// After a beat: tell the game script when the shown factory's queue is not what it was.
void ReportQueueChange(lua_State* L) {
  UnitStore& store = Store(L);
  if (store.queueFactory == 0xffffffffu || !UserState::From(L)->gameInterface) return;
  auto it = store.units.find(store.queueFactory);
  if (it == store.units.end()) return;
  std::vector<CommandView> queue;
  BuildQueue(L, it->second.get(), &queue);
  std::string key = QueueKey(queue);
  if (key == store.queueKey) return;
  store.queueKey = key;
  int top = lua_gettop(L);
  lua_pushcfunction(L, ScriptTraceback);
  lua_getglobal(L, "import");
  lua_pushstring(L, "/lua/ui/game/gamemain.lua");
  if (lua_pcall(L, 1, 1, top + 1) == 0 && lua_istable(L, -1)) {
    lua_pushstring(L, "OnQueueChanged");
    lua_gettable(L, -2);
    if (lua_isfunction(L, -1)) {
      PushBuildQueue(L, queue);
      if (lua_pcall(L, 1, 0, top + 1) != 0)
        Logf(LogLevel::Warning, "Error running '/lua/ui/game/gamemain.lua:OnQueueChanged': %s", lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
    }
  }
  lua_settop(L, top);
}

}  // namespace

void SelectByCategoryCommand(lua_State* L, const std::string& arguments) {
  if (!TheSim(L)) return;
  bool add = false, nearest = false, idle = false, inView = false, goTo = false, noEngineers = false;
  std::string expr;  // the other words, a space after each: side by side they narrow the category
  size_t at = 0;
  while (at < arguments.size()) {
    size_t a = arguments.find_first_not_of(" \t", at);
    if (a == std::string::npos) break;
    size_t b = arguments.find_first_of(" \t", a);
    std::string word = arguments.substr(a, b == std::string::npos ? std::string::npos : b - a);
    at = b == std::string::npos ? arguments.size() : b;
    if (word[0] != '+') {
      expr += word + " ";
      continue;
    }
    std::string flag;
    for (char ch : word) flag.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    if (flag == "+add") add = true;
    else if (flag == "+nearest") nearest = true;
    else if (flag == "+idle") idle = true;
    else if (flag == "+inview") inView = true;
    else if (flag == "+goto") goTo = true;
    else if (flag == "+excludeengineers") noEngineers = true;
    else Logf(LogLevel::Info, "Unknown modifier %s", word.c_str());
  }
  if (expr.empty()) return;
  expr.pop_back();
  if (!SelectUnitsByCategory(L, expr.c_str(), add, inView, nearest, idle, goTo, noEngineers))
    Logf(LogLevel::Warning, "UI_SelectByCategory: invalid category expression: %s", expr.c_str());
}

void ExpandCurrentSelection(lua_State* L) {
  Sim* sim = TheSim(L);
  if (!sim) return;
  UnitStore& store = Store(L);
  std::vector<UserUnit*> now = store.selection, asked;
  std::vector<uint32_t> ids;
  if (UnitsOnScreen(L, &ids)) {
    std::sort(ids.begin(), ids.end());
    for (uint32_t id : ids) {
      auto it = store.units.find(id);
      if (it != store.units.end()) asked.push_back(it->second.get());
    }
  } else {
    for (auto& [id, u] : store.units)
      if (PlaceInCameraView(L, u->view.position)) asked.push_back(u.get());
  }
  for (UserUnit* u : asked) {
    if (u->gone || u->view.dead || !u->view.selectable || u->view.army != sim->focusArmy + 1) continue;
    if (InCategory(L, u, "WALL") || std::find(now.begin(), now.end(), u) != now.end()) continue;
    bool kind = false;
    for (UserUnit* s : store.selection) kind = kind || s->view.blueprint == u->view.blueprint;
    if (kind && !BeingUpgraded(L, sim, u)) now.push_back(u);
  }
  SetSelection(L, now);
}

void SelectUnitsById(lua_State* L, const std::vector<uint32_t>& ids, bool add) {
  Sim* sim = TheSim(L);
  if (!sim) return;
  UnitStore& store = Store(L);
  std::vector<UserUnit*> now;
  if (add) now = store.selection;
  for (uint32_t id : ids) {
    auto it = store.units.find(id);
    if (it == store.units.end()) continue;
    UserUnit* u = it->second.get();
    if (u->gone || u->view.dead || !u->view.selectable || u->view.army != sim->focusArmy + 1) continue;
    if (std::find(now.begin(), now.end(), u) == now.end()) now.push_back(u);
  }
  SetSelection(L, now);
}

bool SelectSameKind(lua_State* L, uint32_t unit, const std::vector<uint32_t>& among, bool add) {
  Sim* sim = TheSim(L);
  if (!sim) return false;
  UnitStore& store = Store(L);
  auto it = store.units.find(unit);
  if (it == store.units.end() || it->second->gone || it->second->view.army != sim->focusArmy + 1) return false;
  const std::string kind = it->second->view.blueprint;
  std::vector<uint32_t> same;
  for (uint32_t id : among) {
    auto other = store.units.find(id);
    if (other != store.units.end() && !other->second->gone && other->second->view.blueprint == kind) same.push_back(id);
  }
  if (std::find(same.begin(), same.end(), unit) == same.end()) same.push_back(unit);
  SelectUnitsById(L, same, add);
  return true;
}

void BuildSpacingOf(lua_State* L, const std::string& blueprint, float* alongX, float* alongZ) {
  float size[2] = {1, 1};
  PushBlueprint(L, blueprint);
  if (lua_istable(L, -1)) {
    const char* tables[2] = {"Footprint", "Physics"};
    const char* fields[2][2] = {{"SizeX", "SizeZ"}, {"SkirtSizeX", "SkirtSizeZ"}};
    for (int t = 0; t < 2; ++t) {
      lua_getfield(L, -1, tables[t]);
      for (int i = 0; i < 2 && lua_istable(L, -1); ++i) {
        lua_getfield(L, -1, fields[t][i]);
        if (lua_isnumber(L, -1)) size[i] = std::max(size[i], static_cast<float>(lua_tonumber(L, -1)));
        lua_pop(L, 1);
      }
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);
  *alongX = size[0], *alongZ = size[1];
}

void SelectionForWorld(lua_State* L, std::vector<uint32_t>* ids, std::vector<WorldLine>* lines, std::vector<WorldGhost>* ghosts,
                       bool queueShown, std::vector<WorldGhost>* queued) {
  Sim* sim = TheSim(L);
  UserState* user = UserState::From(L);
  if (!sim || !user->units) return;
  UnitStore& store = static_cast<UnitStore&>(*user->units);
  int shown = 0;
  for (UserUnit* u : store.selection) {
    if (u->gone || u->view.dead) continue;
    ids->push_back(u->view.id);
    if (!queueShown || u->view.army != sim->focusArmy + 1 || u->view.queueLength == 0 || ++shown > 60) continue;
    UnitStateView s;
    if (!UnitStateOf(*sim, u->view.id, &s)) continue;
    float from[3] = {u->view.position[0], u->view.position[1], u->view.position[2]};
    for (const CommandView& c : s.queue) {
      if (!c.hasPosition) continue;
      if (queued && static_cast<CommandType>(c.type) == CommandType::BuildMobile && !c.blueprint.empty() && queued->size() < 400) {
        WorldGhost site;
        site.blueprint = c.blueprint;
        for (int i = 0; i < 3; ++i) site.position[i] = c.position[i];
        queued->push_back(site);
      }
      // First version of the colours: move light blue, attack red, patrol green, building yellow
      // (as in Chris's picture of the original, 2026-10-13), reclaim a paler yellow, the rest
      // grey. (The original's others are still to be looked up.)
      uint32_t color = 0xffc0c0c0u;
      switch (static_cast<CommandType>(c.type)) {
        case CommandType::Move: case CommandType::FormMove: case CommandType::AggressiveMove: color = 0xff40c8ffu; break;
        case CommandType::Attack: case CommandType::FormAttack: color = 0xffff4040u; break;
        case CommandType::Patrol: case CommandType::FormPatrol: color = 0xff40ff40u; break;
        case CommandType::BuildMobile: case CommandType::BuildFactory: color = 0xffe8c800u; break;
        case CommandType::Reclaim: color = 0xffffff40u; break;
        default: break;
      }
      WorldLine line;
      for (int i = 0; i < 3; ++i) line.a[i] = from[i], line.b[i] = c.position[i], from[i] = c.position[i];
      line.color = color;
      line.width = 3;
      lines->push_back(line);
    }
  }
  // A building ordered and not begun shows where it will stand, whether its builder is selected
  // or not (Chris, 2026-10-13: as the original game shows them; the original's own rule is not
  // read). The one being built is there itself: the first order of a unit that has something it
  // works on. Worked out once a beat, over all of the player's units that have orders.
  if (!ghosts) return;
  if (store.orderedTick != sim->tick()) {
    store.orderedTick = sim->tick();
    store.ordered.clear();
    for (auto& entry : store.units) {
      const UserUnit* u = entry.second.get();
      if (u->gone || u->view.dead || u->view.army != sim->focusArmy + 1 || u->view.queueLength == 0) continue;
      UnitStateView s;
      if (!UnitStateOf(*sim, u->view.id, &s)) continue;
      bool first = true;
      for (const CommandView& c : s.queue) {
        if (!c.hasPosition) continue;
        const bool begun = first && s.focus != 0xffffffffu;
        first = false;
        if (static_cast<CommandType>(c.type) != CommandType::BuildMobile || c.blueprint.empty() || begun || store.ordered.size() >= 400) continue;
        bool there = false;
        for (const WorldGhost& g : store.ordered) there = there || (g.blueprint == c.blueprint && g.position[0] == c.position[0] && g.position[2] == c.position[2]);
        if (there) continue;
        WorldGhost g;
        g.blueprint = c.blueprint;
        for (int i = 0; i < 3; ++i) g.position[i] = c.position[i];
        store.ordered.push_back(g);
      }
    }
  }
  ghosts->insert(ghosts->end(), store.ordered.begin(), store.ordered.end());
}

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
  ReportQueueChange(L);
}

void RegisterUserUnitBindings(lua_State* L) {
  if (!TheSim(L)) return;
  SetGlobal(L, "GetSelectedUnits", l_GetSelectedUnits);
  SetGlobal(L, "SelectUnits", l_SelectUnits);
  SetGlobal(L, "AddSelectUnits", l_AddSelectUnits);
  SetGlobal(L, "UISelectionByCategory", l_UISelectionByCategory);
  SetGlobal(L, "UIZoomTo", l_UIZoomTo);
  SetGlobal(L, "UISelectAndZoomTo", l_UISelectAndZoomTo);
  SetGlobal(L, "ValidateUnitsList", l_ValidateUnitsList);
  SetGlobal(L, "GetUnitById", l_GetUnitById);
  SetGlobal(L, "GetRolloverInfo", l_GetRolloverInfo);
  SetGlobal(L, "GetArmyAvatars", l_GetArmyAvatars);
  SetGlobal(L, "GetIdleEngineers", l_GetIdleEngineers);
  SetGlobal(L, "GetIdleFactories", l_GetIdleFactories);
  SetGlobal(L, "GetUnitCommandData", l_GetUnitCommandData);
  SetGlobal(L, "GetFireState", l_ListZero);
  SetGlobal(L, "IssueCommand", l_IssueCommand);
  SetGlobal(L, "IssueUnitCommand", l_IssueUnitCommand);
  SetGlobal(L, "IssueBlueprintCommand", l_IssueBlueprintCommand);
  SetGlobal(L, "IssueBlueprintCommandToUnit", l_IssueBlueprintCommandToUnit);
  SetGlobal(L, "GetUnitCommandFromCommandCap", l_GetUnitCommandFromCommandCap);
  SetGlobal(L, "SimCallback", l_SimCallback);
  SetGlobal(L, "SetPaused", l_SetPaused);
  SetGlobal(L, "SetAutoMode", l_SetAutoMode);
  SetGlobal(L, "SetFireState", l_SetFireState);
  SetGlobal(L, "ToggleFireState", l_ToggleFireState);
  SetGlobal(L, "ToggleScriptBit", l_ToggleScriptBit);
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

  // The unit's state from the simulation.
  SetMethod(L, cls, "GetCommandQueue", l_GetCommandQueue);
  SetMethod(L, cls, "GetFocus", l_UnitEntity<&UnitStateView::focus>);
  SetMethod(L, cls, "GetGuardedEntity", l_UnitEntity<&UnitStateView::guarded>);
  SetMethod(L, cls, "GetCreator", l_UnitEntity<&UnitStateView::creator>);
  SetMethod(L, cls, "IsAutoMode", l_UnitSwitch<&UnitStateView::autoMode>);
  SetMethod(L, cls, "IsAutoSurfaceMode", l_UnitSwitch<&UnitStateView::autoSurfaceMode>);
  SetMethod(L, cls, "IsRepeatQueue", l_UnitSwitch<&UnitStateView::repeatQueue>);
  SetMethod(L, cls, "GetShieldRatio", l_GetShieldRatioNow);
  SetMethod(L, cls, "GetFuelRatio", l_GetFuelRatioNow);
  SetMethod(L, cls, "GetBuildRate", l_GetBuildRateNow);
  SetMethod(L, cls, "GetEconData", l_GetEconDataNow);
  SetMethod(L, cls, "GetMissileInfo", l_GetMissileInfoNow);
  SetGlobal(L, "GetFireState", l_GetFireStateNow);
  SetGlobal(L, "GetScriptBit", l_GetScriptBitNow);
  SetGlobal(L, "GetIsPaused", l_AnySwitch<&UnitStateView::paused>);
  SetGlobal(L, "GetIsAutoMode", l_AnySwitch<&UnitStateView::autoMode>);
  SetGlobal(L, "GetIsAutoSurfaceMode", l_AnySwitch<&UnitStateView::autoSurfaceMode>);
  SetGlobal(L, "SetCurrentFactoryForQueueDisplay", l_SetCurrentFactoryForQueueDisplay);
  SetGlobal(L, "ClearCurrentFactoryForQueueDisplay", l_ClearCurrentFactoryForQueueDisplay);
  SetGlobal(L, "IncreaseBuildCountInQueue", l_IncreaseBuildCountInQueue);
  SetGlobal(L, "DecreaseBuildCountInQueue", l_DecreaseBuildCountInQueue);
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
