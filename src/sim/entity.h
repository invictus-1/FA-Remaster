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
  float scale[3] = {1, 1, 1};
  // What a host that draws the game is told of the entity (sim/sim_view.h); the simulation reads none of it:
  // Entity:SetMesh's mesh blueprint ("" = its own blueprint's), Entity:SetDrawScale, and SetVizToFocusPlayer /
  // Allies / Enemies / Neutrals (0 never set, 1 'Always', 2 'Never', 3 'Intel').
  std::string mesh;
  float drawScale = 1;
  uint8_t viz[4] = {0, 0, 0, 0};
  const Skeleton* skeleton = nullptr;  // bones of the mesh (nullptr: no mesh; only bone 0, the entity itself)
  float meshScale = 1;                 // Display.UniformScale: model units -> world
  float health = 0, maxHealth = 0;
  float fractionComplete = 1;  // units under construction; props being reclaimed
  bool dead = false;
  bool destroyQueued = false;  // Destroy() was called; OnDestroy runs when the sim processes the queue
  CollisionShape shape;
  // The collision primitive follows its entity in Entity::AdvanceCoords 0x678f10 only (step 13, vt+0x84): once
  // the entity has moved in a beat (Sim::HoldShape) the shape still stands at the transform the beat began with
  bool shapeHeld = false;
  Vec3 shapePos;
  Quat shapeOri;
  // (CollisionOffsetX, SizeY / 2 + CollisionOffsetY, CollisionOffsetZ) of the blueprint: the point "bone -1" of
  // Entity::GetBoneWorldTransform 0x679ce0 (what a weapon aims at on a unit without target bones)
  Vec3 bpCenter;
  // This beat's pose (CAniPose): every bone's local transform, reset to the skeleton's rest values each beat
  // (UpdateBones 0x54b5f0, positions x the mesh scale) and then changed by the unit's manipulators in turn.
  // Empty: the rest pose.
  std::vector<Quat> poseQ;
  std::vector<Vec3> poseP;
  Vec3 attachOffset;          // Entity:SetParentOffset: where it sits relative to the bone it is attached to
  uint32_t attachParent = 0;  // entity attached to (beams to their unit; sim/combat.cpp)
  int attachBone = -1;
  uint32_t lastMoveTick = 0;  // the tick its position last changed (aim lead, FAF patch)
  int shooters = 0;           // weapons targeting it (DesiredShooterCap)
  std::shared_ptr<EntityIntel> intel;  // intel circles (sim/intel.cpp; InitIntel / unit blueprints)
  // the entity grid (sim/entity_grid.h): its cell span (w or h 0: in no cell), the dedup mark
  uint16_t gridX0 = 0, gridZ0 = 0, gridW = 0, gridH = 0;
  bool gridMark = false;
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
