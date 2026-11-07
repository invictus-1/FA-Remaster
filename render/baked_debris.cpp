#include "baked_debris.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>

#include "core/vfs.h"
#include "hot_metal.h"
#include "json.h"
#include "mesh_renderer.h"
#include "models.h"

namespace render {
namespace {

// The bake's own gravity, model units a second squared (Blender's, on the unit's model: the visuals track's rigid bodies fall
// about 10 a second squared, the air slowing them a little): what a chunk still flying at the bake's end falls by.
constexpr float kBakeGravity = 9.81f;
// A chunk still moving faster than this at the bake's end (model units a second), and above the bake's ground, flies on.
constexpr float kStillMoving = 0.5f;
// At most this long after the bake's end does a chunk fly on (seconds).
constexpr float kMostFlyingOn = 6;

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

Quat Normalized(Quat q) {
  const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  return l > 0 ? Quat{q.x / l, q.y / l, q.z / l, q.w / l} : Quat{};
}

Quat Turned(const Quat& q, Vec3 spin, float dt) {  // q turned on by `spin` (radians a second, the model's axes) for dt
  const Quat w{spin.x, spin.y, spin.z, 0};
  const Quat d = Mul(w, q);
  return Normalized(Quat{q.x + 0.5f * dt * d.x, q.y + 0.5f * dt * d.y, q.z + 0.5f * dt * d.z, q.w + 0.5f * dt * d.w});
}

// A chunk's vertices, each from its bone's place at rest (model units), read from the mesh file (as LoadScm reads it: 68 bytes
// a vertex, its bone the first of four bytes at 64), a list a bone.
std::vector<std::vector<Vec3>> ChunkPoints(const std::string& file, const std::vector<ModelBone>& bones) {
  std::vector<std::vector<Vec3>> points(bones.size());
  if (file.size() < 48 || file.compare(0, 4, "MODL") != 0) return points;
  int32_t h[11];
  std::memcpy(h, file.data() + 4, sizeof h);
  const int64_t offset = h[3], count = h[5];
  if (offset < 0 || count <= 0 || offset + count * 68 > static_cast<int64_t>(file.size())) return points;
  for (int64_t i = 0; i < count; ++i) {
    const char* v = file.data() + offset + i * 68;
    float p[3];
    std::memcpy(p, v, sizeof p);
    const uint8_t bone = static_cast<uint8_t>(v[64]);
    if (bone >= bones.size()) continue;
    const ModelBone& b = bones[bone];
    points[bone].push_back(Rotate(Conjugate(b.rotation), Vec3{p[0], p[1], p[2]} - b.position));
  }
  return points;
}

}  // namespace

BakedDebris::Set* BakedDebris::SetOf(const std::string& unitId) {
  const std::string id = Lower(unitId);
  auto it = sets_.find(id);
  if (it != sets_.end()) return it->second.get();
  const auto t0 = std::chrono::steady_clock::now();
  std::unique_ptr<Set> set;
  if (files && models) {
    const std::string trial = "/debris-trial/", folder = "/preview/" + id + "/", name = id + "_debris.scm";
    const std::string mesh = files->Exists(trial + name) ? trial + name : files->Exists(folder + name) ? folder + name : std::string();
    const Model* chunks = mesh.empty() ? nullptr : models->DebrisChunks(id, mesh);
    std::optional<std::string> bytes = chunks ? files->ReadFile(mesh) : std::nullopt;
    if (chunks && bytes) {
      set = std::make_unique<Set>();
      set->model = chunks;
      set->folder = folder;
      set->trial = trial;
      set->points = ChunkPoints(*bytes, chunks->bones);
      printf("render: %s: its baked debris read and made in %.1f ms\n", id.c_str(),
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
  }
  Set* s = set.get();
  sets_[id] = std::move(set);
  return s;
}

bool BakedDebris::Has(const std::string& unitId) { return SetOf(unitId) != nullptr; }

const BakedDebris::Style* BakedDebris::StyleOf(Set& set, const std::string& unitId, const std::string& style) {
  auto it = set.styles.find(style);
  if (it != set.styles.end()) return it->second.get();
  const std::string name = Lower(unitId) + "_debris_" + style + ".json";
  const std::string path = files->Exists(set.trial + name) ? set.trial + name : files->Exists(set.folder + name) ? set.folder + name : std::string();
  std::unique_ptr<Style> read = path.empty() ? nullptr : ReadStyle(set, path);
  const Style* s = read.get();
  set.styles[style] = std::move(read);
  return s;
}

// A style's file: each chunk's keys by its bone's name, then what still flies at the bake's end flown on to its ground (its
// last key's speed and spin, the bake's gravity, a key a frame), so that playing it is only reading keys.
std::unique_ptr<BakedDebris::Style> BakedDebris::ReadStyle(const Set& set, const std::string& path) {
  const auto t0 = std::chrono::steady_clock::now();
  std::optional<std::string> text = files->ReadFile(path);
  JsonValue json;
  std::string error;
  if (!text || !ReadJson(*text, &json, &error)) {
    fprintf(stderr, "render: %s: cannot be read%s%s\n", path.c_str(), error.empty() ? "" : ": ", error.c_str());
    return nullptr;
  }
  const Model& chunks = *set.model;
  auto style = std::make_unique<Style>();
  style->fps = static_cast<float>(std::max(1.0, json["fps"].NumberOr(30)));
  style->seconds = static_cast<float>(json["seconds"].NumberOr(0));
  int flyingOn = 0;
  for (const JsonValue& c : json["chunks"].items) {
    const std::string bone = Lower(c["bone"].StringOr(std::string()));
    int index = -1;
    for (size_t b = 0; b < chunks.bones.size() && index < 0; ++b)
      if (Lower(chunks.bones[b].name) == bone) index = static_cast<int>(b);
    if (index < 0) {
      fprintf(stderr, "render: %s: the mesh has no bone %s (left out)\n", path.c_str(), bone.c_str());
      continue;
    }
    Track t;
    t.bone = static_cast<uint32_t>(index);
    for (const JsonValue& k : c["keys"].items) {
      if (k.items.size() < 7) continue;
      float n[7];
      for (int i = 0; i < 7; ++i) n[i] = static_cast<float>(k.items[static_cast<size_t>(i)].NumberOr(0));
      const Quat q = Normalized(Quat{n[4], n[5], n[6], n[3]});  // (the file's turn is w, x, y, z)
      t.keys.insert(t.keys.end(), {n[0], n[1], n[2], q.x, q.y, q.z, q.w});
    }
    if (t.keys.empty()) continue;
    const std::vector<Vec3>& points = set.points[t.bone];
    for (const Vec3& p : points) t.reach = std::max(t.reach, Length(p));
    // (where its lowest corner is, turned so: the bake's ground is y 0)
    const Quat rest = chunks.bones[t.bone].rotation;
    auto bottom = [&](Vec3 at, const Quat& q) {
      const Quat turn = Mul(q, Conjugate(rest));
      float low = 1e30f;
      for (const Vec3& p : points) low = std::min(low, Rotate(turn, p).y);
      return points.empty() ? at.y : at.y + low;
    };
    // flown on: a chunk still moving at the bake's end and off its ground
    const size_t n = t.keys.size() / 7;
    if (n >= 2) {
      const float* a = &t.keys[(n - 2) * 7];
      const float* b = &t.keys[(n - 1) * 7];
      Vec3 at{b[0], b[1], b[2]};
      Vec3 velocity = (at - Vec3{a[0], a[1], a[2]}) * style->fps;
      Quat q{b[3], b[4], b[5], b[6]};
      if (Length(velocity) > kStillMoving && bottom(at, q) > 0.05f) {
        // (its spin: the turn from the last key but one to the last, as an axis and an angle a second)
        Quat d = Mul(q, Conjugate(Quat{a[3], a[4], a[5], a[6]}));
        if (d.w < 0) d = Quat{-d.x, -d.y, -d.z, -d.w};
        const float half = std::acos(std::clamp(d.w, -1.0f, 1.0f)), s = std::sin(half);
        const Vec3 spin = s > 1e-5f ? Vec3{d.x, d.y, d.z} * (2 * half * style->fps / s) : Vec3{};
        const float dt = 1 / style->fps;
        for (int step = 0; step < static_cast<int>(kMostFlyingOn * style->fps); ++step) {
          velocity.y -= kBakeGravity * dt;
          at += velocity * dt;
          q = Turned(q, spin, dt);
          const float low = bottom(at, q);
          const bool down = low <= 0;
          if (down) at.y -= low;  // (it lies on the ground where it met it)
          t.keys.insert(t.keys.end(), {at.x, at.y, at.z, q.x, q.y, q.z, q.w});
          if (down) break;
        }
        ++flyingOn;
      }
    }
    style->settled = std::max(style->settled, static_cast<float>(t.keys.size() / 7 - 1) / style->fps);
    style->tracks.push_back(std::move(t));
  }
  if (style->tracks.empty()) {
    fprintf(stderr, "render: %s: no chunk's keys (not used)\n", path.c_str());
    return nullptr;
  }
  std::sort(style->tracks.begin(), style->tracks.end(), [](const Track& x, const Track& y) { return x.bone < y.bone; });
  style->settled = std::max(style->settled, style->seconds);
  printf("render: %s: %zu chunks over %.2f s, %d flying on past it (all down at %.2f s); read in %.1f ms\n", path.c_str(), style->tracks.size(),
         style->seconds, flyingOn, style->settled, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  return style;
}

bool BakedDebris::Play(const std::string& unitId, const Model* unit, Vec3 at, const Quat& turn, Vec3 color, double clock, const std::string& style) {
  if (!unit || !unit->ok) return false;
  Set* set = SetOf(unitId);
  if (!set) return false;
  const Style* s = StyleOf(*set, unitId, style);
  if (!s && style != "break") s = StyleOf(*set, unitId, "break");
  if (!s) return false;
  if (Playing() >= mostPlaying) {
    ++refused;
    return false;
  }
  // (past the most lying, the oldest that is not sinking already sinks away now)
  if (static_cast<int>(deaths_.size()) >= mostLying)
    for (auto& d : deaths_)
      if (d->sinkFrom < 0) {
        d->sinkFrom = clock;
        break;
      }
  auto d = std::make_unique<Death>();
  d->set = set;
  d->style = s;
  d->at = {at.x, groundHeight ? groundHeight(at.x, at.z) : at.y, at.z};
  const Vec3 forward = Rotate(turn, Vec3{0, 0, 1});
  const float yaw = std::atan2(forward.x, forward.z);
  d->heading = Quat{0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)};
  d->scale = unit->scale;
  const Vec3 extent = (unit->boundsMax - unit->boundsMin) * unit->scale;
  d->radius = 0.5f * std::max({extent.x, extent.y, extent.z, 0.2f});
  d->color = color * paint;
  d->born = clock;
  d->id = serial_++;
  d->bones.resize(s->tracks.size());
  d->centre = d->at;
  if (played == 0) printf("render: %s: its baked debris (%s) first played at %.3f s\n", Lower(unitId).c_str(), style.c_str(), clock);
  deaths_.push_back(std::move(d));
  ++played;
  return true;
}

int BakedDebris::Playing() const {
  int n = 0;
  for (const auto& d : deaths_) n += clock_ < 0 || clock_ - d->born < d->style->settled ? 1 : 0;
  return n;
}

void BakedDebris::Clear() {
  deaths_.clear();
  clock_ = -1;
}

void BakedDebris::Update(double clock, Vec3 eye, MeshRenderer* meshes) {
  if (clock_ >= 0 && clock < clock_ - 2) Clear();  // (a game started over, or played back from earlier)
  clock_ = clock;
  auto ground = [&](float x, float z) { return groundHeight ? groundHeight(x, z) : 0.0f; };
  const bool before = meshes ? meshes->addingScenery : false;
  if (meshes) meshes->addingScenery = true;  // (as the debris: scenery, pressing no grass flat)
  for (size_t k = 0; k < deaths_.size();) {
    Death& d = *deaths_[k];
    const Style& s = *d.style;
    const float age = static_cast<float>(clock - d.born);
    // lying still after the last chunk is down, then sinking away (each by its own height); sooner past the most lying
    const double sinkFrom = d.sinkFrom >= 0 ? d.sinkFrom : d.born + s.settled + lieSeconds;
    const float sinkFor = d.sinkFrom >= 0 ? 8.0f : sinkSeconds;
    const float sunk = std::clamp(static_cast<float>(clock - sinkFrom) / sinkFor, 0.0f, 1.0f);
    if (clock - sinkFrom >= sinkFor) {
      deaths_.erase(deaths_.begin() + static_cast<std::ptrdiff_t>(k));
      continue;
    }
    // each chunk at that moment of its keys (positions in a straight line between two, turns the shorter way), on the ground
    // under it: its height over the bake's flat ground laid on the map's there, its place in the unit's frame in world units
    // (the model renderer's poses are: MeshRenderer::BuildRows)
    const float frame = std::max(age, 0.0f) * s.fps;
    Vec3 sum{};
    std::vector<Vec3> at(s.tracks.size());
    for (size_t c = 0; c < s.tracks.size(); ++c) {
      const Track& t = s.tracks[c];
      const size_t n = t.keys.size() / 7;
      const size_t i = std::min(static_cast<size_t>(frame), n - 1);
      const float f = i + 1 < n ? frame - static_cast<float>(i) : 0.0f;
      const float* a = &t.keys[i * 7];
      const float* b = &t.keys[std::min(i + 1, n - 1) * 7];
      const Vec3 p = Lerp(Vec3{a[0], a[1], a[2]}, Vec3{b[0], b[1], b[2]}, f) * d.scale;
      const Quat q = Nlerp(Quat{a[3], a[4], a[5], a[6]}, Quat{b[3], b[4], b[5], b[6]}, f);
      const Vec3 flat = Rotate(d.heading, Vec3{p.x, 0, p.z});
      const float lift = ground(d.at.x + flat.x, d.at.z + flat.z) - d.at.y - sunk * (p.y + t.reach * d.scale);
      const Vec3 own{p.x, p.y + lift, p.z};
      FrameBone& bone = d.bones[c];
      bone.bone = t.bone;
      bone.position[0] = own.x, bone.position[1] = own.y, bone.position[2] = own.z;
      bone.orientation[0] = q.x, bone.orientation[1] = q.y, bone.orientation[2] = q.z, bone.orientation[3] = q.w;
      bone.hidden = false;
      at[c] = d.at + Rotate(d.heading, own);
      sum += at[c];
    }
    d.centre = sum * (1.0f / static_cast<float>(std::max<size_t>(at.size(), 1)));
    d.reach = 0;
    for (size_t c = 0; c < at.size(); ++c) d.reach = std::max(d.reach, Length(at[c] - d.centre) + s.tracks[c].reach * d.scale);
    ++k;
    if (!meshes || Length(d.centre - eye) - d.reach > 260) continue;
    Pose pose;
    pose.now = pose.next = d.bones.data();
    pose.nowCount = pose.nextCount = d.bones.size();
    // (hot: its age and how long it glows, as a wreck's; the shader lights the heat mask by them)
    const float seconds = heat ? heat->WreckSeconds(d.radius) : 55.0f;
    const Vec3 tint{0, -age, -seconds};
    meshes->Add(d.set->model, Mat4::FromTRS(d.at, d.heading, d.scale), d.color, 1, &pose, hot && age < seconds ? &tint : nullptr, nullptr,
                0x70000000u | (d.id & 0x0fffffffu));
    meshes->Reach(d.centre, d.reach);
  }
  if (meshes) meshes->addingScenery = before;
}

void BakedDebris::ForEachHot(const std::function<void(Vec3, float, float, float, uint32_t)>& f) const {
  if (!hot || clock_ < 0) return;
  for (const auto& d : deaths_) {
    const float age = static_cast<float>(clock_ - d->born), seconds = heat ? heat->WreckSeconds(d->radius) : 55.0f;
    if (age >= 0 && age < seconds) f(d->centre, d->radius, age, seconds, d->id);
  }
}

}  // namespace render
