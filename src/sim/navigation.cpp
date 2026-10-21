// Pathfinding (see navigation.h).
#include "sim/navigation.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <queue>

#include "core/profile.h"
#include "sim/terrain.h"

namespace moho {

// STIMap::OCCUPY_MobileCheck 0x564ab0 for the footprint with its origin cell at (x, z): the layers the terrain
// allows there (structures not counted). PathGrid keeps it for every cell.
static uint8_t MobileCheckCaps(const TerrainMap& map, const NamedFootprint& fp, const bool* blocking, int x, int z) {
  const int w = map.width(), h = map.height();
  const int sx = std::max<int>(1, fp.sizeX), sz = std::max<int>(1, fp.sizeZ);
  if (x < 0 || z < 0 || x + sx > w - 1 || z + sz > h - 1) return 0;
  const float water = map.hasWater ? map.waterElevation : -10000.0f;
  float lo = 1e30f, hi = -1e30f;
  for (int cz = z; cz <= z + sz; ++cz)
    for (int cx = x; cx <= x + sx; ++cx) {
      float v = map.HeightAt(cx, cz);
      lo = std::min(lo, v);
      hi = std::max(hi, v);
      if (blocking && blocking[map.TerrainType(cx, cz)]) return 0;
    }
  uint8_t caps = fp.caps;
  if (water - hi < fp.minWaterDepth) caps &= 0xf1;  // too shallow somewhere: no seabed/sub/water
  if (water - lo > fp.maxWaterDepth) caps &= 0xfc;  // too deep somewhere: no land/seabed
  if ((caps & 3) && fp.maxSlope != 0.0f) {
    float step = 0;
    for (int cz = z; cz <= z + sz; ++cz)
      for (int cx = x; cx < x + sx; ++cx)
        step = std::max(step, std::fabs(map.HeightAt(cx + 1, cz) - map.HeightAt(cx, cz)));
    for (int cx = x; cx <= x + sx; ++cx)
      for (int cz = z; cz < z + sz; ++cz)
        step = std::max(step, std::fabs(map.HeightAt(cx, cz + 1) - map.HeightAt(cx, cz)));
    if (fp.maxSlope < step) caps &= 0xfc;
  }
  return caps;
}

PathGrid::PathGrid(const TerrainMap& map, const NamedFootprint& fp, const bool* blocking) {
  w_ = map.width();
  h_ = map.height();
  cells_.assign(static_cast<size_t>(w_) * h_, 0);
  blocked_.assign(static_cast<size_t>(w_) * h_, 0);
  blockedWater_.assign(static_cast<size_t>(w_) * h_, 0);
  ignoreStructures = (fp.flags & 1) != 0;
  fp_ = fp;
  sizeX = std::max<int>(1, fp.sizeX);
  sizeZ = std::max<int>(1, fp.sizeZ);
  for (int z = 0; z < h_; ++z)
    for (int x = 0; x < w_; ++x) cells_[static_cast<size_t>(z) * w_ + x] = MobileCheckCaps(map, fp, blocking, x, z);
}

void PathGrid::TerrainChanged(const TerrainMap& map, const bool* blocking, int vx0, int vz0, int vx1, int vz1) {
  // an origin (x, z) covers the vertices [x, x + sizeX] x [z, z + sizeZ]
  const int x0 = std::max(0, vx0 - sizeX), z0 = std::max(0, vz0 - sizeZ);
  const int x1 = std::min(w_ - 1, vx1), z1 = std::min(h_ - 1, vz1);
  for (int z = z0; z <= z1; ++z)
    for (int x = x0; x <= x1; ++x) cells_[static_cast<size_t>(z) * w_ + x] = MobileCheckCaps(map, fp_, blocking, x, z);
}

void Navigation::TerrainChanged(int vx0, int vz0, int vx1, int vz1) {
  if (!map_) return;
  for (auto& [key, g] : grids_) g->TerrainChanged(*map_, blocking_.data(), vx0, vz0, vx1, vz1);
}

uint8_t Navigation::MobileCaps(const NamedFootprint& fp, int x, int z) const {
  return map_ ? MobileCheckCaps(*map_, fp, blocking_.data(), x, z) : 0;
}

void PathGrid::Block(int x0, int z0, int x1, int z1, int delta, uint8_t occ) {
  // origins o with [o, o+size) overlapping [x0, x1)
  int ox0 = std::max(0, x0 - sizeX + 1), oz0 = std::max(0, z0 - sizeZ + 1);
  int ox1 = std::min(w_ - 1, x1 - 1), oz1 = std::min(h_ - 1, z1 - 1);
  for (int z = oz0; z <= oz1; ++z)
    for (int x = ox0; x <= ox1; ++x) {
      const size_t i = static_cast<size_t>(z) * w_ + x;
      if (occ & kOccLand) blocked_[i] = static_cast<uint16_t>(blocked_[i] + delta);
      if (occ & kOccWater) blockedWater_[i] = static_cast<uint16_t>(blockedWater_[i] + delta);
    }
}

void Navigation::SetCells(int x0, int z0, int x1, int z1, uint8_t occ, bool on) {
  if (occ_.empty()) occ_.assign(static_cast<size_t>(map_->width()) * map_->height(), 0);
  if (occWater_.empty()) occWater_.assign(static_cast<size_t>(map_->width()) * map_->height(), 0);
  const uint8_t v = on ? 1 : 0;
  const int d = on ? +1 : -1;
  for (int z = std::max(0, z0); z < std::min(map_->height(), z1); ++z)
    for (int x = std::max(0, x0); x < std::min(map_->width(), x1); ++x) {
      const size_t i = static_cast<size_t>(z) * map_->width() + x;
      if ((occ & kOccLand) && occ_[i] != v) {
        occ_[i] = v;
        for (auto& [k, g] : grids_) g->Block(x, z, x + 1, z + 1, d, kOccLand);
      }
      if ((occ & kOccWater) && occWater_[i] != v) {
        occWater_[i] = v;
        for (auto& [k, g] : grids_) g->Block(x, z, x + 1, z + 1, d, kOccWater);
      }
    }
}

void Navigation::AddStructure(uint32_t entity, int x0, int z0, int x1, int z1, uint8_t fpCaps) {
  const uint8_t occ = OccLayersOf(fpCaps);
  if (!map_ || x1 <= x0 || z1 <= z0 || !occ) return;
  ForgetStructure(entity);  // (occupying again: one record per entity)
  structures_.push_back({entity, x0, z0, x1, z1, occ});
  SetCells(x0, z0, x1, z1, occ, true);
}

void Navigation::RemoveStructure(uint32_t entity, const int* rect) {
  for (size_t i = 0; i < structures_.size(); ++i) {
    if (structures_[i].entity != entity) continue;
    OccupiedRect r = structures_[i];
    structures_.erase(structures_.begin() + static_cast<long>(i));
    if (rect) SetCells(rect[0], rect[1], rect[2], rect[3], r.occ, false);
    else SetCells(r.x0, r.z0, r.x1, r.z1, r.occ, false);
    return;
  }
}

void Navigation::ForgetStructure(uint32_t entity) {
  for (size_t i = 0; i < structures_.size(); ++i)
    if (structures_[i].entity == entity) {
      structures_.erase(structures_.begin() + static_cast<long>(i));
      return;
    }
}

bool Navigation::AnyStructureIn(int x0, int z0, int x1, int z1, uint8_t occ) const {
  if (occ_.empty() || !map_) return false;
  for (int z = std::max(0, z0); z < std::min(map_->height(), z1); ++z)
    for (int x = std::max(0, x0); x < std::min(map_->width(), x1); ++x) {
      const size_t i = static_cast<size_t>(z) * map_->width() + x;
      if (((occ & kOccLand) && occ_[i]) || ((occ & kOccWater) && occWater_[i])) return true;
    }
  return false;
}

const PathGrid* Navigation::Grid(const NamedFootprint& fp) {
  if (!map_) return nullptr;
  std::string key = fp.name;
  key += '/' + std::to_string(fp.sizeX) + 'x' + std::to_string(fp.sizeZ) + '/' + std::to_string(fp.caps) + '/' +
         std::to_string(fp.flags & 1);
  auto it = grids_.find(key);
  if (it != grids_.end()) return it->second.get();
  prof::SectionTimer sec(prof::kSecPathGridMade);  // (MOHO64_PROFILE_HITCH only: which grid, and its time)
  const double hitchT0 = prof::Hitch() ? prof::NowMs() : 0;
  auto g = std::make_unique<PathGrid>(*map_, fp, blocking_.data());
  if (!occ_.empty())  // (the held cells as they are now: SetCells keeps each grid's counts after this)
    for (int z = 0; z < map_->height(); ++z)
      for (int x = 0; x < map_->width(); ++x) {
        const size_t i = static_cast<size_t>(z) * map_->width() + x;
        if (occ_[i]) g->Block(x, z, x + 1, z + 1, +1, kOccLand);
        if (occWater_[i]) g->Block(x, z, x + 1, z + 1, +1, kOccWater);
      }
  const PathGrid* r = g.get();
  grids_[key] = std::move(g);
  if (prof::Hitch()) prof::AddCall("path grid made " + key, prof::NowMs() - hitchT0);
  return r;
}

namespace {
template <class P>
bool LineOfSightP(const P& pass, int x0, int z0, int x1, int z1) {
  // every cell the segment between the two cell centres touches must be passable
  int dx = std::abs(x1 - x0), dz = std::abs(z1 - z0);
  int sx = x0 < x1 ? 1 : -1, sz = z0 < z1 ? 1 : -1;
  int x = x0, z = z0;
  int n = 1 + dx + dz;
  int err = dx - dz;
  dx *= 2;
  dz *= 2;
  for (; n > 0; --n) {
    if (!pass(x, z)) return false;
    if (err > 0) {
      x += sx;
      err -= dz;
    } else if (err < 0) {
      z += sz;
      err += dx;
    } else {  // exactly through a corner: both neighbours must be free
      if (!pass(x + sx, z) || !pass(x, z + sz)) return false;
      x += sx;
      z += sz;
      err += dx - dz;
      --n;
    }
  }
  return true;
}

}  // namespace

bool Navigation::FindPath(const NamedFootprint& fp, const Vec3& from, const Vec3& to, std::vector<Vec3>* out) {
  out->clear();
  const PathGrid* g = Grid(fp);
  const float hx = fp.sizeX * 0.5f, hz = fp.sizeZ * 0.5f;
  auto cellOf = [&](float x, float z, int* cx, int* cz) {
    *cx = static_cast<int>(std::nearbyint(x - hx));
    *cz = static_cast<int>(std::nearbyint(z - hz));
  };
  if (!g) {
    out->push_back(to);
    return true;
  }
  // A unit standing inside a structure (a factory's product, a unit a structure was built over)
  // may move through that structure's cells to get out.
  std::vector<OccupiedRect> escape;
  {
    int ox = static_cast<int>(std::nearbyint(from.x - hx)), oz = static_cast<int>(std::nearbyint(from.z - hz));
    if (g->Blocked(ox, oz))
      for (const auto& r : structures_)
        if (r.x0 < ox + g->sizeX && ox < r.x1 && r.z0 < oz + g->sizeZ && oz < r.z1) escape.push_back(r);
  }
  auto pass = [&](int x, int z) -> bool {
    if (g->Passable(x, z)) return true;
    if (escape.empty() || !g->Caps(x, z)) return false;
    // every held cell under the footprint must belong to one of the structures the unit stands in
    for (int cz = std::max(0, z); cz < std::min(map_->height(), z + g->sizeZ); ++cz)
      for (int cx = std::max(0, x); cx < std::min(map_->width(), x + g->sizeX); ++cx) {
        const size_t i = static_cast<size_t>(cz) * map_->width() + cx;
        if (!occ_[i] && !occWater_[i]) continue;
        bool mine = false;
        for (const auto& r : escape) mine = mine || (r.x0 <= cx && cx < r.x1 && r.z0 <= cz && cz < r.z1);
        if (!mine) return false;
      }
    return true;
  };
  ++searches;
  lastWork = 1;
  int sx, sz, gx, gz;
  cellOf(from.x, from.z, &sx, &sz);
  cellOf(to.x, to.z, &gx, &gz);
  const int W = g->width(), H = g->height();
  sx = std::clamp(sx, 0, W - 1);
  sz = std::clamp(sz, 0, H - 1);
  gx = std::clamp(gx, 0, W - 1);
  gz = std::clamp(gz, 0, H - 1);
  if (sx == gx && sz == gz) {
    out->push_back(to);
    return true;
  }
  // A goal that cannot be stood on: the nearest cell that can (rings around it).
  if (!pass(gx, gz)) {
    bool found = false;
    for (int r = 1; r <= 32 && !found; ++r)
      for (int dz = -r; dz <= r && !found; ++dz)
        for (int dx = -r; dx <= r; ++dx) {
          if (std::max(std::abs(dx), std::abs(dz)) != r) continue;
          if (pass(gx + dx, gz + dz)) {
            gx += dx;
            gz += dz;
            found = true;
            break;
          }
        }
    if (!found) return false;
  }
  lastWork = static_cast<uint64_t>(std::max(std::abs(gx - sx), std::abs(gz - sz))) + 1;
  if (LineOfSightP(pass, sx, sz, gx, gz)) {
    out->push_back({gx + hx, to.y, gz + hz});
    return true;
  }
  // A* over cells (8 neighbours, no corner cutting), octile heuristic as the original's
  // CAiPathFinder::GetHeuristicCost.
  const size_t N = static_cast<size_t>(W) * H;
  std::vector<float> gcost(N, 1e30f);
  std::vector<int32_t> parent(N, -1);
  std::vector<uint8_t> closed(N, 0);
  auto idx = [W](int x, int z) { return static_cast<size_t>(z) * W + x; };
  auto heur = [&](int x, int z) {
    float ax = static_cast<float>(std::abs(x - gx)), az = static_cast<float>(std::abs(z - gz));
    return ax >= az ? ax + az * 0.41421356f : az + ax * 0.41421356f;
  };
  struct Node {
    float f, h;
    int32_t i;
    bool operator<(const Node& o) const { return f != o.f ? f > o.f : h > o.h; }
  };
  std::priority_queue<Node> open;
  size_t s = idx(sx, sz), goal = idx(gx, gz);
  gcost[s] = 0;
  open.push({heur(sx, sz), heur(sx, sz), static_cast<int32_t>(s)});
  size_t best = s;
  float bestH = heur(sx, sz);
  const int kMaxExpand = 400000;
  int expandedHere = 0;
  bool reached = false;
  static const int DX[8] = {1, -1, 0, 0, 1, 1, -1, -1};
  static const int DZ[8] = {0, 0, 1, -1, 1, -1, 1, -1};
  while (!open.empty()) {
    Node n = open.top();
    open.pop();
    size_t i = static_cast<size_t>(n.i);
    if (closed[i]) continue;
    closed[i] = 1;
    if (i == goal) {
      reached = true;
      break;
    }
    if (n.h < bestH) {
      bestH = n.h;
      best = i;
    }
    if (++expandedHere > kMaxExpand) break;
    int x = static_cast<int>(i % W), z = static_cast<int>(i / W);
    for (int k = 0; k < 8; ++k) {
      int nx = x + DX[k], nz = z + DZ[k];
      if (!pass(nx, nz)) continue;
      if (k >= 4 && (!pass(nx, z) || !pass(x, nz))) continue;
      size_t j = idx(nx, nz);
      if (closed[j]) continue;
      float c = gcost[i] + (k >= 4 ? 1.41421356f : 1.0f);
      if (c < gcost[j]) {
        gcost[j] = c;
        parent[j] = static_cast<int32_t>(i);
        float hh = heur(nx, nz);
        open.push({c + hh, hh, static_cast<int32_t>(j)});
      }
    }
  }
  expanded += expandedHere;
  lastWork += static_cast<uint64_t>(expandedHere);
  size_t end = reached ? goal : best;
  if (end == s) return false;
  std::vector<size_t> cells;
  for (size_t c = end; c != s; c = static_cast<size_t>(parent[c])) cells.push_back(c);
  cells.push_back(s);
  std::reverse(cells.begin(), cells.end());
  // line-of-sight smoothing
  size_t anchor = 0;
  while (anchor + 1 < cells.size()) {
    size_t far = anchor + 1;
    int ax = static_cast<int>(cells[anchor] % W), az = static_cast<int>(cells[anchor] / W);
    for (size_t k = cells.size() - 1; k > anchor + 1; --k) {
      int bx = static_cast<int>(cells[k] % W), bz = static_cast<int>(cells[k] / W);
      if (LineOfSightP(pass, ax, az, bx, bz)) {
        far = k;
        break;
      }
    }
    int fx = static_cast<int>(cells[far] % W), fz = static_cast<int>(cells[far] / W);
    out->push_back({fx + hx, 0, fz + hz});
    anchor = far;
  }
  if (reached) out->back() = {gx + hx, to.y, gz + hz};
  return true;
}

}  // namespace moho
