// FA Remastered's own AI, native core, phase 1: the map analysis (see map_analysis.h).
//
// Part of FA Remastered's engine, MIT licence (see LICENSE). Our own design and code (docs/ai-architecture.md 0).
#include "ai/map_analysis.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <queue>
#include <utility>

#include "core/log.h"
#include "sim/blueprints.h"
#include "sim/navigation.h"
#include "sim/sim.h"
#include "sim/terrain.h"
#include "zlib.h"

namespace moho {
namespace ai {

namespace {

// The tuning of phase 1 (docs/ai-architecture.md 3.1; OPEN: to tune on more maps than SETONS_R).
constexpr float kMassGroupLink = 30.0f;   // mass points this close (world units) on one island make one group
constexpr float kMassZoneRadius = 56.0f;  // a mass zone grows this far (travel) from its seed
constexpr float kPlainZoneRadius = 64.0f;  // a plain land zone
constexpr float kWaterZoneRadius = 96.0f;  // a water zone
constexpr int kMinLandArea = 8;    // segments: smaller islands without mass get no zone of their own
constexpr int kMinWaterArea = 16;  // segments: smaller ponds get no zone of their own
constexpr int kMinLandZone = 40;   // segments: a smaller plain land zone joins a neighbour (6b)
constexpr int kMinWaterZone = 80;  // segments: a smaller water zone joins a neighbour
constexpr int kChokeWidth = 6;     // segment pairs: a border at most this wide may be a chokepoint
constexpr float kChokeDetour = 1.5f;  // ... when the way round without it is at least this many times longer
constexpr float kBuildVariation = 1.0f;  // the default MaxGroundVariation
constexpr float kStraight = 4.0f, kDiagonal = 5.656854f;  // step costs on the segment grid (world units)
constexpr float kInf = std::numeric_limits<float>::infinity();

// The domain's footprint: the game's named one when the rules have it, else the stock footprints.lua values.
NamedFootprint DomainFootprint(const Sim& sim, int d, std::string* note) {
  static const char* kNames[kDomains] = {"Vehicle1x1", "Water1x1", "WaterLand1x1", "Amphibious1x1"};
  for (const NamedFootprint& f : sim.blueprints().Footprints())
    if (f.name == kNames[d]) {
      *note += std::string(" ") + kNames[d];
      return f;
    }
  NamedFootprint f;
  f.name = kNames[d];
  f.sizeX = f.sizeZ = 1;
  switch (d) {
    case kLand: f.caps = kLayerLand; f.maxWaterDepth = 0.05f; f.maxSlope = 0.75f; break;
    case kNaval: f.caps = kLayerWater; f.minWaterDepth = 1.5f; break;
    case kHover: f.caps = kLayerLand | kLayerWater; f.maxWaterDepth = 1; f.minWaterDepth = 0.1f; f.maxSlope = 0.75f; break;
    default: f.caps = kLayerLand | kLayerSeabed; f.maxWaterDepth = 25; f.maxSlope = 0.75f; break;
  }
  *note += std::string(" ") + kNames[d] + "(built-in)";
  return f;
}

uint8_t DomainLayers(int d) {
  switch (d) {
    case kLand: return kLayerLand;
    case kNaval: return kLayerWater;
    case kHover: return kLayerLand | kLayerWater;
    default: return kLayerLand | kLayerSeabed;
  }
}

struct Grid {
  int sw, sh;
  // The 8 neighbours of s that pass `ok`, with their step costs; a diagonal step needs both straight steps beside
  // it to pass (no corner cutting). Fixed order: E, W, S, N, then the diagonals.
  template <class Ok, class F>
  void Neighbours(int s, Ok&& ok, F&& f) const {
    const int x = s % sw, z = s / sw;
    static const int dx[8] = {1, -1, 0, 0, 1, -1, 1, -1}, dz[8] = {0, 0, 1, -1, 1, 1, -1, -1};
    for (int k = 0; k < 8; ++k) {
      const int nx = x + dx[k], nz = z + dz[k];
      if (nx < 0 || nz < 0 || nx >= sw || nz >= sh) continue;
      const int t = nz * sw + nx;
      if (!ok(t)) continue;
      if (k >= 4 && (!ok(z * sw + nx) || !ok(nz * sw + x))) continue;
      f(t, k < 4 ? kStraight : kDiagonal);
    }
  }
};

using QItem = std::pair<float, int>;  // (cost, segment): ties go to the lower segment index
using MinQueue = std::priority_queue<QItem, std::vector<QItem>, std::greater<QItem>>;

// Multi-source growth: every segment that passes `ok`, within `cap` of a source, takes the nearest source's label
// (lower cost first, then the source met first in queue order). dist / label hold the result; segments already
// labelled (label != 0 at the start, not a source) are not entered.
template <class Ok>
void Grow(const Grid& g, Ok&& ok, const std::vector<std::pair<int, int>>& sources, float cap, std::vector<float>& dist,
          std::vector<int>& label) {
  MinQueue q;
  for (const auto& [s, l] : sources) {
    dist[s] = 0;
    label[s] = l;
    q.push({0.0f, s});
  }
  while (!q.empty()) {
    const auto [c, s] = q.top();
    q.pop();
    if (c > dist[s]) continue;
    g.Neighbours(s, ok, [&](int t, float step) {
      const float nc = c + step;
      if (nc > cap) return;
      if (label[t] != 0 && dist[t] == kInf) return;  // another's, fixed earlier
      if (nc < dist[t]) {
        dist[t] = nc;
        label[t] = label[s];
        q.push({nc, t});
      }
    });
  }
}

}  // namespace

const char* DomainName(int d) {
  static const char* n[kDomains] = {"land", "naval", "hover", "amphibious"};
  return d >= 0 && d < kDomains ? n[d] : "?";
}

int DomainFromName(const char* s) {
  if (!s) return -1;
  for (int d = 0; d < kDomains; ++d)
    if (!std::strcmp(s, DomainName(d))) return d;
  return -1;
}

int MapAnalysis::Seg(float x, float z) const {
  if (!(x >= 0) || !(z >= 0)) return -1;
  const int sx = static_cast<int>(x) / kSeg, sz = static_cast<int>(z) / kSeg;
  if (sx >= sw || sz >= sh) return -1;
  return sz * sw + sx;
}

int MapAnalysis::ZoneAt(float x, float z, int domain) const {
  const int s = Seg(x, z);
  if (s < 0) return 0;
  if (domain >= 0 && !(pass[s] & (1u << domain))) return 0;
  return zoneOf[s];
}

float MapAnalysis::Distance(int a, int b, int domain) const {
  const int n = static_cast<int>(zones.size());
  if (a < 1 || b < 1 || a > n || b > n || domain < 0 || domain >= kDomains) return -1;
  return dist[domain][static_cast<size_t>(a - 1) * n + (b - 1)];
}

const ZoneLink* MapAnalysis::Link(int a, int b) const {
  if (a < 1 || a > static_cast<int>(zones.size())) return nullptr;
  for (int li : zones[a - 1].links)
    if (links[li].a == b || links[li].b == b) return &links[li];
  return nullptr;
}

namespace {

void Build(Sim& sim, MapAnalysis& m) {
  const TerrainMap& map = *sim.map();
  m.mapW = map.width();
  m.mapH = map.height();
  m.sw = (m.mapW + MapAnalysis::kSeg - 1) / MapAnalysis::kSeg;
  m.sh = (m.mapH + MapAnalysis::kSeg - 1) / MapAnalysis::kSeg;
  const int S = MapAnalysis::kSeg, sw = m.sw, sh = m.sh, N = sw * sh;
  const Grid g{sw, sh};
  const Navigation& nav = sim.navigation();

  // 1. Passability: per cell (land kept per cell for the build spots), per segment by majority.
  NamedFootprint fp[kDomains];
  for (int d = 0; d < kDomains; ++d) fp[d] = DomainFootprint(sim, d, &m.footprintNote);
  std::vector<uint8_t> landCell(static_cast<size_t>(m.mapW) * m.mapH, 0);
  m.pass.assign(N, 0);
  for (int s = 0; s < N; ++s) {
    const int x0 = (s % sw) * S, z0 = (s / sw) * S;
    int count[kDomains] = {};
    for (int z = z0; z < z0 + S && z < m.mapH; ++z)
      for (int x = x0; x < x0 + S && x < m.mapW; ++x)
        for (int d = 0; d < kDomains; ++d) {
          const bool ok = (nav.MobileCaps(fp[d], x, z) & DomainLayers(d)) != 0;
          count[d] += ok;
          if (d == kLand && ok) landCell[static_cast<size_t>(z) * m.mapW + x] = 1;
        }
    for (int d = 0; d < kDomains; ++d)
      if (count[d] * 2 >= S * S) m.pass[s] |= static_cast<uint8_t>(1u << d);
  }
  auto passes = [&](int d) { return [&m, d](int t) { return (m.pass[t] >> d) & 1; }; };

  // 2. Connected areas per domain, numbered in order of their first segment.
  for (int d = 0; d < kDomains; ++d) {
    auto ok = passes(d);
    std::vector<int>& a = m.area[d];
    a.assign(N, 0);
    m.areaSize[d].assign(1, 0);
    std::vector<int> stack;
    for (int s = 0; s < N; ++s) {
      if (a[s] || !ok(s)) continue;
      const int id = static_cast<int>(m.areaSize[d].size());
      m.areaSize[d].push_back(0);
      a[s] = id;
      stack.push_back(s);
      while (!stack.empty()) {
        const int c = stack.back();
        stack.pop_back();
        ++m.areaSize[d][id];
        g.Neighbours(c, ok, [&](int t, float) {
          if (!a[t]) {
            a[t] = id;
            stack.push_back(t);
          }
        });
      }
    }
  }

  // 3. Deposits (CreateResourceDeposit's cells, by their centre).
  for (const ResourceDeposit& r : sim.deposits) {
    ZoneDeposit d;
    d.x = 0.5f * static_cast<float>(r.x0 + r.x1);
    d.z = 0.5f * static_cast<float>(r.z0 + r.z1);
    d.type = r.type;
    m.deposits.push_back(d);
  }

  // 4. Land zones. Mass groups: single link within kMassGroupLink, on one island.
  std::vector<int> label(N, 0);
  std::vector<float> dist(N, kInf);
  std::vector<int> zoneDomain(1, -1), zoneSeed(1, -1);  // by zone id (index 0 unused)
  std::vector<bool> zoneMass(1, false);
  auto nearestPassable = [&](int d, int s, int maxRing) {
    if (s < 0) return -1;
    if ((m.pass[s] >> d) & 1) return s;
    const int x = s % sw, z = s / sw;
    for (int r = 1; r <= maxRing; ++r) {  // ring by ring, in row order: the first passable one
      int best = -1;
      float bd = kInf;
      for (int zz = z - r; zz <= z + r; ++zz)
        for (int xx = x - r; xx <= x + r; ++xx) {
          if (xx < 0 || zz < 0 || xx >= sw || zz >= sh) continue;
          if (std::max(std::abs(xx - x), std::abs(zz - z)) != r) continue;
          const int t = zz * sw + xx;
          if (!((m.pass[t] >> d) & 1)) continue;
          const float dd = static_cast<float>((xx - x) * (xx - x) + (zz - z) * (zz - z));
          if (dd < bd) {
            bd = dd;
            best = t;
          }
        }
      if (best >= 0) return best;
    }
    return -1;
  };
  {
    std::vector<int> massIdx, massSeg;
    for (int i = 0; i < static_cast<int>(m.deposits.size()); ++i)
      if (m.deposits[i].type == 1) {
        const int s = nearestPassable(kLand, m.Seg(m.deposits[i].x, m.deposits[i].z), 4);
        if (s >= 0) {
          massIdx.push_back(i);
          massSeg.push_back(s);
        }
      }
    const int nm = static_cast<int>(massIdx.size());
    std::vector<int> group(nm, -1);
    int groups = 0;
    for (int i = 0; i < nm; ++i) {  // flood over the link graph, groups numbered by their first member
      if (group[i] >= 0) continue;
      std::vector<int> st{i};
      group[i] = groups;
      while (!st.empty()) {
        const int c = st.back();
        st.pop_back();
        const ZoneDeposit& dc = m.deposits[massIdx[c]];
        for (int j = 0; j < nm; ++j) {
          if (group[j] >= 0) continue;
          const ZoneDeposit& dj = m.deposits[massIdx[j]];
          const float dx = dc.x - dj.x, dz = dc.z - dj.z;
          if (dx * dx + dz * dz <= kMassGroupLink * kMassGroupLink &&
              m.area[kLand][massSeg[c]] == m.area[kLand][massSeg[j]]) {
            group[j] = groups;
            st.push_back(j);
          }
        }
      }
      ++groups;
    }
    std::vector<std::pair<int, int>> sources;
    for (int gi = 0; gi < groups; ++gi) {
      float mx = 0, mz = 0;
      int n = 0, first = -1;
      for (int i = 0; i < nm; ++i)
        if (group[i] == gi) {
          mx += m.deposits[massIdx[i]].x;
          mz += m.deposits[massIdx[i]].z;
          ++n;
          if (first < 0) first = i;
        }
      mx /= static_cast<float>(n);
      mz /= static_cast<float>(n);
      int s = nearestPassable(kLand, m.Seg(mx, mz), 4);
      if (s < 0 || m.area[kLand][s] != m.area[kLand][massSeg[first]]) s = massSeg[first];
      if (label[s]) continue;  // two groups on one segment: one zone
      const int id = static_cast<int>(zoneDomain.size());
      zoneDomain.push_back(kLand);
      zoneSeed.push_back(s);
      zoneMass.push_back(true);
      sources.push_back({s, id});
      label[s] = id;
    }
    for (auto& [s, l] : sources) label[s] = 0;  // (Grow sets them)
    Grow(g, passes(kLand), sources, kMassZoneRadius, dist, label);
  }

  // 5. Farthest-point zones over what is left of a domain: the unzoned segment farthest from every zone (ties: the
  // lowest index) seeds a zone that grows over unzoned segments within `radius`; again until none is left.
  auto farthestZones = [&](int d, float radius, int minArea) {
    auto ok = passes(d);
    std::vector<float> far(N, kInf);  // distance (domain d) to the nearest zoned segment
    MinQueue q;
    auto relax = [&]() {
      while (!q.empty()) {
        const auto [c, s] = q.top();
        q.pop();
        if (c > far[s]) continue;
        g.Neighbours(s, ok, [&](int t, float step) {
          if (c + step < far[t]) {
            far[t] = c + step;
            q.push({c + step, t});
          }
        });
      }
    };
    for (int s = 0; s < N; ++s)
      if (label[s] && ok(s)) {
        far[s] = 0;
        q.push({0.0f, s});
      }
    relax();
    for (;;) {
      int best = -1;
      for (int s = 0; s < N; ++s) {
        if (label[s] || !ok(s)) continue;
        if (m.areaSize[d][m.area[d][s]] < minArea) continue;
        if (best < 0 || far[s] > far[best]) best = s;
      }
      if (best < 0) break;
      const int id = static_cast<int>(zoneDomain.size());
      zoneDomain.push_back(d);
      zoneSeed.push_back(best);
      zoneMass.push_back(false);
      std::vector<float> local(N, kInf);
      std::vector<int> mine(N, 0);
      // grow over unzoned segments only
      auto okFree = [&](int t) { return ok(t) && !label[t]; };
      Grow(g, okFree, {{best, id}}, radius, local, mine);
      for (int s = 0; s < N; ++s)
        if (mine[s] == id) {
          label[s] = id;
          far[s] = 0;
          q.push({0.0f, s});
        }
      relax();
    }
  };
  farthestZones(kLand, kPlainZoneRadius, kMinLandArea);
  // Mass on a small island that no mass zone took (none expected) stays unzoned.

  // 6. Water zones (segments a ship stands on that no land zone holds).
  farthestZones(kNaval, kWaterZoneRadius, kMinWaterArea);

  // 6b. Slivers: the bounded growth leaves thin leftovers between grown zones, which the farthest-point step makes
  // into small zones with narrow borders (false chokepoints). A plain zone below the domain's minimum size goes into
  // its same-domain neighbour with the longest border (ties: the lower id); the smallest first (ties: the lower id),
  // until none is left. Then the zones are numbered again in their old order.
  {
    const int n = static_cast<int>(zoneDomain.size());
    std::vector<char> keep(n, 0);  // too small but no same-domain neighbour: stays
    for (;;) {
      std::vector<int> size(n, 0);
      for (int s = 0; s < N; ++s) size[label[s]]++;
      int victim = 0;
      for (int id = 1; id < n; ++id) {
        if (zoneDomain[id] < 0 || zoneMass[id] || keep[id]) continue;
        if (size[id] >= (zoneDomain[id] == kLand ? kMinLandZone : kMinWaterZone)) continue;
        if (!victim || size[id] < size[victim]) victim = id;
      }
      if (!victim) break;
      std::vector<int> border(n, 0);
      for (int s = 0; s < N; ++s) {
        if (label[s] != victim) continue;
        const int x = s % sw, z = s / sw;
        const int nb[4] = {x + 1 < sw ? s + 1 : -1, x > 0 ? s - 1 : -1, z + 1 < sh ? s + sw : -1, z > 0 ? s - sw : -1};
        for (int t : nb)
          if (t >= 0 && label[t] && label[t] != victim && zoneDomain[label[t]] == zoneDomain[victim]) border[label[t]]++;
      }
      int into = 0;
      for (int id = 1; id < n; ++id)
        if (border[id] > (into ? border[into] : 0)) into = id;
      if (!into) {
        keep[victim] = 1;
        continue;
      }
      for (int s = 0; s < N; ++s)
        if (label[s] == victim) label[s] = into;
      zoneDomain[victim] = -1;  // gone
    }
    std::vector<int> renum(n, 0);
    std::vector<int> nd(1, -1), ns(1, -1);
    std::vector<bool> nm(1, false);
    for (int id = 1; id < n; ++id) {
      if (zoneDomain[id] < 0) continue;
      renum[id] = static_cast<int>(nd.size());
      nd.push_back(zoneDomain[id]);
      ns.push_back(zoneSeed[id]);
      nm.push_back(zoneMass[id]);
    }
    for (int s = 0; s < N; ++s) label[s] = renum[label[s]];
    zoneDomain = nd;
    zoneSeed = ns;
    zoneMass = nm;
  }

  // 7. Segments only hover / amphibious units reach (shallows, seabed, tiny islands): the nearest zone through them.
  {
    auto okAny = [&](int t) { return (m.pass[t] & ((1u << kHover) | (1u << kAmphibious) | 1u | 2u)) != 0; };
    std::vector<std::pair<int, int>> sources;
    std::vector<float> d2(N, kInf);
    std::vector<int> l2(N, 0);
    for (int s = 0; s < N; ++s)
      if (label[s]) sources.push_back({s, label[s]});
    Grow(g, okAny, sources, kInf, d2, l2);
    for (int s = 0; s < N; ++s)
      if (!label[s] && l2[s]) label[s] = l2[s];
  }
  m.zoneOf = label;

  // 8. The zones: segments, centre, anchors, deposits, starts.
  const int nz = static_cast<int>(zoneDomain.size()) - 1;
  m.zones.resize(nz);
  std::vector<double> sx(nz + 1, 0), sz(nz + 1, 0);
  for (int i = 1; i <= nz; ++i) {
    Zone& z = m.zones[i - 1];
    z.id = i;
    z.domain = zoneDomain[i];
    z.massSeeded = zoneMass[i];
    z.area = m.area[z.domain][zoneSeed[i]];
  }
  for (int s = 0; s < N; ++s)
    if (const int id = label[s]) {
      ++m.zones[id - 1].segments;
      sx[id] += (s % sw) * S + 0.5 * S;
      sz[id] += (s / sw) * S + 0.5 * S;
    }
  std::vector<double> bestC(nz + 1, 1e300);
  std::vector<double> bestA[kDomains];
  for (int d = 0; d < kDomains; ++d) bestA[d].assign(nz + 1, 1e300);
  for (int s = 0; s < N; ++s) {
    const int id = label[s];
    if (!id) continue;
    Zone& z = m.zones[id - 1];
    const double mx = sx[id] / z.segments, mz = sz[id] / z.segments;
    const double cx = (s % sw) * S + 0.5 * S, cz = (s / sw) * S + 0.5 * S;
    const double dd = (cx - mx) * (cx - mx) + (cz - mz) * (cz - mz);
    if (dd < bestC[id] && ((m.pass[s] >> z.domain) & 1)) {
      bestC[id] = dd;
      z.centreSeg = s;
    }
    for (int d = 0; d < kDomains; ++d)
      if (((m.pass[s] >> d) & 1) && dd < bestA[d][id]) {
        bestA[d][id] = dd;
        z.anchor[d] = s;
      }
  }
  for (Zone& z : m.zones) {
    if (z.centreSeg < 0) z.centreSeg = zoneSeed[z.id];
    z.x = static_cast<float>((z.centreSeg % sw) * S) + 0.5f * S;
    z.z = static_cast<float>((z.centreSeg / sw) * S) + 0.5f * S;
  }
  for (int i = 0; i < static_cast<int>(m.deposits.size()); ++i) {
    ZoneDeposit& d = m.deposits[i];
    const int s = m.Seg(d.x, d.z);
    d.zone = s >= 0 ? label[s] : 0;
    if (!d.zone) continue;
    (d.type == 1 ? m.zones[d.zone - 1].mass : m.zones[d.zone - 1].hydro).push_back(i);
  }
  for (const auto& a : sim.armies())
    if (a->hasStart && !a->civilian) {
      const int s = m.Seg(a->startX, a->startZ);
      if (s >= 0 && label[s]) m.zones[label[s] - 1].starts.push_back(a->index);
    }

  // 9. Links: 4-neighbour border pairs give the width and midpoint; links in (a, b) order.
  {
    struct Acc {
      int a, b, w;
      double x, z;
    };
    std::vector<Acc> pairs;
    for (int s = 0; s < N; ++s) {
      const int za = label[s];
      if (!za) continue;
      const int x = s % sw, z = s / sw;
      const int nb[2] = {x + 1 < sw ? s + 1 : -1, z + 1 < sh ? s + sw : -1};
      for (int k = 0; k < 2; ++k) {
        const int t = nb[k];
        if (t < 0 || !label[t] || label[t] == za) continue;
        const int a = std::min(za, label[t]), b = std::max(za, label[t]);
        const double mx = (x * S + 0.5 * S) + (k == 0 ? 0.5 * S : 0), mz = (z * S + 0.5 * S) + (k == 1 ? 0.5 * S : 0);
        pairs.push_back({a, b, 1, mx, mz});
      }
    }
    std::stable_sort(pairs.begin(), pairs.end(), [](const Acc& p, const Acc& q) {
      return p.a != q.a ? p.a < q.a : p.b < q.b;
    });
    for (size_t i = 0; i < pairs.size();) {
      size_t j = i;
      double x = 0, z = 0;
      while (j < pairs.size() && pairs[j].a == pairs[i].a && pairs[j].b == pairs[i].b) {
        x += pairs[j].x;
        z += pairs[j].z;
        ++j;
      }
      ZoneLink l;
      l.a = pairs[i].a;
      l.b = pairs[i].b;
      l.width = static_cast<int>(j - i);
      l.x = static_cast<float>(x / l.width);
      l.z = static_cast<float>(z / l.width);
      l.coast = m.zones[l.a - 1].domain != m.zones[l.b - 1].domain;
      m.links.push_back(l);
      i = j;
    }
    for (int li = 0; li < static_cast<int>(m.links.size()); ++li) {
      m.zones[m.links[li].a - 1].links.push_back(li);
      m.zones[m.links[li].b - 1].links.push_back(li);
    }
    for (Zone& z : m.zones)  // by neighbour id
      std::stable_sort(z.links.begin(), z.links.end(), [&](int p, int q) {
        const int np = m.links[p].a == z.id ? m.links[p].b : m.links[p].a;
        const int nq = m.links[q].a == z.id ? m.links[q].b : m.links[q].a;
        return np < nq;
      });
  }

  // 10. Neighbour distances per domain: from a's anchor to b's through the segments of a and b.
  {
    std::vector<float> dd(N, kInf);
    std::vector<int> touched;
    for (ZoneLink& l : m.links)
      for (int d = 0; d < kDomains; ++d) {
        const int from = m.zones[l.a - 1].anchor[d], to = m.zones[l.b - 1].anchor[d];
        if (from < 0 || to < 0) continue;
        auto ok = [&](int t) { return ((m.pass[t] >> d) & 1) && (label[t] == l.a || label[t] == l.b); };
        MinQueue q;
        dd[from] = 0;
        touched.push_back(from);
        q.push({0.0f, from});
        while (!q.empty()) {
          const auto [c, s] = q.top();
          q.pop();
          if (c > dd[s]) continue;
          if (s == to) break;
          g.Neighbours(s, ok, [&](int t, float step) {
            if (c + step < dd[t]) {
              if (dd[t] == kInf) touched.push_back(t);
              dd[t] = c + step;
              q.push({c + step, t});
            }
          });
        }
        if (dd[to] != kInf) l.dist[d] = dd[to];
        for (int t : touched) dd[t] = kInf;
        touched.clear();
      }
  }

  // 11. Every pair per domain over the zone graph (Dijkstra from each zone; a zone a domain cannot stand in is no
  // node for it).
  auto zoneGraphDist = [&](int d, int from, int skipLink, std::vector<float>& out) {
    out.assign(nz, kInf);
    if (m.zones[from - 1].anchor[d] < 0) return;
    MinQueue q;
    out[from - 1] = 0;
    q.push({0.0f, from});
    while (!q.empty()) {
      const auto [c, z] = q.top();
      q.pop();
      if (c > out[z - 1]) continue;
      for (int li : m.zones[z - 1].links) {
        if (li == skipLink) continue;
        const ZoneLink& l = m.links[li];
        if (l.dist[d] < 0) continue;
        const int o = l.a == z ? l.b : l.a;
        const float nc = c + l.dist[d];
        if (nc < out[o - 1]) {
          out[o - 1] = nc;
          q.push({nc, o});
        }
      }
    }
  };
  {
    std::vector<float> row;
    for (int d = 0; d < kDomains; ++d) {
      m.dist[d].assign(static_cast<size_t>(nz) * nz, -1.0f);
      for (int a = 1; a <= nz; ++a) {
        zoneGraphDist(d, a, -1, row);
        for (int b = 1; b <= nz; ++b)
          if (row[b - 1] != kInf) m.dist[d][static_cast<size_t>(a - 1) * nz + (b - 1)] = row[b - 1];
      }
    }
  }

  // 12. Chokepoints: a narrow link in its zones' own domain without a short way round.
  {
    std::vector<float> row;
    for (int li = 0; li < static_cast<int>(m.links.size()); ++li) {
      ZoneLink& l = m.links[li];
      if (l.coast || l.width > kChokeWidth) continue;
      const int d = m.zones[l.a - 1].domain;
      if (l.dist[d] < 0) continue;
      zoneGraphDist(d, l.a, li, row);
      const float round = row[l.b - 1];
      l.choke = round == kInf || round >= kChokeDetour * l.dist[d];
    }
  }

  // 13. Cut zones: a land zone whose removal leaves its island's other land zones in more than one piece.
  for (Zone& z : m.zones) {
    if (z.domain != kLand) continue;
    std::vector<int> nb;
    for (int li : z.links) {
      const ZoneLink& l = m.links[li];
      if (l.coast || l.dist[kLand] < 0) continue;
      nb.push_back(l.a == z.id ? l.b : l.a);
    }
    if (nb.size() < 2) continue;
    std::vector<char> seen(nz + 1, 0);
    seen[z.id] = 1;
    std::vector<int> st{nb[0]};
    seen[nb[0]] = 1;
    while (!st.empty()) {
      const int c = st.back();
      st.pop_back();
      for (int li : m.zones[c - 1].links) {
        const ZoneLink& l = m.links[li];
        if (l.coast || l.dist[kLand] < 0) continue;
        const int o = l.a == c ? l.b : l.a;
        if (!seen[o]) {
          seen[o] = 1;
          st.push_back(o);
        }
      }
    }
    for (int o : nb)
      if (!seen[o]) z.cut = true;
  }

  // 14. Build spots (terrain only): the largest square at the segment's corner.
  {
    std::vector<uint8_t> depositCell(static_cast<size_t>(m.mapW) * m.mapH, 0);
    for (const ResourceDeposit& r : sim.deposits)
      for (int z = std::max(0, r.z0); z < std::min(m.mapH, r.z1); ++z)
        for (int x = std::max(0, r.x0); x < std::min(m.mapW, r.x1); ++x) depositCell[static_cast<size_t>(z) * m.mapW + x] = 1;
    m.buildSpots.assign(nz, {});
    static const int sizes[4] = {16, 8, 4, 2};
    for (int s = 0; s < N; ++s) {
      const int id = label[s];
      if (!id || m.zones[id - 1].domain != kLand) continue;
      const int x0 = (s % sw) * S, z0 = (s / sw) * S;
      for (int size : sizes) {
        if (x0 + size >= m.mapW || z0 + size >= m.mapH) continue;
        bool fits = true;
        float lo = kInf, hi = -kInf;
        for (int z = z0; z <= z0 + size && fits; ++z)
          for (int x = x0; x <= x0 + size; ++x) {
            const float h = map.HeightAt(x, z);
            lo = std::min(lo, h);
            hi = std::max(hi, h);
            if (x < x0 + size && z < z0 + size) {
              const size_t c = static_cast<size_t>(z) * m.mapW + x;
              if (!landCell[c] || depositCell[c]) {
                fits = false;
                break;
              }
            }
          }
        if (fits && hi - lo <= kBuildVariation) {
          m.buildSpots[id - 1].push_back({s, static_cast<uint8_t>(size)});
          break;
        }
      }
    }
  }
}

// The session's analysis, kept on the Sim.
struct MapState {
  bool made = false;
  MapAnalysis m;
};

MapState& StateOf(Sim& sim) {
  if (!sim.aiMapState) sim.aiMapState = std::make_shared<MapState>();
  return *std::static_pointer_cast<MapState>(sim.aiMapState);
}

}  // namespace

bool SessionHasOurAi(const Sim& sim) {
  for (const auto& a : sim.armies())
    if (!a->human && a->personality.compare(0, 10, "remastered") == 0) return true;
  return false;
}

const MapAnalysis& MapOf(Sim& sim) {
  MapState& st = StateOf(sim);
  if (!st.made && sim.map()) {
    const auto t0 = std::chrono::steady_clock::now();
    Build(sim, st.m);
    st.m.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    int land = 0, water = 0;
    for (const Zone& z : st.m.zones) (z.domain == kLand ? land : water)++;
    Logf(LogLevel::Info, "moho64 ai: map analysis %d x %d segments, %d land + %d water zones, %zu links, %.1f ms",
         st.m.sw, st.m.sh, land, water, st.m.links.size(), st.m.ms);
  }
  st.made = true;
  return st.m;
}

void AfterSessionStart(Sim& sim) {
  const char* dump = std::getenv("MOHO64_AI_MAPDUMP");
  const bool ours = sim.fixes && SessionHasOurAi(sim);
  if (!ours && !(dump && *dump)) return;
  if (dump && *dump && !sim.HasNavigation())
    Logf(LogLevel::Warning, "moho64 ai: MOHO64_AI_MAPDUMP makes the navigation now (its terrain types are read from "
                            "Lua): this debug run may differ from one without the dump");
  const MapAnalysis& m = MapOf(sim);
  if (dump && *dump) {
    if (WriteMapDump(m, sim.mapFile, dump))
      Logf(LogLevel::Info, "moho64 ai: map dump written to %s (+ .png)", dump);
    else
      Logf(LogLevel::Error, "moho64 ai: cannot write the map dump %s", dump);
  }
}

// ---- The dump -------------------------------------------------------------------------------------------------

namespace {

void PngChunk(FILE* f, const char* type, const std::vector<uint8_t>& data) {
  auto be32 = [&](uint32_t v) {
    const uint8_t b[4] = {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8),
                          static_cast<uint8_t>(v)};
    std::fwrite(b, 1, 4, f);
  };
  be32(static_cast<uint32_t>(data.size()));
  std::fwrite(type, 1, 4, f);
  if (!data.empty()) std::fwrite(data.data(), 1, data.size(), f);
  uLong crc = crc32(0L, reinterpret_cast<const Bytef*>(type), 4);
  if (!data.empty()) crc = crc32(crc, data.data(), static_cast<uInt>(data.size()));
  be32(static_cast<uint32_t>(crc));
}

// An RGB picture as a PNG with stored (uncompressed) deflate blocks: the engine links zlib's inflate side only.
bool WritePng(const std::string& path, int w, int h, const std::vector<uint8_t>& rgb) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  std::fwrite(sig, 1, 8, f);
  std::vector<uint8_t> ihdr = {static_cast<uint8_t>(w >> 24), static_cast<uint8_t>(w >> 16), static_cast<uint8_t>(w >> 8),
                               static_cast<uint8_t>(w),       static_cast<uint8_t>(h >> 24), static_cast<uint8_t>(h >> 16),
                               static_cast<uint8_t>(h >> 8),  static_cast<uint8_t>(h),       8, 2, 0, 0, 0};
  PngChunk(f, "IHDR", ihdr);
  std::vector<uint8_t> raw;
  raw.reserve(static_cast<size_t>(h) * (1 + 3 * w));
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgb.begin() + static_cast<size_t>(y) * 3 * w, rgb.begin() + static_cast<size_t>(y + 1) * 3 * w);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  for (size_t i = 0; i < raw.size() || i == 0;) {
    const size_t n = std::min<size_t>(65535, raw.size() - i);
    const bool last = i + n >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(static_cast<uint8_t>(n));
    z.push_back(static_cast<uint8_t>(n >> 8));
    z.push_back(static_cast<uint8_t>(~n));
    z.push_back(static_cast<uint8_t>(~n >> 8));
    z.insert(z.end(), raw.begin() + i, raw.begin() + i + n);
    i += n;
    if (last) break;
  }
  const uLong ad = adler32(adler32(0L, Z_NULL, 0), raw.data(), static_cast<uInt>(raw.size()));
  z.push_back(static_cast<uint8_t>(ad >> 24));
  z.push_back(static_cast<uint8_t>(ad >> 16));
  z.push_back(static_cast<uint8_t>(ad >> 8));
  z.push_back(static_cast<uint8_t>(ad));
  PngChunk(f, "IDAT", z);
  PngChunk(f, "IEND", {});
  return std::fclose(f) == 0;
}

void ZoneColour(int id, bool water, uint8_t* c) {
  uint32_t h = static_cast<uint32_t>(id) * 2654435761u;
  h ^= h >> 13;
  if (water) {
    c[0] = static_cast<uint8_t>(20 + (h & 63));
    c[1] = static_cast<uint8_t>(60 + ((h >> 8) & 63));
    c[2] = static_cast<uint8_t>(150 + ((h >> 16) & 100));
  } else {
    c[0] = static_cast<uint8_t>(90 + (h & 127));
    c[1] = static_cast<uint8_t>(90 + ((h >> 8) & 127));
    c[2] = static_cast<uint8_t>(40 + ((h >> 16) & 60));
  }
}

}  // namespace

bool WriteMapDump(const MapAnalysis& m, const std::string& mapName, const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) return false;
  const int nz = static_cast<int>(m.zones.size());
  std::fprintf(f, "# moho64 AI map analysis (src/ai/map_analysis.h)\n");
  std::fprintf(f, "map %s %d x %d, segment %d, %d x %d segments, made in %.1f ms\n", mapName.c_str(), m.mapW, m.mapH,
               MapAnalysis::kSeg, m.sw, m.sh, m.ms);
  std::fprintf(f, "footprints%s\n", m.footprintNote.c_str());
  for (int d = 0; d < kDomains; ++d) {
    int big = 0, segs = 0;
    for (size_t i = 1; i < m.areaSize[d].size(); ++i) {
      segs += m.areaSize[d][i];
      if (m.areaSize[d][i] >= 16) ++big;
    }
    std::fprintf(f, "areas %s: %zu (%d of 16+ segments), %d segments; largest:", DomainName(d), m.areaSize[d].size() - 1,
                 big, segs);
    std::vector<std::pair<int, int>> v;
    for (size_t i = 1; i < m.areaSize[d].size(); ++i) v.push_back({-m.areaSize[d][i], static_cast<int>(i)});
    std::sort(v.begin(), v.end());
    for (size_t i = 0; i < v.size() && i < 6; ++i) std::fprintf(f, " #%d=%d", v[i].second, -v[i].first);
    std::fprintf(f, "\n");
  }
  int mass = 0, hydro = 0, massOut = 0;
  for (const ZoneDeposit& d : m.deposits) {
    (d.type == 1 ? mass : hydro)++;
    if (!d.zone) ++massOut;
  }
  std::fprintf(f, "deposits: %d mass, %d hydro, %d in no zone\n", mass, hydro, massOut);
  std::fprintf(f, "\n# zone id, domain, area, seeded by mass, centre x z, segments, mass, hydro, starts, cut, "
                  "neighbours, build spots 16/8/4/2\n");
  for (const Zone& z : m.zones) {
    int bs[4] = {};
    for (const BuildSpotSeg& b : m.buildSpots[z.id - 1]) bs[b.size == 16 ? 0 : b.size == 8 ? 1 : b.size == 4 ? 2 : 3]++;
    std::fprintf(f, "zone %d %s area %d %s centre %.0f %.0f segs %d mass %zu hydro %zu starts", z.id,
                 DomainName(z.domain), z.area, z.massSeeded ? "mass" : "plain", z.x, z.z, z.segments, z.mass.size(),
                 z.hydro.size());
    if (z.starts.empty()) std::fprintf(f, " -");
    for (int a : z.starts) std::fprintf(f, " %d", a);
    std::fprintf(f, "%s nb", z.cut ? " CUT" : "");
    for (int li : z.links) std::fprintf(f, " %d", m.links[li].a == z.id ? m.links[li].b : m.links[li].a);
    std::fprintf(f, " spots %d/%d/%d/%d\n", bs[0], bs[1], bs[2], bs[3]);
  }
  std::fprintf(f, "\n# link a b, border width (segment pairs), midpoint, coast / choke, distance land naval hover "
                  "amphibious (-1: no)\n");
  for (const ZoneLink& l : m.links)
    std::fprintf(f, "link %d %d width %d at %.0f %.0f%s%s dist %.0f %.0f %.0f %.0f\n", l.a, l.b, l.width, l.x, l.z,
                 l.coast ? " coast" : "", l.choke ? " CHOKE" : "", l.dist[0], l.dist[1], l.dist[2], l.dist[3]);
  // Distances between the start zones, per domain.
  std::vector<int> startZones;
  for (const Zone& z : m.zones)
    if (!z.starts.empty()) startZones.push_back(z.id);
  std::fprintf(f, "\n# travel distance between start zones (land / hover / amphibious; -1: no)\n");
  for (int a : startZones) {
    std::fprintf(f, "from zone %d (army %d):", a, m.zones[a - 1].starts[0]);
    for (int b : startZones)
      std::fprintf(f, " %d=%.0f/%.0f/%.0f", b, m.Distance(a, b, kLand), m.Distance(a, b, kHover),
                   m.Distance(a, b, kAmphibious));
    std::fprintf(f, "\n");
  }
  // The zone grid, one character per segment: zone id mod 62 as 0-9a-zA-Z, '.' none.
  std::fprintf(f, "\n# zone grid (%d x %d, one character per segment: zone id mod 62 as 0-9a-zA-Z, '.' none)\n", m.sw,
               m.sh);
  static const char* kDigits = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  for (int z = 0; z < m.sh; ++z) {
    for (int x = 0; x < m.sw; ++x) {
      const int id = m.zoneOf[static_cast<size_t>(z) * m.sw + x];
      std::fputc(id ? kDigits[id % 62] : '.', f);
    }
    std::fputc('\n', f);
  }
  const bool ok = std::fclose(f) == 0;
  (void)nz;

  // The picture: 3 pixels per segment; zones coloured (land warm, water blue), borders dark, chokepoints red,
  // cut zones' centres orange, mass white, hydro cyan, starts magenta, zone centres black.
  const int P = 3, W = m.sw * P, H = m.sh * P;
  std::vector<uint8_t> rgb(static_cast<size_t>(W) * H * 3, 0);
  auto put = [&](int px, int py, uint8_t r, uint8_t g, uint8_t b) {
    if (px < 0 || py < 0 || px >= W || py >= H) return;
    uint8_t* p = &rgb[(static_cast<size_t>(py) * W + px) * 3];
    p[0] = r;
    p[1] = g;
    p[2] = b;
  };
  auto dot = [&](float wx, float wz, int r, uint8_t cr, uint8_t cg, uint8_t cb) {
    const int cx = static_cast<int>(wx * P / MapAnalysis::kSeg), cz = static_cast<int>(wz * P / MapAnalysis::kSeg);
    for (int dz = -r; dz <= r; ++dz)
      for (int dx = -r; dx <= r; ++dx) put(cx + dx, cz + dz, cr, cg, cb);
  };
  for (int s = 0; s < m.sw * m.sh; ++s) {
    const int id = m.zoneOf[s];
    uint8_t c[3] = {0, 0, 0};
    if (id) ZoneColour(id, m.zones[id - 1].domain == kNaval, c);
    else if (m.pass[s]) c[0] = c[1] = c[2] = 60;
    const int sx = s % m.sw, sz = s / m.sw;
    const bool edgeR = sx + 1 < m.sw && m.zoneOf[s + 1] != id, edgeD = sz + 1 < m.sh && m.zoneOf[s + m.sw] != id;
    for (int py = 0; py < P; ++py)
      for (int px = 0; px < P; ++px) {
        const bool edge = (px == P - 1 && edgeR) || (py == P - 1 && edgeD);
        put(sx * P + px, sz * P + py, edge ? c[0] / 3 : c[0], edge ? c[1] / 3 : c[1], edge ? c[2] / 3 : c[2]);
      }
  }
  for (const ZoneLink& l : m.links)
    if (l.choke) dot(l.x, l.z, 3, 230, 20, 20);
  for (const Zone& z : m.zones) dot(z.x, z.z, 1, z.cut ? 255 : 0, z.cut ? 140 : 0, 0);
  for (const ZoneDeposit& d : m.deposits)
    dot(d.x, d.z, 1, d.type == 1 ? 255 : 0, 255, 255);
  for (const Zone& z : m.zones)
    if (!z.starts.empty()) dot(z.x, z.z, 3, 230, 0, 230);
  return WritePng(path + ".png", W, H, rgb) && ok;
}

// ---- Remastered.AI* -------------------------------------------------------------------------------------------

namespace {

const MapAnalysis& M(lua_State* L) { return MapOf(*Sim::From(L)); }

int CheckDomain(lua_State* L, int i, int def) {
  if (lua_isnoneornil(L, i)) return def;
  const int d = DomainFromName(luaL_checkstring(L, i));
  if (d < 0) luaL_argerror(L, i, "domain: 'land', 'naval', 'hover' or 'amphibious'");
  return d;
}

void SetNum(lua_State* L, const char* k, lua_Number v) {
  lua_pushstring(L, k);
  lua_pushnumber(L, v);
  lua_rawset(L, -3);
}
void SetStr(lua_State* L, const char* k, const char* v) {
  lua_pushstring(L, k);
  lua_pushstring(L, v);
  lua_rawset(L, -3);
}
void SetBool(lua_State* L, const char* k, bool v) {
  lua_pushstring(L, k);
  lua_pushboolean(L, v);
  lua_rawset(L, -3);
}
void PushPoint(lua_State* L, float x, float z) {  // {x, z} as an array of two
  lua_newtable(L);
  lua_pushnumber(L, x);
  lua_rawseti(L, -2, 1);
  lua_pushnumber(L, z);
  lua_rawseti(L, -2, 2);
}

// Remastered.AIMapInfo() -> {zones, landZones, waterZones, links, islands, ponds, segment, width, height}
int l_AIMapInfo(lua_State* L) {
  const MapAnalysis& m = M(L);
  int land = 0;
  for (const Zone& z : m.zones) land += z.domain == kLand;
  lua_newtable(L);
  SetNum(L, "zones", static_cast<lua_Number>(m.zones.size()));
  SetNum(L, "landZones", land);
  SetNum(L, "waterZones", static_cast<lua_Number>(m.zones.size()) - land);
  SetNum(L, "links", static_cast<lua_Number>(m.links.size()));
  SetNum(L, "islands", m.areaSize[kLand].empty() ? 0 : static_cast<lua_Number>(m.areaSize[kLand].size() - 1));
  SetNum(L, "ponds", m.areaSize[kNaval].empty() ? 0 : static_cast<lua_Number>(m.areaSize[kNaval].size() - 1));
  SetNum(L, "segment", MapAnalysis::kSeg);
  SetNum(L, "width", m.mapW);
  SetNum(L, "height", m.mapH);
  return 1;
}

// Remastered.AIZoneAt(x, z [, domain]) -> zone id, 0 = none (with a domain: 0 where that domain cannot stand)
int l_AIZoneAt(lua_State* L) {
  const float x = luaL_checknumber(L, 1), z = luaL_checknumber(L, 2);
  const int d = CheckDomain(L, 3, -1);
  lua_pushnumber(L, M(L).ZoneAt(x, z, d));
  return 1;
}

int CheckZone(lua_State* L, int i, const MapAnalysis& m) {
  const int id = static_cast<int>(luaL_checknumber(L, i));
  if (id < 1 || id > static_cast<int>(m.zones.size())) luaL_argerror(L, i, "no such zone");
  return id;
}

// Remastered.AIZone(id) -> {id, domain, area, mass = bool seeded by mass, x, z, segments, cut, massPoints = {{x, z},
// ...}, hydroPoints = {...}, starts = {army, ...}, neighbours = {id, ...}}
int l_AIZone(lua_State* L) {
  const MapAnalysis& m = M(L);
  const Zone& z = m.zones[CheckZone(L, 1, m) - 1];
  lua_newtable(L);
  SetNum(L, "id", z.id);
  SetStr(L, "domain", DomainName(z.domain));
  SetNum(L, "area", z.area);
  SetBool(L, "massSeeded", z.massSeeded);
  SetNum(L, "x", z.x);
  SetNum(L, "z", z.z);
  SetNum(L, "segments", z.segments);
  SetBool(L, "cut", z.cut);
  auto list = [&](const char* k, const std::vector<int>& v) {
    lua_pushstring(L, k);
    lua_newtable(L);
    for (size_t i = 0; i < v.size(); ++i) {
      PushPoint(L, m.deposits[v[i]].x, m.deposits[v[i]].z);
      lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    lua_rawset(L, -3);
  };
  list("massPoints", z.mass);
  list("hydroPoints", z.hydro);
  lua_pushstring(L, "starts");
  lua_newtable(L);
  for (size_t i = 0; i < z.starts.size(); ++i) {
    lua_pushnumber(L, z.starts[i]);
    lua_rawseti(L, -2, static_cast<int>(i + 1));
  }
  lua_rawset(L, -3);
  lua_pushstring(L, "neighbours");
  lua_newtable(L);
  for (size_t i = 0; i < z.links.size(); ++i) {
    const ZoneLink& l = m.links[z.links[i]];
    lua_pushnumber(L, l.a == z.id ? l.b : l.a);
    lua_rawseti(L, -2, static_cast<int>(i + 1));
  }
  lua_rawset(L, -3);
  return 1;
}

void PushLink(lua_State* L, const ZoneLink& l, int from) {
  lua_newtable(L);
  SetNum(L, "zone", from == l.a ? l.b : l.a);
  SetNum(L, "a", l.a);
  SetNum(L, "b", l.b);
  SetNum(L, "width", static_cast<lua_Number>(l.width) * MapAnalysis::kSeg);
  SetNum(L, "x", l.x);
  SetNum(L, "z", l.z);
  SetBool(L, "coast", l.coast);
  SetBool(L, "choke", l.choke);
}

// Remastered.AIZoneNeighbours(id) -> {{zone, width (world units), x, z, coast, choke}, ...} by neighbour id
int l_AIZoneNeighbours(lua_State* L) {
  const MapAnalysis& m = M(L);
  const Zone& z = m.zones[CheckZone(L, 1, m) - 1];
  lua_newtable(L);
  for (size_t i = 0; i < z.links.size(); ++i) {
    PushLink(L, m.links[z.links[i]], z.id);
    lua_rawseti(L, -2, static_cast<int>(i + 1));
  }
  return 1;
}

// Remastered.AIZoneDistance(a, b [, domain = 'land']) -> travel distance (world units), -1 when that domain cannot
int l_AIZoneDistance(lua_State* L) {
  const MapAnalysis& m = M(L);
  const int a = CheckZone(L, 1, m), b = CheckZone(L, 2, m);
  lua_pushnumber(L, m.Distance(a, b, CheckDomain(L, 3, kLand)));
  return 1;
}

// Remastered.AIChokepoints() -> {{a, b, width, x, z}, ...} in link order
int l_AIChokepoints(lua_State* L) {
  const MapAnalysis& m = M(L);
  lua_newtable(L);
  int n = 0;
  for (const ZoneLink& l : m.links)
    if (l.choke) {
      PushLink(L, l, l.a);
      lua_rawseti(L, -2, ++n);
    }
  return 1;
}

// Remastered.AIBuildSpots(zone, size) -> {{x, z}, ...}: the centres of the squares of `size` world units (2, 4, 8 or 16)
// that fit on the terrain at the zone's segments, in segment order (structures and props not counted)
int l_AIBuildSpots(lua_State* L) {
  const MapAnalysis& m = M(L);
  const int id = CheckZone(L, 1, m);
  const int size = static_cast<int>(luaL_checknumber(L, 2));
  lua_newtable(L);
  int n = 0;
  for (const BuildSpotSeg& b : m.buildSpots[id - 1])
    if (b.size >= size) {
      const float x = static_cast<float>((b.seg % m.sw) * MapAnalysis::kSeg) + 0.5f * size;
      const float z = static_cast<float>((b.seg / m.sw) * MapAnalysis::kSeg) + 0.5f * size;
      PushPoint(L, x, z);
      lua_rawseti(L, -2, ++n);
    }
  return 1;
}

}  // namespace

void RegisterMapBindings(lua_State* L) {
  lua_pushstring(L, "Remastered");
  lua_rawget(L, LUA_GLOBALSINDEX);
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return;
  }
  static const struct {
    const char* name;
    lua_CFunction f;
  } kFns[] = {{"AIMapInfo", l_AIMapInfo},           {"AIZoneAt", l_AIZoneAt},
              {"AIZone", l_AIZone},                 {"AIZoneNeighbours", l_AIZoneNeighbours},
              {"AIZoneDistance", l_AIZoneDistance}, {"AIChokepoints", l_AIChokepoints},
              {"AIBuildSpots", l_AIBuildSpots}};
  for (const auto& fn : kFns) {
    lua_pushstring(L, fn.name);
    lua_pushcfunction(L, fn.f);
    lua_rawset(L, -3);
  }
  lua_pop(L, 1);
}

}  // namespace ai
}  // namespace moho
