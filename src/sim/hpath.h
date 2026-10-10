// Land path search the original's way (FA exe, read 2026-10-09; engine-ref/specs/pathfinding.md):
// the gpg "HaStar" cluster hierarchy and the per-army search queue.
//
// - Every named footprint has a cluster map with two levels: 8x8 and 32x32 cells (clusters share
//   their boundary rows/columns). A level-1 cluster's nodes are one cell per passable run on each
//   side; its edges carry a cost class from the shortest path inside the cluster
//   (cost = octile * exp(class/6)). A level-2 cluster's nodes are its sub-clusters' nodes on the
//   32-cell boundary, edges from a search over the sub-cluster graph.
// - A search is a single A* over cells: plain 8-neighbour steps (1 / 1.414, no corner cutting)
//   only near the start (or the recent start boxes) and the goal, level-1 jump edges inside
//   "interesting" 32-clusters, level-2 jumps elsewhere. Heuristic octile * 1.01; the best node
//   for an unreachable goal is the smallest h at discovery.
// - When the unit does not stand on a cell its footprint fits, the plain steps ignore terrain
//   (CanTraverseCell 0x5aa710); the cluster edges never do.
// - Each army has one queue, served first in first out with 2500 work units per tick (one per
//   expansion, 10 per cluster rebuilt on demand); a search may span ticks. Searches are capped at
//   the army pathcap (by map size), or 1000 expansions when no cell around the goal fits.
#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "sim/blueprints.h"

namespace moho {

class Sim;
class Unit;
class PathGrid;
class Navigation;

struct PathCell {
  int x = 0, z = 0;
  bool operator==(const PathCell& o) const { return x == o.x && z == o.z; }
  bool operator!=(const PathCell& o) const { return !(*this == o); }
};

// One searcher (CAiPathFinder): a land navigator owns one.
struct PathTraveler {
  Unit* unit = nullptr;
  NamedFootprint fp;
  int S = 1;  // max(SizeX, SizeZ)
  PathCell anchor;
  int goal[4] = {0, 0, 0, 0};   // half-open rect x0, z0, x1, z1
  int inner[4] = {0, 0, 0, 0};  // excluded rect
  bool goalReachable = true;    // some cell on the goal ring fits
  bool startStandable = true;
  bool ignoreStructures = false;  // CanPathTo
  int mode = 0;                   // 0 none, 1/2 units block, 3 attack variant
  std::deque<std::array<int, 4>> boxes;  // recent start boxes (mode != 0), newest last
  bool useWholeMap = false, insidePlayable = true;
  bool pending = false;
  int armyIndex = 0;
  // the delivered path (start cell first) and whether the goal was reached
  std::vector<PathCell> result;
  bool accepted = false;
  // set by the queue when the search finished; the navigator consumes it
  bool delivered = false;
};

class HPathTables {
 public:
  explicit HPathTables(Navigation* nav, int mapW, int mapH) : nav_(nav), w_(mapW), h_(mapH) {}
  // PathTables ctor 0x76b8c0: one cluster map per named footprint, in the footprint list's order.
  void CreateMaps(const std::vector<NamedFootprint>& named);
  // Structures changed occupancy in cells [x0, x1) x [z0, z1): dirty the clusters around.
  void DirtyRect(int x0, int z0, int x1, int z1);
  // Background build of dirty clusters (path_BackgroundBudget per beat, shared by all maps in order).
  void UpdateBackground(int budget);
  // Queue a search (QueueSearch 0x5aa310; the traveler's fields are set by the caller).
  void Queue(PathTraveler* t);
  void Cancel(PathTraveler* t);
  // CArmyImpl::OnTick: every army's queue with 2500 work units.
  void WorkAll(int armies);
  // Synchronous search (CanPathTo's direct probe, 500 units).
  bool SearchNow(PathTraveler* t, int budget);
  int ArmyPathcap() const;

  struct Cluster {
    std::vector<uint8_t> nx, nz;  // local node cells
    std::vector<int8_t> cls;      // n x n classes (-1: none)
    int n = 0;
    int8_t Cls(int i, int j) const { return cls[static_cast<size_t>(i) * n + j]; }
  };
  struct ClusterMap {
    const PathGrid* grid = nullptr;
    int S = 1;  // footprint max size (dirty margin)
    int sx = 1, sz = 1;
    int wc[3] = {0, 0, 0}, hc[3] = {0, 0, 0};
    std::vector<Cluster> lv[3];
    std::vector<uint8_t> dirty[3];    // 1: needs a build (every cluster starts dirty)
    // 1: built as far as the budgets are concerned (the background paid for it) but its data is not made yet.
    // A cluster's data depends only on the occupancy grid, and every occupancy change dirties it again, so
    // making it when a search first reads it gives the same data as making it in the background.
    std::vector<uint8_t> stale[3];
    NamedFootprint fp;
    size_t bgWord = 0;     // background iterator: word of the top level's dirty bit array (map+0x8c)
    bool bgDone = false;   // nothing dirty left (map+0x88; DirtyRect clears it)
  };

 private:
  struct Node {
    PathCell c;
    float g = 0, h = 0;
    int parent = -1;
    int heapPos = -1;
    uint8_t state = 0;  // 1 open, 2 closed
  };
  struct Search {
    PathTraveler* t = nullptr;
    ClusterMap* cm = nullptr;
    std::vector<Node> nodes;
    std::unordered_map<uint32_t, int> index;
    std::vector<std::pair<float, int>> heap;
    int best = 0;
    float bestH = 0;
    int expansions = 0, cap = 0;
  };
  struct ArmyQueue {
    std::deque<PathTraveler*> waiting;  // oldest first
    std::unique_ptr<Search> active;
  };

  ClusterMap& Map(const NamedFootprint& fp);
  bool Ready(ClusterMap& m, int L, int cx, int cz, int* budget, bool make = true);
  void Make(ClusterMap& m, int L, int cx, int cz);
  void Build1(ClusterMap& m, int cx, int cz);
  void Build2(ClusterMap& m, int cx, int cz, int* budget);
  void Begin(Search& s, PathTraveler* t);
  int WorkOnce(Search& s, int* budget);  // 0 exhausted, 1 goal, 2 suspended, 3 capped
  void Finish(Search& s, bool accepted);
  int Collect(Search& s, const PathCell& c, std::vector<std::pair<PathCell, float>>* out, int* budget);
  bool AddNeighbours(Search& s, const PathCell& c, int L, std::vector<std::pair<PathCell, float>>* out, int* budget);
  bool ShouldSearchRect(const PathTraveler& t, const int* r) const;
  bool CanTraverse(const Search& s, const PathCell& n) const;
  bool EdgeAllowed(const PathTraveler& t, const PathCell& n) const;
  float Heuristic(const PathTraveler& t, const PathCell& c) const;
  bool IsGoal(const PathTraveler& t, const PathCell& c) const;
  void ClusterRect(int x, int z, int L, int* r) const;
  void HeapUp(Search& s, int i);
  void HeapDown(Search& s, int i);

  Navigation* nav_;
  int w_, h_;
  std::map<std::string, std::unique_ptr<ClusterMap>> maps_;
  std::vector<ClusterMap*> mapOrder_;
  std::map<int, ArmyQueue> queues_;
};

// Octile distance in the original's float order (max + 0.41421354 * min).
float PathOctile(int dx, int dz);

}  // namespace moho
