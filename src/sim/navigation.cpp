// Pathfinding (see navigation.h).
#include "sim/navigation.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <queue>

#include "core/log.h"
#include "core/profile.h"
#include "core/workers.h"
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

// MobileCheckCapsRect's buffers, kept between calls: a run of small rects (the deferred terrain changes) allocates
// nothing once they have grown.
struct GridScratch {
  template <class T>
  struct Bufs {
    std::vector<T> rows, a, b;
  };
  std::vector<uint16_t> hv, lo, hi;
  std::vector<uint8_t> bl, anyBlocked;
  std::vector<float> dx, dz, stepX, stepZ;
  Bufs<uint16_t> b16;
  Bufs<uint8_t> b8;
  Bufs<float> bf;
};

namespace {
template <class T>
void Grow(std::vector<T>& v, size_t n) {
  if (v.size() < n) v.resize(n);  // (never shrinks: the buffers keep their largest size)
}

// out[i * step] = op over in[i * step .. (i + k - 1) * step] for i in [0, n - k]: the window's min or max by doubling
// (a[i] holds op over [i, i + len); the last step's two windows overlap, which an idempotent op allows), exact
// whatever the order, as min / max of exact values are.
template <class T, class Op>
void Window1D(const T* in, int n, int k, size_t step, T* out, std::vector<T>& a, std::vector<T>& b, Op op) {
  Grow(a, static_cast<size_t>(n));
  Grow(b, static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) a[i] = in[i * step];
  int len = 1;
  while (len * 2 <= k) {
    for (int i = 0; i + 2 * len <= n; ++i) b[i] = op(a[i], a[i + len]);
    a.swap(b);
    len *= 2;
  }
  for (int i = 0; i + k <= n; ++i) out[i * step] = op(a[i], a[i + k - len]);
}

// The window [x, x + kx) x [z, z + kz) of a W x H array (row stride W), for every (x, z) whose window lies inside:
// out is (W - kx + 1) x (H - kz + 1), row stride W - kx + 1. Rows first, then columns.
template <class T, class Op>
void Window2D(const T* in, int W, int H, int kx, int kz, std::vector<T>& out, GridScratch::Bufs<T>& bufs, Op op) {
  const int OW = W - kx + 1, OH = H - kz + 1;
  Grow(bufs.rows, static_cast<size_t>(OW) * H);
  for (int z = 0; z < H; ++z)
    Window1D(in + static_cast<size_t>(z) * W, W, kx, 1, &bufs.rows[static_cast<size_t>(z) * OW], bufs.a, bufs.b, op);
  Grow(out, static_cast<size_t>(OW) * OH);
  for (int x = 0; x < OW; ++x) Window1D(&bufs.rows[x], H, kz, static_cast<size_t>(OW), &out[x], bufs.a, bufs.b, op);
}

// MOHO64_DEBUG_GRIDCHECK=1: every grid built and every rect of terrain changes taken is compared, cell by cell, with
// MobileCheckCaps (the proof that MobileCheckCapsRect is exact), and CellCaps counts its lookups per key; both are
// logged when the Navigation goes. Off (the default): nothing is checked or counted.
bool GridCheck() {
  static const bool on = [] {
    const char* v = std::getenv("MOHO64_DEBUG_GRIDCHECK");
    return v && *v && *v != '0';
  }();
  return on;
}
struct GridCheckCounts {
  uint64_t grids = 0, rects = 0, cells = 0, diffs = 0;
};
GridCheckCounts g_check;  // (the debug switch only; written on the sim's own thread)

void CheckCells(const TerrainMap& map, const NamedFootprint& fp, const bool* blocking, int x0, int z0, int x1, int z1,
                const uint8_t* cells, const char* what) {
  const int w = map.width();
  uint64_t diffs = 0;
  for (int z = z0; z <= z1; ++z)
    for (int x = x0; x <= x1; ++x) {
      const uint8_t want = MobileCheckCaps(map, fp, blocking, x, z), got = cells[static_cast<size_t>(z) * w + x];
      ++g_check.cells;
      if (want == got) continue;
      if (++diffs <= 5)
        Logf(LogLevel::Info, "gridcheck: DIFF %s %s/%dx%d/%d at (%d, %d): fast %d, MobileCheckCaps %d", what,
             fp.name.c_str(), fp.sizeX, fp.sizeZ, fp.caps, x, z, got, want);
    }
  g_check.diffs += diffs;
}
}  // namespace

// MobileCheckCaps for every origin of [ox0, ox1] x [oz0, oz1] (inclusive; clamped to the map) into cells (row stride
// = the map's width), the same values bit for bit, at a cost per origin that hardly grows with the footprint (mp2's
// prototype, 2026-10-21, _scratch\engine\keep_20261021\mp2b\patches\grid_fast_core_excerpt.cpp). MobileCheckCaps'
// tests over the footprint's (sizeX + 1) x (sizeZ + 1) vertices are each a window's min or max, taken separably
// (rows, then columns):
// - the lowest and the highest height: the min / max of the raw values, scaled as HeightAt scales them afterwards
//   (raw -> raw * scale is monotonic, so the min of the scaled values is the scaled min: the same float);
// - a blocking terrain type at any covered cell: the max of 0 / 1;
// - the largest step between neighbours along x (cells x .. x + sizeX - 1, rows z .. z + sizeZ) and along z: the max
//   of the same fabs(HeightAt - HeightAt) floats MobileCheckCaps takes; it starts its max at 0, as here.
// Then the same tests on the same floats. An origin whose footprint does not fit inside the map is 0.
static void MobileCheckCapsRect(const TerrainMap& map, const NamedFootprint& fp, const bool* blocking, int ox0, int oz0,
                                int ox1, int oz1, uint8_t* cells, GridScratch& s) {
  const int w = map.width(), h = map.height();
  const int sx = std::max<int>(1, fp.sizeX), sz = std::max<int>(1, fp.sizeZ);
  ox0 = std::max(ox0, 0);
  oz0 = std::max(oz0, 0);
  ox1 = std::min(ox1, w - 1);
  oz1 = std::min(oz1, h - 1);
  if (ox1 < ox0 || oz1 < oz0) return;
  // the origins that can hold the footprint: x + sx <= w - 1, z + sz <= h - 1 (MobileCheckCaps' first test)
  const int ax1 = std::min(ox1, w - 1 - sx), az1 = std::min(oz1, h - 1 - sz);
  for (int z = oz0; z <= oz1; ++z)
    for (int x = (z <= az1 ? std::max(ox0, ax1 + 1) : ox0); x <= ox1; ++x) cells[static_cast<size_t>(z) * w + x] = 0;
  if (ax1 < ox0 || az1 < oz0) return;
  const int nx = ax1 - ox0 + 1, nz = az1 - oz0 + 1;
  const int VX = nx + sx, VZ = nz + sz;  // the vertices (and the cells, for the types) the windows cover
  const uint16_t* raw = map.heightData();
  const size_t rs = static_cast<size_t>(w) + 1;  // the heightfield's row stride
  const float scale = map.heightScale();
  const size_t nv = static_cast<size_t>(VX) * VZ;
  Grow(s.hv, nv);
  Grow(s.bl, nv);
  for (int j = 0; j < VZ; ++j) {
    const uint16_t* row = raw + static_cast<size_t>(oz0 + j) * rs + ox0;
    uint16_t* hv = &s.hv[static_cast<size_t>(j) * VX];
    uint8_t* bl = &s.bl[static_cast<size_t>(j) * VX];
    for (int i = 0; i < VX; ++i) {
      hv[i] = row[i];
      bl[i] = blocking && blocking[map.TerrainType(ox0 + i, oz0 + j)] ? 1 : 0;
    }
  }
  const auto mn16 = [](uint16_t a, uint16_t b) { return std::min(a, b); };
  const auto mx16 = [](uint16_t a, uint16_t b) { return std::max(a, b); };
  const auto mx8 = [](uint8_t a, uint8_t b) { return std::max(a, b); };
  const auto mxf = [](float a, float b) { return std::max(a, b); };
  Window2D(s.hv.data(), VX, VZ, sx + 1, sz + 1, s.lo, s.b16, mn16);
  Window2D(s.hv.data(), VX, VZ, sx + 1, sz + 1, s.hi, s.b16, mx16);
  Window2D(s.bl.data(), VX, VZ, sx + 1, sz + 1, s.anyBlocked, s.b8, mx8);
  if ((fp.caps & 3) && fp.maxSlope != 0.0f) {
    auto Hf = [&](int i, int j) { return raw[static_cast<size_t>(oz0 + j) * rs + ox0 + i] * scale; };  // HeightAt
    Grow(s.dx, static_cast<size_t>(VX - 1) * VZ);
    Grow(s.dz, static_cast<size_t>(VX) * (VZ - 1));
    for (int j = 0; j < VZ; ++j)
      for (int i = 0; i + 1 < VX; ++i) s.dx[static_cast<size_t>(j) * (VX - 1) + i] = std::fabs(Hf(i + 1, j) - Hf(i, j));
    for (int j = 0; j + 1 < VZ; ++j)
      for (int i = 0; i < VX; ++i) s.dz[static_cast<size_t>(j) * VX + i] = std::fabs(Hf(i, j + 1) - Hf(i, j));
    Window2D(s.dx.data(), VX - 1, VZ, sx, sz + 1, s.stepX, s.bf, mxf);  // cx in [x, x + sx), cz in [z, z + sz]
    Window2D(s.dz.data(), VX, VZ - 1, sx + 1, sz, s.stepZ, s.bf, mxf);  // cx in [x, x + sx], cz in [z, z + sz)
  }
  const float water = map.hasWater ? map.waterElevation : -10000.0f;
  for (int j = 0; j < nz; ++j) {
    uint8_t* out = cells + static_cast<size_t>(oz0 + j) * w + ox0;
    for (int i = 0; i < nx; ++i) {
      const size_t k = static_cast<size_t>(j) * nx + i;
      uint8_t caps = 0;
      if (!s.anyBlocked[k]) {
        const float lo = s.lo[k] * scale, hi = s.hi[k] * scale;
        caps = fp.caps;
        if (water - hi < fp.minWaterDepth) caps &= 0xf1;  // as MobileCheckCaps
        if (water - lo > fp.maxWaterDepth) caps &= 0xfc;
        if ((caps & 3) && fp.maxSlope != 0.0f) {
          const float step = std::max(0.0f, std::max(s.stepX[k], s.stepZ[k]));
          if (fp.maxSlope < step) caps &= 0xfc;
        }
      }
      out[i] = caps;
    }
  }
}

PathGrid::PathGrid(const TerrainMap& map, const NamedFootprint& fp, const bool* blocking, GridScratch* scratch,
                   WorkerPool* pool) {
  map_ = &map;
  blocking_ = blocking;
  scratch_ = scratch;
  w_ = map.width();
  h_ = map.height();
  cells_.assign(static_cast<size_t>(w_) * h_, 0);
  blocked_.assign(static_cast<size_t>(w_) * h_, 0);
  blockedWater_.assign(static_cast<size_t>(w_) * h_, 0);
  ignoreStructures = (fp.flags & 1) != 0;
  fp_ = fp;
  sizeX = std::max<int>(1, fp.sizeX);
  sizeZ = std::max<int>(1, fp.sizeZ);
  // In bands of rows: each band is MobileCheckCapsRect over its own origins (its windows read sizeZ vertex rows past
  // its end), so every cell is the same whichever thread takes its band, and however many threads there are.
  constexpr int kBand = 64;
  const size_t bands = static_cast<size_t>((h_ + kBand - 1) / kBand);
  auto band = [&](size_t b, GridScratch& s) {
    const int z0 = static_cast<int>(b) * kBand;
    MobileCheckCapsRect(map, fp, blocking, 0, z0, w_ - 1, std::min(h_ - 1, z0 + kBand - 1), cells_.data(), s);
  };
  if (pool && !InParallelTask()) {
    std::vector<GridScratch> per(static_cast<size_t>(pool->Threads()));  // (each worker slot's own buffers)
    pool->ParallelFor("path grid build", bands, 1, [&](size_t b, size_t e, int worker) {
      for (size_t i = b; i < e; ++i) band(i, per[static_cast<size_t>(worker)]);
    });
  } else {
    GridScratch local;
    for (size_t i = 0; i < bands; ++i) band(i, scratch ? *scratch : local);
  }
  if (GridCheck()) {
    ++g_check.grids;
    CheckCells(map, fp, blocking, 0, 0, w_ - 1, h_ - 1, cells_.data(), "grid");
  }
}

void PathGrid::TerrainChanged(int vx0, int vz0, int vx1, int vz1) {
  // an origin (x, z) covers the vertices [x, x + sizeX] x [z, z + sizeZ]
  Rect r{std::max(0, vx0 - sizeX), std::max(0, vz0 - sizeZ), std::min(w_ - 1, vx1), std::min(h_ - 1, vz1)};
  if (r.x0 > r.x1 || r.z0 > r.z1) return;
  // merged with a pending rect it overlaps or touches when their bounding box holds no more origins than the two
  // together (so a run of calls never takes more than the calls one by one; the box's extra origins, if any, take
  // caps that are already theirs, as caps depend on the map alone)
  auto area = [](const Rect& a) { return static_cast<long long>(a.x1 - a.x0 + 1) * (a.z1 - a.z0 + 1); };
  for (size_t i = 0; i < pending_.size();) {
    const Rect& p = pending_[i];
    const Rect u{std::min(r.x0, p.x0), std::min(r.z0, p.z0), std::max(r.x1, p.x1), std::max(r.z1, p.z1)};
    if (r.x0 <= p.x1 + 1 && p.x0 <= r.x1 + 1 && r.z0 <= p.z1 + 1 && p.z0 <= r.z1 + 1 && area(u) <= area(r) + area(p)) {
      r = u;
      pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
      i = 0;  // (the grown rect may meet one it passed)
    } else {
      ++i;
    }
  }
  pending_.push_back(r);
  pendingAny_ = true;
  if (pending_.size() > 16) ApplyTerrainChanges();  // (many separate places: take them now)
}

void PathGrid::ApplyTerrainChanges() const {
  if (!pendingAny_) return;
  GridScratch local;  // (only when the grid was made without Navigation's buffers)
  GridScratch& s = scratch_ ? *scratch_ : local;
  for (const Rect& r : pending_) {
    MobileCheckCapsRect(*map_, fp_, blocking_, r.x0, r.z0, r.x1, r.z1, cells_.data(), s);
    if (GridCheck()) {
      ++g_check.rects;
      CheckCells(*map_, fp_, blocking_, r.x0, r.z0, r.x1, r.z1, cells_.data(), "rect");
    }
  }
  pending_.clear();
  pendingAny_ = false;
}

void Navigation::TerrainChanged(int vx0, int vz0, int vx1, int vz1) {
  if (!map_) return;
  for (auto& [key, g] : grids_) g->TerrainChanged(vx0, vz0, vx1, vz1);
}

uint8_t Navigation::MobileCaps(const NamedFootprint& fp, int x, int z) const {
  return map_ ? MobileCheckCaps(*map_, fp, blocking_.data(), x, z) : 0;
}

namespace {
std::string GridKey(const NamedFootprint& fp) {
  std::string key = fp.name;
  key += '/' + std::to_string(fp.sizeX) + 'x' + std::to_string(fp.sizeZ) + '/' + std::to_string(fp.caps) + '/' +
         std::to_string(fp.flags & 1);
  return key;
}
}  // namespace

Navigation::Navigation(const TerrainMap* map) : map_(map), scratch_(std::make_unique<GridScratch>()) {
  blocking_.fill(false);
}

Navigation::~Navigation() {
  if (!GridCheck()) return;
  for (const auto& [key, n] : cellCounts_)
    Logf(LogLevel::Info, "gridcheck: lookups %s %llu (on its grid %llu, without %llu)%s", key.c_str(),
         static_cast<unsigned long long>(n.first + n.second), static_cast<unsigned long long>(n.first),
         static_cast<unsigned long long>(n.second), grids_.count(key) ? ", grid made" : "");
  Logf(LogLevel::Info, "gridcheck: %llu grids, %llu rects, %llu cells compared, %llu diffs",
       static_cast<unsigned long long>(g_check.grids), static_cast<unsigned long long>(g_check.rects),
       static_cast<unsigned long long>(g_check.cells), static_cast<unsigned long long>(g_check.diffs));
  std::fprintf(stderr, "gridcheck: %llu grids, %llu rects, %llu cells compared, %llu diffs\n",
               static_cast<unsigned long long>(g_check.grids), static_cast<unsigned long long>(g_check.rects),
               static_cast<unsigned long long>(g_check.cells), static_cast<unsigned long long>(g_check.diffs));
}

// STIMap::OccupancyCapsOfFootprintAt 0x564df0 (read 2026-10-23): OCCUPY_MobileCheck's tests on the one cell (x, z)
// whatever the footprint's size: the cell must be x < width, z < height (0x564e02..0x564e15: the heightfield's
// vertex count - 1); the type of that one cell only (0x577f20: map+0x1434[type]); the lowest / highest of its four
// vertices; the largest of its four edge steps (raw, 0x564f1c..0x564f52), scaled as the heights are. MinWaterDepth,
// MaxWaterDepth and MaxSlope as MobileCheck.
static uint8_t SingleCellCaps(const TerrainMap& map, const NamedFootprint& fp, const bool* blocking, int x, int z) {
  const int w = map.width(), h = map.height();
  if (static_cast<unsigned>(x) >= static_cast<unsigned>(w) || static_cast<unsigned>(z) >= static_cast<unsigned>(h))
    return 0;
  if (blocking && blocking[map.TerrainType(x, z)]) return 0;
  const float water = map.hasWater ? map.waterElevation : -10000.0f;
  const uint16_t* raw = map.heightData();
  const size_t rs = static_cast<size_t>(w) + 1, i = static_cast<size_t>(z) * rs + x;
  const int a = raw[i], b = raw[i + 1], c = raw[i + rs], d = raw[i + rs + 1];
  const int lo = std::min(std::min(a, b), std::min(c, d)), hi = std::max(std::max(a, b), std::max(c, d));
  const float scale = map.heightScale();
  uint8_t caps = fp.caps;
  if (water - hi * scale < fp.minWaterDepth) caps &= 0xf1;
  if (water - lo * scale > fp.maxWaterDepth) caps &= 0xfc;
  if ((caps & 3) && fp.maxSlope != 0.0f) {
    const int step = std::max(std::max(std::abs(d - c), std::abs(d - b)), std::max(std::abs(c - a), std::abs(b - a)));
    if (fp.maxSlope < step * scale) caps &= 0xfc;
  }
  return caps;
}

uint8_t Navigation::CellCaps(const NamedFootprint& fp, int x, int z, bool callerCapsAll) {
  if (!map_) return 0;
  if (callerCapsAll && std::max(fp.sizeX, fp.sizeZ) == 1) return SingleCellCaps(*map_, fp, blocking_.data(), x, z);
  const std::string key = GridKey(fp);
  auto it = grids_.find(key);
  if (GridCheck()) ++(it != grids_.end() ? cellCounts_[key].first : cellCounts_[key].second);
  if (it != grids_.end()) {
    const NamedFootprint& g = it->second->footprint();
    if (g.maxSlope == fp.maxSlope && g.minWaterDepth == fp.minWaterDepth && g.maxWaterDepth == fp.maxWaterDepth)
      return it->second->Caps(x, z);
  }
  return MobileCheckCaps(*map_, fp, blocking_.data(), x, z);
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

bool Navigation::WouldChangeCells(int x0, int z0, int x1, int z1, uint8_t fpCaps) const {
  const uint8_t occ = OccLayersOf(fpCaps);
  if (!map_ || x1 <= x0 || z1 <= z0 || !occ) return false;  // (AddStructure's own early return)
  for (int z = std::max(0, z0); z < std::min(map_->height(), z1); ++z)
    for (int x = std::max(0, x0); x < std::min(map_->width(), x1); ++x) {
      const size_t i = static_cast<size_t>(z) * map_->width() + x;
      if ((occ & kOccLand) && (occ_.empty() || occ_[i] != 1)) return true;
      if ((occ & kOccWater) && (occWater_.empty() || occWater_[i] != 1)) return true;
    }
  return false;
}

void Navigation::AddStructure(uint32_t entity, int x0, int z0, int x1, int z1, uint8_t fpCaps, bool append) {
  const uint8_t occ = OccLayersOf(fpCaps);
  if (!map_ || x1 <= x0 || z1 <= z0 || !occ) return;
  if (!append) ForgetStructure(entity);  // (occupying again: one record per entity)
  structures_.push_back({entity, x0, z0, x1, z1, occ});
  SetCells(x0, z0, x1, z1, occ, true);
}

void Navigation::RemoveStructure(uint32_t entity, const int* rect) {
  std::vector<OccupiedRect> mine;
  for (size_t i = 0; i < structures_.size();) {
    if (structures_[i].entity != entity) {
      ++i;
      continue;
    }
    mine.push_back(structures_[i]);
    structures_.erase(structures_.begin() + static_cast<long>(i));
  }
  if (mine.empty()) return;
  if (rect) {
    uint8_t occ = 0;
    for (const OccupiedRect& r : mine) occ |= r.occ;
    SetCells(rect[0], rect[1], rect[2], rect[3], occ, false);
  } else {
    for (const OccupiedRect& r : mine) SetCells(r.x0, r.z0, r.x1, r.z1, r.occ, false);
  }
}

void Navigation::ForgetStructure(uint32_t entity) {
  structures_.erase(std::remove_if(structures_.begin(), structures_.end(),
                                   [&](const OccupiedRect& r) { return r.entity == entity; }),
                    structures_.end());
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
  const std::string key = GridKey(fp);
  auto it = grids_.find(key);
  if (it != grids_.end()) return it->second.get();
  prof::SectionTimer sec(prof::kSecPathGridMade);  // (MOHO64_PROFILE_HITCH only: which grid, and its time)
  const double hitchT0 = prof::Hitch() ? prof::NowMs() : 0;
  auto g = std::make_unique<PathGrid>(*map_, fp, blocking_.data(), scratch_.get(), pool_);
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
