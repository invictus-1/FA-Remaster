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
  bool paused = false;     // Unit:SetPaused (the same flag as UnitStateView::paused)
  int queueLength = 0;     // commands in its queue
  int headCommand = -1;    // the first one's type (the engine's command numbers), -1 = none
  // the mesh blueprint its script set on it with SetMesh (a personal shield's, a build mesh's); "" = its own
  std::string mesh;
  // A shield: hasShield while a shield entity hangs on the unit (a generator's bubble, a personal shield; the
  // script makes it when the unit is finished or the upgrade is, and destroys it with the unit or the upgrade),
  // and shieldRatio as the shield's script last gave Unit:SetShieldRatio, the same number as
  // UnitStateView::shieldRatio: 1 full, the health's share under fire and while it recharges, the charge's
  // progress while it comes up, 0 when it is down or switched off. So hasShield with 0 is a shield that is down.
  bool hasShield = false;
  float shieldRatio = 0;
};

// An entity a script made that is neither unit, projectile nor prop (a shield bubble, a build frame, the pieces
// of a death): every one that has a mesh set now, in creation order. Position and orientation are the
// simulation's own for this beat (an attached one: its parent's bone with Entity:SetParentOffset applied).
struct EntityView {
  uint32_t id = 0;
  bool shield = false;    // made by _c_CreateShield (a shield bubble)
  std::string mesh;       // the mesh blueprint, as Entity:SetMesh last gave it
  int army = 0;           // 1-based, 0 = none
  float position[3] = {0, 0, 0};
  float orientation[4] = {0, 0, 0, 1};  // x, y, z, w
  float scale[3] = {1, 1, 1};           // Entity:SetScale
  float drawScale = 1;                  // Entity:SetDrawScale
  uint32_t parent = 0xffffffff;         // the entity it is attached to (its id), 0xffffffff = none
  int parentBone = -1;                  // that entity's bone (-1: the entity itself)
  // SetVizToFocusPlayer / Allies / Enemies / Neutrals as last called: 0 never called, 1 'Always', 2 'Never',
  // 3 'Intel'. Whom that hides it from is the host's to work out from the entity's army and the viewer's.
  uint8_t vizFocus = 0, vizAllies = 0, vizEnemies = 0, vizNeutrals = 0;
};

// How the scenery changed since the list was last taken. Listed only while Sim::collectProps is set (set it
// before the first beat, or props that went before it are not told). The map's own props are made in the map
// file's order, so one of them is named by its index in the file's prop list (from 0, counting every entry of
// the file); a host that drew them from the file needs no list at the start. Create: a prop made after the
// start (wreckage, a script's CreateProp), with what it is now. Change: a prop whose mesh (SetMesh: a burnt
// tree), position, orientation (a tree knocked over turns about its foot, a little every beat while it falls)
// or scale is no longer what was last told, or, for a map prop, what the file has. Destroy: it is gone
// (reclaimed, destroyed, burnt out). At most one Create or Change per prop per list.
struct PropEvent {
  enum Kind { Create, Change, Destroy } kind = Create;
  uint32_t id = 0;        // the prop's entity id
  int mapIndex = -1;      // its place in the map file's prop list; -1: made after the start
  std::string blueprint;  // Create, Change: the prop blueprint's id (its path)
  std::string mesh;       // Create, Change: the mesh blueprint Entity:SetMesh gave it, "" = its blueprint's own
  float position[3] = {0, 0, 0};
  float orientation[4] = {0, 0, 0, 1};  // x, y, z, w
  float scale[3] = {1, 1, 1};           // the map file's, or Entity:SetScale
  float drawScale = 1;                  // Entity:SetDrawScale
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

// The bones of a unit's mesh that its script has hidden now (Unit:HideBone / ShowBone): a commander's upgrades
// not bought yet, a factory's arm parts, a missile not in its silo. Bone indices as in UnitPose (the mesh's bone
// list, .scm order), in rising order, the children of a bone hidden "with its children" listed too. Only units
// with at least one hidden bone are listed.
struct UnitHiddenBones {
  uint32_t id = 0;  // the unit's entity id
  std::vector<int> bones;
};

struct ProjectileView {
  uint32_t id = 0;
  std::string blueprint;  // the projectile blueprint's id (its path)
  int army = 0;           // 1-based, 0 = none
  float position[3] = {0, 0, 0};
  float orientation[4] = {0, 0, 0, 1};
  float velocity[3] = {0, 0, 0};  // world units per second
  // Entity:SetScale(x, y, z) as last set (1, 1, 1 unless a script set it, or the blueprint's MeshScaleRange drew
  // another size at launch), and Entity:SetDrawScale: the raw numbers, kept for hosts that used them. They are
  // NOT factors on Display.UniformScale (an earlier line here said so): use `size` below.
  float scale[3] = {1, 1, 1};
  float drawScale = 1;
  // Projectile:SetScaleVelocity, or the blueprint's MeshScaleVelocity as drawn at launch: how fast the scale
  // grows, per second. The simulation does not apply it (nothing in it goes by a shell's drawn size), so `scale`
  // stays as set: a host adds scaleVelocity * (seconds since it first saw the shell).
  float scaleVelocity[3] = {0, 0, 0};
  // THE SIZE TO DRAW THE MESH AT this beat, per axis, as the original holds it (its entity scale): the blueprint's
  // Display.UniformScale (+- MeshScaleRange) at launch, grown by the scale velocity every beat (velocity x 0.1,
  // in these same units: a ball of UniformScale 17 with velocity 5.1 is 22.1 a second later), and replaced
  // outright by SetScale(x, y, z) or SetDrawScale(s), which write this very value (UniformScale no longer counts
  // then). A host draws at `size` and needs none of scale / drawScale / scaleVelocity above; between two beats it
  // may add scaleVelocity x the time since the beat.
  float size[3] = {1, 1, 1};
  // SetVizToFocusPlayer / Allies / Enemies / Neutrals as last called on the shell, as on EntityView: 0 never
  // called, 1 'Always', 2 'Never', 3 'Intel'. FAF's ShieldCollider shell (the box it hangs on a crashing
  // aircraft so that a shield stops it) sets all four to 'Never': it is not meant to be seen by anybody.
  uint8_t vizFocus = 0, vizAllies = 0, vizEnemies = 0, vizNeutrals = 0;
  // The shell HIT something in this beat: `position` is the hit point, and it stays there until its script's
  // OnImpact runs at the start of the next beat and (usually) destroys it. `velocity` is still the speed it
  // arrived with (scripts read it), so a host that moves shells on along their velocity between two beats must
  // NOT do so for a stopped one: it would draw the shell flying through what it hit, up to a tenth of a second
  // of flight beyond (Chris, 2026-10-13: "some go through the acu").
  bool stopped = false;
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
  // A factory's rally queue (the orders its products are given when they leave it), first to last; the engine
  // puts the first rally in it when the factory is made, and again whenever the queue is empty
  // (SetUpInitialRally: a Move to the blueprint's InitialRallyX / InitialRallyZ in the factory's own frame).
  // hasFactoryQueue: the unit is a factory that keeps one, also when it
  // is empty. The original's script call unit:GetCommandQueue() answers with THIS queue for such a unit and
  // with `queue` for every other.
  bool hasFactoryQueue = false;
  std::vector<CommandView> factoryQueue;
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
  // Create with a second entity (AttachBeamEntityToEntity): where its point is, taken the same way. The far end
  // can be an entity EntityViews does not list (one without a mesh: the hubs of adjacency's links).
  bool hasPosition2 = false;
  float position2[3] = {0, 0, 0};
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

// What an army knows now, as the simulation's own recon has it. Bits: 1 on radar, 2 on sonar, 4 on omni, 8 seen
// (vision; water vision for what is under water). army: 1-based. Allies' intel counts as the army's own, as it
// does for the simulation.
enum ReconBits : uint32_t {
  kReconRadar = 1, kReconSonar = 2, kReconOmni = 4, kReconSeenNow = 8,
  kReconSeenEver = 0x10,   // (units only) it was seen at some time
  kReconKnownFake = 0x20,  // (units only) the army knows this blip is a jammer's
  kReconMaybeDead = 0x40,  // (units only) the unit is gone, the army has not found out yet
  kReconHeld = 0x80,       // (units only) the army holds a blip of it at all
};
// Of a place: what the army's intel grids say there now. water: 0 = for something on land or in the air, 1 = on
// the water's surface (sonar counts), 2 = under water. 0 outside the map. Without fog of war everything is seen.
uint32_t ReconAt(const Sim& sim, int army, const float position[3], int water = 0);
// Of an entity. A unit of the army or of an ally: radar, sonar, omni and seen, all set. Another army's unit:
// the flags the army's recon holds on it (its blip), which is what the simulation's own weapons and scripts go
// by; they are renewed when the army's recon runs (armies take turns, one a beat), so they can be a few beats
// old; 0 = the army has no blip of it. Anything that is not a unit (a projectile, a prop, a shield): ReconAt its
// position. 0 if there is no such entity.
uint32_t ReconOf(const Sim& sim, int army, uint32_t entityId);

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
// Hidden bones of every unit that has any (see UnitHiddenBones). Take it once per beat, beside UnitPoses.
void HiddenBones(const Sim& sim, std::vector<UnitHiddenBones>* out);
void ProjectileViews(const Sim& sim, std::vector<ProjectileView>* out);
void BeamViews(const Sim& sim, std::vector<BeamView>* out);
// Script-made entities with a mesh (see EntityView). Take it once per beat, after Tick().
void EntityViews(const Sim& sim, std::vector<EntityView>* out);
// The scenery's changes since the last call (see PropEvent). Take it once per beat, after Tick(), with
// Sim::collectProps set from before the first beat. (It compares every prop with what was last told: measured
// 0.066 ms a beat with 6,200 props in a small fight, 0.46 ms with 14,000 props and 200 changes a beat.)
void PropEvents(Sim& sim, std::vector<PropEvent>* out);
// One unit's state for the panels (false: no such unit). Not const: the live caps are read through the scripts'
// state. Ask for the units on screen, not for all of them every beat.
bool UnitStateOf(Sim& sim, uint32_t unitId, UnitStateView* out);
// The ids of the unit blueprints an army may not build (AddBuildRestriction), in blueprint order. army: 1-based.
std::vector<std::string> RestrictedBlueprints(const Sim& sim, int army);
// The ids of the unit blueprints ONE UNIT may not build although its blueprint's Economy.BuildableCategory names
// them: its own restrictions (Unit:AddBuildRestriction / RemoveBuildRestriction, as the commanders' and the
// factories' scripts set them: a commander without the tier 2 upgrade, a factory before its upgrade), in
// blueprint order. Empty for a unit with none, and for an id that is no unit. The army's restrictions
// (RestrictedBlueprints) are a separate list: a build menu takes BuildableCategory less both.
std::vector<std::string> UnitRestrictedBlueprints(const Sim& sim, uint32_t unitId);
// Whether that unit may build that blueprint now: the simulation's own test for a build order (the blueprint's
// BuildableCategory, the unit's restrictions and its army's). False for an unknown unit or blueprint.
bool UnitMayBuild(const Sim& sim, uint32_t unitId, const std::string& blueprintId);
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
  float lifeBarSize = 0, lifeBarHeight = 0, lifeBarOffset = 0;  // LifeBarSize, LifeBarHeight, LifeBarOffset
  // SelectionThickness (the bracket's share of the selection square when it is not 0; else the engine's 0.2)
  // and SelectionCenterOffsetX / Y / Z (the square's centre, in the unit's own frame): docs/engine-answers.md
  // "render 23" has the original's rule.
  float selectionThickness = 0;
  float selectionCenterOffset[3] = {0, 0, 0};
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
// For a reclaim order and its cursor: the prop (tree, rock, wreckage) of the category RECLAIMABLE at a place.
// Among the props whose box (the blueprint's SizeX x SizeZ times the prop's scale, around its position, on the
// map plane) lies within `within` of the place, the one nearest to it; with several at the same distance, the
// one whose centre is nearer. False: none. mass / energy: what is left in it now (the prop script's
// MaxMassReclaim / MaxEnergyReclaim times its ReclaimLeft; 0 for a prop that holds nothing). Units are not
// looked at: a unit under the cursor is the host's to find. Not const: the values are read from the scripts'
// state.
struct ReclaimableView {
  uint32_t id = 0xffffffff;       // the prop's entity id (what a Reclaim order names)
  std::string blueprint;          // the prop blueprint's id
  float position[3] = {0, 0, 0};
  float sizeX = 0, sizeZ = 0;     // the box it was tested with
  float mass = 0, energy = 0;
};
bool ReclaimableAt(Sim& sim, const float at[3], float within, ReclaimableView* out);
// The effect events since the last call (the list is emptied by it). Call once per beat, after Tick(), with
// Sim::collectEffects set from before the first beat.
void EffectEvents(const Sim& sim, std::vector<EffectEvent>* out);
// The sounds since the last call (and the loops that ended with their entity); the list is then empty again.
// Take it once per beat, after Tick(), with Sim::collectSounds set from before the first beat.
void SoundEvents(const Sim& sim, std::vector<SoundEvent>* out);
// (The scenario info for SessionGetScenarioInfo is Sim::sessionScenario: the launched table as bytes.)

}  // namespace moho
