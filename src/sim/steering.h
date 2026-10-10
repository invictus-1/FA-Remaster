// Land steering internals shared by sim/motion.cpp and sim/steering.cpp (engine-ref
// land_motion_blocking.md, unit_collision.md).
#pragma once
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
// CUnitMotion::AddImpulse 0x6b8ac0 (land): v = v/2 + imp (capped at 0.2 x the max speed); pushed.
void AddImpulse(Unit* u, const Vec3& imp);
// CUnitMotion::ProcessSurfaceCollisionFromLastMove 0x6b9020 + Sim::DoCollisionsFor 0x597cd0.
void ProcessSurfaceCollision(Sim& sim, Unit* u);
// Unit::IsHigherPriorityThan 0x6a8d80: a has the right of way over b.
bool UnitPriority(const Unit* a, const Unit* b);
// The unit filter 0x62eea0: e is no obstacle for self (flags 1: path tests, units that did not move;
// 2: collisions and attack paths).
bool UnitIgnores(const Unit* self, const Unit* e, int flags);
// Unit::IsSameFormationLayerWith 0x6a8d40.
bool SameFormationLayer(const Unit* a, const Unit* b);

}  // namespace moho
