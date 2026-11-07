#include "rock_hits.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace render {

bool RockHits::IsRock(const std::string& blueprint, const Model* model) {
  if (!model || !model->ok || model->lods.empty() || model->lods[0].sway > 0 || model->lods[0].stream) return false;
  // (by the file's own name: "/env/stockrocks/" holds rocks only, but other folders mix them with trees)
  const size_t slash = blueprint.find_last_of("/\\");
  std::string name = slash == std::string::npos ? blueprint : blueprint.substr(slash + 1);
  for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (const char* word : {"rock", "boulder", "stone", "crag", "cliff", "slab", "pebble"})
    if (name.find(word) != std::string::npos) return true;
  return false;
}

void RockHits::Begin(size_t props) {
  rocks_.clear();
  cells_.clear();
  cellsX_ = cellsZ_ = 0;
  largest_ = 0;
  made_ = false;
  held_.assign(props, 0);
  heldList_.clear();
  scars_.clear();
  clock_ = chipClock_ = dustClock_ = -1;
  chipsLeft_ = dustLeft_ = 0;
  hits = touched = 0;
}

void RockHits::Add(uint32_t prop, const Model* model, const Mat4& world) {
  Rock r;
  r.prop = prop;
  r.model = model;
  r.world = world;
  r.toModel = Inverse(world);
  r.scale = 0;
  for (int k = 0; k < 3; ++k) r.scale = std::max(r.scale, Length(Vec3{world.m[0][k], world.m[1][k], world.m[2][k]}));
  const Vec3 mid = (model->boundsMin + model->boundsMax) * 0.5f;
  const Vec4 c = world * Vec4{mid.x, mid.y, mid.z, 1};
  r.centre = {c.x, c.y, c.z};
  r.radius = Length(model->boundsMax - model->boundsMin) * 0.5f * r.scale;
  largest_ = std::max(largest_, r.radius);
  rocks_.push_back(r);
}

void RockHits::Finish(float mapWidth, float mapHeight) {
  cellsX_ = std::max(1, static_cast<int>(std::ceil(mapWidth / kCell)));
  cellsZ_ = std::max(1, static_cast<int>(std::ceil(mapHeight / kCell)));
  cells_.assign(static_cast<size_t>(cellsX_) * static_cast<size_t>(cellsZ_), {});
  for (size_t i = 0; i < rocks_.size(); ++i) {
    const Rock& r = rocks_[i];
    const int x0 = std::clamp(static_cast<int>((r.centre.x - r.radius) / kCell), 0, cellsX_ - 1);
    const int x1 = std::clamp(static_cast<int>((r.centre.x + r.radius) / kCell), 0, cellsX_ - 1);
    const int z0 = std::clamp(static_cast<int>((r.centre.z - r.radius) / kCell), 0, cellsZ_ - 1);
    const int z1 = std::clamp(static_cast<int>((r.centre.z + r.radius) / kCell), 0, cellsZ_ - 1);
    for (int z = z0; z <= z1; ++z)
      for (int x = x0; x <= x1; ++x) cells_[static_cast<size_t>(z) * static_cast<size_t>(cellsX_) + static_cast<size_t>(x)].push_back(static_cast<uint32_t>(i));
  }
  made_ = true;
}

void RockHits::Forget() {
  std::fill(held_.begin(), held_.end(), 0);
  heldList_.clear();
  scars_.clear();
}

int RockHits::Hit(Vec3 at, float size, double clock, std::vector<Touched>* out, Vec3 way) {
  if (!made_ || cells_.empty()) return 0;
  ++hits;
  size = std::clamp(size, 0.05f, 12.0f);
  const float reach = size + 0.3f;  // (a hit lands on a rock's surface, which its box only roughly follows)
  // (the squares round it: each rock once, though it may be listed in several)
  const int cx = static_cast<int>(at.x / kCell), cz = static_cast<int>(at.z / kCell);
  const int span = static_cast<int>(std::ceil(reach / kCell));
  std::vector<uint32_t> seen;
  int n = 0;
  for (int z = std::max(cz - span, 0); z <= std::min(cz + span, cellsZ_ - 1); ++z)
    for (int x = std::max(cx - span, 0); x <= std::min(cx + span, cellsX_ - 1); ++x)
      for (uint32_t k : cells_[static_cast<size_t>(z) * static_cast<size_t>(cellsX_) + static_cast<size_t>(x)]) {
        const Rock& r = rocks_[k];
        if (Length(r.centre - at) > r.radius + reach) continue;
        if (std::find(seen.begin(), seen.end(), k) != seen.end()) continue;
        seen.push_back(k);
        // where on its box (its own units) the hit is nearest, and how far that is in the world
        const Vec4 l = r.toModel * Vec4{at.x, at.y, at.z, 1};
        const Vec3 lo = r.model->boundsMin, hi = r.model->boundsMax;
        const Vec3 q{std::clamp(l.x, lo.x, hi.x), std::clamp(l.y, lo.y, hi.y), std::clamp(l.z, lo.z, hi.z)};
        const float away = Length(Vec3{l.x - q.x, l.y - q.y, l.z - q.z}) * r.scale;
        if (away > reach) continue;
        const Vec4 w = r.world * Vec4{q.x, q.y, q.z, 1};
        Touched t;
        t.prop = r.prop;
        t.model = r.model;
        t.at = {w.x, w.y, w.z};
        // out: from where it was struck toward the hit (a blast beside it), else back against the shot's way, else out from its
        // middle; and up, never down into the ground
        Vec3 o = at - t.at;
        if (Length(o) < 0.05f) o = Length(way) > 0.5f ? way * -1.0f : t.at - r.centre;
        if (Length(o) < 1e-3f) o = {0, 1, 0};
        o = Normalize(o);
        o.y = std::max(o.y, 0.25f);
        t.out = Normalize(o);
        t.strength = std::clamp(1 - away / reach, 0.0f, 1.0f);
        t.strength = std::sqrt(t.strength) * 0.75f + 0.25f;  // (what it touches at all it chips a little)
        t.across = std::max(r.radius * 2, 0.1f);
        // its scars: soot and cracks by the hit's size and how near it was; a fresh coat of rock dust
        const float k2 = t.strength * std::clamp(size, 0.15f, 3.0f);
        auto scar = scars_.find(r.prop);
        if (scar == scars_.end() && r.radius <= scarsUpTo && static_cast<int>(scars_.size()) < mostScarred) {
          scar = scars_.emplace(r.prop, Scar{}).first;
          if (r.prop < held_.size()) held_[r.prop] = 1;
          heldList_.push_back(r.prop);
          t.fresh = true;
        }
        if (scar != scars_.end()) {
          Scar& s = scar->second;
          s.soot = std::min(1.0f, s.soot + 0.16f * k2);
          s.worst = std::max(s.worst, s.soot);
          s.crack = std::min(1.0f, s.crack + 0.1f * k2 + 0.01f);
          s.dust = std::max(s.dust, 0.45f + 0.55f * t.strength);
          // (the side it was struck on, in its own frame: the hits' ways out, each by its weight)
          const Vec4 m = r.toModel * Vec4{t.out.x, t.out.y, t.out.z, 0};
          const Vec3 side{m.x, m.y, m.z};
          if (Length(side) > 1e-6f) s.side += Normalize(side) * k2;
        }
        ++touched;
        ++n;
        if (out) out->push_back(t);
      }
  (void)clock;
  return n;
}

int RockHits::TakeChips(int want, double clock) {
  if (chipClock_ < 0 || clock < chipClock_) chipsLeft_ = 2 * chipsPerSecond;
  else chipsLeft_ = std::min(2 * chipsPerSecond, chipsLeft_ + static_cast<float>(clock - chipClock_) * chipsPerSecond);
  chipClock_ = clock;
  const int give = std::clamp(static_cast<int>(chipsLeft_), 0, std::max(want, 0));
  chipsLeft_ -= static_cast<float>(give);
  return give;
}

bool RockHits::TakeDust(double clock) {
  if (dustClock_ < 0 || clock < dustClock_) dustLeft_ = 2 * dustPerSecond;
  else dustLeft_ = std::min(2 * dustPerSecond, dustLeft_ + static_cast<float>(clock - dustClock_) * dustPerSecond);
  dustClock_ = clock;
  if (dustLeft_ < 1) return false;
  dustLeft_ -= 1;
  return true;
}

void RockHits::Update(double clock) {
  const float dt = clock_ < 0 ? 0.0f : static_cast<float>(std::clamp(clock - clock_, 0.0, 1.0));
  clock_ = clock;
  if (dt <= 0) return;
  const float sootEase = std::exp(-dt / std::max(sootEaseSeconds, 1.0f)), dustEase = std::exp(-dt / std::max(dustSeconds, 0.1f));
  for (auto& [prop, s] : scars_) {
    const float kept = s.worst * sootKept;
    if (s.soot > kept) s.soot = kept + (s.soot - kept) * sootEase;
    s.dust *= dustEase;
  }
}

Vec3 RockHits::Tint(uint32_t prop) const {
  const auto s = scars_.find(prop);
  if (s == scars_.end()) return {-0.35f, 0, -1.0f / 65536};
  auto byte = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
  // (its struck side's way about the upright, 0..255 for a whole turn, and how far up, 0..255 for -1..1)
  Vec3 side = Length(s->second.side) > 1e-6f ? Normalize(s->second.side) : Vec3{0, 1, 0};
  const float around = std::atan2(side.z, side.x) * (1 / 6.2831853f);
  const int turn = static_cast<int>(std::lround((around - std::floor(around)) * 256)) & 255;
  return {-0.35f - 0.1f * std::clamp(s->second.dust, 0.0f, 1.0f),
          -0.49f * static_cast<float>(byte(s->second.soot) * 256 + byte(s->second.crack)) / 65535,
          -static_cast<float>(1 + turn * 256 + byte(side.y * 0.5f + 0.5f)) / 65536};
}

}  // namespace render
