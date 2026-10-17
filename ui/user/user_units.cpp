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
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/log.h"
#include "draw/draw.h"
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
  // Its blueprint's record as last looked up, and for which blueprint (BlueprintOf): a category
  // test over thousands of units looks each up once, not each time.
  mutable const BlueprintInfo* bpInfo = nullptr;
  mutable std::string bpInfoFor;
  // Its state as last asked of the simulation, and at which sync (StateOf; ~0: never).
  mutable UnitStateView stateCopy;
  mutable uint64_t stateSync = ~0ull;
};

struct UnitStore : UserState::Extra {
  std::map<uint32_t, std::unique_ptr<UserUnit>> units;
  std::vector<UserUnit*> selection;
  std::vector<UserUnit*> capsOwn;  // (RightClickOrder's list of the selection's own units, kept for its room)
  // GetRolloverInfo's last answer (a registry reference, LUA_NOREF for none) and for which unit.
  int rolloverRef = LUA_NOREF;
  uint32_t rolloverId = 0;
  bool rolloverKnown = false;
  // CanReclaimAtPlace's last question and answer (see there).
  uint32_t reclaimTick = 0xffffffffu;
  std::vector<UserUnit*> reclaimFor;
  float reclaimAt[3] = {0, 0, 0}, reclaimWithin = -1;
  bool reclaimAnswer = false;
  bool reporting = false;  // inside OnSelectionChanged
  std::unique_ptr<CommandWriter> writer;  // the player's orders since the last beat
  uint32_t queueFactory = 0xffffffffu;    // the factory whose queue the construction panel shows
  std::string queueKey;                   // that queue as it was last handed to the script
  // What the game's table says of how each kind of order is drawn (commandgraphparams.lua), as
  // looked up so far: key "UNITCOMMAND_Move/orderline_selected_color" -> its text ("" for none).
  std::map<std::string, std::string> graphParams;
  std::map<std::string, bool> canFly;  // Air.CanFly by blueprint (CanFlyOf)
  // The buildings the player's units have been ordered to build and have not begun, as of beat
  // `orderedTick` (worked out once a beat, for the world's picture).
  std::vector<WorldGhost> ordered;
  std::vector<uint32_t> orderedBy;  // (beside `ordered`: the unit whose order each is)
  uint32_t orderedTick = 0xffffffffu;
  // Building orders the interface has sent that the simulation's queues may not show yet (the
  // beats since are few): their sites, with the beat they were sent in.
  struct SentSite {
    WorldGhost site;
    uint32_t tick;
  };
  std::vector<SentSite> sent;
  // The build template being placed (see BuildTemplate), and the units a script has added to
  // the selection's marks without selecting them (AddToSessionExtraSelectList).
  BuildTemplate buildTemplate;
  std::vector<uint32_t> extraSelect;
  // The simulation's category sets the weapons' views name (CategorySets: taken after each beat; the
  // table only grows), and the blueprints' Physics.MotionType as the original numbers it, as looked up.
  std::vector<std::vector<uint64_t>> categorySets;
  std::map<std::string, int> motionTypes;
};

UnitStore& Store(lua_State* L) {
  UserState* user = UserState::From(L);
  if (!user->units) user->units = std::make_unique<UnitStore>();
  return static_cast<UnitStore&>(*user->units);
}
Sim* TheSim(lua_State* L) { return UserState::From(L)->LiveSim(); }
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

const BlueprintInfo* BlueprintOf(const SimBlueprints* bps, const UserUnit* u) {
  if (u->bpInfo && u->bpInfoFor == u->view.blueprint) return u->bpInfo;
  u->bpInfo = bps->Find(u->view.blueprint);
  u->bpInfoFor = u->view.blueprint;
  return u->bpInfo;
}
// Whether the unit's blueprint is in a category's list (its entity indexes, sorted).
bool InCategoryList(const SimBlueprints* bps, const UserUnit* u, const std::vector<int>& list) {
  const BlueprintInfo* bp = BlueprintOf(bps, u);
  return bp && std::binary_search(list.begin(), list.end(), bp->entityIndex);
}
bool InCategory(lua_State* L, const UserUnit* u, const std::string& category) {
  SimBlueprints* bps = SimBlueprints::From(L);
  if (!bps) return false;
  auto it = bps->Categories().find(category);
  return it != bps->Categories().end() && InCategoryList(bps, u, it->second);
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
// A unit the simulation's script has made unselectable (Unit:SetUnSelectable: a commander while
// it arrives) is not selected by the script either: the original's SelectUnits (0x8bd8f0) takes
// a unit only if its UserUnit says it may be selected (vt+0x30, 0x8c0500: not in the state
// UnSelectable; read by the engine track, docs/engine-answers.md "ui 18"). The game's own
// start-up (/lua/ui/game/gamemain.lua, StartupSequence) calls SelectUnits on the commander every
// tenth of a second until GetSelectedUnits gives something or GameTick() is above 50; the warp
// ends on tick 52, so there, as here, the commander is selected by the script only if a try
// happens to step over tick 51. Before this, ours took the commander two seconds before it
// could build: the build menu came up empty and stayed so until it was selected again.
//
// Whether a unit may be selected (UserUnit::IsSelectable, FA exe 0x8c0500 with FAF's patch at
// 0x1291519; read by the engine track, docs/engine-answers.md "ui 36"): not if it is dead, in the
// state UnSelectable, not in SELECTABLE, being built and neither a FACTORY nor a CQUEMOV, or a POD
// that moves and hangs on something (a docked drone). Used by the scripts' SelectUnits and
// AddSelectUnits, and through CanSelectUnit by the mouse.
bool Moves(lua_State* L, const UserUnit* u);  // (below: its blueprint's motion type one of the eight of moving)
bool IsSelectable(lua_State* L, const UserUnit* u) {
  if (u->gone || u->view.dead || !u->view.selectable || !InCategory(L, u, "SELECTABLE")) return false;
  if (u->view.beingBuilt && !InCategory(L, u, "FACTORY") && !InCategory(L, u, "CQUEMOV")) return false;
  if ((u->view.attached || u->view.parent != 0xffffffffu) && InCategory(L, u, "POD") && Moves(L, u)) return false;
  return true;
}
// What the mouse may select (CWldSession::CanSelectUnit 0x865830, read 2026-10-16; notes in the ui
// chat's scratchpad helper_selectable\findings.md): the click, the box and the double click all ask
// it. A unit that may be selected (IsSelectable: so not a building under construction unless it is a
// factory) and of the player's army. (Not built: its bypass by the console's UI_SelectAnything.)
// The player's army (0-based, -1: none) without waiting for a beat that runs on its own thread: the beat's
// copy once there is one (as GetFocusArmy). The mouse's paths ask it every frame.
int FocusArmyNow(lua_State* L) {
  UserState* user = UserState::From(L);
  if (user->economyCopied) return user->focusArmyCopy;
  Sim* sim = user->HasSim() ? TheSim(L) : nullptr;
  return sim ? sim->focusArmy : -1;
}
// Whether the game was started with cheats on (its Options.CheatsEnabled; INFERRED to be the session's byte
// +0x4d4 the original asks beside UI_SelectAnything).
bool CheatsOn(lua_State* L) {
  bool cheats = false;
  const int top = lua_gettop(L);
  lua_getglobal(L, "SessionGetScenarioInfo");
  if (lua_isfunction(L, -1) && lua_pcall(L, 0, 1, 0) == 0 && lua_istable(L, -1)) {
    lua_pushstring(L, "Options");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
      lua_pushstring(L, "CheatsEnabled");
      lua_gettable(L, -2);
      cheats = lua_isstring(L, -1) && std::string(lua_tostring(L, -1)) == "true";
    }
  }
  lua_settop(L, top);
  return cheats;
}
bool CanSelectUnit(lua_State* L, const UserUnit* u) {
  if (Looks().selectAnything && !u->gone && CheatsOn(L)) return true;  // (the console's UI_SelectAnything, with cheats)
  const int focus = FocusArmyNow(L);
  return focus >= 0 && IsSelectable(L, u) && u->view.army == focus + 1;
}
// A blueprint's Physics.MotionType as the original numbers it (FA exe 0x51fb40: None 0, Land 1, Air 2, Water 3,
// Biped 4, SurfacingSub 5, Amphibious 6, Hover 7, AmphibiousFloating 8, Special 9), looked up once per blueprint;
// a unit "moves" (UserUnit::IsMobile 0x8c04e0) with one of 1 .. 8.
int MotionTypeOf(lua_State* L, const std::string& blueprint) {
  UnitStore& store = Store(L);
  auto it = store.motionTypes.find(blueprint);
  if (it != store.motionTypes.end()) return it->second;
  static const char* const kNames[] = {"RULEUMT_None", "RULEUMT_Land", "RULEUMT_Air", "RULEUMT_Water", "RULEUMT_Biped",
                                       "RULEUMT_SurfacingSub", "RULEUMT_Amphibious", "RULEUMT_Hover",
                                       "RULEUMT_AmphibiousFloating", "RULEUMT_Special"};
  int type = 0;
  PushBlueprint(L, blueprint);
  if (lua_istable(L, -1)) {
    lua_pushstring(L, "Physics");
    lua_rawget(L, -2);
    if (lua_istable(L, -1)) {
      lua_pushstring(L, "MotionType");
      lua_rawget(L, -2);
      if (lua_isstring(L, -1))
        for (int i = 0; i < 10; ++i)
          if (std::strcmp(lua_tostring(L, -1), kNames[i]) == 0) type = i;
      lua_pop(L, 1);
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  store.motionTypes[blueprint] = type;
  return type;
}
bool Moves(lua_State* L, const UserUnit* u) {
  const int type = MotionTypeOf(L, u->view.blueprint);
  return type >= 1 && type <= 8;
}

// Whether the simulation's category set `set` (-1: none) holds the blueprint with that category index (the
// beat's copy of the table; bit i & 63 of word i >> 6, as CategorySets gives them).
bool SetHas(const UnitStore& store, int set, int index) {
  if (set < 0 || index < 0 || static_cast<size_t>(set) >= store.categorySets.size()) return false;
  const std::vector<uint64_t>& words = store.categorySets[static_cast<size_t>(set)];
  const size_t w = static_cast<size_t>(index) >> 6;
  return w < words.size() && ((words[w] >> (index & 63)) & 1u) != 0;
}

// UserUnit::CanAttackTarget (FA exe 0x8c0d30; read 2026-10-16 by the ui chat's helper and the engine track,
// docs/engine-answers.md "ui 37"): whether `s` can attack `t`, by its weapons as the simulation last set them
// (UnitView::weapons: the layers the scripts gave each, its ranges, its target restrictions).
// - No target (nullptr: the place under the mouse): with checkRange a unit that moves can; one that does not
//   if the place is strictly within some weapon's ranges (2D). Without checkRange: no.
// - A unit that moves, with auto-surface on: an OVERLAYANTIAIR one at a target in the air, an OVERLAYDIRECTFIRE
//   one at one on land; on land, an amphibious one (MotionType 6 or 8) with OVERLAYANTINAVY at one under water
//   or on the seabed; on the seabed, an amphibious one (6) with OVERLAYDIRECTFIRE at one on land.
// - Else a weapon that fires at the target's layer, whose restrictions do not keep the target out: for a unit
//   that moves (or without checkRange) that is enough; one that does not needs the target within that weapon's
//   ranges (2D, the ends counted), else the next weapon is asked.
bool CanAttackTarget(lua_State* L, const UserUnit* s, const UserUnit* t, bool checkRange) {
  const int motion = MotionTypeOf(L, s->view.blueprint);
  const bool mobile = motion >= 1 && motion <= 8;
  const std::vector<WeaponView>& weapons = s->view.weapons;
  if (!t) {
    if (!checkRange) return false;
    if (mobile) return true;
    float at[3];
    if (!MouseWorldPlace(L, at)) return false;
    const float dx = at[0] - s->view.position[0], dz = at[2] - s->view.position[2];
    const float d = std::sqrt(dx * dx + dz * dz);
    for (const WeaponView& w : weapons)
      if (w.minRadius < d && d < w.maxRadius) return true;
    return false;
  }
  const int tl = t->view.layer, sl = s->view.layer;
  if (mobile) {
    if (s->view.autoSurfaceMode) {
      if (tl == 16 && InCategory(L, s, "OVERLAYANTIAIR")) return true;
      if (tl == 1 && InCategory(L, s, "OVERLAYDIRECTFIRE")) return true;
    }
    if (sl == 1 && (tl == 4 || tl == 2) && (motion == 6 || motion == 8) && InCategory(L, s, "OVERLAYANTINAVY")) return true;
    if (sl == 2 && tl == 1 && motion == 6 && InCategory(L, s, "OVERLAYDIRECTFIRE")) return true;
  }
  const UnitStore& store = Store(L);
  float d = 0;
  if (checkRange) {
    const float dx = t->view.position[0] - s->view.position[0], dz = t->view.position[2] - s->view.position[2];
    d = std::sqrt(dx * dx + dz * dz);
  }
  for (const WeaponView& w : weapons) {
    if (!(w.layerCaps & tl)) continue;
    if (SetHas(store, w.disallow, t->view.categoryIndex)) continue;
    if (w.onlyAllow >= 0 && !SetHas(store, w.onlyAllow, t->view.categoryIndex)) continue;
    if (mobile || !checkRange) return true;
    if (w.minRadius <= d && d <= w.maxRadius) return true;
  }
  return false;
}
// Whether any of these units can attack `t` (FA exe 0x81d080, the right click's test): not when there is no
// target, it is dead, or it is gone.
bool AnyCanAttack(lua_State* L, const std::vector<UserUnit*>& units, const UserUnit* t) {
  if (!t || t->gone || t->view.dead) return false;
  for (const UserUnit* s : units)
    if (CanAttackTarget(L, s, t, true)) return true;
  return false;
}

void DropUnselectable(lua_State* L, std::vector<UserUnit*>* list) {
  list->erase(std::remove_if(list->begin(), list->end(), [L](const UserUnit* u) { return !IsSelectable(L, u); }), list->end());
}
// (FA exe 0x8bd8f0, read by the engine track, "ui 36": a unit that may not be selected gives its
// place to the unit it hangs on, if that one may be selected and is a TRANSPORTATION: one level.)
int l_SelectUnits(lua_State* L) {
  std::vector<UserUnit*> list, now;
  ReadUnitList(L, 1, &list);
  UnitStore& store = Store(L);
  for (UserUnit* u : list) {
    if (u->view.dead) continue;
    UserUnit* take = u;
    if (!IsSelectable(L, u)) {
      auto parent = store.units.find(u->view.parent);
      take = u->view.parent != 0xffffffffu && parent != store.units.end() && IsSelectable(L, parent->second.get()) &&
                     InCategory(L, parent->second.get(), "TRANSPORTATION")
                 ? parent->second.get()
                 : nullptr;
    }
    if (take && std::find(now.begin(), now.end(), take) == now.end()) now.push_back(take);
  }
  SetSelection(L, now);
  return 0;
}
// ValidateUnitsList(units) (FA exe 0x8bc5f0): the units of the list that still exist and are not
// dead, in the list's order, a unit given twice kept twice.
int l_ValidateUnitsList(lua_State* L) {
  std::vector<UserUnit*> list;
  if (lua_istable(L, 1)) {
    const int n = luaL_getn(L, 1);
    for (int i = 1; i <= n; ++i) {
      lua_rawgeti(L, 1, i);
      UserUnit* u = ToUnit(L, lua_gettop(L));
      if (u && !u->gone && !u->view.dead) list.push_back(u);
      lua_pop(L, 1);
    }
  }
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
  DropUnselectable(L, &now);
  SetSelection(L, now);
  return 0;
}

// Whether a unit is in the original's state "BeingUpgraded" (0x25): the building that an upgrade
// is putting up in place of the old one. INFERRED from what the engine's view gives: unfinished,
// a structure, and made by a structure (a factory's units are mobile).
bool BeingUpgraded(lua_State* L, Sim* sim, const UserUnit* u) {
  (void)sim;
  if (!u->view.beingBuilt || !InCategory(L, u, "STRUCTURE") || u->view.creator == 0xffffffffu) return false;
  auto it = Store(L).units.find(u->view.creator);
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
    // (idle, FA exe 0x8664c6: not hanging on a parent, docked, loaded or stored (UserUnit+0x1a2),
    // and nothing in its queue; until 2026-10-16 this read "being built", a wrong reading)
    if (idle && (u->view.attached || u->view.queueLength != 0)) continue;
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
  const SimBlueprints* bps = SimBlueprints::From(L);
  if (!bps) return;
  auto cat = bps->Categories().find(category);  // (once, not for each unit)
  if (cat == bps->Categories().end()) return;
  for (auto& [id, u] : Store(L).units)
    if (!u->gone && !u->view.dead && u->view.army == army && InCategoryList(bps, u.get(), cat->second) && test(*u))
      out->push_back(u.get());
}
// GetArmyAvatars(): the focus army's commanders, nil when it has none; nothing at all without a
// focus army (a replay, an observer): FA exe 0x8bccb0 returns no value then, and pushes nil
// (0x8bce0d) for an empty list. (/lua/ui/game/gamemain.lua's OnFirstUpdate tests the result
// before taking avatars[1]: with an empty table the replay's start stopped there.)
int l_GetArmyAvatars(lua_State* L) {
  if (TheSim(L)->focusArmy < 0) return 0;
  std::vector<UserUnit*> list;
  FocusUnits(L, "COMMAND", [](const UserUnit&) { return true; }, &list);
  PushUnitList(L, list, true);
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
  bool first = true;
  for (UserUnit* u : list) {
    // What the unit can do right now: its caps as the beat's copy has them (engine ui 26: the
    // unit's own, no asking the simulation for each unit). The orders and toggles of all the units.
    masks[2] |= u->view.commandCaps;
    masks[3] |= u->view.toggleCaps;
    PushBlueprint(L, u->view.blueprint);
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      std::fill(total.begin(), total.end(), 0);  // (a unit without a blueprint builds nothing)
      first = false;
      continue;
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
    // What all of them can build (FA exe 0x840130: each unit's set, the first as it is, every later
    // one intersected with it, BVIntSet::IntersectWith 0x401af0; a unit that builds nothing in the
    // selection leaves nothing to build, as in the original).
    for (size_t w = 0; w < total.size(); ++w) total[w] = first ? mine[w] : (total[w] & mine[w]);
    first = false;
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
// GetIsPaused(units), GetIsAutoMode(units), GetIsAutoSurfaceMode(units), GetScriptBit(units, bit):
// what the order buttons show as switched on: true when any of the units has it (INFERRED: any,
// not all; the original's functions are not read).
template <bool UnitStateView::*Field>
int l_ListSwitch(lua_State* L) {
  std::vector<UserUnit*> list;
  ReadUnitList(L, 1, &list);
  bool any = false;
  for (UserUnit* u : list) {
    UnitStateView s;
    if (!u->gone && StateOf(L, u, &s) && s.*Field) any = true;
  }
  lua_pushboolean(L, any);
  return 1;
}
int l_GetScriptBit(lua_State* L) {
  std::vector<UserUnit*> list;
  ReadUnitList(L, 1, &list);
  const int bit = static_cast<int>(luaL_optnumber(L, 2, 0));
  bool any = false;
  for (UserUnit* u : list) {
    UnitStateView s;
    if (!u->gone && bit >= 0 && bit < 32 && StateOf(L, u, &s) && (s.scriptBits & (1u << bit))) any = true;
  }
  lua_pushboolean(L, any);
  return 1;
}
// GetAttachedUnitsList(units) (FA exe 0x8bc300; read by the engine track, "ui 36"): the units
// hanging on each of the given units (a transport's cargo), in the order they were attached; one
// level; a unit given twice lists its cargo twice. Dead ones, given or hanging, are left out.
// (The original's getter makes a nil, destroyed or foreign entry an error; ours passes over it, as
// our other lists for now.)
int l_GetAttachedUnitsList(lua_State* L) {
  if (lua_gettop(L) != 1) return luaL_error(L, "%s\n  expected %d args, but got %d", "GetAttachedUnitsList", 1, lua_gettop(L));
  std::vector<UserUnit*> out;
  if (lua_istable(L, 1)) {
    UnitStore& store = Store(L);
    const int n = luaL_getn(L, 1);
    for (int i = 1; i <= n; ++i) {
      lua_rawgeti(L, 1, i);
      UserUnit* u = ToUnit(L, lua_gettop(L));
      lua_pop(L, 1);
      if (!u || u->gone || u->view.dead) continue;
      for (uint32_t id : u->view.attachedUnits) {
        auto it = store.units.find(id);
        if (it != store.units.end() && !it->second->gone && !it->second->view.dead) out.push_back(it->second.get());
      }
    }
  }
  PushUnitList(L, out, false);
  return 1;
}
// GetAssistingUnitsList(units) (FA exe 0x8bc820; read by the engine track, "ui 36"): not the units
// guarding or assisting, but the drones of a drone station or carrier: for each given unit that is
// alive, finished and a PODSTAGINGPLATFORM or POD, the player's own living, finished POD units it
// made (their creator), or itself; each once. Within one given unit the original goes by its map
// of places, which the views cannot give: here by entity id (INFERRED stand-in). The game's
// orders.lua gives the first and the second to its two drone buttons. An observer gets none.
int l_GetAssistingUnitsList(lua_State* L) {
  if (lua_gettop(L) != 1) return luaL_error(L, "%s\n  expected %d args, but got %d", "GetAssistingUnitsList", 1, lua_gettop(L));
  std::vector<UserUnit*> out;
  Sim* sim = TheSim(L);
  if (lua_istable(L, 1) && sim && sim->focusArmy >= 0) {
    UnitStore& store = Store(L);
    const int army = sim->focusArmy + 1;
    const int n = luaL_getn(L, 1);
    for (int i = 1; i <= n; ++i) {
      lua_rawgeti(L, 1, i);
      UserUnit* s = ToUnit(L, lua_gettop(L));
      lua_pop(L, 1);
      if (!s || s->gone || s->view.dead || s->view.beingBuilt) continue;
      if (!InCategory(L, s, "PODSTAGINGPLATFORM") && !InCategory(L, s, "POD")) continue;
      for (auto& [id, u] : store.units) {
        if (u->gone || u->view.dead || u->view.beingBuilt || u->view.army != army) continue;
        if (u->view.creator != s->view.id && u.get() != s) continue;
        if (!InCategory(L, u.get(), "POD") || std::find(out.begin(), out.end(), u.get()) != out.end()) continue;
        out.push_back(u.get());
      }
    }
  }
  PushUnitList(L, out, false);
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
    // (FA exe 0x8ba2a0: FilterOut drops a blueprint id that names no blueprint, which FilterDown
    // drops anyway as a member of no category.)
    bool known = true;
    if (!keepMembers && lua_type(L, v) == LUA_TSTRING)
      if (SimBlueprints* bps = SimBlueprints::From(L)) known = bps->Find(lua_tostring(L, v)) != nullptr;
    if (member == keepMembers && known) {
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
// UserUnit:CanAttackTarget(target, rangeCheck) (FA exe 0x8c2090): CanAttackTarget above; the target a unit
// (an error otherwise), rangeCheck true or false (nil: false).
int l_CanAttackTarget(lua_State* L) {
  if (lua_gettop(L) != 3)
    return luaL_error(L, "%s\n  expected %d args, but got %d", "UserUnit:CanAttackTarget(target, rangeCheck)", 3, lua_gettop(L));
  UserUnit* s = CheckUnit(L, 1);
  UserUnit* t = CheckUnit(L, 2);
  lua_pushboolean(L, !t->gone && CanAttackTarget(L, s, t, lua_toboolean(L, 3) != 0));
  return 1;
}
// GetValidAttackingUnits() (FA exe 0x8bd410 / CWldSession::GetValidAttackingUnits 0x896090): the selected units
// that can attack the unit under the mouse (none there: the place under it), checking range; nil for none.
int l_GetValidAttackingUnits(lua_State* L) {
  if (!UserState::From(L)->HasSim()) return 0;
  if (lua_gettop(L) != 0)
    return luaL_error(L, "%s\n  expected %d args, but got %d", "table GetValidAttackingUnits() - return a table of the currently selected units", 0, lua_gettop(L));
  UnitStore& store = Store(L);
  const uint32_t under = UnitUnderMouse(L);
  auto it = under == 0xffffffffu ? store.units.end() : store.units.find(under);
  const UserUnit* t = it != store.units.end() && !it->second->gone ? it->second.get() : nullptr;
  std::vector<UserUnit*> valid;
  for (UserUnit* s : store.selection)
    if (!s->gone && CanAttackTarget(L, s, t, true)) valid.push_back(s);
  PushUnitList(L, valid, true);
  return 1;
}
// GetBlueprint(unit) (FA exe 0x8c59b0, the user state's own): one unit, its blueprint's table.
int l_GetBlueprintGlobal(lua_State* L) {
  if (lua_gettop(L) != 1) return luaL_error(L, "%s\n  expected %d args, but got %d", "blueprint = GetBlueprint()", 1, lua_gettop(L));
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

const std::string* CustomNameOf(lua_State* L, uint32_t id) {
  UnitStore& store = Store(L);
  auto it = store.units.find(id);
  return it == store.units.end() || it->second->gone || it->second->customName.empty() ? nullptr : &it->second->customName;
}

bool AnyCustomNames(lua_State* L) {
  for (const auto& [id, u] : Store(L).units)
    if (!u->gone && !u->customName.empty()) return true;
  return false;
}

std::string SelectionSetsText(lua_State* L, uint32_t id) {
  UnitStore& store = Store(L);
  auto it = store.units.find(id);
  if (it == store.units.end() || it->second->gone || it->second->selectionSets.empty()) return std::string();
  // (The original keeps them in a std::set<std::string>: byte order, each once; FA exe 0x85e490.)
  std::vector<std::string> sets = it->second->selectionSets;
  std::sort(sets.begin(), sets.end());
  sets.erase(std::unique(sets.begin(), sets.end()), sets.end());
  std::string text;
  for (const std::string& s : sets) text += s + " ";
  return text;
}

bool AnySelectionSets(lua_State* L) {
  for (const auto& [id, u] : Store(L).units)
    if (!u->gone && !u->selectionSets.empty()) return true;
  return false;
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
// Position}, Units, Blueprint, and LuaParams when the order carries a Lua value (`luaParams`: its
// stack index, 0 for none): the script reads it for a Script order (an attack-move, an enhancement).
void ReportCommandIssued(lua_State* L, const char* type, const std::vector<UserUnit*>& units, float x, float y, float z,
                         bool clear, const std::string& blueprint, uint32_t entity = 0xffffffffu, int luaParams = 0) {
  int top = lua_gettop(L);
  if (luaParams < 0) luaParams = top + luaParams + 1;
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
      if (luaParams && lua_istable(L, luaParams)) {
        lua_pushstring(L, "LuaParams");
        lua_pushvalue(L, luaParams);
        lua_rawset(L, -3);
      }
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
                const std::string& blueprint, bool ctrl = false) {
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
                 // (orders of their buttons alone: the commander's overcharge, a missile or a
                 // nuke at a place, a teleport; they go out as the order with its target, what
                 // the unit does with it is the simulation's)
                 {"OverCharge", CommandType::OverCharge, false},
                 {"Tactical", CommandType::Tactical, false},
                 {"Nuke", CommandType::Nuke, false},
                 {"Teleport", CommandType::Teleport, false},
                 {"Sacrifice", CommandType::Sacrifice, true},
                 {"Guard", CommandType::Guard, true},
                 {"Repair", CommandType::Repair, true},
                 {"Reclaim", CommandType::Reclaim, true},
                 {"Capture", CommandType::Capture, true}};
  const char* name = nullptr;
  CommandType type = CommandType::None;
  auto same = [](const std::string& a, const char* b) {  // (the order button's name is RULEUCC_Overcharge, the order's OverCharge)
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i)
      if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return i == a.size() && !b[i];
  };
  for (const auto& o : kOrders)
    if (same(order, o.name) && (entity != 0xffffffffu || !o.atUnit)) name = o.name, type = o.type;
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
  // The formation, as the original's interface sends it (the engine track's count over about
  // 1,250 orders of real replays, docs/engine-answers.md "ui 14"): a Move to a place, an Attack on
  // a unit and a Patrol carry formation 0 at scale 1 and a turn about the up axis whose heading
  // is the direction from the MEAN of the units' places to the target, atan2(dx, dz); for one
  // unit alone as well. Everything else carries none, and so does a factory's rally order (the
  // factories' own data below). With it the simulation arranges a group; without, every unit
  // heads for the same spot.
  StreamIssueData rally = d;
  // (A Patrol of one unit carries no formation, as the original's 0x8209c7 and the replays show.)
  const bool forms = type == CommandType::Move || (type == CommandType::Patrol && units.size() > 1) ||
                     (type == CommandType::Attack && entity != 0xffffffffu);
  // Where the units are, or with Shift (an order queued) where each one's last queued order is (CFormation
  // ChooseFormation 0x8384c0 takes the end of each unit's queue when it has one): the mean is the formation's
  // centre, and the first point of a patrol.
  float mean[2] = {0, 0};
  bool queuedPatrol = false;  // (with Shift: a unit whose queue ends in a patrol only gets the new point)
  for (const UserUnit* u : units) {
    float at[2] = {u->view.position[0], u->view.position[2]};
    if (!clear) {
      UnitStateView st;
      if (StateOf(L, u, &st)) {
        const std::vector<CommandView>& q = st.hasFactoryQueue ? st.factoryQueue : st.queue;
        if (!q.empty() && q.back().hasPosition) at[0] = q.back().position[0], at[1] = q.back().position[2];
        if (!q.empty() && (q.back().type == static_cast<int>(CommandType::Patrol) || q.back().type == static_cast<int>(CommandType::FormPatrol))) queuedPatrol = true;
      }
    }
    mean[0] += at[0], mean[1] += at[1];
  }
  mean[0] /= static_cast<float>(units.size()), mean[1] /= static_cast<float>(units.size());
  // Ctrl with more than one unit: an attack on a unit goes as FormAttack (SCommandModeData::HandleEvent 0x81fe4c;
  // FAF's exe leaves a Move as it is: its 0x82074d skips the original's FormMove).
  if (ctrl && units.size() > 1 && type == CommandType::Attack && entity != 0xffffffffu) {
    type = CommandType::FormAttack;
    d.type = static_cast<int>(type);
    name = "FormAttack";
  }
  if (forms) {
    float to[2] = {x, z};
    if (entity != 0xffffffffu) {  // (an Attack on a unit: the target point is the unit's place)
      auto it = Store(L).units.find(entity);
      if (it != Store(L).units.end()) to[0] = it->second->view.position[0], to[1] = it->second->view.position[2];
    }
    const float heading = std::atan2(to[0] - mean[0], to[1] - mean[1]);
    d.formIndex = 0;
    d.scale = 1;
    d.qw = std::cos(heading * 0.5f), d.qx = 0, d.qy = std::sin(heading * 0.5f), d.qz = 0;
  }
  // An order given with a factory selected is the factory's rally order: what the units it builds
  // are told to do (the command stream's IssueFactoryCommand, FA exe 0x6f14d0), kept in a queue
  // of its own. INFERRED: which units count (here: buildings that are factories; a carrier moves
  // itself) and that every order of the world's clicks but a building order goes that way; where
  // the original's interface decides is not read.
  std::vector<uint32_t> factories, others;
  for (UserUnit* u : units)
    (type != CommandType::BuildMobile && InCategory(L, u, "FACTORY") && InCategory(L, u, "STRUCTURE") ? factories : others)
        .push_back(u->view.id);
  if (type == CommandType::BuildMobile && !blueprint.empty()) {  // (remembered until the queues show it: SiteMeetsOrdered)
    UnitStore::SentSite e;
    e.site.blueprint = blueprint;
    e.site.position[0] = x, e.site.position[1] = y, e.site.position[2] = z;
    e.tick = static_cast<uint32_t>(sim->tick());
    if (clear) Store(L).sent.clear();
    Store(L).sent.push_back(e);
  }
  // A Patrol goes as two points (0x820c4e, 0x820c91; factories 0x820d64, as the replays hold them): first where the
  // units are (the mean above, on the ground), clearing the queue unless Shift; then the target, queued. With Shift
  // after a patrol already queued, only the target.
  if (type == CommandType::Patrol && !queuedPatrol) {
    StreamIssueData start = d, startRally = rally;
    float ground = y;
    if (const auto& height = UserState::From(L)->config().groundHeight) ground = height(mean[0], mean[1]);
    start.target.type = startRally.target.type = 2;
    start.target.pos.x = startRally.target.pos.x = mean[0];
    start.target.pos.y = startRally.target.pos.y = ground;
    start.target.pos.z = startRally.target.pos.z = mean[1];
    if (!others.empty()) Writer(L).IssueCommand(others, start, clear);
    if (!factories.empty()) Writer(L).IssueFactoryCommand(factories, startRally, clear);
    clear = false;
  }
  if (!others.empty()) Writer(L).IssueCommand(others, d, clear);
  if (!factories.empty()) Writer(L).IssueFactoryCommand(factories, rally, clear);
  ReportCommandIssued(L, name, units, x, y, z, clear, blueprint, entity);
  return true;
}
}  // namespace

bool IssueToSelection(lua_State* L, const std::string& order, float x, float y, float z, bool clear, const std::string& blueprint) {
  return IssueOrder(L, order, x, y, z, 0xffffffffu, clear, blueprint);
}

// A Script order at a place: what a left click does while the script's order mode is
// RULEUCC_Script (FAF's attack-move, an ability). The game's own script decides whether and with
// what the order goes out: `/lua/user/userscriptcommand.lua` VerifyScriptCommand(command) answers
// with the order's Lua value ({TaskName, Location, UserValidated, ...}) and the units it may go to
// (AuthorizedUnits, which is not sent along); without UserValidated nothing is issued. Real
// replays hold the order so: type Script, the place as its target, the table as its Lua value,
// no formation. (How the original builds the table it hands the script is not read: it gets the
// same one OnCommandIssued gets.)
bool IssueScriptAt(lua_State* L, float x, float y, float z, bool clear) {
  Sim* sim = TheSim(L);
  if (!sim) return false;
  std::vector<uint32_t> ids;
  std::vector<UserUnit*> units;
  for (UserUnit* u : Store(L).selection)
    if (!u->gone && !u->view.dead && u->view.army == sim->focusArmy + 1) ids.push_back(u->view.id), units.push_back(u);
  if (ids.empty()) return false;
  const int top = lua_gettop(L);
  lua_pushcfunction(L, ScriptTraceback);
  lua_getglobal(L, "import");
  lua_pushstring(L, "/lua/user/userscriptcommand.lua");
  if (lua_pcall(L, 1, 1, top + 1) != 0 || !lua_istable(L, -1)) {
    lua_settop(L, top);
    return false;
  }
  lua_pushstring(L, "VerifyScriptCommand");
  lua_gettable(L, -2);
  lua_newtable(L);  // the command, as the script's OnCommandIssued gets it
  lua_pushstring(L, "CommandType");
  lua_pushstring(L, "Script");
  lua_rawset(L, -3);
  lua_pushstring(L, "Clear");
  lua_pushboolean(L, clear);
  lua_rawset(L, -3);
  lua_pushstring(L, "Units");
  PushUnitList(L, units, false);
  lua_rawset(L, -3);
  lua_pushstring(L, "Target");
  lua_newtable(L);
  lua_pushstring(L, "Type");
  lua_pushstring(L, "Position");
  lua_rawset(L, -3);
  lua_pushstring(L, "Position");
  PushVector(L, x, y, z);
  lua_rawset(L, -3);
  lua_rawset(L, -3);
  if (!lua_isfunction(L, -2) || lua_pcall(L, 1, 1, top + 1) != 0 || !lua_istable(L, -1)) {
    if (lua_isstring(L, -1)) Logf(LogLevel::Warning, "Error running '/lua/user/userscriptcommand.lua:VerifyScriptCommand': %s", lua_tostring(L, -1));
    lua_settop(L, top);
    return true;  // (the click is used up all the same)
  }
  const int result = lua_gettop(L);
  lua_pushstring(L, "UserValidated");
  lua_gettable(L, result);
  const bool valid = lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);
  if (!valid) {
    lua_settop(L, top);
    return true;
  }
  lua_pushstring(L, "AuthorizedUnits");
  lua_gettable(L, result);
  if (lua_istable(L, -1)) {
    std::vector<UserUnit*> listed, allowed;
    ReadUnitList(L, lua_gettop(L), &listed);
    ids.clear();
    for (UserUnit* u : listed)
      if (!u->gone && !u->view.dead && u->view.army == sim->focusArmy + 1) ids.push_back(u->view.id), allowed.push_back(u);
    units = allowed;
  }
  lua_pop(L, 1);
  lua_pushstring(L, "AuthorizedUnits");
  lua_pushnil(L);
  lua_settable(L, result);
  if (ids.empty()) {
    lua_settop(L, top);
    return true;
  }
  StreamIssueData d;
  d.type = static_cast<int>(CommandType::Script);
  d.target.type = 2;
  d.target.pos.x = x, d.target.pos.y = y, d.target.pos.z = z;
  SerializeLua(L, result, &d.lua);
  Writer(L).IssueCommand(ids, d, clear);
  ReportCommandIssued(L, "Script", units, x, y, z, clear, std::string(), 0xffffffffu, result);
  lua_settop(L, top);
  return true;
}

// FAF's attack-move by Alt and a right click: the same Script order as its button's, with the
// Lua value real replays hold for it ({TaskName = "AttackMove", UserValidated = true, Location}).
// INFERRED: FAF's patched program makes the order itself for this click (no order mode is on, so
// the verifying script is not the one to ask); which of its code does it is not read.
bool IssueAttackMoveAt(lua_State* L, float x, float y, float z, bool clear) {
  Sim* sim = TheSim(L);
  if (!sim) return false;
  std::vector<uint32_t> ids;
  std::vector<UserUnit*> units;
  for (UserUnit* u : Store(L).selection)
    if (!u->gone && !u->view.dead && u->view.army == sim->focusArmy + 1) ids.push_back(u->view.id), units.push_back(u);
  if (ids.empty()) return false;
  const int top = lua_gettop(L);
  lua_newtable(L);
  lua_pushstring(L, "TaskName");
  lua_pushstring(L, "AttackMove");
  lua_rawset(L, -3);
  lua_pushstring(L, "UserValidated");
  lua_pushboolean(L, 1);
  lua_rawset(L, -3);
  lua_pushstring(L, "Location");
  PushVector(L, x, y, z);
  lua_rawset(L, -3);
  StreamIssueData d;
  d.type = static_cast<int>(CommandType::Script);
  d.target.type = 2;
  d.target.pos.x = x, d.target.pos.y = y, d.target.pos.z = z;
  SerializeLua(L, top + 1, &d.lua);
  Writer(L).IssueCommand(ids, d, clear);
  ReportCommandIssued(L, "Script", units, x, y, z, clear, std::string(), 0xffffffffu, top + 1);
  lua_settop(L, top);
  return true;
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

// The right click's transport orders, issued as the original issues them:
// - "CallTransport" (FA exe 0x820e01): TransportLoadUnits aimed at the hovered unit, the factories among the
//   selection (RALLYPOINT, split by 0x81e610) as their factory order, the rest as theirs; the hovered transport
//   is added to the units ordered (INFERRED: so that it comes to meet them) unless it is an air staging platform
//   or a ferry beacon that is no carrier.
// - "Transport" (0x821014): a teleport beacon with a TELEPORTATION unit selected: TransportUnloadUnits aimed at
//   the beacon; else TransportReverseLoadUnits aimed at the hovered unit, for the whole selection.
// The queue is cleared unless Shift is held.
bool IssueTransportOrderAt(lua_State* L, const std::string& order, uint32_t entity, bool clear) {
  Sim* sim = TheSim(L);
  if (!sim) return false;
  UnitStore& store = Store(L);
  auto it = store.units.find(entity);
  if (it == store.units.end() || it->second->gone) return false;
  UserUnit* hover = it->second.get();
  std::vector<UserUnit*> units;
  for (UserUnit* u : store.selection)
    if (!u->gone && !u->view.dead && u->view.army == sim->focusArmy + 1) units.push_back(u);
  if (units.empty()) return false;
  StreamIssueData d;
  d.target.type = 1;
  d.target.entity = entity;
  d.target.pos.x = hover->view.position[0], d.target.pos.y = hover->view.position[1], d.target.pos.z = hover->view.position[2];
  std::vector<uint32_t> factories, others;
  const char* name = nullptr;
  if (order == "CallTransport") {
    d.type = static_cast<int>(CommandType::TransportLoadUnits);
    name = "TransportLoadUnits";
    for (UserUnit* u : units) (InCategory(L, u, "RALLYPOINT") ? factories : others).push_back(u->view.id);
    const bool joins = !InCategory(L, hover, "AIRSTAGINGPLATFORM") &&
                       !(InCategory(L, hover, "FERRYBEACON") && !InCategory(L, hover, "CARRIER"));
    if (joins && std::find(others.begin(), others.end(), entity) == others.end()) others.push_back(entity);
  } else if (order == "Transport") {
    bool teleporter = false;
    for (UserUnit* u : units) teleporter = teleporter || InCategory(L, u, "TELEPORTATION");
    if (InCategory(L, hover, "TELEPORTBEACON") && teleporter) {
      d.type = static_cast<int>(CommandType::TransportUnloadUnits);
      name = "TransportUnloadUnits";
    } else if (hover->view.commandCaps & 0x200) {
      d.type = static_cast<int>(CommandType::TransportReverseLoadUnits);
      name = "TransportReverseLoadUnits";
    } else {
      return false;
    }
    for (UserUnit* u : units) others.push_back(u->view.id);
  } else {
    return false;
  }
  if (!others.empty()) Writer(L).IssueCommand(others, d, clear);
  if (!factories.empty()) Writer(L).IssueFactoryCommand(factories, d, clear);
  ReportCommandIssued(L, name, units, d.target.pos.x, d.target.pos.y, d.target.pos.z, clear, std::string(), entity);
  return true;
}

bool IssueToSelectionAt(lua_State* L, const std::string& order, uint32_t entity, bool clear, bool ctrl) {
  UnitStore& store = Store(L);
  auto it = store.units.find(entity);
  if (it == store.units.end() || it->second->gone) return false;
  const float* p = it->second->view.position;
  return IssueOrder(L, order, p[0], p[1], p[2], entity, clear, std::string(), ctrl);
}

namespace {
// The reclaimable thing at a place, if the selection holds a unit of the player's that can reclaim.
bool ReclaimTargetAt(lua_State* L, const float at[3], float within, ReclaimableView* prop) {
  Sim* sim = TheSim(L);
  if (!sim) return false;
  UnitStore& store = Store(L);
  const uint32_t kReclaim = 0x100000;
  uint32_t caps = 0;
  for (UserUnit* u : store.selection)
    if (!u->gone && !u->view.dead && u->view.army == sim->focusArmy + 1) caps |= u->view.commandCaps;
  return (caps & kReclaim) && ReclaimableAt(*sim, at, within, prop);
}
}  // namespace

bool IssueReclaimAtPlace(lua_State* L, const float at[3], float within, bool clear) {
  ReclaimableView prop;
  if (!ReclaimTargetAt(L, at, within, &prop)) return false;
  return IssueOrder(L, "Reclaim", prop.position[0], prop.position[1], prop.position[2], prop.id, clear, std::string());
}
bool CanReclaimAtPlace(lua_State* L, const float at[3], float within) {
  // (Asked by the cursor's script again and again with the mouse still: the answer is kept while
  // the place, the reach, the selection and the beat are the same. ReclaimableAt looks at every
  // prop near the place: about a millisecond in a forest.)
  UnitStore& store = Store(L);
  Sim* sim = TheSim(L);
  const uint32_t tick = sim ? sim->tick() : 0;
  if (store.reclaimTick == tick && store.reclaimFor == store.selection && store.reclaimAt[0] == at[0] &&
      store.reclaimAt[2] == at[2] && store.reclaimWithin == within)
    return store.reclaimAnswer;
  ReclaimableView prop;
  store.reclaimAnswer = ReclaimTargetAt(L, at, within, &prop);
  store.reclaimTick = tick, store.reclaimFor = store.selection, store.reclaimWithin = within;
  store.reclaimAt[0] = at[0], store.reclaimAt[1] = at[1], store.reclaimAt[2] = at[2];
  return store.reclaimAnswer;
}

// moho64ui_ReclaimableAt(x, z, within) -> nil, or {id, blueprint, x, z, mass, energy} of the
// tree, rock or wreck the simulation finds there: the host's own function for tests.
int l_ReclaimableAtForTests(lua_State* L) {
  Sim* sim = TheSim(L);
  ReclaimableView prop;
  const float at[3] = {static_cast<float>(luaL_checknumber(L, 1)), 0, static_cast<float>(luaL_checknumber(L, 2))};
  if (!sim || !ReclaimableAt(*sim, at, static_cast<float>(luaL_optnumber(L, 3, 0)), &prop)) {
    lua_pushnil(L);
    return 1;
  }
  lua_newtable(L);
  auto number = [&](const char* k, double d) {
    lua_pushstring(L, k);
    lua_pushnumber(L, d);
    lua_rawset(L, -3);
  };
  number("id", prop.id), number("x", prop.position[0]), number("z", prop.position[2]), number("mass", prop.mass), number("energy", prop.energy);
  lua_pushstring(L, "blueprint");
  lua_pushstring(L, prop.blueprint.c_str());
  lua_rawset(L, -3);
  return 1;
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
// transport and ferry cases (0x81d280, 0x81d660, 0x81da20),
// an attack order's marker under the cursor, the option "switch_right_click_behavior". Things that
// are not units (wreckage, trees, rocks) are the world view's to ask for: IssueReclaimAtPlace.
// The blueprint's Air.CanFly (bp +0x368, the Air block's first field), kept by blueprint.
bool CanFlyOf(lua_State* L, const UserUnit* u) {
  UnitStore& store = Store(L);
  auto it = store.canFly.find(u->view.blueprint);
  if (it != store.canFly.end()) return it->second;
  bool fly = false;
  const int top = lua_gettop(L);
  PushBlueprint(L, u->view.blueprint);
  if (lua_istable(L, -1)) {
    lua_pushstring(L, "Air");
    lua_rawget(L, -2);
    if (lua_istable(L, -1)) {
      lua_pushstring(L, "CanFly");
      lua_rawget(L, -2);
      fly = lua_toboolean(L, -1) && !(lua_isnumber(L, -1) && lua_tonumber(L, -1) == 0);
    }
  }
  lua_settop(L, top);
  store.canFly[u->view.blueprint] = fly;
  return fly;
}

// The selection gets on the hovered transport (FA exe 0x81d280): it lives, is not on the seabed and is not
// being built; one selected unit that lives and is finished qualifies (a commander only for a transport of
// CANTRANSPORTCOMMANDER or a ferry beacon): one that cannot fly for a TRANSPORTATION, TELEPORTATION or
// FERRYBEACON, one that flies (and may use air staging) for an AIRSTAGINGPLATFORM. Neither cargo nor free
// room is asked.
bool CanBoard(lua_State* L, const std::vector<UserUnit*>& selection, const UserUnit* hover) {
  if (!hover || hover->view.dead || hover->view.layer == 2 || hover->view.beingBuilt) return false;
  const bool carrier = InCategory(L, hover, "TRANSPORTATION") || InCategory(L, hover, "TELEPORTATION") ||
                       InCategory(L, hover, "FERRYBEACON");
  const bool staging = InCategory(L, hover, "AIRSTAGINGPLATFORM");
  const bool takesCommander = InCategory(L, hover, "CANTRANSPORTCOMMANDER") || InCategory(L, hover, "FERRYBEACON");
  for (const UserUnit* u : selection) {
    if (u->view.dead || u->view.beingBuilt) continue;
    if (InCategory(L, u, "COMMAND") && !takesCommander) continue;
    if (carrier && !CanFlyOf(L, u)) return true;
    if (!carrier && staging && CanFlyOf(L, u) && !InCategory(L, u, "CANNOTUSEAIRSTAGING")) return true;
  }
  return false;
}

// A selected transport picks up the hovered unit (FA exe 0x81d660): the hovered unit lives and is finished
// and is no experimental; a selected unit qualifies that is TRANSPORTFOCUS, not TELEPORTATION, not on the
// seabed (and CANTRANSPORTCOMMANDER for a commander): a TRANSPORTATION or FERRYBEACON for a unit that cannot
// fly, an AIRSTAGINGPLATFORM for one that flies.
bool CanPickUp(lua_State* L, const UserUnit* hover, const std::vector<UserUnit*>& selection) {
  if (!hover || hover->view.dead || hover->view.beingBuilt || InCategory(L, hover, "EXPERIMENTAL")) return false;
  const bool commander = InCategory(L, hover, "COMMAND");
  for (const UserUnit* s : selection) {
    if (InCategory(L, s, "TELEPORTATION") || s->view.layer == 2 || !InCategory(L, s, "TRANSPORTFOCUS")) continue;
    if (commander && !InCategory(L, s, "CANTRANSPORTCOMMANDER")) continue;
    if ((InCategory(L, s, "TRANSPORTATION") || InCategory(L, s, "FERRYBEACON")) && !CanFlyOf(L, hover)) return true;
    if (InCategory(L, s, "AIRSTAGINGPLATFORM") && CanFlyOf(L, hover)) return true;
  }
  return false;
}

// Every selected unit is a factory (FA exe 0x81da20: the blueprint's FACTORY bit).
bool AllFactories(lua_State* L, const std::vector<UserUnit*>& selection) {
  for (const UserUnit* u : selection)
    if (!InCategory(L, u, "FACTORY")) return false;
  return !selection.empty();
}

std::string RightClickOrder(lua_State* L, uint32_t under) {
  Sim* sim = TheSim(L);
  if (!sim) return std::string();
  UnitStore& store = Store(L);
  int army = sim->focusArmy + 1;
  uint32_t caps = 0;  // (the beat's copies: engine ui 26)
  std::vector<UserUnit*>& own = store.capsOwn;
  own.clear();
  for (UserUnit* u : store.selection) {
    if (u->gone || u->view.dead || u->view.army != army) continue;
    caps |= u->view.commandCaps;
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
  // (IsEnemy 0x5d5540 and IsAlly 0x5bd630 are both false when either army is missing; one's own army counts as an ally.)
  bool noArmy = e->view.army <= 0, enemy = stance == 0 && !noArmy, ally = stance == 2 && !noArmy;
  // (An enemy: Attack when any of the selection can attack it, by its weapons (FA exe 0x81d080).)
  if (enemy && (caps & kAttack) && AnyCanAttack(L, own, e)) return "RULEUCC_Attack";
  UnitStateView target;
  StateOf(L, e, &target);
  // (A unit that hangs on another (loaded, docked) can be neither captured nor reclaimed: the original
  // clears both when SSTIUnitVariableData+0xA is set, 0x81ec00.)
  // Capture (0x81ec00, before Reclaim): not an ally (so not one's own), not being built, the unit's own
  // switch (Unit:SetCapturable, attributes +0x69; on unless a script turned it off), and the selection can
  // capture (RULEUCC_Capture 0x80). A neutral unit qualifies.
  const uint32_t kCapture = 0x80;
  if (!ally && !e->view.beingBuilt && target.capturable && !e->view.attached && (caps & kCapture)) return "RULEUCC_Capture";
  // (The unit's own switch, SetReclaimable, on top of the original's test: a unit a script has
  // made unreclaimable is not offered for reclaim. Inferred, not read.)
  bool reclaimable = (InCategory(L, e, "RECLAIMABLE") || e->view.beingBuilt) && target.reclaimable && !e->view.attached;
  if ((enemy || noArmy || InCategory(L, e, "RECLAIMFRIENDLY")) && reclaimable && (caps & kReclaim)) return "RULEUCC_Reclaim";
  // The transports (0x81ec00, only for a unit of the player's own army, not an ally's): the selection gets
  // on the hovered transport (0x81d280), a selected transport picks up the hovered unit (0x81d660), or
  // factories send what they build onto a ferry beacon (0x81da20).
  if (e->view.army == army) {
    const uint32_t kCallTransport = 0x200;
    if ((caps & kCallTransport) && CanBoard(L, own, e)) return "RULEUCC_CallTransport";
    if ((e->view.commandCaps & kCallTransport) && CanPickUp(L, e, own)) return "RULEUCC_Transport";
    if (InCategory(L, e, "FERRYBEACON") && AllFactories(L, own)) return "RULEUCC_CallTransport";
  }
  // (An enemy none of the selection can attack nor reclaim: with the Attack order among the selection's, the
  // invalid attack (the script's attack-invalid cursor), and a right click gives no order (GetRightMouseButtonAction
  // 0x81ec00, step 4); else a move.)
  if (enemy) return (caps & kAttack) ? "RULEUCC_Invalid" : move;
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
const char* CommandName(CommandType type) {
  for (const auto& c : kCommandNames)
    if (c.type == type) return c.name;
  return nullptr;
}

// A value of the game's table of how orders are drawn (/lua/ui/game/commandgraphparams.lua,
// CommandGraphParams): the entry of the order ("UNITCOMMAND_Move"), else of the entry its
// inherit_from names, and so on; else the entry "default". Empty: the table has none.
std::string CommandGraphParam(lua_State* L, const std::string& command, const char* key) {
  UnitStore& store = Store(L);
  const std::string cacheKey = command + "/" + key;
  auto cached = store.graphParams.find(cacheKey);
  if (cached != store.graphParams.end()) return cached->second;
  std::string value;
  const int top = lua_gettop(L);
  lua_getglobal(L, "import");
  lua_pushstring(L, "/lua/ui/game/commandgraphparams.lua");
  if (lua_pcall(L, 1, 1, 0) == 0 && lua_istable(L, -1)) {
    lua_pushstring(L, "CommandGraphParams");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
      const int table = lua_gettop(L);
      std::string entry = command;
      for (int hops = 0; hops < 8 && !entry.empty(); ++hops) {
        lua_pushstring(L, entry.c_str());
        lua_gettable(L, table);
        std::string next = entry == "default" ? "" : "default";
        bool found = false;
        if (lua_istable(L, -1)) {
          lua_pushstring(L, key);
          lua_gettable(L, -2);
          if (lua_isstring(L, -1) || lua_isnumber(L, -1)) value = lua_tostring(L, -1), found = true;
          lua_pop(L, 1);
          lua_pushstring(L, "inherit_from");
          lua_gettable(L, -2);
          if (lua_isstring(L, -1)) next = lua_tostring(L, -1);
          lua_pop(L, 1);
        }
        lua_pop(L, 1);
        if (found) break;
        entry = next;
      }
    }
  }
  lua_settop(L, top);
  store.graphParams[cacheKey] = value;
  return value;
}

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
  ReportCommandIssued(L, plain, units, 0, 0, 0, clear, blueprint, 0xffffffffu, params && lua_istable(L, params) ? params : 0);
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
// unit:ProcessInfo(key, value): the same for one unit, with the key and value the script names
// (the construction panel's infinite-build button: 'SetRepeatQueue', 'true' | 'false'; FAF's
// external factories: 'SetPaused'). It was a placeholder that did nothing until Chris,
// 2026-10-13: "infinite build for factories doesn't work when I click it then click on a unit
// to infinite build it turns off": the simulation never heard of the switch, so the panel read
// it back as off when it was next laid out.
int l_UnitProcessInfo(lua_State* L) {
  UserUnit* u = CheckUnit(L, 1);
  const std::string key = luaL_checkstring(L, 2), value = luaL_checkstring(L, 3);
  Sim* sim = TheSim(L);
  if (!sim || u->gone || u->view.dead || u->view.army != sim->focusArmy + 1) return 0;
  Writer(L).ProcessInfoPair(u->view.id, key, value);
  return 0;
}
// SetPaused(units, paused), SetAutoMode(units, on), SetFireState(units, state),
// ToggleFireState(units, currentState), ToggleScriptBit(units, bit, currentState)
int l_SetPaused(lua_State* L) {
  // (Each call is written to the log: a pause is rare, and "I pressed pause and it went on
  // building" can then be read back: who was paused, when, and whether a second call undid it.)
  std::vector<UserUnit*> list;
  ReadUnitList(L, 1, &list);
  std::string names;
  for (const UserUnit* u : list) names += (names.empty() ? "" : " ") + u->view.blueprint;
  Sim* sim = TheSim(L);
  Logf(LogLevel::Info, "moho64ui: SetPaused(%s) at tick %u for %d unit(s): %s", lua_toboolean(L, 2) ? "true" : "false",
       sim ? static_cast<unsigned>(sim->tick()) : 0u, static_cast<int>(list.size()), names.c_str());
  InfoPair(L, 1, "SetPaused", lua_toboolean(L, 2) ? "true" : "false");
  return 0;
}
int l_SetAutoMode(lua_State* L) {
  InfoPair(L, 1, "SetAutoMode", lua_toboolean(L, 2) ? "true" : "false");
  return 0;
}
// SetAutoSurfaceMode(units, on) (FA exe 0x8bb360; its help text is GetIsAutoSurfaceMode's): the
// same for a submarine's auto-surface switch, the info pair SetAutoSurfaceMode "true" | "false".
int l_SetAutoSurfaceMode(lua_State* L) {
  InfoPair(L, 1, "SetAutoSurfaceMode", lua_toboolean(L, 2) ? "true" : "false");
  return 0;
}
// GetIsSubmerged(units) (FA exe 0x8bafe0; orders.lua's dive button): -1 when every unit is in the
// Sub layer, 1 when none is, 0 when they differ or there are none (each user unit's layer,
// UserUnit+0xf0; the engine's UnitView::layer, ui request 28).
int l_GetIsSubmerged(lua_State* L) {
  std::vector<UserUnit*> list;
  ReadUnitList(L, 1, &list);
  int state = 0;
  for (size_t i = 0; i < list.size(); ++i) {
    const int one = list[i]->view.layer == 4 ? -1 : 1;
    if (i == 0) state = one;
    else if (one != state) {
      state = 0;
      break;
    }
  }
  lua_pushnumber(L, state);
  return 1;
}

// IssueDockCommand(clear) (FA exe 0x840a70, read by a helper; the Dock order's button and FAF's
// dock-damaged key): the selected units that can dock (RULEUCC_Dock, 0x400000) go to the player's
// own air staging platforms. A platform counts when it is finished and alive, in category
// AIRSTAGINGPLATFORM, not under water (layer Seabed or Sub), not hanging on a parent, with nothing
// in its queue, and with room: StorageSlots - what is attached to it, or DockingSlots when it
// stores none (docked aircraft are not counted against those, as the original's). Platforms by
// their distance from the units' middle (each unit's place, or with `clear` false where its last
// order goes): all go to the nearest when it has room for all; else the platforms within 100 of the
// nearest's distance share them by their room (each takes ceil(room * max(1, units / all their
// room)), the roomiest first), one Dock order (39, the platform as its target) per platform.
// Without a platform the first selected unit's army hears PlayNoStagingPlatformsVO, with none free
// PlayBusyStagingPlatformsVO. Ours: units in the order of their ids (the original's: memory
// addresses), and a last order without a place counts as the unit's own place (the original's
// gives an invalid point there, and then no order at all).
int l_IssueDockCommand(lua_State* L) {
  const bool clear = lua_toboolean(L, 1) != 0;
  UserState* user = UserState::From(L);
  const int focus = user->economyCopied ? user->focusArmyCopy : -1;
  if (focus < 0) return 0;
  UnitStore& store = Store(L);
  std::vector<UserUnit*> selected;
  for (UserUnit* u : store.selection)
    if (!u->gone && !u->view.dead) selected.push_back(u);
  std::sort(selected.begin(), selected.end(), [](const UserUnit* a, const UserUnit* b) { return a->view.id < b->view.id; });
  std::vector<UserUnit*> docking;
  float cx = 0, cz = 0;
  for (UserUnit* u : selected) {
    if (!(u->view.commandCaps & 0x400000u)) continue;
    float p[3] = {u->view.position[0], u->view.position[1], u->view.position[2]};
    UnitStateView st;
    if (!clear && StateOf(L, u, &st) && !st.queue.empty()) {
      const CommandView& last = st.queue.back();
      if (last.hasPosition) {
        std::copy(last.position, last.position + 3, p);
      } else if (last.entity != 0xffffffffu) {
        auto it = store.units.find(last.entity);
        if (it != store.units.end()) std::copy(it->second->view.position, it->second->view.position + 3, p);
      }
    }
    cx += p[0], cz += p[2];
    docking.push_back(u);
  }
  if (docking.empty()) return 0;
  cx /= static_cast<float>(docking.size()), cz /= static_cast<float>(docking.size());
  struct Platform {
    uint32_t id;
    float distance2;
    int room;
  };
  std::vector<Platform> platforms;
  bool foundAny = false, foundFree = false;
  for (const auto& [id, u] : store.units) {
    if (u->gone || u->view.army != focus + 1 || u->view.beingBuilt || u->view.dead) continue;
    if (!InCategory(L, u.get(), "AIRSTAGINGPLATFORM")) continue;
    foundAny = true;
    if (u->view.layer == 2 || u->view.layer == 4 || u->view.attached || u->view.queueLength != 0) continue;
    int storage = 0, docking = 0;
    int top = lua_gettop(L);
    PushBlueprint(L, u->view.blueprint);
    if (lua_istable(L, -1)) {
      lua_pushstring(L, "Transport");
      lua_rawget(L, -2);
      if (lua_istable(L, -1)) {
        lua_pushstring(L, "StorageSlots");
        lua_rawget(L, -2);
        storage = lua_isnumber(L, -1) ? static_cast<int>(lua_tonumber(L, -1)) : 0;
        lua_pop(L, 1);
        lua_pushstring(L, "DockingSlots");
        lua_rawget(L, -2);
        docking = lua_isnumber(L, -1) ? static_cast<int>(lua_tonumber(L, -1)) : 0;
        lua_pop(L, 1);
      }
    }
    lua_settop(L, top);
    const int room = storage > 0 ? storage - u->view.attachedCount : docking;
    if (room <= 0) continue;
    foundFree = true;
    const float dx = u->view.position[0] - cx, dz = u->view.position[2] - cz;
    platforms.push_back({id, dx * dx + dz * dz, room});
  }
  auto dock = [&](const std::vector<uint32_t>& ids, uint32_t platform) {
    StreamIssueData d;
    d.type = 39;  // Dock
    d.target.type = 1;
    d.target.entity = platform;
    Writer(L).IssueCommand(ids, d, clear);
  };
  if (!platforms.empty()) {
    std::sort(platforms.begin(), platforms.end(), [](const Platform& a, const Platform& b) {
      return a.distance2 < b.distance2 || (a.distance2 == b.distance2 && a.id < b.id);
    });
    if (static_cast<int>(docking.size()) <= platforms[0].room) {
      std::vector<uint32_t> ids;
      for (const UserUnit* u : docking) ids.push_back(u->view.id);
      dock(ids, platforms[0].id);
    } else {
      const float reach = std::sqrt(platforms[0].distance2) + 100.0f;
      std::vector<Platform> near;
      int allRoom = 0;
      for (const Platform& p : platforms)
        if (p.distance2 < reach * reach) near.push_back(p), allRoom += p.room;
      const float factor = std::max(1.0f, static_cast<float>(docking.size()) / static_cast<float>(allRoom));
      std::sort(near.begin(), near.end(), [](const Platform& a, const Platform& b) {
        return a.room > b.room || (a.room == b.room && (a.distance2 < b.distance2 || (a.distance2 == b.distance2 && a.id < b.id)));
      });
      size_t next = 0;
      for (const Platform& p : near) {
        const int quota = static_cast<int>(std::ceil(static_cast<float>(p.room) * factor));
        std::vector<uint32_t> ids;
        while (next < docking.size() && static_cast<int>(ids.size()) < quota) ids.push_back(docking[next++]->view.id);
        if (ids.empty()) return 0;  // (the units ran out: no more orders, no voice; the original's)
        dock(ids, p.id);
      }
    }
  }
  if (!selected.empty() && (!foundAny || !foundFree))
    Writer(L).ProcessInfoPair(selected.front()->view.id, foundAny ? "PlayBusyStagingPlatformsVO" : "PlayNoStagingPlatformsVO", "play");
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
// (FA exe 0x8bbe60: only the units whose toggles have the bit and whose bit is now curState.)
int l_ToggleScriptBit(lua_State* L) {
  const int bit = static_cast<int>(luaL_checknumber(L, 2));
  const bool cur = lua_toboolean(L, 3) != 0;
  std::vector<UserUnit*> list, which;
  ReadUnitList(L, 1, &list);
  for (UserUnit* u : list) {
    UnitStateView s;
    if (u->view.dead || bit < 0 || bit >= 32 || !(u->view.toggleCaps & (1u << bit)) || !StateOf(L, u, &s)) continue;
    if (((s.scriptBits >> bit & 1u) != 0) == cur) which.push_back(u);
  }
  std::vector<uint32_t> ids;
  std::vector<UserUnit*> units;
  OwnIds(L, which, &ids, &units);
  for (uint32_t id : ids) Writer(L).ProcessInfoPair(id, "ToggleScriptBit", std::to_string(bit));
  return 0;
}


// --- unit states (the engine's UnitStateOf, sim/sim_view.h) ---
//
// What the panels show of a unit beyond where it is: its queue, what it works on, its switches
// and meters. Asked of the simulation when a script asks, for the units it asks about.

// A unit's state changes only in a beat: it is kept until the next beat's sync comes (the panels
// ask the same units every frame; each answer runs in the simulation's Lua), and while a beat runs
// on its own thread the state from before it is the answer (no wait for the beat).
bool StateOf(lua_State* L, const UserUnit* u, UnitStateView* out) {
  if (!u || u->gone) return false;
  UserState* user = UserState::From(L);
  const bool beat = user->config().beatRunning && user->config().beatRunning();
  if (u->stateSync != ~0ull && (beat || u->stateSync == user->syncCount)) {
    *out = u->stateCopy;
    return true;
  }
  Sim* sim = TheSim(L);
  if (!sim || !UnitStateOf(*sim, u->view.id, out)) return false;
  u->stateCopy = *out, u->stateSync = user->syncCount;
  return true;
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
  // A unit that works on itself (a commander's enhancement: work under way and nothing it works
  // on): `focusUpgrade`, by which the game's unit panel shows the work's per cent. INFERRED: the
  // original's table for it is not read (0x8421f0); the script only asks whether it is there.
  if (withFocus && s.focus == 0xffffffffu && u->view.workProgress > 0) {
    lua_pushstring(L, "focusUpgrade");
    lua_newtable(L);
    lua_pushstring(L, "workProgress");
    lua_pushnumber(L, u->view.workProgress);
    lua_rawset(L, -3);
    lua_rawset(L, -3);
  }
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
int RolloverInfoNow(lua_State* L);
// GetRolloverInfo(): asked by the hover panel every frame; while a beat runs on its own thread,
// the answer from before it when the mouse is on the same unit (no wait for the beat).
int l_GetRolloverInfo(lua_State* L) {
  UserState* user = UserState::From(L);
  UnitStore& store = Store(L);
  const uint32_t under = UnitUnderMouse(L);
  if (user->config().beatRunning && user->config().beatRunning() && store.rolloverKnown && store.rolloverId == under) {
    if (store.rolloverRef == LUA_NOREF) return 0;
    lua_rawgeti(L, LUA_REGISTRYINDEX, store.rolloverRef);
    return 1;
  }
  const int results = RolloverInfoNow(L);
  if (store.rolloverRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, store.rolloverRef);
  store.rolloverRef = LUA_NOREF;
  if (results > 0) {
    lua_pushvalue(L, -1);
    store.rolloverRef = luaL_ref(L, LUA_REGISTRYINDEX);
  }
  store.rolloverId = under, store.rolloverKnown = true;
  return results;
}
int RolloverInfoNow(lua_State* L) {
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

// The switches of a list of units, as the order buttons ask (as the original's, read by the engine
// track, 2026-10-16). GetFireState(units) (FA exe 0x8bb560): the state the units with the Retaliate
// toggle share; -1 when they differ or none of them has it. GetIsPaused (0x8baa70): true when any
// is paused. GetIsAutoMode, GetIsAutoSurfaceMode (0x8bac20, 0x8bb1a0): true when all have it on,
// and for an empty list. GetScriptBit (0x8bbc10): among the units whose toggles have that bit,
// true when any has it on.
const uint32_t kRetaliateToggle = 1u << 5;  // (RULEUCC_RetaliateToggle in the units' order bits)
int l_GetFireStateNow(lua_State* L) {
  std::vector<UserUnit*> list;
  ReadUnitList(L, 1, &list);
  int state = -2;
  for (UserUnit* u : list) {
    UnitStateView s;
    if (u->view.dead || !(u->view.commandCaps & kRetaliateToggle) || !StateOf(L, u, &s)) continue;
    state = state == -2 || state == s.fireState ? s.fireState : -1;
  }
  lua_pushnumber(L, state == -2 ? -1 : state);
  return 1;
}
template <bool UnitStateView::*Field>
int l_AllSwitch(lua_State* L) {
  std::vector<UserUnit*> list;
  ReadUnitList(L, 1, &list);
  bool all = true;
  for (UserUnit* u : list) {
    UnitStateView s;
    if (!u->view.dead && StateOf(L, u, &s) && !(s.*Field)) all = false;
  }
  lua_pushboolean(L, all);
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
// GetScriptBit(units, bit): whether any of the units with that toggle has it on
int l_GetScriptBitNow(lua_State* L) {
  std::vector<UserUnit*> list;
  ReadUnitList(L, 1, &list);
  int bit = static_cast<int>(luaL_checknumber(L, 2));
  bool any = false;
  for (UserUnit* u : list) {
    UnitStateView s;
    if (bit < 0 || bit >= 32 || !(u->view.toggleCaps & (1u << bit))) continue;
    any = any || (StateOf(L, u, &s) && (s.scriptBits >> bit & 1u));
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
// The queue as the panel shows it: orders for the same thing that follow one another are ONE
// entry with their counts added up (two engineers, a tank, two engineers: three entries of 2, 1
// and 2), as players know it from the original. (INFERRED where the original adds them up: in
// what it hands the script, as here, or by putting a second order into the first.)
struct QueueStack {
  std::string blueprint;
  int count = 0;
  std::vector<CommandView> orders;  // first to last
};
std::vector<QueueStack> StacksOf(const std::vector<CommandView>& queue) {
  std::vector<QueueStack> stacks;
  for (const CommandView& c : queue) {
    if (stacks.empty() || stacks.back().blueprint != c.blueprint || c.type != stacks.back().orders.back().type) {
      stacks.emplace_back();
      stacks.back().blueprint = c.blueprint;
    }
    stacks.back().count += c.count;
    stacks.back().orders.push_back(c);
  }
  return stacks;
}
void PushBuildQueue(lua_State* L, const std::vector<CommandView>& queue) {
  lua_newtable(L);
  int n = 0;
  for (const QueueStack& stack : StacksOf(queue)) {
    lua_newtable(L);
    lua_pushstring(L, "id");
    lua_pushstring(L, stack.blueprint.c_str());
    lua_rawset(L, -3);
    lua_pushstring(L, "count");
    lua_pushnumber(L, stack.count);
    lua_rawset(L, -3);
    lua_rawseti(L, -2, ++n);
  }
}
std::string QueueKey(const std::vector<CommandView>& queue) {
  std::string key;
  for (const QueueStack& stack : StacksOf(queue)) key += stack.blueprint + "*" + std::to_string(stack.count) + ",";
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
  // (The index is the shown entry's: more goes onto its last order; less is taken from its last
  // order backwards, as many orders as it takes.)
  const std::vector<QueueStack> stacks = StacksOf(queue);
  if (index < 1 || index > static_cast<int>(stacks.size())) return 0;
  const QueueStack& stack = stacks[static_cast<size_t>(index - 1)];
  if (more) {
    Writer(L).IncreaseCommandCount(stack.orders.back().id, count);
    return 0;
  }
  for (size_t i = stack.orders.size(); i-- > 0 && count > 0;) {
    const int take = std::min(count, std::max(stack.orders[i].count, 1));
    Writer(L).DecreaseCommandCount(stack.orders[i].id, take);
    count -= take;
  }
  return 0;
}
int l_IncreaseBuildCountInQueue(lua_State* L) { return ChangeBuildCount(L, true); }
int l_DecreaseBuildCountInQueue(lua_State* L) { return ChangeBuildCount(L, false); }

// RenderOverlayEconomy(on) (FA exe 0x847d30): the session's switch for the economy overlay (drawn
// by the world view, DrawEconomyOverlay).
int l_RenderOverlayEconomy(lua_State* L) {
  UserState::From(L)->economyOverlay = lua_toboolean(L, 1) != 0;
  return 0;
}

// DeleteCommand(id) (FA exe 0x843fa0): one less of the order with that id (FAF's command graph
// would pass it when a waypoint is dragged away; its call is commented out there), the same message
// as DecreaseBuildCountInQueue (15, a count of 1): at 0 the simulation takes the order from every
// unit that holds it. The original sends it only for an id its command manager knows (the orders it
// has seen; no check of whose they are, R: the simulation checks); here: an order in the queue of
// one of the player's units, as the last beat left it.
int l_DeleteCommand(lua_State* L) {
  const uint32_t id = static_cast<uint32_t>(static_cast<int64_t>(luaL_checknumber(L, 1)));
  Sim* sim = TheSim(L);
  if (!sim) return 0;
  for (const auto& [unitId, u] : Store(L).units) {
    if (u->gone || u->view.dead || u->view.army != sim->focusArmy + 1) continue;
    UnitStateView st;
    if (!StateOf(L, u.get(), &st)) continue;
    for (const std::vector<CommandView>* queue : {&st.queue, &st.factoryQueue})
      for (const CommandView& c : *queue)
        if (c.id == id) {
          Writer(L).DecreaseCommandCount(id, 1);
          return 0;
        }
  }
  return 0;
}

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

// A unit kind's rank in a selection by box (the blueprints' General.SelectionPriority: 1 the
// fighting units, 3 engineers and commanders, 5 buildings; 1 where a blueprint says nothing).
int SelectionPriorityOf(lua_State* L, const UserUnit* u) {
  int priority = 1;
  PushBlueprint(L, u->view.blueprint);
  if (lua_istable(L, -1)) {
    lua_pushstring(L, "General");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
      lua_pushstring(L, "SelectionPriority");
      lua_gettable(L, -2);
      if (lua_isnumber(L, -1)) priority = static_cast<int>(lua_tonumber(L, -1));
      lua_pop(L, 1);
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  return priority;
}

// (The original, CWldSession::DrawEconomyOverlay 0x858d80: the focus army's units in the camera's
// view, E = (energyProduced - energyRequested) * 10 and M the same for mass, a unit left out when
// both are 0. Ours are a second already: produced and consumptionPerSecond, as GetEconData gives
// them. INFERRED, not built: its distance cutoff per unit.)
void EconomyMarks(lua_State* L, std::vector<EconomyMark>* out) {
  UserState* user = UserState::From(L);
  const int focus = user->economyCopied ? user->focusArmyCopy : -1;
  if (focus < 0) return;
  for (const auto& [id, u] : Store(L).units) {
    if (u->gone || u->view.dead || u->view.army != focus + 1) continue;
    UnitStateView st;
    if (!StateOf(L, u.get(), &st)) continue;
    const float energy = st.produced[0] - st.consumptionPerSecond[0], mass = st.produced[1] - st.consumptionPerSecond[1];
    if (energy == 0 && mass == 0) continue;
    out->push_back({{u->view.position[0], u->view.position[1], u->view.position[2]}, energy, mass});
  }
}

bool UnitPlace(lua_State* L, uint32_t id, float position[3], float orientation[4]) {
  UnitStore& store = Store(L);
  auto it = store.units.find(id);
  if (it == store.units.end() || it->second->gone || it->second->view.dead) return false;
  for (int i = 0; i < 3; ++i) position[i] = it->second->view.position[i];
  for (int i = 0; i < 4; ++i) orientation[i] = it->second->view.orientation[i];
  return true;
}

void SelectUnitsById(lua_State* L, const std::vector<uint32_t>& ids, bool add) {
  UnitStore& store = Store(L);
  std::vector<UserUnit*> now;
  if (add) now = store.selection;
  for (uint32_t id : ids) {
    auto it = store.units.find(id);
    if (it == store.units.end() || !CanSelectUnit(L, it->second.get())) continue;
    if (std::find(now.begin(), now.end(), it->second.get()) == now.end()) now.push_back(it->second.get());
  }
  SetSelection(L, now);
}

namespace {
UserUnit* LiveUnit(lua_State* L, uint32_t id) {
  auto it = Store(L).units.find(id);
  return it == Store(L).units.end() || it->second->gone ? nullptr : it->second.get();
}
}  // namespace

// The mouse's selection, as the original's (FA exe; notes: the ui chat's scratchpad
// helper_selectable\findings.md).
// A click (CWldSession::ReleaseDrag 0x865920) on the unit under the mouse: one that may not be
// selected (or nothing) clears the selection, unless Shift or Ctrl is held (then nothing changes).
// Ctrl: the player's units of its kind across the whole map, finished and alive (no other test);
// with Shift as well, that kind leaves the selection if the unit is in it, else joins it. Shift:
// the unit joins the selection or leaves it. Else it alone is selected.
void ClickSelect(lua_State* L, uint32_t unit, bool shift, bool ctrl, bool alt) {
  if (!UserState::From(L)->HasSim()) return;
  const int focus = FocusArmyNow(L);
  UnitStore& store = Store(L);
  UserUnit* u = unit == 0xffffffffu ? nullptr : LiveUnit(L, unit);
  // (ui_DebugAltClick and Alt on another army's unit: the player looks through that army, ReleaseDrag's first step)
  if (alt && u && Looks().debugAltClick && u->view.army != focus + 1) {
    if (Sim* sim = TheSim(L)) sim->RequestFocusArmy(u->view.army - 1);
    return;
  }
  if (!u || !CanSelectUnit(L, u)) {
    if (!shift && !ctrl) SetSelection(L, {});
    return;
  }
  std::vector<UserUnit*> now = store.selection;
  const bool selected = std::find(now.begin(), now.end(), u) != now.end();
  if (ctrl) {
    std::vector<UserUnit*> kind;
    for (auto& [id, o] : store.units)
      if (!o->gone && !o->view.dead && !o->view.beingBuilt && o->view.army == focus + 1 && o->view.blueprint == u->view.blueprint)
        kind.push_back(o.get());
    if (!shift) {
      SetSelection(L, kind);
    } else if (selected) {
      now.erase(std::remove_if(now.begin(), now.end(), [&](UserUnit* s) { return s->view.blueprint == u->view.blueprint; }), now.end());
      SetSelection(L, now);
    } else {
      for (UserUnit* k : kind)
        if (std::find(now.begin(), now.end(), k) == now.end()) now.push_back(k);
      SetSelection(L, now);
    }
    return;
  }
  if (shift) {
    if (selected) now.erase(std::find(now.begin(), now.end(), u));
    else now.push_back(u);
    SetSelection(L, now);
    return;
  }
  SetSelection(L, {u});
}

// A box (CWldSession::DragRelease 0x863870, its pruning 0x863e20): of the units in it, those that
// may be selected. With Shift they join the selection, or, when all of them are in it already,
// they leave it. Without: a building being upgraded is passed over, and of the rest only those of
// the best (lowest) General.SelectionPriority are the selection (FAF's patch 0x12914d1: a unit being
// built with LOWSELECTPRIO counts as 6); none: the selection is cleared.
void BoxSelect(lua_State* L, const std::vector<uint32_t>& ids, bool shift) {
  if (!UserState::From(L)->HasSim()) return;
  Sim* const sim = nullptr;  // (BeingUpgraded reads the views only)
  std::vector<UserUnit*> in;
  for (uint32_t id : ids) {
    UserUnit* u = LiveUnit(L, id);
    if (u && CanSelectUnit(L, u) && std::find(in.begin(), in.end(), u) == in.end()) in.push_back(u);
  }
  std::vector<UserUnit*> now = Store(L).selection;
  if (shift) {
    bool all = true;
    for (UserUnit* u : in) all = all && std::find(now.begin(), now.end(), u) != now.end();
    if (all) {
      now.erase(std::remove_if(now.begin(), now.end(), [&](UserUnit* s) { return std::find(in.begin(), in.end(), s) != in.end(); }), now.end());
    } else {
      for (UserUnit* u : in)
        if (std::find(now.begin(), now.end(), u) == now.end()) now.push_back(u);
    }
    SetSelection(L, now);
    return;
  }
  int best = 1 << 30;
  std::vector<std::pair<UserUnit*, int>> ranked;
  for (UserUnit* u : in) {
    if (BeingUpgraded(L, sim, u)) continue;
    int priority = std::max(1, SelectionPriorityOf(L, u));
    if (u->view.beingBuilt && InCategory(L, u, "LOWSELECTPRIO")) priority = 6;
    ranked.push_back({u, priority});
    best = std::min(best, priority);
  }
  std::vector<UserUnit*> chosen;
  for (const auto& [u, priority] : ranked)
    if (priority == best) chosen.push_back(u);
  SetSelection(L, chosen);
}

// A double click (HandleDoubleClickSelection 0x865e20) on one of the player's units: the units in
// view of its kind that may be selected, not a building being upgraded, join the selection. False
// when the unit is not the player's. (A wall too: the test for walls is taken out, FAF's patch.)
bool DoubleClickSelect(lua_State* L, uint32_t unit, const std::vector<uint32_t>& inView) {
  Sim* const sim = nullptr;  // (BeingUpgraded reads the views only)
  UserUnit* u = unit == 0xffffffffu ? nullptr : LiveUnit(L, unit);
  if (!UserState::From(L)->HasSim() || !u || u->view.army != FocusArmyNow(L) + 1) return false;
  std::vector<UserUnit*> now = Store(L).selection;
  for (uint32_t id : inView) {
    UserUnit* o = LiveUnit(L, id);
    if (!o || o->view.blueprint != u->view.blueprint || !CanSelectUnit(L, o) || BeingUpgraded(L, sim, o)) continue;
    if (std::find(now.begin(), now.end(), o) == now.end()) now.push_back(o);
  }
  SetSelection(L, now);
  return true;
}

// Which unit the mouse is over, of the one the picture finds under it (UpdateSelection 0x86f520):
// a unit on a transport or a factory (one being built on its pad) gives its place to that parent;
// a building being upgraded to the building that upgrades it; one inside a carrier is not there.
// 0xffffffff: none.
uint32_t HoverOf(lua_State* L, uint32_t picked) {
  UserUnit* u = picked == 0xffffffffu ? nullptr : LiveUnit(L, picked);
  if (!u) return picked;
  if (u->view.parent != 0xffffffffu) {
    if (UserUnit* parent = LiveUnit(L, u->view.parent)) {
      if (InCategory(L, parent, "CARRIER")) return 0xffffffffu;
      if (InCategory(L, parent, "TRANSPORTATION") || InCategory(L, parent, "FACTORY")) return parent->view.id;
    }
  }
  if (BeingUpgraded(L, nullptr, u)) return u->view.creator;  // (from the views: no wait for a running beat)
  return picked;
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

const BuildTemplate* ActiveBuildTemplate(lua_State* L) {
  UserState* user = UserState::From(L);
  if (!user->units) return nullptr;
  const BuildTemplate& t = static_cast<UnitStore&>(*user->units).buildTemplate;
  return t.buildings.empty() ? nullptr : &t;
}

namespace {
// GenerateBuildTemplateFromSelection(): the selected buildings become the active template (FA
// exe CWldSession::GenerateBuildTemplates 0x896aa0). As read there: the selected units that
// count are gathered (two tests on each, taken here as "not mobile" and "not being built":
// INFERRED which the two virtual calls at +0x30 and +0x28 are; with none that counts nothing
// changes); each gives its place, a number and its blueprint; they are sorted by the number,
// the size of the whole is the box round every building's skirt, and every place is then taken
// relative to the first building's. The number (unit + 0x19c) is not identified; FAF's notes
// call it the build order: the entity id stands in for it (INFERRED).
int l_GenerateBuildTemplateFromSelection(lua_State* L) {
  UnitStore& store = Store(L);
  std::vector<const UserUnit*> from;
  for (const UserUnit* u : store.selection)
    if (!u->gone && !u->view.dead && !u->view.beingBuilt && InCategory(L, u, "STRUCTURE")) from.push_back(u);
  if (from.empty()) return 0;
  std::sort(from.begin(), from.end(), [](const UserUnit* a, const UserUnit* b) { return a->view.id < b->view.id; });
  BuildTemplate t;
  float lo[2] = {10000, 10000}, hi[2] = {-10000, -10000};
  for (const UserUnit* u : from) {
    BuildTemplate::Building b;
    b.blueprint = u->view.blueprint;
    b.order = static_cast<int>(u->view.id & 0xfffffu);
    b.x = u->view.position[0] - from[0]->view.position[0];
    b.z = u->view.position[2] - from[0]->view.position[2];
    t.buildings.push_back(b);
    float w = 1, h = 1;
    BuildSpacingOf(L, b.blueprint, &w, &h);
    lo[0] = std::min(lo[0], u->view.position[0] - w * 0.5f), hi[0] = std::max(hi[0], u->view.position[0] + w * 0.5f);
    lo[1] = std::min(lo[1], u->view.position[2] - h * 0.5f), hi[1] = std::max(hi[1], u->view.position[2] + h * 0.5f);
  }
  t.width = hi[0] - lo[0], t.height = hi[1] - lo[1];
  store.buildTemplate = std::move(t);
  return 0;
}
// GetActiveBuildTemplate() -> {width, height, {blueprint, order, x, z}, ...}, an empty table
// when there is none (0x8472f0).
int l_GetActiveBuildTemplate(lua_State* L) {
  const BuildTemplate& t = Store(L).buildTemplate;
  lua_newtable(L);
  if (t.buildings.empty()) return 1;
  lua_pushnumber(L, t.width);
  lua_rawseti(L, -2, 1);
  lua_pushnumber(L, t.height);
  lua_rawseti(L, -2, 2);
  int at = 3;
  for (const BuildTemplate::Building& b : t.buildings) {
    lua_newtable(L);
    lua_pushstring(L, b.blueprint.c_str());
    lua_rawseti(L, -2, 1);
    lua_pushnumber(L, b.order);
    lua_rawseti(L, -2, 2);
    lua_pushnumber(L, b.x);
    lua_rawseti(L, -2, 3);
    lua_pushnumber(L, b.z);
    lua_rawseti(L, -2, 4);
    lua_rawseti(L, -2, at++);
  }
  return 1;
}
// SetActiveBuildTemplate(template): the same table back (0x8475e0); the script starts the
// build mode itself, with the first building's name.
int l_SetActiveBuildTemplate(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  BuildTemplate t;
  lua_rawgeti(L, 1, 1);
  t.width = static_cast<float>(lua_tonumber(L, -1));
  lua_rawgeti(L, 1, 2);
  t.height = static_cast<float>(lua_tonumber(L, -1));
  lua_pop(L, 2);
  const int entries = luaL_getn(L, 1);
  for (int at = 3; at <= entries; ++at) {
    lua_rawgeti(L, 1, at);
    if (!lua_istable(L, -1) || luaL_getn(L, -1) < 4) {
      Logf(LogLevel::Warning, "Template info error!");
      lua_pop(L, 1);
      continue;
    }
    BuildTemplate::Building b;
    lua_rawgeti(L, -1, 1);
    if (lua_isstring(L, -1)) b.blueprint = lua_tostring(L, -1);
    lua_rawgeti(L, -2, 2);
    b.order = static_cast<int>(lua_tonumber(L, -1));
    lua_rawgeti(L, -3, 3);
    b.x = static_cast<float>(lua_tonumber(L, -1));
    lua_rawgeti(L, -4, 4);
    b.z = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 5);
    if (!b.blueprint.empty()) t.buildings.push_back(b);
  }
  if (!t.buildings.empty()) Store(L).buildTemplate = std::move(t);  // (an empty one keeps the old)
  return 0;
}
int l_ClearBuildTemplates(lua_State* L) {
  Store(L).buildTemplate = BuildTemplate();
  return 0;
}
// AddToSessionExtraSelectList(unit), RemoveFromSessionExtraSelectList(unit),
// ClearSessionExtraSelectList(): units that show as selected without being in the selection
// (the construction panel's buttons for the units attached to the selected one; the session's
// list at 0x896780 / 0x896830 / 0x896870). Here they get the selection's mark in the world.
int l_AddToSessionExtraSelectList(lua_State* L) {
  UnitStore& store = Store(L);
  if (const UserUnit* u = ToUnit(L, 1))
    if (std::find(store.extraSelect.begin(), store.extraSelect.end(), u->view.id) == store.extraSelect.end()) store.extraSelect.push_back(u->view.id);
  return 0;
}
int l_RemoveFromSessionExtraSelectList(lua_State* L) {
  UnitStore& store = Store(L);
  if (const UserUnit* u = ToUnit(L, 1))
    store.extraSelect.erase(std::remove(store.extraSelect.begin(), store.extraSelect.end(), u->view.id), store.extraSelect.end());
  return 0;
}
int l_ClearSessionExtraSelectList(lua_State* L) {
  Store(L).extraSelect.clear();
  return 0;
}
}  // namespace

void StructuresForWorld(lua_State* L, const float near[3], float within, std::vector<WorldGhost>* out) {
  UserState* user = UserState::From(L);
  if (!user->units) return;
  UnitStore& store = static_cast<UnitStore&>(*user->units);
  for (auto& entry : store.units) {
    const UserUnit* u = entry.second.get();
    if (u->gone || u->view.dead || out->size() >= 600) continue;
    const float dx = u->view.position[0] - near[0], dz = u->view.position[2] - near[2];
    if (dx * dx + dz * dz > within * within || !InCategory(L, u, "STRUCTURE")) continue;
    WorldGhost g;
    g.blueprint = u->view.blueprint;
    for (int i = 0; i < 3; ++i) g.position[i] = u->view.position[i];
    out->push_back(g);
  }
}

// A building's site against the buildings ordered and not begun. The simulation refuses a site
// that meets a standing building's ground (skirt against skirt); what is only ordered it does
// not know as ground taken, so two buildings could be ordered into one another (Chris,
// 2026-10-13: two queued power generators half over each other). The original's preview is
// red there. INFERRED where the original tests it (its interface, by the build orders it
// shows); the rectangles are those of the guide (BuildSpacingOf), edges may touch.
bool SiteMeetsOrdered(lua_State* L, const std::string& blueprint, const float position[3], bool adding) {
  Sim* sim = TheSim(L);
  UserState* user = UserState::From(L);
  if (!sim || !user->units) return false;
  UnitStore& store = static_cast<UnitStore&>(*user->units);
  float w = 1, h = 1;
  BuildSpacingOf(L, blueprint, &w, &h);
  auto meets = [&](const WorldGhost& g) {
    float gw = 1, gh = 1;
    BuildSpacingOf(L, g.blueprint, &gw, &gh);
    return std::fabs(g.position[0] - position[0]) < (w + gw) * 0.5f - 0.01f && std::fabs(g.position[2] - position[2]) < (h + gh) * 0.5f - 0.01f;
  };
  for (size_t i = 0; i < store.ordered.size(); ++i) {
    if (!adding && i < store.orderedBy.size()) {  // (a selected unit's own queue goes with the new order)
      bool own = false;
      for (const UserUnit* u : store.selection) own = own || u->view.id == store.orderedBy[i];
      if (own) continue;
    }
    if (meets(store.ordered[i])) return true;
  }
  const uint32_t now = static_cast<uint32_t>(sim->tick());
  store.sent.erase(std::remove_if(store.sent.begin(), store.sent.end(), [&](const UnitStore::SentSite& e) { return now - e.tick > 5; }),
                   store.sent.end());
  for (const UnitStore::SentSite& e : store.sent)
    if (meets(e.site)) return true;
  return false;
}

void SelectionForWorld(lua_State* L, std::vector<uint32_t>* ids, std::vector<WorldLine>* lines, std::vector<WorldGhost>* ghosts,
                       bool queueShown, std::vector<WorldGhost>* queued, std::vector<WorldWaypoint>* waypoints) {
  Sim* sim = TheSim(L);
  UserState* user = UserState::From(L);
  if (!sim || !user->units) return;
  UnitStore& store = static_cast<UnitStore&>(*user->units);
  int shown = 0;
  for (uint32_t id : store.extraSelect) {  // (shown as selected, not selected)
    auto it = store.units.find(id);
    if (it != store.units.end() && !it->second->gone && !it->second->view.dead) ids->push_back(id);
  }
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
      // The line's colour and the mark at the order's place are the game's own: its table of
      // how each kind of order is drawn (the colour of a selected unit's orders, which these are).
      uint32_t color = 0xffc0c0c0u;
      if (const char* kind = CommandName(static_cast<CommandType>(c.type))) {
        const std::string command = std::string("UNITCOMMAND_") + kind;
        const std::string lineColor = CommandGraphParam(L, command, "orderline_selected_color");
        if (!lineColor.empty()) color = ParseColor(lineColor);
        const std::string texture = CommandGraphParam(L, command, "waypoint_texture");
        if (waypoints && !texture.empty() && waypoints->size() < 600) {
          WorldWaypoint mark;
          for (int i = 0; i < 3; ++i) mark.position[i] = c.position[i];
          mark.texture = texture;
          const std::string markColor = CommandGraphParam(L, command, "waypoint_selected_color");
          if (!markColor.empty()) mark.color = ParseColor(markColor);
          const std::string scale = CommandGraphParam(L, command, "waypoint_selected_scale");
          if (!scale.empty()) mark.scale = static_cast<float>(std::atof(scale.c_str()));
          waypoints->push_back(mark);
        }
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
    store.orderedBy.clear();
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
        store.orderedBy.push_back(u->view.id);
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
  CategorySets(*sim, store.categorySets.size(), &store.categorySets);  // (only the new ones: the table only grows)
  for (auto& [id, u] : store.units) u->gone = true;
  // (A unit whose id went to another unit is kept aside until the selection has let go of it: the
  // selection holds the units themselves.)
  std::vector<std::unique_ptr<UserUnit>> retired;
  std::vector<UserUnit*> newCreator;  // (units whose creator changed: UserUnit::AddToSelectionSet below)
  for (const UnitView& v : views) {
    auto& slot = store.units[v.id];
    if (slot && slot->view.blueprint != v.blueprint) {  // the id went to another unit
      slot->gone = true;
      retired.push_back(std::move(slot));
    }
    uint32_t oldCreator = 0xffffffffu;
    if (!slot) {
      slot = std::make_unique<UserUnit>();
      slot->view = v;
      MakeTable(L, slot.get());
    } else {
      oldCreator = slot->view.creator;
    }
    slot->view = v;
    slot->gone = false;
    if (v.creator != oldCreator && v.creator != 0xffffffffu) newCreator.push_back(slot.get());
  }
  // A unit made by a factory joins the factory's control groups (FA exe UserUnit::UpdateUnitData 0x8c0750: when the
  // unit's creator changes and the creator is a FACTORY, UserUnit::AddToSelectionSet 0x8bff30): each of the
  // factory's selection sets is added to the unit, then the game's script hears of it,
  // /lua/ui/game/selection.lua AddUnitToSelectionSet(name, unit), which keeps it or takes it out again by the
  // player's options (FAF: "add to factory control group"; buildings always, so an upgrade keeps its group).
  for (UserUnit* u : newCreator) {
    auto f = store.units.find(u->view.creator);
    if (f == store.units.end() || f->second->gone || !InCategory(L, f->second.get(), "FACTORY")) continue;
    const std::vector<std::string> sets = f->second->selectionSets;
    for (const std::string& name : sets) {
      if (std::find(u->selectionSets.begin(), u->selectionSets.end(), name) == u->selectionSets.end()) u->selectionSets.push_back(name);
      const int top = lua_gettop(L);
      lua_pushcfunction(L, ScriptTraceback);
      lua_getglobal(L, "import");
      lua_pushstring(L, "/lua/ui/game/selection.lua");
      if (lua_pcall(L, 1, 1, top + 1) == 0 && lua_istable(L, -1)) {
        lua_pushstring(L, "AddUnitToSelectionSet");
        lua_gettable(L, -2);
        if (lua_isfunction(L, -1)) {
          lua_pushstring(L, name.c_str());
          PushObject(L, u);
          if (lua_pcall(L, 2, 0, top + 1) != 0) LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "(error object is not a string)");
        }
      }
      lua_settop(L, top);
    }
  }
  // Units that are gone leave the selection (the script hears of it) and then the store.
  std::vector<UserUnit*> kept;
  for (UserUnit* u : store.selection)
    if (!u->gone && !u->view.dead) kept.push_back(u);
  SetSelection(L, kept);
  for (auto& old : retired) old->UnbindLua();
  retired.clear();
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
  if (!UserState::From(L)->HasSim()) {
    // (The front end has SelectUnits too, its script calls it while it sets up; with no game there
    // is nothing to select, and it does nothing.)
    SetGlobal(L, "SelectUnits", [](lua_State*) -> int { return 0; });
    return;
  }
  SetGlobal(L, "GetSelectedUnits", l_GetSelectedUnits);
  SetGlobal(L, "SelectUnits", l_SelectUnits);
  SetGlobal(L, "AddSelectUnits", l_AddSelectUnits);
  SetGlobal(L, "UISelectionByCategory", l_UISelectionByCategory);
  SetGlobal(L, "UIZoomTo", l_UIZoomTo);
  SetGlobal(L, "UISelectAndZoomTo", l_UISelectAndZoomTo);
  SetGlobal(L, "ValidateUnitsList", l_ValidateUnitsList);
  SetGlobal(L, "GetUnitById", l_GetUnitById);
  SetGlobal(L, "GetRolloverInfo", l_GetRolloverInfo);
  lua_register(L, "moho64ui_ReclaimableAt", l_ReclaimableAtForTests);
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
  SetGlobal(L, "GenerateBuildTemplateFromSelection", l_GenerateBuildTemplateFromSelection);
  SetGlobal(L, "GetActiveBuildTemplate", l_GetActiveBuildTemplate);
  SetGlobal(L, "SetActiveBuildTemplate", l_SetActiveBuildTemplate);
  SetGlobal(L, "ClearBuildTemplates", l_ClearBuildTemplates);
  SetGlobal(L, "AddToSessionExtraSelectList", l_AddToSessionExtraSelectList);
  SetGlobal(L, "RemoveFromSessionExtraSelectList", l_RemoveFromSessionExtraSelectList);
  SetGlobal(L, "ClearSessionExtraSelectList", l_ClearSessionExtraSelectList);
  SetGlobal(L, "SetAutoMode", l_SetAutoMode);
  SetGlobal(L, "SetAutoSurfaceMode", l_SetAutoSurfaceMode);
  SetGlobal(L, "GetIsSubmerged", l_GetIsSubmerged);
  SetGlobal(L, "IssueDockCommand", l_IssueDockCommand);
  SetGlobal(L, "SetFireState", l_SetFireState);
  SetGlobal(L, "ToggleFireState", l_ToggleFireState);
  SetGlobal(L, "ToggleScriptBit", l_ToggleScriptBit);
  SetGlobal(L, "GetScriptBit", l_GetScriptBit);
  SetGlobal(L, "GetIsPaused", l_ListSwitch<&UnitStateView::paused>);
  SetGlobal(L, "GetIsAutoMode", l_ListSwitch<&UnitStateView::autoMode>);
  SetGlobal(L, "GetIsAutoSurfaceMode", l_ListSwitch<&UnitStateView::autoSurfaceMode>);
  SetGlobal(L, "GetAttachedUnitsList", l_GetAttachedUnitsList);
  SetGlobal(L, "GetBlueprint", l_GetBlueprintGlobal);
  SetGlobal(L, "GetValidAttackingUnits", l_GetValidAttackingUnits);
  SetGlobal(L, "GetAssistingUnitsList", l_GetAssistingUnitsList);

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
                           "HasUnloadCommandQueuedUp"})
    SetMethod(L, cls, name, l_UnitFalse);
  SetMethod(L, cls, "ProcessInfo", l_UnitProcessInfo);
  SetMethod(L, cls, "CanAttackTarget", l_CanAttackTarget);

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
  SetGlobal(L, "GetIsAutoMode", l_AllSwitch<&UnitStateView::autoMode>);
  SetGlobal(L, "GetIsAutoSurfaceMode", l_AllSwitch<&UnitStateView::autoSurfaceMode>);
  SetGlobal(L, "SetCurrentFactoryForQueueDisplay", l_SetCurrentFactoryForQueueDisplay);
  SetGlobal(L, "ClearCurrentFactoryForQueueDisplay", l_ClearCurrentFactoryForQueueDisplay);
  SetGlobal(L, "IncreaseBuildCountInQueue", l_IncreaseBuildCountInQueue);
  SetGlobal(L, "DecreaseBuildCountInQueue", l_DecreaseBuildCountInQueue);
  SetGlobal(L, "DeleteCommand", l_DeleteCommand);
  SetGlobal(L, "RenderOverlayEconomy", l_RenderOverlayEconomy);
  // moho64ui_EconomyMarks() -> the economy overlay's numbers now, each {energy, mass}; the host's
  // own function for tests.
  lua_register(L, "moho64ui_EconomyMarks", [](lua_State* L) -> int {
    std::vector<EconomyMark> marks;
    EconomyMarks(L, &marks);
    lua_newtable(L);
    for (size_t i = 0; i < marks.size(); ++i) {
      lua_newtable(L);
      lua_pushstring(L, "energy");
      lua_pushnumber(L, marks[i].energy);
      lua_rawset(L, -3);
      lua_pushstring(L, "mass");
      lua_pushnumber(L, marks[i].mass);
      lua_rawset(L, -3);
      lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
  });
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
