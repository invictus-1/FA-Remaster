// What a host that shows the game reads from a running simulation between beats: the armies, an army's economy
// totals, the units with their bone poses, projectiles and beams. Plain values, copied out; the host builds its own tables from them.
// (The per-beat Sync table is Sim::TakeSync.)
#pragma once
#include <cstdint>
#include <utility>
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
  // ui request 45: the army's start (SetArmyStart / GenerateArmyStart: the original's army+0x1c4 / +0x1c8, x and
  // z) and its no-rush circle, as the user side's army holds them (+0x1bc .. +0x1d0, a copy of the sim army's
  // +0x1c4 .. +0x1d8). noRushTicks: the no-rush timer in beats now (the same number as CAiBrain:GetNoRushTicks;
  // 0 when off or run out); noRushRadius: ScenarioInfo.norushradius (100 when the map gives none);
  // noRushOffset: ScenarioInfo.norushoffsetX_<army name> and norushoffsetY_<army name> (both 0 unless both are
  // given). The original draws the circle (ExtractFocusArmyNoRushRange 0x7ef1c0) for the focus army only while
  // noRushTicks > 0, around (start[0] + noRushOffset[0], 0, noRushOffset[1] + start[1]) with noRushRadius.
  float start[2] = {0, 0};
  int noRushTicks = 0;
  float noRushRadius = 0;
  float noRushOffset[2] = {0, 0};
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

// ui request 37: one weapon of a unit as the original's user side holds it, for UserUnit:CanAttackTarget,
// GetValidAttackingUnits and the right click's attack-invalid rule (docs/engine-answers.md "ui 37" has the rules).
// The original's entry (UnitWeaponInfo, 0x98 bytes, the vector SSTIUnitVariableData+0xf8 = UserUnit+0x290) is
// filled by Unit::Sync 0x6abcc0 (0x6abf60..0x6ac11a) from attacker->GetWeapon(i):
//   +0x00 <- weapon+0xf8 (UnitWeapon::GetCat1 0x6a4c70): the TargetRestrictDisallow set
//   +0x28 <- weapon+0x120 (0x6a4ca0): the TargetRestrictOnlyAllow set (both parsed by the weapon's constructor,
//          0x6d4654 / 0x6d46d3; no script binding changes them later)
//   +0x50 <- weapon+0x148 (0x6ac045): the fire-target layer caps, as UnitWeapon:SetFireTargetLayerCaps 0x6da3d0 last
//          set them (0 until the scripts' first call)
//   +0x54 <- weapon+0x60 unless it is below 0, else the blueprint's MinRadius (bp+0x58) (0x6ac05f)
//   +0x58 <- weapon+0x64 unless it is below 0, else the blueprint's MaxRadius (bp+0x5c) (0x6ac07c): the overrides
//          are what ChangeMinRadius / ChangeMaxRadius set
// (and +0x5c the EffectiveRadius, +0x60 / +0x7c the UIMinRangeVisualId / UIMaxRangeVisualId: not here).
// The original refreshes the entries only in a beat that marked the unit (its byte +0x68e, set by the weapon's
// creation, SetFireTargetLayerCaps, ChangeMinRadius / ChangeMaxRadius), and no more once the unit is dead or being
// destroyed (0x6abeb4 / 0x6abec9: IUnit::IsDead, DestroyQueued). These are the weapon's values after the beat: the
// same for a live unit; a dead one's may have moved on since its death (a crashing aircraft's new layer).
struct WeaponView {
  int layerCaps = 0;  // ELayer bits, as UnitView::layer (1 Land, 2 Seabed, 4 Sub, 8 Water, 16 Air, 32 Orbit)
  float minRadius = 0, maxRadius = 0;
  // the two sets, as indices into the simulation's table of category sets (CategorySetHas, CategorySets below);
  // -1 = the empty set (no restriction)
  int disallow = -1, onlyAllow = -1;
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
  // what it can do this beat: RULEUCC_ and RULEUTC_ bits (the blueprint's, as scripts changed them since); the
  // same numbers as UnitStateView's
  uint32_t commandCaps = 0, toggleCaps = 0;
  // ui request 28. layer: the original's ELayer bits (1 Land, 2 Seabed, 4 Sub, 8 Water, 16 Air, 32 Orbit; 0 none);
  // attached: it hangs on a parent (docked, loaded, stored: the original's "parent weak ptr != null",
  // SSTIUnitVariableData+0xa); attachedCount: the entities attached to it now (units by AttachTo / AttachBoneTo,
  // and script entities such as a shield on it: the original's children, SSTIEntityVariableData+0x68).
  int layer = 0;
  bool attached = false;
  int attachedCount = 0;
  // ui request 36 (how the interface's GetAttachedUnitsList, SelectUnits and GetAssistingUnitsList use these:
  // docs/engine-answers.md "ui 36"). Entity ids, 0xffffffff = none (not 0: 0 is army 1's first unit).
  // parent: the entity it hangs on, as EntityView::parent: the original's SSTIEntityVariableData+0x64
  // (Entity::SyncInterface 0x67a2b6 from the parent weak pointer, entity+0x18c), what the user side's
  // GetAttachmentParent 0x838030 looks up. attachedUnits: the units hanging on it, in the order they were
  // attached: the original's children list (+0x68; Entity::AttachTo 0x679550 appends, DetachFrom 0x6796f0 keeps
  // the others' order) less what is not a unit; a killed unit not yet destroyed is still in it.
  // creator: the unit that made it (its factory or engineer, or Unit:SetCreator: a pod's station or commander), the
  // same as UnitStateView::creator: the original's SSTIUnitVariableData+0x0 (Unit::Sync 0x6abcf7), the user side's
  // UserUnit+0x3c0 (its GetCreator 0x8c4a00).
  uint32_t parent = 0xffffffff;
  std::vector<uint32_t> attachedUnits;
  uint32_t creator = 0xffffffff;
  // the mesh blueprint its script set on it with SetMesh (a personal shield's, a build mesh's); "" = its own
  std::string mesh;
  // A shield: hasShield while a shield entity hangs on the unit (a generator's bubble, a personal shield; the
  // script makes it when the unit is finished or the upgrade is, and destroys it with the unit or the upgrade),
  // and shieldRatio as the shield's script last gave Unit:SetShieldRatio, the same number as
  // UnitStateView::shieldRatio: 1 full, the health's share under fire and while it recharges, the charge's
  // progress while it comes up, 0 when it is down or switched off. So hasShield with 0 is a shield that is down.
  bool hasShield = false;
  float shieldRatio = 0;
  // The texture scroll of its mesh (a tank's treads; AirUnit's ping-pong): the original's two offsets as the
  // simulation left them this beat (entity+0x100 / +0x104 -> scroll) and before this beat's step (+0xf8 / +0xfc
  // -> scrollPrev). Unwrapped (they grow without bound while a tank drives; the texture wraps them). The
  // original draws, per frame (0x7e8276, the D3D9 instance packer 0x945334):
  //   f = the frame's fraction of the way from the last beat to this one, 0..1 (the global 0x10a6410, which
  //       MeshInstance::UpdateInterpolatedFields 0x7dec95 also blends the unit's transform by)
  //   material.z = scrollPrev[0] + (scroll[0] - scrollPrev[0]) * f
  //   material.w = scrollPrev[1] + (scroll[1] - scrollPrev[1]) * f
  //   anim.w     = 255 when the drawn LOD's mesh blueprint has Scrolling = true (DisplayLod::scrolling), else 0
  // and mesh.fx's ComputeScrolledTexcoord adds material.z to texcoord0.x and .z of the vertices with
  // texcoord0.y > 0.95, material.w to those with 0.90 < texcoord0.y <= 0.95. Use scrollPrev as given, NOT last
  // beat's scroll: a ping-pong sets both to its new value (a jump, no blend), RemoveScroller puts scroll back to
  // scrollPrev, and a standing tank's step changes neither (so a scroller still on while it stands blends the
  // same last step again every beat, as the original does).
  // How the simulation moves them (CTextureScroller::Tick 0x777730, once a beat at the start of the unit's turn,
  // from its committed transform C = (q, p) and the one before it P; entity.h TextureScroller):
  //   AddThreadScroller(sideDist, mult) (FAF: 1, Treads.ScrollMultiplier or 0.2): if p_C == p_P (bitwise) no
  //     change; else X = each transform's local +x axis in the world, Z = (Z_C + Z_P) * 0.5 (local +z, forward),
  //     scrollPrev = scroll, scroll[0] += dot(Z, (p_C + X_C sd) - (p_P + X_P sd)) * mult (the +x side's point),
  //     scroll[1] += dot(Z, (p_C - X_C sd) - (p_P - X_P sd)) * mult (the -x side's): forward motion of each tread,
  //     with the turn's part opposite on the two sides. Positive when driving forward.
  //   AddManualScroller(a, b): scrollPrev = scroll; scroll += (a, b) every beat.
  //   AddPingPongScroller(...): both jump between their ping and pong values at the blueprint's timing.
  //   RemoveScroller(): scroll = scrollPrev, and no more steps.
  // All 0 for a unit that never had a scroller.
  float scrollPrev[2] = {0, 0};
  float scroll[2] = {0, 0};
  // ui request 45: its intel attributes as the simulation holds them (the original's EntityAttributes,
  // entity+0x128, which the user side reads at UserEntity+0x100). Eight words, each a radius in the low 31 bits
  // and bit 31 = enabled: [0] Vision, [1] WaterVision, [2] Radar, [3] Sonar, [4] Omni, [5] RadarStealth,
  // [6] SonarStealth, [7] Cloak; [5] .. [7] hold the radar stealth, sonar stealth and cloak FIELD radii.
  // At the unit's creation each word is its blueprint's radius (Intel.VisionRadius .. OmniRadius,
  // RadarStealthFieldRadius, SonarStealthFieldRadius, CloakFieldRadius) with bit 31 set when the radius is not 0
  // (EntityAttributes::Initialize 0x6a46a0); then InitIntel / SetIntelRadius change the radius (keeping the bit)
  // and EnableIntel / DisableIntel the bit, for these kinds (Jammer, Spoof and the '...Field' kinds have no word).
  // The original's interface reads them so (radius = word & 0x7fffffff; the enabled bit is not looked at):
  //   UserUnit::GetIntelRanges 0x8bfd70: omni [4], radar [2], sonar [3]; nothing when the unit has the live
  //     toggle cap 8 (RULEUTC_IntelToggle) and that toggle is on: toggleCaps & 8 and UnitStateView::scriptBits & 8
  //     (the user side's unit attributes +0x64 / +0x70, the sim unit's +0x48c / +0x498);
  //   UserUnit::GetMaxCounterIntel 0x8bfe50: the largest of [7], [6], [5], the blueprint's Intel.JamRadius.Max and
  //     Intel.SpoofRadius.Max; nothing when toggle 4 (Jamming) or 0x20 (Stealth) is held so in the same way.
  uint32_t intel[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  // ui request 37. categoryIndex: its blueprint's bit in the simulation's category sets (what CategorySetHas takes;
  // the original's REntityBlueprint+0x5c, which UserUnit::CanAttackTarget 0x8c0d30 looks up in a weapon's sets,
  // 0x8c1115 / 0x8c115f); -1 = none. The interface state's own blueprint numbering may differ: use this one.
  // weapons: one per weapon in the blueprint's order, DummyWeapon entries left out (Unit::Unit 0x6a5fb6 makes no
  // weapon for them), so weapons[i] is the scripts' GetWeapon(i + 1); empty for an unarmed unit.
  // autoSurfaceMode: the SetAutoSurfaceMode flag (UnitStateView::autoSurfaceMode), the original's
  // SSTIUnitVariableData+9 that CanAttackTarget reads for each shooter (UserUnit+0x1a1, 0x8c0e7c).
  int categoryIndex = -1;
  std::vector<WeaponView> weapons;
  bool autoSurfaceMode = false;
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
  // Every number statistic the unit's scripts keep (Unit:SetStat / FAF's Unit:UpdateStat), by name, sorted: what
  // the interface's UserUnit:GetStat(name, default) answers with (the original's binding, cfunc_UserUnitGetStat
  // 0x8c3160, asks an object at UserUnit+0x148 through its vtable +0x50 / +0x54; that it holds the sim unit's
  // stats as synced is inferred from FAF's Unit.lua 5060 "retrieve on the UI side"). FAF's veterancy is kept there:
  // VetLevel, VetExperience, VetMassKillCredit (defaultcomponents.lua 621-627, 682-742; unitview.lua 553-554
  // reads the first two for the stars and the bar). KILLS is among them too.
  std::vector<std::pair<std::string, float>> stats;
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

// Entity:ShakeCamera(radius, maxShakeEpicenter, minShakeAtRadius, interval) calls in a beat, in the order of the
// calls (cfunc_EntityShakeCameraL 0x68ab70: the original keeps the same seven numbers per call for its user side,
// CameraImpl::CameraShake 0x7a7130). Listed only while Sim::collectShakes is set. Every army's calls are listed;
// the host decides whether its camera shakes.
struct ShakeEvent {
  uint32_t entity = 0xffffffff;   // the entity it was called on
  int army = 0;                   // that entity's army, 1-based, 0 = none
  float position[3] = {0, 0, 0};  // where the entity stood at the call
  // the four arguments as given. The exe's own help text: radius = distance from the epicenter at which shaking
  // falls off to min; max = size of shaking in world units when looking at the epicenter; min = size of shaking in
  // world units at radius or farther; interval = how long it shakes, in seconds
  float radius = 0, maxShakeEpicenter = 0, minShakeAtRadius = 0, interval = 0;
};

// Hits, impacts and health changes in a beat, in the order they happened (render REQUESTS 36: marks, ripples,
// blasts, wear). Listed only while Sim::collectHits / collectImpacts / collectHealth are set; taken with
// Sim::TakeHits / TakeImpacts / TakeHealth. Every army's are listed; the host decides what its player sees. Taking
// them reads the simulation only (no script call, no Lua object, no random draw): the game plays the same with them
// on or off. Ids are entity ids (as UnitView::id, ProjectileView::id, EntityView::id name them); 0xffffffff = none.
// Blueprint ids are as UnitView::blueprint / ProjectileView::blueprint name them.
//
// A hit: one damage an entity takes, at SIM_DoDamagePoint 0x737140 (sim/damage.cpp), i.e. each time the engine
// calls the target's OnDamage(instigator, amount, vector, type): the scripts' Damage, DamageArea and DamageRing,
// for every entity they reach, only when the amount after armour is above 0. (What the target's script then does
// with it, a personal shield taking it, veterancy, is the script's: the health list says what came of it.)
struct HitEvent {
  uint32_t target = 0xffffffff;      // the entity that took it; a shield's hit: the shield's OWNER (its unit)
  int targetKind = 0;                // 0 unit, 1 shield, 2 prop, 3 projectile, 4 other entity
  bool shield = false;               // a shield took it (targetKind 1): target is its owner, shieldId the bubble
  uint32_t shieldId = 0xffffffff;    // the shield entity (EntityView::shield) when shield is set
  uint32_t instigator = 0xffffffff;  // the damage call's instigator (usually the shooting unit; a shell, a beam)
  int army = 0;                      // the instigator's army, 1-based, 0 = none
  // The weapon that fired, as FAF's own weapon BlueprintId names it (lua/system/Blueprints.lua: "<unit blueprint
  // id>-<index in bp.Weapon>-<Label>", e.g. "uel0201-1-MainGun"), and the projectile's blueprint id. Known when the
  // damage comes from a shell's or a beam's impact (its OnImpact, or a shell as the instigator); "" otherwise
  // (a damage-over-time pulse after the first, a death weapon, a crash, a script's own Damage call).
  std::string weapon;
  std::string projectile;            // "" for a beam's hit and when not known
  bool beam = false;                 // a collision beam's hit
  std::string damageType;            // as the damage call got it ('Normal', 'Fire', 'Deathnuke', 'TreeForce', ...)
  float amount = 0;                  // after the target's armour: what OnDamage gets
  // Where: the damage call's origin. A shell's direct hit: the shell where it struck (FAF moves it by the target's
  // velocity when the shell has a radius); an area's: the blast's centre (radius > 0; the target may stand off it).
  float position[3] = {0, 0, 0};
  float radius = 0;                  // 0 Damage; DamageArea's radius; DamageRing's outer radius
  // The shot's direction, a unit vector: the shell's velocity as it struck, the beam's axis, else from the origin
  // to the target's position (what OnDamage gets as its vector, made unit length); 0 0 0 when that is 0.
  float direction[3] = {0, 0, 0};
};

// An impact: each time a shell's or a beam's OnImpact(type, entity) runs (Projectile::Impact 0x69dec0, the beam's
// CheckCollision 0x6732d0), misses included, and a dead unit's OnImpact as its wreck hits the ground or the water.
struct ImpactEvent {
  enum Kind { Projectile, Beam, Crash } kind = Projectile;
  // As the simulation decides it (ENT_GetImpactType 0x67b240; the names are EImpactType's, 0x509f90): 'Terrain',
  // 'Water', 'Air' (a miss: lifetime run out, or a beam that met nothing), 'Underwater', 'Projectile',
  // 'ProjectileUnderwater', 'Prop', 'Shield', 'Unit', 'UnitAir', 'UnitUnderwater'. A crash: 'Terrain' or 'Water'.
  std::string type;
  uint32_t source = 0xffffffff;  // the shell, the beam, or the crashing unit
  int army = 0;                  // its army, 1-based, 0 = none
  std::string projectile;        // the shell's blueprint id; a crash: the unit's blueprint id; a beam: ""
  std::string weapon;            // the weapon that fired it (HitEvent::weapon's form), "" when not known
  uint32_t entity = 0xffffffff;  // what it met (a shield: the shield entity itself), none for a miss
  float position[3] = {0, 0, 0};   // where it struck (a crash: where the unit comes to the surface)
  float direction[3] = {0, 0, 0};  // a unit vector: the shell's velocity, the beam's axis, the crash's last step
  float speed = 0;                 // the shell's / the falling unit's speed as it struck (units a second); beam 0
};

// A health change: each time an entity's health fraction changes (Entity::AdjustHealth 0x679860 / SetHealth
// 0x679940: damage taken, regeneration, repair, building, a script's SetHealth), and when a script sets its maximum
// (SetMaxHealth). One entity can change several times in a beat: each is listed, the last is where it stands.
// A new unit is not listed (UnitView::health has its start); a unit under construction is, as it grows.
struct HealthEvent {
  uint32_t entity = 0xffffffff;
  int kind = 0;           // as HitEvent::targetKind: 0 unit, 1 shield, 2 prop, 3 projectile, 4 other entity
  float fraction = 0;     // health / max health after it (0 when the max is 0; SetMaxHealth does not clamp: can be > 1)
  float before = 0;       // the same before it
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
// The simulation's category sets that WeaponView::disallow / onlyAllow name (ui request 37), each distinct set
// once: it only grows, and an index always names the same set. CategorySetHas: whether the blueprint with that
// categoryIndex (UnitView::categoryIndex, CategoryIndexOf) is in set `set`; false for -1, an unknown set or index.
// CategorySets: the sets from index `from` on, appended to out, for a host that tests the bits itself: the
// blueprint with category index i is bit (i & 63) of word i >> 6. CategoryIndexOf: the category index of any
// unit, prop or projectile blueprint by its id (as UnitView::blueprint names it), -1 = none.
bool CategorySetHas(const Sim& sim, int set, int categoryIndex);
void CategorySets(const Sim& sim, size_t from, std::vector<std::vector<uint64_t>>* out);
int CategoryIndexOf(const Sim& sim, const std::string& blueprintId);
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
  // Scrolling (RMeshBlueprintLOD+0xc8 -> MeshLOD+0xac, MeshLOD::Load 0x7dd025 -> the batch +0x3c, 0x7e712b): the
  // mesh's texture scrolls by the unit's UnitView::scroll; it is the instance's anim.w (255) in mesh.fx
  bool scrolling = false;
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
// The camera shakes since the last call; the list is then empty again. Take it once per beat, after Tick(), with
// Sim::collectShakes set from before the first beat.
void ShakeEvents(const Sim& sim, std::vector<ShakeEvent>* out);
// (The scenario info for SessionGetScenarioInfo is Sim::sessionScenario: the launched table as bytes.)

}  // namespace moho
