// Land steering internals shared by sim/motion.cpp and sim/steering.cpp (engine-ref
// land_motion_blocking.md, unit_collision.md).
#pragma once
#include <cstdint>
#include <vector>

#include "sim/entity.h"

namespace moho {

class Sim;
class Unit;
struct UnitMotion;

// CAiSteeringImpl::Stop: the spline and the collision record go.
void SteeringStop(UnitMotion& m);

// CAiSteeringImpl::UpdatePath 0x5d3680: clears the collision record, then a new batch of points
// toward `tgt` (CAiPathSpline::Generate 0x5b2ff0 for modes 0/1/2; Update 0x5b26c0, braking to a
// stop, for modes 3/4). fresh: from the unit's position and velocity; else from the last point.
void UpdatePath(Sim& sim, Unit* u, const Vec3& tgt, bool fresh, int mode);
// Unit::IsAtPosition 0x62cc40: the footprint-origin cells of the unit and of q are equal.
bool AtPosition(const Unit* u, const Vec3& q);
// A box primitive against an oriented query box with BoxBox 0x474830's own arithmetic (touching counts).
struct WorldShape;
bool BoxPrimitiveMeetsBox(const WorldShape& s, const Vec3& centre, const Vec3 axes[3], const float half[3]);
// An oriented box's AABB as CollectEntitiesInBox 0x721dc0 takes it (0x4fe2d0: its eight corners, then the centre and
// half size of their bounds, 0xa3ae40, and centre -/+ half size).
void OrientedBoxAabb(const Vec3& centre, const Vec3 axes[3], const float half[3], Vec3* mn, Vec3* mx);
// v turned a quarter turn about +y with the quaternion of 0x10b965c through its matrix (0x452d40).
Vec3 RotateQuarterTurnY(const Vec3& v);
// CUnitMotion::AddImpulse 0x6b8ac0 (land): v = v/2 + imp (capped at 0.2 x the max speed); pushed.
void AddImpulse(Unit* u, const Vec3& imp);
// CUnitMotion::ProcessSurfaceCollisionFromLastMove 0x6b9020 + Sim::DoCollisionsFor 0x597cd0.
void ProcessSurfaceCollision(Sim& sim, Unit* u);
// What ProcessSurfaceCollision's search finds before DoCollisionsFor acts on it: the motion's contact list (EntityRefs)
// and the hits (a unit's, or a prop's with its normal), both in the grid's order.
struct SurfaceHit {
  Unit* u;  // a unit's hit, or null for a prop's
  float depth;
  Entity* prop = nullptr;
  Vec3 n{};  // (props: the contact normal handed to OnCollision)
};
struct SurfaceContacts {
  std::vector<uint32_t> contacts;
  std::vector<SurfaceHit> hits;
  // (a search made ahead: the grid query it made and what it found, so that its verifier can tell whether the same
  // query still finds the same entities)
  struct Query {
    Sim* sim = nullptr;
    float x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    uint64_t version = 0;  // the grid's unit lists' version when it ran
    std::vector<Entity*> found;
  } query;
};
// MOHO64_PAR_MOTION (multicore step 3, core/speculate.h): before the entity pass, every unit's search of this beat
// made ahead on the worker pool from the state as the pass begins; at the unit's turn ProcessSurfaceCollision takes it
// if nothing it read has changed since. order = the pass's units (Sim::Tick's motionOrder).
void SurfaceContactsAhead(Sim& sim, const std::vector<Unit*>& order);
// Unit::IsHigherPriorityThan 0x6a8d80: a has the right of way over b.
bool UnitPriority(const Unit* a, const Unit* b);
// The unit filter 0x62eea0: e is no obstacle for self (flags 1: path tests, units that did not move;
// 2: collisions and attack paths).
bool UnitIgnores(const Unit* self, const Unit* e, int flags);
// Unit::IsSameFormationLayerWith 0x6a8d40.
bool SameFormationLayer(const Unit* a, const Unit* b);

}  // namespace moho
