// Entities: everything that exists in the world (units, props, projectiles, ...).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sim/script_object.h"

namespace moho {

struct BlueprintInfo;
class Army;
class Skeleton;
struct EntityIntel;

struct Vec3 {
  float x = 0, y = 0, z = 0;
};
struct Quat {  // x, y, z, w
  float x = 0, y = 0, z = 0, w = 1;
};

// An entity's collision primitive in its own frame (sim/collision.h).
enum class ShapeType : uint8_t { None = 0, Box = 1, Sphere = 2 };
struct CollisionShape {
  ShapeType type = ShapeType::None;
  Vec3 center;
  Vec3 half;       // box half-extents
  float radius = 0;
};

// An entity's texture scroller (CTextureScroller, 0x3c bytes at entity+0x1f0, made by the first of
// Entity:AddThreadScroller / AddPingPongScroller / AddManualScroller / RemoveScroller, ctor 0x676ba0). Every
// one of those calls hands CTextureScroller::SetScroller 0x777690 a whole SScroller (type, eight numbers,
// sideDist, scrollMult: 0x2c bytes copied over the old one); the other numbers of a call are 0, and its sideDist
// and scrollMult 1 unless it is AddThreadScroller. It is stepped by CTextureScroller::Tick 0x777730 at the very
// start of the entity's own turn (Entity::TaskTick 0x679c51) and only moves the entity's two texture offsets,
// Entity::scroll below, which nothing in the simulation reads (entities.cpp, TextureScrollerTick).
struct TextureScroller {
  enum Type : uint8_t { kNone = 0, kPingPong = 1, kManual = 2, kThread = 3 };
  uint8_t type = kNone;  // +0x04
  // +0x08..+0x24. PingPong: [0] Ping1Speed, [1] Pong1Speed, [2] Ping2Speed, [3] Pong2Speed (seconds a value is
  // held), [4] Ping1, [5] Ping2, [6] Pong1, [7] Pong2 (the values). Manual: [0], [1] added per beat. Thread: unused.
  float f[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  float sideDist = 1, scrollMult = 1;  // +0x28, +0x2c (Thread)
  // PingPong's state per offset (+0x30 bytes, +0x34 ints): which value it holds (true: the ping) and the beats
  // left until it flips. Zeroed by SetScroller for a PingPong only.
  bool ping[2] = {false, false};
  int32_t beatsLeft[2] = {0, 0};
};

class Entity : public ScriptObject {
 public:
  Entity() { typeBits |= kTypeEntity; }
  ~Entity() override;
  enum class Kind { Entity, Unit, Prop, Projectile, Shield, Blip, Beam };
  Kind kind = Kind::Entity;
  uint32_t id = 0;      // the game's entity id (GetEntityId; reused after a 99-beat quarantine)
  uint32_t handle = 0;  // the engine's own reference, never reused (EntityRef)
  const BlueprintInfo* blueprint = nullptr;
  Army* army = nullptr;
  Vec3 position;
  Quat orientation;
  // entity+0x258: the ONE Lua vector Entity:GetPosition() hands out (registry reference, made by the first call)
  int positionRef = LUA_NOREF;
  lua_State* positionState = nullptr;
  // GetPosition's table is let go (at the purge, as ~Entity 0x678635 destroys the LuaObject at entity+0x258)
  void ReleaseScriptPosition();
  float scale[3] = {1, 1, 1};
  // What a host that draws the game is told of the entity (sim/sim_view.h); the simulation reads none of it:
  // Entity:SetMesh's mesh blueprint ("" = its own blueprint's), Entity:SetDrawScale, and SetVizToFocusPlayer /
  // Allies / Enemies / Neutrals (0 never set, 1 'Always', 2 'Never', 3 'Intel').
  std::string mesh;
  float drawScale = 1;
  uint8_t viz[4] = {0, 0, 0, 0};
  // The texture scroller (nullptr until a script asks for one) and the two texture offsets it moves, as the
  // original keeps them in the entity's synced data (SSTIEntityVariableData +0x80..+0x8c = entity+0xf8..+0x104,
  // all 0 at creation, 0x558865): scrollPrev = the offsets before this beat's step (+0xf8, +0xfc), scroll = after
  // it (+0x100, +0x104). The original's mesh renderer draws prev + (cur - prev) x the frame's fraction between
  // two beats (0x7e8276) into the instance's material.z (offset 0) and material.w (offset 1). Only for a host.
  std::unique_ptr<TextureScroller> scroller;
  float scrollPrev[2] = {0, 0};
  float scroll[2] = {0, 0};
  // Unit:HideBone / ShowBone: per bone of the skeleton, 1 = hidden (empty: none ever hidden). Also only for a host.
  std::vector<uint8_t> hiddenBones;
  const Skeleton* skeleton = nullptr;  // bones of the mesh (nullptr: no mesh; only bone 0, the entity itself)
  float meshScale = 1;                 // Display.UniformScale: model units -> world
  float health = 0, maxHealth = 0;
  float fractionComplete = 1;  // units under construction; props being reclaimed
  bool dead = false;
  bool destroyQueued = false;  // Destroy() was called; OnDestroy runs when the sim processes the queue
  // ui REQUESTS 47. The original's entity+0x9a: set by Entity:RequestRefreshUI (0x68bb60) and Entity::Kill (0x679ad8),
  // copied into the beat's sync by Entity::Sync's writer (0x67a290) and cleared (0x67a10b). refreshUI is the pending
  // flag; refreshUISynced is the beat's copy, which the views read (Sim::SyncStep). Nothing in the sim reads either.
  bool refreshUI = false, refreshUISynced = false;
  CollisionShape shape;
  // The collision primitive follows its entity in Entity::AdvanceCoords 0x678f10 only (step 13, vt+0x84): once
  // the entity has moved in a beat (Sim::HoldShape) the shape still stands at the transform the beat began with
  bool shapeHeld = false;
  Vec3 shapePos;
  Quat shapeOri;
  // (CollisionOffsetX, SizeY / 2 + CollisionOffsetY, CollisionOffsetZ) of the blueprint: the point "bone -1" of
  // Entity::GetBoneWorldTransform 0x679ce0 (what a weapon aims at on a unit without target bones)
  Vec3 bpCenter;
  float bpSizeY = 1;  // the blueprint's SizeY (a unit's "bone -1" is half of it above its origin, 0x6aa637)
  // This beat's pose (CAniPose): every bone's local transform, reset to the skeleton's rest values each beat
  // (UpdateBones 0x54b5f0, positions x the mesh scale) and then changed by the unit's manipulators in turn.
  // Empty: the rest pose.
  std::vector<Quat> poseQ;
  // Counts every write of the pose (poseQ / poseP: PoseReset, PoseRotate, a slider's or an animator's bone, a
  // transferred unit's copy): a result made ahead from the pose (core/speculate.h) notes it instead of the bones.
  uint32_t poseVersion = 0;
  std::vector<Vec3> poseP;
  Vec3 attachOffset;          // Entity:SetParentOffset: where it sits relative to the bone it is attached to
  uint32_t attachParent = 0;  // entity attached to (beams to their unit; sim/combat.cpp)
  int attachBone = -1;
  uint32_t lastMoveTick = 0;  // the tick its position last changed (aim lead, FAF patch)
  // The units that took it as a weapon's target (Entity::AddShooter 0x67a130 from UnitWeapon::SetTarget: a set,
  // each unit once, whatever the number of its weapons), by entity handle. A unit leaves it when ONE of its
  // weapons lets go of the entity (0x6d5f89), and the entity's own turn each beat drops the ones that are gone,
  // dead or queued for destruction (0x67a190). Its size is what DesiredShooterCap is compared with.
  std::vector<uint32_t> shooters;
  std::shared_ptr<EntityIntel> intel;  // intel circles (sim/intel.cpp; InitIntel / unit blueprints)
  // The original's EntityAttributes (entity+0x128, synced to the user side's UserEntity+0x100): eight words, a
  // radius each (low 31 bits) with bit 31 = enabled, for Vision, WaterVision, Radar, Sonar, Omni, RadarStealth,
  // SonarStealth, Cloak (the last three hold the stealth / cloak FIELD radii). Only for the view: the sim reads
  // its circles (EntityIntel). Writers in sim/intel.cpp (IntelAttrSlot).
  uint32_t intelAttr[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  // the entity grid (sim/entity_grid.h): its cell span (w or h 0: in no cell), the dedup mark
  uint16_t gridX0 = 0, gridZ0 = 0, gridW = 0, gridH = 0;
  bool gridMark = false;
  // The stage speculation running now (core/speculate.h) knows the entity as its item specItem of round specRound
  // (a beat's tick): set by the stage before its tasks run, read at the entity's serial turn.
  uint32_t specItem = 0, specRound = 0xffffffffu;
  bool gridSeen = false;  // its cells were worked out at least once, for the transform below
  Vec3 gridPos;
  Quat gridOri;
};

// The engine's fields that refer to an entity store EntityRef(e): its handle, unique for the whole
// game (the original keeps weak pointers; its entity ids are reused, so they cannot be references).
// 0 means none. Sim::FindEntity takes a reference; Sim::FindEntityById takes a game id.
inline uint32_t EntityRef(const Entity* e) { return e ? e->handle : 0; }

template <>
struct ScriptTypeOf<Entity> {
  static constexpr uint32_t bit = kTypeEntity;
};

}  // namespace moho
