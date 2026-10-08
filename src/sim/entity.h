// Entities: everything that exists in the world (units, props, projectiles, ...).
#pragma once
#include <cstdint>
#include <string>

#include "sim/script_object.h"

namespace moho {

struct BlueprintInfo;
class Army;

struct Vec3 {
  float x = 0, y = 0, z = 0;
};
struct Quat {  // x, y, z, w
  float x = 0, y = 0, z = 0, w = 1;
};

class Entity : public ScriptObject {
 public:
  enum class Kind { Entity, Unit, Prop, Projectile, Shield, Blip, Beam };
  Kind kind = Kind::Entity;
  uint32_t id = 0;
  const BlueprintInfo* blueprint = nullptr;
  Army* army = nullptr;
  Vec3 position;
  Quat orientation;
  float scale[3] = {1, 1, 1};
  float health = 0, maxHealth = 0;
  bool dead = false;
};

}  // namespace moho
