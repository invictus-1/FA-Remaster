#include "debris.h"

#include <algorithm>
#include <cmath>

#include "blasts.h"
#include "mesh_renderer.h"

namespace render {
namespace {

constexpr float kGravity = 4.9f;  // the simulation's, world units a second squared (as fx_entities.cpp has it)

Quat Turned(const Quat& q, Vec3 spin, float dt) {  // q turned on by `spin` (radians a second, world axes) for dt
  const Quat w{spin.x, spin.y, spin.z, 0};
  const Quat d = Mul(w, q);
  Quat r{q.x + 0.5f * dt * d.x, q.y + 0.5f * dt * d.y, q.z + 0.5f * dt * d.z, q.w + 0.5f * dt * d.w};
  const float l = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
  return l > 0 ? Quat{r.x / l, r.y / l, r.z / l, r.w / l} : Quat{};
}

}  // namespace

Debris::~Debris() {
  for (Shape& s : shapes_) {
    if (s.vertices) device_->Destroy(s.vertices);
    if (s.indices) device_->Destroy(s.indices);
  }
}

float Debris::Random() {
  seed_ ^= seed_ << 13;
  seed_ ^= seed_ >> 17;
  seed_ ^= seed_ << 5;
  return static_cast<float>(seed_ >> 8) * (1.0f / 16777216.0f);
}

void Debris::Clear() {
  flying_.clear();
  lying_.clear();
  dying_.clear();
  clock_ = -1;
}

// The six shapes, each within a cube of one world unit at its middle (a chunk is drawn at its size): two blocks, two
// flat pieces, two wedges, their corners pushed about a little; every face flat, wearing its own patch of the
// texture (each shape a patch of its own, about a tenth of the texture across).
void Debris::MakeShapes() {
  shapesMade_ = true;
  uint32_t s = 0x2545f491u;
  auto rand = [&s]() {
    s = s * 1664525u + 1013904223u;
    return static_cast<float>(s >> 8) / 16777216.0f;
  };
  static const float kPatch[kShapes][2] = {{0.30f, 0.32f}, {0.68f, 0.30f}, {0.50f, 0.55f}, {0.28f, 0.74f}, {0.72f, 0.72f}, {0.50f, 0.18f}};
  for (int k = 0; k < kShapes; ++k) {
    std::vector<Vec3> corners;
    std::vector<std::vector<int>> faces;  // (each corner list round a face)
    const float j = 0.09f;
    auto jitter = [&](Vec3 p) { return p + Vec3{(rand() - 0.5f) * 2 * j, (rand() - 0.5f) * 2 * j, (rand() - 0.5f) * 2 * j}; };
    if (k < 4) {  // a block (0, 1) or a flat piece (2, 3)
      const Vec3 half = k < 2 ? Vec3{0.45f, 0.32f + 0.08f * static_cast<float>(k), 0.38f} : Vec3{0.48f, 0.11f + 0.03f * static_cast<float>(k - 2), 0.40f};
      for (int c = 0; c < 8; ++c) corners.push_back(jitter({(c & 1) ? half.x : -half.x, (c & 2) ? half.y : -half.y, (c & 4) ? half.z : -half.z}));
      faces = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    } else {  // a wedge: a triangle drawn out along z
      const float zh = k == 4 ? 0.42f : 0.30f;
      const Vec3 tri[3] = {{-0.45f, -0.30f, 0}, {0.45f, -0.30f, 0}, {k == 4 ? 0.1f : -0.25f, 0.38f, 0}};
      for (int e = 0; e < 2; ++e)
        for (int c = 0; c < 3; ++c) corners.push_back(jitter(tri[c] + Vec3{0, 0, e ? zh : -zh}));
      faces = {{0, 2, 1}, {3, 4, 5}, {0, 1, 4, 3}, {1, 2, 5, 4}, {2, 0, 3, 5}};
    }
    Vec3 middle{};
    for (const Vec3& c : corners) middle += c;
    middle = middle * (1.0f / static_cast<float>(corners.size()));
    std::vector<ScmVertex> vertices;
    std::vector<uint16_t> indices;
    for (const std::vector<int>& f : faces)
      for (size_t t = 1; t + 1 < f.size(); ++t) {
        Vec3 a = corners[static_cast<size_t>(f[0])], b = corners[static_cast<size_t>(f[t])], c = corners[static_cast<size_t>(f[t + 1])];
        Vec3 n = Normalize(Cross(b - a, c - a));
        if (Dot(n, (a + b + c) * (1.0f / 3) - middle) < 0) {  // (outward, its corners counter-clockwise seen from outside)
          std::swap(b, c);
          n = n * -1.0f;
        }
        const Vec3 along = Normalize(b - a), across = Cross(n, along);
        for (const Vec3& p : {a, b, c}) {
          ScmVertex v{};
          v.position[0] = p.x, v.position[1] = p.y, v.position[2] = p.z;
          v.normal[0] = n.x, v.normal[1] = n.y, v.normal[2] = n.z;
          v.tangent[0] = along.x, v.tangent[1] = along.y, v.tangent[2] = along.z;
          v.binormal[0] = across.x, v.binormal[1] = across.y, v.binormal[2] = across.z;
          v.uv0[0] = v.uv1[0] = kPatch[k][0] + 0.11f * Dot(p, along);
          v.uv0[1] = v.uv1[1] = kPatch[k][1] + 0.11f * Dot(p, across);
          indices.push_back(static_cast<uint16_t>(vertices.size()));
          vertices.push_back(v);
        }
      }
    Shape& shape = shapes_[k];
    gfx::BufferDesc vb;
    vb.stride = sizeof(ScmVertex);
    vb.size = static_cast<uint32_t>(vertices.size() * sizeof(ScmVertex));
    vb.data = vertices.data();
    shape.vertices = device_->CreateBuffer(vb);
    gfx::BufferDesc ib;
    ib.kind = gfx::BufferKind::Index;
    ib.stride = 2;
    ib.size = static_cast<uint32_t>(indices.size() * sizeof(uint16_t));
    ib.data = indices.data();
    shape.indices = device_->CreateBuffer(ib);
    shape.vertexCount = static_cast<uint32_t>(vertices.size());
    shape.indexCount = static_cast<uint32_t>(indices.size());
  }
}

const std::array<std::unique_ptr<Model>, 2 * Debris::kShapes>& Debris::Looks(const Model* unit) {
  auto known = looks_.find(unit);
  if (known != looks_.end()) return known->second;
  if (!shapesMade_) MakeShapes();
  std::array<std::unique_ptr<Model>, 2 * kShapes>& looks = looks_[unit];
  const ModelLod* from = unit && !unit->lods.empty() ? &unit->lods[0] : nullptr;
  for (int i = 0; i < 2 * kShapes; ++i) {
    const Shape& shape = shapes_[i % kShapes];
    auto m = std::make_unique<Model>();
    ModelLod lod;
    lod.serial = 0x40000000u + serial_++;  // (apart from the models' own numbers, which count up from 1)
    lod.vertices = shape.vertices;
    lod.indices = shape.indices;
    lod.indexCount = shape.indexCount;
    lod.vertexCount = shape.vertexCount;
    lod.indexBytes = 2;
    // the unit's own paint and material (without one: plain grey, the army's colour over it)
    lod.albedo = from && from->albedo ? from->albedo : textures_->White();
    lod.normals = from && from->normals ? from->normals : textures_->FlatNormal();
    lod.specular = from && from->specular ? from->specular : textures_->Black();
    lod.secondary = from && from->secondary ? from->secondary : textures_->White();
    if (from) {
      lod.labPbr = from->labPbr;
      lod.plainMaterial = from->plainMaterial;
      lod.teamMask = from->teamMask;
      lod.faction = from->faction;
      lod.shaderName = from->shaderName;
    } else {
      lod.teamMask = false;
      lod.shaderName = "Unit";
    }
    lod.wreck = i >= kShapes;  // (half of them burnt, as its wreck is)
    m->lods.push_back(lod);
    m->ok = shape.vertices && shape.indices;
    m->scale = 1;
    m->boundsMin = {-0.55f, -0.45f, -0.55f};
    m->boundsMax = {0.55f, 0.45f, 0.55f};
    looks[static_cast<size_t>(i)] = std::move(m);
  }
  return looks;
}

void Debris::Throw(const std::string& unitId, const Model* model, Vec3 position, const Quat& turn, Vec3 color, Vec3 velocity, uint32_t seed) {
  if (!model || !model->ok) return;
  seed_ ^= seed * 2654435761u + 0x6d2b79f5u;
  if (!seed_) seed_ = 1;
  const Vec3 lo = model->boundsMin * model->scale, hi = model->boundsMax * model->scale;
  const Vec3 extent = hi - lo;
  const float across = std::max({extent.x, extent.z, 0.3f});
  // its pieces: its own fragments where there are some, else the generic chunks, each from somewhere in its lower body,
  // turned any way
  std::vector<Piece> pieces;
  if (fragments) pieces = fragments(unitId, model);
  if (pieces.empty()) {
    const auto& looks = Looks(model);
    const int count = std::clamp(static_cast<int>(std::lround((4 + 3.5f * across) * amount)), amount > 0 ? 3 : 0, 28);
    for (int i = 0; i < count; ++i) {
      Piece p;
      p.rest = {lo.x + extent.x * (0.1f + 0.8f * Random()), lo.y + extent.y * (0.15f + 0.55f * Random()), lo.z + extent.z * (0.1f + 0.8f * Random())};
      const Vec3 tilt = Normalize(Vec3{Random() - 0.5f, Random() - 0.5f, Random() - 0.5f});
      const float half = Random() * 3.14159265f, s = std::sin(half);
      p.turn = Quat{tilt.x * s, tilt.y * s, tilt.z * s, std::cos(half)};
      p.size = p.scale = std::clamp(across * (0.06f + 0.09f * Random()), 0.035f, 0.5f);
      p.model = looks[static_cast<size_t>(static_cast<int>(Random() * 2 * kShapes) % (2 * kShapes))].get();
      pieces.push_back(p);
    }
  }
  // each flies from where it sat, out from the unit's middle and up; the bigger, the slower it turns
  const float push = std::clamp(std::sqrt(across), 0.6f, 2.5f), lift = std::clamp(std::sqrt(across), 0.7f, 2.2f);
  const Vec3 middle = position + Rotate(turn, (lo + hi) * 0.5f);
  for (const Piece& p : pieces) {
    if (!p.model || flying_.size() >= 800) continue;
    Chunk c;
    c.at = position + Rotate(turn, p.rest);
    c.turn = Mul(turn, p.turn);
    Vec3 out{c.at.x - middle.x, 0, c.at.z - middle.z};
    if (Length(out) < 1e-3f) {
      const float a = Random() * 6.2831853f;
      out = {std::cos(a), 0, std::sin(a)};
    }
    out = Normalize(out);
    c.velocity = out * ((0.8f + 1.8f * Random()) * push) + Vec3{0, (2.0f + 2.6f * Random()) * lift, 0} + velocity * 0.5f;
    c.spin = Normalize(Vec3{Random() - 0.5f, Random() - 0.5f, Random() - 0.5f}) * ((5 + 10 * Random()) * std::min(1.0f, 0.15f / std::max(p.size, 0.01f)));
    c.size = p.size;
    c.scale = p.scale;
    c.color = color * 0.6f;  // (its army's colour dulled, sooty: torn bits of paint, not bright specks that catch the eye)
    c.look = p.model;
    c.key = 0x40000000u | (nextKey_++ & 0x3fffffffu);
    flying_.push_back(c);
    ++thrown;
  }
}

void Debris::Watch(const Frame& frame, double clock, const std::function<const Model*(const std::string&)>& model,
                   const std::function<Vec3(uint32_t)>& color, const Blasts* flashes) {
  if (!frame.blueprints) return;
  ++stamp_;
  for (const FrameUnit& u : frame.units) {
    if (!u.dead) {
      dying_.erase(u.id);  // (an id the game gave again)
      continue;
    }
    if (!u.Seen() || u.blueprint >= frame.blueprints->size()) continue;
    const Vec3 at{u.position[0], u.position[1], u.position[2]};
    auto it = dying_.find(u.id);
    if (it == dying_.end()) {
      Dying d;
      d.blueprint = u.blueprint;
      d.army = u.Army();
      d.at = at;
      d.seen = d.died = clock;
      d.air = (u.layer & 0x10) != 0;
      // (none from what dies on or in the water: it goes down)
      d.thrown = (u.layer & (2 | 4 | 8)) != 0;
      it = dying_.emplace(u.id, d).first;
    }
    Dying& d = it->second;
    const double dt = clock - d.seen;
    if (dt > 1e-3) d.velocity = (at - d.at) * static_cast<float>(1 / dt);
    d.at = at;
    d.turn = Quat{u.orientation[0], u.orientation[1], u.orientation[2], u.orientation[3]};
    d.seen = clock;
    d.stamp = stamp_;
    // on the ground (a land unit, a building): with its death's explosion, the flash its script makes on it (seen up to
    // half a second before its death was: the picture's frame and the effects' beat need not be the same), or four
    // seconds on at the latest; falling from the air: when it is down
    const bool now = d.air ? at.y - Ground(at.x, at.z) < 1.2f : !flashes || flashes->FlashedOn(u.id, d.died - 0.5) || clock - d.died > 4;
    if (!d.thrown && now) {
      d.thrown = true;
      const std::string& id = (*frame.blueprints)[u.blueprint];
      Throw(id, model(id), at, d.turn, color(d.army), d.air ? d.velocity : Vec3{}, u.id);
    }
  }
  for (auto it = dying_.begin(); it != dying_.end();) {
    Dying& d = it->second;
    if (d.stamp == stamp_) {
      ++it;
      continue;
    }
    // gone before its chunks were thrown (an aircraft that met the ground between two frames): where it was last
    if (!d.thrown && clock - d.seen < 2 && d.at.y - Ground(d.at.x, d.at.z) < 4 && d.blueprint < frame.blueprints->size()) {
      d.thrown = true;
      const std::string& id = (*frame.blueprints)[d.blueprint];
      Throw(id, model(id), {d.at.x, Ground(d.at.x, d.at.z) + 0.2f, d.at.z}, d.turn, color(d.army), d.velocity, it->first);
    }
    // (kept a while: a frame may leave a unit out for a moment)
    if (clock - d.seen > 5 || clock < d.seen - 1) it = dying_.erase(it);
    else ++it;
  }
}

void Debris::Update(double clock, Vec3 eye, MeshRenderer* meshes, Blasts* splashes) {
  // (a game started over, or played back from earlier: what was thrown later is not there yet)
  if (clock_ >= 0 && clock < clock_ - 2) Clear();
  const float step = clock_ < 0 ? 0.0f : static_cast<float>(std::clamp(clock - clock_, 0.0, 0.1));
  clock_ = clock;
  // the flying chunks: falling, turning, bouncing off the ground (in steps of at most a sixtieth of a second)
  const int steps = std::max(1, static_cast<int>(std::ceil(step * 60)));
  const float dt = step / static_cast<float>(steps);
  for (size_t i = 0; i < flying_.size();) {
    Chunk& c = flying_[i];
    bool gone = false, rests = false;
    for (int s = 0; s < steps && dt > 0 && !gone && !rests; ++s) {
      c.velocity.y -= kGravity * dt;
      c.at += c.velocity * dt;
      c.turn = Turned(c.turn, c.spin, dt);
      if (c.at.x < 0 || c.at.z < 0 || (mapWidth > 0 && c.at.x > mapWidth) || (mapHeight > 0 && c.at.z > mapHeight)) {
        gone = true;
        break;
      }
      const float ground = Ground(c.at.x, c.at.z);
      if (ground < waterLevel && c.at.y < waterLevel) {  // into the water: gone, with a splash
        if (splashes) splashes->Add({c.at.x, waterLevel, c.at.z}, c.size, clock, true);
        gone = true;
        break;
      }
      const float rest = c.size * 0.3f;  // (its middle over the ground where it touches)
      if (c.at.y - rest > ground) continue;
      // a bounce: off the ground's slope, losing most of its speed into it and some along it
      const float e = 0.5f;
      const Vec3 n = Normalize(Vec3{Ground(c.at.x - e, c.at.z) - Ground(c.at.x + e, c.at.z), 2 * e, Ground(c.at.x, c.at.z - e) - Ground(c.at.x, c.at.z + e)});
      c.at.y = ground + rest;
      const float into = Dot(c.velocity, n);
      if (into < 0) {
        const Vec3 normal = n * into, along = c.velocity - normal;
        c.velocity = along * 0.55f - normal * (0.28f + 0.12f * Random());
        c.spin = c.spin * 0.45f + Normalize(Cross(n, along)) * (Length(along) * 3.0f / std::max(c.size, 0.05f) * 0.15f);
        ++c.bounces;
      }
      if (Length(c.velocity) < 0.45f || c.bounces >= 5) rests = true;
    }
    if (gone) {
      flying_[i] = flying_.back();
      flying_.pop_back();
      continue;
    }
    if (rests) {  // at rest: half sunk into the ground, as rubble lies
      c.velocity = c.spin = {};
      c.at.y = Ground(c.at.x, c.at.z) + c.size * 0.12f;
      c.settled = clock;
      c.world = Mat4::FromTRS(c.at, c.turn, c.scale);
      lying_.push_back(c);
      flying_[i] = flying_.back();
      flying_.pop_back();
      continue;
    }
    ++i;
  }
  // past the most kept, or after it has lain `rubbleLasts`, the oldest rubble sinks away over ten seconds
  int fading = 0;
  for (const Chunk& c : lying_) {
    if (c.fading <= 0) break;
    ++fading;
  }
  for (size_t k = static_cast<size_t>(fading); k < lying_.size(); ++k) {
    const bool old = rubbleLasts > 0 && clock - lying_[k].settled > rubbleLasts;
    if (!old && static_cast<int>(lying_.size()) - fading <= mostRubble) break;
    lying_[k].fading = clock;
    ++fading;
  }
  while (!lying_.empty() && lying_.front().fading > 0 && clock - lying_.front().fading > 10) lying_.pop_front();
  if (!meshes) return;
  // drawn as scenery (pressing no grass flat), those near enough to be more than a dot
  const bool before = meshes->addingScenery;
  meshes->addingScenery = true;
  for (const Chunk& c : flying_)
    if (Length(c.at - eye) < 260) meshes->Add(c.look, Mat4::FromTRS(c.at, c.turn, c.scale), c.color, 1, nullptr, nullptr, nullptr, c.key);
  for (const Chunk& c : lying_) {
    if (Length(c.at - eye) > 260) continue;
    if (c.fading > 0) {
      const float f = std::clamp(static_cast<float>(clock - c.fading) / 10, 0.0f, 1.0f);
      meshes->Add(c.look, Mat4::FromTRS(c.at - Vec3{0, c.size * 0.7f * f, 0}, c.turn, c.scale * (1 - 0.5f * f)), c.color, 1, nullptr, nullptr, nullptr, c.key);
    } else {
      meshes->Add(c.look, c.world, c.color, 1, nullptr, nullptr, nullptr, c.key);
    }
  }
  meshes->addingScenery = before;
}

}  // namespace render
