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
#include "particles.h"

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
    // ([debrispile] a set: its chunks' mesh, or a style's own, in the trial folder or beside the export; its styles read as asked)
    std::string trial = "/debris-trial/";
    const std::string folder = "/preview/" + id + "/";
    // ([debrisstyle] or visuals' sets folder, a unit's set in <ID>/deliver/ of it)
    for (const char* name : {"_debris.scm", "_debris_break.scm"})
      if (files->Exists(trial + id + "/deliver/" + id + name)) {
        trial += id + "/deliver/";
        break;
      }
    bool any = false;
    for (const char* name : {"_debris.scm", "_debris_break.scm", "_debris_melt.scm", "_debris_vaporise.scm"})
      any = any || files->Exists(trial + id + name) || files->Exists(folder + id + name);
    if (any) {
      set = std::make_unique<Set>();
      set->folder = folder;
      set->trial = trial;
      printf("render: %s: has baked debris (looked for in %.1f ms)\n", id.c_str(),
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
  }
  Set* s = set.get();
  sets_[id] = std::move(set);
  return s;
}

bool BakedDebris::Has(const std::string& unitId) { return SetOf(unitId) != nullptr; }

const Model* BakedDebris::HullOf(const std::string& unitId) {  // [debrishull]
  Set* set = SetOf(unitId);
  if (!set) return nullptr;
  if (!set->hullAsked) {
    set->hullAsked = true;
    const std::string id = Lower(unitId), name = id + "_debris_hull.scm";
    const std::string path = files->Exists(set->trial + name) ? set->trial + name : files->Exists(set->folder + name) ? set->folder + name : std::string();
    if (!path.empty()) set->hull = models->DebrisHull(id, path);
  }
  return set->hull;
}

const BakedDebris::Style* BakedDebris::StyleOf(Set& set, const std::string& unitId, const std::string& style) {
  auto it = set.styles.find(style);
  if (it != set.styles.end()) return it->second.get();
  const std::string id = Lower(unitId);
  auto find = [&](const std::string& name) {
    return files->Exists(set.trial + name) ? set.trial + name : files->Exists(set.folder + name) ? set.folder + name : std::string();
  };
  const std::string path = find(id + "_debris_" + style + ".json");
  // [debrispile] its chunks: the style's own mesh where it has one (the same chunks, bent its own way), else the set's
  std::string mesh = find(id + "_debris_" + style + ".scm");
  if (mesh.empty()) mesh = find(id + "_debris.scm");
  const Model* chunks = path.empty() || mesh.empty() ? nullptr : models->DebrisChunks(id, mesh);
  std::unique_ptr<Style> read = chunks ? ReadStyle(*chunks, mesh, path, style) : nullptr;
  const Style* s = read.get();
  set.styles[style] = std::move(read);
  return s;
}

// A style's file: each chunk's keys by its bone's name, then what still flies at the bake's end flown on to its ground (its
// last key's speed and spin, the bake's gravity, a key a frame), so that playing it is only reading keys.
std::unique_ptr<BakedDebris::Style> BakedDebris::ReadStyle(const Model& chunks, const std::string& mesh, const std::string& path,
                                                          const std::string& name) {
  const auto t0 = std::chrono::steady_clock::now();
  std::optional<std::string> text = files->ReadFile(path);
  JsonValue json;
  std::string error;
  if (!text || !ReadJson(*text, &json, &error)) {
    fprintf(stderr, "render: %s: cannot be read%s%s\n", path.c_str(), error.empty() ? "" : ": ", error.c_str());
    return nullptr;
  }
  std::optional<std::string> bytes = files->ReadFile(mesh);
  const std::vector<std::vector<Vec3>> chunkPoints = bytes ? ChunkPoints(*bytes, chunks.bones) : std::vector<std::vector<Vec3>>(chunks.bones.size());
  auto style = std::make_unique<Style>();
  style->model = &chunks;  // [debrispile]
  style->strays = name == "vaporise";
  style->melts = name == "melt";
  style->fps = static_cast<float>(std::max(1.0, json["fps"].NumberOr(30)));
  style->seconds = static_cast<float>(json["seconds"].NumberOr(0));
  // [debrisairsea] its class, an air set's arriving speed, how fast its chunks sink in water
  const std::string cls = Lower(json["class"].StringOr("land"));
  style->cls = cls == "air" ? 1 : cls == "naval" ? 2 : 0;
  style->impactSpeed = static_cast<float>(json["impact"]["speed"].NumberOr(0));
  style->sink = static_cast<float>(std::clamp(json["sink"].NumberOr(0.6), 0.05, 20.0));
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
    const std::vector<Vec3>& points = chunkPoints[t.bone];
    for (const Vec3& p : points) t.reach = std::max(t.reach, Length(p));
    // ([debrisairsea] its longest way: the point farthest from its middle, then the one farthest from that; its thickness:
    // how far its points lie off that line)
    if (!points.empty()) {
      Vec3 a = points[0], b = points[0];
      for (const Vec3& p : points)
        if (Length(p) > Length(a)) a = p;
      for (const Vec3& p : points)
        if (Length(p - a) > Length(b - a)) b = p;
      if (Length(b - a) > 1e-4f) t.along = (b - a) * (1 / Length(b - a));
      for (const Vec3& p : points) {
        const Vec3 off = p - t.along * Dot(p, t.along);
        t.thin = std::max(t.thin, Length(off));
      }
    }
    // (where its lowest corner is, turned so: the bake's ground is y 0)
    const Quat rest = chunks.bones[t.bone].rotation;
    auto bottom = [&](Vec3 at, const Quat& q) {
      const Quat turn = Mul(q, Conjugate(rest));
      float low = 1e30f;
      for (const Vec3& p : points) low = std::min(low, Rotate(turn, p).y);
      return points.empty() ? at.y : at.y + low;
    };
    // [debrisairsea] where it first meets its ground (a naval set's keys end at the water: its last)
    {
      const size_t keys = t.keys.size() / 7;
      t.touches = static_cast<uint32_t>(keys - 1);
      if (style->cls != 2)
        for (size_t k = 1; k < keys; ++k) {
          const float* a = &t.keys[k * 7];
          if (bottom(Vec3{a[0], a[1], a[2]}, Quat{a[3], a[4], a[5], a[6]}) <= 0.03f) {
            t.touches = static_cast<uint32_t>(k);
            break;
          }
        }
    }
    // flown on: a chunk still moving at the bake's end and off its ground (not a naval set's: its keys end at the water)
    const size_t n = t.keys.size() / 7;
    if (n >= 2 && style->cls != 2) {
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
  // [debrispile] when each chunk comes to rest (its last key that is not where it ends); VAPORISE: those that end beyond
  // strayBeyond times the unit's box across (the chunks' at rest: the unit's own) are thrown debris, the biggest of those
  // trailing smoke while they fly
  const Vec3 middle = (chunks.boundsMin + chunks.boundsMax) * 0.5f, half = (chunks.boundsMax - chunks.boundsMin) * (0.5f * strayBeyond);
  std::vector<Track*> thrown;
  for (Track& t : style->tracks) {
    const size_t n = t.keys.size() / 7;
    const float* last = &t.keys[(n - 1) * 7];
    size_t moving = 0;
    for (size_t k = 0; k < n; ++k) {
      const float* a = &t.keys[k * 7];
      if (std::fabs(a[0] - last[0]) + std::fabs(a[1] - last[1]) + std::fabs(a[2] - last[2]) > 0.01f) moving = k + 1;
    }
    t.rests = static_cast<float>(moving) / style->fps;
    t.stray = style->strays && (std::fabs(last[0] - middle.x) > half.x || std::fabs(last[2] - middle.z) > half.z);
    if (t.stray) thrown.push_back(&t);
  }
  std::sort(thrown.begin(), thrown.end(), [](const Track* x, const Track* y) { return x->reach > y->reach; });
  for (size_t k = 0; k < thrown.size() && static_cast<int>(k) < mostTrails; ++k) thrown[k]->smokes = true;
  if (style->strays)
    printf("render: %s: %zu of its chunks thrown beyond %.1f times the unit's box (debris, not the wreck)\n", path.c_str(), thrown.size(), strayBeyond);
  printf("render: %s: %zu chunks over %.2f s, %d flying on past it (all down at %.2f s); read in %.1f ms\n", path.c_str(), style->tracks.size(),
         style->seconds, flyingOn, style->settled, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  return style;
}

bool BakedDebris::Play(const std::string& unitId, const Model* unit, Vec3 at, const Quat& turn, Vec3 color, double clock, const std::string& style,
                       Vec3 fall) {
  if (!unit || !unit->ok) return false;
  Set* set = SetOf(unitId);
  if (!set) return false;
  const Style* s = StyleOf(*set, unitId, style);
  if (!s && style != "break") s = StyleOf(*set, unitId, "break");
  if (!s) return false;
  // [debrisairsea] (on water: a naval set, or an aircraft's coming down at sea; a land set plays nothing there)
  const float bed = groundHeight ? groundHeight(at.x, at.z) : at.y;
  const bool sea = waterLevel > bed + 0.05f;
  if (sea && s->cls == 0) return false;
  if (Playing() >= mostPlaying) {
    ++refused;
    return false;
  }
  // (past the most lying, the oldest that is not sinking already sinks away now; [debrispile] not one drawn as a wreck)
  int lying = 0;
  for (const auto& d : deaths_) lying += !d->prop && !d->unseen ? 1 : 0;
  if (lying >= mostLying)
    for (auto& d : deaths_)
      if (d->sinkFrom < 0 && !d->prop && !d->unseen) {
        d->sinkFrom = clock;
        break;
      }
  auto d = std::make_unique<Death>();
  d->set = set;
  d->style = s;
  d->at = {at.x, sea ? waterLevel : bed, at.z};
  d->sea = sea;
  Vec3 forward = Rotate(turn, Vec3{0, 0, 1});
  // [debrisairsea] (an aircraft: the way it was going, and its throw stretched by its speed over the bake's)
  const float across = std::hypot(fall.x, fall.z);
  if (s->cls == 1 && across > 1) {
    forward = Vec3{fall.x, 0, fall.z};
    if (s->impactSpeed > 0) d->stretch = std::clamp(across / s->impactSpeed, 0.5f, 1.5f);
  }
  const float yaw = std::atan2(forward.x, forward.z);
  d->heading = Quat{0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)};
  d->scale = unit->scale;
  d->settled = s->settled;
  if (sea) {  // (each chunk down to the bed at its sinking speed from where it meets the water)
    d->splashed.assign(s->tracks.size(), 0);
    for (const Track& t : s->tracks) {
      const float* k = &t.keys[std::min<size_t>(t.touches, t.keys.size() / 7 - 1) * 7];
      const Vec3 flat = Rotate(d->heading, Vec3{k[0], 0, k[2] * d->stretch} * d->scale);
      const float depth = waterLevel - (groundHeight ? groundHeight(d->at.x + flat.x, d->at.z + flat.z) : bed);
      d->settled = std::max(d->settled, static_cast<float>(t.touches) / s->fps + 0.3f + std::max(depth, 0.0f) / s->sink + 0.5f);
    }
  }
  const Vec3 extent = (unit->boundsMax - unit->boundsMin) * unit->scale;
  d->radius = 0.5f * std::max({extent.x, extent.y, extent.z, 0.2f});
  d->color = color * paint;
  d->born = d->heatFrom = clock;
  d->id = serial_++;
  d->bones.resize(s->tracks.size());
  d->trails.assign(s->tracks.size(), 0);  // [debrispile]
  d->centre = d->at;
  if (played == 0) printf("render: %s: its baked debris (%s) first played at %.3f s\n", Lower(unitId).c_str(), style.c_str(), clock);
  deaths_.push_back(std::move(d));
  ++played;
  return true;
}

int BakedDebris::Playing() const {
  int n = 0;
  for (const auto& d : deaths_) n += clock_ < 0 || clock_ - d->born < std::max(d->settled, d->style->settled) ? 1 : 0;
  return n;
}

void BakedDebris::Clear() {
  for (auto& d : deaths_) Drop(*d);  // [debrispile]
  deaths_.clear();
  wrecks_.clear();  // [debrispile]
  clock_ = -1;
}

// [debrispile] (baked_debris.h)
bool BakedDebris::HasPile(const std::string& unitId) {
  Set* set = SetOf(unitId);
  return set && PileStyle(*set, unitId);
}

const BakedDebris::Style* BakedDebris::PileStyle(Set& set, const std::string& unitId) {
  for (const char* style : {"break", "melt", "vaporise"})
    if (const Style* s = StyleOf(set, unitId, style)) return s->cls == 2 ? nullptr : s;  // ([debrisairsea] a ship's wreck: its own)
  return nullptr;
}

void BakedDebris::Drop(Death& d) {
  for (uint32_t& t : d.trails)
    if (t && smoke) smoke->Stop(t), t = 0;
}

void BakedDebris::Wreck(uint32_t prop, const std::string& unitId, Vec3 at, const Quat& turn, float scale, double born) {
  Standing& w = wrecks_[prop];
  const std::string id = Lower(unitId);
  if (w.unit != id) {  // (new, or the game gave its number to another)
    w = Standing{};
    w.unit = id;
    w.set = SetOf(id);
  }
  w.at = at;
  w.turn = turn;
  w.scale = scale;
  w.born = born;
  w.frame = frame_;
}

int BakedDebris::Piles() const {
  int n = 0;
  for (const auto& d : deaths_) n += d->prop ? 1 : 0;
  return n;
}

// [debrispile] The wrecks told since the last Update matched to their deaths (baked_debris.h); those not told any more gone, and
// their piles with them.
void BakedDebris::MatchWrecks(double clock) {
  for (auto it = wrecks_.begin(); it != wrecks_.end();) it = it->second.frame != frame_ || !it->second.set ? wrecks_.erase(it) : std::next(it);
  deaths_.erase(std::remove_if(deaths_.begin(), deaths_.end(),
                               [&](const std::unique_ptr<Death>& d) {
                                 const bool gone = d->prop && !wrecks_.count(d->prop);
                                 if (gone) Drop(*d);
                                 return gone;
                               }),
                deaths_.end());
  if (wrecks_.empty()) return;
  auto apart = [](Vec3 a, Vec3 b) { return std::hypot(a.x - b.x, a.z - b.z); };
  auto reach = [](const Death& d) { return d.radius * 1.5f + 2.0f; };
  // a death thrown just now where a wreck was drawn without one a moment ago (the wreck came a beat before its death was
  // seen): the death takes the wreck over
  for (size_t i = 0; i < deaths_.size(); ++i) {
    Death& d = *deaths_[i];
    if (d.prop || d.unseen || clock - d.born > 1.5) continue;
    for (size_t k = 0; k < deaths_.size(); ++k) {
      Death& u = *deaths_[k];
      if (!u.unseen || u.set != d.set || clock - (u.born + u.style->settled + 1) > 3 || apart(u.at, d.at) > reach(d)) continue;
      d.prop = u.prop;
      --unseenPiles, ++matched;
      Drop(u);
      deaths_.erase(deaths_.begin() + static_cast<std::ptrdiff_t>(k));
      if (k < i) --i;
      break;
    }
  }
  std::unordered_map<uint32_t, bool> drawn;
  for (const auto& d : deaths_)
    if (d->prop) drawn[d->prop] = true;
  for (auto& [prop, w] : wrecks_) {
    if (drawn.count(prop)) continue;
    if (w.told < 0) w.told = clock;
    // its death, seen: of its kind, near it, played from ten seconds before it appeared to four after
    Death* best = nullptr;
    float nearest = 1e30f;
    for (auto& d : deaths_) {
      if (d->prop || d->unseen || d->set != w.set || d->born < w.born - 10 || d->born > w.born + 4) continue;
      const float away = apart(d->at, w.at);
      if (away <= reach(*d) && away < nearest) best = d.get(), nearest = away;
    }
    if (best) {
      best->prop = prop;
      best->sinkFrom = -1;
      ++matched;
      continue;
    }
    // (its death still to be thrown: it waits, a while at most)
    if (coming && clock - w.told < 8 && coming(w.unit, w.at)) continue;
    // no death seen: the break style's last frame (or the style it has), burnt through, at the wreck's place and heading
    const Style* s = PileStyle(*w.set, w.unit);
    if (!s) continue;
    auto d = std::make_unique<Death>();
    d->set = w.set;
    d->style = s;
    d->at = {w.at.x, groundHeight ? groundHeight(w.at.x, w.at.z) : w.at.y, w.at.z};
    const Vec3 forward = Rotate(w.turn, Vec3{0, 0, 1});
    const float yaw = std::atan2(forward.x, forward.z);
    d->heading = Quat{0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)};
    d->scale = w.scale;
    const Vec3 extent = (s->model->boundsMax - s->model->boundsMin) * w.scale;
    d->radius = 0.5f * std::max({extent.x, extent.y, extent.z, 0.2f});
    d->color = Vec3{1, 1, 1} * paint;
    d->born = clock - s->settled - 1;  // (at rest from the first)
    d->heatFrom = w.born;
    d->id = serial_++;
    d->bones.resize(s->tracks.size());
    d->trails.assign(s->tracks.size(), 0);
    d->centre = d->at;
    d->prop = prop;
    d->unseen = true;
    deaths_.push_back(std::move(d));
    ++unseenPiles;
  }
}

void BakedDebris::Update(double clock, Vec3 eye, MeshRenderer* meshes) {
  if (clock_ >= 0 && clock < clock_ - 2) {  // (a game started over, or played back from earlier: [debrispile] its wrecks told anew)
    for (auto& d : deaths_) Drop(*d);
    deaths_.clear();
  }
  clock_ = clock;
  MatchWrecks(clock);  // [debrispile]
  ++frame_;
  auto ground = [&](float x, float z) { return groundHeight ? groundHeight(x, z) : 0.0f; };
  const bool before = meshes ? meshes->addingScenery : false;
  if (meshes) meshes->addingScenery = true;  // (as the debris: scenery, pressing no grass flat)
  for (size_t k = 0; k < deaths_.size();) {
    Death& d = *deaths_[k];
    const Style& s = *d.style;
    const float age = static_cast<float>(clock - d.born);
    // lying still after the last chunk is down, then sinking away (each by its own height); sooner past the most lying.
    // [debrispile] Drawn as a wreck: lying for as long as the wreck stands (MatchWrecks takes it away with it)
    const bool wreck = d.prop != 0;
    const float settled = std::max(d.settled, s.settled);  // [debrisairsea] (later for what sinks in water)
    const double sinkFrom = d.sinkFrom >= 0 ? d.sinkFrom : d.born + settled + lieSeconds;
    const float sinkFor = d.sinkFrom >= 0 ? 8.0f : sinkSeconds;
    const float sunk = wreck ? 0.0f : std::clamp(static_cast<float>(clock - sinkFrom) / sinkFor, 0.0f, 1.0f);
    if (!wreck && clock - sinkFrom >= sinkFor) {
      Drop(d);
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
      Vec3 p = Lerp(Vec3{a[0], a[1], a[2]}, Vec3{b[0], b[1], b[2]}, f);
      p.z *= d.stretch;  // [debrisairsea] (an aircraft's throw along its way, by its speed)
      p = p * d.scale;
      Quat q = Nlerp(Quat{a[3], a[4], a[5], a[6]}, Quat{b[3], b[4], b[5], b[6]}, f);
      // ([debrispile] VAPORISE: a chunk thrown beyond the wreck lies and sinks away as a death's without one, and is gone)
      const float own = wreck && t.stray ? std::clamp(static_cast<float>(clock - (d.born + settled + lieSeconds)) / sinkSeconds, 0.0f, 1.0f) : sunk;
      float lift = 0;
      if (!d.sea) {
        const Vec3 flat = Rotate(d.heading, Vec3{p.x, 0, p.z});
        lift = ground(d.at.x + flat.x, d.at.z + flat.z) - d.at.y - own * (p.y + t.reach * d.scale);
      } else {
        // [debrisairsea] on water (its place the surface): its keys until it meets the water, then a splash, and it sinks,
        // drifting on a little as it slows, tilting, down to the bed, where it lies (and later sinks into it)
        const size_t cut = std::min<size_t>(t.touches, n - 1);
        float lying = 0;  // (how far it has turned level: it rests on its thickness then)
        if (frame > static_cast<float>(cut)) {
          const float tau = (frame - static_cast<float>(cut)) / s.fps;
          const float* k = &t.keys[cut * 7];
          const float* j = &t.keys[(cut > 0 ? cut - 1 : 0) * 7];
          Vec3 drift = (Vec3{k[0], 0, k[2] * d.stretch} - Vec3{j[0], 0, j[2] * d.stretch}) * (s.fps * d.scale);
          p = Vec3{k[0], k[1], k[2] * d.stretch} * d.scale + drift * ((1 - std::exp(-tau * 1.5f)) / 1.5f);
          p.y -= s.sink * std::max(tau - 0.3f, 0.0f);
          // (turning as it goes down until its longest way lies level - a crane's arm along the bed, not standing out of the
          // water - with a little rock of its own about that)
          const Quat at{k[3], k[4], k[5], k[6]};
          const Quat rest = s.model && t.bone < s.model->bones.size() ? s.model->bones[t.bone].rotation : Quat{};
          const Vec3 way = Rotate(Mul(at, Conjugate(rest)), t.along);
          const Vec3 level = std::hypot(way.x, way.z) > 1e-3f ? Vec3{way.x, 0, way.z} * (1 / std::hypot(way.x, way.z)) : Vec3{1, 0, 0};
          const Vec3 axis = Cross(way, level);
          const float turnBy = std::acos(std::clamp(Dot(way, level), -1.0f, 1.0f)) * std::clamp(tau / 2.5f, 0.0f, 1.0f);
          Quat r{};
          if (Length(axis) > 1e-4f) {
            const Vec3 u = axis * (std::sin(turnBy * 0.5f) / Length(axis));
            r = Quat{u.x, u.y, u.z, std::cos(turnBy * 0.5f)};
          }
          const float h = std::fmod(static_cast<float>(c) * 0.6180339f + static_cast<float>(d.id) * 0.137f, 1.0f);
          const float rock = std::sin(tau * 0.9f + h * 6.283f) * 0.12f * std::max(0.0f, 1 - tau / 8);
          const Quat wobble{level.x * std::sin(rock * 0.5f), 0, level.z * std::sin(rock * 0.5f), std::cos(rock * 0.5f)};
          q = Normalized(Mul(wobble, Mul(r, at)));
          lying = std::clamp(tau / 2.5f, 0.0f, 1.0f);
        }
        if (frame >= static_cast<float>(cut) && c < d.splashed.size() && !d.splashed[c]) {
          d.splashed[c] = 1;
          const Vec3 flat = Rotate(d.heading, Vec3{p.x, 0, p.z});
          if (splash && !d.unseen && age < settled) splash(Vec3{d.at.x + flat.x, d.at.y, d.at.z + flat.z}, std::max(t.reach * d.scale * 2, 0.2f));
        }
        const Vec3 flat = Rotate(d.heading, Vec3{p.x, 0, p.z});
        const float rests = (t.thin > 0 ? t.thin + (t.reach * 0.35f - t.thin) * (1 - lying) : t.reach * 0.35f) * d.scale;
        const float floor = ground(d.at.x + flat.x, d.at.z + flat.z) - d.at.y + rests;
        lift = (p.y < floor ? floor - p.y : 0.0f) - own * t.reach * d.scale * 1.2f;
      }
      const Vec3 placed{p.x, p.y + lift, p.z};
      FrameBone& bone = d.bones[c];
      bone.bone = t.bone;
      bone.position[0] = placed.x, bone.position[1] = placed.y, bone.position[2] = placed.z;
      bone.orientation[0] = q.x, bone.orientation[1] = q.y, bone.orientation[2] = q.z, bone.orientation[3] = q.w;
      bone.hidden = own >= 1 || (d.unseen && t.stray);  // [debrispile]
      at[c] = d.at + Rotate(d.heading, placed);
      sum += at[c];
      // [debrispile] (VAPORISE: the biggest thrown trail the debris's smoke while they fly)
      if (t.smokes && smoke && !trail.empty() && c < d.trails.size()) {
        if (!d.unseen && age >= 0 && age < t.rests) {
          if (!d.trails[c]) d.trails[c] = smoke->Start(trail, at[c], Quat{}, std::clamp(t.reach * d.scale / 0.15f, 0.5f, 1.5f), -1, true);
          else smoke->Move(d.trails[c], at[c], Quat{});
        } else if (d.trails[c]) {
          smoke->Stop(d.trails[c]);
          d.trails[c] = 0;
        }
      }
    }
    d.centre = sum * (1.0f / static_cast<float>(std::max<size_t>(at.size(), 1)));
    d.reach = 0;
    for (size_t c = 0; c < at.size(); ++c) d.reach = std::max(d.reach, Length(at[c] - d.centre) + s.tracks[c].reach * d.scale);
    ++k;
    if (!meshes || Length(d.centre - eye) - d.reach > 260) continue;
    Pose pose;
    pose.now = pose.next = d.bones.data();
    pose.nowCount = pose.nextCount = d.bones.size();
    // (hot: its age and how long it glows, as a wreck's; the shader lights the heat mask by them. [debrispile] And how far it
    // has cooled into its wreck's textures, in the tint's x: all of it by the time its chunks rest, eased in and out)
    const float heated = static_cast<float>(clock - d.heatFrom);
    // ([debrisairsea] on water its chunks are quenched as they go in: no glow past the first seconds)
    const float seconds = std::min(heat ? heat->WreckSeconds(d.radius) : 55.0f, d.sea ? 2.5f : 1e9f);
    const float cooled = d.unseen ? 1.0f : std::clamp(age / std::max(settled, 0.1f), 0.0f, 1.0f);
    const bool glows = hot && heated >= 0 && heated < seconds;
    // ([debrispile] MELT: how far its hot parts have sagged, eased over meltSeconds and then held; 101 times that in hundredths
    // over the burn's hundredths: the vertex shader)
    const float melted = s.melts && meltSag > 0 ? std::clamp(age / std::max(meltSeconds, 0.1f), 0.0f, 1.0f) : 0.0f;
    const float sag = std::round(std::clamp(meltSag, 0.0f, 1.0f) * melted * (2 - melted) * 100);
    const float burn = std::round(cooled * cooled * (3 - 2 * cooled) * 100);
    const Vec3 tint{burn + 101 * sag, glows ? -heated : 0.0f, glows ? -seconds : 0.0f};
    meshes->Add(s.model, Mat4::FromTRS(d.at, d.heading, d.scale), d.color, 1, &pose, tint.x > 0 || glows ? &tint : nullptr, nullptr,
                0x70000000u | (d.id & 0x0fffffffu));
    meshes->Reach(d.centre, d.reach);
  }
  if (meshes) meshes->addingScenery = before;
}

void BakedDebris::ForEachHot(const std::function<void(Vec3, float, float, float, uint32_t)>& f) const {
  if (!hot || clock_ < 0) return;
  for (const auto& d : deaths_) {
    const float age = static_cast<float>(clock_ - d->heatFrom);  // ([debrispile] heatFrom)
    const float seconds = std::min(heat ? heat->WreckSeconds(d->radius) : 55.0f, d->sea ? 2.5f : 1e9f);  // ([debrisairsea])
    if (age >= 0 && age < seconds) f(d->centre, d->radius, age, seconds, d->id);
  }
}

}  // namespace render
