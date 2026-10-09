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
  enum class Kind { Entity, Unit, Prop, Projectile, Shield, Blip, Beam };
  Kind kind = Kind::Entity;
  uint32_t id = 0;      // the game's entity id (GetEntityId; reused after a 99-beat quarantine)
  uint32_t handle = 0;  // the engine's own reference, never reused (EntityRef)
  const BlueprintInfo* blueprint = nullptr;
  Army* army = nullptr;
  Vec3 position;
  Quat orientation;
  float scale[3] = {1, 1, 1};
  const Skeleton* skeleton = nullptr;  // bones of the mesh (nullptr: no mesh; only bone 0, the entity itself)
  float meshScale = 1;                 // Display.UniformScale: model units -> world
  float health = 0, maxHealth = 0;
  float fractionComplete = 1;  // units under construction; props being reclaimed
  bool dead = false;
  bool destroyQueued = false;  // Destroy() was called; OnDestroy runs when the sim processes the queue
  CollisionShape shape;
  // Extra local rotation per bone this tick (aim controllers; empty: rest pose). Reset every tick
  // before the unit's manipulators run (sim/combat.cpp).
  std::vector<Quat> poseRot;
  uint32_t attachParent = 0;  // entity attached to (beams to their unit; sim/combat.cpp)
  int attachBone = -1;
  uint32_t lastMoveTick = 0;  // the tick its position last changed (aim lead, FAF patch)
  int shooters = 0;           // weapons targeting it (DesiredShooterCap)
  std::shared_ptr<EntityIntel> intel;  // intel circles (sim/intel.cpp; InitIntel / unit blueprints)
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
