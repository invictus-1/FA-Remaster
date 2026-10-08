// Pathfinding (see navigation.h).
#include "sim/navigation.h"

#include <algorithm>
#include <cmath>
#include <queue>

#include "sim/terrain.h"

namespace moho {

PathGrid::PathGrid(const TerrainMap& map, const NamedFootprint& fp, const bool* blocking) {
  w_ = map.width();
  h_ = map.height();
  cells_.assign(static_cast<size_t>(w_) * h_, 0);
  const int sx = std::max<int>(1, fp.sizeX), sz = std::max<int>(1, fp.sizeZ);
  const float water = map.hasWater ? map.waterElevation : -10000.0f;
  for (int z = 0; z < h_; ++z) {
    for (int x = 0; x < w_; ++x) {
      if (x + sx > w_ - 1 || z + sz > h_ - 1) continue;
      float lo = 1e30f, hi = -1e30f;
      bool blocked = false;
      for (int cz = z; cz <= z + sz && !blocked; ++cz)
        for (int cx = x; cx <= x + sx; ++cx) {
          float v = map.HeightAt(cx, cz);
          lo = std::min(lo, v);
          hi = std::max(hi, v);
          if (blocking && blocking[map.TerrainType(cx, cz)]) {
            blocked = true;
            break;
          }
        }
      if (blocked) continue;
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
      cells_[static_cast<size_t>(z) * w_ + x] = caps;
    }
  }
}

const PathGrid* Navigation::Grid(const NamedFootprint& fp) {
  if (!map_) return nullptr;
  std::string key = fp.name;
  key += '/' + std::to_string(fp.sizeX) + 'x' + std::to_string(fp.sizeZ) + '/' + std::to_string(fp.caps);
  auto it = grids_.find(key);
  if (it != grids_.end()) return it->second.get();
  auto g = std::make_unique<PathGrid>(*map_, fp, blocking_.data());
  const PathGrid* r = g.get();
  grids_[key] = std::move(g);
  return r;
}

bool Navigation::LineOfSight(const PathGrid& g, int x0, int z0, int x1, int z1) const {
  // every cell the segment between the two cell centres touches must be passable
  int dx = std::abs(x1 - x0), dz = std::abs(z1 - z0);
  int sx = x0 < x1 ? 1 : -1, sz = z0 < z1 ? 1 : -1;
  int x = x0, z = z0;
  int n = 1 + dx + dz;
  int err = dx - dz;
  dx *= 2;
  dz *= 2;
  for (; n > 0; --n) {
    if (!g.Passable(x, z)) return false;
    if (err > 0) {
      x += sx;
      err -= dz;
    } else if (err < 0) {
      z += sz;
      err += dx;
    } else {  // exactly through a corner: both neighbours must be free
      if (!g.Passable(x + sx, z) || !g.Passable(x, z + sz)) return false;
      x += sx;
      z += sz;
      err += dx - dz;
      --n;
    }
  }
  return true;
}

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
  if (!g->Passable(gx, gz)) {
    bool found = false;
    for (int r = 1; r <= 32 && !found; ++r)
      for (int dz = -r; dz <= r && !found; ++dz)
        for (int dx = -r; dx <= r; ++dx) {
          if (std::max(std::abs(dx), std::abs(dz)) != r) continue;
          if (g->Passable(gx + dx, gz + dz)) {
            gx += dx;
            gz += dz;
            found = true;
            break;
          }
        }
    if (!found) return false;
  }
  lastWork = static_cast<uint64_t>(std::max(std::abs(gx - sx), std::abs(gz - sz))) + 1;
  if (LineOfSight(*g, sx, sz, gx, gz)) {
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
      if (!g->Passable(nx, nz)) continue;
      if (k >= 4 && (!g->Passable(nx, z) || !g->Passable(x, nz))) continue;
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
      if (LineOfSight(*g, ax, az, bx, bz)) {
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
