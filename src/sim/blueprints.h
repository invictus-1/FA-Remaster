// Blueprints in the sim: the rules state registers them (Register*Blueprint), the sim state gets
// a copy as `__blueprints` (keyed by ordinal and by id, as the original), and entity categories
// (`categories.TECH1`, ...) are sets over the entity blueprints.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "script/lua.hpp"

namespace moho {

class ScriptState;

enum class BpKind { Unit, Projectile, Prop, Mesh, Emitter, TrailEmitter, Beam, Other };

// A named footprint (SpecFootprints in /lua/footprints.lua), also the engine's SFootprint.
struct NamedFootprint {
  std::string name;
  uint8_t sizeX = 0, sizeZ = 0, caps = 0, flags = 0;
  float maxSlope = 0, minWaterDepth = 0, maxWaterDepth = 0;
};

struct BlueprintInfo {
  BpKind kind;
  std::string id;      // BlueprintId as registered (units: lower case id)
  int ordinal = 0;     // 0-based registration order over all blueprint kinds kept in the sim
  int entityIndex = -1;  // bit index in categories (units, props, projectiles)
  int ref = LUA_NOREF;   // the blueprint table in the sim state
  // Units: the footprint the engine uses (the nearest named footprint; set by DeriveBlueprint).
  mutable bool hasFootprint = false;
  mutable NamedFootprint footprint;
};


class SimBlueprints {
 public:
  // false: keep the script tables as they are (to regenerate defaults with tools/gen_bp_defaults.py)
  static inline bool reflect = true;

  // In the rules state: keep every registered blueprint (call before running RuleInit).
  static void StartRecording(ScriptState& rules);

  // Copy the recorded blueprints from the rules state into the sim state (__blueprints, categories).
  void CopyToSim(lua_State* rules, lua_State* sim);

  const BlueprintInfo* Find(const std::string& id) const;
  const BlueprintInfo* ByEntityIndex(int i) const { return entities_[i]; }
  int EntityCount() const { return static_cast<int>(entities_.size()); }
  void PushTable(lua_State* L, const BlueprintInfo& bp) const { lua_rawgeti(L, LUA_REGISTRYINDEX, bp.ref); }
  size_t Count() const { return all_.size(); }

  // Category name -> member entity indices (as listed in the blueprints' Categories).
  const std::map<std::string, std::vector<int>>& Categories() const { return categories_; }

  const std::vector<NamedFootprint>& Footprints() const { return footprints_; }
  // Whether a game file exists (the default mesh and texture names depend on it).
  std::function<bool(const std::string&)> fileExists;

  static SimBlueprints* From(lua_State* L);

 private:
  std::vector<NamedFootprint> footprints_;
  void ReadFootprints(lua_State* rules, int list);
  std::vector<BlueprintInfo> all_;
  std::vector<const BlueprintInfo*> entities_;
  std::unordered_map<std::string, size_t> byId_;  // lower-case id -> index in all_
  std::map<std::string, std::vector<int>> categories_;
};

void RegisterCategoryBindings(lua_State* L, SimBlueprints* bps);
// The sim's view of a blueprint (engine defaults, sounds, bit sets...): see sim/bp_reflect.cpp.
void ReflectBlueprint(lua_State* L, int t, const BlueprintInfo& bp, const SimBlueprints& bps);
// Fields the engine derives (footprints, inertia, motion, air speeds, mesh names): sim/bp_derived.cpp.
void DeriveBlueprint(lua_State* L, int t, const BlueprintInfo& bp, const SimBlueprints& bps);

// Copy the engine fields of `from` (a reflected blueprint table) that `to` lacks.
void CarryOverEngineFields(lua_State* L, int to, int from, BpKind kind);

// A sound object from the sound parameter table at t ({ Bank, Cue, LodCutoff }).
void PushSound(lua_State* L, int t);

// Entity category value: push a new category set (all bits clear) and get its words.
uint64_t* PushCategory(lua_State* L);
const uint64_t* ToCategory(lua_State* L, int idx);
bool CategoryHas(const uint64_t* bits, int entityIndex);

}  // namespace moho
