// The simulation: its Lua state, armies and their brains, the world, and the tick loop.
//
// Start-up follows the original (see lua/simInit.lua's header): blueprints are copied in,
// simInit.lua runs, ScenarioInfo is filled from the session, SetupSession() runs, the armies
// and their brains are created (OnCreateArmyBrain), then BeginSession().
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "sim/blueprints.h"
#include "sim/entity.h"
#include "sim/replay.h"

namespace moho {

class Vfs;
class ScriptState;
class ThreadScheduler;
class AiBrain;
class TerrainMap;
class Unit;
class Prop;
class Platoon;

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
  // Economy (TODO(M4): production/consumption; until then stored values are what scripts set)
  float massStored = 0, energyStored = 0, massMax = 0, energyMax = 0;
  std::map<std::string, float> stats;  // army statistics (GetArmyStat)
  float unitCap = 1000;
  bool ignoreUnitCap = false;
  std::vector<int> alliance;  // per army index: 0 enemy, 1 neutral, 2 ally
};

class AiBrain : public ScriptObject {
 public:
  Army* army = nullptr;
};

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
  uint32_t tick() const { return tick_; }
  Army* GetArmy(lua_State* L, int idx);  // army by 1-based index or name
  const std::vector<std::unique_ptr<Army>>& armies() const { return armies_; }
  const SimBlueprints& blueprints() const { return bps_; }
  const TerrainMap* map() const { return map_.get(); }
  float Random();  // [0, 1)

  // Entities. Creation runs the scripts the way the original does (see sim/entities.cpp).
  Unit* CreateUnit(lua_State* L, const BlueprintInfo& bp, Army* army, Vec3 pos, Quat q, bool complete);
  class Projectile* CreateProjectile(lua_State* L, const BlueprintInfo& bp, Entity* launcher, Vec3 pos, Vec3 dir);
  Prop* CreateProp(lua_State* L, const BlueprintInfo& bp, Vec3 pos, Quat q, Vec3 scale);
  // _c_CreateEntity / _c_CreateShield: bind a script-made table to a new engine entity.
  Entity* AdoptScriptEntity(lua_State* L, int tableIdx, int specIdx, bool shield);
  Platoon* CreatePlatoon(lua_State* L, Army* army, const std::string& name, const std::string& plan);
  Entity* FindEntity(uint32_t id) const;
  void QueueDestroy(Entity* e);  // Entity:Destroy(): OnDestroy and removal happen at the end of the tick
  const std::map<uint32_t, Entity*>& entities() const { return entities_; }
  // Call obj:method(args...) for the nargs values on L's stack; logs script errors.
  // (Every function taking a lua_State works on the caller's state: it may be a thread.)
  bool CallMethod(lua_State* L, ScriptObject* obj, const char* method, int nargs);
  int focusArmy = -1;  // 0-based, -1 = observer/replay
  // The session's mod list as the lobby sent it (a replay's serialized mods): becomes
  // __active_mods in the rules and sim states. Empty: built from the mod uids instead.
  std::string sessionMods;
  bool cheats = false;

  static Sim* From(lua_State* L);

 private:
  Vfs* vfs_;
  std::vector<std::string> hookDirs_;
  std::vector<std::string> modUids_;
  std::unique_ptr<ScriptState> rules_;
  std::unique_ptr<ScriptState> state_;
  std::unique_ptr<ThreadScheduler> threads_;
  std::unique_ptr<TerrainMap> map_;
  SimBlueprints bps_;
  std::vector<std::unique_ptr<Army>> armies_;
  std::mt19937 rng_;
  uint32_t tick_ = 0;
  std::vector<std::unique_ptr<ScriptObject>> owned_;
  std::map<uint32_t, Entity*> entities_;
  uint32_t propSerial_ = 0;
  std::vector<Entity*> destroyQueue_;
  void ProcessDestroyQueue();

  // Push the script class for a blueprint: bp.ScriptModule/ScriptClass, else
  // <dir>/<name>_script.lua's TypeClass, else the default module's class.
  void PushScriptClass(lua_State* L, const BlueprintInfo& bp, const char* defModule, const char* defClass);
  bool PushImport(lua_State* L, const std::string& module);  // import(module) -> table, or false (logged)
  void InitializeArmor(lua_State* L, Unit* u);
  bool armorLoaded_ = false;
  std::map<std::string, std::map<std::string, float>> armorTypes_;  // armour type -> damage type -> multiplier

  bool CallGlobal(const char* fn, int nargs);  // args pushed; logs errors
  void SetModsGlobal(ScriptState& st);
  bool CreateArmies(const ReplayHeader& replay);
  void CreateMapProps();
};

void RegisterSimBindings(lua_State* L);

}  // namespace moho
