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
#include <algorithm>
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
class WorkerPool;

struct PathCell {
  int x = 0, z = 0;
  bool operator==(const PathCell& o) const { return x == o.x && z == o.z; }
  bool operator!=(const PathCell& o) const { return !(*this == o); }
};

// One searcher (CAiPathFinder): a land navigator owns one.
struct PathTraveler {
  Unit* unit = nullptr;
  NamedFootprint fp;  // the cluster map's (CAiPathFinder::GetFootprint 0x5a9d30: unit+0x124 only)
  // the grids of the unit's Footprint and AltFootprint: CanTraverseCell's level-0 test takes Entity::GetFootprint
  // as it is when the cell is tested (the AltFootprint while unit+0x124 or +0x125 is set)
  const PathGrid* fitGrid[2] = {nullptr, nullptr};
  int S = 1;  // max(SizeX, SizeZ)
  PathCell anchor;
  int goal[4] = {0, 0, 0, 0};   // half-open rect x0, z0, x1, z1
  int inner[4] = {0, 0, 0, 0};  // excluded rect
  bool goalReachable = true;    // some cell on the goal ring fits
  bool startStandable = true;
  bool dropSub = false;  // the unit is on the Water layer: the cell tests take Sub from its caps (0x5aa788)
  bool ignoreStructures = false;  // CanPathTo
  int mode = 0;                   // 0 none, 1/2 units block, 3 attack variant
  std::deque<std::array<int, 4>> boxes;  // recent start boxes (mode != 0), newest last
  bool useWholeMap = false, insidePlayable = true;
  bool pending = false;
  int armyIndex = 0;
  // The army queue it waits in (an intrusive list, as the original's: QueueSearch 0x5aa318..0x5aa345 unlinks the
  // finder's node, +0x4, and links it at the tail of the queue at +0x2c), or whose active search is its own.
  PathTraveler* qPrev = nullptr;
  PathTraveler* qNext = nullptr;
  bool queued = false, searching = false;
  int queuedIn = 0, searchingIn = 0;  // (the queues_ keys)
  // the delivered path (start cell first) and whether the goal was reached
  std::vector<PathCell> result;
  bool accepted = false;
  // set by the queue when the search finished; the navigator consumes it
  bool delivered = false;
  // HPathTables::PrepareSearches' memory of the traveller: the cluster map FindMap gave for the footprint prepFp
  // (prepFound), and the box of 32-clusters (prepBox, of the map prepScanned) it last scanned for clusters to make
  // ahead, at the tables' pick epoch prepEpoch (0: none yet)
  const void* prepFound = nullptr;
  NamedFootprint prepFp;
  const void* prepScanned = nullptr;
  uint64_t prepEpoch = 0;
  int prepBox[4] = {0, 0, 0, 0};
  // HPathTables::SearchesAhead: the round (beat) and the item its search was played ahead as
  uint32_t aheadRound = 0xffffffffu, aheadItem = 0;
};

// MOHO64_PAR_PATH: set on a worker while it plays a search ahead; a step that would write shared state (Ready of a
// dirty or stale cluster, a map or grid made, the unit grid made) throws PathAheadStop instead.
extern thread_local bool tl_pathAheadDry;
struct PathAheadStop {
  int why = 0;  // 1 a dirty cluster, 2 a stale one, 3 no map or grid, 4 the unit grid due, 5 a category cache
};

class HPathTables {
 public:
  HPathTables(Navigation* nav, int mapW, int mapH);
  ~HPathTables();
  // The sim's playable rect (Sim::playableRect, x0 z0 x1 z1), read live by EdgeAllowed as IsInBounds 0x5aa680 reads
  // the STIMap's; null = the whole map.
  const int* playRect = nullptr;
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
  // MOHO64_PAR_PATH (multicore, the path searches; core/speculate.h), just before WorkAll (after PrepareSearches):
  // every army's carried search (on a copy, with the 2500 work units it will have) and its next waiting searches (no
  // limit) played on `pool` by the same code, each up to the first step that would write shared state (a dirty or
  // stale cluster, a map or grid not made yet, a due unit grid), that step left undone. WorkAll takes a search's
  // result when its army's work units left cover the steps: the serial loop would have made the same steps from the
  // same state, as nothing in the loop changes a clean cluster, a unit or a grid. A stopped one goes on serially.
  void SearchesAhead(WorkerPool& pool, uint32_t round);
  // Synchronous search (CanPathTo's direct probe, 500 units).
  bool SearchNow(PathTraveler* t, int budget);
  // Multicore step 2a, before WorkAll: what the queued searches would make lazily, made now: the occupancy grids
  // of their cluster maps and footprints, and with margin > 0 the data of the dirty or stale clusters within
  // `margin` 32-clusters of each search's start-goal box (ClusterMap::ahead); with `all` also the data of every
  // stale cluster of the cluster maps that have a search (paid by the background, its data not made yet). The data
  // is made on `pool` (null: here): the 8-clusters, then the 32-clusters. Changes nothing a search computes or is
  // charged. True when a queued search is blocked by units (the caller makes the unit grid then). The grids must
  // have no pending terrain changes (Navigation::ApplyTerrainChanges first): the stage reads them on several threads.
  // `cool` (with `all`): the sweep of stale clusters leaves out those DirtyRect dirtied within the last `cool` calls
  // (beats); 0: none left out.
  bool PrepareSearches(int margin, bool all = false, WorkerPool* pool = nullptr, int cool = 0);
  // The heightfield vertices [vx0, vx1] x [vz0, vz1] changed (FlattenMapRect): data made ahead there is dropped
  // (the clusters' own accounting is not touched: the original does not dirty them for it).
  void TerrainChanged(int vx0, int vz0, int vx1, int vz1);
  // Called BEFORE the occupancy grids change for the origins [ox0, ox1] x [oz0, oz1] of a map whose footprint is
  // sx x sz (cells: the origins whose footprint covers them; heights: TerrainChanged's origins): every 8-cluster
  // whose 9x9 origin bitmap holds one of them and that the background has paid for (stale) gets its data made now,
  // from the grid as it still is; a dirty one made ahead drops that data. The original makes a cluster's data when
  // it pays for it (WorkOnCluster 0x8e37d0: the bitmap fetch through the grid's vtable 0x8e388f and the build at
  // once), and a change that does not dirty a cluster leaves its data as it was: DirtyRect 0x8e3620 widens the rect
  // by the footprint only on its far side (x1 + sx + 1), so an origin left of or below a new structure, whose
  // footprint now overlaps it, keeps its old bit in a cluster built before. Seen (probe mp1_9785c): MP1 9752, army
  // 6's cruiser 0x600016 (Water6x6) searches past the naval factory 0x60005e (placed at 9287, cells (939,557)-(943,570),
  // so origins x 934..942 blocked): the original's g at (928,564) is 26.414 + 35.3137 from (960,572), class 0, as its
  // 8-clusters at x 928..936 were built before 9287; ours made them at 9752 with the factory in and took the class-1
  // detour 41.718, so its path ran via (928,572).
  void BeforeOccupancyChange(int cx0, int cz0, int cx1, int cz1);  // cells [cx0, cx1) x [cz0, cz1)
  void BeforeTerrainChange(int vx0, int vz0, int vx1, int vz1);    // vertices [vx0, vx1] x [vz0, vz1]
  // MOHO64_PROFILE: the pre-pass's clusters (made ahead, taken by a search, dropped before), the clusters
  // searches made themselves, and the pre-pass's time.
  struct AheadCounts {
    uint64_t made = 0, taken = 0, dropped = 0, lazy = 0, lazyInWork = 0;  // (lazyInWork: of lazy, in WorkAll)
    double seconds = 0;
  } aheadCounts;
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
    // 1: the cluster is dirty or stale and lv holds its data made ahead (PrepareSearches), as Build / Make would
    // make it now. Ready / Make take it in place of building (the accounting as before); DirtyRect and
    // TerrainChanged drop it. (Searches never read a dirty or stale cluster's lv before building it, so lv may hold
    // the new data early.)
    std::vector<uint8_t> ahead[3];
    std::vector<uint8_t> picked[3];  // (PrepareSearches: on its list of clusters to make; cleared when made)
    // PrepareSearches' count (prepBeat_) + 1 when DirtyRect last dirtied the cluster; 0: never since the map was made
    std::vector<uint32_t> dirtiedAt[3];
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
  // A search's cell -> node index: open addressing with linear probing, an entry counting only with the table's
  // current generation, so that clear() is one increment. (It replaced a std::unordered_map, whose allocation per
  // node and hashing were ~11 s of B1's path searches at cores. The index is only found, set and cleared, never
  // walked, so nothing depends on its order.)
  class CellIndex {
   public:
    void clear() {
      count_ = 0;
      if (++gen_ == 0) {  // (wrapped: every old stamp could match again)
        std::fill(gens_.begin(), gens_.end(), 0u);
        gen_ = 1;
      }
    }
    int Find(uint32_t key) const {
      if (gens_.empty()) return -1;
      for (size_t i = Slot(key);; i = (i + 1) & mask_) {
        if (gens_[i] != gen_) return -1;
        if (keys_[i] == key) return vals_[i];
      }
    }
    void Set(uint32_t key, int val) {
      if ((count_ + 1) * 2 > gens_.size()) Grow();
      for (size_t i = Slot(key);; i = (i + 1) & mask_) {
        if (gens_[i] != gen_) {
          gens_[i] = gen_;
          keys_[i] = key;
          vals_[i] = val;
          ++count_;
          return;
        }
        if (keys_[i] == key) {
          vals_[i] = val;
          return;
        }
      }
    }

   private:
    size_t Slot(uint32_t key) const { return (key * 0x9E3779B1u) >> shift_ & mask_; }
    void Grow() {
      std::vector<uint32_t> oldKeys = std::move(keys_), oldGens = std::move(gens_);
      std::vector<int> oldVals = std::move(vals_);
      const size_t n = oldGens.empty() ? 1024 : oldGens.size() * 2;
      int bits = 0;
      while ((size_t{1} << bits) < n) ++bits;
      shift_ = 32 - bits;
      mask_ = n - 1;
      keys_.assign(n, 0);
      vals_.assign(n, 0);
      gens_.assign(n, 0);
      const uint32_t g = gen_;
      gen_ = 1;
      count_ = 0;
      for (size_t i = 0; i < oldGens.size(); ++i)
        if (oldGens[i] == g) Set(oldKeys[i], oldVals[i]);
    }
    std::vector<uint32_t> keys_, gens_;
    std::vector<int> vals_;
    uint32_t gen_ = 1;
    size_t count_ = 0, mask_ = 0;
    int shift_ = 32;
  };
  struct Search {
    PathTraveler* t = nullptr;
    ClusterMap* cm = nullptr;
    std::vector<Node> nodes;
    CellIndex index;
    std::vector<std::pair<float, int>> heap;
    int best = 0;
    float bestH = 0;
    int expansions = 0, cap = 0;
  };
  struct ArmyQueue {
    PathTraveler* head = nullptr;  // the waiting travelers, oldest first (PathTraveler::qNext)
    PathTraveler* tail = nullptr;
    std::unique_ptr<Search> active;
    std::unique_ptr<Search> spare;  // (the last finished search, made again by Begin: its vectors keep their capacity)
  };
  static void Link(ArmyQueue& q, int key, PathTraveler* t);
  static void Unlink(ArmyQueue& q, PathTraveler* t);

  ClusterMap& Map(const NamedFootprint& fp);
  ClusterMap* FindMap(const NamedFootprint& fp);  // (no new map: its place in mapOrder_ is the first search's)
  bool TakeAhead(ClusterMap& m, int L, size_t i);  // the cluster's data made ahead, if any (dropped from ahead)
  void MakeAhead(ClusterMap& m, int L, int cx, int cz);
  // PrepareSearches' list: the cluster and, for a 32-cluster, its dirty or stale 8-clusters, as MakeAhead would make
  // them (not those made ahead already or listed).
  void PickAhead(ClusterMap& m, int L, int cx, int cz);
  void PrepareTraveler(PathTraveler& t, ClusterMap* cm, int margin);
  struct AheadItem {
    ClusterMap* m;
    int cx, cz;
    size_t i;
  };
  std::vector<AheadItem> pick_[3];  // (PrepareSearches' lists by level, kept between beats for their capacity)
  uint32_t prepBeat_ = 0;           // PrepareSearches' calls (one a beat): the clock of ClusterMap::dirtiedAt
  // Counts every change that can make a cluster pickable for data made ahead (PickAhead: dirty or stale, neither made
  // ahead nor picked): a dirtied rect, data made ahead dropped, a new map. A traveller's box scanned since the last
  // one holds nothing new to pick (PathTraveler::prepEpoch).
  uint64_t pickEpoch_ = 1;
  bool Ready(ClusterMap& m, int L, int cx, int cz, int* budget, bool make = true);
  void Make(ClusterMap& m, int L, int cx, int cz);
  void MakeStaleOrigins(ClusterMap& m, int ox0, int oz0, int ox1, int oz1);  // (BeforeOccupancyChange)
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
  bool inWork_ = false;  // (WorkAll runs: for aheadCounts.lazyInWork)
  struct PathSpec;
  std::unique_ptr<PathSpec> spec_;  // MOHO64_PAR_PATH
  ClusterMap* DryMap(const NamedFootprint& fp);
  void PlayAhead(Search& s, const Search* from, PathTraveler* t, int start, int* steps, int* r, int* why = nullptr);
  bool SameSearch(const Search& a, const Search& b, std::string* why) const;
};

// Octile distance in the original's float order (max + 0.41421354 * min).
float PathOctile(int dx, int dz);

}  // namespace moho
