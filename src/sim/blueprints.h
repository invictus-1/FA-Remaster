// Blueprints in the sim: the rules state registers them (Register*Blueprint), the sim state gets
// a copy as `__blueprints` (keyed by ordinal and by id, as the original), and entity categories
// (`categories.TECH1`, ...) are sets over the entity blueprints.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
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
  mutable NamedFootprint altFootprint;  // the AltFootprint likewise (bp+0x300; the main one when there is none)
  // Units: General.CommandCaps / ToggleCaps as masks, read once by CopyToSim, for views read outside the beat
  // (UnitView, UnitStateView: no sim Lua there). The sim's own reads go through its Lua as before.
  uint32_t commandCaps = 0, toggleCaps = 0;
  // Props: in RECLAIMABLE, and SizeX / SizeZ (1 when missing), read once by CopyToSim, for the same views.
  bool reclaimable = false;
  float sizeX = 1, sizeZ = 1;
};


// An effect blueprint (Emitter, TrailEmitter, Beam). The engine keeps these in the rules (RRuleGameRules::
// GetEmitterBlueprint 0x52aef0, GetTrailBlueprint 0x52b090, GetBeamBlueprint 0x52afc0), not in the sim's
// __blueprints; the effect manager reads only their Lifetime (sim/effects.cpp). Without the field, the blueprint
// ctors' defaults: emitter 0 (REmitterBlueprint +0x23c, 0x50ea7e), trail 0 (RTrailBlueprint +0x28, 0x50edae), beam 1
// (RBeamBlueprint +0x2c, 0x50ef1a).
struct EffectBlueprint {
  BpKind kind;
  float lifetime;
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
  const BlueprintInfo* Find(const char* id) const;
  // An effect blueprint by its id (lower case, as Find), or nullptr.
  const EffectBlueprint* FindEffect(const std::string& id) const;
  const BlueprintInfo* ByEntityIndex(int i) const { return entities_[i]; }
  int EntityCount() const { return static_cast<int>(entities_.size()); }
  // 64-bit words in a category set over these blueprints (set by CopyToSim; multicore step 1: it was process-wide)
  int CategoryWords() const { return words_; }
  // The sound objects CopyToSim made from these blueprints, by handle (sim/bp_reflect.cpp; multicore step 1: the
  // list was process-wide)
  std::shared_ptr<struct SoundState> sounds;
  void PushTable(lua_State* L, const BlueprintInfo& bp) const { lua_rawgeti(L, LUA_REGISTRYINDEX, bp.ref); }
  size_t Count() const { return all_.size(); }

  // Category name -> member entity indices (as listed in the blueprints' Categories).
  const std::map<std::string, std::vector<int>>& Categories() const { return categories_; }

  const std::vector<NamedFootprint>& Footprints() const { return footprints_; }
  // Whether a game file exists (the default mesh and texture names depend on it).
  std::function<bool(const std::string&)> fileExists;

  static SimBlueprints* From(lua_State* L);

  // The category class table's address (moho.EntityCategory, the metatable NewCat gives each category it makes; NewCat
  // keeps it): ToCategory takes a userdata with that metatable without looking the class up again. No script assigns
  // moho.EntityCategory (FAF, its mods, M28: none); a metatable at another address still goes through the look-up.
  mutable const void* categoryClass = nullptr;
  // The entity index Find(name) gives (its entityIndex; -1 for none or a blueprint with none) for a Lua string's text
  // `s` of `len` bytes: kept by the string's address and checked against its text, so a string made again at another
  // string's old address cannot be taken for it. Find is a pure function of the name once CopyToSim is done.
  int EntityIndexOfName(const char* s, size_t len) const;

 private:
  struct NameIndex {
    const char* s = nullptr;
    uint32_t len = 0;
    int index = -1;
    char text[40];
  };
  mutable std::array<NameIndex, 2048> nameIndex_{};
  std::vector<NamedFootprint> footprints_;
  void ReadFootprints(lua_State* rules, int list);
  std::vector<BlueprintInfo> all_;
  std::vector<const BlueprintInfo*> entities_;
  std::unordered_map<std::string, size_t> byId_;  // lower-case id -> index in all_
  std::unordered_map<std::string, EffectBlueprint> effects_;  // lower-case id -> effect blueprint (read by CopyToSim)
  std::map<std::string, std::vector<int>> categories_;
  int words_ = 1;
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
int l_GetCueBank(lua_State* L);  // GetCueBank(sound) -> cue, bank
// The bank and cue of the sound at stack index arg (a sound object, or a parameter table). False: no sound there.
bool SoundNames(lua_State* L, int arg, std::string* bank, std::string* cue);

// Entity category value: push a new category set (all bits clear) and get its words.
uint64_t* PushCategory(lua_State* L);
const uint64_t* ToCategory(lua_State* L, int idx);
bool CategoryHas(const uint64_t* bits, int entityIndex);
// Number of 64-bit words in a category set of L's blueprints (SimBlueprints::From(L)); parse an expression
// ("TECH1 LAND", "A + B", ...).
int CategoryWordCount(lua_State* L);
bool ParseCategory(lua_State* L, const char* expr, std::vector<uint64_t>& out);

}  // namespace moho
