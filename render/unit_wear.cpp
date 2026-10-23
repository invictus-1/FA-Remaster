#include "unit_wear.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "dds.h"
#include "models.h"
#include "scmap.h"

namespace render {
namespace {

bool Has(const std::string& s, const char* word) { return s.find(word) != std::string::npos; }

// A number 0..1 from two (a mark's own, the spot of a hit no one saw).
float Hash01(uint32_t a, uint32_t b) {
  uint32_t h = a * 0x9e3779b1u ^ (b + 0x7f4a7c15u) * 0x85ebca77u;
  h ^= h >> 15;
  h *= 0x2c1b3c6du;
  h ^= h >> 12;
  return static_cast<float>(h & 0xffffff) / 16777216.0f;
}

// The box round the unit's model (world units, its own frame), and how far a point of that frame lies outside it.
float OutsideBox(Vec3 p, Vec3 lo, Vec3 hi) {
  const Vec3 c{std::clamp(p.x, lo.x, hi.x), std::clamp(p.y, lo.y, hi.y), std::clamp(p.z, lo.z, hi.z)};
  return Length(p - c);
}

// A shot stops on the game's collision box, which is often larger than the model: where it stopped is carried on
// along its way to where it enters the box round the model's part (or, missing it, to the nearest point of that box).
Vec3 OntoBox(Vec3 p, Vec3 way, Vec3 lo, Vec3 hi) {
  if (OutsideBox(p, lo, hi) <= 0) return p;
  const float o[3] = {p.x, p.y, p.z}, v[3] = {way.x, way.y, way.z}, a[3] = {lo.x, lo.y, lo.z}, b[3] = {hi.x, hi.y, hi.z};
  float t0 = 0, t1 = 1e30f;
  bool miss = false;
  for (int k = 0; k < 3 && !miss; ++k) {
    if (std::fabs(v[k]) < 1e-6f) {
      miss = o[k] < a[k] || o[k] > b[k];
      continue;
    }
    float s0 = (a[k] - o[k]) / v[k], s1 = (b[k] - o[k]) / v[k];
    if (s0 > s1) std::swap(s0, s1);
    t0 = std::max(t0, s0);
    t1 = std::min(t1, s1);
    miss = t0 > t1;
  }
  if (!miss) return p + way * t0;
  return {std::clamp(p.x, lo.x, hi.x), std::clamp(p.y, lo.y, hi.y), std::clamp(p.z, lo.z, hi.z)};
}

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Does the ray from + way * t (t from 0 to `far`; `inv` 1 / way) pass through the box?
bool RayBox(Vec3 lo, Vec3 hi, Vec3 from, Vec3 inv, float far) {
  float t0 = 0, t1 = far;
  const float o[3] = {from.x, from.y, from.z}, v[3] = {inv.x, inv.y, inv.z}, a[3] = {lo.x, lo.y, lo.z}, b[3] = {hi.x, hi.y, hi.z};
  for (int k = 0; k < 3; ++k) {
    float s0 = (a[k] - o[k]) * v[k], s1 = (b[k] - o[k]) * v[k];
    if (s0 > s1) std::swap(s0, s1);
    t0 = std::max(t0, s0);
    t1 = std::min(t1, s1);
    if (t0 > t1) return false;
  }
  return true;
}

// How far along the ray the triangle lies (either side of it facing), below 0: it misses (Moeller and Trumbore's
// way, "Fast, Minimum Storage Ray/Triangle Intersection", 1997).
float RayTriangle(Vec3 from, Vec3 way, Vec3 a, Vec3 b, Vec3 c) {
  const Vec3 e1 = b - a, e2 = c - a, p = Cross(way, e2);
  const float det = Dot(e1, p);
  if (std::fabs(det) < 1e-12f) return -1;
  const float inv = 1 / det;
  const Vec3 s = from - a;
  const float u = Dot(s, p) * inv;
  if (u < 0 || u > 1) return -1;
  const Vec3 q = Cross(s, e1);
  const float v = Dot(way, q) * inv;
  if (v < 0 || u + v > 1) return -1;
  return Dot(e2, q) * inv;
}

// A bone as posed this beat, undone: what brings a point of the unit's own frame (world units) back onto the model
// at rest (rest = Rotate(undo, point - shift)). Not posed: as it is.
struct Unpose {
  Quat undo;
  Vec3 shift;
  bool hidden = false;
};

// Where a shot along `way` (unit length) from `from` (both in the unit's own frame, world units) first meets the
// model's hit shape within `far`, each part where its bone has it (`poses` by bone; missing ones at rest): its place
// on the model at rest and its way there. False: it meets none, or the model has no hit shape.
bool CastOnto(const Model& m, const std::vector<Unpose>& poses, Vec3 from, Vec3 way, float far, Vec3* rest, Vec3* restWay) {
  const HitShape* shape = m.hitShape.get();
  if (!shape) return false;
  const float s = m.scale > 0 ? m.scale : 1;
  float best = far / s;
  bool found = false;
  for (size_t b = 0; b < shape->roots.size(); ++b) {
    if (shape->roots[b] < 0) continue;
    Vec3 o = from, w = way;
    if (b < poses.size()) {
      if (poses[b].hidden) continue;
      o = Rotate(poses[b].undo, from - poses[b].shift);
      w = Rotate(poses[b].undo, way);
    }
    o = o * (1 / s);
    const float t = shape->Cast(b, o, w, best);
    if (t >= 0 && t < best) {
      best = t;
      found = true;
      *rest = (o + w * t) * s;
      *restWay = w;
    }
  }
  return found;
}

// The kind a name says (a projectile's id, a weapon's label, lower case), by the words in it; false: it says none.
bool KindByWords(const std::string& id, int* kind) {
  auto any = [&](std::initializer_list<const char*> words) {
    for (const char* w : words)
      if (Has(id, w)) return true;
    return false;
  };
  if (any({"napalm", "flame", "incendi", "fire"})) *kind = UnitWear::Fire;
  else if (Has(id, "overcharge")) *kind = UnitWear::Energy;  // (a commander's bolt, though its name holds "charge")
  else if (any({"missile", "rocket", "artillery", "bomb", "torpedo", "mortar", "nuke", "nuclear", "warhead", "fragment", "cluster", "grenade",
                "charge", "flak", "kamikaze", "dart", "mine", "cavitation", "burst", "fizz", "death"}))
    *kind = UnitWear::Explosive;
  else if (any({"laser", "plasma", "ionized", "disrupt", "phason", "phasic", "chronatron", "quantum", "quantic", "oblivion", "tau", "disintegr",
                "proton", "microwave", "pulse", "beam", "electron", "spectra", "ultrachrom", "ginsu", "sonic", "graviton", "quarnon",
                "molecular", "energy", "bolter", "sniper", "ohcannon", "quatum", "reacton", "particle"}))
    *kind = UnitWear::Energy;
  else if (any({"gauss", "cannon", "shell", "bullet", "riot", "gatling", "rail", "autogun", "rifle", "gun", "slug", "flechette"}))
    *kind = UnitWear::Kinetic;
  else
    return false;
  return true;
}

}  // namespace

float HitShape::Cast(size_t bone, Vec3 from, Vec3 way, float far) const {
  if (bone >= roots.size() || roots[bone] < 0) return -1;
  auto inverse = [](float v) { return std::fabs(v) > 1e-12f ? 1 / v : (v < 0 ? -1e30f : 1e30f); };
  const Vec3 inv{inverse(way.x), inverse(way.y), inverse(way.z)};
  float best = far;
  bool found = false;
  uint32_t stack[64];
  int top = 0;
  stack[top++] = static_cast<uint32_t>(roots[bone]);
  while (top > 0) {
    const Node& n = nodes[stack[--top]];
    if (!RayBox(n.lo, n.hi, from, inv, best)) continue;
    if (n.count > 0) {
      for (uint32_t k = n.start; k < n.start + n.count; ++k) {
        const float t = RayTriangle(from, way, corners[k * 3], corners[k * 3 + 1], corners[k * 3 + 2]);
        if (t >= 0 && t < best) {
          best = t;
          found = true;
        }
      }
    } else if (top + 2 <= 64) {
      stack[top++] = n.start;
      stack[top++] = n.start + 1;
    }
  }
  return found ? best : -1;
}

std::shared_ptr<const HitShape> MakeHitShape(const char* vertices, int64_t vertexCount, const char* indices, int64_t indexCount, int indexBytes,
                                             size_t bones) {
  auto shape = std::make_shared<HitShape>();
  struct Tri {
    uint32_t bone;
    Vec3 a, b, c, mid;
  };
  std::vector<Tri> tris;
  tris.reserve(static_cast<size_t>(indexCount / 3));
  auto index = [&](int64_t k) -> int64_t {
    if (indexBytes == 4) {
      uint32_t i;
      std::memcpy(&i, indices + k * 4, 4);
      return i;
    }
    uint16_t i;
    std::memcpy(&i, indices + k * 2, 2);
    return i;
  };
  auto vertex = [&](int64_t i, uint8_t* bone) {
    ScmVertex v;
    std::memcpy(&v, vertices + i * static_cast<int64_t>(sizeof(ScmVertex)), sizeof v);
    if (bone) *bone = v.bones[0];
    return Vec3{v.position[0], v.position[1], v.position[2]};
  };
  for (int64_t t = 0; t + 2 < indexCount; t += 3) {
    const int64_t i0 = index(t), i1 = index(t + 1), i2 = index(t + 2);
    if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) continue;
    uint8_t bone = 0;
    Tri tri;
    tri.a = vertex(i0, &bone);
    tri.b = vertex(i1, nullptr);
    tri.c = vertex(i2, nullptr);
    tri.bone = std::min<uint32_t>(bone, static_cast<uint32_t>(bones - 1));
    tri.mid = (tri.a + tri.b + tri.c) * (1.0f / 3);
    tris.push_back(tri);
  }
  std::stable_sort(tris.begin(), tris.end(), [](const Tri& x, const Tri& y) { return x.bone < y.bone; });
  shape->roots.assign(bones, -1);
  shape->nodes.reserve(tris.size() / 2 + bones + 1);
  // each bone's part in a tree: a box split in two along its longest side at the middle triangle, down to four
  std::function<void(uint32_t, size_t, size_t)> fill = [&](uint32_t at, size_t from, size_t to) {
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f}, mlo = lo, mhi = hi;
    for (size_t k = from; k < to; ++k) {
      for (const Vec3& p : {tris[k].a, tris[k].b, tris[k].c}) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
      }
      const Vec3& m = tris[k].mid;
      mlo = {std::min(mlo.x, m.x), std::min(mlo.y, m.y), std::min(mlo.z, m.z)};
      mhi = {std::max(mhi.x, m.x), std::max(mhi.y, m.y), std::max(mhi.z, m.z)};
    }
    shape->nodes[at].lo = lo;
    shape->nodes[at].hi = hi;
    if (to - from <= 4) {
      shape->nodes[at].start = static_cast<uint32_t>(from);
      shape->nodes[at].count = static_cast<uint32_t>(to - from);
      return;
    }
    const Vec3 e = mhi - mlo;
    const int axis = e.x >= e.y && e.x >= e.z ? 0 : e.y >= e.z ? 1 : 2;
    const size_t half = (from + to) / 2;
    std::nth_element(tris.begin() + static_cast<std::ptrdiff_t>(from), tris.begin() + static_cast<std::ptrdiff_t>(half),
                     tris.begin() + static_cast<std::ptrdiff_t>(to), [axis](const Tri& x, const Tri& y) {
                       return axis == 0 ? x.mid.x < y.mid.x : axis == 1 ? x.mid.y < y.mid.y : x.mid.z < y.mid.z;
                     });
    const uint32_t below = static_cast<uint32_t>(shape->nodes.size());
    shape->nodes.resize(shape->nodes.size() + 2);
    shape->nodes[at].start = below;
    shape->nodes[at].count = 0;
    fill(below, from, half);
    fill(below + 1, half, to);
  };
  for (size_t from = 0; from < tris.size();) {
    size_t to = from;
    while (to < tris.size() && tris[to].bone == tris[from].bone) ++to;
    const uint32_t top = static_cast<uint32_t>(shape->nodes.size());
    shape->nodes.emplace_back();
    shape->roots[tris[from].bone] = static_cast<int32_t>(top);
    fill(top, from, to);
    from = to;
  }
  shape->corners.reserve(tris.size() * 3);
  for (const Tri& t : tris) {
    shape->corners.push_back(t.a);
    shape->corners.push_back(t.b);
    shape->corners.push_back(t.c);
  }
  return shape;
}

UnitWear::Kind UnitWear::KindOf(const std::string& blueprint) {
  // (its id: the file's name without "_proj.bp", lower case)
  std::string id = blueprint;
  const size_t slash = id.rfind('/');
  if (slash != std::string::npos) id = id.substr(slash + 1);
  id = Lower(id);
  const size_t proj = id.find("_proj");
  if (proj != std::string::npos) id = id.substr(0, proj);
  if (Has(id, "napalm") || Has(id, "flame") || Has(id, "incendi") || Has(id, "fire")) return Fire;
  if (Has(id, "overcharge")) return Energy;
  // the game's names: a faction's letter, then what the weapon is for (IF indirect fire, BO bomb, AN anti-navy)
  const std::string kind = id.size() >= 3 && std::string("tacs").find(id[0]) != std::string::npos ? id.substr(1, 2) : std::string();
  if (kind == "if" || kind == "bo" || kind == "an") return Explosive;
  int byWords = Kinetic;
  if (KindByWords(id, &byWords)) return static_cast<Kind>(byWords);
  // else by its faction: the UEF's guns fire slugs, the others' weapons are of light and plasma
  return !id.empty() && (id[0] == 'c' || id[0] == 'a' || id[0] == 's') && kind == "df" ? Energy : Kinetic;
}

UnitWear::Kind UnitWear::KindOf(const std::string& blueprint, const std::string& damageType) {
  const std::string type = Lower(damageType);
  return Has(type, "fire") || Has(type, "napalm") ? Fire : KindOf(blueprint);
}

UnitWear::Kind UnitWear::KindOf(const HitEvent& hit) {
  const std::string type = Lower(hit.damageType);
  if (Has(type, "fire") || Has(type, "napalm")) return Fire;
  if (hit.beam) return Energy;
  if (!hit.blueprint.empty()) return KindOf(hit.blueprint);
  if (!hit.weapon.empty()) {
    // FAF's "<unit id>-<number>-<Label>": the label's words, else the unit's faction (its id's second letter, as
    // "uel0201", "xsl0202": the UEF's guns fire slugs, the others' weapons are of light and plasma)
    const std::string w = Lower(hit.weapon);
    const size_t dash = w.rfind('-');
    int byWords = Kinetic;
    if (KindByWords(dash == std::string::npos ? w : w.substr(dash + 1), &byWords)) return static_cast<Kind>(byWords);
    return w.size() > 1 && (w[0] == 'u' || w[0] == 'x' || w[0] == 'd') && (w[1] == 'a' || w[1] == 'r' || w[1] == 's') ? Energy : Kinetic;
  }
  return hit.radius > 0 ? Explosive : Kinetic;
}

void UnitWear::AddMark(Unit& u, Vec3 rest, Vec3 way, Kind kind, float share, float damage, double clock) {
  const int tier = std::max(TierOf(u.taken), 1);
  // its size: by the hit's calibre (hit points, a cube root: a T1 tank's shell some 0.08 world units, a T3 gun's
  // 0.17, artillery more), by its kind, larger in the higher tiers, never more than some 7% of the unit's length (a
  // medium tank's marks 0.075 at most, a hand's breadth on its 1.5; an experimental's 0.3): marks, not new paint
  float size = 1;
  if (u.model) size = Length(u.model->boundsMax - u.model->boundsMin) * 0.5f * u.model->scale;
  const float byKind = kind == Explosive ? 1.3f : kind == Fire ? 1.3f : kind == Energy ? 1.1f : 1.0f;
  const float radius = std::clamp(0.03f * std::cbrt(std::max(damage, 1.0f)) * byKind, 0.045f, std::max(0.075f * size, 0.06f)) *
                       (1 + 0.15f * static_cast<float>(tier - 1));
  const float strength = std::clamp(0.55f + share * 3, 0.55f, 1.0f);
  const size_t most = tier <= 1 ? 6 : tier == 2 ? 11 : kWearMarks;
  ++u.hits;
  // a hit near another of its kind deepens and widens it; past the tier's most marks, the nearest takes it
  Mark* merge = nullptr;
  float nearest = 1e30f;
  for (Mark& m : u.marks) {
    const float d = Length(m.at - rest);
    if (m.kind == kind && d < 0.7f * (m.radius + radius) && d < nearest) {
      nearest = d;
      merge = &m;
    }
  }
  if (!merge && u.marks.size() >= most) {
    for (Mark& m : u.marks) {
      const float d = Length(m.at - rest) / std::max(m.radius, 1e-3f);
      if (d < nearest) {
        nearest = d;
        merge = &m;
      }
    }
  }
  if (merge) {
    const float a = merge->radius * merge->radius, b = radius * radius;
    merge->at = (merge->at * a + rest * b) * (1 / (a + b));
    merge->way = Normalize(merge->way * a + way * b);
    merge->radius = std::min(std::sqrt(a + 0.3f * b), std::max(merge->radius, std::max(0.1f * size, 0.07f)));
    merge->strength = std::min(1.0f, merge->strength + 0.25f * strength);
    merge->fresh = 1;
    if (b > a * 0.6f) merge->kind = kind;  // (a bigger hit of another kind takes the place over)
    merge->born = clock;
    return;
  }
  Mark m;
  m.at = rest;
  m.way = way;
  m.radius = radius;
  m.kind = kind;
  m.strength = strength;
  m.seed = Hash01(u.hits, static_cast<uint32_t>(u.marks.size()) * 7919u);
  m.born = clock;
  u.marks.push_back(m);
  ++marksMade;
}

void UnitWear::Hit(uint32_t id, const Model* model, Vec3 rest, Vec3 way, Kind kind, float share, float damage, double clock) {
  Unit& u = units_[id];
  u.model = model;
  u.seen = clock;
  u.taken += std::max(share, 0.0f);
  way = Length(way) > 1e-4f ? Normalize(way) : Vec3{0, -1, 0};
  // (on the first part of the model the shot meets along its way through that point, where the model has a hit shape)
  if (model && model->hitShape) {
    const float reach = Length(model->boundsMax - model->boundsMin) * model->scale;
    Vec3 onto, ontoWay;
    if (CastOnto(*model, {}, rest - way * reach, way, 2 * reach, &onto, &ontoWay)) rest = onto;
  }
  AddMark(u, rest, way, kind, share, damage, clock);
  u.snow = std::max(0.0f, u.snow - (0.12f + 2.5f * share));
}

void UnitWear::Heal(uint32_t id, float share) {
  auto it = units_.find(id);
  if (it == units_.end() || share <= 0) return;
  // (some two fifths of its life given back mends its marks: the patches go on as the engineers work)
  for (Mark& m : it->second.marks) m.fresh = std::max(0.0f, m.fresh - share * 2.5f);
  it->second.burn = std::max(0.0f, it->second.burn - share * 2);
  ++heals;
}

int UnitWear::Tier(uint32_t id) const {
  auto it = units_.find(id);
  return it == units_.end() ? 0 : TierOf(it->second.taken);
}

void UnitWear::Watch(const Frame& frame, double clock, float snowing, float snowLies, const std::function<const Model*(uint32_t, bool)>& modelOf) {
  if (haveTick_ && frame.tick == lastTick_) return;  // (once a beat)
  const float dt = haveTick_ && frame.tick > lastTick_ ? std::min(static_cast<float>(frame.tick - lastTick_) * 0.1f, 1.0f) : 0.0f;
  const bool restarted = haveTick_ && frame.tick < lastTick_;  // (a stand's play begun anew, a replay started over)
  lastTick_ = frame.tick;
  haveTick_ = true;
  if (restarted) {
    units_.clear();
    shots_.clear();
    told_.clear();
  }
  // the hits told for this beat and before (those of later beats wait for theirs)
  const auto due = std::stable_partition(told_.begin(), told_.end(), [&](const HitEvent& t) { return t.tick <= frame.tick; });
  std::vector<HitEvent> later(std::make_move_iterator(due), std::make_move_iterator(told_.end()));
  told_.erase(due, told_.end());
  const std::vector<std::string>* names = frame.blueprints;
  // the units of this beat, alive and finished or being built, with their models, places and boxes
  struct Here {
    const FrameUnit* f;
    Unit* u;
    Vec3 position;
    Quat turn;
    Vec3 lo, hi;     // its model's box at rest, world units, its own frame
    float radius;    // of the sphere round it
    Vec3 centre;     // in the world
    std::vector<const HitEvent*> told;  // the hits told on it (Tell) since the last beat
    float firing = 0;  // how much it fired this beat (its snow shakes off)
  };
  std::vector<Here> here;
  here.reserve(frame.units.size());
  // its box from its model (without one a stand-in)
  auto boxOf = [](Here& h) {
    const Model* m = h.u->model;
    if (m && m->ok) {
      h.lo = m->boundsMin * m->scale;
      h.hi = m->boundsMax * m->scale;
    } else {
      h.lo = {-0.5f, 0, -0.5f};
      h.hi = {0.5f, 0.8f, 0.5f};
    }
    h.radius = Length(h.hi - h.lo) * 0.5f;
    h.centre = h.position + Rotate(h.turn, (h.lo + h.hi) * 0.5f);
  };
  // a unit hit: its model made now if it was not yet
  auto madeFor = [&](Here& h) {
    if (h.u->model && h.u->model->ok) return;
    if (const Model* m = modelOf(h.f->blueprint, true)) {
      h.u->model = m;
      boxOf(h);
    }
  };
  for (const FrameUnit& f : frame.units) {
    if (f.dead) continue;
    Unit& u = units_[f.id];
    const bool fresh = u.blueprint != f.blueprint;
    if (fresh) {  // (first seen, or another unit under the id: a new one)
      u = Unit{};
      u.blueprint = f.blueprint;
      // a unit fresh from the factory has no snow; one there from the start has what lies everywhere
      u.snow = f.fractionComplete < 1 || f.beingBuilt ? 0.0f : 1.0f;
      u.health = f.health;
      u.most = f.maxHealth;
    }
    if (const Model* made = modelOf(f.blueprint, false)) u.model = made;  // (made already; else when it is hit)
    u.seen = clock;
    Here h{};
    h.f = &f;
    h.u = &u;
    h.position = {f.position[0], f.position[1], f.position[2]};
    h.turn = {f.orientation[0], f.orientation[1], f.orientation[2], f.orientation[3]};
    boxOf(h);
    here.push_back(h);
  }
  for (const HitEvent& t : told_)
    for (Here& h : here)
      if (h.f->id == t.unit) h.told.push_back(&t);
  // where on the model at rest a shot through a point of the world along a way lies: in the unit's own frame, the
  // first part of the model it meets on that line (its hit shape, each part as its bone is posed), or, where the line
  // misses the model (the game's collision box is larger), the first part toward the model's middle from the point;
  // without a hit shape, back through the bone whose part of the model is nearest to it as posed (Model::boneBoxes),
  // onto that part's box
  auto toRest = [&](const Here& h, Vec3 world, Vec3 way, Vec3* rest, Vec3* restWay) {
    const Quat back = Conjugate(h.turn);
    Vec3 local = Rotate(back, world - h.position), localWay = Rotate(back, way);
    *rest = local;
    *restWay = localWay;
    Vec3 lo = h.lo, hi = h.hi;  // (the box of the part it lies on, at rest)
    const Model* m = h.u->model;
    if (m && m->ok && m->hitShape && h.f->firstBone + h.f->boneCount <= frame.bones.size()) {
      std::vector<Unpose> poses(m->bones.size());
      for (uint32_t i = h.f->firstBone; i < h.f->firstBone + h.f->boneCount; ++i) {
        const FrameBone& fb = frame.bones[i];
        if (fb.bone >= m->bones.size()) continue;
        // (the bone's matrix as the model renderer makes it, MeshRenderer::BuildRows, undone)
        const ModelBone& restBone = m->bones[fb.bone];
        const Quat q = Mul(Quat{fb.orientation[0], fb.orientation[1], fb.orientation[2], fb.orientation[3]}, Conjugate(restBone.rotation));
        poses[fb.bone].undo = Conjugate(q);
        poses[fb.bone].shift = Vec3{fb.position[0], fb.position[1], fb.position[2]} - Rotate(q, restBone.position * m->scale);
        poses[fb.bone].hidden = fb.hidden;
      }
      const Vec3 w = Length(localWay) > 1e-6f ? Normalize(localWay) : Vec3{0, -1, 0};
      if (CastOnto(*m, poses, local - w * (2 * h.radius), w, 4 * h.radius, rest, restWay)) return;
      const Vec3 toMiddle = (h.lo + h.hi) * 0.5f - local;
      if (Length(toMiddle) > 1e-4f && CastOnto(*m, poses, local, Normalize(toMiddle), 2 * h.radius, rest, restWay)) return;
      *rest = local;
      *restWay = localWay;
    }
    if (!m || !m->ok || m->boneBoxes.empty() || h.f->firstBone + h.f->boneCount > frame.bones.size()) {
      *rest = OntoBox(*rest, Normalize(*restWay), lo, hi);
      return;
    }
    const float s = m->scale;
    float best = 1e30f, bestSize = 1e30f;
    // (bones the pose does not list are at rest: the local point itself)
    std::vector<uint8_t> posed(m->boneBoxes.size(), 0);
    auto consider = [&](size_t bone, Vec3 r, Vec3 w) {
      const Model::BoneBox& b = m->boneBoxes[bone];
      if (b.lo.x > b.hi.x) return;
      const float out = OutsideBox(r, b.lo * s, b.hi * s);
      const Vec3 e = (b.hi - b.lo) * s;
      const float size = e.x * e.y + e.y * e.z + e.z * e.x;
      // (the nearest part; among parts it lies in, the smallest: a turret on its hull)
      if (out < best - 0.02f || (out <= best + 0.02f && size < bestSize)) {
        best = out;
        bestSize = size;
        *rest = r;
        *restWay = w;
        lo = b.lo * s;
        hi = b.hi * s;
      }
    };
    for (uint32_t i = h.f->firstBone; i < h.f->firstBone + h.f->boneCount; ++i) {
      const FrameBone& fb = frame.bones[i];
      if (fb.bone >= m->boneBoxes.size() || fb.bone >= m->bones.size()) continue;
      posed[fb.bone] = 1;
      if (fb.hidden) continue;
      // the bone's matrix as the model renderer makes it (MeshRenderer::BuildRows), undone
      const ModelBone& restBone = m->bones[fb.bone];
      const Quat q = Mul(Quat{fb.orientation[0], fb.orientation[1], fb.orientation[2], fb.orientation[3]}, Conjugate(restBone.rotation));
      const Vec3 t = Vec3{fb.position[0], fb.position[1], fb.position[2]} - Rotate(q, restBone.position * s);
      const Quat undo = Conjugate(q);
      consider(fb.bone, Rotate(undo, local - t), Rotate(undo, localWay));
    }
    for (size_t b = 0; b < posed.size(); ++b)
      if (!posed[b]) consider(b, local, localWay);
    *rest = OntoBox(*rest, Normalize(*restWay), lo, hi);
  };
  // the unit a point of the world lies on: inside its box (a little larger: the game's collision box is), the
  // nearest middle; null: none
  auto unitAt = [&](Vec3 p, float pad) -> Here* {
    Here* found = nullptr;
    float nearest = 1e30f;
    for (Here& h : here) {
      const Vec3 d = p - h.centre;
      if (Dot(d, d) > (h.radius + pad) * (h.radius + pad)) continue;
      const Vec3 local = Rotate(Conjugate(h.turn), p - h.position);
      if (OutsideBox(local, h.lo, h.hi) > pad) continue;
      const float far = Length(d);
      if (far < nearest) {
        nearest = far;
        found = &h;
      }
    }
    return found;
  };
  // this beat's hits: the shots that stopped (each once) and the beams' ends; and who fired
  struct Struck {
    Here* on;
    Vec3 at, way;
    Kind kind;
    bool beam;
  };
  std::vector<Struck> struck;
  struct Blast {
    Vec3 at;
    Kind kind;
  };
  std::vector<Blast> blasts;
  auto kindOf = [&](uint32_t blueprint) {
    if (blueprint >= kinds_.size()) kinds_.resize(names ? std::max(names->size(), static_cast<size_t>(blueprint) + 1) : blueprint + 1, -1);
    int8_t& k = kinds_[blueprint];
    if (k < 0) k = static_cast<int8_t>(names && blueprint < names->size() ? KindOf((*names)[blueprint]) : Kinetic);
    return static_cast<Kind>(k);
  };
  for (const FrameProjectile& p : frame.projectiles) {
    const Vec3 at{p.position[0], p.position[1], p.position[2]};
    auto known = shots_.find(p.id);
    if (known == shots_.end()) {
      known = shots_.emplace(p.id, frame.tick).first;
      // a new shot: the unit whose box it leaves (its muzzle's) fires it, and its snow shakes off ([grime] its firing is
      // fighting: soot and dust)
      if (!p.stopped && (snow || grime))
        if (Here* h = unitAt(at, 2.5f)) h->firing += kindOf(p.blueprint) == Explosive ? 2.5f : 1.0f;
    }
    known->second = (known->second & 0x80000000u) | frame.tick;
    if (!p.stopped || (known->second & 0x80000000u)) continue;
    known->second |= 0x80000000u;
    const Vec3 v{p.velocity[0], p.velocity[1], p.velocity[2]};
    const Vec3 way = Length(v) > 1e-3f ? Normalize(v) : Vec3{0, -1, 0};
    const Kind kind = kindOf(p.blueprint);
    if (Here* h = unitAt(at, 0.6f)) struck.push_back({h, at, way, kind, false});
    blasts.push_back({at, kind});
  }
  for (const FrameBeam& b : frame.beams) {
    const Vec3 from{b.start[0], b.start[1], b.start[2]}, to{b.end[0], b.end[1], b.end[2]};
    const Vec3 way = Length(to - from) > 1e-3f ? Normalize(to - from) : Vec3{0, -1, 0};
    if (Here* h = unitAt(to, 0.6f)) struck.push_back({h, to, way, Energy, true});
    if (snow || grime)  // ([grime] as a shot's)
      if (Here* h = unitAt(from, 1.0f)) h->firing += 0.2f;  // (a beam's unit: its heat)
  }
  if (frame.beams.size() + frame.projectiles.size() == 0 && shots_.size() > 20000) shots_.clear();
  if (frame.tick % 100 == 0)  // (shots gone for half a minute are forgotten)
    for (auto it = shots_.begin(); it != shots_.end();)
      it = frame.tick - (it->second & 0x7fffffffu) > 300 ? shots_.erase(it) : std::next(it);
  // the side of a unit's box that faced a point of the world (a blast's centre), a little in from its face, and the
  // way from the point to it
  auto facingSide = [](const Here& h, Vec3 point, Vec3* at, Vec3* way) {
    const Vec3 local = Rotate(Conjugate(h.turn), point - h.position);
    const Vec3 mid = (h.lo + h.hi) * 0.5f;
    Vec3 side{std::clamp(local.x, h.lo.x, h.hi.x), std::clamp(local.y, h.lo.y, h.hi.y), std::clamp(local.z, h.lo.z, h.hi.z)};
    side = side + (mid - side) * 0.1f;
    *at = h.position + Rotate(h.turn, side);
    *way = Length(side - local) > 1e-3f ? Rotate(h.turn, Normalize(side - local)) : Vec3{0, -1, 0};
  };
  // each unit: what its health did, its burning, its snow
  for (Here& h : here) {
    Unit& u = *h.u;
    const FrameUnit& f = *h.f;
    u.built = f.fractionComplete >= 1 && !f.beingBuilt;
    const float before = u.snow;
    const float most = std::max(f.maxHealth, 1e-3f);
    // the hits told on it: exact (each its own place, way, kind and amount)
    for (const HitEvent* t : h.told) {
      if (t->shielded || !u.built) {
        u.snow -= 0.05f;
        continue;
      }
      const float share = t->amount / most;
      if (scars) {
        madeFor(h);
        // (an area's damage: on the side that faced the blast's centre; a hit without a way: toward the unit)
        Vec3 at = t->at, way = t->way;
        if (t->radius > 0) facingSide(h, t->at, &at, &way);
        else if (Length(way) <= 1e-4f) way = h.centre - at;
        Vec3 rest, restWay;
        toRest(h, at, Length(way) > 1e-4f ? Normalize(way) : Vec3{0, -1, 0}, &rest, &restWay);
        AddMark(u, rest, restWay, KindOf(*t), share, t->amount, clock);
      }
      u.toldAhead += t->amount;
      u.snow -= 0.12f + 2.5f * share;
      ++toldHits;
    }
    if (f.maxHealth > 0 && u.health >= 0) {
      float drop = u.health - f.health;
      // (what was told is already marked: only a drop beyond it is guessed at)
      if (drop > 0) {
        u.taken += std::min(drop, u.toldAhead) / most;
        const float matched = std::min(drop, u.toldAhead);
        u.toldAhead -= matched;
        drop -= matched;
      }
      if (h.told.empty()) u.toldAhead = 0;  // (told hits wait one beat for their drop, no longer)
      // (with every hit told, a drop no hit was told for is no damage: a unit being reclaimed, a script's SetHealth)
      if (drop > most * 1e-4f && u.built && !exactHits) {
        const float share = drop / most;
        u.taken += share;
        if (scars) {
          madeFor(h);
          // the hits on it this beat share what it lost
          std::vector<const Struck*> on;
          for (const Struck& s : struck)
            if (s.on == &h) on.push_back(&s);
          for (const Struck* s : on) {
            Vec3 rest, way;
            toRest(h, s->at, s->way, &rest, &way);
            AddMark(u, rest, way, s->kind, share / static_cast<float>(on.size()), drop / static_cast<float>(on.size()), clock);
            ++(s->beam ? beamHits : shotHits);
          }
          // no shot seen on it: the nearest blast round it marks the side that faced it, else a dent somewhere on its
          // upper half (a recording has no shots); not for a small drop with no blast near (below 3% of its health:
          // being reclaimed, burning, a splinter: in a game of M28s nine in ten drops no shot was seen for were power
          // generators being reclaimed)
          const Blast* near = nullptr;
          if (on.empty()) {
            float nearest = 3 + h.radius * 1.5f;
            for (const Blast& b : blasts) {
              const float d = Length(b.at - h.centre);
              if (d < nearest) {
                nearest = d;
                near = &b;
              }
            }
          }
          if (on.empty() && (near || share >= 0.03f)) {
            Vec3 at, way;
            Kind kind = Kinetic;
            if (near) {
              facingSide(h, near->at, &at, &way);
              kind = near->kind == Kinetic || near->kind == Energy ? Explosive : near->kind;
              ++blastHits;
            } else {
              const float a = Hash01(f.id, u.hits * 3 + 1) * 6.2831853f, up = 0.15f + 0.7f * Hash01(f.id, u.hits * 3 + 2);
              const Vec3 dir{std::cos(a) * std::cos(up), std::sin(up), std::sin(a) * std::cos(up)};
              const Vec3 mid = (h.lo + h.hi) * 0.5f, half = (h.hi - h.lo) * 0.5f;
              const Vec3 local = mid + Vec3{dir.x * half.x, dir.y * half.y, dir.z * half.z};
              at = h.position + Rotate(h.turn, local);
              way = Rotate(h.turn, -dir);
              ++unseenHits;
            }
            Vec3 rest, restWay;
            toRest(h, at, way, &rest, &restWay);
            AddMark(u, rest, restWay, kind, share, drop, clock);
          }
        }
        u.snow -= 0.12f + 2.5f * share;  // (a hit shakes it off)
      } else if (drop < -most * 1e-4f && u.built) {
        Heal(f.id, -drop / most);
      }
      if (f.health >= f.maxHealth * 0.999f)
        for (Mark& m : u.marks) m.fresh = 0;  // (whole again: every mark mended)
      // burning: the game's fire on it below half its health (FxDamage2), more below a quarter (FxDamage3)
      const float left = f.health / most;
      if (left < 0.5f && u.built) {
        u.burn = std::min(1.0f, u.burn + dt / (left < 0.25f ? 20.0f : 45.0f));
        u.snow -= dt / (left < 0.25f ? 6.0f : 15.0f);
      }
    }
    // [grime] its fighting (hits taken, its own firing, blasts near it) gathers soot and dust; its health for the damage shown
    if ((grime || damage) && !u.grimeTold) {
      const float lost = u.health >= 0 && f.maxHealth > 0 ? std::max(u.health - f.health, 0.0f) / most : 0.0f;
      if (grime && u.built) {
        float near = 0;  // (blasts round it, the nearer the more; a blast's earth thrown up is dust, its smoke soot)
        for (const Blast& b : blasts) {
          if (b.kind != Explosive && b.kind != Fire) continue;
          const float reach = 3 + h.radius * 1.5f, d = Length(b.at - h.centre);
          if (d < reach) near += 1 - d / reach;
        }
        u.combat = std::clamp(u.combat - dt / 30 + lost * 3 + h.firing * 0.04f + near * 0.15f, 0.0f, 1.0f);
        // (some four minutes of hard fighting to its most soot, a little more for dust; every blast near adds its own)
        u.gSoot = std::min(1.0f, u.gSoot + dt * u.combat * 0.004f + near * 0.02f + lost * 0.25f + dt * u.burn * 0.01f);
        u.gDust = std::min(1.0f, u.gDust + dt * u.combat * 0.003f + near * 0.015f);
      }
      if (damage) {
        const float share = std::clamp(f.health / most, 0.0f, 1.0f);
        if (!u.built || u.recent < 0) {  // (being built: its health grows with the work, no damage)
          u.lowest = u.built ? share : 1.0f;
          u.recent = u.built ? share : -1.0f;
        } else {
          u.lowest = std::min(u.lowest, share);
          // (what it lost glows for some seconds where the damage set says it is hot: from its health then down to now)
          u.recent = share >= u.recent ? share : u.recent + (share - u.recent) * (1 - std::exp(-dt / 2.5f));
        }
      }
    }
    u.health = f.health;
    u.most = f.maxHealth;
    // its speed
    if (u.placed && dt > 0) u.speed += (Length(h.position - u.place) / dt - u.speed) * 0.5f;
    u.place = h.position;
    u.placed = true;
    // its snow
    if (snow) {
      if (!u.built) {
        u.snow = 0;
      } else {
        // blasts near shake it off, the nearer the more
        for (const Blast& b : blasts) {
          if (b.kind != Explosive) continue;
          const float reach = 3 + h.radius * 1.5f, d = Length(b.at - h.centre);
          if (d < reach) u.snow -= 0.6f * (1 - d / reach);
        }
        u.snow -= h.firing * 0.05f;
        // moving: shaken toward what packed snow stays on its flat tops (none when fast, or flying)
        const bool flying = (f.layer & 0x10) != 0;
        const float stays = flying ? 0.0f : std::clamp(0.35f - 0.06f * u.speed, 0.0f, 0.35f);
        if (u.speed > 0.25f && u.snow > stays) u.snow = std::max(stays, u.snow - dt * std::min(0.4f * u.speed, 2.0f));
        // standing still in snowfall it builds up (as fast as on the ground: two and a half minutes)
        if (snowing > 0 && (u.speed <= 0.25f || u.snow < stays)) u.snow += dt / 150;
      }
      u.snow = std::clamp(u.snow, 0.0f, 1.0f);
      // much at once: a puff of it falling (not more than one a half second)
      const float fell = (before - u.snow) * snowLies;
      if (fell > 0.1f && before * snowLies > 0.15f && clock - u.puffAt > 0.5) {
        u.puffAt = clock;
        puffs_.push_back({h.centre.x, h.centre.y + (h.hi.y - h.lo.y) * 0.3f, h.centre.z, std::clamp(h.radius * (0.4f + fell), 0.4f, 6.0f)});
      }
    }
  }
  told_.swap(later);
  // the dead and those long not seen are forgotten
  for (const FrameUnit& f : frame.units)
    if (f.dead) {
      auto it = units_.find(f.id);
      if (it != units_.end() && clock - it->second.seen > 10) units_.erase(it);
    }
  if (frame.tick % 50 == 0)
    for (auto it = units_.begin(); it != units_.end();) it = clock - it->second.seen > 60 || clock < it->second.seen - 1 ? units_.erase(it) : std::next(it);
}

const WearLook* UnitWear::Look(uint32_t id, double clock, const Model* drawn) {
  auto it = units_.find(id);
  if (it == units_.end()) return nullptr;
  Unit& u = it->second;
  if (!u.box && drawn && drawn->ok) {  // [unit dirt] (its box, from the model it is drawn with)
    u.lo = drawn->boundsMin * drawn->scale;
    u.hi = drawn->boundsMax * drawn->scale;
    u.box = true;
  }
  WearLook& w = u.look;
  w.general = 0;
  w.count = 0;
  w.snow = snow ? u.snow : -1;
  if (scars) {
    // its wear over all: none in the first tier, growing through the second (to 2) and the third (to 3), and its
    // burning's soot
    const float t = u.taken;
    w.general = std::clamp((t - 0.35f) / 0.65f, 0.0f, 1.0f) * 2 + std::clamp(t - 1, 0.0f, 1.0f) + u.burn * 0.8f;
    w.general = std::min(w.general, 3.0f);
    for (const Mark& m : u.marks) {
      if (w.count >= kWearMarks) break;
      const float age = static_cast<float>(clock - m.born);
      // hot: an energy weapon's mark glows for a moment and cools in some seconds; a blast's embers for one
      float heat = 0;
      if (age >= 0 && m.fresh > 0.5f) heat = m.kind == Energy ? std::exp(-std::max(age - 0.3f, 0.0f) / 2.2f) : m.kind == Explosive ? std::exp(-age / 0.8f) * 0.7f : 0.0f;
      Vec4* r = w.rows + w.count * 3;
      r[0] = {m.at.x, m.at.y, m.at.z, m.radius};
      r[1] = {m.way.x, m.way.y, m.way.z, static_cast<float>(m.kind)};
      r[2] = {m.fresh, heat, m.seed, m.strength};
      ++w.count;
    }
  }
  // [unit dirt] its own wetness, mud and dust (once worked out and its model's box known)
  w.dirt = dirt && u.dirty && u.box;
  if (w.dirt) DirtRows(id, u, &w);
  // [grime] its grime, weather dirt and damage (its box from its model where the dirt has not found it)
  if ((grime || damage) && !u.box && u.model && u.model->ok) {
    u.lo = u.model->boundsMin * u.model->scale;
    u.hi = u.model->boundsMax * u.model->scale;
    u.box = true;
  }
  w.grime = false;
  if ((grime || damage) && u.box) GrimeRows(id, u, &w);
  if (w.count == 0 && w.general <= 0 && w.snow < 0 && !w.dirt && !w.grime) return nullptr;
  return &w;
}

// [grime] (unit_wear.h, WearLook::grimeRows; unit_grime.h says what each is)
void UnitWear::GrimeRows(uint32_t id, const Unit& u, WearLook* w) const {
  const float height = std::max(u.hi.y - u.lo.y, 0.05f);
  const float dust = grime ? u.gDust : 0.0f, caked = grime ? u.gCaked : 0.0f, soot = grime ? u.gSoot : 0.0f;
  const float splash = grime ? u.splash : 0.0f;
  // frost where snow lies (its own share of it with the snow on units), none on a unit hot from fighting or burning
  const float frost = grime ? std::clamp(frostLying, 0.0f, 1.0f) * (snow ? u.snow : 1.0f) * (1 - 0.8f * u.combat) * (1 - u.burn) : 0.0f;
  const float wet = grime ? (u.dirty ? u.wet : std::clamp(groundWet, 0.0f, 1.0f)) : 0.0f;
  // its health shown (a unit being built shows none); what was mended stays as trophies: the first three tenths of the
  // worst damage it lived through, patched (from that order of the damage on), where it is whole again above it
  float share = -1, recent = -1, mend = 2;
  if (damage && u.most > 0 && u.recent >= 0) {
    share = std::clamp(u.health / u.most, 0.0f, 1.0f);
    recent = std::max(u.recent, share);
    if (u.lowest < share - 0.02f) mend = 1 - (1 - u.lowest) * 0.3f;
  }
  const bool shows = dust + caked + soot + frost + splash > 0.002f || (share >= 0 && (share < 0.999f || mend <= 1));
  if (!shows) return;
  w->grime = true;
  w->grimeRows[0] = {dust, caked, soot, frost};
  w->grimeRows[1] = {wet, splash, share, recent};
  w->grimeRows[2] = {u.lo.y, 1 / height, mend, Hash01(id, 0x6e1au)};
}

void UnitWear::SetGrime(uint32_t id, const Model* model, float dust, float caked, float soot, float health, float lowest, float recent, float splash,
                        double clock) {
  Unit& u = units_[id];
  u.model = model;
  u.seen = clock;
  u.grimeTold = true;
  u.gDust = std::clamp(dust, 0.0f, 1.0f);
  u.gCaked = std::clamp(caked, 0.0f, 1.0f);
  u.gSoot = std::clamp(soot, 0.0f, 1.0f);
  u.splash = std::clamp(splash, 0.0f, 1.0f);
  u.most = 1;
  u.health = std::clamp(health, 0.0f, 1.0f);
  u.lowest = std::clamp(std::min(lowest, u.health), 0.0f, 1.0f);
  u.recent = std::max(recent, u.health);
  u.box = model && model->ok;
  if (u.box) {
    u.lo = model->boundsMin * model->scale;
    u.hi = model->boundsMax * model->scale;
  }
}

std::string UnitWear::GrimeSummary() const {
  int n = 0, hurt = 0;
  double dust = 0, caked = 0, soot = 0, splash = 0, combat = 0;
  for (const auto& [id, u] : units_) {
    ++n;
    dust += u.gDust;
    caked += u.gCaked;
    soot += u.gSoot;
    splash += u.splash;
    combat += u.combat;
    if (u.most > 0 && u.health < u.most * 0.999f) ++hurt;
  }
  char b[200];
  snprintf(b, sizeof b, "%d units, dust %.3f, caked mud %.3f, soot %.3f, splashed %.3f, fighting %.3f, %d hurt", n, n ? dust / n : 0.0,
           n ? caked / n : 0.0, n ? soot / n : 0.0, n ? splash / n : 0.0, n ? combat / n : 0.0, hurt);
  return b;
}

// [unit dirt] (unit_wear.h says what it does)
void UnitWear::DirtRows(uint32_t id, const Unit& u, WearLook* w) const {
  const float height = std::max(u.hi.y - u.lo.y, 0.05f), half = std::max((u.hi.z - u.lo.z) * 0.5f, 0.05f);
  // how high the mud reaches: on what rolls up to some two fifths of its hull's height with all the mud it can hold
  // (its box's height, but not over three fifths of its length: an aerial is no hull), on a walker a quarter of its
  // height (its feet and shins); on the largest at most 0.3 world units over a seventh of their height (an
  // experimental's mud a man's height up, not over half of it); a little mud only on its running gear
  const float body = u.moves == Walks ? height : std::min(height, 0.6f * std::max(u.hi.z - u.lo.z, u.hi.x - u.lo.x));
  const float most = std::min(body * (u.moves == Walks ? 0.26f : 0.42f), 0.3f + body * 0.15f);
  const float reach = most * (0.25f + 0.75f * std::sqrt(std::clamp(u.mud, 0.0f, 1.0f)));
  w->dirtRows[0] = {u.wet, u.mud, u.mudWet, u.dust};
  w->dirtRows[1] = {u.lo.y, height, (u.lo.z + u.hi.z) * 0.5f, 1 / half};
  w->dirtRows[2] = {reach, u.front * reach * 0.6f, u.sandy, Hash01(id, 0x5eed)};
}

int UnitWear::MovesOf(const std::string& blueprint) {
  auto known = moves_.find(blueprint);
  if (known != moves_.end()) return known->second;
  int moves = Rolls;
  BpValue bp;
  if (!blueprint.empty() && readBlueprint && readBlueprint(blueprint, &bp)) {
    // (as movement dust reads it, movement_dust.cpp's KindOf)
    const std::string motion = bp["Physics"]["MotionType"].StringOr(std::string());
    if (motion == "RULEUMT_Air") moves = Flies;
    else if (motion == "RULEUMT_Hover") moves = Hovers;
    else if (motion == "RULEUMT_Land" || motion == "RULEUMT_Amphibious" || motion == "RULEUMT_AmphibiousFloating")
      moves = bp["Display"]["MovementEffects"]["Land"]["Footfall"].IsTable() || !bp["Display"]["AnimationWalk"].StringOr(std::string()).empty() ? Walks : Rolls;
    else if (!motion.empty() && motion != "RULEUMT_None") moves = Sails;
  }
  moves_[blueprint] = moves;
  return moves;
}

void UnitWear::Dirt(const Frame& frame, double clock, const DirtWeather& weather, const std::function<const Model*(uint32_t, bool)>& modelOf) {
  const float rain = std::clamp(weather.rain, 0.0f, 1.0f), groundWet = std::clamp(weather.wet, 0.0f, 1.0f);
  const float sun = std::clamp(weather.sun, 0.0f, 1.0f), lying = std::clamp(weather.snowLying, 0.0f, 1.0f);
  const float pace = std::max(weather.pace, 0.0f);
  for (const FrameUnit& f : frame.units) {
    if (f.dead) continue;  // (a dying unit keeps what it had)
    Unit& u = units_[f.id];
    const Vec3 at{f.position[0], f.position[1], f.position[2]};
    const bool built = f.fractionComplete >= 1 && !f.beingBuilt;
    u.seen = clock;
    if (u.dirtBlueprint != f.blueprint || clock < u.dirtClock - 1) {
      // first seen, another unit under its id, or the game played again from earlier: a unit fresh from the factory
      // is dry and clean, one there from the start as wet as the ground
      u.dirtBlueprint = f.blueprint;
      u.dirty = true;
      u.wet = built ? groundWet : 0.0f;
      u.mud = u.mudWet = u.dust = u.sandy = u.front = 0;
      if (!u.grimeTold) u.gDust = u.gCaked = u.gSoot = u.splash = u.combat = 0;  // [grime] (clean from the factory too)
      if (startGiven_ && built) {  // (--dirt-test: as told)
        u.wet = start_[0], u.mud = start_[1], u.mudWet = start_[2], u.dust = start_[3], u.sandy = start_[4], u.front = start_[5];
      }
      u.moves = frame.blueprints && f.blueprint < frame.blueprints->size() ? MovesOf((*frame.blueprints)[f.blueprint]) : Rolls;
      u.dirtClock = clock;
      u.dirtPlace = at;
      u.dirtSpeed = 0;
      u.box = false;
    }
    if (!u.box)
      if (const Model* m = modelOf(f.blueprint, false)) {
        u.lo = m->boundsMin * m->scale;
        u.hi = m->boundsMax * m->scale;
        u.box = true;
      }
    const float step = static_cast<float>(std::clamp(clock - u.dirtClock, 0.0, 0.5));
    u.dirtClock = clock;
    if (step <= 0) continue;
    // its speed (a jump of more than 20 world units in a step: put there, not driven)
    const float moved = Length(at - u.dirtPlace);
    u.dirtPlace = at;
    if (moved < 20) u.dirtSpeed += (moved / step - u.dirtSpeed) * std::min(step * 4, 1.0f);
    const float v = std::clamp(u.dirtSpeed, 0.0f, 6.0f);
    const float dt = step * pace;
    if (!built) {  // (under construction: as it comes out of the factory)
      u.wet = u.mud = u.dust = 0;
      continue;
    }
    // under water (an amphibious unit on the bottom, or wading): soaked, and the water washes it
    const bool onGround = u.moves == Rolls || u.moves == Walks;
    const bool underWater = (f.layer & 6u) != 0 || (onGround && at.y < weather.waterLevel - 0.05f);
    // its wetness: soaked within a quarter of a minute of rain; dry in some three minutes, faster as it drives (the
    // wind of it) and in sun
    if (underWater) u.wet = 1;
    else if (rain > 0) u.wet = std::min(1.0f, u.wet + dt * (0.25f + rain) / 12);
    else u.wet = std::max(0.0f, u.wet - dt / 200 * (1 + 0.15f * std::min(v, 4.0f)) * (1 + 1.5f * sun));
    // what it drives through (what rolls or walks, on the ground, moving)
    if (onGround && !underWater && (f.layer & 0x10u) == 0 && v > 0.3f) {
      float k[6];
      GroundAt(at.x, at.z, k);
      // mud's ground gives mud, the more and the wetter the wetter it is (dry, it is caked earth: none); dirt turns
      // to mud in the wet (as the ground's shader has it), grass's earth a little; dry sand, dry dirt and dry mud
      // (caked earth) give dust (none in rain or where snow lies)
      const float muddy = k[1] * std::pow(groundWet, 0.7f) + k[4] * groundWet * 0.8f + k[3] * groundWet * 0.15f;
      const float sand = k[2], dusts = k[2] + k[4] * 0.6f + k[1] * 0.5f + k[3] * 0.05f;
      const float dusty = dusts * std::pow(1 - groundWet, 1.5f) * (rain > 0 ? 0.0f : 1.0f) * (1 - lying);
      const float thrown = std::min(v, 4.0f) / 4 * (u.moves == Walks ? 0.6f : 1.0f);  // (the faster, the more)
      const float gain = muddy * thrown * dt / 18;  // (full in some twenty seconds of fast driving through mud)
      if (gain > 0) {
        const float sum = std::max(u.mud + gain, 1e-4f);
        u.mudWet = (u.mudWet * u.mud + std::clamp(0.4f + 0.6f * groundWet, 0.0f, 1.0f) * gain) / sum;  // (as wet as the mud thrown)
        u.front = (u.front * u.mud + std::min(v / 4, 1.0f) * gain) / sum;
        u.mud = std::min(1.0f, u.mud + gain);
      }
      const float dgain = dusty * thrown * dt / 25;
      if (dgain > 0) {
        u.sandy = (u.sandy * u.dust + (dusts > 0 ? sand / dusts : 0) * dgain) / std::max(u.dust + dgain, 1e-4f);
        u.dust = std::min(1.0f, u.dust + dgain);
      }
      if (grime && !u.grimeTold) {  // [grime] what it drives through leaves its grime, slowly: a crust of mud, a coat of dust
        u.gCaked = std::min(1.0f, u.gCaked + gain * 0.06f + dgain * 0.02f);  // (some five minutes of fast driving through mud)
        u.gDust = std::min(1.0f, u.gDust + dgain * 0.12f);                   // (some three and a half through dry dirt)
      }
    }
    // [grime] weather dirt: rain splashes mud from soft ground onto its lower body (the more as it drives), drying on and
    // flaking off over a quarter of an hour; rain washes dust off in minutes, soot and caked mud slowly; water at once
    if (grime && !u.grimeTold) {
      if (rain > 0 && onGround && !underWater) {
        float k[6];
        GroundAt(at.x, at.z, k);
        const float soft = std::clamp(k[1] + k[4] + 0.3f * k[3], 0.0f, 1.0f);
        u.splash = std::min(1.0f, u.splash + dt * rain * soft * (0.15f + 0.85f * std::min(v / 3, 1.0f)) / 90);
      } else {
        u.splash = std::max(0.0f, u.splash - dt / 900 * (1 + sun) - (underWater ? dt * 0.2f : 0.0f));
      }
      u.gDust = std::max(0.0f, u.gDust - dt * (rain / 240 + (underWater ? 0.05f : 0.0f)));
      u.gSoot = std::max(0.0f, u.gSoot - dt * (rain / 1500 + (underWater ? 0.01f : 0.0f)));
      u.gCaked = std::max(0.0f, u.gCaked - dt * (rain / 3000 + (underWater ? 0.01f : 0.0f)));
    }
    // its mud dries in some two and a half minutes (faster in sun), and in rain is wet again; dry it cracks and flakes
    // off over some six minutes (the faster as it drives), rain washes it off in a minute and a half, water at once
    if (rain > 0) u.mudWet = std::min(1.0f, u.mudWet + dt * rain / 20);
    else u.mudWet = std::max(0.0f, u.mudWet - dt / 150 * (1 + 1.2f * sun) * (1 + 0.1f * v));
    const float dry = 1 - u.mudWet;
    u.mud = std::max(0.0f, u.mud - dt * (dry * dry / 480 * (1 + 0.15f * std::min(v, 4.0f)) + rain / 90 + (underWater ? 0.2f : 0.0f)));
    // its dust: blown off slowly, washed off by rain or water within seconds
    u.dust = std::max(0.0f, u.dust - dt * (1.0f / 400 + rain / 15 + (underWater ? 0.3f : 0.0f)));
  }
  static const bool debug = std::getenv("MOHO64_DEBUG_DIRT") != nullptr;
  if (debug && static_cast<int64_t>(clock) / 5 != static_cast<int64_t>(dirtSwept_) / 5)
    printf("render: unit dirt at %.1f s (rain %.2f, ground wet %.2f, sun %.2f): %s\n", clock, rain, groundWet, sun, DirtSummary().c_str());
  // (those long not seen are forgotten, as Watch does; units told straight by SetDirt are kept)
  if (static_cast<int64_t>(clock) / 5 != static_cast<int64_t>(dirtSwept_) / 5) {
    dirtSwept_ = clock;
    for (auto it = units_.begin(); it != units_.end();)
      it = it->second.dirtBlueprint != kToldDirt && (clock - it->second.seen > 60 || clock < it->second.seen - 1) ? units_.erase(it) : std::next(it);
  }
}

std::string UnitWear::DirtSummary() const {
  int n = 0;
  double wet = 0, mud = 0, mudWet = 0, dust = 0, most = 0;
  for (const auto& [id, u] : units_) {
    if (!u.dirty) continue;
    ++n;
    wet += u.wet;
    mud += u.mud;
    mudWet += u.mudWet * u.mud;
    dust += u.dust;
    most = std::max(most, static_cast<double>(u.mud));
  }
  char b[200];
  snprintf(b, sizeof b, "%d units, wet %.2f, mud %.2f (most %.2f, its wetness %.2f), dust %.2f", n, n ? wet / n : 0.0, n ? mud / n : 0.0, most,
           mud > 0 ? mudWet / mud : 0.0, n ? dust / n : 0.0);
  return b;
}

void UnitWear::SetDirt(uint32_t id, const Model* model, float wet, float mud, float mudWet, float dust, float sandy, float front, bool walks, double clock) {
  Unit& u = units_[id];
  u.model = model;
  u.seen = clock;
  u.dirtBlueprint = kToldDirt;
  u.dirty = true;
  u.wet = std::clamp(wet, 0.0f, 1.0f);
  u.mud = std::clamp(mud, 0.0f, 1.0f);
  u.mudWet = std::clamp(mudWet, 0.0f, 1.0f);
  u.dust = std::clamp(dust, 0.0f, 1.0f);
  u.sandy = std::clamp(sandy, 0.0f, 1.0f);
  u.front = std::clamp(front, 0.0f, 1.0f);
  u.moves = walks ? Walks : Rolls;
  u.dirtClock = clock;
  u.box = model && model->ok;
  if (u.box) {
    u.lo = model->boundsMin * model->scale;
    u.hi = model->boundsMax * model->scale;
  }
}

void UnitWear::SetGround(const MapData& map, const float kinds[10][6]) {
  const auto started = std::chrono::steady_clock::now();
  groundMade_ = true;
  ground_.clear();
  groundW_ = groundH_ = 0;
  if (map.width <= 0 || map.height <= 0) return;
  // the masks (the second only for the ground's look of eight layers, as its shader reads them), as movement dust reads
  // them (movement_dust.cpp, MakeGrid): plain ones where they lie in the map's file, compressed ones decoded whole
  struct Mask {
    DdsImage image;
    std::vector<uint8_t> decoded;
    const uint8_t* texels = nullptr;
    int w = 0, h = 0;
    bool swap = false;  // (stored blue first)
    bool Open(const std::string& dds) {
      std::string why;
      if (dds.empty() || !ParseDds(dds, &image, &why) || image.width <= 0 || image.height <= 0) return false;
      w = image.width;
      h = image.height;
      if (image.format == gfx::Format::RGBA8 || image.format == gfx::Format::BGRA8) {
        texels = image.data;
        swap = image.format == gfx::Format::BGRA8;
        return true;
      }
      if (!DecodeRgba(image, 0, &decoded)) return false;
      texels = decoded.data();
      return true;
    }
    float At(float u, float v, int k) const {  // (channel k: 0 red .. 3 alpha, as a shader reads it)
      const uint8_t* t = texels + (static_cast<size_t>(std::min(static_cast<int>(v * static_cast<float>(h)), h - 1)) * static_cast<size_t>(w) +
                                   static_cast<size_t>(std::min(static_cast<int>(u * static_cast<float>(w)), w - 1))) * 4;
      return static_cast<float>(t[swap && k != 1 && k != 3 ? 2 - k : k]) / 255.0f;
    }
  };
  Mask low, high;
  const bool haveLow = low.Open(map.maskLow);
  const bool haveHigh = map.shader == "TTerrainXP" && high.Open(map.maskHigh);
  const float width = static_cast<float>(map.width), height = static_cast<float>(map.height);
  groundCell_ = std::max(2.0f, std::max(width, height) / 512.0f);
  groundW_ = static_cast<int>(std::ceil(width / groundCell_)) + 1;
  groundH_ = static_cast<int>(std::ceil(height / groundCell_)) + 1;
  ground_.assign(static_cast<size_t>(groundW_) * static_cast<size_t>(groundH_) * 6, 0);
  double mudSum = 0;
  for (int gz = 0; gz < groundH_; ++gz)
    for (int gx = 0; gx < groundW_; ++gx) {
      const float u = std::min(static_cast<float>(gx) * groundCell_ / width, 1.0f), v = std::min(static_cast<float>(gz) * groundCell_ / height, 1.0f);
      float k[6];
      std::copy(kinds[0], kinds[0] + 6, k);
      // (laid over each other as the ground's shader lays the layers, Terrain::GroundKinds: each by its mask pushed
      // to its upper half)
      auto over = [&](int layer, float raw) {
        const float t = std::clamp(raw * 2 - 1, 0.0f, 1.0f);
        for (int i = 0; i < 6; ++i) k[i] += (kinds[layer][i] - k[i]) * t;
      };
      for (int c = 0; c < 4 && haveLow; ++c) over(1 + c, low.At(u, v, c));
      for (int c = 0; c < 4 && haveHigh; ++c) over(5 + c, high.At(u, v, c));
      uint8_t* cell = ground_.data() + (static_cast<size_t>(gz) * static_cast<size_t>(groundW_) + static_cast<size_t>(gx)) * 6;
      for (int i = 0; i < 6; ++i) cell[i] = static_cast<uint8_t>(std::clamp(k[i], 0.0f, 1.0f) * 255 + 0.5f);
      mudSum += k[1];
    }
  printf("render: unit dirt: the ground's kinds over %dx%d cells of %.1f units (masks %dx%d%s), mud on %.1f%% of it, in %.1f ms\n", groundW_,
         groundH_, groundCell_, low.w, low.h, haveHigh ? " and the second" : "", 100 * mudSum / static_cast<double>(groundW_) / static_cast<double>(groundH_),
         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
}

void UnitWear::GroundAt(float x, float z, float out[6]) const {
  if (ground_.empty()) {
    const float dirtOnly[6] = {0, 0, 0, 0, 1, 0};
    std::copy(dirtOnly, dirtOnly + 6, out);
    return;
  }
  const float gx = std::clamp(x / groundCell_, 0.0f, static_cast<float>(groundW_ - 1)), gz = std::clamp(z / groundCell_, 0.0f, static_cast<float>(groundH_ - 1));
  const int x0 = std::min(static_cast<int>(gx), std::max(groundW_ - 2, 0)), z0 = std::min(static_cast<int>(gz), std::max(groundH_ - 2, 0));
  const int x1 = std::min(x0 + 1, groundW_ - 1), z1 = std::min(z0 + 1, groundH_ - 1);
  const float fx = gx - static_cast<float>(x0), fz = gz - static_cast<float>(z0);
  auto at = [&](int i, int k, int c) { return static_cast<float>(ground_[(static_cast<size_t>(k) * static_cast<size_t>(groundW_) + static_cast<size_t>(i)) * 6 + c]) / 255.0f; };
  for (int c = 0; c < 6; ++c) out[c] = (at(x0, z0, c) * (1 - fx) + at(x1, z0, c) * fx) * (1 - fz) + (at(x0, z1, c) * (1 - fx) + at(x1, z1, c) * fx) * fz;
}

// (unit_wear.h says what it does)
const char kWearShaderBlock[] = R"hlsl(
float3 WearTemper(float t) {  // the colours heat leaves on steel, from the hottest (0) outward: grey-blue, blue, purple, bronze, straw
  float3 c = lerp(float3(0.30, 0.36, 0.48), float3(0.10, 0.16, 0.45), saturate(t / 0.3));
  c = lerp(c, float3(0.30, 0.13, 0.36), saturate((t - 0.3) / 0.2));
  c = lerp(c, float3(0.50, 0.28, 0.12), saturate((t - 0.5) / 0.2));
  return lerp(c, float3(0.70, 0.56, 0.30), saturate((t - 0.7) / 0.25));
}
// Small round pits, one in each cell of a grid laid over the model (a third of the cells without): x 1 inside a pit,
// y its rim. `p` in cells, `w` a pixel in cells (their edges as sharp as the picture allows).
float2 WearPits(float3 p, float seed, float w) {
  float3 cell = floor(p);
  float h = CloudHash(cell + seed * 13.1);
  float3 at = cell + 0.3 + 0.4 * float3(frac(h * 13.7), CloudHash(cell + 7.3 + seed), CloudHash(cell + 3.1 - seed));
  float size = 0.07 + 0.11 * frac(h * 7.7);
  float d = length(p - at);
  float there = step(0.33, h);
  float pit = (1 - smoothstep(size - w, size + w, d)) * there;
  float rim = (1 - smoothstep(size * 1.7 - w, size * 1.7 + w, d)) * there - pit;
  return float2(pit, rim);
}
void WearSurface(StructuredBuffer<float4> rows, float4 wear, float3 p, float3 n, float3 world, float teamMask, float glowDim,
                 inout float3 base, inout float metallic, inout float roughness, inout float3 emission, inout float3 N) {
  const float3 bright = float3(0.36, 0.37, 0.38);  // steel scraped bright just now (thin: a torn lip, a gouge), as metal's colour
  const float3 steel = float3(0.13, 0.13, 0.135);  // steel where the paint is gone, dulled
  const float3 burnt = float3(0.03, 0.026, 0.023); // burnt black
  float pixel = max(length(fwidth(p)), 1e-4);      // world units a pixel spans here (what is smaller is not drawn)
  float keep = 1 - 0.45 * teamMask;                // (team colour must still read: soot and wear take less of it)
  float grey = dot(base, float3(0.3, 0.6, 0.1));
  // fresh paint over a mended place: its own colour, a little lighter and less faded
  float3 paint = lerp(float3(grey, grey, grey), base, 0.75) * 1.3 + 0.02;
  float height = 0;  // the surface raised (world units; below 0: pressed in): dents, holes, plates
  // the surface's own ways here: down it (where melt runs and soot rises against) and across
  float3 down = float3(0, -1, 0) + n * n.y;
  down = dot(down, down) > 1e-4 ? normalize(down) : float3(1, 0, 0);
  float3 side = cross(n, down);
  float upright = saturate(1.25 - abs(n.y) * 1.6);  // (1 a wall, 0 a roof)
  // its wear over all: grime in patches, and small crisp chips of paint gone to the metal
  float g = wear.z;
  [branch] if (g > 0.001) {
    float patches = CloudNoise(p * 1.1 + 17.3);  // (one noise each for the patches and the chips: the costliest part)
    float grime = saturate((patches - 0.45) * 2.2) * saturate(g * 0.35) * keep * (grimeTaken ? 0 : 1);  // [grime] (its own grime instead)
    base *= 1 - 0.3 * grime;
    roughness = lerp(roughness, 0.86, 0.5 * grime);
    metallic *= 1 - 0.35 * grime;
    float fine = CloudNoise(p * 30.0 + 3.1);
    float cut = 0.86 - 0.03 * saturate(g - 1) - 0.06 * patches;
    float fw = pixel * 45 + 0.002;  // (a pixel, in the noise's numbers)
    float chips = smoothstep(cut - fw, cut + fw, fine) * saturate(g) * keep * saturate(2 - pixel * 30);
    base = lerp(base, steel, chips);
    metallic = lerp(metallic, 0.85, chips);
    roughness = lerp(roughness, 0.55, chips);
  }
  uint first = (uint)(wear.x + 0.5), count = (uint)wear.y;  // ([unit dirt], [grime]: its quarters say what rows follow the marks)
  // which marks lie near enough here (none reaches farther than 2.2 radii): four at a time, their rows read together
  // (read one after another, the waits for them were most of what the marks cost)
  uint near = 0;
  for (uint g4 = 0; g4 < count; g4 += 4) {
    uint last = count - 1;
    float4 m0 = rows[first + min(g4, last) * 3], m1 = rows[first + min(g4 + 1, last) * 3];
    float4 m2 = rows[first + min(g4 + 2, last) * 3], m3 = rows[first + min(g4 + 3, last) * 3];
    float3 d0 = p - m0.xyz, d1 = p - m1.xyz, d2 = p - m2.xyz, d3 = p - m3.xyz;
    float4 far2 = float4(dot(d0, d0), dot(d1, d1), dot(d2, d2), dot(d3, d3));
    float4 reach = float4(m0.w, m1.w, m2.w, m3.w);
    reach *= reach * 4.84;
    near |= ((far2.x <= reach.x ? 1u : 0u) | (far2.y <= reach.y ? 2u : 0u) | (far2.z <= reach.z ? 4u : 0u) | (far2.w <= reach.w ? 8u : 0u)) << g4;
  }
  near &= (1u << count) - 1;  // (the reads past the last were of the last again)
  while (near != 0) {
    uint k = firstbitlow(near);
    near &= near - 1;
    float4 a = rows[first + k * 3], b = rows[first + k * 3 + 1], c = rows[first + k * 3 + 2];
    float r = a.w;
    float3 d = p - a.xyz;
    int kind = (int)(b.w + 0.5);
    // how far from where it struck, along the surface here (a round mark on the surface whichever way the shot came),
    // and how far off the surface's plane that place lies (another surface, behind or in front of it, takes none)
    float off = dot(d, n);
    float q = length(d - n * off) / r;
    if (q > (kind == 3 ? 2.2 : 1.5) || abs(off) > r * 0.8) continue;
    float facing = saturate(dot(n, -b.xyz) * 2 + 0.5);  // (what faced away from the shot takes none of it)
    if (facing <= 0) continue;
    float fresh = c.x, mended = 1 - c.x, seed = c.z;
    float hit = c.w * facing * (1 - smoothstep(r * 0.5, r * 0.8, abs(off)));
    float seen = saturate(r / pixel * 0.25 - 0.5);  // (a mark of a few pixels: no fine detail)
    float aw = max(pixel / r, 0.012);               // a pixel, in shares of its radius
    float aw2 = aw * 1.8;                           // (the same across a ragged outline)
    float rag = CloudNoise(p * (3.0 / r) + seed * 31.7) - 0.5;
    float qr = q + rag * 0.3;
    // across and along the way the shot went over the surface; across and down the surface
    float3 across = cross(b.xyz, n);
    across = dot(across, across) > 1e-6 ? normalize(across) : side;
    float3 ahead = cross(n, across);
    float sx = dot(d, across) / r, sy = dot(d, ahead) / r;
    float ux = dot(d, side) / r, uy = dot(d, down) / r;
    float graze = saturate(1 - abs(dot(n, b.xyz)) * 1.3);  // (1: the shot skimmed the surface)
    float patchAt = 0.42;  // where a mended patch ends, in its own measure (below)
    float patchQ = qr;
    if (kind == 0) {
      // kinetic: a crater, dark, its torn lip a thin ring of bright metal, flakes of paint gone round it; a gouge
      // where it skimmed
      float crater = 1 - smoothstep(0.24 - aw2, 0.24 + aw2, qr);
      float lip = (1 - smoothstep(0.31 - aw2, 0.31 + aw2, qr)) - crater;
      float fv = CloudNoise(p * (9 / r) + seed * 5) - (0.3 + 0.75 * saturate((qr - 0.3) / 0.35));
      float flakes = smoothstep(-aw * 12, aw * 12, fv) * (1 - smoothstep(0.6, 0.7, qr)) * (1 - crater - lip) * seen;
      float len = 0.6 + 0.9 * frac(seed * 7.3);
      float wide = 0.08 * (1 - saturate(sy / len)) + 0.025;
      float gouge = (1 - smoothstep(wide - aw, wide + aw, abs(sx + 0.12 * sy * (frac(seed * 3.1) - 0.5)))) * smoothstep(0.15, 0.25, sy) *
                    (1 - smoothstep(len - aw, len + aw, sy)) * step(0.4, frac(seed * 11.3)) * saturate(graze * 1.5) * seen;
      float bowl = saturate(1 - q / 0.4);
      height -= bowl * bowl * (3 - 2 * bowl) * r * 0.22 * hit * lerp(1, 0.35, mended) * saturate(seen * 2);
      height -= gouge * r * 0.03 * hit;
      base = lerp(base, steel, flakes * hit * fresh);
      metallic = lerp(metallic, 0.85, flakes * hit * fresh);
      roughness = lerp(roughness, 0.55, flakes * hit * fresh);
      float scrape = saturate(lip + gouge) * hit * fresh;
      base = lerp(base, bright, scrape);
      metallic = lerp(metallic, 1, scrape);
      roughness = lerp(roughness, 0.38, scrape);
      float pit = crater * hit * fresh;
      base = lerp(base, float3(0.03, 0.029, 0.028), pit * 0.95);
      metallic = lerp(metallic, 0.5, pit);
      roughness = lerp(roughness, 0.75, pit);
    } else if (kind == 1) {
      // energy: a pool of melt stretched along the way, its lip set bright, a thin band of dulled heat colours, the
      // paint round it burnt brown-black; on a wall, runs of melt down from it
      float qe = length(float2(sx, sy * 0.62)) + rag * 0.22;
      float pool = 1 - smoothstep(0.24 - aw2, 0.24 + aw2, qe);
      float lip = (1 - smoothstep(0.29 - aw2, 0.29 + aw2, qe)) - pool;
      float band = (1 - smoothstep(0.35 - aw2, 0.35 + aw2, qe)) - pool - lip;
      float scorch = (1 - smoothstep(0.34, 0.62, qe + rag * 0.22)) * saturate(1 - pool - lip);
      float col = ux * 4 + seed * 17;
      float runs = (1 - smoothstep(0.13 - aw * 4, 0.13 + aw * 4, abs(frac(col) - 0.5))) * step(abs(ux), 0.24) *
                   smoothstep(0.1, 0.18, uy) * (1 - smoothstep(0.0, aw, uy - 0.3 - 0.7 * CloudHash(float3(floor(col), seed * 9, 1.7)))) * upright * seen;
      float slag = saturate(pool + runs) * hit;
      base = lerp(base, burnt * float3(1.3, 1.0, 0.8), scorch * hit * 0.75 * lerp(1, 0.3, mended) * keep);
      roughness = lerp(roughness, 0.82, scorch * hit);
      metallic *= 1 - 0.6 * scorch * hit;
      float3 temper = lerp(WearTemper(saturate((qe - 0.29) / 0.06) * 0.8), float3(0.12, 0.1, 0.08), 0.55);
      base = lerp(base, temper, band * hit * fresh * 0.7);
      metallic = lerp(metallic, 0.9, band * hit * fresh);
      roughness = lerp(roughness, 0.4, band * hit * fresh);
      base = lerp(base, bright, lip * hit * fresh);
      metallic = lerp(metallic, 1, lip * hit * fresh);
      roughness = lerp(roughness, 0.3, lip * hit * fresh);
      base = lerp(base, float3(0.04, 0.035, 0.03), slag * lerp(1, 0.2, mended));
      metallic = lerp(metallic, 0.85, slag * fresh);
      roughness = lerp(roughness, 0.18, slag * fresh);
      height -= pool * r * 0.06 * hit;
      height += runs * r * 0.02 * hit;
      // glowing where it is hot (c.y: 1 just hit, cooling to 0)
      float hot = saturate(pool + lip * 0.6 + runs * 0.5) * c.y * c.y * facing;
      emission += lerp(float3(0.9, 0.12, 0.02), float3(1.0, 0.75, 0.45), c.y) * (hot * 2.5 * glowDim);
      patchQ = qe;
      patchAt = 0.45;
    } else if (kind == 2) {
      // explosive: a hole torn open, its torn edge dull metal, the metal round it burnt black, soot thrown out in
      // rays, shrapnel's pits round it
      float2 ray = float2(sx, sy) / max(length(float2(sx, sy)), 1e-4);
      float spikes = CloudNoise(float3(ray * 2.6, seed * 13)) * 0.6 + CloudNoise(float3(ray * 6.1, seed * 7 + 3)) * 0.4;
      float reach = 0.42 + 0.6 * spikes * spikes;
      float hole = 1 - smoothstep(0.17 - aw2, 0.17 + aw2, qr);
      float edge = (1 - smoothstep(0.22 - aw2, 0.22 + aw2, qr + (spikes - 0.5) * 0.12)) - hole;
      float core = 1 - smoothstep(0.38 - aw2, 0.38 + aw2, qr);
      float soot = (1 - smoothstep(reach * 0.8, reach, q)) * (0.5 + 0.5 * saturate(1 - q / reach)) * hit * keep;
      float2 pits = WearPits(p * (10 / r), seed, pixel * 10 / r) * step(0.32, q) * (1 - smoothstep(0.9, 1.1, q)) * seen * hit * fresh;
      base *= 1 - 0.7 * soot * lerp(1, 0.3, mended);
      roughness = lerp(roughness, 0.9, soot);
      metallic *= 1 - 0.7 * soot;
      base = lerp(base, burnt, core * hit * fresh * 0.92);
      roughness = lerp(roughness, 0.92, core * hit * fresh);
      base = lerp(base, float3(0.006, 0.005, 0.005), hole * hit * fresh);
      base = lerp(base, steel * 1.6, saturate(edge + pits.y * 0.7) * hit * fresh);
      metallic = lerp(metallic, 0.9, saturate(edge + pits.y) * hit * fresh);
      roughness = lerp(roughness, 0.45, saturate(edge + pits.y) * hit * fresh);
      base = lerp(base, float3(0.02, 0.018, 0.016), pits.x);
      height -= (1 - smoothstep(0.0, 0.32, q)) * r * 0.16 * hit * fresh * saturate(seen * 2);
      height -= pits.x * r * 0.02;
      // (a blast's embers, for a moment)
      emission += float3(1.0, 0.3, 0.05) * (saturate(hole + edge * 0.5 + core * 0.2) * c.y * c.y * facing * 1.5 * glowDim);
      // mended: a square plate welded over it, along the way it came
      patchQ = max(abs(sx), abs(sy)) * 0.9;
      patchAt = 0.42;
    } else {
      // fire: soot rising from where it burnt (up a wall; round it on a roof), the paint blistered
      float rise = -uy;
      float spread = 0.4 + 0.4 * saturate(rise);
      float plume = lerp(q, length(float2(ux / spread, rise > 0 ? rise * 0.5 : rise * 1.8)), upright);
      float edgeOf = plume + rag * 0.45;
      float soot = (1 - smoothstep(0.45, 0.85, edgeOf)) * (0.65 + 0.35 * CloudNoise(p * (3 / r) + 2.9)) * hit * keep;
      // (round the soot the paint browned by the heat: what shows on dark paint)
      float browned = saturate((1 - smoothstep(0.75, 1.15, edgeOf)) - soot) * hit * keep * lerp(1, 0.3, mended);
      base = lerp(base, float3(0.12, 0.07, 0.04), browned * 0.45);
      roughness = lerp(roughness, 0.8, browned * 0.5);
      float2 blisters = WearPits(p * (12 / r), seed + 0.5, pixel * 12 / r) * (1 - smoothstep(0.3, 0.55, q)) * seen * hit * fresh;
      base *= 1 - 0.9 * soot * lerp(1, 0.25, mended);
      roughness = lerp(roughness, 0.92, soot);
      metallic *= 1 - 0.7 * soot;
      base = lerp(base, burnt * 2, blisters.x);
      base *= 1 - 0.3 * blisters.y;
      height += blisters.x * r * 0.012;
      patchAt = 0;  // (soot is washed off, not patched)
    }
    // mended: fresh paint over what was hit, matt, lighter than the old; round it a weld's bead (a plate's, over a
    // blast's hole), raised, dull metal, with a dark line just outside it
    float patch = (1 - smoothstep(patchAt - aw, patchAt + aw, patchQ)) * mended * hit;
    float bead = saturate(1 - abs(patchQ - patchAt) / max(aw * 1.5, 0.035)) * mended * hit * seen * step(0.01, patchAt);
    float outline = saturate(1 - abs(patchQ - patchAt - 0.07) / max(aw * 1.2, 0.02)) * mended * hit * seen * step(0.01, patchAt);
    base = lerp(base, paint, 0.85 * patch);
    roughness = lerp(roughness, 0.62, patch);
    metallic *= 1 - 0.8 * patch;
    base = lerp(base, steel * 1.6, 0.8 * bead);
    metallic = lerp(metallic, 0.8, bead);
    roughness = lerp(roughness, 0.7, bead);
    base *= 1 - 0.45 * outline;
    height += patch * r * (kind == 2 ? 0.03 : 0.0) + bead * r * 0.02;
  }
  // the dents tilt the surface: the slope of their depth across the screen turned into a tilt of its way (the
  // surface gradient of Mikkelsen's "Bump Mapping Unparametrized Surfaces on the GPU", 2010)
  float3 dpdx = ddx(world), dpdy = ddy(world);
  float hx = ddx(height), hy = ddy(height);
  float3 r1 = cross(dpdy, N), r2 = cross(N, dpdx);
  float det = dot(dpdx, r1);
  float3 tilt = (det < 0 ? -1 : 1) * (hx * r1 + hy * r2);
  float3 bent = abs(det) * N - tilt;
  if (dot(bent, bent) > 1e-12) N = normalize(bent);
}
// [unit dirt] A unit's own wetness, mud and dust (unit_wear.h: WearLook::dirtRows, three rows from `at`) on its surface,
// before it is lit. `p`, `n`: the place on the model at rest and its surface's way there (world units, its own frame);
// `world`: the place in the world; `teamMask`: how much of the surface is team colour. Its own wetness stands in for
// the weather's (surfaceWetShare 0, lighting.h).
void DirtSurface(StructuredBuffer<float4> rows, uint at, float3 p, float3 n, float3 world, float teamMask, inout float3 base, inout float metallic,
                 inout float roughness, inout float3 N) {
  surfaceWetShare = 0;
  float4 d0 = rows[at], d1 = rows[at + 1], d2 = rows[at + 2];
  float wet = d0.x, mud = d0.y, mudWet = d0.z, dust = d0.w;
  if (wet + mud + dust < 0.002) return;
  float pixel = max(length(fwidth(p)), 1e-4);  // world units a pixel spans here
  float h = p.y - d1.x;                        // its height over the unit's lowest point (world units)
  float tall = max(d1.y, 0.05);
  float front = saturate((p.z - d1.z) * d1.w);  // 0 its middle and back .. 1 its front
  float upward = saturate(n.y);                 // (1 a roof)
  float upright = saturate(1.25 - abs(n.y) * 1.6);  // (1 a wall)
  float seed = d2.w * 61.7;
  float keep = 1 - 0.4 * teamMask;              // (team colour must still read: dust takes less of it)
  float height = 0;                             // the mud's lumps (world units)
  // dust: a pale coat, in patches, more on what faces up and low down; darker while wet
  [branch] if (dust > 0.002) {
    float patches = CloudNoise(p * 2.7 + seed);
    float lie = dust * saturate(0.3 + 0.7 * upward + 0.6 * saturate(1 - h / (tall * 0.6))) * (0.5 + 0.5 * patches) * keep;
    float3 colour = lerp(float3(0.15, 0.125, 0.095), float3(0.28, 0.24, 0.17), d2.z);  // (dry earth's, dry sand's: SETONS_R's soil and sand_dry, paler)
    colour *= 1 - 0.5 * saturate(wet * 1.5);
    base = lerp(base, colour, saturate(lie * 0.45));
    roughness = lerp(roughness, 0.88, saturate(lie * 0.9) * (1 - 0.7 * wet));
    metallic *= 1 - 0.8 * saturate(lie);
  }
  // mud: on the running gear and the lower hull up to the height it reaches (higher at the front by its speed), in
  // splashes with a ragged edge, spatter over it, less on what faces up (it is thrown from below); fresh dark and
  // glossy, drying from its thin edges and spatter down to the thick crust low down, lighter and matt, cracked, flaking
  float reach = d2.x + d2.y * front;
  [branch] if (mud > 0.002 && h < reach * 2.0 + 0.05) {
    // (its patterns by its own size: some half a dozen splashes up the height the mud reaches)
    float s = 1 / max(d2.x, 0.04);
    float big = CloudNoise(p * (1.7 * s) + seed);
    float fine = CloudNoise(p * (6.0 * s) + seed * 1.7);
    // its edge: ragged, thrown higher up the walls in streaks (a noise stretched up and down)
    float streak = CloudNoise(float3(p.x * (9 * s), p.y * (2.2 * s), p.z * (9 * s)) + seed * 2.9);
    float edge = reach * (0.6 + 0.55 * big + (streak - 0.5) * 0.9 * upright) + (fine - 0.5) * 0.25 * d2.x;
    float below = edge - h;                                    // how far under its edge this place lies
    float aw = pixel * 1.2;
    float gear = saturate(1.3 - h / max(d2.x * 0.4, 0.02));    // (the running gear, low down: coated whichever way it faces)
    float thick = saturate(below / max(reach * 0.5, 0.02));    // 0 at its edge .. 1 deep in the crust
    float solid = saturate(thick * 3 + fine * 0.8 - 0.3);      // (solid but toward its edge, in patches there)
    float band = smoothstep(-aw, aw, below) * lerp(lerp(1, 0.25, upward) * solid, 1, gear) * saturate(mud * 5);
    // over its edge, fewer the higher: blots in cells of a third of the height it reaches, and spatter in cells of some
    // five hundredths of a world unit (not where a drop is under a pixel); less on what faces up
    float over = saturate(1 + below / max(reach * 0.7, 0.05)) * saturate(mud * 1.6) * lerp(0.35, 1, 1 - upward);
    float3 bq = p * (3.2 * s);
    float3 bcell = floor(bq);
    float bh = CloudHash(bcell + seed * 0.71);
    float3 bc = bcell + 0.25 + 0.5 * float3(frac(bh * 7.3), frac(bh * 13.1), frac(bh * 3.7));
    float br = 0.12 + 0.2 * frac(bh * 5.1);
    float bw = pixel * 3.2 * s;
    float blot = (1 - smoothstep(br - bw, br + bw, length((bq - bc) * float3(1, 0.6, 1)) + (fine - 0.5) * 0.5)) * step(bh, over * 0.6);  // (thrown up: long)
    float3 q = p * 21.0;
    float3 cell = floor(q);
    float hc = CloudHash(cell + seed * 0.37);
    float3 centre = cell + 0.3 + 0.4 * float3(frac(hc * 7.3), frac(hc * 13.1), frac(hc * 3.7));
    float radius = 0.16 + 0.2 * frac(hc * 5.1);
    float drop = (1 - smoothstep(radius - pixel * 21, radius + pixel * 21, length(q - centre))) * step(hc, over * 0.55) * saturate(4 - pixel * 70);
    float thrown = saturate(blot + drop) * (1 - band);
    float muddy = saturate(band + thrown);
    muddy *= grimeSplash < 0 ? 1 : saturate(0.3 + grimeSplash * 1.5);  // [grime] (where the unit's masks say weather dirt sits)
    [branch] if (muddy > 0.001) {
      // how wet it is here: what was thrown over its edge and its thin edge dry first, the thick crust last
      float damp = saturate(mudWet * 1.7 - (1 - thick) * 0.7 - thrown * 0.35);
      float dryness = 1 - damp;
      // dry: it cracks, and flakes off where it was thin (more of it the less mud is left)
      float cracks = 0;
      [branch] if (dryness > 0.3 && pixel < 0.02) {
        float c = CloudNoise(p * 17.0 + seed * 2.3);
        cracks = (1 - smoothstep(0.015, 0.045 + pixel * 2, abs(c - 0.5))) * saturate((dryness - 0.3) * 2) * band;
        muddy *= 1 - step(c, 0.1 + 0.35 * (1 - mud)) * saturate((dryness - 0.6) * 3) * (1 - thick * 0.7);
      }
      float3 wetMud = float3(0.085, 0.057, 0.031);  // (the map's mud material's colour: setonsr_mud's albedo, its lighter tenth, browner)
      float3 dryMud = float3(0.15, 0.115, 0.075);   // (dried: a pale crust, twice as light as SETONS_R's soil and greyer)
      float3 colour = lerp(dryMud, wetMud, damp) * (0.75 + 0.35 * big + 0.25 * fine) * (1 - 0.6 * cracks);
      base = lerp(base, colour, muddy);
      metallic *= 1 - muddy;
      roughness = lerp(roughness, lerp(0.92, lerp(0.55, 0.38, thick), damp), muddy);
      height += (muddy * (0.002 + 0.016 * thick * (0.3 + big)) - cracks * 0.002) * min(d2.x * 4, 2);
    }
  }
  // wet: the paint a little darker while damp; a film of water over it (glossy): all over in rain, drawing back as it
  // dries into the streaks down its walls and the pools on its tops, the last of it low down; drops running down
  [branch] if (wet > 0.002) {
    base *= 1 - 0.18 * saturate(wet * 1.5);
    float2 along = float2(-n.z, n.x);
    float across = dot(p.xz, along / max(length(along), 1e-3));  // (along the surface, level: across its streaks)
    float film = 1;
    [branch] if (wet < 0.97) {
      float streaks = CloudNoise(float3(across * 9.0, p.y * 1.3, seed));
      float pools = CloudNoise(p * 3.1 + seed + 5.3);
      float holds = lerp(pools, streaks, upright) * 0.75 + saturate(1 - h / tall) * 0.25;  // how long it stays wet here
      film = saturate((wet * 1.35 - (1 - holds)) * 3);
    }
    roughness = lerp(roughness, 0.1, film * 0.8 * (1 - 0.75 * metallic));  // (bare metal under water stays as it was, nearly)
    base *= 1 - 0.06 * film;
    // drops running down its walls, in columns some seven hundredths of a world unit apart (near the eye only): after
    // the rain and, faster, in it
    float drips = saturate(wet * 1.6 - 0.2);
    [branch] if (drips > 0.01 && upright > 0.3 && pixel < 0.01) {
      float col = across * 14 + seed;
      float hc = CloudHash(float3(floor(col), 3.7, seed));
      [branch] if (hc < drips * 0.45) {
        float dx = (frac(col) - 0.5) / 14;                  // across the column (world units)
        float period = 0.25 + 0.35 * frac(hc * 3.3);        // (one drop a column in that much of its height)
        float speed = (0.12 + 0.2 * frac(hc * 9.1)) * (1 + 2 * falling.x);
        float u = frac((p.y + time * speed) / period + hc * 7) * period;  // above the drop's foot along its column
        float dropAt = length(float2(dx, (u - 0.009) * 0.8));
        float bead = (1 - smoothstep(0.007, 0.007 + pixel, dropAt)) * upright;
        float trail = (1 - smoothstep(0.0015, 0.0015 + pixel, abs(dx))) * step(0.009, u) * saturate(1 - u / 0.12) * upright;
        roughness = lerp(roughness, 0.04, saturate(bead + trail * 0.8));
        base *= 1 - 0.15 * trail;
        height += bead * 0.004 * sqrt(saturate(1 - dropAt / 0.009));
      }
    }
  }
  // the mud's lumps and the drops tilt the surface (as WearSurface's dents)
  float3 dpdx = ddx(world), dpdy = ddy(world);
  float hx = ddx(height), hy = ddy(height);
  if (hx != 0 || hy != 0) {
    float3 r1 = cross(dpdy, N), r2 = cross(N, dpdx);
    float det = dot(dpdx, r1);
    float3 tilt = (det < 0 ? -1 : 1) * (hx * r1 + hy * r2);
    float3 bent = abs(det) * N - tilt;
    if (dot(bent, bent) > 1e-12) N = normalize(bent);
  }
}
)hlsl";

}  // namespace render
