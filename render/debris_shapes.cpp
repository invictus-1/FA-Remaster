#include "debris_shapes.h"

#include <algorithm>
#include <cmath>

namespace render {
namespace {

constexpr float kPi = 3.14159265f;

struct Rand {
  uint32_t s;
  float operator()() {  // 0..1
    s = s * 1664525u + 1013904223u;
    return static_cast<float>(s >> 8) * (1.0f / 16777216.0f);
  }
};

// A triangle as it is made, before the shape is bent: its corners, their texture places, and whether it is torn. Its
// corners are put counter-clockwise seen from the side `out` says is its outside (Add).
struct Raw {
  Vec3 p[3];
  Vec2 t[3];
  bool torn;
};

void Add(std::vector<Raw>* raws, bool torn, Vec3 a, Vec3 b, Vec3 c, Vec2 ta, Vec2 tb, Vec2 tc, Vec3 out) {
  if (Dot(Cross(b - a, c - a), out) < 0) {
    std::swap(b, c);
    std::swap(tb, tc);
  }
  raws->push_back({{a, b, c}, {ta, tb, tc}, torn});
}

void AddQuad(std::vector<Raw>* raws, bool torn, Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec2 ta, Vec2 tb, Vec2 tc, Vec2 td, Vec3 out) {
  Add(raws, torn, a, b, c, ta, tb, tc, out);
  Add(raws, torn, a, c, d, ta, tc, td, out);
}

Vec2 Sub(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }

// --- plates, panels and shards ---

// An outline round a middle: `corners` points at random distances, some pulled in (tears, `ragged` how far), `stretch`
// times as long (x) as it is wide (z); in order round the middle.
std::vector<Vec2> RoundOutline(Rand& r, int corners, float ragged, float stretch) {
  std::vector<Vec2> outline;
  for (int i = 0; i < corners; ++i) {
    const float a = 2 * kPi * (static_cast<float>(i) + (r() - 0.5f) * 0.55f) / static_cast<float>(corners);
    float rad = 0.5f * (1 - ragged * r());
    if (r() < 0.3f) rad *= 0.6f;  // (a tear in from the edge)
    outline.push_back({std::cos(a) * rad * stretch, std::sin(a) * rad});
  }
  return outline;
}

// A panel's outline: a rectangle `stretch` long and 0.8 wide, its corners pushed about, one corner torn off and a
// notch or two torn into its sides.
std::vector<Vec2> PanelOutline(Rand& r, float stretch) {
  const float w = 0.5f * stretch, h = 0.4f;
  const Vec2 corner[4] = {{-w, -h}, {w, -h}, {w, h}, {-w, h}};
  const int lost = static_cast<int>(r() * 4) % 4;  // (the corner torn off)
  std::vector<Vec2> outline;
  for (int k = 0; k < 4; ++k) {
    const Vec2 a = corner[k], b = corner[(k + 1) % 4];
    const Vec2 jitter{(r() - 0.5f) * 0.08f, (r() - 0.5f) * 0.08f};
    if (k == lost) {  // two points either side of it instead
      const Vec2 prev = corner[(k + 3) % 4];
      const float t1 = 0.2f + 0.15f * r(), t2 = 0.2f + 0.15f * r();
      outline.push_back({a.x + (prev.x - a.x) * t1, a.y + (prev.y - a.y) * t1});
      outline.push_back({a.x + (b.x - a.x) * t2, a.y + (b.y - a.y) * t2});
    } else {
      outline.push_back({a.x + jitter.x, a.y + jitter.y});
    }
    if (r() < 0.55f) {  // a notch torn into this side: in toward the middle and out again
      const float t = 0.35f + 0.3f * r(), depth = 0.1f + 0.12f * r();
      const Vec2 at{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
      const float l = std::sqrt(at.x * at.x + at.y * at.y);
      const Vec2 in{at.x - at.x / std::max(l, 1e-3f) * depth, at.y - at.y / std::max(l, 1e-3f) * depth};
      outline.push_back({a.x + (b.x - a.x) * (t - 0.08f), a.y + (b.y - a.y) * (t - 0.08f)});
      outline.push_back(in);
      outline.push_back({a.x + (b.x - a.x) * (t + 0.08f), a.y + (b.y - a.y) * (t + 0.08f)});
    }
  }
  return outline;
}

// A plate (x, z across it, y through it) of that outline, `thick` thick, crumpled by `crumple` (its rim's points lifted
// and lowered a little, so its faces catch the light differently), folded along the line x = `fold` by `angle` radians
// (the side beyond it turned up; 0: none). Its two faces are skin, wearing the texture as it lay flat; its rim is torn.
void MakePlate(Rand& r, std::vector<Raw>* raws, const std::vector<Vec2>& outline, float thick, float crumple, float fold, float angle) {
  // the rim with the points where it crosses the fold put in, so that every triangle lies on one side of the fold
  std::vector<Vec2> ring;
  std::vector<size_t> crossings;
  for (size_t i = 0; i < outline.size(); ++i) {
    const Vec2 p = outline[i], q = outline[(i + 1) % outline.size()];
    ring.push_back(p);
    if (angle != 0 && (p.x - fold) * (q.x - fold) < 0) {
      const float t = (fold - p.x) / (q.x - p.x);
      crossings.push_back(ring.size());
      ring.push_back({fold, p.y + (q.y - p.y) * t});
    }
  }
  // the faces are fans from a middle on the fold (or the middle of the rim without one)
  Vec2 apex{0, 0};
  if (angle != 0 && crossings.size() >= 2) {
    float lo = 1e9f, hi = -1e9f;
    for (size_t c : crossings) {
      lo = std::min(lo, ring[c].y);
      hi = std::max(hi, ring[c].y);
    }
    apex = {fold, 0.5f * (lo + hi)};
  } else {
    angle = 0;
    for (const Vec2& p : ring) apex = {apex.x + p.x / static_cast<float>(ring.size()), apex.y + p.y / static_cast<float>(ring.size())};
  }
  std::vector<float> lift(ring.size());
  for (float& l : lift) l = (r() - 0.5f) * crumple;
  const float half = 0.5f * thick;
  const Vec3 topApex{apex.x, half, apex.y}, bottomApex{apex.x, -half, apex.y};
  const size_t first = raws->size();
  float along = 0;
  for (size_t i = 0; i < ring.size(); ++i) {
    const size_t j = (i + 1) % ring.size();
    const Vec2 a = ring[i], b = ring[j];
    const Vec3 ta{a.x, half + lift[i], a.y}, tb{b.x, half + lift[j], b.y}, ba{a.x, -half + lift[i], a.y}, bb{b.x, -half + lift[j], b.y};
    Add(raws, false, topApex, ta, tb, apex, a, b, {0, 1, 0});
    Add(raws, false, bottomApex, bb, ba, apex, b, a, {0, -1, 0});
    const Vec2 mid{0.5f * (a.x + b.x) - apex.x, 0.5f * (a.y + b.y) - apex.y};
    const float l = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
    AddQuad(raws, true, ta, tb, bb, ba, {along, half}, {along + l, half}, {along + l, -half}, {along, -half}, {mid.x, 0, mid.y});
    along += l;
  }
  if (angle == 0) return;
  // the fold: the side beyond it turned about the line x = fold, y = 0
  const float c = std::cos(angle), s = std::sin(angle);
  for (size_t k = first; k < raws->size(); ++k)
    for (Vec3& p : (*raws)[k].p)
      if (p.x > fold + 1e-6f) {
        const float dx = p.x - fold;
        p = {fold + dx * c - p.y * s, dx * s + p.y * c, p.z};
      }
}

// --- girders and struts ---

// A bar: `section` (x, y; counter-clockwise, every point seen from `centre`) drawn out along z from -0.5 to 0.5,
// kinked at z = `kinkAt` by `kink` radians, its ends torn ragged (each point of an end pushed along by up to `ragged`).
// Its sides are skin; its broken ends torn.
void MakeBar(Rand& r, std::vector<Raw>* raws, const std::vector<Vec2>& section, Vec2 centre, float kinkAt, float kink, float ragged) {
  const size_t n = section.size();
  std::vector<float> start(n), end(n);
  float cs = 0, ce = 0;  // (each broken end's middle)
  for (size_t i = 0; i < n; ++i) {
    start[i] = -0.5f + (r() - 0.5f) * ragged;
    end[i] = 0.5f + (r() - 0.5f) * ragged;
    cs += start[i] / static_cast<float>(n);
    ce += end[i] / static_cast<float>(n);
  }
  const size_t first = raws->size();
  float around = 0;
  for (size_t i = 0; i < n; ++i) {
    const size_t j = (i + 1) % n;
    const Vec2 a = section[i], b = section[j], d = Sub(b, a);
    const float l = std::sqrt(d.x * d.x + d.y * d.y);
    const Vec3 out{d.y, -d.x, 0};
    const float zs[3][2] = {{start[i], start[j]}, {kinkAt, kinkAt}, {end[i], end[j]}};
    for (int seg = 0; seg < 2; ++seg) {
      const float z0a = zs[seg][0], z0b = zs[seg][1], z1a = zs[seg + 1][0], z1b = zs[seg + 1][1];
      AddQuad(raws, false, {a.x, a.y, z0a}, {b.x, b.y, z0b}, {b.x, b.y, z1b}, {a.x, a.y, z1a}, {around, z0a}, {around + l, z0b},
              {around + l, z1b}, {around, z1a}, out);
    }
    around += l;
    // the two broken ends, fans from the middle
    Add(raws, true, {centre.x, centre.y, cs}, {a.x, a.y, start[i]}, {b.x, b.y, start[j]}, centre, a, b, {0, 0, -1});
    Add(raws, true, {centre.x, centre.y, ce}, {b.x, b.y, end[j]}, {a.x, a.y, end[i]}, centre, b, a, {0, 0, 1});
  }
  if (kink == 0) return;
  const float c = std::cos(kink), s = std::sin(kink);
  for (size_t k = first; k < raws->size(); ++k)
    for (Vec3& p : (*raws)[k].p)
      if (p.z > kinkAt + 1e-6f) {
        const float dz = p.z - kinkAt;
        p = {p.x, p.y * c - dz * s, kinkAt + p.y * s + dz * c};
      }
}

// --- lumps of concrete ---

struct Face {
  std::vector<Vec3> p;  // in order round it
  Vec3 normal;          // outward
  bool cut;             // a fracture (torn), not the outside
};

// A convex lump: a box of those half sizes with `cuts` random corners and edges broken off (each a plane cut through
// it). The box's faces are skin; the fractures are torn.
void MakeLump(Rand& r, std::vector<Raw>* raws, Vec3 half, int cuts) {
  std::vector<Face> faces;
  for (int axis = 0; axis < 3; ++axis)
    for (int sign = -1; sign <= 1; sign += 2) {
      const int u = (axis + 1) % 3, v = (axis + 2) % 3;
      const float h[3] = {half.x, half.y, half.z};
      Face f;
      f.cut = false;
      float n[3] = {0, 0, 0};
      n[axis] = static_cast<float>(sign);
      f.normal = {n[0], n[1], n[2]};
      const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
      for (const auto& c : corners) {
        float p[3];
        p[axis] = sign * h[axis];
        p[u] = c[0] * h[u];
        p[v] = c[1] * h[v];
        f.p.push_back({p[0], p[1], p[2]});
      }
      faces.push_back(f);
    }
  for (int k = 0; k < cuts; ++k) {
    const Vec3 n = Normalize(Vec3{r() - 0.5f, r() - 0.5f, r() - 0.5f});
    const float reach = std::abs(n.x) * half.x + std::abs(n.y) * half.y + std::abs(n.z) * half.z;
    const float d = reach * (0.5f + 0.3f * r());
    std::vector<Vec3> rim;
    std::vector<Face> kept;
    for (const Face& f : faces) {
      Face g = f;
      g.p.clear();
      for (size_t i = 0; i < f.p.size(); ++i) {
        const Vec3 a = f.p[i], b = f.p[(i + 1) % f.p.size()];
        const float da = Dot(n, a) - d, db = Dot(n, b) - d;
        if (da <= 0) g.p.push_back(a);
        if ((da < 0 && db > 0) || (da > 0 && db < 0)) {
          const Vec3 x = a + (b - a) * (da / (da - db));
          g.p.push_back(x);
          rim.push_back(x);
        }
      }
      if (g.p.size() >= 3) kept.push_back(g);
    }
    faces.swap(kept);
    // the fracture: the points where the cut crossed the faces' edges, in order round it
    std::vector<Vec3> unique;
    for (const Vec3& p : rim) {
      bool known = false;
      for (const Vec3& q : unique) known = known || Length(p - q) < 1e-5f;
      if (!known) unique.push_back(p);
    }
    if (unique.size() < 3) continue;
    Vec3 middle{};
    for (const Vec3& p : unique) middle += p;
    middle = middle * (1.0f / static_cast<float>(unique.size()));
    const Vec3 u = Normalize(Cross(n, std::abs(n.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0})), v = Cross(n, u);
    std::sort(unique.begin(), unique.end(), [&](const Vec3& a, const Vec3& b) {
      return std::atan2(Dot(a - middle, v), Dot(a - middle, u)) < std::atan2(Dot(b - middle, v), Dot(b - middle, u));
    });
    faces.push_back({unique, n, true});
  }
  for (const Face& f : faces) {
    // its texture: the face laid flat along the two axes it lies most along
    const Vec3 a{std::abs(f.normal.x), std::abs(f.normal.y), std::abs(f.normal.z)};
    auto place = [&](Vec3 p) -> Vec2 { return a.x >= a.y && a.x >= a.z ? Vec2{p.z, p.y} : a.y >= a.z ? Vec2{p.x, p.z} : Vec2{p.x, p.y}; };
    for (size_t i = 1; i + 1 < f.p.size(); ++i)
      Add(raws, f.cut, f.p[0], f.p[i], f.p[i + 1], place(f.p[0]), place(f.p[i]), place(f.p[i + 1]), f.normal);
  }
}

// The triangles into a shape: its middle (the box round it) at the origin, its widest one unit, each corner with its
// face's normal and its texture's tangents (along the texture's u, and its v).
void Finish(DebrisShape* shape, std::vector<Raw>& raws) {
  Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
  for (const Raw& t : raws)
    for (const Vec3& p : t.p) {
      lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
      hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
  const Vec3 middle = (lo + hi) * 0.5f;
  const float widest = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z, 1e-3f});
  shape->lo = (lo - middle) * (1 / widest);
  shape->hi = (hi - middle) * (1 / widest);
  for (Raw& t : raws) {
    for (Vec3& p : t.p) p = (p - middle) * (1 / widest);
    const Vec3 a = t.p[0], b = t.p[1], c = t.p[2];
    Vec3 n = Cross(b - a, c - a);
    const float len = Length(n);
    if (len < 1e-9f) continue;
    n = n * (1 / len);
    const Vec3 e1 = b - a, e2 = c - a;
    const float du1 = t.t[1].x - t.t[0].x, dv1 = t.t[1].y - t.t[0].y, du2 = t.t[2].x - t.t[0].x, dv2 = t.t[2].y - t.t[0].y;
    const float det = du1 * dv2 - du2 * dv1;
    Vec3 tangent = std::abs(det) > 1e-9f ? (e1 * dv2 - e2 * dv1) * (1 / det) : e1;
    Vec3 binormal = std::abs(det) > 1e-9f ? (e2 * du1 - e1 * du2) * (1 / det) : Cross(n, e1);
    tangent = tangent - n * Dot(n, tangent);
    if (Length(tangent) < 1e-6f) tangent = Cross(n, std::abs(n.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0});
    tangent = Normalize(tangent);
    binormal = binormal - n * Dot(n, binormal) - tangent * Dot(tangent, binormal);
    if (Length(binormal) < 1e-6f) binormal = Cross(n, tangent);
    binormal = Normalize(binormal);
    std::vector<ScmVertex>& out = t.torn ? shape->torn : shape->skin;
    for (int k = 0; k < 3; ++k) {
      ScmVertex v{};
      v.position[0] = t.p[k].x, v.position[1] = t.p[k].y, v.position[2] = t.p[k].z;
      v.normal[0] = n.x, v.normal[1] = n.y, v.normal[2] = n.z;
      v.tangent[0] = tangent.x, v.tangent[1] = tangent.y, v.tangent[2] = tangent.z;
      v.binormal[0] = binormal.x, v.binormal[1] = binormal.y, v.binormal[2] = binormal.z;
      v.uv0[0] = v.uv1[0] = t.t[k].x;
      v.uv0[1] = v.uv1[1] = t.t[k].y;
      out.push_back(v);
    }
  }
}

}  // namespace

std::vector<DebrisShape> MakeDebrisShapes() {
  std::vector<DebrisShape> shapes(kDebrisShapes);
  Rand r{0x5eed0d1eu};
  for (int k = 0; k < kDebrisShapes; ++k) {
    std::vector<Raw> raws;
    DebrisShape& shape = shapes[static_cast<size_t>(k)];
    if (k < 5) {  // torn plates: three of them bent
      shape.kind = DebrisShape::Plate;
      const float stretch = 1.0f + 0.6f * r();
      const auto outline = RoundOutline(r, 7 + static_cast<int>(r() * 4), 0.35f + 0.15f * r(), stretch);
      const bool bent = k < 3;
      MakePlate(r, &raws, outline, 0.08f + 0.03f * r(), 0.06f + 0.06f * r(), (r() - 0.5f) * 0.2f * stretch, bent ? 0.35f + 0.55f * r() : 0.0f);
    } else if (k < 7) {  // panels folded nearly square
      shape.kind = DebrisShape::Panel;
      const float stretch = 1.1f + 0.4f * r();
      MakePlate(r, &raws, PanelOutline(r, stretch), 0.07f, 0.03f, (r() - 0.5f) * 0.15f, 1.05f + 0.5f * r());
    } else if (k < 9) {  // girders: an angle, a tee
      shape.kind = DebrisShape::Girder;
      const float t = 0.07f, a = 0.3f;
      std::vector<Vec2> section;
      Vec2 centre;
      if (k == 7) {
        section = {{0, 0}, {a, 0}, {a, t}, {t, t}, {t, a}, {0, a}};
        centre = {0.5f * t, 0.5f * t};
      } else {
        const float f = 0.17f, h = 0.28f;
        section = {{-f, 0}, {f, 0}, {f, t}, {0.5f * t, t}, {0.5f * t, h}, {-0.5f * t, h}, {-0.5f * t, t}, {-f, t}};
        centre = {0, 0.5f * t};
      }
      MakeBar(r, &raws, section, centre, (r() - 0.5f) * 0.4f, 0.25f + 0.35f * r(), 0.16f);
    } else if (k < 10) {  // a strut: a six-sided rod, bent
      shape.kind = DebrisShape::Strut;
      std::vector<Vec2> section;
      for (int i = 0; i < 6; ++i) section.push_back({0.075f * std::cos(i * kPi / 3), 0.075f * std::sin(i * kPi / 3)});
      MakeBar(r, &raws, section, {0, 0}, (r() - 0.5f) * 0.3f, 0.35f + 0.4f * r(), 0.12f);
    } else if (k < 13) {  // shards: small plates of few corners, long and sharp
      shape.kind = DebrisShape::Shard;
      const float stretch = 2.0f + 0.8f * r();
      const auto outline = RoundOutline(r, 3 + static_cast<int>(r() * 3), 0.25f, stretch);
      MakePlate(r, &raws, outline, 0.1f, 0.05f, (r() - 0.5f) * 0.3f, k == 11 ? 0.4f : 0.0f);
    } else {  // lumps of concrete
      shape.kind = DebrisShape::Lump;
      MakeLump(r, &raws, {0.5f, 0.3f + 0.15f * r(), 0.38f + 0.12f * r()}, 5 + static_cast<int>(r() * 3));
    }
    Finish(&shape, raws);
  }
  return shapes;
}

int PickDebrisShape(char kind, float random, float random2) {
  struct Group {
    int first, count;
    float share;
  };
  static const Group kBuilding[] = {{13, 3, 0.45f}, {7, 3, 0.2f}, {0, 5, 0.25f}, {5, 2, 0.1f}};
  static const Group kAircraft[] = {{0, 5, 0.5f}, {10, 3, 0.3f}, {5, 2, 0.15f}, {9, 1, 0.05f}};
  static const Group kVehicle[] = {{0, 5, 0.45f}, {5, 2, 0.15f}, {7, 3, 0.2f}, {10, 3, 0.2f}};
  const Group* groups = kind == 'b' ? kBuilding : kind == 'a' ? kAircraft : kVehicle;
  float reached = 0;
  for (int g = 0; g < 4; ++g) {
    reached += groups[g].share;
    if (random < reached || g == 3) return groups[g].first + std::min(static_cast<int>(random2 * static_cast<float>(groups[g].count)), groups[g].count - 1);
  }
  return 0;
}

}  // namespace render
