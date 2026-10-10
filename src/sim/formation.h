// Formations (the original's CAiFormationInstance and CAiFormationDBImpl; engine-ref/specs/formations.md).
//
// - A command issued with a formation index (IssueForm*, a carrier launch) to two or more units gets a
//   formation when its first unit dispatches it. The formation script (/lua/formations.lua) lays out the
//   slots per layer class (land/naval, air); each slot takes the nearest remaining unit its category
//   filter allows, in the original's slot order. The formation's centre is the command's target.
// - Every tick (formation DB step, after motion) the travel slots follow each group's leader, and a group
//   is "at goal" when its members stand at their final positions.
// - Before its motion each tick a member reads its slot, leader, path delay and the speed cap:
//   0.85 x the slowest member's top speed, times a catch-up multiplier (0.85 by default).
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "sim/entity.h"

struct lua_State;

namespace moho {

class Sim;
class Unit;
struct UnitCommand;

struct FormNode {
  uint32_t ref = 0;        // the unit (entity handle)
  int order = 0;           // assignment order, 1, 2, ...
  float offX = 0, offZ = 0;  // slot offset from the formation centre (world units)
  Vec3 pos;                // smoothed travel slot (node+0x24)
  bool posSet = false;
  float angle = 0;         // smoothed formation angle (+inf: not set)
  bool angleSet = false;
  float d = 0, rank = 0;   // distance to this tick's slot; rank radius
  float row = 0;           // the slot's 4th field (path delay rows)
  const uint64_t* cats = nullptr;
};

struct FormGroup {
  std::map<uint32_t, FormNode> nodes;  // keyed by entity id
  float cx = 0, cz = 0;                // unit centroid at rebuild (box centre)
  float extX = 0, extZ = 0;            // slot extents
  bool atGoal = false;
  float speed = 0;                     // +inf until set
  bool speedSet = false;
  float mid = 0;                       // mid slot error
  uint32_t leaderCache = 0;            // entity handle
};

struct Formation {
  Sim* sim = nullptr;
  int type = 0;                // command type
  std::string script;
  std::vector<uint32_t> units;  // raw unit list (entity handles, F+0x20 order)
  float cx = 0, cz = 0;         // centre (the command target)
  float qw = 0, qx = 0, qy = 0, qz = 0;  // orientation (all zero: none)
  Vec3 fwd;                     // forward vector (0 when none)
  float aw = 0, ax = 0, ay = 0, az = 0;  // assignment-frame rotation (all zero: none)
  float scale = 1;
  bool dirty = true;
  int largest = 0;              // largest footprint
  std::vector<FormGroup> groups[2];  // [0] non-air, [1] air
  std::map<uint32_t, std::pair<float, float>> adjusted;  // per entity id: adjusted position cache
  struct Reservation {
    float x, z;
    int size, layer;
  };
  std::vector<Reservation> reservations;
  uint32_t rebuilds = 0;        // FormationUpdated broadcasts
  bool released = false;
};

// Lua IssueForm*: the script index of a formation name for these units (-1: unknown).
int FormationScriptIndex(lua_State* L, const std::string& name, const std::vector<Unit*>& units);
// DispatchTask: CUnitCommand::GenerateFormation for u (>= 2 units and an index).
void GenerateFormation(Sim& sim, UnitCommand& c, Unit* u);
// The command's formation holds u (F.Contains(u, 1)).
bool FormationHas(const UnitCommand& c, const Unit* u);
// CUnitCommand::RemoveUnit's formation part (u leaves; the formation goes with its last unit).
void FormationRemoveUnit(UnitCommand& c, Unit* u);
// The final position of u (its formation cell's world centre). False when u has none.
bool FormationGoal(Sim& sim, UnitCommand& c, Unit* u, Vec3* out);
// u's group has reached the goal (FormationAtGoal + IsInFormation(u)).
bool FormationGroupAtGoal(Sim& sim, UnitCommand& c, Unit* u);
// Per tick: the formation DB update (beat step 11).
void FormationsTick(Sim& sim);
// Unit::UpdateInfoCache 0x6a9810 (before the unit's motion): formation fields and the speed cap.
void UpdateInfoCache(Sim& sim, Unit* u);
// u's current formation (Unit::GetFormation), or null.
Formation* GetFormation(Sim& sim, Unit* u);
bool FormationIsForm(const Formation& f);
// Unit::GetFormationVector 0x6a8c20: the formation's forward (an AIR unit following a non-air leader: the
// leader's horizontal forward); zero when none.
Vec3 FormationVector(Sim& sim, Unit* u);
// Unit::UpdateGuardFormation 0x6aa7a0 (MotionTick, on the guarded unit): make its guard formation.
void UpdateGuardFormation(Sim& sim, Unit* G);
// SetGuardedUnit deletes the old and the new guarded unit's guard formation.
void ReleaseGuardFormation(Unit* G);
// GetPathDelay(u, 0): the think delay of a formation member, max(1, row x 10) (1 when not in one).
int FormationPathDelay(Sim& sim, Unit* u);

}  // namespace moho
