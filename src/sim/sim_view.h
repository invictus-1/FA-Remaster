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

// One entry of a unit's command queue, first to last.
struct CommandView {
  uint32_t id = 0;                // the command's id (what CommandWriter::RemoveCommandFromQueue and the others take)
  int type = 0;                   // the engine's command numbers (2 Move, 7 BuildFactory, 8 BuildMobile, ...)
  bool hasPosition = false;
  float position[3] = {0, 0, 0};  // its target position, when it has one
  uint32_t entity = 0xffffffff;   // its target entity's id, 0xffffffff = none
  std::string blueprint;          // a build's blueprint id
  int count = 1;                  // how many (factory builds)
};

// What the panels show of one unit beyond UnitView. Entity ids as in UnitView; 0xffffffff = none.
struct UnitStateView {
  uint32_t id = 0;
  std::vector<CommandView> queue;
  uint32_t focus = 0xffffffff;    // what it builds, repairs, reclaims, captures (GetFocus)
  uint32_t guarded = 0xffffffff;  // the unit it guards (GetGuardedEntity)
  uint32_t creator = 0xffffffff;  // the factory, engineer or upgrading unit that made it (GetCreator)
  int fireState = 0;              // 0 return fire, 1 hold fire, 2 hold ground
  uint32_t scriptBits = 0;        // bit n set = toggle n is on
  bool paused = false, repeatQueue = false, autoMode = false, autoSurfaceMode = false;
  // what it can do right now: RULEUCC_ and RULEUTC_ bits (the blueprint's, as scripts changed them since)
  uint32_t commandCaps = 0, toggleCaps = 0;
  float shieldRatio = 0;          // as the shield's script last set it (0: none or down)
  float fuelRatio = -1;           // -1: the unit uses no fuel
  int tacticalSiloCount = 0, nukeSiloCount = 0;  // missiles in store
  // missiles asked for and not finished yet (the one being built among them), and the store's room
  // (MaxProjectileStorage; 0 = no such silo)
  int tacticalSiloBuildCount = 0, nukeSiloBuildCount = 0;
  int tacticalSiloMaxStorage = 0, nukeSiloMaxStorage = 0;
  int kills = 0;                  // the unit's KILLS statistic, as its scripts last set it
  // what SetCapturable / SetReclaimable left (both true until a script says otherwise)
  bool capturable = true, reclaimable = true;
  // economy, [0] energy, [1] mass, per second: what its scripts ask for, and what last beat gave and took
  float productionPerSecond[2] = {0, 0}, consumptionPerSecond[2] = {0, 0};
  float produced[2] = {0, 0}, consumed[2] = {0, 0};
  float buildRate = 0;            // as it is now
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

// What the scripts did with sounds in a beat, in the order of their calls. Listed only while Sim::collectSounds
// is set. Play: Entity:PlaySound(sound) or UnitWeapon:PlaySound(sound), a sound heard once. Loop:
// Entity:SetAmbientSound(sound, rumble), the entity's running sound from now on; both names empty = it stops
// (the script passed nil, or the entity is gone). The simulation plays nothing and leaves nothing out: every
// call of every army is listed, and the host decides what its player may hear.
struct SoundEvent {
  enum Kind { Play, Loop } kind = Play;
  uint32_t entity = 0xffffffff;       // the entity it was called on (a weapon: its unit)
  int army = 0;                       // that entity's army, 1-based, 0 = none
  std::string bank, cue;              // the sound (as GetCueBank names them)
  std::string rumbleBank, rumbleCue;  // Loop: the second sound of the call
  float position[3] = {0, 0, 0};      // where the entity stood at the call
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
// One unit's state for the panels (false: no such unit). Not const: the live caps are read through the scripts'
// state. Ask for the units on screen, not for all of them every beat.
bool UnitStateOf(Sim& sim, uint32_t unitId, UnitStateView* out);
// The ids of the unit blueprints an army may not build (AddBuildRestriction), in blueprint order. army: 1-based.
std::vector<std::string> RestrictedBlueprints(const Sim& sim, int army);
// The seed the simulation's random stream was started with: the same for every player of a game and in its
// replay (the replay header holds it), different from game to game. For things that are only seen and must look
// the same to everyone (weather). Valid after Start(). Reading it draws nothing from the stream.
uint32_t SessionSeed(const Sim& sim);
// The display part of the blueprints as the simulation has them, i.e. after the game's blueprint scripts and the
// mods changed them (what tools/dump_display.lua writes to a log, as a call). Read it once after Start().
struct DisplayLod {
  std::string mesh, albedo, normals, specular, secondary, lookup, shader;  // the game's virtual paths; "" = not set
  float cutoff = 0;                                                        // LODCutoff (0: not set)
};
struct BlueprintDisplay {
  std::string id;          // as UnitView::blueprint and ProjectileView::blueprint name it (a prop: its .bp path)
  int kind = 0;            // 0 unit, 1 projectile, 2 prop
  float uniformScale = 1;  // Display.UniformScale (1 when not set)
  std::vector<DisplayLod> lods;  // the mesh blueprint's LODs, in the blueprint's order (nearest first)
  // units only; "" / 0 when the blueprint does not set them:
  std::string icon;        // StrategicIconName
  int iconSortPriority = 0;      // StrategicIconSortPriority
  float iconFadeInZoom = 0;      // Display.Mesh.IconFadeInZoom
  float selectionSizeX = 0, selectionSizeZ = 0;
};
// Every unit, projectile and prop blueprint whose Display.MeshBlueprint names a mesh blueprint with LODs, in
// blueprint order.
void BlueprintDisplays(const Sim& sim, std::vector<BlueprintDisplay>* out);
// For a build preview: where a structure ordered at `pos` would stand (its footprint snapped to the cell grid, y
// on the ground or the water) and whether the placement test passes there (CAiBrain::CanBuildStructureAt: the
// layers and flatness, no structure or other structure's skirt in the way, no mass or hydrocarbon deposit under
// its skirt unless it is the kind built on one). False: no such unit blueprint. Not const: blueprint data is
// read through the scripts' state. It does not say whether a given builder may build it (RestrictedBlueprints,
// the unit's live caps) or can reach it.
bool BuildSiteOf(Sim& sim, const std::string& blueprint, const float pos[3], float site[3], bool* canBuild);
// The effect events since the last call (the list is emptied by it). Call once per beat, after Tick(), with
// Sim::collectEffects set from before the first beat.
void EffectEvents(const Sim& sim, std::vector<EffectEvent>* out);
// The sounds since the last call (and the loops that ended with their entity); the list is then empty again.
// Take it once per beat, after Tick(), with Sim::collectSounds set from before the first beat.
void SoundEvents(const Sim& sim, std::vector<SoundEvent>* out);
// (The scenario info for SessionGetScenarioInfo is Sim::sessionScenario: the launched table as bytes.)

}  // namespace moho
