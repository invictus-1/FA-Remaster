// Entities: everything that exists in the world (units, props, projectiles, ...).
#pragma once
#include <cstdint>
#include <string>

#include "sim/script_object.h"

namespace moho {

struct BlueprintInfo;
class Army;
class Skeleton;

struct Vec3 {
  float x = 0, y = 0, z = 0;
};
struct Quat {  // x, y, z, w
  float x = 0, y = 0, z = 0, w = 1;
};

class Entity : public ScriptObject {
 public:
  Entity() { typeBits |= kTypeEntity; }
  enum class Kind { Entity, Unit, Prop, Projectile, Shield, Blip, Beam };
  Kind kind = Kind::Entity;
  uint32_t id = 0;
  const BlueprintInfo* blueprint = nullptr;
  Army* army = nullptr;
  Vec3 position;
  Quat orientation;
  float scale[3] = {1, 1, 1};
  const Skeleton* skeleton = nullptr;  // bones of the mesh (nullptr: no mesh; only bone 0, the entity itself)
  float meshScale = 1;                 // Display.UniformScale: model units -> world
  float health = 0, maxHealth = 0;
  bool dead = false;
  bool destroyQueued = false;  // Destroy() was called; OnDestroy runs when the sim processes the queue
};

template <>
struct ScriptTypeOf<Entity> {
  static constexpr uint32_t bit = kTypeEntity;
};

}  // namespace moho
