// What a host that shows the game reads from a running simulation between beats: the armies, an army's economy
// totals, the units with their bone poses, projectiles and beams. Plain values, copied out; the host builds its own tables from them.
// (The per-beat Sync table is Sim::TakeSync.)
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace moho {

class Sim;

struct ArmyView {
  int index = 0;         // 1-based, as the scripts count armies
  std::string name;      // "ARMY_1"
  std::string nickname;  // the player's name
  int faction = 0;       // 0-based (UEF = 0)
  bool human = false;
  bool civilian = false;
  bool outOfGame = false;  // SetArmyOutOfGame
  bool showScore = true;   // SetArmyShowScore
  // The lobby's colour numbers (the options' ArmyColor and PlayerColor: indices into /lua/GameColors.lua's
  // ArmyColors and PlayerColors, 0 = not given), and the colour a script set with SetArmyColor (-1 = none; it
  // wins over the indices). A civilian without either takes GameColors.CivilianArmyColor.
  int armyColor = 0, playerColor = 0;
  int rgb[3] = {-1, -1, -1};
  std::vector<int> alliance;  // towards each army, by index - 1: 0 enemy, 1 neutral, 2 ally
};

// One army's totals, [0] energy, [1] mass. The rates are per beat (ten beats a second), as the engine keeps them.
struct EconomyTotals {
  float income[2] = {0, 0};            // last beat's income
  float lastUseRequested[2] = {0, 0};  // last beat's requests
  float lastUseActual[2] = {0, 0};     // what was granted of them
  float stored[2] = {0, 0};
  float maxStorage[2] = {0, 0};
  float reclaimed[2] = {0, 0};         // since the game began
};

struct UnitView {
  uint32_t id = 0;        // the entity id (stable while the unit lives; reused later, as in the recordings)
  std::string blueprint;  // "uel0001"
  int army = 0;           // 1-based
  float position[3] = {0, 0, 0};
  float orientation[4] = {0, 0, 0, 1};  // a quaternion: x, y, z, w
  float health = 0, maxHealth = 0;
  float fractionComplete = 1;
  float workProgress = 0;
  bool dead = false;
  bool beingBuilt = false;
  bool selectable = true;  // not in the 'UnSelectable' state
  int queueLength = 0;     // commands in its queue
  int headCommand = -1;    // the first one's type (the engine's command numbers), -1 = none
};

// One bone of a unit's pose of this beat, where it differs from the rest pose: the bone's transform RELATIVE
// TO THE UNIT (down the bone chain from the unit's origin; positions in world units, the mesh's uniform scale
// applied). A bone below a moved bone is listed too, since its transform in the unit's frame moved with it.
struct BonePose {
  int bone = 0;                         // index in the mesh's bone list (.scm order)
  float position[3] = {0, 0, 0};
  float orientation[4] = {0, 0, 0, 1};  // x, y, z, w
};
struct UnitPose {
  uint32_t id = 0;  // the unit's entity id
  std::vector<BonePose> bones;
};

struct ProjectileView {
  uint32_t id = 0;
  std::string blueprint;  // the projectile blueprint's id (its path)
  int army = 0;           // 1-based, 0 = none
  float position[3] = {0, 0, 0};
  float orientation[4] = {0, 0, 0, 1};
  float velocity[3] = {0, 0, 0};  // world units per second
};

// A weapon's collision beam while it is on: from its muzzle to where it ends this beat.
struct BeamView {
  uint32_t id = 0;
  std::string blueprint;  // the beam blueprint's id, empty if it has none
  int army = 0;
  uint32_t unit = 0;      // the firing unit's entity id (0xffffffff if gone)
  float start[3] = {0, 0, 0};
  float end[3] = {0, 0, 0};
};

// What the scripts did with effects in a beat (emitters, beams, trails, light particles, decals), in the order
// of their calls. Listed only while Sim::collectEffects is set. Create: one of the Create... / AttachBeam...
// functions; Call: a method on an effect made earlier (ScaleEmitter, OffsetEmitter, SetEmitterParam,
// SetEmitterCurveParam, ResizeEmitterCurve, SetBeamParam) or AttachBeamToEntity; Destroy: the script destroyed
// it, or the entity an attached effect was on is gone.
struct EffectEvent {
  enum Kind { Create, Call, Destroy } kind = Create;
  uint32_t id = 0;          // the effect's own number, from 1, never reused
  std::string name;         // the function ("CreateEmitterAtBone") or the method ("ScaleEmitter")
  std::string blueprint;    // Create: the emitter / beam / trail blueprint, or the texture
  int army = 0;             // Create: as the script gave it (1-based, or -1)
  uint32_t entity = 0xffffffff, entity2 = 0xffffffff;  // entity ids: what it is on, and a beam's other end
  int bone = -1, bone2 = -1;                            // the bones given (-1: the entity itself)
  // Create: where that point is when the list is taken (the entity's bone in this beat's pose; where it was at
  // the call if the entity is gone by then), or the position given (decals)
  float position[3] = {0, 0, 0};
  std::vector<float> numbers;  // the call's other numbers, in order
  std::string text, text2;     // the call's other strings, in order (a parameter's name; a second texture)
};

std::vector<ArmyView> ArmyViews(const Sim& sim);
// army: 1-based; an army that does not exist gives zeros.
EconomyTotals EconomyTotalsOf(const Sim& sim, int army);
// Every unit the simulation holds (those waiting to be destroyed left out), in entity-id order.
void UnitViews(const Sim& sim, std::vector<UnitView>* out);
// The posed bones of one unit (false: no such unit; an empty list: it stands in its rest pose), and of every
// unit that has any, in entity-id order. Read once per beat, after Tick().
bool UnitPoseOf(const Sim& sim, uint32_t unitId, std::vector<BonePose>* out);
void UnitPoses(const Sim& sim, std::vector<UnitPose>* out);
// The projectiles in flight (creation order) and the beams that are on.
void ProjectileViews(const Sim& sim, std::vector<ProjectileView>* out);
void BeamViews(const Sim& sim, std::vector<BeamView>* out);
// The effect events since the last call (the list is emptied by it). Call once per beat, after Tick(), with
// Sim::collectEffects set from before the first beat.
void EffectEvents(const Sim& sim, std::vector<EffectEvent>* out);
// (The scenario info for SessionGetScenarioInfo is Sim::sessionScenario: the launched table as bytes.)

}  // namespace moho
