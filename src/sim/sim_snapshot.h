// The sync point's copies for an interface: what the placement test under the cursor and the reclaim cursor read of
// the simulation, copied at the end of every beat, answered from any thread at any time without waiting for a beat
// (ui nowait report, 2026-11-03: the last two waits of the interface's calls on the running simulation).
//
// How a host uses it:
//   sim.keepSnapshots = true;               // before Start (or any time: the next beat's end makes the first copy)
//   SyncSnapshots* snaps = &sim.snapshots(); // made with the Sim, never moves: keep the pointer
//   ... any thread, any time:
//   if (auto p = snaps->Placement()) BuildSiteOf(*p, query, site, &canBuild);
//   if (auto r = snaps->Reclaim()) ReclaimableAt(*r, at, within, &prop);
// The Sim calls Update itself at the end of Start (after the setup's Sync), of every Tick and of every PausedBeat
// (after the beat's Sync), on the thread that runs the beat, so the copies are those of the sync point the
// interface's other copies are made at (UserState::CopyFromSimulation). Nothing else calls Update.
//
// Threads: Update builds the new snapshots from the last ones (only what changed is copied, the rest is shared) and
// publishes them with a pointer swap under a mutex that the swap alone holds; Placement() / Reclaim() take a copy of
// the pointer under the same mutex. The beat never holds it (Update takes it only after the beat's work, for the
// swap). A query then reads the immutable snapshot with no lock; a snapshot held stays valid and unchanged however
// many beats later, and is freed with its last holder.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "sim/sim_view.h"  // ReclaimableView

namespace moho {

class Sim;
class PlacementSnapshot;  // immutable once published (sim/sim_snapshot.cpp)
class ReclaimSnapshot;    // immutable once published

class SyncSnapshots {
 public:
  SyncSnapshots();
  ~SyncSnapshots();
  SyncSnapshots(const SyncSnapshots&) = delete;
  SyncSnapshots& operator=(const SyncSnapshots&) = delete;
  // Between beats only, on the thread that runs them (the Sim calls it: see above).
  void Update(Sim& sim);
  // Any thread, any time: the last published (null before the first Update).
  std::shared_ptr<const PlacementSnapshot> Placement() const;
  std::shared_ptr<const ReclaimSnapshot> Reclaim() const;
  uint32_t tick() const { return tick_.load(std::memory_order_acquire); }  // the sim's tick at the last Update
  // What the copies cost (for measurements; Update's thread writes, any may read a torn mix): Update's last, longest
  // and total time, the number of Updates, and the bytes the last published snapshots hold (shared parts counted
  // once), and how many of their parts the last Update made anew.
  struct Costs {
    double lastMs = 0, maxMs = 0, totalMs = 0, firstMs = 0;
    uint64_t updates = 0;
    size_t placementBytes = 0, reclaimBytes = 0;
    size_t structures = 0, reclaimables = 0;
    size_t heightBandsCopied = 0, occBandsCopied = 0, cellsCopied = 0;
    double unitsMsTotal = 0, reclaimMsTotal = 0;  // (of totalMs: the units list, the reclaim copy)
    double scanMsTotal = 0;                       // (of reclaimMsTotal: the walk over the props)
    size_t propsMissed = 0;  // (MOHO64_SNAPSHOT_CHECK=1) prop changes Sim::NotePropTouched was not told of
  };
  Costs costs() const { return costs_; }

 private:
  struct Builder;  // Update's own state (only Update touches it)
  std::unique_ptr<Builder> b_;
  mutable std::mutex m_;
  std::shared_ptr<const PlacementSnapshot> placement_;
  std::shared_ptr<const ReclaimSnapshot> reclaim_;
  std::atomic<uint32_t> tick_{0};
  Costs costs_;
};

// ---- the placement test under the cursor --------------------------------------------------------------------------
// What the interface has at the cursor. heading and army are taken as the interface has them; the simulation's test
// (CAiBrain::CanBuildStructureAt with relation 3: every army's structures count; structures stand unturned) reads
// neither, so today they change nothing.
struct PlacementQuery {
  std::string blueprint;          // a unit blueprint id ("ueb0101"; any case, as BuildSiteOf takes it)
  float position[3] = {0, 0, 0};  // the cursor's world position
  float heading = 0;              // (not read by the test)
  int army = 0;                   // 1-based (not read by the test)
};
// Exactly BuildSiteOf(Sim&, ...) of sim/sim_view.h as it answered at the snapshot's sync point: the same code
// (sim/placement_test.h) on the copies. False: no such unit blueprint. site: where the structure would stand (its
// footprint snapped to the cell grid, y on the ground, the water or the sea bed); *canBuild: the placement test there.
// Runs in the simulation's floating-point state (core/fpenv.h), whatever the calling thread's.
bool BuildSiteOf(const PlacementSnapshot& snap, const PlacementQuery& q, float site[3], bool* canBuild);
uint32_t SnapshotTick(const PlacementSnapshot& snap);  // the sim's tick it was made at

// ---- the reclaim cursor ---------------------------------------------------------------------------------------------
// ReclaimableAt(Sim&, ...)'s choice at the snapshot's sync point (the props of a RECLAIMABLE blueprint with a script
// object, not destroy-queued; box = SizeX x SizeZ times the prop's scale; nearest edge within `within`, then nearer
// centre, then the props' order), from props' current places. out->id, blueprint, position, sizeX / sizeZ as the
// live call; out->mass and out->energy are -1: not copied (they live in the prop's script table; the live call
// keeps them).
bool ReclaimableAt(const ReclaimSnapshot& snap, const float at[3], float within, ReclaimableView* out);
uint32_t SnapshotTick(const ReclaimSnapshot& snap);

}  // namespace moho
