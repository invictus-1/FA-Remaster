#include "debris.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <optional>

#include "blasts.h"
#include "core/vfs.h"
#include "dds.h"
#include "hot_metal.h"
#include "mesh_renderer.h"
#include "particles.h"

namespace render {
namespace {

constexpr float kGravity = 4.9f;  // the simulation's, world units a second squared (as fx_entities.cpp has it)
// The game's debris plate (/meshes/projectiles/debris_misc_04, from the game's meshes.scd) is about this many of its
// model units across; its shells draw it at their blueprint's UniformScale (0.1, 0.23, 0.4).
constexpr float kShellAcross = 0.75f;
// The thin smoke the biggest chunks trail: the game's own trail on its debris shells (GenericDebrisTrails01).
constexpr const char* kTrail = "/effects/emitters/destruction_explosion_debris_trail_01_emit.bp";
// A piece's army colour against the unit's (a little dulled and sooty: torn bits of paint, not bright specks)
constexpr float kDulled = 0.85f;

Quat Turned(const Quat& q, Vec3 spin, float dt) {  // q turned on by `spin` (radians a second, world axes) for dt
  const Quat w{spin.x, spin.y, spin.z, 0};
  const Quat d = Mul(w, q);
  Quat r{q.x + 0.5f * dt * d.x, q.y + 0.5f * dt * d.y, q.z + 0.5f * dt * d.z, q.w + 0.5f * dt * d.w};
  const float l = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
  return l > 0 ? Quat{r.x / l, r.y / l, r.z / l, r.w / l} : Quat{};
}

// (the torn faces' textures) Smooth noise in 0..1 that repeats every `period` cells.
float Hash(int x, int y, uint32_t seed) {
  uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + seed * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return static_cast<float>((h ^ (h >> 16)) & 0xffffffu) / 16777216.0f;
}
float Noise(float x, float y, int period, uint32_t seed) {
  const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
  float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
  fx = fx * fx * (3 - 2 * fx);
  fy = fy * fy * (3 - 2 * fy);
  auto at = [&](int i, int j) { return Hash(((i % period) + period) % period, ((j % period) + period) % period, seed); };
  const float a = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * fx, b = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * fx;
  return a + (b - a) * fy;
}
// A square RGBA8 texture of `size` from f(x, y) -> its four bytes, with its smaller sizes made by averaging.
template <class F>
gfx::Texture Pattern(gfx::Device* device, int size, F f) {
  std::vector<uint8_t> all(static_cast<size_t>(size * size * 4));
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x) f(x, y, &all[static_cast<size_t>((y * size + x) * 4)]);
  int mips = 1;
  size_t level = 0;
  for (int s = size; s > 1; s /= 2, ++mips) {
    const int h = s / 2;
    const size_t next = all.size();
    all.resize(next + static_cast<size_t>(h * h * 4));
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < h; ++x)
        for (int c = 0; c < 4; ++c) {
          int sum = 0;
          for (int k = 0; k < 4; ++k) sum += all[level + static_cast<size_t>(((2 * y + (k >> 1)) * s + 2 * x + (k & 1)) * 4 + c)];
          all[next + static_cast<size_t>((y * h + x) * 4 + c)] = static_cast<uint8_t>((sum + 2) / 4);
        }
    level = next;
  }
  gfx::TextureDesc d;
  d.width = d.height = size;
  d.mips = mips;
  d.format = gfx::Format::RGBA8;
  d.data = all.data();
  d.dataSize = all.size();
  return device->CreateTexture(d);
}

uint8_t Byte(float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f); }

// (a texture file's level of that size, in bytes: the formats DecodeRgba reads; 0 others)
size_t LevelBytes(gfx::Format f, int w, int h) {
  const size_t blocks = static_cast<size_t>(std::max(1, (w + 3) / 4)) * static_cast<size_t>(std::max(1, (h + 3) / 4));
  if (f == gfx::Format::BC1) return blocks * 8;
  if (f == gfx::Format::BC2 || f == gfx::Format::BC3) return blocks * 16;
  if (f == gfx::Format::RGBA8 || f == gfx::Format::BGRA8) return static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
  return 0;
}

}  // namespace

Debris::~Debris() {
  {  // [groundfix] (the thread reading ahead where paint glows: stopped)
    std::lock_guard<std::mutex> lock(glowMutex_);
    glowStopping_ = true;
  }
  glowWake_.notify_all();
  if (glowThread_.joinable()) glowThread_.join();
  for (Shape& s : shape_) {
    if (s.skinIndices) device_->Destroy(s.skinIndices);
    if (s.charredVertices) device_->Destroy(s.charredVertices);
    if (s.tornVertices) device_->Destroy(s.tornVertices);
    if (s.tornIndices) device_->Destroy(s.tornIndices);
  }
  for (auto& [unit, looks] : looks_)
    for (gfx::Buffer b : looks.buffers)
      if (b) device_->Destroy(b);
  for (gfx::Texture t : {steel_, steelMaterial_, concrete_, concreteMaterial_, charred_, charredMaterial_})
    if (t) device_->Destroy(t);
}

float Debris::Random() {
  seed_ ^= seed_ << 13;
  seed_ ^= seed_ >> 17;
  seed_ ^= seed_ << 5;
  return static_cast<float>(seed_ >> 8) * (1.0f / 16777216.0f);
}

void Debris::Clear() {
  if (smoke)
    for (Chunk& c : flying_) StopTrail(&c);
  flying_.clear();
  lying_.clear();
  dying_.clear();
  shells_.clear();
  shellKinds_.clear();
  clock_ = -1;
}

// The library's shapes (debris_shapes.h), and what all units' pieces share: each shape's corner numbers, its charred
// skin and its torn faces in their own materials. Charred: sooty dark grey-brown with paler blotches of dust, rough,
// hardly metal (a burnt piece's whole skin, and a painted one's once dust has settled on it as rubble; one look for all
// units' pieces, so that each unit adds only its painted ones to what is drawn). Scorched steel: dark, sooty in blotches, metal 0.85 and roughness 0.42, which is what the
// hot metal's shader takes for a torn rim of bare steel (hot_metal.h; the visuals track's wreck kit paints its rims so),
// so it glows there while hot. A lump's fractures: broken concrete, grey, rough, no metal (they do not glow).
void Debris::MakeShapes() {
  shapesMade_ = true;
  shapes_ = MakeDebrisShapes();
  steel_ = Pattern(device_, 64, [](int x, int y, uint8_t* p) {
    const float n = 0.6f * Noise(x / 8.0f, y / 8.0f, 8, 1) + 0.4f * Noise(x / 3.0f, y / 3.0f, 21, 2);
    const float rust = std::max(Noise(x / 11.0f, y / 11.0f, 6, 3) - 0.6f, 0.0f) * 2.5f;
    p[0] = Byte(62 + 60 * n + 30 * rust), p[1] = Byte(57 + 54 * n + 8 * rust), p[2] = Byte(52 + 48 * n), p[3] = 255;
  });
  steelMaterial_ = Pattern(device_, 64, [](int x, int y, uint8_t* p) {
    const float n = Noise(x / 5.0f, y / 5.0f, 13, 4) - 0.5f;
    p[0] = Byte(217 + 18 * n), p[1] = Byte(107 + 16 * n), p[2] = 0, p[3] = 0;
  });
  concrete_ = Pattern(device_, 64, [](int x, int y, uint8_t* p) {
    const float n = 0.5f * Noise(x / 6.0f, y / 6.0f, 11, 5) + 0.5f * Noise(x / 2.0f, y / 2.0f, 32, 6);
    p[0] = Byte(100 + 50 * n), p[1] = Byte(97 + 49 * n), p[2] = Byte(92 + 46 * n), p[3] = 255;
  });
  concreteMaterial_ = Pattern(device_, 4, [](int, int, uint8_t* p) { p[0] = 0, p[1] = 232, p[2] = 0, p[3] = 0; });
  charred_ = Pattern(device_, 64, [](int x, int y, uint8_t* p) {
    const float n = 0.55f * Noise(x / 7.0f, y / 7.0f, 9, 7) + 0.45f * Noise(x / 2.5f, y / 2.5f, 26, 8);
    const float dust = std::max(Noise(x / 9.0f, y / 9.0f, 7, 9) - 0.55f, 0.0f) * 2.2f;
    p[0] = Byte(38 + 30 * n + 42 * dust), p[1] = Byte(36 + 27 * n + 37 * dust), p[2] = Byte(34 + 23 * n + 30 * dust), p[3] = 255;
  });
  charredMaterial_ = Pattern(device_, 32, [](int x, int y, uint8_t* p) {
    const float n = Noise(x / 4.0f, y / 4.0f, 8, 10) - 0.5f;
    p[0] = Byte(46 + 30 * n), p[1] = Byte(220 + 20 * n), p[2] = 0, p[3] = 0;
  });
  for (int k = 0; k < kDebrisShapes; ++k) {
    const DebrisShape& s = shapes_[static_cast<size_t>(k)];
    Shape& shape = shape_[k];
    auto indices = [&](size_t count) {
      std::vector<uint16_t> list(count);
      for (size_t i = 0; i < count; ++i) list[i] = static_cast<uint16_t>(i);
      gfx::BufferDesc ib;
      ib.kind = gfx::BufferKind::Index;
      ib.stride = 2;
      ib.size = static_cast<uint32_t>(list.size() * sizeof(uint16_t));
      ib.data = list.data();
      return device_->CreateBuffer(ib);
    };
    shape.skinIndices = indices(s.skin.size());
    shape.skinCount = static_cast<uint32_t>(s.skin.size());
    // (a model of one level in a material of the debris's own)
    auto model = [&](gfx::Buffer vertices, gfx::Buffer indices, uint32_t count, gfx::Texture albedo, gfx::Texture material) {
      ModelLod lod;
      lod.serial = 0x40000000u + serial_++;  // (apart from the models' own numbers, which count up from 1)
      lod.vertices = vertices;
      lod.indices = indices;
      lod.indexCount = lod.vertexCount = count;
      lod.indexBytes = 2;
      lod.albedo = albedo;
      lod.specular = material;
      lod.normals = textures_->FlatNormal();
      lod.secondary = textures_->White();
      lod.labPbr = true;
      lod.teamMask = false;
      lod.shaderName = "LabPBR";
      auto m = std::make_unique<Model>();
      m->lods.push_back(lod);
      m->ok = vertices && indices;
      m->scale = 1;
      m->boundsMin = s.lo;
      m->boundsMax = s.hi;
      return m;
    };
    {  // the charred skin (the textures' blotches about a fifth of the piece)
      std::vector<ScmVertex> skin = s.skin;
      for (ScmVertex& v : skin) {
        v.uv0[0] = v.uv1[0] = v.uv0[0] * 1.3f + 0.17f * static_cast<float>(k);
        v.uv0[1] = v.uv1[1] = v.uv0[1] * 1.3f + 0.41f * static_cast<float>(k);
      }
      gfx::BufferDesc vb;
      vb.stride = sizeof(ScmVertex);
      vb.size = static_cast<uint32_t>(skin.size() * sizeof(ScmVertex));
      vb.data = skin.data();
      shape.charredVertices = device_->CreateBuffer(vb);
      shape.charred = model(shape.charredVertices, shape.skinIndices, shape.skinCount, charred_, charredMaterial_);
    }
    if (s.torn.empty()) continue;
    std::vector<ScmVertex> torn = s.torn;
    for (ScmVertex& v : torn) {  // (the textures' blotches a little finer than the piece)
      v.uv0[0] = v.uv1[0] = v.uv0[0] * 1.7f + 0.13f * static_cast<float>(k);
      v.uv0[1] = v.uv1[1] = v.uv0[1] * 1.7f + 0.29f * static_cast<float>(k);
    }
    gfx::BufferDesc vb;
    vb.stride = sizeof(ScmVertex);
    vb.size = static_cast<uint32_t>(torn.size() * sizeof(ScmVertex));
    vb.data = torn.data();
    shape.tornVertices = device_->CreateBuffer(vb);
    shape.tornIndices = indices(torn.size());
    const bool lump = s.kind == DebrisShape::Lump;
    shape.torn = model(shape.tornVertices, shape.tornIndices, static_cast<uint32_t>(torn.size()), lump ? concrete_ : steel_,
                       lump ? concreteMaterial_ : steelMaterial_);
    // (a plate's or a bar's torn faces are thin rims and ends: the skin's shadow is the piece's; a lump's fractures are
    // half of it, and cast theirs)
    shape.torn->lods[0].noShadow = !lump;
  }
}

// A unit's paint spots whose patches (`piece` model units across) are clear of where its paint glows: its specular
// texture's blue (the glow of every faction's look and of LabPBR), read from its file at a level no larger than 256
// (once for each kind of unit that dies: 0.3 to 9 ms for the game's units, 5 to 43 for a remastered one's, 2026-10-19;
// [groundfix] read ahead on a thread of its own as the kind is first drawn, Warm, and taken here). Pieces glowing with
// the unit's lights would read as shots. When every spot glows somewhere, all are kept.
std::vector<Vec3> Debris::ClearSpots(const Model* unit, float piece) {
  if (unit->paintMaterial.empty() || unit->paintSpots.empty()) return unit->paintSpots;
  {  // [groundfix] read ahead (Warm): taken when done; waited for while its thread works on it; one still waiting in the
     // queue is taken out of it and read here (as one never told)
    std::unique_lock<std::mutex> lock(glowMutex_);
    auto it = glow_.find(unit);
    if (it != glow_.end() && it->second.started) {
      glowDone_.wait(lock, [&] { return glow_[unit].done; });
      return glow_[unit].clear;
    }
    if (it != glow_.end()) {
      glowQueue_.erase(std::remove(glowQueue_.begin(), glowQueue_.end(), unit), glowQueue_.end());
      glow_.erase(it);
    }
  }
  const auto file = textures_->ReadFile(unit->paintMaterial);
  return ClearOf(unit->paintSpots, file ? &*file : nullptr, piece);
}

// (ClearSpots' work, from the specular file's bytes; on any thread)
std::vector<Vec3> Debris::ClearOf(const std::vector<Vec3>& spots, const std::string* file, float piece) {
  DdsImage image;
  std::string error;
  if (!file || !ParseDds(*file, &image, &error) || image.layers != 1) return spots;
  size_t offset = 0;
  int level = 0;
  while (level + 1 < image.mips && std::max(image.width >> level, image.height >> level) > 256) {
    offset += LevelBytes(image.format, std::max(1, image.width >> level), std::max(1, image.height >> level));
    ++level;
  }
  DdsImage small = image;
  small.width = std::max(1, image.width >> level);
  small.height = std::max(1, image.height >> level);
  small.mips = 1;
  small.data = image.data + offset;
  std::vector<uint8_t> rgba;
  if (LevelBytes(image.format, small.width, small.height) == 0 || offset + LevelBytes(image.format, small.width, small.height) > image.dataSize ||
      !DecodeRgba(small, 0, &rgba))
    return spots;
  std::vector<Vec3> clear;
  for (const Vec3& spot : spots) {
    const float patch = std::clamp(piece * spot.z, 0.01f, 0.12f);
    int most = 0;
    for (int j = 0; j < 5; ++j)
      for (int i = 0; i < 5; ++i) {
        const float u = spot.x + patch * (static_cast<float>(i) / 4 - 0.5f) * 1.2f, v = spot.y + patch * (static_cast<float>(j) / 4 - 0.5f) * 1.2f;
        const int x = ((static_cast<int>(std::floor(u * static_cast<float>(small.width))) % small.width) + small.width) % small.width;
        const int y = ((static_cast<int>(std::floor(v * static_cast<float>(small.height))) % small.height) + small.height) % small.height;
        most = std::max<int>(most, rgba[static_cast<size_t>((y * small.width + x) * 4 + 2)]);
      }
    if (most < 40) clear.push_back(spot);  // (blue below about 0.15: no glow to speak of in any faction's look)
  }
  return clear.empty() ? spots : clear;
}

// (a usual piece's size, the unit's model units)
float Debris::PieceOf(const Model* unit) {
  const Vec3 extent = (unit->boundsMax - unit->boundsMin) * unit->scale;
  return std::max({extent.x, extent.z, 0.3f}) * 0.07f / std::max(unit->scale, 1e-4f);
}

// [groundfix] (debris.h says what it does)
void Debris::Warm(const Model* unit) {
  if (!unit || !unit->ok || unit->paintMaterial.empty() || unit->paintSpots.empty() || !files || looks_.count(unit)) return;
  std::lock_guard<std::mutex> lock(glowMutex_);
  if (glow_.count(unit)) return;
  Glow& g = glow_[unit];
  g.material = unit->paintMaterial;  // (copies: the thread reads nothing of the model)
  g.spots = unit->paintSpots;
  g.piece = PieceOf(unit);
  glowQueue_.push_back(unit);
  if (!glowThread_.joinable()) glowThread_ = std::thread([this] { GlowWorker(); });
  glowWake_.notify_one();
}

// [groundfix] (Warm's thread: each kind told, in turn; waits while there is none; stops with the debris)
void Debris::GlowWorker() {
  std::unique_lock<std::mutex> lock(glowMutex_);
  for (;;) {
    glowWake_.wait(lock, [this] { return glowStopping_ || !glowQueue_.empty(); });
    if (glowStopping_) return;
    const Model* unit = glowQueue_.front();
    glowQueue_.pop_front();
    Glow& g = glow_[unit];
    g.started = true;
    const std::string material = g.material;
    const std::vector<Vec3> spots = g.spots;
    const float piece = g.piece;
    lock.unlock();
    const std::optional<std::string> file = files->ReadFile(moho::Vfs::Normalize(material));
    std::vector<Vec3> clear = ClearOf(spots, file ? &*file : nullptr, piece);
    lock.lock();
    Glow& done = glow_[unit];  // (again: the map may have grown meanwhile)
    done.clear = std::move(clear);
    done.done = true;
    glowDone_.notify_all();
  }
}

// A unit's pieces' painted skins: each shape's skin laid onto a patch of the unit's texture where its paint lies (one of
// its paint spots for each shape; a piece's patch about as much of the texture as the piece's size takes of the unit),
// in the unit's own textures and material. Without a unit (a debris shell whose unit is not known): plain, the colour
// given over it.
const Debris::Looks& Debris::LooksOf(const Model* unit, char kind) {
  auto known = looks_.find(unit);
  if (known != looks_.end()) return known->second;
  if (!shapesMade_) MakeShapes();
  Looks& looks = looks_[unit];
  {  // its few shapes, by its kind, from a number of its own (its size: the same every run)
    uint32_t h = 2166136261u ^ static_cast<uint32_t>(static_cast<unsigned char>(kind));
    if (unit)
      for (float f : {unit->boundsMin.x, unit->boundsMin.y, unit->boundsMin.z, unit->boundsMax.x, unit->boundsMax.y, unit->boundsMax.z}) {
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        h = (h ^ bits) * 16777619u;
      }
    auto next = [&h]() {
      h ^= h << 13;
      h ^= h >> 17;
      h ^= h << 5;
      return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
    };
    for (int tries = 0; tries < 64 && static_cast<int>(looks.picks.size()) < kPicks; ++tries) {
      const int k = PickDebrisShape(kind, next(), next());
      if (std::find(looks.picks.begin(), looks.picks.end(), k) == looks.picks.end()) looks.picks.push_back(k);
    }
  }
  const ModelLod* from = unit && !unit->lods.empty() ? &unit->lods[0] : nullptr;
  const float piece = unit ? PieceOf(unit) : 0.0f;  // (a usual piece's size, the unit's model units)
  const std::vector<Vec3> spots = unit ? ClearSpots(unit, piece) : std::vector<Vec3>{};
  for (int k = 0; k < kDebrisShapes; ++k) {
    const DebrisShape& s = shapes_[static_cast<size_t>(k)];
    Vec2 spot{0.3f + 0.4f * std::fmod(0.618f * static_cast<float>(k), 1.0f), 0.3f + 0.4f * std::fmod(0.382f * static_cast<float>(k), 1.0f)};
    float patch = 0.11f;
    if (!spots.empty()) {
      const Vec3& p = spots[static_cast<size_t>(k * 7 + 3) % spots.size()];
      spot = {p.x, p.y};
      patch = std::clamp(piece * p.z, 0.01f, 0.12f);
    }
    std::vector<ScmVertex> skin = s.skin;
    for (ScmVertex& v : skin) {
      v.uv0[0] = v.uv1[0] = spot.x + v.uv0[0] * patch;
      v.uv0[1] = v.uv1[1] = spot.y + v.uv0[1] * patch;
    }
    gfx::BufferDesc vb;
    vb.stride = sizeof(ScmVertex);
    vb.size = static_cast<uint32_t>(skin.size() * sizeof(ScmVertex));
    vb.data = skin.data();
    const gfx::Buffer vertices = device_->CreateBuffer(vb);
    looks.buffers.push_back(vertices);
    {
      auto m = std::make_unique<Model>();
      ModelLod lod;
      lod.serial = 0x40000000u + serial_++;
      lod.vertices = vertices;
      lod.indices = shape_[k].skinIndices;
      lod.indexCount = shape_[k].skinCount;
      lod.vertexCount = static_cast<uint32_t>(skin.size());
      lod.indexBytes = 2;
      // the unit's own paint and material (without one: plain grey, the colour given over it)
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
      m->lods.push_back(lod);
      m->ok = vertices && shape_[k].skinIndices;
      m->scale = 1;
      m->boundsMin = s.lo;
      m->boundsMax = s.hi;
      looks.painted[static_cast<size_t>(k)] = std::move(m);
    }
  }
  return looks;
}

void Debris::Dress(Chunk* c, const Model* unit, char kind, Vec3 color) {
  const Looks& looks = LooksOf(unit, kind);
  const int k = looks.picks[std::min(static_cast<size_t>(Random() * static_cast<float>(looks.picks.size())), looks.picks.size() - 1)];
  // (three in ten charred; the painted ones dulled by dust some time after they come to rest)
  const bool charred = Random() < 0.3f;
  c->look = charred ? shape_[k].charred.get() : looks.painted[static_cast<size_t>(k)].get();
  c->dusty = charred ? nullptr : shape_[k].charred.get();
  c->torn = shape_[k].torn.get();
  c->solid = shapes_[static_cast<size_t>(k)].kind == DebrisShape::Lump;
  c->color = color * kDulled;  // (its army's colour a little dulled, sooty: torn bits of paint, not bright specks)
}

void Debris::StopTrail(Chunk* c) {
  if (c->trail && smoke) smoke->Stop(c->trail);
  c->trail = 0;
}

void Debris::Sink(Chunk* c, double clock) {
  c->sank = clock;
  c->at.y = std::min(c->at.y, waterLevel);
  c->velocity = {c->velocity.x * 0.25f, -0.3f, c->velocity.z * 0.25f};
  c->spin = c->spin * 0.3f;
  StopTrail(c);
}

void Debris::Throw(const std::string& unitId, const Model* model, Vec3 position, const Quat& turn, Vec3 color, Vec3 velocity, uint32_t seed) {
  if (!model || !model->ok) return;
  seed_ ^= seed * 2654435761u + 0x6d2b79f5u;
  if (!seed_) seed_ = 1;
  const Vec3 lo = model->boundsMin * model->scale, hi = model->boundsMax * model->scale;
  const Vec3 extent = hi - lo;
  const float across = std::max({extent.x, extent.z, 0.3f});
  const char kind = unitId.size() > 2 ? static_cast<char>(std::tolower(static_cast<unsigned char>(unitId[2]))) : 'l';
  // its pieces: its own fragments where there are some, else the library's, each from somewhere in its lower body,
  // turned any way; mostly small, a few larger
  std::vector<Piece> pieces;
  if (fragments) pieces = fragments(unitId, model);
  const bool own = !pieces.empty();
  if (!own) {
    const int count = std::clamp(static_cast<int>(std::lround((4 + 3.5f * across) * amount)), amount > 0 ? 3 : 0, 28);
    for (int i = 0; i < count; ++i) {
      Piece p;
      p.rest = {lo.x + extent.x * (0.1f + 0.8f * Random()), lo.y + extent.y * (0.15f + 0.55f * Random()), lo.z + extent.z * (0.1f + 0.8f * Random())};
      const Vec3 tilt = Normalize(Vec3{Random() - 0.5f, Random() - 0.5f, Random() - 0.5f});
      const float half = Random() * 3.14159265f, s = std::sin(half);
      p.turn = Quat{tilt.x * s, tilt.y * s, tilt.z * s, std::cos(half)};
      const float big = Random();
      p.size = p.scale = std::clamp(across * (0.035f + 0.05f * Random() + 0.09f * big * big * big * big), 0.03f, 0.45f);
      pieces.push_back(p);
    }
  }
  // (the two biggest trail smoke while they fly, if they are big enough to)
  float biggest = 0, second = 0;
  for (const Piece& p : pieces) {
    if (p.size > biggest) second = biggest, biggest = p.size;
    else if (p.size > second) second = p.size;
  }
  // each flies from where it sat, out from the unit's middle and up; the bigger, the slower it turns
  const float push = std::clamp(std::sqrt(across), 0.6f, 2.5f), lift = std::clamp(std::sqrt(across), 0.7f, 2.2f);
  const Vec3 middle = position + Rotate(turn, (lo + hi) * 0.5f);
  for (const Piece& p : pieces) {
    if ((own && !p.model) || flying_.size() >= 800) continue;
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
    if (own) {
      c.look = p.model;
      c.color = color * kDulled;
    } else {
      Dress(&c, model, kind, color);
    }
    c.smokes = p.size >= 0.09f && p.size >= second;
    c.born = clock_ >= 0 ? clock_ : 0;
    c.key = 0x40000000u | (nextKey_++ & 0x1fffffffu);
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
      // (none from what dies on or in the water: it goes down; the game's shells are its debris)
      d.thrown = (u.layer & (2 | 4 | 8)) != 0;
      // (what the game's debris shells thrown near it are pieces of: Shell)
      d.unit = (*frame.blueprints)[u.blueprint];
      d.model = model(d.unit);
      d.color = color(d.army);
      if (d.model && d.model->ok) {
        const Vec3 extent = (d.model->boundsMax - d.model->boundsMin) * d.model->scale;
        d.radius = 0.5f * std::max({extent.x, extent.y, extent.z});
      }
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
      Throw(d.unit, d.model, at, d.turn, d.color, d.air ? d.velocity : Vec3{}, u.id);
    }
  }
  for (auto it = dying_.begin(); it != dying_.end();) {
    Dying& d = it->second;
    if (d.stamp == stamp_) {
      ++it;
      continue;
    }
    // gone before its chunks were thrown (an aircraft that met the ground between two frames): where it was last
    if (!d.thrown && clock - d.seen < 2 && d.at.y - Ground(d.at.x, d.at.z) < 4) {
      d.thrown = true;
      Throw(d.unit, d.model, {d.at.x, Ground(d.at.x, d.at.z) + 0.2f, d.at.z}, d.turn, d.color, d.velocity, it->first);
    }
    // (kept a while: a frame may leave a unit out for a moment)
    if (clock - d.seen > 5 || clock < d.seen - 1) it = dying_.erase(it);
    else ++it;
  }
}

void Debris::BeginShells() {
  ++shellStamp_;
  shellFrame_ = true;
}

bool Debris::Shell(uint32_t id, uint32_t blueprint, const std::string& path, Vec3 at, const Quat& turn, Vec3 velocity, float scale, bool stopped) {
  // (which blueprints are debris shells: the death's DebrisMisc pieces, /effects/entities/DebrisMisc04/... - the game's
  // GenericDebris; not the bits a unit's bones are carried off on, DebrisBoneAttach..., which are its own parts)
  if (blueprint >= shellKinds_.size()) shellKinds_.resize(static_cast<size_t>(blueprint) + 1, -1);
  int8_t& kind = shellKinds_[blueprint];
  if (kind < 0) {
    std::string p = path;
    for (char& ch : p) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    kind = p.find("/effects/entities/debrismisc") != std::string::npos ? 1 : 0;
  }
  if (!kind) return false;
  auto it = shells_.find(id);
  if (it == shells_.end()) {
    GameShell s;
    ++shellsSeen;
    s.drawn = Random() < shellsDrawn;
    if (s.drawn) {
      // whose: the dying unit nearest where it first shows, within its size and a little (a shell is thrown from inside
      // the unit's box and has flown a beat before it is seen)
      const double now = clock_ >= 0 ? clock_ : 0;
      const Dying* from = nullptr;
      float best = 1e9f;
      for (const auto& [unit, d] : dying_) {
        if (now - d.seen > 1.5) continue;
        const float away = Length(at - d.at);
        if (away < d.radius + 4 && away < best) {
          best = away;
          from = &d;
        }
      }
      Chunk& c = s.piece;
      std::vector<Piece> own;
      if (from && from->model && fragments) own = fragments(from->unit, from->model);
      if (!own.empty()) {  // (one of its own fragments, at its own size)
        const Piece& p = own[std::min(static_cast<size_t>(Random() * static_cast<float>(own.size())), own.size() - 1)];
        c.look = p.model;
        c.scale = p.scale;
        c.size = p.size;
        c.color = from->color * kDulled;
      } else {
        const char unitKind = from && from->unit.size() > 2 ? static_cast<char>(std::tolower(static_cast<unsigned char>(from->unit[2]))) : 'l';
        Dress(&c, from ? from->model : nullptr, unitKind, from ? from->color : Vec3{0.75f, 0.75f, 0.75f});
        c.size = c.scale = std::clamp(scale * kShellAcross * shellSize, 0.02f, 0.45f);
      }
      c.born = now;
      c.key = 0x40000000u | (nextKey_++ & 0x1fffffffu);
      s.drawn = c.look != nullptr;
    }
    it = shells_.emplace(id, std::move(s)).first;
  }
  GameShell& s = it->second;
  s.stamp = shellStamp_;
  if (s.drawn && !s.taken) {
    s.piece.at = at;
    s.piece.turn = turn;
    if (!stopped) s.piece.velocity = velocity;  // (a stopped one's is that of the beat it hit in, or none)
    // it hit the ground or the water (its script ends it at the next beat), or it is at the water's surface (whatever the
    // game does with it there, nothing rests on the water): the piece is the debris's from now on
    const bool water = Ground(at.x, at.z) < waterLevel;
    if (stopped || (water && at.y <= waterLevel + 0.02f)) TakeOver(&s, clock_ >= 0 ? clock_ : 0);
  }
  return true;
}

// A game's shell's piece goes on as the debris's own, where the game has it now or had it last: into the water it sinks,
// on the ground it hops and comes to rest with the rubble; one that went out of sight in the air is let go.
void Debris::TakeOver(GameShell* s, double clock) {
  if (s->taken || !s->drawn) return;
  s->taken = true;
  if (flying_.size() >= 800) return;
  Chunk c = s->piece;
  const float ground = Ground(c.at.x, c.at.z);
  const bool water = ground < waterLevel;
  if (c.at.y - (water ? waterLevel : ground) >= 1.5f) return;
  const float speed = Length(c.velocity);
  c.spin = Normalize(Vec3{Random() - 0.5f, Random() - 0.5f, Random() - 0.5f}) * ((3 + 6 * Random()) * std::min(1.0f, 0.15f / std::max(c.size, 0.01f)));
  if (water) {
    Sink(&c, clock);
  } else {
    c.at.y = std::max(c.at.y, ground + c.size * 0.3f);
    c.velocity = {c.velocity.x * 0.35f, 0.3f + 0.12f * speed, c.velocity.z * 0.35f};
  }
  flying_.push_back(c);
}

void Debris::Update(double clock, Vec3 eye, MeshRenderer* meshes, Blasts* splashes) {
  // (a game started over, or played back from earlier: what was thrown later is not there yet)
  if (clock_ >= 0 && clock < clock_ - 2) Clear();
  const float step = clock_ < 0 ? 0.0f : static_cast<float>(std::clamp(clock - clock_, 0.0, 0.1));
  clock_ = clock;
  // the game's shells it has ended since the last frame of the game (they met the ground or the water: GenericDebris's
  // OnImpact ends them): a drawn one's piece goes on as the debris's own (TakeOver), if it is not already
  if (shellFrame_) {
    shellFrame_ = false;
    for (auto it = shells_.begin(); it != shells_.end();) {
      GameShell& s = it->second;
      if (s.stamp == shellStamp_) {
        ++it;
        continue;
      }
      TakeOver(&s, clock);
      it = shells_.erase(it);
    }
  }
  // the flying chunks: falling, turning, bouncing off the ground (in steps of at most a sixtieth of a second); those in
  // the water sinking
  const int steps = std::max(1, static_cast<int>(std::ceil(step * 60)));
  const float dt = step / static_cast<float>(steps);
  for (size_t i = 0; i < flying_.size();) {
    Chunk& c = flying_[i];
    bool gone = false, rests = false;
    for (int s = 0; s < steps && dt > 0 && !gone && !rests; ++s) {
      if (c.sank > 0) {  // (drawn down slowly through the water, slowing; gone on the bottom, or after three seconds)
        c.velocity.x *= 1 - 2 * dt;
        c.velocity.z *= 1 - 2 * dt;
        c.at += c.velocity * dt;
        c.turn = Turned(c.turn, c.spin, dt);
        if (c.at.y < Ground(c.at.x, c.at.z) + c.size * 0.2f) gone = true;
        continue;
      }
      c.velocity.y -= kGravity * dt;
      c.at += c.velocity * dt;
      c.turn = Turned(c.turn, c.spin, dt);
      if (c.at.x < 0 || c.at.z < 0 || (mapWidth > 0 && c.at.x > mapWidth) || (mapHeight > 0 && c.at.z > mapHeight)) {
        gone = true;
        break;
      }
      const float ground = Ground(c.at.x, c.at.z);
      if (ground < waterLevel && c.at.y < waterLevel) {  // into the water: a splash, and it sinks
        if (splashes) splashes->Add({c.at.x, waterLevel, c.at.z}, c.size, clock, true);
        Sink(&c, clock);
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
    if (c.sank > 0 && clock - c.sank > 3) gone = true;
    if (gone) {
      StopTrail(&c);
      flying_[i] = flying_.back();
      flying_.pop_back();
      continue;
    }
    if (rests) {  // at rest: half sunk into the ground, as rubble lies; its paint dulled by dust some time later
      StopTrail(&c);
      c.velocity = c.spin = {};
      c.at.y = Ground(c.at.x, c.at.z) + c.size * 0.12f;
      c.settled = clock;
      c.dustAfter = 15 + 75 * Random();
      c.world = Mat4::FromTRS(c.at, c.turn, c.scale);
      lying_.push_back(c);
      flying_[i] = flying_.back();
      flying_.pop_back();
      continue;
    }
    // (the biggest trail the game's thin debris smoke while they fly)
    if (c.smokes && c.sank <= 0 && smoke) {
      if (!c.trail) c.trail = smoke->Start(kTrail, c.at, Quat{}, std::clamp(c.size / 0.15f, 0.5f, 1.5f), -1, true);
      else smoke->Move(c.trail, c.at, Quat{});
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
  for (const Chunk& c : flying_) {
    const float away = Length(c.at - eye);
    if (away < 260) Draw(meshes, c, Mat4::FromTRS(c.at, c.turn, c.scale), c.look, clock, c.solid || away < c.size * 140);
  }
  for (const auto& [id, s] : shells_) {
    if (!s.drawn || s.taken || s.stamp != shellStamp_) continue;
    const Chunk& c = s.piece;
    const float away = Length(c.at - eye);
    if (away < 260) Draw(meshes, c, Mat4::FromTRS(c.at, c.turn, c.scale), c.look, clock, c.solid || away < c.size * 140);
  }
  for (const Chunk& c : lying_) {
    const float away = Length(c.at - eye);
    if (away > 260) continue;
    const Model* look = c.dusty && clock - c.settled > c.dustAfter ? c.dusty : c.look;
    if (c.fading > 0) {
      const float f = std::clamp(static_cast<float>(clock - c.fading) / 10, 0.0f, 1.0f);
      Draw(meshes, c, Mat4::FromTRS(c.at - Vec3{0, c.size * 0.7f * f, 0}, c.turn, c.scale * (1 - 0.5f * f)), look, clock, c.solid || away < c.size * 140);
    } else {
      Draw(meshes, c, c.world, look, clock, c.solid || away < c.size * 140);
    }
  }
  meshes->addingScenery = before;
}

void Debris::Show(const Model* unit, Vec3 at, Vec3 right, float size, double clock, MeshRenderer* meshes) {
  if (!meshes || (unit && !unit->ok)) return;
  const Looks& looks = LooksOf(unit, 'l');
  for (int row = 0; row < 2; ++row)
    for (int k = 0; k < kDebrisShapes; ++k) {
      Chunk c;
      c.look = row ? shape_[k].charred.get() : looks.painted[static_cast<size_t>(k)].get();
      c.torn = shape_[k].torn.get();
      c.solid = shapes_[static_cast<size_t>(k)].kind == DebrisShape::Lump;
      c.color = Vec3{0.2f, 0.45f, 1.0f} * kDulled;
      c.born = clock - std::fmod(clock, 8.0);  // (fresh again every eight seconds)
      c.key = 0x5f000000u + static_cast<uint32_t>(row * kDebrisShapes + k);
      const float a = static_cast<float>(clock) * 0.5f + static_cast<float>(k);
      const Vec3 axis = Normalize(Vec3{0.3f, 1, 0.2f});
      const Quat turn{axis.x * std::sin(a * 0.5f), axis.y * std::sin(a * 0.5f), axis.z * std::sin(a * 0.5f), std::cos(a * 0.5f)};
      const Vec3 place = at + right * ((static_cast<float>(k) - 0.5f * (kDebrisShapes - 1)) * size * 1.5f) - Vec3{0, row * size * 1.6f, 0};
      const bool before = meshes->addingScenery;
      meshes->addingScenery = true;
      Draw(meshes, c, Mat4::FromTRS(place, turn, size), c.look, clock, true);
      meshes->addingScenery = before;
    }
}

// A piece: its skin, and its torn faces when it is near enough for them to show (a plate's rims are thin), glowing while
// it is fresh with hot metal on (a lump's concrete does not).
void Debris::Draw(MeshRenderer* meshes, const Chunk& c, const Mat4& world, const Model* look, double clock, bool near) {
  meshes->Add(look, world, c.color, 1, nullptr, nullptr, nullptr, c.key);
  if (!c.torn || !near) return;
  Vec3 heat;
  const Vec3* tint = nullptr;
  // (as if a second old already: its rims start a deep orange, not the white-hot of a wreck's heart, so that a flying piece
  // is never taken for a shot)
  const float age = static_cast<float>(clock - c.born) + 1.0f;
  if (hot && !c.solid && age >= 1.0f && age < hotSeconds) {
    heat = HotMetal::Tint(age, hotSeconds);
    tint = &heat;
  }
  meshes->Add(c.torn, world, {1, 1, 1}, 1, nullptr, tint, nullptr, c.key | 0x20000000u);
}

}  // namespace render
