// Land path search (see hpath.h). Addresses refer to the FA exe; engine-ref/specs/pathfinding.md
// gives the details this follows.
#include "sim/hpath.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "core/workers.h"
#include "sim/navigation.h"
#include "sim/units.h"

namespace moho {

bool PathUnitBlocked(Unit* u, int x, int z, int flags);  // sim/landnav.cpp (COGrid::UnitIsBlocked)

namespace {

constexpr float kOct = 0.41421354f;    // 0xe4f788
constexpr float kDiag = 1.41400003f;   // 0xe35e9c
constexpr float kHScale = 1.01f;       // 0xe4f7c4
constexpr int kSize[3] = {1, 8, 32};
constexpr int kShift[3] = {0, 3, 5};
constexpr int kTop = 2;
// directions (AddNeighbours level 0): (0,-1) (-1,0) (0,+1) (+1,0) (-1,-1) (-1,+1) (+1,+1) (+1,-1)
constexpr int kDX[8] = {0, -1, 0, 1, -1, -1, 1, 1};
constexpr int kDZ[8] = {-1, 0, 1, 0, -1, 1, 1, -1};
constexpr int kNeed[8] = {0, 0, 0, 0, 3, 6, 12, 9};

// The edge cost class multipliers: the constant table at 0xe35da0 (.rdata, read by AddNeighbours 0x7666e3, the level-2
// build's ExpandNode 0x9306e2 and 0x7659f0). exp(k / 6) as the source wrote it, to seven digits (1.18136, 1.395612,
// 1.648721 ...), so eight entries are 1 to 4 float steps off the exact exp (k = 1, 2, 3, 5, 6, 7, 8, 26). Seen:
// Skirmish 1981, army 2's engineer 0x20001d asks a path to (746, 426): two level-2 routes past (828, 384) cost the
// same to the last float step, and with the exact exp the one via (832, 412) came out one step cheaper; the
// original goes via (828, 416) (its navigator takes (828.5, 416.5) at 1982).
float Cls(int k) {
  static const uint32_t bits[32] = {
      0x3f800000, 0x3f9736ce, 0x3fb2a36a, 0x3fd3094a, 0x3ff94f59, 0x40134331, 0x402df855, 0x404d8577,
      0x4072cb75, 0x408f69ff, 0x40a96c76, 0x40c82683, 0x40ec7326, 0x410baa8d, 0x4124ff03, 0x4142eb7f,
      0x4166454a, 0x4188042e, 0x41a0af2e, 0x41bdd379, 0x41e040c5, 0x42047639, 0x421c7c32, 0x4238dd87,
      0x425a6481, 0x4281000c, 0x4298654e, 0x42b408c5, 0x42d4af73, 0x42fb4210, 0x431469c5, 0x432f5456};
  float f;
  std::memcpy(&f, &bits[k], sizeof f);
  return f;
}

// QuantizeEdgeCost 0x92d8b0: ceil(6 ln(dist / octile)), clamped to 0..31 (above 30: 31)
int Quantize(float d, float o) {
  float v = static_cast<float>(0.6931471805599453 * std::log2(static_cast<double>(d) / static_cast<double>(o)) * 6.0);
  float r = std::nearbyint(v);
  int c = static_cast<int>(r) + (r < v ? 1 : 0);
  if (c > 30) c = 31;
  if (c < 0) c = 0;
  return c;
}

uint32_t Key(int x, int z) { return (static_cast<uint32_t>(static_cast<uint16_t>(z)) << 16) | static_cast<uint16_t>(x); }

}  // namespace

float PathOctile(int dx, int dz) {
  float a = static_cast<float>(std::abs(dx)), b = static_cast<float>(std::abs(dz));
  return b <= a ? b * kOct + a : a * kOct + b;
}

// ---- cluster maps ---------------------------------------------------------------------------

namespace {
std::string MapKey(const NamedFootprint& fp) {
  return fp.name + '/' + std::to_string(fp.sizeX) + 'x' + std::to_string(fp.sizeZ) + '/' + std::to_string(fp.caps) +
         '/' + std::to_string(fp.flags & 1);
}
}  // namespace

HPathTables::ClusterMap* HPathTables::FindMap(const NamedFootprint& fp) {
  auto it = maps_.find(MapKey(fp));
  return it != maps_.end() ? it->second.get() : nullptr;
}

HPathTables::ClusterMap& HPathTables::Map(const NamedFootprint& fp) {
  std::string key = MapKey(fp);
  auto it = maps_.find(key);
  if (it != maps_.end()) {
    if (!it->second->grid) it->second->grid = nav_->Grid(it->second->fp);
    return *it->second;
  }
  auto m = std::make_unique<ClusterMap>();
  m->fp = fp;
  m->grid = nav_->Grid(fp);
  m->sx = std::max<int>(1, fp.sizeX);
  m->sz = std::max<int>(1, fp.sizeZ);
  m->S = std::max(m->sx, m->sz);
  int w32 = (w_ + 31) & ~31, h32 = (h_ + 31) & ~31;
  for (int L = 1; L <= 2; ++L) {
    m->wc[L] = w32 >> kShift[L];
    m->hc[L] = h32 >> kShift[L];
    size_t n = static_cast<size_t>(m->wc[L]) * m->hc[L];
    m->lv[L].assign(n, {});
    m->dirty[L].assign(n, 1);
    m->stale[L].assign(n, 0);
    m->ahead[L].assign(n, 0);
    m->picked[L].assign(n, 0);
  }
  ClusterMap* r = m.get();
  mapOrder_.push_back(r);
  maps_[key] = std::move(m);
  return *r;
}

// PathTables::PathTables 0x76b8c0: a ClusterMap for every footprint of the rules' list (index = mIndex),
// all clusters dirty. (Their occupancy grids are made when first needed.)
void HPathTables::CreateMaps(const std::vector<NamedFootprint>& named) {
  for (const NamedFootprint& fp : named) {
    std::string key = fp.name + '/' + std::to_string(fp.sizeX) + 'x' + std::to_string(fp.sizeZ) + '/' +
                      std::to_string(fp.caps) + '/' + std::to_string(fp.flags & 1);
    if (maps_.count(key)) continue;
    ClusterMap& m = Map(fp);
    m.grid = nullptr;
  }
}

void HPathTables::DirtyRect(int x0, int z0, int x1, int z1) {
  for (ClusterMap* m : mapOrder_) {
    m->bgDone = false;  // ClusterMap::DirtyRect 0x8e3620
    int a0 = x0 - 1, b0 = z0 - 1, a1 = x1 + m->sx + 1, b1 = z1 + m->sz + 1;
    for (int L = 1; L <= 2; ++L) {
      int s = kShift[L];
      int ix0 = std::max(0, (a0 - 1) >> s), ix1 = std::min(m->wc[L], ((a1 - 1) >> s) + 1);
      int iz0 = std::max(0, (b0 - 1) >> s), iz1 = std::min(m->hc[L], ((b1 - 1) >> s) + 1);
      for (int cz = iz0; cz < iz1; ++cz)
        for (int cx = ix0; cx < ix1; ++cx) {
          size_t i = static_cast<size_t>(cz) * m->wc[L] + cx;
          m->dirty[L][i] = 1;
          if (m->ahead[L][i]) {  // (made from the occupancy before this change)
            m->ahead[L][i] = 0;
            ++aheadCounts.dropped;
          }
        }
    }
    // Data made ahead is dropped wherever the change can reach a cluster's data, which is wider than the rect the
    // original dirties above (that rect is the original's, ClusterMap::DirtyRect 0x8e3620, and stays as it is: it
    // decides what is rebuilt and charged). A cluster's data is a function of its origins' passability, and the
    // origins whose footprint [o, o + size) overlaps the changed cells [x0, x1) are [x0 - size + 1, x1 - 1]
    // (PathGrid::Block); the dirty rect reaches only 1-2 cells left of (above) x0 (z0), so a footprint wider than
    // that changes origins of 8-clusters it leaves clean or stale. A clean one keeps its data either way (it is not
    // rebuilt, here or in the original). A stale one's data is made when a search first reads it, from the grid as
    // it is then, which holds this change; data made ahead before the change would not, and a search would take it.
    // So the drop covers every 8-cluster holding an origin in that range, and its 32-cluster. (Seen: MP1 differed at
    // 12449 with MOHO64_PATH_AHEAD=2, the serial step 2a and the pooled one alike; identical with this drop.)
    const int ox0 = x0 - m->sx + 1, ox1 = x1 - 1, oz0 = z0 - m->sz + 1, oz1 = z1 - 1;
    const int jx0 = std::max(0, (ox0 >> 3) - 1), jx1 = std::min(m->wc[1] - 1, ox1 >> 3);
    const int jz0 = std::max(0, (oz0 >> 3) - 1), jz1 = std::min(m->hc[1] - 1, oz1 >> 3);
    for (int L = 1; L <= 2; ++L) {
      const int sh = L == 1 ? 0 : 2;
      for (int cz = jz0 >> sh; cz <= (jz1 >> sh) && cz < m->hc[L]; ++cz)
        for (int cx = jx0 >> sh; cx <= (jx1 >> sh) && cx < m->wc[L]; ++cx) {
          const size_t i = static_cast<size_t>(cz) * m->wc[L] + cx;
          if (m->ahead[L][i]) {
            m->ahead[L][i] = 0;
            ++aheadCounts.dropped;
          }
        }
    }
  }
}

void HPathTables::UpdateBackground(int budget) {
  // PathTables::UpdateBackground 0x76bc10 (Sim::AdvanceBeat, before the armies). Only the "/genpath" command
  // line switch builds everything at once; a normal game builds the clusters here, path_BackgroundBudget
  // (1000) per beat shared by the maps in order, or on demand from a search's budget.
  // ClusterMap::UpdateBackground 0x8e3c00: while budget > 0, the next dirty top-level cluster from the
  // saved word of the dirty bit array (0x8d8270: words scanned cyclically from the saved one, which is kept;
  // a word holds 32 clusters of one column, word = (cz >> 5) * wc + cx, bit cz & 31, lowest bit first).
  for (ClusterMap* m : mapOrder_) {
    if (m->bgDone) continue;
    const int wc = m->wc[kTop], hc = m->hc[kTop];
    const size_t nWords = static_cast<size_t>(wc) * ((hc + 31) >> 5);
    while (budget > 0) {
      if (m->bgWord >= nWords) m->bgWord = 0;
      bool found = false;
      int cx = 0, cz = 0;
      for (size_t k = 0; k < nWords && !found; ++k) {
        size_t w = (m->bgWord + k) % nWords;
        int x = static_cast<int>(w % wc), zb = static_cast<int>(w / wc) << 5;
        for (int b = 0; b < 32 && zb + b < hc; ++b)
          if (m->dirty[kTop][static_cast<size_t>(zb + b) * wc + x]) {
            found = true;
            cx = x;
            cz = zb + b;
            m->bgWord = w;
            break;
          }
      }
      if (!found) {
        m->bgDone = true;
        break;
      }
      Ready(*m, kTop, cx, cz, &budget, false);  // (budget accounting only; the data is made when read)
    }
    if (budget <= 0) return;
  }
}

// WorkOnCluster 0x8e37d0. make = false (the background): pay and clear the dirty bit, make the data later.
bool HPathTables::Ready(ClusterMap& m, int L, int cx, int cz, int* budget, bool make) {
  if (cx < 0 || cz < 0 || cx >= m.wc[L] || cz >= m.hc[L]) return true;
  size_t i = static_cast<size_t>(cz) * m.wc[L] + cx;
  if (!m.dirty[L][i]) {
    if (make && m.stale[L][i]) Make(m, L, cx, cz);
    return true;
  }
  if (*budget <= 0) return false;
  if (L == 2)
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k)
        if (!Ready(m, 1, 4 * cx + k, 4 * cz + j, budget, make)) return false;
  if (make) {
    if (!m.grid) m.grid = nav_->Grid(m.fp);
    if (!TakeAhead(m, L, i)) {
      if (L == 1) Build1(m, cx, cz);
      else Build2(m, cx, cz, budget);
    }
  }
  *budget -= 10;
  m.dirty[L][i] = 0;
  m.stale[L][i] = make ? 0 : 1;
  return true;
}

// The data of a clean cluster the background paid for (Ready above).
void HPathTables::Make(ClusterMap& m, int L, int cx, int cz) {
  if (!m.grid) m.grid = nav_->Grid(m.fp);
  if (L == 2)
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k) {
        int sx = 4 * cx + k, sz = 4 * cz + j;
        if (sx >= m.wc[1] || sz >= m.hc[1]) continue;
        size_t si = static_cast<size_t>(sz) * m.wc[1] + sx;
        if (m.stale[1][si] && !m.dirty[1][si]) Make(m, 1, sx, sz);
      }
  const size_t i = static_cast<size_t>(cz) * m.wc[L] + cx;
  if (!TakeAhead(m, L, i)) {
    if (L == 1) Build1(m, cx, cz);
    else Build2(m, cx, cz, nullptr);
  }
  m.stale[L][i] = 0;
}

bool HPathTables::TakeAhead(ClusterMap& m, int L, size_t i) {
  if (!m.ahead[L][i]) {
    ++aheadCounts.lazy;
    if (inWork_) ++aheadCounts.lazyInWork;
    return false;
  }
  m.ahead[L][i] = 0;
  ++aheadCounts.taken;
  return true;
}

// The data of a dirty or stale cluster, made now into lv (ahead = 1). A 32-cluster's sub-clusters that are dirty or
// stale are made first, as both of the serial ways do before Build2 reads them (Ready of a dirty 32-cluster readies
// all 16; Make of a stale one makes its stale ones, and a stale 32-cluster has no dirty one: DirtyRect dirties both
// levels over the same cells).
void HPathTables::MakeAhead(ClusterMap& m, int L, int cx, int cz) {
  if (cx < 0 || cz < 0 || cx >= m.wc[L] || cz >= m.hc[L]) return;
  const size_t i = static_cast<size_t>(cz) * m.wc[L] + cx;
  if (m.ahead[L][i] || !(m.dirty[L][i] || m.stale[L][i])) return;
  if (!m.grid) m.grid = nav_->Grid(m.fp);
  if (L == 2)
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k) MakeAhead(m, 1, 4 * cx + k, 4 * cz + j);
  if (L == 1) Build1(m, cx, cz);
  else Build2(m, cx, cz, nullptr);
  m.ahead[L][i] = 1;
  ++aheadCounts.made;
}

// One queued search: its grids, and (margin > 0) the data of the dirty or stale 32-clusters, with their
// sub-clusters, in the box around its start and its goal, `margin` 32-clusters wider on each side. (Measured, MP1 to
// 7100: with margin 1 the searches found 55% of the clusters they built made ahead, 65% with 2; the rest lay
// outside the box, and a third of what was made ahead was dirtied again before a search reached it. So cluster data
// is better made by the search that first needs it, outside the accounting: ahead / TakeAhead are that split.)
void HPathTables::PrepareTraveler(const PathTraveler& t, ClusterMap* cm, int margin) {
  if (!cm) {
    nav_->Grid(t.fp);  // (the map itself is the first search's to make: its place in mapOrder_; Map() makes it)
    return;
  }
  if (!cm->grid) cm->grid = nav_->Grid(cm->fp);
  if (margin <= 0) return;
  const int x0 = std::min(t.anchor.x, t.goal[0]), x1 = std::max(t.anchor.x, t.goal[2]);
  const int z0 = std::min(t.anchor.z, t.goal[1]), z1 = std::max(t.anchor.z, t.goal[3]);
  const int cx0 = std::max(0, (x0 >> 5) - margin), cx1 = std::min(cm->wc[2] - 1, (x1 >> 5) + margin);
  const int cz0 = std::max(0, (z0 >> 5) - margin), cz1 = std::min(cm->hc[2] - 1, (z1 >> 5) + margin);
  for (int cz = cz0; cz <= cz1; ++cz)
    for (int cx = cx0; cx <= cx1; ++cx) PickAhead(*cm, 2, cx, cz);
}

void HPathTables::PickAhead(ClusterMap& m, int L, int cx, int cz) {
  if (cx < 0 || cz < 0 || cx >= m.wc[L] || cz >= m.hc[L]) return;
  const size_t i = static_cast<size_t>(cz) * m.wc[L] + cx;
  if (m.ahead[L][i] || m.picked[L][i] || !(m.dirty[L][i] || m.stale[L][i])) return;
  if (!m.grid) m.grid = nav_->Grid(m.fp);  // (made here, on this thread: the builds only read it)
  if (L == 2)
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k) PickAhead(m, 1, 4 * cx + k, 4 * cz + j);
  m.picked[L][i] = 1;
  pick_[L].push_back({&m, cx, cz, i});
}

bool HPathTables::PrepareSearches(int margin, bool all, WorkerPool* pool) {
  bool units = false;
  pick_[1].clear();
  pick_[2].clear();
  std::vector<ClusterMap*> searched;  // (the cluster maps with a search, in the work loop's order: for `all`)
  auto note = [&](ClusterMap* cm) {
    if (cm && std::find(searched.begin(), searched.end(), cm) == searched.end()) searched.push_back(cm);
  };
  for (auto& [a, q] : queues_) {  // (the work loop's order: armies, then each queue's active search and the rest)
    if (q.active) {
      PrepareTraveler(*q.active->t, q.active->cm, margin);
      note(q.active->cm);
      units = units || q.active->t->mode != 0;
    }
    for (PathTraveler* t = q.head; t; t = t->qNext) {
      ClusterMap* cm = FindMap(t->fp);
      PrepareTraveler(*t, cm, margin);
      note(cm);
      units = units || t->mode != 0;
    }
  }
  if (all)
    for (ClusterMap* m : searched) {
      for (int cz = 0; cz < m->hc[2]; ++cz)
        for (int cx = 0; cx < m->wc[2]; ++cx)
          if (m->stale[2][static_cast<size_t>(cz) * m->wc[2] + cx]) PickAhead(*m, 2, cx, cz);
      for (int cz = 0; cz < m->hc[1]; ++cz)
        for (int cx = 0; cx < m->wc[1]; ++cx)
          if (m->stale[1][static_cast<size_t>(cz) * m->wc[1] + cx]) PickAhead(*m, 1, cx, cz);
    }
  // The data: each cluster's is a function of its map's occupancy grid (an 8-cluster) or of its 8-clusters' data (a
  // 32-cluster) alone, written to its own lv entry; so the 8-clusters first, then the 32-clusters, each list in any
  // order and on any thread (MakeAhead's data, the same as a search would make: data made ahead is dropped by any
  // occupancy or terrain change before it is read).
  for (int L = 1; L <= 2; ++L) {
    std::vector<AheadItem>& list = pick_[L];
    if (list.empty()) continue;
    auto build = [&](size_t b, size_t e, int /*worker*/) {
      for (size_t k = b; k < e; ++k) {
        const AheadItem& it = list[k];
        if (L == 1) Build1(*it.m, it.cx, it.cz);
        else Build2(*it.m, it.cx, it.cz, nullptr);
      }
    };
    if (pool) pool->ParallelFor(L == 1 ? "path clusters ahead (8)" : "path clusters ahead (32)", list.size(), L == 1 ? 4 : 1, build);
    else build(0, list.size(), 0);
    for (const AheadItem& it : list) {
      it.m->picked[L][it.i] = 0;
      it.m->ahead[L][it.i] = 1;
      ++aheadCounts.made;
    }
  }
  return units;
}

void HPathTables::MakeStaleOrigins(ClusterMap& m, int ox0, int oz0, int ox1, int oz1) {
  // the 8-clusters whose origins [8 cx, 8 cx + 8] x [8 cz, 8 cz + 8] (Build1's 9x9 bitmap) meet the rect
  const int ix0 = std::max(0, (ox0 >> 3) - 1), ix1 = std::min(m.wc[1] - 1, ox1 >> 3);
  const int iz0 = std::max(0, (oz0 >> 3) - 1), iz1 = std::min(m.hc[1] - 1, oz1 >> 3);
  for (int cz = iz0; cz <= iz1; ++cz)
    for (int cx = ix0; cx <= ix1; ++cx) {
      if (8 * cx > ox1 || 8 * cx + 8 < ox0 || 8 * cz > oz1 || 8 * cz + 8 < oz0) continue;
      const size_t i = static_cast<size_t>(cz) * m.wc[1] + cx;
      if (m.dirty[1][i]) {
        if (m.ahead[1][i]) {  // (made from the grid before this change; the original builds it later, from the new one)
          m.ahead[1][i] = 0;
          ++aheadCounts.dropped;
        }
      } else if (m.stale[1][i]) {
        Make(m, 1, cx, cz);
      }
    }
}

void HPathTables::BeforeOccupancyChange(int cx0, int cz0, int cx1, int cz1) {
  for (ClusterMap* m : mapOrder_) MakeStaleOrigins(*m, cx0 - m->sx + 1, cz0 - m->sz + 1, cx1 - 1, cz1 - 1);
}

void HPathTables::BeforeTerrainChange(int vx0, int vz0, int vx1, int vz1) {
  for (ClusterMap* m : mapOrder_) MakeStaleOrigins(*m, vx0 - m->sx, vz0 - m->sz, vx1, vz1);
}

void HPathTables::TerrainChanged(int vx0, int vz0, int vx1, int vz1) {
  for (ClusterMap* m : mapOrder_) {
    // the origins whose footprint covers a changed vertex (PathGrid::TerrainChanged), then every 8-cluster whose
    // 9x9 origins hold one of them, and their 32-clusters
    const int ox0 = vx0 - m->sx, oz0 = vz0 - m->sz, ox1 = vx1, oz1 = vz1;
    const int ix0 = std::max(0, (ox0 >> 3) - 1), ix1 = std::min(m->wc[1] - 1, ox1 >> 3);
    const int iz0 = std::max(0, (oz0 >> 3) - 1), iz1 = std::min(m->hc[1] - 1, oz1 >> 3);
    for (int L = 1; L <= 2; ++L) {
      const int s = L == 1 ? 0 : 2;
      for (int cz = iz0 >> s; cz <= (iz1 >> s) && cz < m->hc[L]; ++cz)
        for (int cx = ix0 >> s; cx <= (ix1 >> s) && cx < m->wc[L]; ++cx) {
          const size_t i = static_cast<size_t>(cz) * m->wc[L] + cx;
          if (m->ahead[L][i]) {
            m->ahead[L][i] = 0;
            ++aheadCounts.dropped;
          }
        }
    }
  }
}

namespace {
// DropUnreachedClusterNodes 0x954650: nodes without an edge go
void Prune(std::vector<uint8_t>* nx, std::vector<uint8_t>* nz, std::vector<int8_t>* cls, int* n) {
  int N = *n;
  std::vector<int> keep;
  for (int i = 0; i < N; ++i) {
    bool any = false;
    for (int j = 0; j < N && !any; ++j)
      if (j != i && (*cls)[static_cast<size_t>(i) * N + j] >= 0) any = true;
    if (any) keep.push_back(i);
  }
  int M = static_cast<int>(keep.size());
  std::vector<uint8_t> ax(M), az(M);
  std::vector<int8_t> c(static_cast<size_t>(M) * M, -1);
  for (int a = 0; a < M; ++a) {
    ax[a] = (*nx)[keep[a]];
    az[a] = (*nz)[keep[a]];
    for (int b = 0; b < M; ++b) c[static_cast<size_t>(a) * M + b] = (*cls)[static_cast<size_t>(keep[a]) * N + keep[b]];
  }
  *nx = std::move(ax);
  *nz = std::move(az);
  *cls = std::move(c);
  *n = M;
}
}  // namespace

// ClusterBuild level 1 (0x9552d0): nodes per passable side run, edges from the shortest path
// inside the 9x9 origin bitmap.
void HPathTables::Build1(ClusterMap& m, int cx, int cz) {
  const int x0 = cx * 8, z0 = cz * 8;
  bool bm[9][9];
  for (int r = 0; r < 9; ++r)
    for (int i = 0; i < 9; ++i) bm[r][i] = m.grid && m.grid->Passable(x0 + i, z0 + r);
  std::vector<std::pair<int, int>> nodes;  // (x, z) local
  const int sides[4][4] = {{0, 0, 1, 0}, {0, 8, 1, 0}, {0, 0, 0, 1}, {8, 0, 0, 1}};
  for (const auto& sd : sides) {
    int st = -1, en = -1;
    for (int i = 0; i < 9; ++i) {
      int x = sd[0] + sd[2] * i, z = sd[1] + sd[3] * i;
      if (bm[z][x]) {
        en = i;
        if (st < 0) st = i;
      }
      if (st < 0) continue;
      if (i == 8 || i != en) {
        int p;
        if (st <= 4 && en >= 4) p = 4;
        else if (st == 0) p = 0;
        else if (en == 8) p = 8;
        else p = (st + en + (en < 4 ? 1 : 0)) >> 1;
        nodes.push_back(sd[2] ? std::make_pair(p, sd[1]) : std::make_pair(sd[0], p));
        st = en = -1;
      }
    }
  }
  std::sort(nodes.begin(), nodes.end(), [](const auto& a, const auto& b) {
    return (a.first | (a.second << 8)) < (b.first | (b.second << 8));
  });
  nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
  int n = static_cast<int>(nodes.size());
  std::vector<int8_t> cls(static_cast<size_t>(n) * n, -1);
  for (int i = 0; i < n; ++i) {
    // label-correcting shortest paths (the original's work list; distances converge the same)
    float dist[9][9];
    for (auto& row : dist)
      for (float& d : row) d = std::numeric_limits<float>::infinity();
    std::vector<std::pair<int, int>> work{nodes[i]};
    dist[nodes[i].second][nodes[i].first] = 0;
    while (!work.empty()) {
      auto c = work.back();
      work.pop_back();
      int added = 0;
      for (int k = 0; k < 8; ++k) {
        if ((kNeed[k] & added) != kNeed[k]) continue;
        int nx = c.first + kDX[k], nz = c.second + kDZ[k];
        if (nx < 0 || nz < 0 || nx > 8 || nz > 8 || !bm[nz][nx]) continue;
        added |= 1 << k;
        float nd = (k < 4 ? 1.0f : kDiag) + dist[c.second][c.first];
        if (nd < dist[nz][nx]) {
          dist[nz][nx] = nd;
          work.push_back({nx, nz});
        }
      }
    }
    for (int j = i + 1; j < n; ++j) {
      float d = dist[nodes[j].second][nodes[j].first];
      if (!std::isfinite(d)) continue;
      int c = Quantize(d, PathOctile(nodes[i].first - nodes[j].first, nodes[i].second - nodes[j].second));
      cls[static_cast<size_t>(i) * n + j] = cls[static_cast<size_t>(j) * n + i] = static_cast<int8_t>(c);
    }
  }
  Cluster& C = m.lv[1][static_cast<size_t>(cz) * m.wc[1] + cx];
  C.nx.resize(n);
  C.nz.resize(n);
  for (int i = 0; i < n; ++i) {
    C.nx[i] = static_cast<uint8_t>(nodes[i].first);
    C.nz[i] = static_cast<uint8_t>(nodes[i].second);
  }
  C.cls = std::move(cls);
  C.n = n;
  Prune(&C.nx, &C.nz, &C.cls, &C.n);
}

// ClusterBuild level 2 (0x9310e0): the sub-clusters' nodes on the 32-cell boundary, edges from a
// Dijkstra over the sub-cluster graph.
void HPathTables::Build2(ClusterMap& m, int cx, int cz, int* /*budget*/) {
  std::vector<std::pair<int, int>> nodes;  // local to the 32-cluster
  auto sub = [&](int i, int j) -> const Cluster* {
    int a = 4 * cx + i, b = 4 * cz + j;
    if (a >= m.wc[1] || b >= m.hc[1]) return nullptr;
    return &m.lv[1][static_cast<size_t>(b) * m.wc[1] + a];
  };
  for (int j = 0; j < 4; ++j)
    for (int i = 0; i < 4; ++i) {
      const Cluster* s = sub(i, j);
      if (!s) continue;
      for (int k = 0; k < s->n; ++k) {
        int gx = s->nx[k] + i * 8, gz = s->nz[k] + j * 8;
        if (gx % 32 == 0 || gz % 32 == 0) nodes.push_back({gx, gz});
      }
    }
  std::sort(nodes.begin(), nodes.end(), [](const auto& a, const auto& b) {
    return (a.first | (a.second << 8)) < (b.first | (b.second << 8));
  });
  nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
  int n = static_cast<int>(nodes.size());
  std::vector<int8_t> cls(static_cast<size_t>(n) * n, -1);
  // the sub-cluster graph: every node of the 16 sub-clusters (local 0..32 coordinates), edges
  // from each sub-cluster containing it (cost octile * exp(class/6))
  std::vector<std::pair<int, int>> gn;  // graph nodes (x, z)
  std::unordered_map<int, int> gid;
  auto idOf = [&](int x, int z) {
    int k = x | (z << 8);
    auto it = gid.find(k);
    if (it != gid.end()) return it->second;
    int id = static_cast<int>(gn.size());
    gn.push_back({x, z});
    gid[k] = id;
    return id;
  };
  std::vector<std::vector<std::pair<int, float>>> adj;
  for (int sz = 0; sz < 4; ++sz)
    for (int sx = 0; sx < 4; ++sx) {
      const Cluster* sc = sub(sx, sz);
      if (!sc) continue;
      std::vector<int> ids(sc->n);
      for (int a = 0; a < sc->n; ++a) ids[a] = idOf(sc->nx[a] + sx * 8, sc->nz[a] + sz * 8);
      if (adj.size() < gn.size()) adj.resize(gn.size());
      for (int a = 0; a < sc->n; ++a)
        for (int b = 0; b < sc->n; ++b) {
          if (b == a) continue;
          int cl = sc->Cls(a, b);
          if (cl < 0) continue;
          adj[ids[a]].push_back({ids[b], Cls(cl) * PathOctile(sc->nx[a] - sc->nx[b], sc->nz[a] - sc->nz[b])});
        }
    }
  adj.resize(gn.size());
  // a node's neighbours in the order the original visits them: sub-clusters x-major over the
  // cluster index rect, z inner, node order inside each (as the sub-cluster loop above, but per node)
  std::vector<std::vector<std::pair<int, float>>> ordered(gn.size());
  for (size_t v = 0; v < gn.size(); ++v) {
    int x = gn[v].first, z = gn[v].second;
    int ix0 = std::max(0, (x - 1) >> 3), ix1 = std::min(4, (x >> 3) + 1);
    int iz0 = std::max(0, (z - 1) >> 3), iz1 = std::min(4, (z >> 3) + 1);
    for (int sx = ix0; sx < ix1; ++sx)
      for (int sz = iz0; sz < iz1; ++sz) {
        const Cluster* sc = sub(sx, sz);
        if (!sc) continue;
        int lx = x - sx * 8, lz = z - sz * 8, a = -1;
        for (int k = 0; k < sc->n; ++k)
          if (sc->nx[k] == lx && sc->nz[k] == lz) {
            a = k;
            break;
          }
        if (a < 0) continue;
        for (int b = 0; b < sc->n; ++b) {
          if (b == a) continue;
          int cl = sc->Cls(a, b);
          if (cl < 0) continue;
          ordered[v].push_back({gid[(sc->nx[b] + sx * 8) | ((sc->nz[b] + sz * 8) << 8)],
                                Cls(cl) * PathOctile(sc->nx[a] - sc->nx[b], sc->nz[a] - sc->nz[b])});
        }
      }
  }
  std::vector<float> dist(gn.size());
  std::vector<uint8_t> done(gn.size());
  struct QE {
    float d;
    int x, z, id;
  };
  auto cmp = [](const QE& a, const QE& b) {
    if (a.d != b.d) return a.d > b.d;
    if (a.x != b.x) return a.x > b.x;
    return a.z > b.z;
  };
  std::vector<QE> pq;
  for (int i = 1; i < n; ++i) {
    std::fill(dist.begin(), dist.end(), std::numeric_limits<float>::infinity());
    std::fill(done.begin(), done.end(), 0);
    int src = gid[nodes[i].first | (nodes[i].second << 8)];
    dist[src] = 0;
    pq.clear();
    pq.push_back({0.0f, nodes[i].first, nodes[i].second, src});
    while (!pq.empty()) {
      std::pop_heap(pq.begin(), pq.end(), cmp);
      QE top = pq.back();
      pq.pop_back();
      if (done[top.id]) continue;
      done[top.id] = 1;
      for (const auto& [nb, cost] : ordered[top.id]) {
        float nd = top.d + cost;
        if (nd < dist[nb]) {
          dist[nb] = nd;
          pq.push_back({nd, gn[nb].first, gn[nb].second, nb});
          std::push_heap(pq.begin(), pq.end(), cmp);
        }
      }
    }
    for (int j = 0; j < i; ++j) {
      auto it = gid.find(nodes[j].first | (nodes[j].second << 8));
      if (it == gid.end() || !std::isfinite(dist[it->second])) continue;
      int c = Quantize(dist[it->second],
                       PathOctile(nodes[j].first - nodes[i].first, nodes[j].second - nodes[i].second));
      cls[static_cast<size_t>(i) * n + j] = cls[static_cast<size_t>(j) * n + i] = static_cast<int8_t>(c);
    }
  }
  Cluster& C = m.lv[2][static_cast<size_t>(cz) * m.wc[2] + cx];
  C.nx.resize(n);
  C.nz.resize(n);
  for (int i = 0; i < n; ++i) {
    C.nx[i] = static_cast<uint8_t>(nodes[i].first);
    C.nz[i] = static_cast<uint8_t>(nodes[i].second);
  }
  C.cls = std::move(cls);
  C.n = n;
  Prune(&C.nx, &C.nz, &C.cls, &C.n);
}

// ---- the traveler's predicates (CAiPathFinder virtuals) --------------------------------------

// ClusterRect 0x9542d0: the union of the level-L clusters containing the cell (half-open)
void HPathTables::ClusterRect(int x, int z, int L, int* r) const {
  int S = kSize[L];
  r[0] = std::max(0, (x - 1) & -S);
  r[1] = std::max(0, (z - 1) & -S);
  r[2] = std::min(w_, ((x + S) & -S) + 1);
  r[3] = std::min(h_, ((z + S) & -S) + 1);
}

// ShouldSearchRect 0x5aa860
bool HPathTables::ShouldSearchRect(const PathTraveler& t, const int* r) const {
  if (t.mode == 0 || t.ignoreStructures) {
    if (r[0] <= t.anchor.x && t.anchor.x < r[2] && r[1] <= t.anchor.z && t.anchor.z < r[3]) return true;
  } else {
    for (const auto& b : t.boxes)
      if (r[0] <= b[2] && b[0] <= r[2] && r[1] <= b[3] && b[1] <= r[3] && r[0] < r[2] && r[1] < r[3]) return true;
  }
  const int* g = t.goal;
  const int* in = t.inner;
  if (g[0] < r[2] && r[0] < g[2] && g[1] < r[3] && r[1] < g[3] && g[0] < g[2] && g[1] < g[3] && r[0] < r[2] &&
      r[1] < r[3]) {
    if (r[0] < in[0] || in[2] < r[2] || r[1] < in[1] || in[3] < r[3]) return true;
  }
  return false;
}

// CanTraverseCell 0x5aa710 (level 0 only)
bool HPathTables::CanTraverse(const Search& s, const PathCell& n) const {
  const PathTraveler& t = *s.t;
  // 0x5aa73e / 0x5aa7ca: Entity::GetFootprint of the unit (its AltFootprint while unit+0x124 or +0x125 is set), not
  // the cluster map's footprint (slot 0, +0x124 only)
  const bool alt = t.unit && (t.unit->altFootprintNav || t.unit->altFootprintForced);
  const PathGrid* g = t.fitGrid[alt ? 1 : 0] ? t.fitGrid[alt ? 1 : 0] : s.cm->grid;
  if (t.ignoreStructures) {
    if (!g || !g->Caps(n.x, n.z)) return false;
  } else if (t.startStandable) {
    // (0x5aa7ba..0x5aa7fc: OCCUPY_MobileCheck's caps without Sub for a unit on the Water layer, as Reach takes them)
    if (!g || !g->Passable(n.x, n.z, t.dropSub ? kLayerSub : 0)) return false;
  }
  if (t.mode != 0 && PathUnitBlocked(t.unit, n.x, n.z, t.mode == 3 ? 2 : 1)) return false;
  return true;
}

// EdgeAllowed (CAiPathFinder::IsInBounds 0x5aa680): with neither the army's whole-map flag (+0x74) nor a start
// outside the playable rect (+0x75), the cell with a margin of the footprint size inside the rect (as floats there;
// the ints compare the same)
bool HPathTables::EdgeAllowed(const PathTraveler& t, const PathCell& n) const {
  if (t.useWholeMap || !t.insidePlayable) return true;
  const int r[4] = {playRect ? playRect[0] : 0, playRect ? playRect[1] : 0, playRect ? playRect[2] : w_,
                    playRect ? playRect[3] : h_};
  return n.x - t.S >= r[0] && n.z - t.S >= r[1] && n.x + t.S <= r[2] && n.z + t.S <= r[3];
}

float HPathTables::Heuristic(const PathTraveler& t, const PathCell& c) const {
  int dx = std::max({0, t.goal[0] - c.x, c.x - t.goal[2] + 1});
  int dz = std::max({0, t.goal[1] - c.z, c.z - t.goal[3] + 1});
  return PathOctile(dx, dz) * kHScale;
}

bool HPathTables::IsGoal(const PathTraveler& t, const PathCell& c) const {
  const int* g = t.goal;
  const int* in = t.inner;
  return g[0] <= c.x && c.x < g[2] && g[1] <= c.z && c.z < g[3] &&
         (c.x < in[0] || in[2] <= c.x || c.z < in[1] || in[3] <= c.z);
}

int HPathTables::ArmyPathcap() const {
  int m = std::max(w_, h_);
  if (m >= 4096) return 20000;
  if (m >= 2048) return 10000;
  if (m >= 1024) return 2000;
  if (m >= 512) return 1000;
  return 500;
}

// ---- the search ------------------------------------------------------------------------------

void HPathTables::HeapUp(Search& s, int i) {
  auto& H = s.heap;
  while (i > 0) {
    int p = (i - 1) >> 1;
    if (H[i].first > H[p].first) break;  // an equal key moves up
    std::swap(H[i], H[p]);
    s.nodes[H[i].second].heapPos = i;
    s.nodes[H[p].second].heapPos = p;
    i = p;
  }
}

void HPathTables::HeapDown(Search& s, int i) {
  auto& H = s.heap;
  int n = static_cast<int>(H.size());
  for (;;) {
    int l = 2 * i + 1;
    if (l >= n) return;
    int sm = i;
    if (H[i].first > H[l].first) sm = l;
    if (l + 1 < n && H[sm].first > H[l + 1].first) sm = l + 1;
    if (sm == i) return;
    std::swap(H[i], H[sm]);
    s.nodes[H[i].second].heapPos = i;
    s.nodes[H[sm].second].heapPos = sm;
    i = sm;
  }
}

// BeginQuery 0x765fe0
void HPathTables::Begin(Search& s, PathTraveler* t) {
  s.t = t;
  s.cm = &Map(t->fp);
  s.nodes.clear();
  s.index.clear();
  s.heap.clear();
  s.expansions = 0;
  s.cap = t->goalReachable ? ArmyPathcap() : 1000;  // GetPathcap 0x5aaa30
  Node st;
  st.c = t->anchor;
  st.h = Heuristic(*t, st.c);
  st.state = 1;
  st.heapPos = 0;
  s.nodes.push_back(st);
  s.index[Key(st.c.x, st.c.z)] = 0;
  s.heap.push_back({st.h, 0});
  s.best = 0;
  s.bestH = st.h;  // (the oracle probe: an unreachable goal next to the start gives the path [start])
}

// AddNeighbours 0x766280 (level 0) / 0x766350 (levels 1, 2)
bool HPathTables::AddNeighbours(Search& s, const PathCell& c, int L, std::vector<std::pair<PathCell, float>>* out,
                                int* budget) {
  const PathTraveler& t = *s.t;
  if (L == 0) {
    int added = 0;
    for (int k = 0; k < 8; ++k) {
      if ((kNeed[k] & added) != kNeed[k]) continue;
      PathCell n{c.x + kDX[k], c.z + kDZ[k]};
      int r[4];
      ClusterRect(n.x, n.z, 1, r);
      if (!ShouldSearchRect(t, r)) continue;
      if (!CanTraverse(s, n) || !EdgeAllowed(t, n)) continue;
      out->push_back({n, k < 4 ? 1.0f : kDiag});
      added |= 1 << k;
    }
    return true;
  }
  ClusterMap& m = *s.cm;
  int sh = kShift[L];
  int ix0 = std::max(0, (c.x - 1) >> sh), ix1 = std::min(m.wc[L], (c.x >> sh) + 1);
  int iz0 = std::max(0, (c.z - 1) >> sh), iz1 = std::min(m.hc[L], (c.z >> sh) + 1);
  for (int cx = ix0; cx < ix1; ++cx)
    for (int cz = iz0; cz < iz1; ++cz) {
      if (!Ready(m, L, cx, cz, budget)) return false;
      const Cluster& C = m.lv[L][static_cast<size_t>(cz) * m.wc[L] + cx];
      int lx = c.x - (cx << sh), lz = c.z - (cz << sh);
      int a = -1;
      for (int k = 0; k < C.n; ++k)
        if (C.nx[k] == lx && C.nz[k] == lz) {
          a = k;
          break;
        }
      if (a < 0) continue;
      for (int b = 0; b < C.n; ++b) {
        if (b == a) continue;
        int cl = C.Cls(a, b);
        if (cl < 0) continue;
        PathCell nb{C.nx[b] + (cx << sh), C.nz[b] + (cz << sh)};
        if (L != kTop) {
          int r[4];
          ClusterRect(nb.x, nb.z, L + 1, r);
          if (!ShouldSearchRect(t, r)) continue;
        }
        if (!EdgeAllowed(t, nb)) continue;
        out->push_back({nb, PathOctile(C.nx[a] - C.nx[b], C.nz[a] - C.nz[b]) * Cls(cl)});
      }
    }
  return true;
}

// CollectNeighbours 0x766280: 1 done, 0 not ready (cluster budget)
int HPathTables::Collect(Search& s, const PathCell& c, std::vector<std::pair<PathCell, float>>* out, int* budget) {
  int r[4];
  ClusterRect(c.x, c.z, 0, r);
  int L = ShouldSearchRect(*s.t, r) ? 0 : kTop;
  for (; L >= 0; --L) {
    int S = kSize[L];
    if ((c.x & (S - 1)) == 0 || (c.z & (S - 1)) == 0) {
      if (!AddNeighbours(s, c, L, out, budget)) return 0;
      if (L > 0) {
        ClusterRect(c.x, c.z, L, r);
        if (!ShouldSearchRect(*s.t, r)) return 1;
      }
    }
  }
  return 1;
}

// WorkOnce 0x7685a0 / ExpandNode 0x7661c0
int HPathTables::WorkOnce(Search& s, int* budget) {
  std::vector<std::pair<PathCell, float>> nb;
  for (;;) {
    if (s.heap.empty()) return 0;
    int ni = s.heap[0].second;
    *budget -= 1;
    s.expansions += 1;
    if (*budget < 1) return 2;
    PathCell c = s.nodes[ni].c;
    if (IsGoal(*s.t, c)) {
      s.best = ni;
      return 1;
    }
    if (s.expansions > s.cap) return 3;
    nb.clear();
    if (!Collect(s, c, &nb, budget)) return 2;
    s.nodes[ni].state = 2;
    // pop
    int last = static_cast<int>(s.heap.size()) - 1;
    std::swap(s.heap[0], s.heap[last]);
    s.nodes[s.heap[0].second].heapPos = 0;
    s.heap.pop_back();
    s.nodes[ni].heapPos = -1;
    if (!s.heap.empty()) HeapDown(s, 0);
    for (const auto& [m, cost] : nb) {
      float g = s.nodes[ni].g + cost;
      auto it = s.index.find(Key(m.x, m.z));
      if (it == s.index.end()) {
        Node n;
        n.c = m;
        n.h = Heuristic(*s.t, m);
        if (n.h < s.bestH) {
          s.bestH = n.h;
          s.best = static_cast<int>(s.nodes.size());
        }
        n.g = g;
        n.parent = ni;
        n.state = 1;
        n.heapPos = static_cast<int>(s.heap.size());
        int idx = static_cast<int>(s.nodes.size());
        s.nodes.push_back(n);
        s.index[Key(m.x, m.z)] = idx;
        s.heap.push_back({n.h + g, idx});
        HeapUp(s, n.heapPos);
      } else {
        Node& n = s.nodes[it->second];
        if (n.state == 1 && g < n.g) {
          n.g = g;
          n.parent = ni;
          int i = n.heapPos;
          float old = s.heap[i].first;
          s.heap[i].first = n.h + g;
          if (old > s.heap[i].first) HeapUp(s, i);
          else HeapDown(s, i);
        }
      }
    }
  }
}

// FinishQuery 0x766140: the path to the best node, start included
void HPathTables::Finish(Search& s, bool accepted) {
  PathTraveler* t = s.t;
  static const bool dbg = getenv("MOHO64_DEBUG_NAV") != nullptr;
  if (dbg) fprintf(stderr, "hpath: army %d search done: %d expansions, %zu nodes, accepted %d\n", t->armyIndex, s.expansions, s.nodes.size(), accepted ? 1 : 0);
  t->result.clear();
  for (int i = s.best; i >= 0; i = s.nodes[i].parent) t->result.push_back(s.nodes[i].c);
  std::reverse(t->result.begin(), t->result.end());
  t->accepted = accepted;
  t->pending = false;
  t->delivered = true;
}

void HPathTables::Link(ArmyQueue& q, int key, PathTraveler* t) {
  t->qPrev = q.tail;
  t->qNext = nullptr;
  (q.tail ? q.tail->qNext : q.head) = t;
  q.tail = t;
  t->queued = true;
  t->queuedIn = key;
}

void HPathTables::Unlink(ArmyQueue& q, PathTraveler* t) {
  (t->qPrev ? t->qPrev->qNext : q.head) = t->qNext;
  (t->qNext ? t->qNext->qPrev : q.tail) = t->qPrev;
  t->qPrev = t->qNext = nullptr;
  t->queued = false;
}

void HPathTables::Queue(PathTraveler* t) {
  Cancel(t);
  t->pending = true;
  t->delivered = false;
  Link(queues_[t->armyIndex], t->armyIndex, t);
}

// The traveler leaves the queue it waits in, or its search is dropped (the search state goes; the unlink is O(1),
// as the original's list node). A traveler is in at most one queue: Queue cancels first.
void HPathTables::Cancel(PathTraveler* t) {
  if (t->queued) {
    auto it = queues_.find(t->queuedIn);
    if (it != queues_.end()) Unlink(it->second, t);
    t->queued = false;
  }
  if (t->searching) {
    auto it = queues_.find(t->searchingIn);
    if (it != queues_.end() && it->second.active && it->second.active->t == t) it->second.active.reset();
    t->searching = false;
  }
  t->pending = false;
}

// PathQueue::Work 0x765ed0 with path_ArmyBudget = 2500, per army (CArmyImpl::OnTick)
void HPathTables::WorkAll(int /*armies*/) {
  inWork_ = true;
  for (auto& [a, q] : queues_) {
    int budget = 2500;
    while (budget >= 1) {
      if (!q.active) {
        if (!q.head) break;
        q.active = std::make_unique<Search>();
        PathTraveler* t = q.head;
        Unlink(q, t);
        t->searching = true;
        t->searchingIn = a;
        Begin(*q.active, t);
      }
      int before = budget;
      int r = WorkOnce(*q.active, &budget);
      static const bool dbg = getenv("MOHO64_DEBUG_NAV") != nullptr;
      if (dbg) fprintf(stderr, "hpath: army %d work %d -> %d (r=%d)\n", a, before, budget, r);
      if (r == 2) break;
      Finish(*q.active, r == 1);
      q.active->t->searching = false;
      q.active.reset();
    }
  }
  inWork_ = false;
}

bool HPathTables::SearchNow(PathTraveler* t, int budget) {
  Search s;
  Begin(s, t);
  int r = WorkOnce(s, &budget);
  if (r == 2) return false;
  Finish(s, r == 1);
  return r == 1;
}

}  // namespace moho
