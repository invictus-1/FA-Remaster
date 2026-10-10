// Collision primitives (the original's CColPrimitive<Box3/Sphere3>) and the queries projectiles,
// beams and area damage make against them.
//
// What the original does (FA exe, read 2026-10-08; see FINDINGS "Rebuild M4c"):
// - An entity has at most one primitive: none, an oriented box or a sphere, in its own frame.
//   The default comes from the entity blueprint (Entity::RevertCollisionShape 0x67ae70):
//   CollisionShape (Box by default; projectiles default to None), Size{X,Y,Z} (1) and
//   CollisionOffset{X,Y,Z} (0): a box centred at (OffX, OffY + SizeY/2, OffZ) with half-extents
//   Size/2; a sphere of radius SizeX/2 centred at (OffX, OffY + SizeX/2, OffZ).
//   Entity:SetCollisionShape(type, cx, cy, cz, half-extents or radius) replaces it; 'None'
//   removes it. An entity without a primitive is invisible to line tests and area damage.
// - The world primitive follows the entity transform (centre rotated and moved; box axes are the
//   orientation's). Every test is full 3-D.
// - Segment tests report the first point where the segment enters the shape; a segment that
//   starts inside reports where it leaves; one entirely inside reports nothing.
#pragma once
#include <cstdint>

#include "script/lua.hpp"
#include "sim/entity.h"

namespace moho {

class Sim;

struct WorldShape {
  ShapeType type = ShapeType::None;
  Vec3 c;
  Vec3 ax[3];  // box axes (unit)
  Vec3 half;   // box half-extents
  float r = 0; // sphere radius
};

// The entity's primitive in the world (false: it has none).
bool GetWorldShape(const Entity* e, WorldShape* out);
// Segment p0->p1 against the shape: entry point (or exit point when p0 is inside) and its
// distance from p0.
bool SegmentHit(const WorldShape& s, Vec3 p0, Vec3 p1, Vec3* hit, float* dist);
bool SphereOverlap(const WorldShape& s, Vec3 c, float r);
bool PointInShape(const WorldShape& s, Vec3 p);
void ShapeBounds(const WorldShape& s, Vec3* mn, Vec3* mx);
// CollideBox against an axis-aligned box (touching counts).
bool ShapeOverlapsAABox(const WorldShape& s, Vec3 mn, Vec3 mx);

// The blueprint's shape (Entity::RevertCollisionShape).
void RevertCollisionShape(lua_State* L, Entity* e);
void RegisterCollisionBindings(lua_State* L);

}  // namespace moho
