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
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
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
#include "user/keymap.h"
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
  // The statistics GetStat last read of the simulation, by name, and at which sync (StatOf): a number, a text,
  // or none (the simulation keeps no such statistic for the unit).
  struct StatCopy {
    uint64_t sync = ~0ull;
    int type = LUA_TNIL;  // LUA_TNUMBER, LUA_TSTRING or LUA_TNIL
    lua_Number number = 0;
    std::string text;
  };
  mutable std::map<std::string, StatCopy> statCopies;
};

// The original's command graph (FA exe UICommandGraph.cpp; the object GetCommandGraph 0x895eb0 makes
// with 0x824810 while Shift is held, 0xddc bytes): the orders of every unit the interface knows as
// nodes joined by legs, from which the order lines and the waypoints' marks are drawn. Worked out by
// OrderLinesForWorld (further down), kept here between frames.
struct OrderGraph {
  // How one kind of order is drawn: the record at graph+4 + type*0x54 (filled by 0x824d50 from the
  // game's commandgraphparams.lua), by the engine's command number.
  struct Record {
    std::string lineTexture;          // +0x00 orderline_texture ("": the leg is never drawn)
    float uvAspect = 1;               // +0x08 orderline_uv_aspect_ratio
    float animRate = 0.1f;            // +0x0c orderline_anim_rate (0.1 before the table's default)
    uint32_t lineColor[3] = {0, 0, 0};    // +0x10 normal, +0x14 selected, +0x18 highlight
    float glow[3] = {0, 0, 0};            // +0x1c.. (the bloom pass's; not drawn here)
    uint32_t markColor[3] = {0, 0, 0};    // +0x28.. waypoint_color, _selected_, _highlight_
    float markScale[3] = {1, 1, 1};       // +0x34.. waypoint_scale, ...
    float capOffset = 0;                  // +0x40 arrowhead_cap_offset (arrowheads: never drawn)
    std::string markTexture;              // +0x44 waypoint_texture ("": no mark)
    std::string arrowTexture;             // +0x4c arrowhead_texture
  };
  // A node: one per command (its id; shared by the units that carry it), and one start node per first
  // command at the middle of the units whose queue begins with it (command none). 0x826140.
  struct Node {
    uint32_t command = 0xffffffffu;
    int type = 0;
    float sum[3] = {0, 0, 0};  // +0x0c.. the places added, and how many (+0x18): the place is sum / count
    float count = 0;
    float tangent[3] = {0, 0, 0};  // +0x28 (0x8275b0)
    float size = 0;                // +0x40
    std::vector<int> in, out;      // its legs (+0x48, +0x60)
    std::string blueprint;         // a build order's blueprint (the command's +0x20)
    // When its order will be done, a beat number (+0x44, WorkOutEtas), and whether it is worked out
    // (the original's test is the value 0).
    int64_t eta = 0;
    bool etaDone = false;
  };
  // A leg between two nodes (0x826960): how many units go along it (+0x00), and the queue walked last
  // along it (its byte +0x18: whether that queue's unit is the one under the mouse or shares an order
  // with it, so set by each unit that walks it, the last one's standing).
  struct Leg {
    int from = 0, to = 0;
    int units = 0;
    int lastQueue = -1;
  };
  // A queue walked (a unit's own, or a factory's second one), and the units that carry any of its
  // commands (for the leg's byte +0x18 above: the unit under the mouse among them).
  struct Queue {
    uint32_t unit = 0xffffffffu;
    std::vector<uint32_t> partners;
  };
  std::vector<Record> records;  // 0x28 kinds
  bool made = false;            // it exists (Shift held); made again on the next press
  uint64_t builtSync = ~0ull;   // the sync it was worked out after
  std::vector<Node> nodes;
  std::vector<Leg> legs;
  std::vector<Queue> queues;
  std::map<uint32_t, std::vector<uint32_t>> carriers;  // a command's units (the original's per command)
  std::map<uint32_t, int> nodeOf;  // a command's node (the hash map graph+0xd30)
  std::map<int, float> widths;  // CalculateWaypointLineWidth by units, as asked (graph+0xda8)
  // The waypoint under the mouse (the session's +0x4c8, 0x829800): its command's id, 0xffffffff none.
  uint32_t hovered = 0xffffffffu;
  // A waypoint being dragged (UICommandDragger, 0x823fe0): its command; once moved (the node's byte +0x1c, set by
  // each move, 0x829b40) the node stands at `at`, the ground under the mouse, instead of its order's place; `fit`: a
  // building's site there passes the placement test (the node's byte +0x1e, 1 at first, 0x824600).
  struct Drag {
    uint32_t command = 0xffffffffu;
    bool moved = false, fit = true;
    float at[3] = {0, 0, 0};
    int type = 0;           // the order's kind and a building's blueprint, as when the drag began
    std::string blueprint;
    bool hasFrom = false;   // the order's own place then (its building's ghost stands there until the drag)
    float from[3] = {0, 0, 0};
    bool hasHome = false;   // where its node stood then (a cancelled drag puts it back there at once)
    float home[3] = {0, 0, 0};
  } drag;
  // Orders a drag has given a new target that the simulation's queues do not show yet: the original sets its own
  // copy of the order at once (0x8b4a40), so the node stands there from the release on. By command: the place,
  // and the sync it was sent after.
  struct Retarget {
    float at[3] = {0, 0, 0};
    uint32_t entity = 0xffffffffu;  // the unit it was sent onto (0xffffffff: a place)
    uint64_t sync = 0;
  };
  std::map<uint32_t, Retarget> retargets;
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
  // The selection's size as SetSelection last left it (the session's +0x4ac, written at 0x896387): a unit gone from
  // the simulation leaves the selection without a word (RefreshUserUnits), and CheckForNecessaryUIRefresh, after the
  // beat's OnSync, finds the selection smaller than this.
  size_t selectionSetSize = 0;
  std::unique_ptr<CommandWriter> writer;  // the player's orders since the last beat
  uint32_t queueFactory = 0xffffffffu;    // the factory whose queue the construction panel shows
  std::string queueKey;                   // that queue as it was last handed to the script
  // The order lines while Shift is held (the original's command graph; see OrderLinesForWorld).
  OrderGraph graph;
  std::map<std::string, bool> canFly;  // Air.CanFly by blueprint (CanFlyOf)
  std::map<std::string, float> speeds;  // the speed the order lines' times go by, by blueprint (SpeedOf)
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
  // Orders taken out of units' queues by a Shift+Ctrl right click on their waypoint that the simulation's queues may
  // still show (RemoveHoveredWaypointOrder: the original's copy of a unit's queue drops the order at once): by unit and
  // order, with the sync they were sent after.
  struct Removal {
    uint32_t unit = 0xffffffffu, command = 0xffffffffu;
    uint64_t sync = 0;
  };
  std::vector<Removal> removals;
  // The order the interface sent last to each unit (IssueOrder), with the sync it was sent after: the original's copy
  // of a unit's queue takes an order at once (ISSUE_Command 0x8b0577), the simulation's queues show it a beat later;
  // what a double click's ISSUE_RemoveLastCommand takes out (RemoveSelectionsLastOrders).
  struct Sent {
    uint32_t command = 0xffffffffu;
    uint64_t sync = 0;
  };
  std::map<uint32_t, Sent> lastSent;
  // Orders given a new type by the patrol conversion (ConvertMovesToPatrol: the original's copy of the order takes it
  // at once, 0x8b4ac0) that the simulation's queues may not show yet: by order, the new type and the sync.
  struct Retype {
    int type = 0;
    uint64_t sync = 0;
  };
  std::map<uint32_t, Retype> retypes;
  // The changes the interface's own copy of each unit's queues has taken from the orders it issued, which the
  // simulation's queues do not show yet (READ 2026-10-21): ISSUE_Command (0x8b0180, its loop 0x8b0517..0x8b0588) and
  // ISSUE_FactoryCommand (0x8b0730, 0x8b0a60..0x8b0ac8) give each unit's copy (the unit's slot 0x18, its own queue
  // +0x3c8; slot 0x20, the factory's rally queue +0x3cc) a change stamped with the beat the order's message goes with
  // (the sim driver's +0x24): with `clear` first a reset (0x8b6e60: the earlier changes are dropped, then {beat, 1});
  // then {beat, 0, order, insert-before id} (0x8b6de0), unless the copy holds more than 500 orders and `clear` is
  // not given (0x8b0540). The copy as read (0x8b6f60, GetCommandQueue 0x8c5480 and the rest) is the simulation's
  // queue as last synced with every change still kept applied in order: 1 empties it, 0 appends the order or puts it
  // before the order of that id (none there: not at all; 0x8b7081..0x8b72c6), 2 takes an order out (the removals,
  // DropRemovedOrders). A change goes when the beat it was stamped with has come back (UserUnit::Tick 0x8c0a30 ->
  // 0x8b7350: every change of a beat up to the synced one). So the order shows at once, before the game's script
  // hears of it (OnCommandIssued, 0x8b0599): FAF's enhancement queue (enhancementqueue.lua) counts on it.
  // `firstSync`: the user state's sync count from which the simulation's queues include the order (the next beat's,
  // or the one after it while a beat runs on its thread: what is written then goes with the beat after it).
  struct QueueChange {
    uint32_t unit = 0xffffffffu;
    bool factory = false;  // the factory's rally queue (IssueFactoryCommand), else the unit's own
    bool reset = false;    // 1: empty the queue; else 0: the order goes in
    CommandView order;
    uint32_t insertBefore = 0xffffffffu;
    uint64_t firstSync = 0;
  };
  std::vector<QueueChange> queueChanges;
  // In a test run (TestFileWritesAllowed): the messages of the batches handed to the simulation, {type, payload},
  // for moho64ui_SentMessages.
  std::vector<std::pair<int, std::string>> sentForTests;
  // The last attack sent coordinated with another (AttackClick), for the tests (moho64ui_OrderGraph's coordinated).
  uint32_t coordinatedOrder = 0xffffffffu, coordinatedWith = 0xffffffffu;
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

// `always`: the script hears of it even when nothing changes (CheckForNecessaryUIRefresh). The original's
// CWldSession::SetSelection (0x896140) tells its listeners every time, changed or not (0x896352 -> 0x8986f0, the
// SelectionListener among them: 0x869060 calls OnSelectionChanged), and only the set of ids it hands the simulation
// (0x8964cf) and UI_EndCommandMode (0x8964d1) wait for something added or removed (0x89638d); ours keeps its other
// callers' old way (nothing when nothing changes).
void SetSelection(lua_State* L, std::vector<UserUnit*> now, bool always = false) {
  UnitStore& store = Store(L);
  if (now == store.selection && !always) {
    store.selectionSetSize = store.selection.size();
    return;
  }
  std::vector<UserUnit*> before = store.selection;
  store.selection = std::move(now);
  store.selectionSetSize = store.selection.size();  // (0x89637b .. 0x896387)
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
  std::vector<uint64_t> total(static_cast<size_t>(CategoryWordCount(L)), 0);
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
// GetStat(name [, default]): a statistic the unit's script keeps; {Value = default} without a
// simulation to read them from (with one: l_GetStatNow).
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

namespace {
// In a test run: the batch's messages ({u8 type, u16 length with the three header bytes, payload}, 0x6e417a), kept for
// moho64ui_SentMessages (the last 400).
void NoteSentForTests(UnitStore& store, const std::string& batch) {
  size_t at = 0;
  while (at + 3 <= batch.size()) {
    const int type = static_cast<unsigned char>(batch[at]);
    const size_t len = static_cast<unsigned char>(batch[at + 1]) | (static_cast<size_t>(static_cast<unsigned char>(batch[at + 2])) << 8);
    if (len < 3 || at + len > batch.size()) break;
    store.sentForTests.emplace_back(type, batch.substr(at + 3, len - 3));
    at += len;
  }
  if (store.sentForTests.size() > 400) store.sentForTests.erase(store.sentForTests.begin(), store.sentForTests.end() - 400);
}
}  // namespace

std::string TakeOrders(lua_State* L) {
  UserState* user = UserState::From(L);
  if (!user->units) return std::string();
  UnitStore& store = static_cast<UnitStore&>(*user->units);
  std::string batch = store.writer ? store.writer->Take() : std::string();
  if (!batch.empty() && TestFileWritesAllowed()) NoteSentForTests(store, batch);
  return batch;
}

namespace {
// What an order issued now does to the interface's copy of its units' queues (UnitStore::QueueChange; ISSUE_Command
// 0x8b0517..0x8b0588, ISSUE_FactoryCommand 0x8b0a60..0x8b0ac8): each unit's copy takes a reset when `clear` (the
// earlier changes dropped) and the order, unless the copy holds more than 500 orders and `clear` is not given. The
// order as the copy shows it: its id, type, blueprint and count, and its target's place (a place; a unit: where the
// unit stands now, INFERRED as the simulation's view gives an order on a unit its place).
void NoteIssued(lua_State* L, const std::vector<uint32_t>& ids, const StreamIssueData& d, uint32_t id, bool clear, bool factory) {
  UserState* user = UserState::From(L);
  UnitStore& store = Store(L);
  CommandView order;
  order.id = id;
  order.type = d.type;
  order.blueprint = d.blueprint;
  order.count = d.count;
  if (d.target.type == 2) {
    order.hasPosition = true;
    order.position[0] = d.target.pos.x, order.position[1] = d.target.pos.y, order.position[2] = d.target.pos.z;
  } else if (d.target.type == 1) {
    order.entity = d.target.entity;
    auto it = store.units.find(d.target.entity);
    if (it != store.units.end() && !it->second->gone) {
      order.hasPosition = true;
      for (int i = 0; i < 3; ++i) order.position[i] = it->second->view.position[i];
    }
  }
  const uint64_t firstSync = user->syncCount + (user->config().beatRunning && user->config().beatRunning() ? 2 : 1);
  for (uint32_t unit : ids) {
    auto it = store.units.find(unit);
    if (it == store.units.end() || it->second->gone) continue;
    UnitStateView s;
    if (!StateOf(L, it->second.get(), &s)) continue;
    if (factory && !s.hasFactoryQueue) continue;  // (no rally queue: the unit's slot 0x20 gives none)
    const size_t held = (factory ? s.factoryQueue : s.queue).size();
    if (held > 500 && !clear) continue;
    if (clear) {
      auto& changes = store.queueChanges;
      changes.erase(std::remove_if(changes.begin(), changes.end(),
                                   [&](const UnitStore::QueueChange& c) { return c.unit == unit && c.factory == factory; }),
                    changes.end());
      UnitStore::QueueChange reset;
      reset.unit = unit, reset.factory = factory, reset.reset = true, reset.firstSync = firstSync;
      changes.push_back(reset);
    }
    UnitStore::QueueChange add;
    add.unit = unit, add.factory = factory, add.order = order, add.firstSync = firstSync;
    add.insertBefore = static_cast<uint32_t>(d.insertBefore);
    store.queueChanges.push_back(add);
  }
  // (The original then marks its order lines to be made again, CWldSession::DirtyCommandGraph 0x8b05a5 -> 0x895f70:
  // the graph's byte +0; where that byte is read, and so whether a waypoint hovered between the two clicks of a
  // double click already sees the first click's order, is not traced. The lines here are made again with the next
  // sync, as before, so the hovered attack of a coordinated double click stays the one hovered at the first click.)
}

// An order for units' own queues and one for factories' rally queues (the command stream's IssueCommand and
// IssueFactoryCommand), noted in the interface's copy of their queues as the original's ISSUE_Command does.
uint32_t IssueToUnits(lua_State* L, const std::vector<uint32_t>& ids, const StreamIssueData& d, bool clear) {
  const uint32_t id = Writer(L).IssueCommand(ids, d, clear);
  NoteIssued(L, ids, d, id, clear, false);
  return id;
}
uint32_t IssueRallyToUnits(lua_State* L, const std::vector<uint32_t>& ids, const StreamIssueData& d, bool clear) {
  const uint32_t id = Writer(L).IssueFactoryCommand(ids, d, clear);
  NoteIssued(L, ids, d, id, clear, true);
  return id;
}
}  // namespace

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

const UnitView* UserUnitView(lua_State* L, uint32_t id) {
  UserState* user = UserState::From(L);
  if (!user->units) return nullptr;
  UnitStore& store = static_cast<UnitStore&>(*user->units);
  auto it = store.units.find(id);
  if (it == store.units.end() || it->second->gone || it->second->view.dead) return nullptr;
  return &it->second->view;
}

bool UserUnitScriptBits(lua_State* L, uint32_t id, uint32_t* bits) {
  UserState* user = UserState::From(L);
  if (!user->units) return false;
  UnitStore& store = static_cast<UnitStore&>(*user->units);
  auto it = store.units.find(id);
  UnitStateView s;
  if (it == store.units.end() || !StateOf(L, it->second.get(), &s)) return false;
  *bits = s.scriptBits;
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
// engine issued for the player, as a table (func_UserScriptCommandObj 0x83e150, READ 2026-10-21): Units, the
// units' list; Blueprint, the order's blueprint id or the empty string when it has none (0x83e21c..0x83e240, the
// issue data's +0x50; never nil); Target = {Type = the lexical of the target's ESTITargetType (data +0x10: "None",
// "Entity", "Position", registered at 0x55af30), and for an entity EntityId ("%d") and Position, the entity's
// place (0x83e2cf..0x83e3ab), else Position, the target's place (data +0x18, 0x83e452)}; LuaParams, a copy of the
// order's Lua value when it has one (0x83e3d1; `luaParams`: its stack index, 0 for none), which the script reads
// for a Script order (an attack-move, an enhancement); CommandType, the order's name; Clear. An order without a
// target (`hasTarget` false: Stop, Script, Upgrade, a factory's build order; IssueCommand and its kin) has Type
// "None" and the place the issue data starts with, 0, 0, 0 (SSTICommandIssueData 0x552550: +0x18..+0x20 zeroed):
// FAF's AddCommandFeedbackByType shows the default blips there for such an order, in the original too.
void ReportCommandIssued(lua_State* L, const char* type, const std::vector<UserUnit*>& units, float x, float y, float z,
                         bool clear, const std::string& blueprint, uint32_t entity = 0xffffffffu, int luaParams = 0,
                         bool hasTarget = true) {
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
      lua_pushstring(L, blueprint.c_str());
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
      lua_pushstring(L, !hasTarget ? "None" : entity != 0xffffffffu ? "Entity" : "Position");
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
                const std::string& blueprint, bool ctrl = false, bool form = false, float headingGiven = NAN,
                uint32_t coordWith = 0xffffffffu) {
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
  // A building order goes out with the selection as it stands, an empty one too (READ 2026-10-20): the build
  // dragger's release (FA exe 0x823220) gives each site that passes its test to ISSUE_Command (0x8b05e0 ->
  // 0x8b0180) with the session's selection (+0x4a0), and neither looks at the set's size; the client writes the
  // message with no units (CClientManagerImpl::IssueCommand 0x6e6690: no test either; our simulation gives it to
  // none), and the script's OnCommandIssued hears it with Units = {} (0x8b0599). FAF's spawn window (Alt-F2,
  // createunit.lua) builds on that: the unit picked there is placed in the build mode with nothing selected, and
  // commandmode.lua's OnBuildMobileIssued sends CheatSpawnUnit for a building order without units. The other orders
  // here still want a unit (INFERRED: where the original's other ways to them test the selection is not read).
  if (ids.empty() && type != CommandType::BuildMobile) return false;
  StreamIssueData d;
  d.type = static_cast<int>(type);
  d.target.type = entity != 0xffffffffu ? 1 : 2;  // a unit, or a position
  d.target.entity = entity != 0xffffffffu ? entity : d.target.entity;
  d.target.pos.x = x, d.target.pos.y = y, d.target.pos.z = z;
  d.blueprint = blueprint;
  // (the issue data's second word, SSTICommandIssueData +4: the order this one is coordinated with, 0x81fef1)
  if (coordWith != 0xffffffffu) d.coordWith = static_cast<int32_t>(coordWith);
  // The formation, as the original's interface sends it (the engine track's count over about
  // 1,250 orders of real replays, docs/engine-answers.md "ui 14"): a Move to a place, an Attack on
  // a unit and a Patrol carry formation 0 at scale 1 and a turn about the up axis whose heading
  // is the direction from the MEAN of the units' places to the target, atan2(dx, dz); for one
  // unit alone as well. Everything else carries none, and so does a factory's rally order (the
  // factories' own data below). With it the simulation arranges a group; without, every unit
  // heads for the same spot.
  StreamIssueData rally = d;
  // (A Patrol of one unit carries no formation, as the original's 0x8209c7 and the replays show.)
  // (An AggressiveMove, FAF's attack-move by Alt and a right click, too: the 468 of the three FAF replays in
  // replays/mp all carry formation 0, the heading and scale 1, one unit alone as well, and none is a
  // FormAggressiveMove.)
  const bool forms = type == CommandType::Move || type == CommandType::AggressiveMove ||
                     (type == CommandType::Patrol && units.size() > 1) || (type == CommandType::Attack && entity != 0xffffffffu);
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
  if (!units.empty()) mean[0] /= static_cast<float>(units.size()), mean[1] /= static_cast<float>(units.size());
  // Ctrl with more than one unit: an attack on a unit goes as FormAttack (SCommandModeData::HandleEvent 0x81fe4c;
  // FAF's exe leaves a Move as it is: its 0x82074d skips the original's FormMove).
  if (ctrl && units.size() > 1 && type == CommandType::Attack && entity != 0xffffffffu) {
    type = CommandType::FormAttack;
    d.type = static_cast<int>(type);
    name = "FormAttack";
  }
  // The right button held half a second (the formation's "drag held", 0x820767 / 0x81fe53 / 0x8209e5): a group's
  // Move, Attack on a unit or Patrol goes as its formation order.
  if (form && units.size() > 1) {
    if (type == CommandType::Move) type = CommandType::FormMove, name = "FormMove";
    else if (type == CommandType::Attack && entity != 0xffffffffu) type = CommandType::FormAttack, name = "FormAttack";
    else if (type == CommandType::Patrol) type = CommandType::FormPatrol, name = "FormPatrol";
    d.type = static_cast<int>(type);
  }
  if (forms) {
    float to[2] = {x, z};
    if (entity != 0xffffffffu) {  // (an Attack on a unit: the target point is the unit's place)
      auto it = Store(L).units.find(entity);
      if (it != Store(L).units.end()) to[0] = it->second->view.position[0], to[1] = it->second->view.position[2];
    }
    const float heading = std::isnan(headingGiven) ? std::atan2(to[0] - mean[0], to[1] - mean[1]) : headingGiven;
    d.formIndex = 0;
    d.scale = 1;
    d.qw = std::cos(heading * 0.5f), d.qx = 0, d.qy = std::sin(heading * 0.5f), d.qz = 0;
  }
  // An order given with a factory selected is the factory's rally order: what the units it builds
  // are told to do (the command stream's IssueFactoryCommand, FA exe 0x6f14d0), kept in a queue
  // of its own. INFERRED: which units count (here: buildings that are factories; a carrier moves
  // itself) and that every order of the world's clicks but a building order goes that way; where
  // the original's interface decides is not read.
  // A Guard of a structure (SCommandModeData::HandleEvent's Guard case 0x82012f..0x820367, READ 2026-10-21): when the
  // unit under the cursor has a blueprint and is in STRUCTURE (0x82018e), the selection is split by the category
  // REBUILDER (SplitSelectionByRebuilderCategory 0x81e9e0): the others get the Guard of the unit, the REBUILDERs a
  // Guard of its place (target type 2, no entity, the unit's position +0x84), both carrying the structure's blueprint
  // (the issue data's +0x50) and both sent, the second also with no units in it (no stock unit is a REBUILDER: the
  // lab's replays of the original hold 544 such pairs, the second without units). FAF's OnGuardIssued goes by that
  // blueprint (ValidateAssist for a factory, AssistToCap for a structure). Any other Guard of a unit carries no
  // blueprint (0x82036c).
  const UserUnit* structure = nullptr;
  if (type == CommandType::Guard && entity != 0xffffffffu) {
    auto it = Store(L).units.find(entity);
    if (it != Store(L).units.end() && !it->second->view.blueprint.empty() && InCategory(L, it->second.get(), "STRUCTURE"))
      structure = it->second.get();
  }
  std::vector<uint32_t> factories, others, rebuilders;
  std::vector<UserUnit*> guarding, rebuilding;  // (each order's units, as the script hears them)
  for (UserUnit* u : units) {
    if (structure && InCategory(L, u, "REBUILDER")) {
      rebuilders.push_back(u->view.id), rebuilding.push_back(u);
      continue;
    }
    guarding.push_back(u);
    (type != CommandType::BuildMobile && InCategory(L, u, "FACTORY") && InCategory(L, u, "STRUCTURE") ? factories : others)
        .push_back(u->view.id);
  }
  if (structure) d.blueprint = rally.blueprint = structure->view.blueprint;
  // (remembered until the queues show it: SiteMeetsOrdered; an order without units is in no queue)
  if (type == CommandType::BuildMobile && !blueprint.empty() && !units.empty()) {
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
  if ((type == CommandType::Patrol || type == CommandType::FormPatrol) && !queuedPatrol) {
    StreamIssueData start = d, startRally = rally;
    float ground = y;
    if (const auto& height = UserState::From(L)->config().groundHeight) ground = height(mean[0], mean[1]);
    start.target.type = startRally.target.type = 2;
    start.target.pos.x = startRally.target.pos.x = mean[0];
    start.target.pos.y = startRally.target.pos.y = ground;
    start.target.pos.z = startRally.target.pos.z = mean[1];
    if (!others.empty()) IssueToUnits(L, others, start, clear);
    if (!factories.empty()) IssueRallyToUnits(L, factories, startRally, clear);
    clear = false;
  }
  // (none at all: a building order with nothing selected, above; a structure's Guard goes out also when only
  // REBUILDERs are selected, as the original's two ISSUE_Command calls test nothing)
  if (!others.empty() || units.empty() || (structure && factories.empty())) {
    const uint32_t id = IssueToUnits(L, others, d, clear);
    UnitStore& store = Store(L);
    for (uint32_t unit : others) store.lastSent[unit] = {id, UserState::From(L)->syncCount};
    if (coordWith != 0xffffffffu) store.coordinatedOrder = id, store.coordinatedWith = coordWith;
  }
  if (!factories.empty()) IssueRallyToUnits(L, factories, rally, clear);
  ReportCommandIssued(L, name, structure ? guarding : units, x, y, z, clear, structure ? d.blueprint : blueprint, entity);
  if (structure) {  // (the second: the REBUILDERs' Guard of the structure's place, 0x82029e..0x820327)
    StreamIssueData site = d;
    site.target = StreamIssueData().target;
    site.target.type = 2;
    site.target.pos.x = x, site.target.pos.y = y, site.target.pos.z = z;
    const uint32_t id = IssueToUnits(L, rebuilders, site, clear);
    for (uint32_t unit : rebuilders) Store(L).lastSent[unit] = {id, UserState::From(L)->syncCount};
    ReportCommandIssued(L, name, rebuilding, x, y, z, clear, d.blueprint);
  }
  return true;
}
}  // namespace

bool IssueToSelection(lua_State* L, const std::string& order, float x, float y, float z, bool clear, const std::string& blueprint) {
  return IssueOrder(L, order, x, y, z, 0xffffffffu, clear, blueprint);
}

bool IssueFormationOrder(lua_State* L, const std::string& order, float x, float y, float z, uint32_t entity, bool clear,
                         bool ctrl, bool form, float heading) {
  return IssueOrder(L, order, x, y, z, entity, clear, std::string(), ctrl, form, heading);
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
  IssueToUnits(L, ids, d, clear);
  ReportCommandIssued(L, "Script", units, x, y, z, clear, std::string(), 0xffffffffu, result);
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
  if (!others.empty()) IssueToUnits(L, others, d, clear);
  if (!factories.empty()) IssueRallyToUnits(L, factories, d, clear);
  ReportCommandIssued(L, name, units, d.target.pos.x, d.target.pos.y, d.target.pos.z, clear, std::string(), entity);
  return true;
}

bool IssueToSelectionAt(lua_State* L, const std::string& order, uint32_t entity, bool clear, bool ctrl, bool form, float heading,
                        uint32_t coordWith) {
  UnitStore& store = Store(L);
  auto it = store.units.find(entity);
  if (it == store.units.end() || it->second->gone) return false;
  const float* p = it->second->view.position;
  return IssueOrder(L, order, p[0], p[1], p[2], entity, clear, std::string(), ctrl, form, heading, coordWith);
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
// With no unit under the cursor, the waypoint under it (the session's, cursor info +0x18; 0x81f513) of an Attack or
// FormAttack makes the click an Attack when the selection can attack (the click then attacks the ground there,
// AttackClick).
// Left out so far: Capture (the original asks a flag of the unit, +0x69 of its attributes, that
// the simulation's view does not give; without it engineers reclaim, as players know it), the
// transport and ferry cases (0x81d280, 0x81d660, 0x81da20),
// the option "switch_right_click_behavior". Things that
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

namespace {
bool FindCommandView(lua_State* L, uint32_t id, CommandView* out, std::vector<uint32_t>* carriers);  // further down
}  // namespace

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
  if (!e) {
    // (0x81f513: the hovered order looked up in the session's table, 0x8b6160; an Attack or FormAttack, and the
    // selection's bits hold RULEUCC_Attack: an Attack)
    const uint32_t hovered = HoveredWaypoint(L);
    CommandView c;
    if ((caps & kAttack) && (hovered & 0xff000000u) != 0xff000000u && FindCommandView(L, hovered, &c, nullptr) &&
        (c.type == static_cast<int>(CommandType::Attack) || c.type == static_cast<int>(CommandType::FormAttack)))
      return "RULEUCC_Attack";
    return move;
  }
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

// The game's tables of how orders are drawn, read each time the command graph is made (0x824810,
// when Shift is pressed), as the original reads them:
// - /lua/ui/game/commandgraphparams.lua CommandGraphParams (0x824d50): a record starts as
//   OrderGraph::Record's zeros, takes the entry `default` (0x825570), and each kind of order
//   0..0x27 then a copy of that and its entry "UNITCOMMAND_" + the order's name over it, where an
//   entry's inherit_from (another entry's name) goes under it first, and so on. A key the entry
//   has replaces the record's; colours as the game's colour texts ("3300ffff").
// - /lua/ui/game/commandwaypoint.lua CommandWaypointParams (0x825150): each console variable it
//   names, over the console's (GraphVars). ui_CommandClickScale goes into ui_WaypointLineScale's
//   value (0x825524 stores to 0xf57cdc, ui_WaypointLineScale's, as 0x8254b8 does: GPG's slip,
//   kept; both are 1 in the stock file).
void ApplyGraphEntry(lua_State* L, int table, int entry, OrderGraph::Record* r, int depth) {
  if (!lua_istable(L, entry) || depth > 16 || !lua_checkstack(L, 4)) return;  // (its room on the stack: prefs.cpp PushCopy)
  lua_getfield(L, entry, "inherit_from");
  if (lua_isstring(L, -1)) {
    lua_gettable(L, table);
    ApplyGraphEntry(L, table, lua_gettop(L), r, depth + 1);
  }
  lua_pop(L, 1);
  auto text = [&](const char* key, std::string* out) {
    lua_getfield(L, entry, key);
    if (lua_isstring(L, -1)) *out = lua_tostring(L, -1);
    lua_pop(L, 1);
  };
  auto number = [&](const char* key, float* out) {
    lua_getfield(L, entry, key);
    if (lua_isnumber(L, -1)) *out = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);
  };
  auto color = [&](const char* key, uint32_t* out) {
    lua_getfield(L, entry, key);
    if (lua_isstring(L, -1)) *out = ParseColor(lua_tostring(L, -1));
    lua_pop(L, 1);
  };
  text("orderline_texture", &r->lineTexture);
  number("orderline_uv_aspect_ratio", &r->uvAspect);
  number("orderline_anim_rate", &r->animRate);
  color("orderline_color", &r->lineColor[0]);
  color("orderline_selected_color", &r->lineColor[1]);
  color("orderline_highlight_color", &r->lineColor[2]);
  number("orderline_glow", &r->glow[0]);
  number("orderline_selected_glow", &r->glow[1]);
  number("orderline_highlight_glow", &r->glow[2]);
  text("waypoint_texture", &r->markTexture);
  color("waypoint_color", &r->markColor[0]);
  color("waypoint_selected_color", &r->markColor[1]);
  color("waypoint_highlight_color", &r->markColor[2]);
  number("waypoint_scale", &r->markScale[0]);
  number("waypoint_selected_scale", &r->markScale[1]);
  number("waypoint_highlight_scale", &r->markScale[2]);
  number("arrowhead_cap_offset", &r->capOffset);
  text("arrowhead_texture", &r->arrowTexture);
}
// The module's table (import(path)[name]) pushed, or nil.
void PushModuleTable(lua_State* L, const char* path, const char* name) {
  lua_getglobal(L, "import");
  lua_pushstring(L, path);
  if (lua_pcall(L, 1, 1, 0) != 0 || !lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_pushnil(L);
    return;
  }
  lua_getfield(L, -1, name);
  lua_remove(L, -2);
}
void LoadGraphParams(lua_State* L, OrderGraph* g) {
  const int top = lua_gettop(L);
  OrderGraph::Record base;
  g->records.assign(0x28, base);
  PushModuleTable(L, "/lua/ui/game/commandgraphparams.lua", "CommandGraphParams");
  if (lua_istable(L, -1)) {
    const int table = lua_gettop(L);
    lua_getfield(L, table, "default");
    ApplyGraphEntry(L, table, lua_gettop(L), &base, 0);
    lua_pop(L, 1);
    for (int type = 0; type < 0x28; ++type) {
      OrderGraph::Record& r = g->records[static_cast<size_t>(type)];
      r = base;
      const char* name = CommandName(static_cast<CommandType>(type));
      lua_getfield(L, table, (std::string("UNITCOMMAND_") + (name ? name : "None")).c_str());
      ApplyGraphEntry(L, table, lua_gettop(L), &r, 0);
      lua_pop(L, 1);
    }
  }
  lua_settop(L, top);
  PushModuleTable(L, "/lua/ui/game/commandwaypoint.lua", "CommandWaypointParams");
  if (lua_istable(L, -1)) {
    CommandGraphVars& v = GraphVars();
    auto read = [&](const char* key, auto* out) {
      lua_getfield(L, -1, key);
      if (lua_isnumber(L, -1)) *out = static_cast<std::remove_pointer_t<decltype(out)>>(lua_tonumber(L, -1));  // (ints: _ftol 0xa89cc0)
      lua_pop(L, 1);
    };
    read("ui_CurveSegments", &v.curveSegments);
    read("ui_CurveSmoothness", &v.curveSmoothness);
    read("ui_PathSmoothness", &v.pathSmoothness);
    read("ui_CommandGraphMaxNodeUnits", &v.maxNodeUnits);
    read("ui_MinWaypointSize", &v.minWaypointSize);
    read("ui_MaxWaypointSize", &v.maxWaypointSize);
    read("ui_WaypointLineScale", &v.waypointLineScale);
    read("ui_CommandClickScale", &v.waypointLineScale);
  }
  lua_settop(L, top);
}
// CalculateWaypointLineWidth(units) of the game's commandwaypoint.lua (min(sqrt(units), 10) in the
// stock file), asked once for each count while the graph lives (0x826ad6, its cache graph+0xda8).
float WaypointLineWidth(lua_State* L, OrderGraph* g, int units) {
  auto known = g->widths.find(units);
  if (known != g->widths.end()) return known->second;
  float width = std::min(std::sqrt(static_cast<float>(units)), 10.0f);  // (if the script does not answer)
  const int top = lua_gettop(L);
  lua_getglobal(L, "import");
  lua_pushstring(L, "/lua/ui/game/commandwaypoint.lua");
  if (lua_pcall(L, 1, 1, 0) == 0 && lua_istable(L, -1)) {
    lua_getfield(L, -1, "CalculateWaypointLineWidth");
    if (lua_isfunction(L, -1)) {
      lua_pushnumber(L, units);
      if (lua_pcall(L, 1, 1, 0) == 0 && lua_isnumber(L, -1)) width = static_cast<float>(lua_tonumber(L, -1));
      else if (lua_isstring(L, -1)) Logf(LogLevel::Warning, "CalculateWaypointLineWidth: %s", lua_tostring(L, -1));
    }
  }
  lua_settop(L, top);
  g->widths[units] = width;
  return width;
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
  IssueToUnits(L, ids, d, clear);
  ReportCommandIssued(L, plain, units, 0, 0, 0, clear, blueprint, 0xffffffffu, params && lua_istable(L, params) ? params : 0, false);
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

// ExecLuaInSim(function, arg) (FA exe 0x8ba5c0, "Execute some lua code in the sim"): exactly two arguments, the
// first a string; with a session (0x10c4f50) the client sends ExecuteLuaInSim (vtable +0x84): the simulation
// calls its global `function` with `arg` (the stream's message 21, carried out only with cheats on, 0x749a70).
// The path debugger's scripts use it; so do the tests (the simulation's print reaching the console).
int l_ExecLuaInSim(lua_State* L) {
  if (lua_gettop(L) != 2)
    return luaL_error(L, "%s\n  expected %d args, but got %d", "Execute some lua code in the sim", 2, lua_gettop(L));
  const std::string name = luaL_checkstring(L, 1);
  std::string value;
  if (!lua_isnil(L, 2)) SerializeLua(L, 2, &value);
  if (TheSim(L)) Writer(L).ExecuteLuaInSim(name, value);
  return 0;
}

// IUnit::CalcSpawnElevation 0x541540 as the interface's TeleportSelectedUnits calls it (READ): by the unit's layer
// now (not the new place's), Land or Seabed: the terrain's height there (STIMap::GetElevation 0x44fb90); Water: the
// water's (none on the map: -10000); Sub: that plus the unit's elevation; Air or Orbit: the surface there (GetSurface
// 0x541a30: the terrain, or the water when the map has water above it) plus the unit's elevation; else 0. The
// elevation is its attributes' +0x30 (IUnit slot 0x40, GetAttributes 0x8bef50: UserUnit+0x338); INFERRED: the
// blueprint's Physics.Elevation, which they start from.
float SpawnElevation(lua_State* L, const UserUnit* u, float x, float z) {
  UserState* user = UserState::From(L);
  const auto& height = user->config().groundHeight;
  const float terrain = height ? height(x, z) : 0.0f;
  const float water = user->hasWaterCopy ? user->waterElevationCopy : -10000.0f;
  const int layer = u->view.layer;
  if (layer & 3) return terrain;
  if (layer & 8) return water;
  float elevation = 0;
  if (layer & (4 | 0x30)) {
    PushBlueprint(L, u->view.blueprint);
    if (lua_istable(L, -1)) {
      lua_pushstring(L, "Physics");
      lua_rawget(L, -2);
      if (lua_istable(L, -1)) {
        lua_pushstring(L, "Elevation");
        lua_rawget(L, -2);
        if (lua_isnumber(L, -1)) elevation = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
      }
      lua_pop(L, 1);
    }
    lua_pop(L, 1);
  }
  if (layer & 4) return water + elevation;
  if (layer & 0x30) return (user->hasWaterCopy && water > terrain ? water : terrain) + elevation;
  return 0;
}
}  // namespace

// The console's DoSimCommand (FA exe 0x88e440, READ; the sim's console commands come to it through their aliases,
// keymap.cpp): the command line goes to the simulation as the stream's DebugCommand (20) with every unit of the
// selection (the session's +0x4a0, whoever owns them: no test of army or cheats here, the simulation's command
// decides), the place of the world under the mouse (+0x4b4) and the focus army (+0x488, as one byte). The original
// asks its sim driver for that (0x10c4f50 vtable +0x8c, SimDriver::ExecuteDebugCommand 0x73d1b0, which hands it to
// the client, 0x6e7070, and wakes the simulation's thread). The units go as the client's WriteEntIdSet (0x6e7600)
// writes the set they were gathered in (a BVIntSet, BVIntSet::Add 0x88e51a): their count, then the ids rising, each
// once. Here no session or no live simulation: nothing (the console's caller has printed the "no session" line).
void SendSimCommand(lua_State* L, const std::string& command) {
  if (!TheSim(L)) return;
  std::vector<uint32_t> ids;
  for (const UserUnit* u : Store(L).selection)
    if (!u->gone) ids.push_back(u->view.id);
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  float mouse[3] = {0, 0, 0};
  MouseWorldPlace(L, mouse);
  Writer(L).DebugCommand(command, mouse[0], mouse[1], mouse[2], FocusArmyNow(L), ids);
}

// The console's TeleportSelectedUnits (FA exe 0x833f90, READ): no session, the engine's "no session" line; else each
// selected unit of the focus army (the unit's army +0x120 the session's army, 0x8340e2: an observer moves none) goes
// to the place under the mouse (+0x4b4..+0x4bc) as the stream's WarpEntity (10; the sim driver's vtable +0x5c), turned
// as it is (+0x74..+0x80), its height CalcSpawnElevation's for its layer (SpawnElevation). No cheat test here: the
// simulation carries a warp out only with cheats on (0x748cd0).
void TeleportSelectedUnits(lua_State* L) {
  if (!UserState::From(L)->gameInterface) return ConsoleNoSession(L);
  if (!TheSim(L)) return;
  float mouse[3] = {0, 0, 0};
  MouseWorldPlace(L, mouse);
  const int army = FocusArmyNow(L);
  for (const UserUnit* u : Store(L).selection) {
    if (u->gone || army < 0 || u->view.army != army + 1) continue;
    const float* q = u->view.orientation;  // (x, y, z, w; the message's transform has w first, 0x6e58c0)
    Writer(L).WarpEntity(u->view.id, q[3], q[0], q[1], q[2], mouse[0], SpawnElevation(L, u, mouse[0], mouse[2]), mouse[2]);
  }
}

// The console's KillSelectedUnits and DestroySelectedUnits (FA exe 0x833c70 / 0x833d60, READ): no session, the
// engine's "no session" line; else the selection gets the order KillSelf (30) or DestroySelf (31), its queue cleared
// first, through ISSUE_Command (0x8b05e0) as any order: here IssuePlain, the player's own units (the simulation wants
// cheats for a DestroySelf, the command stream's IssueCommand 0x749290).
void SelectionOrdersItself(lua_State* L, bool destroy) {
  if (!UserState::From(L)->gameInterface) return ConsoleNoSession(L);
  if (!TheSim(L)) return;
  IssuePlain(L, Store(L).selection, destroy ? "DestroySelf" : "KillSelf", 0, true, std::string(), 1);
}

namespace {

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
    IssueToUnits(L, ids, d, clear);
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

// The orders the interface issued that the simulation's queues do not show yet are in the unit's queues at once, as
// the original's copy of them (UnitStore::queueChanges, NoteIssued): each change still kept applied in the order
// made (0x8b7081..0x8b72c6), a reset empties the queue, an order goes at its end, or before the order of the id its
// message names (none such there: it does not go in; an id whose top byte is 0xff names none). The changes whose
// orders the simulation's queues include by now are dropped first (0x8b7350).
void ApplyQueueChanges(lua_State* L, const UserUnit* u, UnitStateView* out) {
  std::vector<UnitStore::QueueChange>& changes = Store(L).queueChanges;
  if (changes.empty()) return;
  const uint64_t sync = UserState::From(L)->syncCount;
  changes.erase(std::remove_if(changes.begin(), changes.end(), [&](const UnitStore::QueueChange& c) { return c.firstSync <= sync; }),
                changes.end());
  for (const UnitStore::QueueChange& c : changes) {
    if (c.unit != u->view.id) continue;
    std::vector<CommandView>& q = c.factory ? out->factoryQueue : out->queue;
    if (c.reset) {
      q.clear();
      continue;
    }
    if ((c.insertBefore >> 24) == 0xffu) {
      q.push_back(c.order);
      continue;
    }
    auto at = std::find_if(q.begin(), q.end(), [&](const CommandView& o) { return o.id == c.insertBefore; });
    if (at != q.end()) q.insert(at, c.order);
  }
}

// An order a Shift+Ctrl right click has taken out of the unit's queue is not in it from then on, as the original's
// copy of the queue drops it at once (UnitStore::removals; 0x8b6ee0, further down "a waypoint under the mouse").
void DropRemovedOrders(lua_State* L, const UserUnit* u, UnitStateView* out) {
  std::vector<UnitStore::Removal>& removals = Store(L).removals;
  if (removals.empty()) return;
  const uint64_t sync = UserState::From(L)->syncCount;
  for (auto r = removals.begin(); r != removals.end();) {
    if (r->unit != u->view.id) {
      ++r;
      continue;
    }
    // (the unit's own queue only: 0x8b1220 changes the copy the unit's slot 0x18 gives, +0x3c8, not its factory
    // queue's, +0x3cc)
    std::vector<CommandView>& q = out->queue;
    auto at = std::find_if(q.begin(), q.end(), [&](const CommandView& c) { return c.id == r->command; });
    // Still there ten syncs on: INFERRED, the simulation kept it (it turned the message down), so the interface shows
    // it again (the original's copy keeps its changes by beat, 0x8b76d0; when it lets them go is not read). Not there:
    // gone from the simulation's queue, or not in it yet (an order sent and taken out before the simulation's queues
    // showed it, RemoveSelectionsLastOrders): the record is kept as long, so that the order stays out when it comes.
    if (sync > r->sync + 10) {
      r = removals.erase(r);
      continue;
    }
    if (at != q.end()) q.erase(at);
    ++r;
  }
}

// An order the patrol conversion has given a new type shows it at once, as the original's copy of the order takes it
// (0x8b4ac0; UnitStore::retypes), until the simulation's queues show it (or ten syncs on: INFERRED, as above).
void RetypeSentOrders(lua_State* L, UnitStateView* out) {
  std::map<uint32_t, UnitStore::Retype>& retypes = Store(L).retypes;
  if (retypes.empty()) return;
  const uint64_t sync = UserState::From(L)->syncCount;
  for (auto r = retypes.begin(); r != retypes.end();) r = sync > r->second.sync + 10 ? retypes.erase(r) : std::next(r);
  for (std::vector<CommandView>* q : {&out->queue, &out->factoryQueue})
    for (CommandView& c : *q) {
      auto r = retypes.find(c.id);
      if (r == retypes.end()) continue;
      if (c.type == r->second.type) {
        retypes.erase(r);  // (the simulation's queues show it: every unit's copy of this sync does)
        continue;
      }
      c.type = r->second.type;
    }
}

// A unit's state changes only in a beat: it is kept until the next beat's sync comes (the panels
// ask the same units every frame; each answer runs in the simulation's Lua), and while a beat runs
// on its own thread the state from before it is the answer (no wait for the beat). Orders issued
// since are in its queues at once (ApplyQueueChanges), orders taken out by their waypoints are left
// out of them (DropRemovedOrders), and orders the patrol conversion has changed show their new type
// (RetypeSentOrders).
bool StateOf(lua_State* L, const UserUnit* u, UnitStateView* out) {
  if (!u || u->gone) return false;
  UserState* user = UserState::From(L);
  const bool beat = user->config().beatRunning && user->config().beatRunning();
  if (u->stateSync != ~0ull && (beat || u->stateSync == user->syncCount)) {
    *out = u->stateCopy;
    ApplyQueueChanges(L, u, out);
    DropRemovedOrders(L, u, out);
    RetypeSentOrders(L, out);
    return true;
  }
  Sim* sim = TheSim(L);
  if (!sim || !UnitStateOf(*sim, u->view.id, out)) return false;
  u->stateCopy = *out, u->stateSync = user->syncCount;
  ApplyQueueChanges(L, u, out);
  DropRemovedOrders(L, u, out);
  RetypeSentOrders(L, out);
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
    // "ID": FAF's patch of the original (FA exe 0x8c55aa jumps to 0x129115c: LuaObject::SetInteger("ID", command+4));
    // the original itself gives only type and position. Signed, as PushCommandId says (read 2026-10-18).
    lua_pushstring(L, "ID");
    PushCommandId(L, c.id);
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

// One statistic of a unit as the simulation's scripts keep it (FAF's Unit:UpdateStat / SetStat), from the engine's
// view of the unit (UnitStateView::stats, engine 13a6e16: every number statistic, sorted by name), into
// u->statCopies: the view is the one safe way to the simulation's state (the engine's fix 47: reading it allocates
// nothing in the simulation's Lua), and StateOf keeps it per sync (while a beat runs on its own thread, the copy from
// before it). A unit the simulation no longer holds: the copy read before (none: type LUA_TNIL).
const UserUnit::StatCopy& StatOf(lua_State* L, const UserUnit* u, const std::string& name) {
  UserUnit::StatCopy& copy = u->statCopies[name];
  UserState* user = UserState::From(L);
  if (copy.sync == user->syncCount) return copy;
  UnitStateView s;
  if (!StateOf(L, u, &s)) return copy;
  auto it = std::lower_bound(s.stats.begin(), s.stats.end(), name,
                             [](const std::pair<std::string, float>& e, const std::string& n) { return e.first < n; });
  const bool found = it != s.stats.end() && it->first == name;
  copy.type = found ? LUA_TNUMBER : LUA_TNIL;
  copy.number = found ? it->second : 0;
  copy.text.clear();
  copy.sync = user->syncCount;
  return copy;
}
// unit:GetStat(name [, default]) (FA exe 0x8c3160, READ 2026-10-20): a statistic of the unit, as its scripts in the
// simulation keep it (Unit:UpdateStat, SetStat). The original's user unit reads a statistics tree it holds by a
// shared pointer (from the beat's sync, its new units' entries of 0x1c bytes at 0x894a1b: the UserUnit constructor
// copies the entry's +0x10 / +0x14 into +0x188 / +0x18c, 0x8bf556; IUnit_UserUnit::GetStat 0x8bf080.. ->
// Stats_StatItem::GetItem 0x40c200, under the tree's lock). INFERRED that the tree is the simulation unit's own (the
// simulation's side of the entry is not read; the lock fits two threads, and FAF's scripts set the statistics in
// the simulation for the interface to read), so the panels see what the simulation set last: FAF's unit view shows
// HitpointsRegeneration, which Unit.lua's OnCreate sets to the
// blueprint's Defense.RegenRate and SetRegen to what veterancy or an aura makes of it; VetLevel, VetExperience,
// ReclaimedMass the same way. (Until 2026-10-20 this gave the default for every name: "+0/s" for every unit, Chris's
// playthrough of 2026-10-19.) The answer: {Value = v} (STAT_GetLuaTable 0x41b390 fills more, by the item's own slot 1,
// and `Children`; the scripts read Value). A name the simulation keeps nothing under: {Value = default}, 0 without
// one, as before. NOT as the original: without a default it finds no item and gives nil (0x8bf080: GetItem(name,
// false), 0x8c3403); with a number it makes the item (0x8bf0a0: GetItem(name, true), the default not used) in the
// tree it shares with the simulation, which the interface here does not write.
int l_GetStatNow(lua_State* L) {
  const UserUnit* u = CheckUnit(L, 1);
  const UserUnit::StatCopy& s = StatOf(L, u, luaL_checkstring(L, 2));
  const bool hasDefault = lua_gettop(L) >= 3 && !lua_isnil(L, 3);
  lua_newtable(L);
  lua_pushstring(L, "Value");
  if (s.type == LUA_TNUMBER) lua_pushnumber(L, s.number);
  else if (s.type == LUA_TSTRING) lua_pushstring(L, s.text.c_str());
  else if (hasDefault) lua_pushvalue(L, 3);
  else lua_pushnumber(L, 0);
  lua_rawset(L, -3);
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
// Any builder's, a factory's or a commander's alike (READ, the queue's making 0x835df0): the unit's own command queue
// (its slot 0x18, +0x3c8; not the factory's rally queue, +0x3cc), its BuildFactory, BuildMobile and Upgrade orders
// (0x8b4140: 7, 8, 0x1b) that name a blueprint; and only for a unit whose blueprint is in SHOWQUEUE (0x835e55: no
// queue at all otherwise; every commander, engineer and factory of the game has it).

void BuildQueue(lua_State* L, const UserUnit* factory, std::vector<CommandView>* out) {
  UnitStateView s;
  if (!factory || !InCategory(L, factory, "SHOWQUEUE") || !StateOf(L, factory, &s)) return;
  for (const CommandView& c : s.queue)
    if (c.type == static_cast<int>(CommandType::BuildFactory) || c.type == static_cast<int>(CommandType::BuildMobile) ||
        c.type == static_cast<int>(CommandType::Upgrade))
      if (!c.blueprint.empty()) out->push_back(c);
}
// The queue as the panel shows it: orders for the same thing that follow one another are ONE
// entry with their counts added up (two engineers, a tank, two engineers: three entries of 2, 1
// and 2), as the original makes its list (0x835f5a..0x835fe3, READ: an order whose blueprint id is the last entry's
// adds its count, 0x8b4220, to it and its id to the entry's ids; else a new entry), whatever the orders' types.
struct QueueStack {
  std::string blueprint;
  int count = 0;
  std::vector<CommandView> orders;  // first to last
};
std::vector<QueueStack> StacksOf(const std::vector<CommandView>& queue) {
  std::vector<QueueStack> stacks;
  for (const CommandView& c : queue) {
    if (stacks.empty() || stacks.back().blueprint != c.blueprint) {
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
// (An empty queue is nil, 0x836536 READ; OnQueueChanged gets a table, empty too, 0x836080.)
int l_SetCurrentFactoryForQueueDisplay(lua_State* L) {
  UnitStore& store = Store(L);
  UserUnit* u = ToUnit(L, 1);
  store.queueFactory = u ? u->view.id : 0xffffffffu;
  std::vector<CommandView> queue;
  BuildQueue(L, u, &queue);
  store.queueKey = QueueKey(queue);
  if (queue.empty()) lua_pushnil(L);
  else PushBuildQueue(L, queue);
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

// After a beat: tell the game script when the shown factory's queue is not what it was (the UI manager's beat,
// CUIManager::DoBeat 0x84d310 -> 0x836180, READ: the queue made again and compared with the last). When the unit is
// gone and its last queue was not empty, the script hears OnQueueChanged(nil) once (0x8362ec).
void ReportQueueChange(lua_State* L) {
  UnitStore& store = Store(L);
  if (store.queueFactory == 0xffffffffu || !UserState::From(L)->gameInterface) return;
  auto it = store.units.find(store.queueFactory);
  const bool gone = it == store.units.end();
  if (gone && store.queueKey.empty()) return;
  std::vector<CommandView> queue;
  if (!gone) BuildQueue(L, it->second.get(), &queue);
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
      if (gone) lua_pushnil(L);
      else PushBuildQueue(L, queue);
      if (lua_pcall(L, 1, 0, top + 1) != 0)
        Logf(LogLevel::Warning, "Error running '/lua/ui/game/gamemain.lua:OnQueueChanged': %s", lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
    }
  }
  lua_settop(L, top);
}

}  // namespace

// READ (0x866a30): without a session the engine's "no session" line; without arguments the
// command's usage (0x866ad6: one text of several lines); a word with '+' it does not know, "Unknown
// modifier %s" (0x866c69), and on. (The original hands that %s the string object itself, copied
// onto the stack, 0x866c6e, so what it shows there is not the word; here the word.)
void SelectByCategoryCommand(lua_State* L, const std::string& arguments) {
  if (!TheSim(L)) return ConsoleNoSession(L);
  if (arguments.find_first_not_of(" \t") == std::string::npos)
    return ConsolePrint(L,
                        "UI_SelectByCategory [+add] [+nearest] [+idle] [+goto] categoryExpression\n"
                        "Select a set of units with the following parameters....\n"
                        "+add : add to current selection\n"
                        "+nearest : select only nearest unit matching category\n"
                        "+idle : select only idle units\n"
                        "+inview: select only units in the current view\n"
                        "+goto: goto the unit if it's already selected\n"
                        "categoryExpression uses the format CAT1 CAT2, CAT3 CAT4 where a space means intersection and a comma means "
                        "union\n");
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
    else ConsolePrintf(L, "Unknown modifier %s", word.c_str());
  }
  if (expr.empty()) return;
  expr.pop_back();
  if (!SelectUnitsByCategory(L, expr.c_str(), add, inView, nearest, idle, goTo, noEngineers))
    Logf(LogLevel::Warning, "UI_SelectByCategory: invalid category expression: %s", expr.c_str());
}

void ExpandCurrentSelection(lua_State* L) {
  Sim* sim = TheSim(L);
  if (!sim) return ConsoleNoSession(L);  // (READ 0x866985: the engine's "no session" line)
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

// The outlines round building sites while a building is being placed: the original's DrawAllUnitSkirts (FA exe
// 0x85ad80, READ 2026-10-27; the world view's drawing calls it in a build mode, 0x853e9a). It walks the session's
// command table (session+0x3fc, the CommandManager: its map +0xcb4, nodes from +0xcb8, each node's +0x10 the command)
// and draws, for each command whose type is 8 (BuildMobile, 0x8b4140), the skirt of the command's blueprint (its
// +0x20, 0x555040) at its target's place (0x8b4080 -> 0x8bed50) in 0xc00080ff (Moho::DrawUnitSkirt 0x85abd0:
// RUnitBlueprint::GetSkirtRect round the place). Standing buildings get none: only build orders, begun or not.
// Which commands the table holds (READ the same day): the simulation publishes a command (CCommandDb::PublishSyncData
// 0x6e0f50 -> CUnitCommand::RefreshPublishedCommandEvent 0x6e8dc0) only while one of its units is visible to the
// focus army in the entity sense (the entity's +0x110), and a Unit's is set only for the focus army's own units, or
// for every unit without a focus army (Unit::UpdateVisibility 0x6abc30); every other army's units reach the interface
// as recon blips, whose unit data never carries a queue. So: the focus army's build orders (an observer's: every
// army's), never an ally's or an enemy's. (Ours before drew the outline round every standing building of any army,
// fog or not: Chris, 2026-10-25, "the outline (blue construction box) of what enemies are building is visible".)
// The order's units walked here are the interface's units with orders; at most 600 sites (ours, for the cost).
void BuildOrderSitesForWorld(lua_State* L, std::vector<WorldGhost>* out) {
  Sim* sim = TheSim(L);
  UserState* user = UserState::From(L);
  if (!sim || !user->units) return;
  UnitStore& store = static_cast<UnitStore&>(*user->units);
  const int focus = sim->focusArmy + 1;
  std::set<uint32_t> seen;
  for (auto& entry : store.units) {
    const UserUnit* u = entry.second.get();
    if (u->gone || u->view.dead || (focus >= 1 && u->view.army != focus) || u->view.queueLength == 0) continue;
    UnitStateView s;
    if (!StateOf(L, u, &s)) continue;
    for (const CommandView& c : s.queue) {
      if (static_cast<CommandType>(c.type) != CommandType::BuildMobile || c.blueprint.empty() || !c.hasPosition) continue;
      if (!seen.insert(c.id).second || out->size() >= 600) continue;
      WorldGhost g;
      g.blueprint = c.blueprint;
      for (int i = 0; i < 3; ++i) g.position[i] = c.position[i];
      out->push_back(g);
    }
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

void SelectionForWorld(lua_State* L, std::vector<uint32_t>* ids, std::vector<WorldGhost>* ghosts, bool queueShown,
                       std::vector<WorldGhost>* queued) {
  Sim* sim = TheSim(L);
  UserState* user = UserState::From(L);
  if (!sim || !user->units) return;
  UnitStore& store = static_cast<UnitStore&>(*user->units);
  int shown = 0;
  for (uint32_t id : store.extraSelect) {  // (shown as selected, not selected)
    auto it = store.units.find(id);
    if (it != store.units.end() && !it->second->gone && !it->second->view.dead) ids->push_back(id);
  }
  // (The lines of the orders and their marks: OrderLinesForWorld, the original's command graph.)
  for (UserUnit* u : store.selection) {
    if (u->gone || u->view.dead) continue;
    ids->push_back(u->view.id);
    if (!queued || !queueShown || u->view.army != sim->focusArmy + 1 || u->view.queueLength == 0 || ++shown > 60) continue;
    UnitStateView s;
    if (!UnitStateOf(*sim, u->view.id, &s)) continue;
    for (const CommandView& c : s.queue) {
      if (!c.hasPosition) continue;
      if (static_cast<CommandType>(c.type) == CommandType::BuildMobile && !c.blueprint.empty() && queued->size() < 400) {
        WorldGhost site;
        site.blueprint = c.blueprint;
        for (int i = 0; i < 3; ++i) site.position[i] = c.position[i];
        queued->push_back(site);
      }
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

// --- the order lines while Shift is held: the original's command graph ---
//
// Read from the FA exe by a helper of the ui chat (2026-10-17; UICommandGraph.cpp): the graph exists
// while Shift (key 306) is held (GetCommandGraph(true) 0x895eb0, made by 0x824810, which reads the
// two tables, LoadGraphParams), and is worked out again when marked (0x828fb0 -> 0x826740, 0x826ba0).
// It holds the orders of EVERY unit the session knows (its entity table session+0x50, mask 0x100),
// not only the selection's; no army is tested in the graph's code (the session's units with orders
// are the focus army's: see BuildOrderGraph). For each unit not INSIGNIFICANTUNIT (0x8267ca) its
// queue is walked (0x826140), and a non-MOBILE FACTORY's second
// queue too (vtbl+0x20: the factory's rally orders, the queue its products are given):
// - a unit being upgraded (unit state 0x25, BeingUpgraded) is left out (0x8261a6);
// - the start node of the queue's first command (one per first command, graph+0xd58) gathers the
//   units' places: it stands at their middle (a group's order: one leg from the group's centre);
// - each command not of the kinds never drawn (NeverDrawn) gets its node (one per command, shared by
//   its units, graph+0xd30, at its target's place), and a leg from the node before (0x826960): legs
//   between the same two nodes are one, counting its units (n; its width CalculateWaypointLineWidth(n)
//   of the game's Lua, 0x826ad6);
// - the first Guard, Patrol, FormPatrol, or Attack / FormAttack on a place on the ground (its target's
//   type 2, 0x826484), is where the queue loops back to: a leg from the last node to it (0x826501).
// Each node's tangent is the sum of its legs' directions in and out, each counting min(n,
// ui_CommandGraphMaxNodeUnits), made unit length; its size sqrt(max(in, out)) of those counts (0x8275b0).
// When drawn, each leg's state (0x828bec): highlighted when the last queue walked along it is the unit
// under the mouse's or shares a command with it (the leg's byte +0x18), else the state of the node it
// leads to (0x8281e0): highlighted when the unit under the mouse carries that node's command or the
// waypoint under the mouse is it (OrderGraph::hovered, see "a waypoint under the mouse" below), selected
// when any unit carrying it is selected (session+0x4a0), else normal. The leg is drawn as the command it leads to
// says (its record: texture, colours by state, uvAspect, animRate); a node's mark (0x8282b0) by its
// own command's record and state: its waypoint texture in the state's colour, at the node's size
// times the state's scale; without a texture none, but a highlighted node a solid ff30f030 square.
// Where the original's order is not this (its legs go by texture, 0x829190): the legs are handed
// highlighted first, then selected, then the rest (the first leg handed keeps a pixel where legs
// cross, the original's stencil; the renderer's answer to request 30).
// The time left at each node (the ETA texts, 0x828610): WorkOutEtas, drawn by the world view.
// Left out: the glow pass (only the bloom's mask). The path preview (ui_DrawPathPreview, never on in the
// original; ours an option): WorkOutPathPreview. (Dragging a node: further down.)
namespace {

// The kinds of orders the graph never draws (0x82632e: 1 << type against 0x2_d80000ea): Stop, Dive,
// BuildSiloTactical, BuildSiloNuke, BuildFactory, Upgrade, Script, KillSelf, DestroySelf, Pause. (So an
// attack-move, a Script order and an AggressiveMove, draws the AggressiveMove's leg alone.)
bool NeverDrawn(int type) { return type >= 0 && type < 64 && ((0x2d80000eaull >> type) & 1) != 0; }

// Where a queue loops back to (0x82646e, the jump table 0x826530 by type - 10).
bool StartsLoop(const CommandView& c) {
  switch (static_cast<CommandType>(c.type)) {
    case CommandType::Guard:
    case CommandType::Patrol:
    case CommandType::FormPatrol:
      return true;
    case CommandType::Attack:
    case CommandType::FormAttack:  // (on a place on the ground: no entity, a place)
      return c.entity == 0xffffffffu && c.hasPosition;
    default:
      return false;
  }
}

// The place a command's node stands at (0x81cfd0 -> 0x8bed50): its target entity's place (the
// entity's +0x84: as of the last beat; the node follows it, 0x828fb0), else the place on the ground
// it names. An order without a target has the original's "no place" (0x10a99a4); INFERRED drawn
// nowhere: such an order gets no node here. A target unit the player's army knows nothing of now
// stands where the order put it.
bool CommandPlace(lua_State* L, UnitStore& store, const CommandView& c, float out[3]) {
  if (c.entity != 0xffffffffu) {
    auto it = store.units.find(c.entity);
    if (it != store.units.end() && !it->second->gone && !it->second->view.dead && ReconBitsOf(L, it->second.get()) != 0) {
      for (int i = 0; i < 3; ++i) out[i] = it->second->view.position[i];
      return true;
    }
  }
  if (!c.hasPosition) return false;
  for (int i = 0; i < 3; ++i) out[i] = c.position[i];
  return true;
}

void NodePlace(const OrderGraph::Node& n, float out[3]) {
  const float k = n.count != 0 ? 1.0f / n.count : 0.0f;
  for (int i = 0; i < 3; ++i) out[i] = n.sum[i] * k;
}

// The graph worked out afresh from the units' queues as of the last beat.
void BuildOrderGraph(lua_State* L, Sim& sim, UnitStore& store, OrderGraph& g) {
  g.nodes.clear();
  g.legs.clear();
  g.queues.clear();
  g.carriers.clear();
  // Whose orders the interface holds (READ 2026-10-27): the focus army's units' alone, every army's for an observer
  // (no focus army). The session's units with a queue are the simulation's Units whose interface is visible to the
  // focus army, and Unit::UpdateVisibility (FA exe 0x6abc30) makes that the focus army's own units (every unit when
  // there is none); an ally's or an enemy's unit reaches the interface as a recon blip (ReconBlip::UpdateVisibility
  // 0x5bee40), whose unit data (blip+0x298) is refreshed by 0x5bf810 (its place, layer, a few flags), never its queue;
  // and the commands themselves are published only while one of their units is so visible (0x6e8dc0). (Ours before
  // held its allies' too: Chris, 2026-10-25, "Pressing shift I can see all the actions of allies vs just my own".)
  const int focus = sim.focusArmy + 1;
  std::map<int, bool> known;
  for (const ArmyView& a : ArmyViews(sim)) known[a.index] = focus < 1 || a.index == focus;
  // The queues to walk, by the units' entity ids (INFERRED: the original goes through its session's
  // entity table in its own order), and every command's units.
  struct Walk {
    const UserUnit* unit;
    std::vector<CommandView> queue;
  };
  std::vector<Walk> walks;
  for (const auto& entry : store.units) {
    const UserUnit* u = entry.second.get();
    if (u->gone || u->view.dead || !known[u->view.army]) continue;
    const bool factory = InCategory(L, u, "FACTORY") && !InCategory(L, u, "MOBILE");
    if (u->view.queueLength == 0 && !factory) continue;
    UnitStateView s;
    if (!StateOf(L, u, &s)) continue;
    for (const CommandView& c : s.queue) g.carriers[c.id].push_back(u->view.id);
    if (factory && s.hasFactoryQueue)
      for (const CommandView& c : s.factoryQueue) g.carriers[c.id].push_back(u->view.id);
    if (InCategory(L, u, "INSIGNIFICANTUNIT") || BeingUpgraded(L, &sim, u)) continue;
    if (!s.queue.empty()) walks.push_back({u, std::move(s.queue)});
    if (factory && s.hasFactoryQueue && !s.factoryQueue.empty()) walks.push_back({u, std::move(s.factoryQueue)});
  }
  std::map<uint32_t, int>& nodeOf = g.nodeOf;
  std::map<uint32_t, int> startOf;
  nodeOf.clear();
  std::map<std::pair<int, int>, int> legOf;
  const uint64_t sync = UserState::From(L)->syncCount;
  // A node being dragged stands where the mouse put it (its byte +0x1c: it does not follow its order, 0x828fb0; the
  // original keeps its command nodes from one working out to the next, 0x826000, so the drag outlives it); an
  // order a drag has just given a new target, where it was sent (the original's own copy of the order, 0x8b4a40),
  // until the simulation's queue shows it (or ten syncs have passed: INFERRED, the simulation turned it down).
  auto placeOf = [&](const CommandView& c, float at[3]) {
    if (g.drag.command == c.id && g.drag.moved) {
      for (int i = 0; i < 3; ++i) at[i] = g.drag.at[i];
      return;
    }
    auto r = g.retargets.find(c.id);
    if (r == g.retargets.end()) return;
    const OrderGraph::Retarget& t = r->second;
    const bool shown = t.entity != 0xffffffffu ? c.entity == t.entity
                                               : c.hasPosition && std::fabs(c.position[0] - t.at[0]) < 0.05f && std::fabs(c.position[2] - t.at[2]) < 0.05f;
    if (shown || sync > t.sync + 10) {
      g.retargets.erase(r);
      return;
    }
    for (int i = 0; i < 3; ++i) at[i] = t.at[i];
  };
  auto addLeg = [&](int from, int to, int queue) {
    auto [it, fresh] = legOf.try_emplace({from, to}, static_cast<int>(g.legs.size()));
    if (fresh) {
      OrderGraph::Leg leg;
      leg.from = from, leg.to = to;
      g.legs.push_back(leg);
      g.nodes[static_cast<size_t>(from)].out.push_back(it->second);
      g.nodes[static_cast<size_t>(to)].in.push_back(it->second);
    }
    OrderGraph::Leg& leg = g.legs[static_cast<size_t>(it->second)];
    ++leg.units;
    leg.lastQueue = queue;
  };
  for (const Walk& w : walks) {
    const int queue = static_cast<int>(g.queues.size());
    OrderGraph::Queue q;
    q.unit = w.unit->view.id;
    for (const CommandView& c : w.queue)
      for (uint32_t unit : g.carriers[c.id])
        if (std::find(q.partners.begin(), q.partners.end(), unit) == q.partners.end()) q.partners.push_back(unit);
    g.queues.push_back(std::move(q));
    auto [start, fresh] = startOf.try_emplace(w.queue[0].id, static_cast<int>(g.nodes.size()));
    if (fresh) g.nodes.emplace_back();
    OrderGraph::Node& s = g.nodes[static_cast<size_t>(start->second)];
    for (int i = 0; i < 3; ++i) s.sum[i] += w.unit->view.position[i];
    s.count += 1;
    int before = start->second, loop = -1;
    for (const CommandView& c : w.queue) {
      if (NeverDrawn(c.type)) continue;
      auto seen = nodeOf.find(c.id);
      int node;
      if (seen == nodeOf.end()) {
        float at[3];
        if (!CommandPlace(L, store, c, at)) continue;
        placeOf(c, at);
        node = static_cast<int>(g.nodes.size());
        OrderGraph::Node n;
        n.command = c.id, n.type = c.type;
        n.blueprint = c.blueprint;
        for (int i = 0; i < 3; ++i) n.sum[i] = at[i];
        n.count = 1;
        g.nodes.push_back(std::move(n));
        nodeOf[c.id] = node;
      } else {
        node = seen->second;
      }
      if (loop < 0 && StartsLoop(c)) loop = node;
      addLeg(before, node, queue);
      before = node;
    }
    if (loop >= 0 && loop != before) addLeg(before, loop, queue);
  }
  // The nodes' tangents and sizes (0x8275b0).
  const int most = GraphVars().maxNodeUnits;
  for (OrderGraph::Node& n : g.nodes) {
    float at[3], sum[3] = {0, 0, 0};
    NodePlace(n, at);
    int in = 0, out = 0;
    auto add = [&](const float d0[3], int units, int* total) {
      const int w = std::min(units, most);
      float d[3] = {d0[0], d0[1], d0[2]};
      const float length2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
      if (length2 > 0) {
        const float k = static_cast<float>(w) / std::sqrt(length2);
        for (float& v : d) v *= k;
      }
      for (int i = 0; i < 3; ++i) sum[i] += d[i];
      *total += w;
    };
    for (int li : n.in) {
      const OrderGraph::Leg& leg = g.legs[static_cast<size_t>(li)];
      float from[3];
      NodePlace(g.nodes[static_cast<size_t>(leg.from)], from);
      const float d[3] = {at[0] - from[0], at[1] - from[1], at[2] - from[2]};
      add(d, leg.units, &in);
    }
    for (int li : n.out) {
      const OrderGraph::Leg& leg = g.legs[static_cast<size_t>(li)];
      float to[3];
      NodePlace(g.nodes[static_cast<size_t>(leg.to)], to);
      const float d[3] = {to[0] - at[0], to[1] - at[1], to[2] - at[2]};
      add(d, leg.units, &out);
    }
    const float length = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
    for (int i = 0; i < 3; ++i) n.tangent[i] = length > 1e-6f ? sum[i] / length : 0.0f;
    n.size = std::sqrt(static_cast<float>(std::max(in, out)));
  }
}

// --- the time left at the graph's nodes: the original's ETA texts ---
//
// Read from the FA exe by helpers of the ui chat (2026-10-17, 2026-10-18). Only when the graph has been
// worked out again (0x828fb0: 0x826740 / 0x826ba0, then the nodes put at their targets) is every node's
// time worked out (0x8272a0, at 0x8290de), kept in the node (+0x44, 0 in a new node) as the beat its
// order will be done in; it is drawn as that beat less the session's beat now (session+0x458, which
// CWldSession::DoBeat sets to each beat's number, 0x8945a3; here the simulation's beat), so between two
// workings out it counts down. A node:
//   its +0x44 = now (it is being worked out); latest = now; for each leg into it: the node it comes
//   from worked out first, latest = max(latest, that node's + the leg's walk, TravelTicks); then
//   latest += its own work (WorkTicks); +0x44 = latest when that is later than now, else now - 1
//   (nothing to wait for: no text, 0x828648 draws none below now).
// So a start node (no leg in, no work) is now - 1, and a first order's time is its walk less a beat. A
// node met again while it is being worked out (a patrol's loop) gives now. The original goes through
// the nodes of its list graph+0xd38 (start nodes too) in that list's order; this, through the command
// nodes in the order the queues were walked (they can differ only round a loop).

// The speed of a unit as the order lines' times take it (0x826d9e): its blueprint's Air.MaxAirspeed
// (bp+0x370) when Air.CanFly (bp+0x368), else its Physics.MaxSpeed (bp+0x2a0); no buff, no multiplier.
float SpeedOf(lua_State* L, const UserUnit* u) {
  UnitStore& store = Store(L);
  auto it = store.speeds.find(u->view.blueprint);
  if (it != store.speeds.end()) return it->second;
  const bool fly = CanFlyOf(L, u);
  float speed = 0;
  const int top = lua_gettop(L);
  PushBlueprint(L, u->view.blueprint);
  if (lua_istable(L, -1)) {
    lua_pushstring(L, fly ? "Air" : "Physics");
    lua_rawget(L, -2);
    if (lua_istable(L, -1)) {
      lua_pushstring(L, fly ? "MaxAirspeed" : "MaxSpeed");
      lua_rawget(L, -2);
      speed = static_cast<float>(lua_tonumber(L, -1));
    }
  }
  lua_settop(L, top);
  store.speeds[u->view.blueprint] = speed;
  return speed;
}

// The beats the units of the order a leg leads to take along it (0x826c50): the straight distance
// between its two nodes' places, times 10, over the slowest speed of the order's units (the command's
// own list, 0x8b43f0; those of speed 0 left out), rounded up (0x826e45). 0 when the order is not one
// they walk to (the jump table 0x826ee4 by type - 2: Move, FormMove, BuildMobile, Attack, FormAttack,
// Patrol, FormPatrol, AggressiveMove and FormAggressiveMove walk) or none of its units moves (a
// factory's rally point).
int64_t TravelTicks(lua_State* L, UnitStore& store, OrderGraph& g, const OrderGraph::Leg& leg) {
  const OrderGraph::Node& to = g.nodes[static_cast<size_t>(leg.to)];
  switch (static_cast<CommandType>(to.type)) {
    case CommandType::Move:
    case CommandType::FormMove:
    case CommandType::BuildMobile:
    case CommandType::Attack:
    case CommandType::FormAttack:
    case CommandType::Patrol:
    case CommandType::FormPatrol:
    case CommandType::AggressiveMove:
    case CommandType::FormAggressiveMove:
      break;
    default:
      return 0;
  }
  float a[3], b[3];
  NodePlace(g.nodes[static_cast<size_t>(leg.from)], a);
  NodePlace(to, b);
  const float distance = std::sqrt((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]) + (b[2] - a[2]) * (b[2] - a[2]));
  float slowest = std::numeric_limits<float>::max();
  for (uint32_t id : g.carriers[to.command]) {
    auto it = store.units.find(id);
    if (it == store.units.end() || it->second->gone) continue;
    const float speed = SpeedOf(L, it->second.get());
    if (speed > 0 && speed < slowest) slowest = speed;
  }
  if (!(slowest < std::numeric_limits<float>::max()) || !(slowest > 0)) return 0;
  return static_cast<int64_t>(std::ceil(distance * 10.0f / slowest));
}

// The beats a build order's builders still need at its node (0x826f10): only BuildMobile and
// BuildFactory orders (a BuildFactory has no node: NeverDrawn). Its builders are the order's units in
// FACTORY or ENGINEER (0x827035); each adds its rate, 1 / the first value of the game's
// GetConstructEconomyModel(builder, the blueprint's Economy) (0x830e30, lua_call with one result;
// /lua/game.lua: BuildTime over builder:GetBuildRate(), in seconds; an error is logged "Error
// estimating work time: %s"); what is left of the building is 1, or less for a builder whose first
// order this is (0x8b72f0: the first command of its queue) and which works on something (its focus,
// unit+0x210 as GetFocus, 0x824500): 1 - its health / max health (+0x68 / +0x6c, as GetHealth
// 0x8c3fa8 and GetMaxHealth read them), the least of them. The beats: what is left times 10 over the
// rates' sum, cut to a whole number (0x827255, the x87 control word's 0xc00; 0 without a rate).
int64_t WorkTicks(lua_State* L, UnitStore& store, OrderGraph& g, const OrderGraph::Node& n) {
  if (n.command == 0xffffffffu) return 0;
  const auto type = static_cast<CommandType>(n.type);
  if (type != CommandType::BuildMobile && type != CommandType::BuildFactory) return 0;
  if (n.blueprint.empty()) return 0;
  const int top = lua_gettop(L);
  lua_getglobal(L, "import");
  lua_pushstring(L, "/lua/game.lua");
  if (lua_pcall(L, 1, 1, 0) != 0 || !lua_istable(L, -1)) {
    lua_settop(L, top);
    return 0;
  }
  lua_pushstring(L, "GetConstructEconomyModel");
  lua_gettable(L, -2);
  const int model = lua_gettop(L);
  float left = 1, rate = 0;
  for (uint32_t id : g.carriers[n.command]) {
    auto it = store.units.find(id);
    if (it == store.units.end() || it->second->gone) continue;
    UserUnit* u = it->second.get();
    if (!InCategory(L, u, "FACTORY") && !InCategory(L, u, "ENGINEER")) continue;
    UnitStateView s;
    if (StateOf(L, u, &s) && s.focus != 0xffffffffu && !s.queue.empty() && s.queue[0].id == n.command) {
      auto f = store.units.find(s.focus);
      if (f != store.units.end() && !f->second->gone && f->second->view.maxHealth > 0)
        left = std::min(left, 1.0f - f->second->view.health / f->second->view.maxHealth);
    }
    lua_pushvalue(L, model);
    PushObject(L, u);
    PushBlueprint(L, n.blueprint);
    if (lua_istable(L, -1)) {
      lua_pushstring(L, "Economy");
      lua_gettable(L, -2);
      lua_remove(L, -2);
    }
    if (lua_pcall(L, 2, 1, 0) != 0) {
      Logf(LogLevel::Warning, "Error estimating work time: %s", lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
      lua_settop(L, model);
      continue;
    }
    const float seconds = static_cast<float>(lua_tonumber(L, -1));
    lua_settop(L, model);
    if (seconds > 0) rate += 1.0f / seconds;
  }
  lua_settop(L, top);
  if (!(rate > 0)) return 0;
  return static_cast<int64_t>(static_cast<double>(left) * 10.0 / static_cast<double>(rate));  // (0xc00: cut toward 0)
}

void EtaOf(lua_State* L, UnitStore& store, OrderGraph& g, int node, int64_t now) {
  OrderGraph::Node& n = g.nodes[static_cast<size_t>(node)];
  if (n.etaDone) return;
  n.etaDone = true;
  n.eta = now;
  int64_t latest = now;
  for (int li : n.in) {
    const OrderGraph::Leg& leg = g.legs[static_cast<size_t>(li)];
    EtaOf(L, store, g, leg.from, now);
    latest = std::max(latest, g.nodes[static_cast<size_t>(leg.from)].eta + TravelTicks(L, store, g, leg));
  }
  latest += WorkTicks(L, store, g, n);
  n.eta = n.eta < latest ? latest : n.eta - 1;
}

// Every command node's time (0x8290d8; the start nodes as they are met).
void WorkOutEtas(lua_State* L, UnitStore& store, OrderGraph& g, int64_t now) {
  for (OrderGraph::Node& n : g.nodes) n.eta = 0, n.etaDone = false;
  for (size_t i = 0; i < g.nodes.size(); ++i)
    if (g.nodes[i].command != 0xffffffffu) EtaOf(L, store, g, static_cast<int>(i), now);
}

// What the graph gives to draw now, with what the test function reports beside it.
struct OrderGraphOut {
  struct LegOut {
    WorldOrderLeg leg;
    int units = 0, state = 0, from = -1, to = -1;  // state: 0 normal, 1 highlighted, 2 selected (0x8281e0's)
  };
  struct MarkOut {
    WorldOrderMark mark;
    int state = 0, node = -1;
  };
  struct EtaOut {
    OrderEta eta;
    int node = -1;
  };
  std::vector<std::string> textures;
  std::vector<LegOut> legs;
  std::vector<MarkOut> marks;
  std::vector<EtaOut> etas;
  WorldOrderSettings settings;
  // The path preview (ui_DrawPathPreview; WorkOutPathPreview): its legs, the arrowhead's last, drawn before the
  // order lines. `arrow`: the last leg is the arrowhead. `pathfinding`: ui_PathPreview asked for the path the
  // simulation found (none here). `type`: the order whose record draws it; `unit`: the unit it starts from;
  // `points`: the line's points as listed (before the ends are drawn in); `width`: the one width of all of it.
  struct PreviewOut {
    bool shown = false, arrow = false, pathfinding = false;
    int type = 0;
    uint32_t unit = 0xffffffffu;
    std::vector<std::array<float, 3>> points;
    float width = 0, w0 = 0, size = 0;  // (w0: w at the first point; size: max(SizeX, 3 w0) x 0.2)
    std::vector<WorldOrderLeg> legs;
  } preview;
};

// A colour times a colour, as the picture multiplies a texture by its vertex colour.
uint32_t ColorTimes(uint32_t a, uint32_t b) {
  uint32_t out = 0;
  for (int shift = 0; shift < 32; shift += 8) out |= (((a >> shift & 0xffu) * (b >> shift & 0xffu) + 127u) / 255u) << shift;
  return out;
}

// The graph while it is held: Shift held (the world view's drawing, GetCommandGraph(true) 0x895eb0 at 0x86ed32), or a
// waypoint being dragged (the dragger holds it too, 0x824026). Not held: let go (made again, its tables read again,
// when next held). While a beat runs on its own thread the graph stays as it is (it would wait for the beat).
OrderGraph& HoldOrderGraph(lua_State* L, bool held) {
  UserState* user = UserState::From(L);
  OrderGraph& g = Store(L).graph;
  held = held || g.drag.command != 0xffffffffu;
  if (!held || !user->HasSim()) {
    g.made = false;
    return g;
  }
  if (user->config().beatRunning && user->config().beatRunning()) return g;
  Sim* sim = TheSim(L);
  if (!sim) {
    g.made = false;
    return g;
  }
  if (!g.made) {
    LoadGraphParams(L, &g);
    g.widths.clear();
    g.made = true;
    g.builtSync = ~0ull;
  }
  // INFERRED: worked out again after every beat (the original marks its graph to be worked out
  // again, the byte graph+0; what marks it is not read; its nodes follow their targets each frame).
  if (g.builtSync != user->syncCount) {
    BuildOrderGraph(L, *sim, Store(L), g);
    WorkOutEtas(L, Store(L), g, sim->tick());
    g.builtSync = user->syncCount;
  }
  return g;
}

// --- the path preview ---
//
// Read from the FA exe by a helper of the ui chat (2026-10-23; UICommandGraph.cpp). The original has it and never
// turns it on; here it is the Interface tab's Path Preview (ours, off unless chosen; Chris, 2026-10-23: "try it
// first ... I want to see it in action first").
// - When: the command graph's drawing (0x829190, only while Shift is held: the world view's 0x86ed00 -> 0x85af40)
//   calls 0x82a380 after the legs, the waypoints' icons and the queued buildings' ghosts, before the ETA texts, when
//   the console's bool ui_DrawPathPreview (0x10a6443, "Turns on/off the arrow line"; 0 at start, nothing in the exe
//   or the game's Lua sets it) is on, with the technique TCommandOther (primbatcher.fx: the texture times the vertex
//   colour, alpha above 0, blended, no depth test, stencil Ref 2 NotEqual Replace: over the order lines, Ref 1, and
//   within itself the first drawn keeps a pixel). Nothing without a focus army (session+0x488, its slot +0x3f0).
// - Two kinds, by the bool ui_PathPreview (0x10a6448, "Turns on/off the pathfinding preview line"):
//   - on: the points are the focus army's previewed path (the army's variable data +0xf8..+0xfc, a vector of path
//     cells, each to the world by COORDS_ToWorldPos 0x50b0f0 with the footprint at +0x104: SizeX, SizeZ, caps), the
//     footprint's SizeX the size below; fewer than two: nothing. The simulation fills it: the graph's update
//     (0x828fb0, 0x829113) runs the console's "path_GeneratePreview end" while Shift is down (GetAsyncKeyState, the
//     window in front) else "path_GeneratePreview start" (0x82916f), the alias of "DoSimCommand path_GeneratePreview"
//     (0xbdc820); the sim's command (CSimConCommand 0xbdc850, handler 0x764cf0: "path preview for army %d", a unit
//     of the command's (0x764b90), from its place or, with "end", the end of its orders (0x764c10) to the cursor's
//     place, a PathPreviewFinder 0x7647e0 of the army, path_TimeoutPreview) finds it. NOT BUILT: our simulation has
//     no path_GeneratePreview and its sync gives the interface no army path (an engine request); so this kind draws
//     nothing here, as the original's does with an empty path, and the command is not sent (it would put a
//     DebugCommand in the replay every frame).
//   - off: the arrow line. Only while the session's cursor info says the cursor is in the world (+0x4b0; INFERRED:
//     the mouse over the world view with ground under it, by the picture last drawn). Of the selected units (0x82a2b0) the one whose blueprint has a
//     named footprint (bp+0x2fc, the pathfinder's; INFERRED ours: BlueprintInfo's footprint found by name, which
//     DeriveUnit does for units that move on land or water) and is not being built (vt+0x34 IsBeingBuilt), the
//     largest by SizeZ (bp+0xb4; a later one only when larger; none: nothing drawn); its Footprint.SizeX (bp+0xd8)
//     the size below. Two points: the unit's place (IUnit::GetPosition, unit+0x84, as of the last beat), or while
//     Shift is down (MAUI_KeyIsDown 0x132; always, as it is drawn only then) the place of the last order of its own
//     queue (0x8b7320: the last non-empty entry of its copy, vt+0x18 = unit+0x3c8; 0x81cfd0: its target's place,
//     CommandPlace; INFERRED: an order without a place leaves the unit's), then the cursor's place (+0x4b4).
// - Drawn (both kinds): w0 = w(first point); size = max(SizeX, 3 w0) x 0.2; one width for all of it, W =
//   (CalculateWaypointLineWidth(the selection's count) + 2 w0) x ui_WaypointLineScale (the game's Lua asked each
//   time). The record (graph+4 + type x 0x54) is the order a click would give now: the right button's action
//   (GetRightMouseButtonAction 0x81ec00 with the cursor info); when it is the end of a command mode (kind 7) the left
//   button's (0x81f7b0); an order (kind 1): its RULEUCC bit (+4), else RULEUCC_Move; to a command type by
//   UnitCommandCapToCommandType 0x821f50 (a bit it does not know, as RULEUCC_Script's: 0, the record of None). Its
//   orderline_texture (none: a plain white, 0x4478c0, where the order lines leave such a leg out), its selected
//   colour (+0x14) for every leg and the arrowhead, its uv aspect (+8) and anim rate (+0xc).
//   Each point in turn (prev and cur both the first at the start): prevprev = prev, prev = cur, cur = the point, next
//   the one after (the last: itself). At the last point, when the record has an arrowhead_texture (+0x4c), the leg
//   prev -> cur is drawn in by W at BOTH ends (MakeSegment3fFromEndpoints 0x4fe130: centre +- (cur - prev) unit x
//   (half its length - W)). The leg prev -> cur (0x827a00, the order lines' curve) with the tangents cur - prevprev
//   and next - prev, each cut to ui_PathSmoothness (2) long (Catmull-Rom without the half). Then the arrowhead: a
//   square of side 2W at cur turned by COORDS_Orient(-(cur - prev) unit) 0x50b480, its corners (+-W, 0, +-W + cap),
//   cap = W / size x arrowhead_cap_offset (+0x40; the game's -0.1), U 1 at the front edge and 0 at the back, V 0..1
//   across (0x82adb7..0x82b137), after a flush (the line's pixels go first).
// Here the legs go to the renderer's order-line legs (WorldPicture::SetOrderLines), before the order lines' (the
// first leg keeps a pixel, as Ref 2 NotEqual keeps the preview over the order lines; INFERRED close enough: the
// original blends the preview over them, ours leaves the order line out under it; and the waypoints' icons, drawn
// after every leg here, go over the preview, where the original draws it after them). Each leg's base width is set
// so that the renderer's (base + 2 max(w(a), w(b))) x scale comes to W (w by the picture last drawn: within a frame's
// camera move of the renderer's). The renderer's curve takes unit tangents times min(0.25 |b - a|, ui_CurveSmoothness
// x width): the tangents' own length (ui_PathSmoothness) is not given (a straight line, the arrow line's, is
// straight either way). The arrowhead: a straight leg of width 2W from cur - (W + cap) to cur + (W - cap) along the
// leg's direction, its U from 1 to 2 (uv aspect 1; the texture repeats in U: the original's 0 to 1), with the record's
// arrowhead_texture (orderline_arrow04: its tip at U 0.8, so INFERRED COORDS_Orient turns +Z onto the vector given:
// the tip forward). The first point's leg to itself (no length: the original's curve there is a speck at most) is
// left out.

// A click's order (its RULEUCC name) as the record's command type (UnitCommandCapToCommandType 0x821f50, READ).
int CommandTypeOfCap(const std::string& rule) {
  static const std::pair<const char*, CommandType> kCaps[] = {
      {"RULEUCC_Move", CommandType::Move}, {"RULEUCC_Stop", CommandType::Stop}, {"RULEUCC_Attack", CommandType::Attack},
      {"RULEUCC_Guard", CommandType::Guard}, {"RULEUCC_Patrol", CommandType::Patrol}, {"RULEUCC_Repair", CommandType::Repair},
      {"RULEUCC_Capture", CommandType::Capture}, {"RULEUCC_Transport", CommandType::TransportUnloadUnits},
      {"RULEUCC_CallTransport", CommandType::TransportLoadUnits}, {"RULEUCC_Nuke", CommandType::Nuke},
      {"RULEUCC_Tactical", CommandType::Tactical}, {"RULEUCC_Teleport", CommandType::Teleport}, {"RULEUCC_Ferry", CommandType::Ferry},
      {"RULEUCC_SiloBuildTactical", CommandType::BuildSiloTactical}, {"RULEUCC_SiloBuildNuke", CommandType::BuildSiloNuke},
      {"RULEUCC_Sacrifice", CommandType::Sacrifice}, {"RULEUCC_Pause", CommandType::Pause}, {"RULEUCC_Overcharge", CommandType::OverCharge},
      {"RULEUCC_Dive", CommandType::Dive}, {"RULEUCC_Reclaim", CommandType::Reclaim}, {"RULEUCC_SpecialAction", CommandType::SpecialAction},
  };
  for (const auto& [name, type] : kCaps)
    if (rule == name) return static_cast<int>(type);
  return 0;  // (RULEUCC_RetaliateToggle, _Dock, _Script and others: none)
}

void WorkOutPathPreview(lua_State* L, OrderGraph& g, uint32_t rollover, const PathPreviewView& view,
                        const std::function<int(const std::string&)>& texture, OrderGraphOut::PreviewOut* out) {
  const CommandGraphVars& vars = GraphVars();
  if (!vars.drawPathPreview || FocusArmyNow(L) < 0) return;
  UnitStore& store = Store(L);
  using Point = std::array<float, 3>;
  std::vector<Point>& points = out->points;
  float sizeX = 1;
  if (vars.pathPreview) {
    out->pathfinding = true;  // (the army's previewed path: not given to the interface, see above)
    return;
  }
  if (!view.cursorInWorld) return;
  // the selected unit it starts from (0x82a2b0)
  SimBlueprints* bps = SimBlueprints::From(L);
  UserUnit* unit = nullptr;
  float largest = 0;
  for (UserUnit* u : store.selection) {
    if (u->gone) continue;
    const BlueprintInfo* bp = bps ? BlueprintOf(bps, u) : nullptr;
    if (!bp || !bp->hasFootprint || bp->footprint.name.empty() || u->view.beingBuilt) continue;
    float sizeZ = 1;
    const int top = lua_gettop(L);
    PushBlueprint(L, u->view.blueprint);
    if (lua_istable(L, -1)) {
      lua_getfield(L, -1, "SizeZ");
      if (lua_isnumber(L, -1)) sizeZ = static_cast<float>(lua_tonumber(L, -1));
    }
    lua_settop(L, top);
    if (sizeZ > largest) largest = sizeZ, unit = u, sizeX = bp->footprint.sizeX;
  }
  if (!unit) return;
  out->unit = unit->view.id;
  Point start = {unit->view.position[0], unit->view.position[1], unit->view.position[2]};
  if (KeyIsDown(L, 306)) {
    UnitStateView s;
    if (StateOf(L, unit, &s) && !s.queue.empty()) {
      float at[3];
      if (CommandPlace(L, store, s.queue.back(), at)) start = {at[0], at[1], at[2]};
    }
  }
  points.push_back(start);
  points.push_back({view.cursor[0], view.cursor[1], view.cursor[2]});

  // the order a click would give now, and its record
  std::string rule;
  if (!view.mode.empty()) {
    if (view.mode == "order") rule = view.name;  // (the right button ends the mode: the left button's, the mode's order)
    else rule = "RULEUCC_Move";                  // (a building's or a ping's left action is no order, kind 1)
  } else {
    rule = RightClickOrder(L, rollover);
    if (rule.empty() || rule == "RULEUCC_Invalid") rule = "RULEUCC_Move";  // (no order: kind 0, RULEUCC_Move)
  }
  out->type = CommandTypeOfCap(rule);
  const OrderGraph::Record& r = g.records[static_cast<size_t>(out->type < static_cast<int>(g.records.size()) ? out->type : 0)];

  // one width for all of it
  auto pixelAt = [&](const Point& p) {
    const float w = view.pixelAt ? view.pixelAt(p.data()) : 0.0f;
    return w > 0 && std::isfinite(w) ? w : 0.0f;
  };
  const float w0 = pixelAt(points[0]);
  const float size = 0.2f * std::max(sizeX, 3 * w0);
  out->w0 = w0, out->size = size;
  int count = 0;
  for (const UserUnit* u : store.selection) count += u->gone ? 0 : 1;
  const float scale = vars.waypointLineScale;
  const float W = (WaypointLineWidth(L, &g, count) + 2 * w0) * scale;
  out->width = W;
  const int lineTexture = texture(r.lineTexture);  // (none: -1, drawn plain white)
  const bool arrow = !r.arrowTexture.empty();
  auto sub = [](const Point& a, const Point& b) { return Point{a[0] - b[0], a[1] - b[1], a[2] - b[2]}; };
  auto length = [](const Point& v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
  auto leg = [&](const Point& a, const Point& b, const Point& ta, const Point& tb, float width, int tex, float aspect, float rate) {
    WorldOrderLeg l;
    for (int i = 0; i < 3; ++i) l.a[i] = a[i], l.b[i] = b[i], l.ta[i] = ta[i], l.tb[i] = tb[i];
    // (the renderer widens a leg by two pixels' worth at its nearer end: given so that it comes to `width`)
    l.baseWidth = scale != 0 ? width / scale - 2 * std::max(pixelAt(a), pixelAt(b)) : 0.0f;
    l.color = r.lineColor[1];
    l.texture = tex, l.uvAspect = aspect, l.animRate = rate;
    out->legs.push_back(l);
  };
  Point prevprev = points[0], prev = points[0], cur = points[0];
  for (size_t i = 0; i < points.size(); ++i) {
    prevprev = prev, prev = cur, cur = points[i];
    const bool last = i + 1 == points.size();
    const Point next = last ? cur : points[i + 1];
    if (last && arrow) {  // (both ends drawn in by W, 0x82aafe)
      const Point half = {(cur[0] - prev[0]) * 0.5f, (cur[1] - prev[1]) * 0.5f, (cur[2] - prev[2]) * 0.5f};
      const float extent = length(half);
      if (extent > 0) {
        const Point centre = {prev[0] + half[0], prev[1] + half[1], prev[2] + half[2]};
        const float k = (extent - W) / extent;
        for (int j = 0; j < 3; ++j) cur[j] = centre[j] + half[j] * k, prev[j] = centre[j] - half[j] * k;
      }
    }
    // the tangents, each at most ui_PathSmoothness long (here: their directions, unit length)
    auto tangent = [&](Point t) {
      const float n = length(t);
      for (float& v : t) v = n > 0 ? v / n : 0.0f;
      return t;
    };
    const Point chord = sub(cur, prev);
    if (length(chord) > 1e-6f) leg(prev, cur, tangent(sub(cur, prevprev)), tangent(sub(next, prev)), W, lineTexture, r.uvAspect, r.animRate);
    if (last && arrow && length(chord) > 1e-6f && size > 0) {
      const Point dir = tangent(chord);
      const float cap = W / size * r.capOffset;
      const Point back = {cur[0] - dir[0] * (W + cap), cur[1] - dir[1] * (W + cap), cur[2] - dir[2] * (W + cap)};
      const Point front = {cur[0] + dir[0] * (W - cap), cur[1] + dir[1] * (W - cap), cur[2] + dir[2] * (W - cap)};
      leg(back, front, dir, dir, 2 * W, texture(r.arrowTexture), 1.0f, 0.0f);
      out->arrow = true;
    }
  }
  out->shown = !out->legs.empty();
}

void WorkOutOrderGraph(lua_State* L, bool shown, uint32_t rollover, OrderGraphOut* out, const PathPreviewView* preview = nullptr) {
  OrderGraph& g = HoldOrderGraph(L, shown);
  const CommandGraphVars& vars = GraphVars();
  out->settings.lineScale = vars.waypointLineScale;
  out->settings.curveSmoothness = vars.curveSmoothness;
  out->settings.curveSegments = vars.curveSegments;
  out->settings.markMinPixels = vars.minWaypointSize;
  out->settings.markMaxPixels = vars.maxWaypointSize;
  if (!g.made || !shown) return;  // (drawn only while Shift is held, 0x86ed00: a drag alone holds it undrawn)
  UnitStore& store = Store(L);
  std::vector<uint32_t> selected;
  for (const UserUnit* u : store.selection) selected.push_back(u->view.id);
  auto has = [](const std::vector<uint32_t>& list, uint32_t id) { return std::find(list.begin(), list.end(), id) != list.end(); };
  auto stateOf = [&](const OrderGraph::Node& n) {
    if (n.command == 0xffffffffu) return 0;
    const std::vector<uint32_t>& units = g.carriers[n.command];
    if (rollover != 0xffffffffu && has(units, rollover)) return 1;
    if (n.command == g.hovered) return 1;  // (the waypoint under the mouse, session+0x4c8: 0x828246)
    for (uint32_t u : units)
      if (has(selected, u)) return 2;
    return 0;
  };
  auto slot = [](int state) { return state == 1 ? 2 : state == 2 ? 1 : 0; };  // (the record's normal, selected, highlight)
  auto record = [&](int type) -> const OrderGraph::Record& {
    return g.records[static_cast<size_t>(type >= 0 && type < static_cast<int>(g.records.size()) ? type : 0)];
  };
  std::map<std::string, int> textureOf;
  auto texture = [&](const std::string& path) {
    if (path.empty()) return -1;
    auto [it, fresh] = textureOf.try_emplace(path, static_cast<int>(out->textures.size()));
    if (fresh) out->textures.push_back(path);
    return it->second;
  };
  for (const OrderGraph::Leg& leg : g.legs) {
    const OrderGraph::Node& a = g.nodes[static_cast<size_t>(leg.from)];
    const OrderGraph::Node& b = g.nodes[static_cast<size_t>(leg.to)];
    const OrderGraph::Record& r = record(b.type);
    if (r.lineTexture.empty()) continue;  // (0x826a35: in no texture's list, so never drawn)
    const bool hovered = rollover != 0xffffffffu && leg.lastQueue >= 0 &&
                         has(g.queues[static_cast<size_t>(leg.lastQueue)].partners, rollover);
    OrderGraphOut::LegOut o;
    o.units = leg.units, o.from = leg.from, o.to = leg.to;
    o.state = hovered ? 1 : stateOf(b);
    NodePlace(a, o.leg.a);
    NodePlace(b, o.leg.b);
    // The curve's directions: each node's tangent, or where it has none the leg's own (0x828a7c).
    float chord[3] = {o.leg.b[0] - o.leg.a[0], o.leg.b[1] - o.leg.a[1], o.leg.b[2] - o.leg.a[2]};
    const float length = std::sqrt(chord[0] * chord[0] + chord[1] * chord[1] + chord[2] * chord[2]);
    for (float& v : chord) v = length > 0 ? v / length : 0.0f;
    const bool aBends = a.tangent[0] != 0 || a.tangent[1] != 0 || a.tangent[2] != 0;
    const bool bBends = b.tangent[0] != 0 || b.tangent[1] != 0 || b.tangent[2] != 0;
    for (int i = 0; i < 3; ++i) o.leg.ta[i] = aBends ? a.tangent[i] : chord[i], o.leg.tb[i] = bBends ? b.tangent[i] : chord[i];
    o.leg.baseWidth = WaypointLineWidth(L, &g, leg.units);
    o.leg.color = r.lineColor[slot(o.state)];
    o.leg.texture = texture(r.lineTexture);
    o.leg.uvAspect = r.uvAspect, o.leg.animRate = r.animRate;
    out->legs.push_back(o);
  }
  auto rank = [](int state) { return state == 1 ? 0 : state == 2 ? 1 : 2; };
  std::stable_sort(out->legs.begin(), out->legs.end(),
                   [&](const OrderGraphOut::LegOut& x, const OrderGraphOut::LegOut& y) { return rank(x.state) < rank(y.state); });
  for (size_t i = 0; i < g.nodes.size(); ++i) {
    const OrderGraph::Node& n = g.nodes[i];
    if (n.command == 0xffffffffu) continue;
    const OrderGraph::Record& r = record(n.type);
    OrderGraphOut::MarkOut m;
    m.state = stateOf(n), m.node = static_cast<int>(i);
    NodePlace(n, m.mark.position);
    m.mark.color = r.markColor[slot(m.state)];
    m.mark.size = n.size * r.markScale[slot(m.state)];
    if (!r.markTexture.empty()) {
      m.mark.texture = texture(r.markTexture);
    } else if (m.state == 1) {  // (0x828375: a solid square of ff30f030 for a highlighted node without one)
      m.mark.texture = -1;
      m.mark.color = ColorTimes(0xff30f030u, m.mark.color);
    } else {
      continue;
    }
    out->marks.push_back(m);
  }
  // The time left at each command node (0x828610): the beat its order will be done in less the beat
  // now, none below 0; whole seconds of it (0x82874d: times 0.1, rounded down) as minutes and seconds.
  // With it, the node's size times its state's waypoint_scale (the record's +0x34 normal, +0x38
  // selected, +0x3c highlight): where the text goes.
  // (Now: the simulation's beat, as when the times were worked out; the scene is not made while a
  // beat runs on its own thread.)
  const int64_t now = TheSim(L)->tick();
  for (size_t i = 0; i < g.nodes.size(); ++i) {
    const OrderGraph::Node& n = g.nodes[i];
    if (n.command == 0xffffffffu || n.eta - now < 0) continue;
    OrderGraphOut::EtaOut e;
    e.node = static_cast<int>(i);
    e.eta.ticks = static_cast<int>(n.eta - now);
    e.eta.state = stateOf(n);
    NodePlace(n, e.eta.position);
    e.eta.size = n.size * record(n.type).markScale[slot(e.eta.state)];
    const int seconds = static_cast<int>(std::floor(static_cast<float>(e.eta.ticks) * 0.1f));
    char text[48];
    std::snprintf(text, sizeof text, "ETA: %d.%02d", seconds / 60, seconds % 60);
    e.eta.text = text;
    out->etas.push_back(std::move(e));
  }
  // The path preview (ui_DrawPathPreview), drawn with the graph (0x82961c).
  if (preview) WorkOutPathPreview(L, g, rollover, *preview, texture, &out->preview);
}

// moho64ui_OrderGraph([unit]) -> {shown, legs = {...}, marks = {...}, etas = {...}, textures, settings}: the order lines
// as the picture is given them now (the host's own function for tests), with `unit` standing for the
// unit under the mouse (else the one there). Each leg: a, b, ta, tb ({x, y, z}), units, width (its
// base width), state ("highlight", "selected", "normal"), color ("aarrggbb"), texture (its path, or
// nil), uvAspect, animRate, from and to (the commands' names; "start" for a start node) and fromId,
// toId (the commands' ids). Each mark: position, size, state, color, texture, command, id. Each eta
// (a node with time left, before the world view's tests of ui_MaxTextLOD and display_eta): position,
// size (the node's times its state's waypoint scale), state, ticks, text ("ETA: 1.05"), command, id.
// hovered: the id of the waypoint under the mouse's order (nil: none); dragged: the id of the order
// whose waypoint is being dragged; coordinated: {order, with}, the last attack the interface sent
// coordinated with another order (a double click on its waypoint) and that order's id (nil: none yet).
// preview (nil: none drawn): the path preview in the main world view (ui_DrawPathPreview): order (the command's name
// whose record draws it), unit (the entity id it starts from), points ({x, y, z} each, as listed), width (its one
// width W), arrow (its last leg the arrowhead), pathfinding (ui_PathPreview), and legs as the order lines' (width:
// the base width handed to the renderer, which with its own two pixels' worth comes to W; the arrowhead's to 2W).
int l_OrderGraphForTests(lua_State* L) {
  uint32_t rollover = UnitUnderMouse(L);
  if (const UserUnit* u = ToUnit(L, 1)) rollover = u->view.id;
  const bool shown = KeyIsDown(L, 306);
  OrderGraphOut out;
  PathPreviewView preview;
  const bool hasView = GraphVars().drawPathPreview && MainPathPreviewView(L, &preview);
  WorkOutOrderGraph(L, shown, rollover, &out, hasView ? &preview : nullptr);
  const OrderGraph& g = Store(L).graph;
  auto vector = [&](const float v[3]) {
    lua_newtable(L);
    for (int i = 0; i < 3; ++i) {
      lua_pushnumber(L, v[i]);
      lua_rawseti(L, -2, i + 1);
    }
  };
  auto field = [&](const char* key) {  // (the value on top into the table under it)
    lua_pushstring(L, key);
    lua_insert(L, -2);
    lua_rawset(L, -3);
  };
  auto color = [&](uint32_t c) {
    char text[16];
    std::snprintf(text, sizeof text, "%08x", c);
    lua_pushstring(L, text);
  };
  const char* states[3] = {"normal", "highlight", "selected"};
  auto command = [&](int node) {
    const OrderGraph::Node& n = g.nodes[static_cast<size_t>(node)];
    const char* name = n.command == 0xffffffffu ? "start" : CommandName(static_cast<CommandType>(n.type));
    lua_pushstring(L, name ? name : "?");
  };
  auto commandId = [&](int node) {
    const OrderGraph::Node& n = g.nodes[static_cast<size_t>(node)];
    if (n.command == 0xffffffffu) lua_pushnil(L);
    else PushCommandId(L, n.command);  // (as GetCommandQueue's ID, so that tests can compare them)
  };
  lua_newtable(L);
  lua_pushboolean(L, g.made && shown);
  field("shown");
  if (g.hovered != 0xffffffffu) PushCommandId(L, g.hovered), field("hovered");
  if (g.drag.command != 0xffffffffu) PushCommandId(L, g.drag.command), field("dragged");
  if (Store(L).coordinatedOrder != 0xffffffffu) {  // (the last attack sent coordinated with another: AttackClick)
    lua_newtable(L);
    PushCommandId(L, Store(L).coordinatedOrder), field("order");
    PushCommandId(L, Store(L).coordinatedWith), field("with");
    field("coordinated");
  }
  lua_newtable(L);
  for (size_t i = 0; i < out.legs.size(); ++i) {
    const OrderGraphOut::LegOut& o = out.legs[i];
    lua_newtable(L);
    vector(o.leg.a), field("a");
    vector(o.leg.b), field("b");
    vector(o.leg.ta), field("ta");
    vector(o.leg.tb), field("tb");
    lua_pushnumber(L, o.units), field("units");
    lua_pushnumber(L, o.leg.baseWidth), field("width");
    lua_pushstring(L, states[o.state]), field("state");
    color(o.leg.color), field("color");
    if (o.leg.texture >= 0) lua_pushstring(L, out.textures[static_cast<size_t>(o.leg.texture)].c_str()), field("texture");
    lua_pushnumber(L, o.leg.uvAspect), field("uvAspect");
    lua_pushnumber(L, o.leg.animRate), field("animRate");
    command(o.from), field("from");
    command(o.to), field("to");
    commandId(o.from), field("fromId");
    commandId(o.to), field("toId");
    lua_rawseti(L, -2, static_cast<int>(i) + 1);
  }
  field("legs");
  lua_newtable(L);
  for (size_t i = 0; i < out.marks.size(); ++i) {
    const OrderGraphOut::MarkOut& m = out.marks[i];
    lua_newtable(L);
    vector(m.mark.position), field("position");
    lua_pushnumber(L, m.mark.size), field("size");
    lua_pushstring(L, states[m.state]), field("state");
    color(m.mark.color), field("color");
    if (m.mark.texture >= 0) lua_pushstring(L, out.textures[static_cast<size_t>(m.mark.texture)].c_str()), field("texture");
    command(m.node), field("command");
    commandId(m.node), field("id");
    lua_rawseti(L, -2, static_cast<int>(i) + 1);
  }
  field("marks");
  lua_newtable(L);
  for (size_t i = 0; i < out.etas.size(); ++i) {
    const OrderGraphOut::EtaOut& e = out.etas[i];
    lua_newtable(L);
    vector(e.eta.position), field("position");
    lua_pushnumber(L, e.eta.size), field("size");
    lua_pushstring(L, states[e.eta.state]), field("state");
    lua_pushnumber(L, e.eta.ticks), field("ticks");
    lua_pushstring(L, e.eta.text.c_str()), field("text");
    command(e.node), field("command");
    commandId(e.node), field("id");
    lua_rawseti(L, -2, static_cast<int>(i) + 1);
  }
  field("etas");
  lua_newtable(L);
  for (size_t i = 0; i < out.textures.size(); ++i) {
    lua_pushstring(L, out.textures[i].c_str());
    lua_rawseti(L, -2, static_cast<int>(i) + 1);
  }
  field("textures");
  lua_newtable(L);
  lua_pushnumber(L, out.settings.lineScale), field("lineScale");
  lua_pushnumber(L, out.settings.curveSmoothness), field("curveSmoothness");
  lua_pushnumber(L, out.settings.curveSegments), field("curveSegments");
  lua_pushnumber(L, out.settings.markMinPixels), field("minWaypointSize");
  lua_pushnumber(L, out.settings.markMaxPixels), field("maxWaypointSize");
  field("settings");
  const OrderGraphOut::PreviewOut& pv = out.preview;
  if (pv.shown || pv.pathfinding) {
    lua_newtable(L);
    const char* name = CommandName(static_cast<CommandType>(pv.type));
    lua_pushstring(L, name ? name : "None"), field("order");
    if (pv.unit != 0xffffffffu) lua_pushnumber(L, pv.unit), field("unit");
    lua_newtable(L);
    for (size_t i = 0; i < pv.points.size(); ++i) {
      vector(pv.points[i].data());
      lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    field("points");
    lua_pushnumber(L, pv.width), field("width");
    lua_pushnumber(L, pv.w0), field("w0");
    lua_pushnumber(L, pv.size), field("size");
    lua_pushboolean(L, pv.arrow), field("arrow");
    lua_pushboolean(L, pv.pathfinding), field("pathfinding");
    lua_newtable(L);
    for (size_t i = 0; i < pv.legs.size(); ++i) {
      const WorldOrderLeg& l = pv.legs[i];
      lua_newtable(L);
      vector(l.a), field("a");
      vector(l.b), field("b");
      vector(l.ta), field("ta");
      vector(l.tb), field("tb");
      lua_pushnumber(L, l.baseWidth), field("width");
      color(l.color), field("color");
      if (l.texture >= 0) lua_pushstring(L, out.textures[static_cast<size_t>(l.texture)].c_str()), field("texture");
      lua_pushnumber(L, l.uvAspect), field("uvAspect");
      lua_pushnumber(L, l.animRate), field("animRate");
      lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    field("legs");
    field("preview");
  }
  return 1;
}
}  // namespace

void OrderLinesForWorld(lua_State* L, bool shown, uint32_t rollover, WorldScene* scene, std::vector<OrderEta>* etas,
                        const PathPreviewView* preview) {
  OrderGraphOut out;
  WorkOutOrderGraph(L, shown, rollover, &out, preview);
  scene->orderTextures = std::move(out.textures);
  scene->orderLegs.clear();
  scene->orderMarks.clear();
  scene->orderLegs.reserve(out.preview.legs.size() + out.legs.size());
  // (the path preview's first: it keeps its pixels over the order lines, as its stencil Ref 2 NotEqual does)
  for (const WorldOrderLeg& l : out.preview.legs) scene->orderLegs.push_back(l);
  for (const OrderGraphOut::LegOut& o : out.legs) scene->orderLegs.push_back(o.leg);
  scene->orderMarks.reserve(out.marks.size());
  for (const OrderGraphOut::MarkOut& m : out.marks) scene->orderMarks.push_back(m.mark);
  scene->orderSettings = out.settings;
  if (etas) {
    etas->clear();
    for (OrderGraphOut::EtaOut& e : out.etas) etas->push_back(std::move(e.eta));
  }
}

// --- a waypoint under the mouse, and dragging it ---
//
// Read from the FA exe by a helper of the ui chat (2026-10-17; UICommandGraph.cpp, UICommandDragger, the world
// view's cursor update 0x86f520 and HandleEvent 0x8704b6):
// - Which waypoint is under the mouse (0x829800, called at 0x86ff9a by the cursor update the world view runs on
//   each of its events, 0x8705e0, and each frame the mouse is over it, 0x871140): only while the view holds the
//   command graph (its +0x20c: Shift was held when it last drew, 0x86ed00), its highlight is on (+0x2a4,
//   SetHighlightEnabled) and it is not a minimap (+0x135); else none. The graph's command nodes (its list
//   graph+0xd38; not the start nodes) that are in view (the camera's frustum planes, 0x829890) are measured on
//   the screen: d the pixels from the mouse to the node's place (0x829a1a), w the map units a pixel is there
//   (0x829a23: dot(cam+0x2a4, p) + cam+0x2b0). A node counts when d * w <= ui_CommandClickScale * r * w, r =
//   size / w held to ui_MinWaypointSize .. ui_MaxWaypointSize (its mark's half side as drawn, without the order's
//   waypoint scale); a Ferry's d * w less 0.1 (0x829aab). Of those the nearest wins, except that a node whose
//   order a selected unit carries is taken whenever it counts (0x829af4: nearer, or selected: the last such in
//   the list). The answer, the order's id, is the session's (+0x4c8; 0xffffffff none).
// - That node's state is then highlight (0x8281e0, 0x828246), so its mark and the legs leading to it are drawn
//   highlighted (a leg takes the state of the node it leads to).
// - The cursor: the game's script asks HasHighlightCommand() (0x871f40: session+0x4c8 != -1) in OnUpdateCursor and
//   shows HOVERCOMMAND, or MOVE2PATROLCOMMAND when ShowConvertToPatrolCursor() (0x872020: the view's +0x274). The
//   world view sets +0x274 on an event (0x870853) when a selected unit carries the hovered order, it is a Move or
//   FormMove, and 0x81dd00 says yes: every selected unit with orders is no POD, has more than one order, the
//   hovered one among them and not the last, all of the hovered one's type.
// - Dragging: with no command mode a left press is action 5 when a waypoint is hovered, else 4, the selection
//   (CUIActionHandler::GetLeftMouseButtonAction 0x81f8df; no focus army: action 0). Action 5 makes a UICommandDragger
//   (0x870ebb -> 0x8242b0 -> 0x823fe0; vtable 0xe42600): it holds the graph (GetCommandGraph(true), 0x824026), calls
//   /lua/ui/game/commandgraph.lua OnCommandDragBegin() (0x823e40) and is posted as the left button's dragger. Each move
//   (0x8241b0) puts the node at the ground under the mouse (0x829b40: its place is that point, count 1, and its byte
//   +0x1c is set: it no longer follows its order's target, 0x828fb0); a building's node (BuildMobile) is tested there
//   from the second move on (0x8c1880 into the byte +0x1e; the session's +0x4d5, 1 unless the console command
//   SkipUIChecks (0x897580) toggled it, makes a failed test count), and its ghost is drawn at the node, the ground
//   under the mouse, with its skirt's outline green d800d800 or red d8d80000 (0x828dd0; not dragged: at its order's
//   place, no outline). The release (0x824210) does the same once more, clears +0x1c and sends the order's new target
//   (0x829b40 with its last argument 1): the place held inside the playable rectangle less one (x0 + 1 .. x1 - 1, z0 +
//   1 .. z1 - 1); an order on an entity (0x824550) goes onto the unit nearest that place on the map plane (any
//   distance) of the army of the one it was on, none of the order's own units (none: nothing is sent; so an order on a
//   prop goes nowhere); a building (BuildMobile) goes to the site snapped for its blueprint (0x579300, 0x50b1a0), only
//   when the test passed; any other order to the place. That is the command stream's SetCommandTarget (0x8b0ee0 -> the
//   driver's slot 0x74; the stream's message 16, CDecoder::DecodeSetCommandTarget 0x749740): once for the order, which
//   all its units share; with a focus army whose byte +0x170 is clear the target's place inside the playable rectangle
//   (0x86da60), and for TransportLoadUnits only onto a FERRYBEACON, TRANSPORTATION or AIRSTAGINGPLATFORM unit
//   (0x8b1023). The interface's own copy of the order takes the new target at once (0x8b4a40). Then
//   OnCommandDragEnd(event, cmdId) (0x823f00). Cancelled (0x824290: a dragger posted over it) the node goes back to its
//   order's place (0x82a030), nothing is sent and the script is not called. A double click after the press does nothing
//   (0x870c24).
// - Taking an order out (read 2026-10-19; the world view's HandleEvent 0x8704b6, run as FAF's copy of it 0x12903b8, the
//   same there): the right button's press (or double click; the event's button +0x14 == 3) gives no order, it keeps
//   the one a release would give (GetRightMouseButtonAction 0x81ec00 into the view's +0x1a8, 0x870f25). Its release
//   (0x870ff7 / 0x1290ecc), after the cursor update has worked out the waypoint under the mouse for that event too
//   (0x8705e0) and the script's handler has left the event alone: when a waypoint is hovered (session+0x4c8, its top
//   byte not 0xff), the release's own modifiers (+0x1c) hold Shift (1) and Ctrl (2; Alt is not asked) and the session
//   is no observer (0x896590: a focus army whose slot holds an army), the order is looked up by its id in the
//   session's table (0x8b5bb0: session+0x3fc +0xcb4) and taken out of the queue of each of ITS units (0x8b43f0: the
//   order's own unit set, every unit that carries it, selected or not; 0x8b1220 per unit). Then no other order is
//   given, also when the lookup finds nothing. Else the kept order is given (0x81fcd0; none when there was none).
//   Either way the formation is reset and the event goes on (al 0). 0x8b1220 (order, unit): the command stream's
//   RemoveCommandFromQueue(order's id +4, unit's id +0x44) (the sim driver 0x10c4f50, slot 0x80 = CSimDriver
//   0x73cfd0: the client's slot 0x48 under the driver's lock; it gives the beat the message goes with, +0x24); then
//   the interface's own copy of the unit's queue (its slot 0x18, +0x3c8: the unit's own queue, not the factory's)
//   takes a change {beat, 2: out, order} (0x8b6ee0 -> 0x8b76d0; the copy is made again when next read, +0x60, 0x8b6f60:
//   GetCommandQueue 0x8c5480 reads it so), and the order's unit set a change {beat, 3, unit} (0x8b4880): the order is
//   gone from the interface at once.
// - Clicks on a hovered waypoint (read 2026-10-19; HandleEvent 0x870612 .. 0x8708af, SCommandModeData::HandleEvent
//   0x81fcd0, the same in FAF's copy 0x12903b8): see "Clicks on a hovered waypoint" below.
// Left out (READ, not built): the no-rush circle 0x8b0ee0 tests (the focus army's +0x1bc..+0x1d0: a target
// farther from its start than the radius is not sent while the timer runs; the interface's other orders do not
// test it either).
namespace {

// A command by its id, from the queues of the units the interface knows (the original's session command table,
// session+0x3fc +0xcb4), with the units that carry it.
bool FindCommandView(lua_State* L, uint32_t id, CommandView* out, std::vector<uint32_t>* carriers) {
  UnitStore& store = Store(L);
  bool found = false;
  for (const auto& entry : store.units) {
    const UserUnit* u = entry.second.get();
    if (u->gone || u->view.dead) continue;
    UnitStateView s;
    if (!StateOf(L, u, &s)) continue;
    bool carries = false;
    for (const std::vector<CommandView>* q : {&s.queue, &s.factoryQueue}) {
      for (const CommandView& c : *q) {
        if (c.id != id) continue;
        if (!found) *out = c;
        found = carries = true;
      }
    }
    if (carries && carriers) carriers->push_back(u->view.id);
  }
  return found;
}

// 0x829b40 for a move (`last` false) or the release: the node to the ground under the mouse; a building's site
// tested there when the node was already being dragged (its byte +0x1c from the move before).
void MoveDraggedNode(lua_State* L, const float at[3], bool last) {
  OrderGraph& g = Store(L).graph;
  OrderGraph::Drag& d = g.drag;
  if (!std::isfinite(at[0]) || !std::isfinite(at[1]) || !std::isfinite(at[2])) return;  // (0x5657f0)
  for (int i = 0; i < 3; ++i) d.at[i] = at[i];
  if (d.moved && d.type == static_cast<int>(CommandType::BuildMobile) && !d.blueprint.empty()) {
    // A building's order dragged on from the move before: can it stand there (USERUNIT_CanBeBuiltAt 0x8c1880 at
    // 0x829c95; here the simulation's own test, BuildSiteOf, on the site snapped there). A failed test counts
    // unless the console's SkipUIChecks has turned the session's checks off (+0x4d5, 1 from the start: 0x8934f1,
    // toggled by 0x897580); that switch is not built: the checks are always on. (The test asks the simulation: not
    // while a beat runs on its own thread, the answer from the move before then; always for the release.)
    UserState* user = UserState::From(L);
    const bool beat = user->config().beatRunning && user->config().beatRunning();
    Sim* sim = !beat || last ? TheSim(L) : nullptr;
    bool free = true;
    float site[3];
    if (sim && BuildSiteOf(*sim, d.blueprint, at, site, &free)) d.fit = free;
  }
  d.moved = !last;
  // The node itself, now (0x829c05): the legs to and from it follow the mouse.
  auto it = g.nodeOf.find(d.command);
  if (it != g.nodeOf.end() && it->second < static_cast<int>(g.nodes.size())) {
    OrderGraph::Node& n = g.nodes[static_cast<size_t>(it->second)];
    for (int i = 0; i < 3; ++i) n.sum[i] = at[i];
    n.count = 1;
  }
}

}  // namespace

uint32_t UpdateHoveredWaypoint(lua_State* L, bool active, float mx, float my, const WaypointScreen& screenOf) {
  if (!active && !UserState::From(L)->units) return 0xffffffffu;  // (a state without units: the front end's)
  OrderGraph& g = Store(L).graph;
  g.hovered = 0xffffffffu;
  if (!active) return g.hovered;  // (the graph itself is the picture's to let go)
  HoldOrderGraph(L, true);
  if (!g.made) return g.hovered;
  UnitStore& store = Store(L);
  std::vector<uint32_t> selected;
  for (const UserUnit* u : store.selection) selected.push_back(u->view.id);
  const CommandGraphVars& vars = GraphVars();
  const float clickScale = Looks().commandClickScale;   // (ui_CommandClickScale, 0xf57ce0)
  float best = std::numeric_limits<float>::infinity();  // (0x10c19e0: +inf, 0xbe3b50)
  for (const OrderGraph::Node& n : g.nodes) {
    if (n.command == 0xffffffffu) continue;
    float at[3], sx = 0, sy = 0, w = 0;
    NodePlace(n, at);
    if (!screenOf(at, &sx, &sy, &w) || !(w > 0)) continue;
    float d = std::sqrt((sx - mx) * (sx - mx) + (sy - my) * (sy - my)) * w;
    float r = n.size / w;  // (0x829a6d: held below by the maximum, then above by the minimum)
    if (!(vars.maxWaypointSize > r)) r = vars.maxWaypointSize;
    if (vars.minWaypointSize > r) r = vars.minWaypointSize;
    if (n.type == static_cast<int>(CommandType::Ferry)) d -= 0.1f;
    if (clickScale * r * w < d) continue;
    bool carriedBySelected = false;  // (0x829ad2: 0x831110, the selection against the order's units)
    for (uint32_t u : g.carriers[n.command]) carriedBySelected = carriedBySelected || std::find(selected.begin(), selected.end(), u) != selected.end();
    if (best > d || carriedBySelected) {
      best = d;
      g.hovered = n.command;
    }
  }
  return g.hovered;
}

uint32_t HoveredWaypoint(lua_State* L) {
  UserState* user = UserState::From(L);
  return user->units ? Store(L).graph.hovered : 0xffffffffu;
}

// --- clicks on a hovered waypoint ---
//
// Read from the FA exe by a helper of the ui chat (2026-10-19; the world view's HandleEvent 0x8704b6, run as FAF's copy
// 0x12903b8, the same there; CUIActionHandler's GetRightMouseButtonAction 0x81ec00 and GetLeftMouseButtonAction 0x81f7b0;
// SCommandModeData::HandleEvent 0x81fcd0):
// - The tips (0x870612 .. 0x8708af): every event the script's handler leaves to the view (its input not locked, +0x138)
//   clears +0x274 (ShowConvertToPatrolCursor) and, while the view holds the command graph (+0x20c) and a waypoint is
//   hovered whose order the session's table holds (0x8b5bb0), looks at that order:
//   - no selected unit carries it (0x8b4300 for each) and it is an Attack or FormAttack (0x8b4140: 10, 11): when the
//     right click or the left click there would give an Attack (0x81ec00 and 0x81f7b0 asked with no modifiers: their
//     action's +4, the order's RULEUCC bit, is 4), "<LOC Engine0007>Double left-click for coordinated attack" in the
//     command mode "order" (0x4207d0 on the mode's name), else "<LOC Engine0008>Double right-click ...";
//   - else a Move or FormMove (2, 4) that 0x81dd00 (MovesConvertibleAt) says yes to: "<LOC Engine0009>Left-click to
//     convert moves into patrol" in the command mode "order", else "<LOC Engine0010>Right-click ..."; and +0x274 is set.
//   Either is /lua/ui/uimain.lua StartCursorText(x, y, text, "ffffffff", -1, false) (0x83ecf0: the mouse's place on the
//   screen, session+0x4cc; the color by SCR_EncodeColor 0x4b2e60, "%08x"); every other such event StopCursorText()
//   (0x83edf0; its only caller). No option picks the button's word: the command mode does (in the mode "order" the
//   left click gives the mode's order, 0x81f8ff, and the right click ends the mode, action 7 at 0x81ed4e).
// - What gives the order: the left click in the command mode "order" (0x81f91c: the action's +4 the RULEUCC bit of the
//   mode's name), at the press (0x870edc -> 0x870310); the right click by what is under the cursor (RightClickOrder;
//   with no unit there an attack waypoint makes it an Attack, 0x81f513), at the release (0x870ff7 -> 0x8710ca). Both
//   actions keep the hovered order's id as they are made (+0x58, from the cursor info's +0x18: 0x81fbeb, 0x81f59b) and
//   the mouse's place on the screen (+0x28). A double click (event type 6): the right button's action is made again at
//   the second press and given at its release with the double flag (+0x144 == 6, 0x8710b8); the left button's is the
//   first press's, kept (+0x148), given again with the flag when it was an order (kind 1: 0x870c7a, +0x140 == 6).
// - An Attack (0x81fdad; the only branch that asks the double flag): with it, first ISSUE_RemoveLastCommand (0x8b1390
//   -> 0x8b1270: for each selected unit the last order of its own queue's copy goes, the stream's
//   RemoveCommandFromQueue(order, unit) and the copy's change, as 0x8b1220). On a unit (the cursor info's +0x10): with
//   the flag and a kept order (+0x58) that 0x81db40 says yes to (CoordinatedAttackOn), the new attack's issue data takes
//   that order's id as its second word (+4: the command stream's coordinated id) and "<LOC Engine0011>Coordinated
//   Attack!" shows (StartCursorText at +0x28, "ffff0000", 3 seconds, flashing); then ISSUE_Command (0x8b05e0). On the
//   ground (0x81ff17) neither is asked; that branch splits the selection for an aggressive move (0x81eb20), which is not
//   built: AttackClick sends the plain Attack there, as the order mode's click did before.
// - A Move (0x820661): with the kept order (+0x58) valid and 0x81dd00 saying yes (0x8207e6), no move is sent (nor the
//   factories' rally order): 0x81def0 (RestartQueuedCommandsFromHelper) turns the moves into a patrol and "<LOC
//   Engine0012>Patrol Initiated to location!" shows ("ff00ff00", 3 seconds, flashing). 0x81def0: Move becomes Patrol,
//   FormMove FormPatrol (0x81df23: 0x10 + 2 * (type != Move)); for each selected unit (living, with a queue) each order
//   of its queue's copy from the hovered one on, of the hovered one's type, gets the stream's SetCommandType(order,
//   type) (the sim driver's slot 0x78, message 17) and its copy the type at once (0x8b4ac0). "From the hovered one on"
//   is a flag set once for all the units (0x81df1e), so the units after the first that holds it have every order of
//   that type turned; as the copy changes at once, an order two units share is sent once.
// - The words (loc/US/strings_db.lua): Engine0007 "Double left-click for coordinated attack", Engine0008 "Double
//   right-click for coordinated attack", Engine0009 "Left-click to convert moves into patrol", Engine0010 "Right-click
//   to convert moves into patrol", Engine0011 "Coordinated Attack!", Engine0012 "Patrol Initiated to location!".
// - With FAF's scripts: its cursor script turns its ignore mode on in the Move and Attack order modes
//   (/lua/ui/controls/worldview.lua OnCursorMove, OnCursorAttackGround: EnableIgnoreMode, ui_CommandClickScale 0 and a
//   select tolerance that picks no unit), so there no waypoint is hovered and the Attack mode's click attacks the
//   ground: the left button's joins and conversions are met only with that mode off. In the other order modes (Patrol,
//   Guard, ...) the left button's tip shows as the original's does, and the click gives that mode's order.
// What the simulation makes of the coordinated id is the engine's (its IssueCommand reads it, CmdData; not used yet).
namespace {

// 0x81dd00 (the selection, an order): the order is a Move or FormMove, and each selected unit (living; not a unit or
// with no queue: passed over) is no POD and has more than one order, the order among them and not the last, all of the
// order's type. (An order not in the session's table: no, 0x81dd1e.)
bool MovesConvertibleAt(lua_State* L, uint32_t command) {
  UnitStore& store = Store(L);
  CommandView c;
  if (store.selection.empty() || !FindCommandView(L, command, &c, nullptr)) return false;
  const int type = c.type;
  if (type != static_cast<int>(CommandType::Move) && type != static_cast<int>(CommandType::FormMove)) return false;
  for (const UserUnit* u : store.selection) {
    if (u->gone || u->view.dead) continue;
    UnitStateView s;
    if (!StateOf(L, u, &s)) continue;
    if (InCategory(L, u, "POD")) return false;
    const std::vector<CommandView>& q = s.queue;
    if (q.size() <= 1 || q.back().id == command) return false;
    bool in = false;
    for (const CommandView& o : q) in = in || o.id == command;
    if (!in) return false;
    for (const CommandView& o : q)
      if (o.type != type) return false;
  }
  return true;
}

// 0x81db40 (the session, an order): the order is an Attack or FormAttack in the session's table and no selected unit
// (living: INFERRED for the unit's own two tests, slots +0x28 and +0x2c of its interface, not read) has it in its queue.
bool CoordinatedAttackOn(lua_State* L, uint32_t command) {
  if ((command & 0xff000000u) == 0xff000000u) return false;  // (0x81fea6)
  CommandView c;
  if (!FindCommandView(L, command, &c, nullptr)) return false;
  if (c.type != static_cast<int>(CommandType::Attack) && c.type != static_cast<int>(CommandType::FormAttack)) return false;
  for (const UserUnit* u : Store(L).selection) {
    if (u->gone || u->view.dead) continue;
    UnitStateView s;
    if (!StateOf(L, u, &s)) continue;
    for (const CommandView& o : s.queue)
      if (o.id == command) return false;
  }
  return true;
}

// ISSUE_RemoveLastCommand (0x8b1390 -> 0x8b1270): each selected unit's last order (its own queue's copy, slot 0x18;
// the last entry that holds one) is taken out: the command stream's RemoveCommandFromQueue(order, unit) (the sim
// driver's slot 0x80) and the copy drops it at once (UnitStore::removals). The copy here lags the original's by a
// beat: an order sent to the unit that its queue does not show yet is its last (UnitStore::lastSent).
void RemoveSelectionsLastOrders(lua_State* L) {
  UserState* user = UserState::From(L);
  UnitStore& store = Store(L);
  for (const UserUnit* u : store.selection) {  // (the selection's entities that are units, in its order: 0x8b13fc)
    if (u->gone) continue;
    UnitStateView s;
    if (!StateOf(L, u, &s)) continue;
    uint32_t last = s.queue.empty() ? 0xffffffffu : s.queue.back().id;
    auto sent = store.lastSent.find(u->view.id);
    if (sent != store.lastSent.end() && user->syncCount <= sent->second.sync + 10) {
      const uint32_t id = sent->second.command;
      bool shown = false, removed = false;
      for (const CommandView& c : s.queue) shown = shown || c.id == id;
      for (const UnitStore::Removal& r : store.removals) removed = removed || (r.unit == u->view.id && r.command == id);
      if (!shown && !removed) last = id;
    }
    if (last == 0xffffffffu) continue;
    Writer(L).RemoveCommandFromQueue(last, u->view.id);
    UnitStore::Removal r;
    r.unit = u->view.id, r.command = last, r.sync = user->syncCount;
    store.removals.push_back(r);
    if (sent != store.lastSent.end() && sent->second.command == last) store.lastSent.erase(sent);
    Logf(LogLevel::Debug, "moho64ui: a double click's attack: the last order %u of unit %u taken out", last, u->view.id);
  }
  store.graph.builtSync = ~0ull;
}

}  // namespace

WaypointOffer HoveredWaypointOffer(lua_State* L) {
  UserState* user = UserState::From(L);
  if (!user->units) return WaypointOffer::None;
  UnitStore& store = Store(L);
  const uint32_t hovered = store.graph.hovered;
  if ((hovered & 0xff000000u) == 0xff000000u) return WaypointOffer::None;  // (0x87062a)
  CommandView c;
  std::vector<uint32_t> carriers;
  if (!FindCommandView(L, hovered, &c, &carriers)) return WaypointOffer::None;  // (0x870651)
  bool carried = false;  // (0x8706c6: a selected unit carries it)
  for (const UserUnit* u : store.selection) carried = carried || std::find(carriers.begin(), carriers.end(), u->view.id) != carriers.end();
  if (!carried)
    return c.type == static_cast<int>(CommandType::Attack) || c.type == static_cast<int>(CommandType::FormAttack) ? WaypointOffer::JoinAttack
                                                                                                                  : WaypointOffer::None;
  return (c.type == static_cast<int>(CommandType::Move) || c.type == static_cast<int>(CommandType::FormMove)) && MovesConvertibleAt(L, hovered)
             ? WaypointOffer::ToPatrol
             : WaypointOffer::None;
}

bool AttackClick(lua_State* L, uint32_t under, const float at[3], uint32_t hovered, bool dbl, bool clear, bool ctrl, bool form,
                 float heading, float sx, float sy) {
  if (!UserState::From(L)->units) return false;
  if (dbl) RemoveSelectionsLastOrders(L);  // (0x81fdb9)
  if (under == 0xffffffffu) return IssueToSelection(L, "Attack", at[0], at[1], at[2], clear);  // (0x81ff17)
  uint32_t with = 0xffffffffu;
  if (dbl && CoordinatedAttackOn(L, hovered)) {  // (0x81fe9e .. 0x81fef1)
    with = hovered;
    StartCursorText(L, sx, sy, "<LOC Engine0011>Coordinated Attack!", 0xffff0000u, 3.0f, true);
    Logf(LogLevel::Debug, "moho64ui: a double click on the waypoint of attack %u: the attack is coordinated with it", hovered);
  }
  return IssueToSelectionAt(L, "Attack", under, clear, ctrl, form, heading, with);
}

bool ConvertMovesToPatrol(lua_State* L, uint32_t hovered, float sx, float sy) {
  UserState* user = UserState::From(L);
  if (!user->units || (hovered & 0xff000000u) == 0xff000000u) return false;  // (0x82078b)
  if (!MovesConvertibleAt(L, hovered)) return false;                        // (0x8207d9, 0x8207e6)
  UnitStore& store = Store(L);
  CommandView c;
  FindCommandView(L, hovered, &c, nullptr);
  const int from = c.type;
  const int to = static_cast<int>(from == static_cast<int>(CommandType::Move) ? CommandType::Patrol : CommandType::FormPatrol);
  // (INFERRED: the selection's units by their ids; the original goes through its selection set in that set's order,
  // which decides which unit comes first for the flag below)
  std::vector<const UserUnit*> units(store.selection.begin(), store.selection.end());
  std::sort(units.begin(), units.end(), [](const UserUnit* a, const UserUnit* b) { return a->view.id < b->view.id; });
  bool found = false;  // (0x81df1e: once, for all the units)
  int sent = 0;
  for (const UserUnit* u : units) {
    if (u->gone || u->view.dead) continue;
    UnitStateView s;
    if (!StateOf(L, u, &s)) continue;  // (the copy, with the types already changed: RetypeSentOrders)
    for (const CommandView& o : s.queue) {
      if (o.id == hovered) found = true;
      else if (!found) continue;
      if (o.type != from) continue;
      Writer(L).SetCommandType(o.id, to);
      store.retypes[o.id] = {to, user->syncCount};
      ++sent;
    }
  }
  store.graph.builtSync = ~0ull;
  StartCursorText(L, sx, sy, "<LOC Engine0012>Patrol Initiated to location!", 0xff00ff00u, 3.0f, true);
  Logf(LogLevel::Debug, "moho64ui: a click on the waypoint of move %u: the moves become a patrol (SetCommandType for %d order(s))", hovered,
       sent);
  return true;
}

// The script's function by its name in /lua/ui/uimain.lua, imported for each call (as SCR_Import and the lookup at
// 0x83ed31 .. 0x83ed56): its arguments are on the stack above `top + 1` (the traceback).
namespace {
void CallCursorText(lua_State* L, const char* fn, int top, int args) {
  lua_pushcfunction(L, ScriptTraceback);
  lua_insert(L, top + 1);
  lua_getglobal(L, "import");
  lua_pushstring(L, "/lua/ui/uimain.lua");
  bool ok = lua_pcall(L, 1, 1, top + 1) == 0 && lua_istable(L, -1);
  if (ok) {
    lua_pushstring(L, fn);
    lua_gettable(L, -2);
    lua_remove(L, -2);
    ok = lua_isfunction(L, -1);
    if (ok) {
      lua_insert(L, top + 2);
      ok = lua_pcall(L, args, 0, top + 1) == 0;
      if (!ok)
        Logf(LogLevel::Warning, "Error running '/lua/ui/uimain.lua:%s': %s", fn, lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
    }
  }
  lua_settop(L, top);
}
}  // namespace

void StartCursorText(lua_State* L, float x, float y, const char* text, uint32_t color, float time, bool flash) {
  const int top = lua_gettop(L);
  char encoded[16];
  std::snprintf(encoded, sizeof encoded, "%08x", color);  // (SCR_EncodeColor 0x4b2e60)
  lua_pushnumber(L, x);
  lua_pushnumber(L, y);
  lua_pushstring(L, text);
  lua_pushstring(L, encoded);
  lua_pushnumber(L, time);
  lua_pushboolean(L, flash);
  CallCursorText(L, "StartCursorText", top, 6);
}

void StopCursorText(lua_State* L) { CallCursorText(L, "StopCursorText", lua_gettop(L), 0); }

bool BeginWaypointDrag(lua_State* L, uint32_t command) {
  if (command == 0xffffffffu) return false;
  Sim* sim = TheSim(L);
  if (!sim || sim->focusArmy < 0) return false;  // (0x81f899: no focus army, no action 5)
  OrderGraph& g = Store(L).graph;
  g.drag = OrderGraph::Drag();
  g.drag.command = command;
  CommandView c;
  if (FindCommandView(L, command, &c, nullptr)) {
    g.drag.type = c.type, g.drag.blueprint = c.blueprint;
    if (c.hasPosition) {
      g.drag.hasFrom = true;
      for (int i = 0; i < 3; ++i) g.drag.from[i] = c.position[i];
    }
  }
  auto node = g.nodeOf.find(command);
  if (node != g.nodeOf.end() && node->second < static_cast<int>(g.nodes.size())) {
    g.drag.hasHome = true;
    NodePlace(g.nodes[static_cast<size_t>(node->second)], g.drag.home);
  }
  return true;
}

void MoveWaypointDrag(lua_State* L, const float at[3]) {
  if (Store(L).graph.drag.command != 0xffffffffu) MoveDraggedNode(L, at, false);
}

uint32_t ReleaseWaypointDrag(lua_State* L, const float at[3], const float playable[4]) {
  UnitStore& store = Store(L);
  OrderGraph& g = store.graph;
  const uint32_t command = g.drag.command;
  if (command == 0xffffffffu) return command;
  MoveDraggedNode(L, at, true);
  const bool fit = g.drag.fit;
  g.drag = OrderGraph::Drag();  // (the byte +0x1c cleared: the node follows its order again)
  g.builtSync = ~0ull;          // (worked out again: the node to its order's place, or where it was sent)
  if (!std::isfinite(at[0]) || !std::isfinite(at[1]) || !std::isfinite(at[2])) return command;  // (0x829bcd)
  CommandView c;
  std::vector<uint32_t> carriers;
  if (!FindCommandView(L, command, &c, &carriers)) return command;  // (0x829bc0: the order is gone)
  if (!fit) {  // (0x829cd7: a building where it cannot stand)
    Logf(LogLevel::Debug, "moho64ui: waypoint of order %u let go where its building cannot stand: no new target", command);
    return command;
  }
  // The place, held inside the playable rectangle less one (0x829d03 .. 0x829d7d).
  float x = at[0], z = at[2];
  if (!(playable[2] - 1 > x)) x = playable[2] - 1;
  if (playable[0] + 1 > x) x = playable[0] + 1;
  if (!(playable[3] - 1 > z)) z = playable[3] - 1;
  if (playable[1] + 1 > z) z = playable[1] + 1;
  StreamTarget t;
  float sent[3] = {x, at[1], z};
  uint32_t onto = 0xffffffffu;
  const UserUnit* ontoUnit = nullptr;
  if (c.entity != 0xffffffffu) {
    // An order on an entity: onto the nearest unit of that entity's army, none of the order's own (0x829dc0 ..
    // 0x829e94: the session's units, mask 0x100, by their place +0x84 / +0x8c). INFERRED: the units the player's
    // army knows of now (the session's table holds what the army sees); an entity that is no unit the interface
    // knows (a prop, a wreck) has no army among them, so nothing is sent (FAF's commandgraph.lua: "If the target
    // of an order is a prop, its node cannot be dragged").
    auto target = store.units.find(c.entity);
    if (target == store.units.end() || target->second->gone) return command;
    const int army = target->second->view.army;
    float nearest = std::numeric_limits<float>::infinity();
    for (const auto& entry : store.units) {
      const UserUnit* u = entry.second.get();
      if (u->gone || u->view.dead || u->view.army != army) continue;
      if (std::find(carriers.begin(), carriers.end(), u->view.id) != carriers.end()) continue;  // (0x8b4300)
      if (ReconBitsOf(L, u) == 0) continue;
      const float dx = x - u->view.position[0], dz = z - u->view.position[2];
      const float d = std::sqrt(dx * dx + dz * dz);
      if (nearest > d) nearest = d, ontoUnit = u;
    }
    if (!ontoUnit) return command;
    onto = ontoUnit->view.id;
    t.type = 1;
    t.entity = onto;
    for (int i = 0; i < 3; ++i) sent[i] = ontoUnit->view.position[i];
  } else if (c.type == static_cast<int>(CommandType::BuildMobile) && !c.blueprint.empty()) {
    // A building: its site snapped for its blueprint (0x829f50 / 0x829f5e; INFERRED the simulation's own
    // snapping, BuildSiteOf, is the same).
    Sim* sim = TheSim(L);
    bool free = true;
    float site[3];
    if (!sim || !BuildSiteOf(*sim, c.blueprint, sent, site, &free)) return command;
    for (int i = 0; i < 3; ++i) sent[i] = site[i];
    t.type = 2;
    t.pos = {site[0], site[1], site[2]};
  } else {
    t.type = 2;
    t.pos = {sent[0], sent[1], sent[2]};
  }
  // CWldSession's SetTarget (0x8b0ee0): with a focus army the target's place inside the playable rectangle
  // (0x86da60, by whole map units; skipped when the army's byte +0x170 is set, not read what that is); without one
  // no test (the press needed one, 0x81f899); a transport's load only onto a transport.
  Sim* sim = TheSim(L);
  if (!sim) return command;
  const int cx = static_cast<int>(sent[0]), cz = static_cast<int>(sent[2]);
  if (sim->focusArmy >= 0 &&
      (cx < static_cast<int>(playable[0]) || cx >= static_cast<int>(playable[2]) || cz < static_cast<int>(playable[1]) || cz >= static_cast<int>(playable[3])))
    return command;
  if (c.type == static_cast<int>(CommandType::TransportLoadUnits) &&
      (!ontoUnit || !(InCategory(L, ontoUnit, "FERRYBEACON") || InCategory(L, ontoUnit, "TRANSPORTATION") || InCategory(L, ontoUnit, "AIRSTAGINGPLATFORM"))))
    return command;
  Writer(L).SetCommandTarget(command, t);
  OrderGraph::Retarget r;
  for (int i = 0; i < 3; ++i) r.at[i] = sent[i];
  r.entity = onto;
  r.sync = UserState::From(L)->syncCount;
  g.retargets[command] = r;
  Logf(LogLevel::Debug, "moho64ui: waypoint of order %u dragged: SetCommandTarget %s %.1f %.1f", command, onto != 0xffffffffu ? "onto a unit at" : "to",
       sent[0], sent[2]);
  return command;
}

void CancelWaypointDrag(lua_State* L) {
  OrderGraph& g = Store(L).graph;
  if (g.drag.command == 0xffffffffu) return;
  // (0x82a030: the node back to its order's place at once, count 1, its byte +0x1c cleared; here where it stood when
  // the drag began, until the graph is worked out again, as it is at once unless a beat runs on its own thread)
  auto node = g.nodeOf.find(g.drag.command);
  if (g.drag.hasHome && node != g.nodeOf.end() && node->second < static_cast<int>(g.nodes.size())) {
    OrderGraph::Node& n = g.nodes[static_cast<size_t>(node->second)];
    for (int i = 0; i < 3; ++i) n.sum[i] = g.drag.home[i];
    n.count = 1;
  }
  g.drag = OrderGraph::Drag();
  g.builtSync = ~0ull;
}

bool RemoveHoveredWaypointOrder(lua_State* L, bool shift, bool ctrl) {
  UserState* user = UserState::From(L);
  if (!user->units) return false;
  UnitStore& store = Store(L);
  const uint32_t command = store.graph.hovered;
  if ((command & 0xff000000u) == 0xff000000u || !shift || !ctrl) return false;  // (0x87100a .. 0x871027)
  Sim* sim = TheSim(L);
  if (!sim || sim->focusArmy < 0) return false;  // (an observer, 0x896590: the kept order instead)
  // (0x871048: the order and its units; not found: nothing, and no other order either)
  CommandView c;
  std::vector<uint32_t> carriers;
  if (!FindCommandView(L, command, &c, &carriers)) return true;
  // One message per unit (0x871073 .. 0x87109c). INFERRED: in the order of the units' ids (the original goes through
  // the order's unit set in its own order; each message touches one unit's queue only).
  for (uint32_t unit : carriers) {
    Writer(L).RemoveCommandFromQueue(command, unit);
    UnitStore::Removal r;
    r.unit = unit, r.command = command, r.sync = user->syncCount;
    store.removals.push_back(r);
  }
  store.graph.builtSync = ~0ull;  // (worked out again without it. INFERRED: at once; what marks the original's graph to
                                  // be worked out again is not read)
  Logf(LogLevel::Debug, "moho64ui: Shift+Ctrl right click on the waypoint of order %u: RemoveCommandFromQueue for %zu unit(s)", command,
       carriers.size());
  return true;
}

bool DraggedBuildingSite(lua_State* L, WorldGhost* site, float from[3]) {
  UserState* user = UserState::From(L);
  if (!user->units) return false;
  const OrderGraph::Drag& d = Store(L).graph.drag;
  if (d.command == 0xffffffffu || !d.moved || d.type != static_cast<int>(CommandType::BuildMobile) || d.blueprint.empty()) return false;
  site->blueprint = d.blueprint;
  for (int i = 0; i < 3; ++i) site->position[i] = d.at[i], from[i] = d.hasFrom ? d.from[i] : NAN;
  site->free = d.fit;
  return true;
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
  // Units that are gone leave the selection without a word, then the store: the original's selection is a set of weak
  // references, and an entity the beat's sync destroys is orphaned (CWldSession::DoBeat 0x894c00 .. 0x894c84) and,
  // where the player sees it, deleted that beat (UserEntity::OrphanUpdate 0x8b9670 from 0x895635); its empty entry is
  // dropped as the set is next walked (0x66a330 -> 0x66a550), so neither the old selection nor `removed` names it.
  // The script hears of the smaller selection after OnSync (CheckForNecessaryUIRefresh); a dead unit leaves there too.
  // (Not built: the original keeps the orphan of a unit the player does not see, an enemy's under the fog, and with it
  // its place in the selection, until the player sees the place, 0x8b9694 .. 0x8b96a7; ours drops it at once.)
  store.selection.erase(std::remove_if(store.selection.begin(), store.selection.end(), [](const UserUnit* u) { return u->gone; }),
                        store.selection.end());
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

// CWldSession::CheckForNecessaryUIRefresh (FA exe 0x8943e0, READ 2026-10-21), once a beat, after the beat's OnSync()
// (CWldSession::DoBeat runs OnSync at 0x8950a3, this at 0x89570a, near its end). Over the selection: a unit whose copy
// of the entity's synced data asks for a refresh (UserEntity::RequiresUIRefresh, vt+0x2c 0x8b8530: byte +0x72, the
// copy at +0x50 (UpdateEntityData 0x8b9038) of SSTIEntityVariableData +0x22, the simulation's entity+0x9a, set by
// Entity:RequestRefreshUI 0x68bb60 and Entity::Kill 0x679ad8: UnitView::refreshUI) makes it refresh (0x89446e ..
// 0x89447b); the units not dead (var data +0x21, entity+0x99, which Entity::Kill sets at 0x679ade: UnitView::dead;
// 0x89447d) make the new selection. It is set (CWldSession::SetSelection, its script told even when nothing changes)
// when one asked, when the selection is smaller than SetSelection left it (a unit gone: RefreshUserUnits), or when a
// dead unit is left out (0x8944bc .. 0x8944e7). So the panels are made again (gamemain.OnSelectionChanged with the old
// selection as the new one: FAF's isOldSelection) once in the beat whose sync carries the request, and only for a
// selected unit (a sweep of the user side, 0x7e0000 .. 0x8d0000, finds no other read of +0x72 than 0x8b8530; the
// other vt+0x2c calls looked at are on other classes or on UserUnit's +0x148 part).
void CheckForNecessaryUIRefresh(lua_State* L) {
  UserState* user = UserState::From(L);
  if (!user->units) return;
  UnitStore& store = Store(L);
  bool refresh = false;
  std::vector<UserUnit*> kept;
  for (UserUnit* u : store.selection) {
    if (u->view.refreshUI) refresh = true;
    if (!u->view.dead) kept.push_back(u);
  }
  if (refresh || store.selection.size() < store.selectionSetSize || kept.size() < store.selection.size())
    SetSelection(L, kept, true);
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
  lua_register(L, "moho64ui_OrderGraph", l_OrderGraphForTests);
  // moho64ui_SentMessages() -> the command stream's messages handed to the simulation since the last call (in a test
  // run only: UnitStore::sentForTests), each {type = n, payload = its bytes as a string}, oldest first. The host's
  // own function for tests.
  lua_register(L, "moho64ui_SentMessages", [](lua_State* S) -> int {
    UnitStore& store = Store(S);
    lua_newtable(S);
    int n = 0;
    for (const auto& [type, payload] : store.sentForTests) {
      lua_newtable(S);
      lua_pushstring(S, "type");
      lua_pushnumber(S, type);
      lua_rawset(S, -3);
      lua_pushstring(S, "payload");
      lua_pushlstring(S, payload.data(), payload.size());
      lua_rawset(S, -3);
      lua_rawseti(S, -2, ++n);
    }
    store.sentForTests.clear();
    return 1;
  });
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
  SetGlobal(L, "ExecLuaInSim", l_ExecLuaInSim);
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
  SetMethod(L, cls, "GetStat", l_GetStatNow);
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
