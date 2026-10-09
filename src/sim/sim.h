// The simulation: its Lua state, armies and their brains, the world, and the tick loop.
//
// Start-up follows the original (see lua/simInit.lua's header): blueprints are copied in,
// simInit.lua runs, ScenarioInfo is filled from the session, SetupSession() runs, the armies
// and their brains are created (OnCreateArmyBrain), then BeginSession().
#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "sim/blueprints.h"
#include "sim/economy.h"
#include "sim/entity.h"
#include "sim/replay.h"
#include "sim/skeleton.h"
#include "sim/units.h"

namespace moho {

class Vfs;
class ScriptState;
class ThreadScheduler;
class AiBrain;
class TerrainMap;
class Unit;
class Prop;
class Platoon;
class Navigation;
struct UnitCommand;

class Army {
 public:
  int index = 0;  // 1-based, as Lua sees it
  std::string name;      // "ARMY_1"
  std::string nickname;  // player name
  int faction = 0;       // 0-based faction index (UEF = 0)
  bool civilian = false;
  bool human = false;
  bool outOfGame = false;
  std::string plans;
  AiBrain* brain = nullptr;
  Platoon* pool = nullptr;      // the "ArmyPool" platoon every army has
  float startX = 0, startZ = 0;
  bool hasStart = false;
  uint32_t serial = 0;          // entity ids of this army
  ArmyEconomy econ;  // sim/economy.cpp
  std::vector<uint64_t> buildRestricted;  // AddBuildRestriction(army, category)
  std::map<std::string, float> stats;  // army statistics (GetArmyStat)
  float unitCap = 1000;
  float unitCost = 0;  // sum of the live units' General.CapCost (GetArmyUnitCostTotal)
  bool ignoreUnitCap = false;
  std::vector<int> alliance;  // per army index: 0 enemy, 1 neutral, 2 ally
  std::vector<Unit*> units;   // the army's live units, by entity id
};

// A resource deposit (CreateResourceDeposit): cells [x0, x1) x [z0, z1); type 1 mass, 2 hydrocarbon.
struct ResourceDeposit {
  int x0 = 0, z0 = 0, x1 = 0, z1 = 0, type = 0;
};

class AiBrain : public ScriptObject {
 public:
  AiBrain() { typeBits |= kTypeBrain; }
  Army* army = nullptr;
};

template <> struct ScriptTypeOf<AiBrain> { static constexpr uint32_t bit = kTypeBrain; };

class Sim {
 public:
  Sim(Vfs* vfs, std::vector<std::string> hookDirs, std::vector<std::string> modUids);
  ~Sim();

  // Rules state: run the blueprint loader and keep the blueprints for the sim.
  bool LoadRules();
  // Start the session described by a replay header (no commands are played yet).
  bool Start(const ReplayHeader& replay);
  // Advance one tick (runs the script threads that are due).
  void Tick();
  // Run Lua source in the sim state (debugging aid: --sim-lua).
  bool RunString(const std::string& code, const std::string& chunkName);

  lua_State* L() const;
  Vfs* vfs() const { return vfs_; }
  uint32_t tick() const { return tick_; }
  Army* GetArmy(lua_State* L, int idx);  // army by 1-based index or name
  const std::vector<std::unique_ptr<Army>>& armies() const { return armies_; }
  const SimBlueprints& blueprints() const { return bps_; }
  const TerrainMap* map() const { return map_.get(); }
  float Random();  // [0, 1)

  // Entities. Creation runs the scripts the way the original does (see sim/entities.cpp).
  Unit* CreateUnit(lua_State* L, const BlueprintInfo& bp, Army* army, Vec3 pos, Quat q, bool complete,
                   Unit* builder = nullptr);
  // PROJ_Create (sim/projectile.cpp): runs OnPreCreate, OnLayerChange and OnCreate; nullptr when
  // it destroyed itself (a homing projectile without a target).
  class Projectile* CreateProjectile(lua_State* L, const BlueprintInfo& bp, Army* army, Entity* launcher, Vec3 pos,
                                     Quat q, float damage, float damageRadius, const std::string& damageType,
                                     const AiTarget& target, bool ignoresAlly);
  std::vector<class Projectile*> projectiles;  // live projectiles, in creation order
  std::vector<class ShieldEntity*> shields;    // shield entities (area damage pre-pass, collisions)
  std::vector<class CollisionBeam*> beams;
  std::vector<Prop*> props;
  template <class F>
  void ForPropsInRect(float x0, float z0, float x1, float z1, F&& f) {
    if (propGridDirty_) RebuildPropGrid();
    if (x1 < 0 || z1 < 0) return;
    int cx0 = std::max(0, static_cast<int>(x0) >> 4), cz0 = std::max(0, static_cast<int>(z0) >> 4);
    int cx1 = std::min(gridW_ - 1, static_cast<int>(x1) >> 4), cz1 = std::min(gridH_ - 1, static_cast<int>(z1) >> 4);
    for (int cz = cz0; cz <= cz1; ++cz)
      for (int cx = cx0; cx <= cx1; ++cx)
        for (Prop* p : propGrid_[static_cast<size_t>(cz) * gridW_ + cx]) f(p);
  }
  void MarkPropsMoved() { propGridDirty_ = true; }
  // Entity id for a new projectile / shield / beam of an army (separate id families).
  uint32_t ReserveId(Army* army, uint32_t family);
  // Register an engine-created entity (collision beams): owned by the sim, found by id.
  void AddEntity(std::unique_ptr<Entity> e) {
    entities_[e->id] = e.get();
    owned_.push_back(std::move(e));
  }
  // The unit's armour multiplier for a damage type (armordefinition.lua, AlterArmor): 1 if none.
  float ArmorMult(const Unit* u, const std::string& damageType) const;
  Prop* CreateProp(lua_State* L, const BlueprintInfo& bp, Vec3 pos, Quat q, Vec3 scale);
  // _c_CreateEntity / _c_CreateShield: bind a script-made table to a new engine entity.
  Entity* AdoptScriptEntity(lua_State* L, int tableIdx, int specIdx, bool shield);
  Platoon* CreatePlatoon(lua_State* L, Army* army, const std::string& name, const std::string& plan);
  Entity* FindEntity(uint32_t id) const;
  void QueueDestroy(Entity* e);  // Entity:Destroy(): OnDestroy and removal happen at the end of the tick
  const std::map<uint32_t, Entity*>& entities() const { return entities_; }
  // Live units of all armies, by entity id (kept up to date on creation and destruction).
  const std::vector<Unit*>& units() const { return units_; }
  // Units within [x0,x1] x [z0,z1] by position (a 16-unit grid rebuilt when units moved, were
  // created or destroyed); calls f(unit) in id order within each cell row-major - callers that
  // need id order sort.
  template <class F>
  void ForUnitsInRect(float x0, float z0, float x1, float z1, F&& f) {
    if (gridDirty_) RebuildUnitGrid();
    int cx0 = std::max(0, static_cast<int>(x0) >> 4), cz0 = std::max(0, static_cast<int>(z0) >> 4);
    int cx1 = std::min(gridW_ - 1, static_cast<int>(x1) >> 4), cz1 = std::min(gridH_ - 1, static_cast<int>(z1) >> 4);
    if (x1 < 0 || z1 < 0) return;
    for (int cz = cz0; cz <= cz1; ++cz)
      for (int cx = cx0; cx <= cx1; ++cx)
        for (Unit* u : grid_[static_cast<size_t>(cz) * gridW_ + cx]) {
          const Vec3& p = u->position;
          if (p.x >= x0 && p.x <= x1 && p.z >= z0 && p.z <= z1) f(u);
        }
  }
  void MarkUnitsMoved() { gridDirty_ = true; }
  // Call obj:method(args...) for the nargs values on L's stack; logs script errors.
  // (Every function taking a lua_State works on the caller's state: it may be a thread.)
  bool CallMethod(lua_State* L, ScriptObject* obj, const char* method, int nargs);
  int focusArmy = -1;  // 0-based, -1 = observer/replay
  // The session's mod list as the lobby sent it (a replay's serialized mods): becomes
  // __active_mods in the rules and sim states. Empty: built from the mod uids instead.
  std::string sessionMods;
  bool cheats = false;

  static Sim* From(lua_State* L);

  // Movement (M3): pathfinding, path searches waiting their turn, issued commands.
  Navigation& navigation();
  std::deque<Unit*> pathQueue;
  std::vector<ResourceDeposit> deposits;
  std::map<uint32_t, std::weak_ptr<UnitCommand>> commandsById;
  uint32_t nextCommandId = 1;
  // Keep an engine object alive for the session (objects scripts may still hold).
  ScriptObject* Own(std::unique_ptr<ScriptObject> o) {
    owned_.push_back(std::move(o));
    return owned_.back().get();
  }

 private:
  Vfs* vfs_;
  std::vector<std::string> hookDirs_;
  std::vector<std::string> modUids_;
  std::unique_ptr<ScriptState> rules_;
  std::unique_ptr<ScriptState> state_;
  std::unique_ptr<ThreadScheduler> threads_;
  std::unique_ptr<TerrainMap> map_;
  std::unique_ptr<Navigation> nav_;
  SimBlueprints bps_;
  std::vector<std::unique_ptr<Army>> armies_;
  std::mt19937 rng_;
  uint32_t tick_ = 0;
  std::vector<std::unique_ptr<ScriptObject>> owned_;
  std::map<uint32_t, Entity*> entities_;
  std::vector<Unit*> units_;
  std::vector<std::vector<Unit*>> grid_;
  int gridW_ = 0, gridH_ = 0;
  bool gridDirty_ = true;
  void RebuildUnitGrid();
  std::vector<std::vector<Prop*>> propGrid_;
  bool propGridDirty_ = true;
  void RebuildPropGrid();
  std::map<uint32_t, uint32_t> familySerial_;  // (family << 8 | army) -> next serial
  void AddUnitToLists(Unit* u);
  void RemoveUnitFromLists(Unit* u);
  uint32_t propSerial_ = 0;
  std::vector<Entity*> destroyQueue_;
  void ProcessDestroyQueue();

  // Push the script class for a blueprint: bp.ScriptModule/ScriptClass, else
  // <dir>/<name>_script.lua's TypeClass, else the default module's class.
  void PushScriptClass(lua_State* L, const BlueprintInfo& bp, const char* defModule, const char* defClass);
  bool PushImport(lua_State* L, const std::string& module);  // import(module) -> table, or false (logged)
  void InitializeArmor(lua_State* L, Unit* u);
  // The entity's skeleton: Display.MeshBlueprint -> that mesh blueprint's first LOD MeshName.
  void AttachSkeleton(lua_State* L, Entity* e);
  std::unique_ptr<SkeletonCache> skeletons_;
  bool armorLoaded_ = false;
  std::map<std::string, std::map<std::string, float>> armorTypes_;  // armour type -> damage type -> multiplier

  bool CallGlobal(const char* fn, int nargs);  // args pushed; logs errors
  void SetModsGlobal(ScriptState& st);
  bool CreateArmies(const ReplayHeader& replay);
  void CreateMapProps();
};

void RegisterSimBindings(lua_State* L);

}  // namespace moho
