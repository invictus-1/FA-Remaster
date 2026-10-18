#include "ripples.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "scmap.h"

namespace render {
namespace {

constexpr double kStep = 1.0 / 60;   // the field's step (seconds of the game's clock)
constexpr int kMostSteps = 3;        // steps in one frame at most (a slow frame or a fast game: the rest let go)
constexpr double kQuiet = 8;         // seconds with nothing put in, after which the field has died out (2^-10 at 0.8 s)
constexpr float kSmallest = 1.0f / 16, kLargest = 1.0f;  // a cell's size, world units
constexpr int kSponge = 48;          // cells along the square's edges where ripples are let die out

// How strongly what moves pushes the water (height put in per world unit it moves, by a bump half a unit across at
// its bow), and how high a splash of strength 1 stands at its deepest (world units).
constexpr float kHull = 0.6f;
constexpr float kSink = 0.25f;
constexpr float kSplash = 0.2f;

const char kShader[] = R"hlsl(
cbuffer Step : register(b1) {
  float4 cells;    // x: cells a side; y: a cell's size (world units); z, w: the first cell's corner (world x, z)
  int4 shift;      // x, y: where cell i is read in the field read, less i; z: bumps put in this step; w: 1 = the surface written
  float4 wave;     // x: k^2 (c dt / dx)^2 in deep water; y: the field kept a step (its die-out); z: 1 = the field read is empty; w: the water's level
  float4 mapInfo;  // x, y: the map's width and height; z: world units of the heights' largest value; w: the sponge's width in cells
  float4 from;     // (Carry) the field read: x a cell's size, y, z its first cell's corner
  float4 entries[256];  // the bumps: xy where (world), z radius (negative: with a hollow round it), w how much they raise the water at their middle
};
Texture2D<float> nowIn : register(t0);     // the heights now
Texture2D<float> beforeIn : register(t1);  // and a step before
Texture2D<float> heights : register(t2);   // the ground's (Terrain::HeightTexture), one a grid point
RWTexture2D<float> nowOut : register(u0);
RWTexture2D<float> beforeOut : register(u1);
RWTexture2D<float4> surface : register(u2);  // what the water reads: the height, its slope along x and z

float GroundAt(float2 xz) {
  float2 p = clamp(xz, 0, mapInfo.xy);
  float2 f = floor(p);
  float2 t = p - f;
  int2 c = int2(f);
  int2 most = int2(mapInfo.xy);
  float a = heights.Load(int3(min(c, most), 0)), b = heights.Load(int3(min(c + int2(1, 0), most), 0));
  float d = heights.Load(int3(min(c + int2(0, 1), most), 0)), e = heights.Load(int3(min(c + int2(1, 1), most), 0));
  return lerp(lerp(a, b, t.x), lerp(d, e, t.x), t.y) * mapInfo.z;
}

// (nothing outside the field read: calm water comes in at the edge the square moves toward)
float NowAt(int2 at) {
  int n = int(cells.x);
  return wave.z > 0.5 || any(at < 0) || any(at >= n) ? 0 : nowIn.Load(int3(at, 0));
}
float BeforeAt(int2 at) {
  int n = int(cells.x);
  return wave.z > 0.5 || any(at < 0) || any(at >= n) ? 0 : beforeIn.Load(int3(at, 0));
}

[numthreads(8, 8, 1)] void Step(uint3 id : SV_DispatchThreadID) {
  int2 i = int2(id.xy);
  int n = int(cells.x);
  if (any(i >= n)) return;
  int2 j = i + shift.xy;
  float2 world = cells.zw + (float2(i) + 0.5) * cells.y;
  float h = NowAt(j), before = BeforeAt(j);
  float l = NowAt(j - int2(1, 0)), r = NowAt(j + int2(1, 0)), d = NowAt(j - int2(0, 1)), u = NowAt(j + int2(0, 1));
  float ld = NowAt(j - int2(1, 1)), rd = NowAt(j + int2(1, -1)), lu = NowAt(j + int2(-1, 1)), ru = NowAt(j + int2(1, 1));
  // (the slopes over the eight round it, the corners at half weight: smoother than over the four alone)
  if (shift.w != 0) surface[i] = float4(h, (2 * (r - l) + (rd - ld) + (ru - lu)) / (8 * cells.y), (2 * (u - d) + (lu - ld) + (ru - rd)) / (8 * cells.y), 0);
  float depth = wave.w - GroundAt(world);
  if (depth <= 0) {  // (dry ground: the water stops there)
    nowOut[i] = 0;
    beforeOut[i] = 0;
    return;
  }
  // (shallow water: the waves slow down, and lose their strength as they come in to the shore; at the square's
  // edge they die out)
  float edge = float(min(min(i.x, i.y), n - 1 - max(i.x, i.y)));
  float sponge = saturate(edge / mapInfo.w);
  float kept = lerp(0.9, 1.0, saturate(depth / 0.4)) * lerp(0.85, 1.0, sponge);
  float k2 = wave.x * lerp(0.2, 1.0, saturate(depth / 0.8));
  // (the nine-point Laplacian, the corners a quarter of the sides: rings stay round where the four-point one squares
  // them at a few cells a wave)
  float laplacian = (4 * (l + r + d + u) + (ld + rd + lu + ru) - 20 * h) / 6;
  // (the whole field dies out, not only its motion: a broad rise left behind by what moved would otherwise stay)
  float next = (h + (h - before) * kept + k2 * laplacian) * wave.y * lerp(0.92, 1.0, sponge);
  for (int e = 0; e < shift.z; ++e) {
    float4 b = entries[e];
    float2 off = world - b.xy;
    float t = dot(off, off) / (b.z * b.z);
    if (t < 4) {
      // (a bump; with a negative radius less a bump twice as wide a quarter as high: as much water taken from round
      // it as it raises, so that what moves stirs ripples and does not heap the water up)
      float s = saturate(1 - t), w = 1 - t * 0.25;
      next += b.w * (s * s - (b.z < 0 ? 0.25 * w * w : 0));
    }
  }
  nowOut[i] = clamp(next, -0.6, 0.6);
  beforeOut[i] = h;
}

// The field carried over to cells of another size: each new cell from the old ones round its middle.
[numthreads(8, 8, 1)] void Carry(uint3 id : SV_DispatchThreadID) {
  int2 i = int2(id.xy);
  int n = int(cells.x);
  if (any(i >= n)) return;
  float2 world = cells.zw + (float2(i) + 0.5) * cells.y;
  float2 at = (world - from.yz) / from.x - 0.5;
  int2 c = int2(floor(at));
  float2 t = at - floor(at);
  float a = NowAt(c), b = NowAt(c + int2(1, 0)), d = NowAt(c + int2(0, 1)), e = NowAt(c + int2(1, 1));
  nowOut[i] = lerp(lerp(a, b, t.x), lerp(d, e, t.x), t.y);
  a = BeforeAt(c), b = BeforeAt(c + int2(1, 0)), d = BeforeAt(c + int2(0, 1)), e = BeforeAt(c + int2(1, 1));
  beforeOut[i] = lerp(lerp(a, b, t.x), lerp(d, e, t.x), t.y);
}
)hlsl";

struct StepConstants {
  float cells[4];
  int32_t shift[4];
  float wave[4];
  float mapInfo[4];
  float from[4];
  float entries[Ripples::kMostEntries][4];
};

}  // namespace

Ripples::~Ripples() {
  for (gfx::Texture t : {now_[0], now_[1], before_[0], before_[1], surface_})
    if (t) device_->Destroy(t);
}

void Ripples::SetMap(const MapData* map) {
  map_ = map;
  haveWater_ = map && map->water.present;
  level_ = haveWater_ ? map->water.elevation : 0;
  fresh_ = true;
  splashes_.clear();
  hulls_.clear();
  shots_.clear();
  entries_.clear();
}

bool Ripples::Ready() {
  if (tried_) return made_;
  tried_ = true;
  std::string error;
  gfx::ComputeDesc cd;
  cd.source = kShader;
  cd.name = "water ripples: step";
  cd.entry = "Step";
  step_ = device_->CreateCompute(cd, &error);
  if (step_) {
    cd.name = "water ripples: carry";
    cd.entry = "Carry";
    carry_ = device_->CreateCompute(cd, &error);
  }
  if (step_ && carry_) {
    gfx::TargetDesc td;
    td.storage = true;
    td.width = td.height = kCells;
    td.format = gfx::Format::R32F;
    for (int k = 0; k < 2; ++k) {
      now_[k] = device_->CreateTarget(td);
      before_[k] = device_->CreateTarget(td);
    }
    td.format = gfx::Format::RGBA16F;
    surface_ = device_->CreateTarget(td);
    made_ = now_[0] && now_[1] && before_[0] && before_[1] && surface_;
    if (!made_) error = "its storage targets cannot be made";
  }
  if (!made_) fprintf(stderr, "render: water ripples cannot be used (%s)\n", error.c_str());
  return made_;
}

void Ripples::Begin(double clock, bool on, bool side) {
  side_ = side;
  active_ = false;  // (until Update says otherwise)
  if (side) {  // (a picture for something else: the field and what is put in are kept for the game's next frame)
    stepsDue_ = 0;
    return;
  }
  if (!on || !haveWater_) {
    if (on_) {
      splashes_.clear();
      hulls_.clear();
      shots_.clear();
      entries_.clear();
    }
    on_ = false;
    fresh_ = true;
    stepsDue_ = 0;
    return;
  }
  if (!on_) {
    on_ = true;
    fresh_ = true;
    simClock_ = clock;
  }
  clock_ = clock;
  ++stamp_;
  // (the game's clock went back - a replay started over - or leapt far ahead: the water is calm again)
  if (clock < simClock_ - 0.25 || clock - simClock_ > 2) {
    fresh_ = true;
    simClock_ = clock;
    splashes_.clear();
    hulls_.clear();
    shots_.clear();
    entries_.clear();
  }
  int due = static_cast<int>(std::floor((clock - simClock_) / kStep + 1e-3));
  due = std::max(due, 0);
  simClock_ += due * kStep;
  if (due > kMostSteps) {
    due = kMostSteps;
    simClock_ = clock;
  }
  stepsDue_ = due;
  // (what has not been seen for a while is forgotten: ids are used again)
  if ((stamp_ & 63) == 0) {
    for (auto it = hulls_.begin(); it != hulls_.end();) it = stamp_ - it->second.stamp > 240 ? hulls_.erase(it) : std::next(it);
    for (auto it = shots_.begin(); it != shots_.end();) it = stamp_ - it->second.stamp > 240 ? shots_.erase(it) : std::next(it);
  }
}

void Ripples::Put(float x, float z, float radius, float amount) {
  if (amount == 0 || radius == 0 || entries_.size() >= 8192) return;  // (a host that never renders: not without end)
  entries_.push_back({x, z, radius, amount});
  lastPut_ = clock_;
}

void Ripples::Add(float x, float z, float radius, float strength) {
  if (!on_ || side_ || !haveWater_ || !map_ || strength <= 0 || radius <= 0) return;
  if (map_->Height(x, z) >= level_ - 0.02f) return;  // (no water there)
  if (splashes_.size() >= 512) return;
  radius = std::clamp(radius, 0.1f, 8.0f);
  // (its swing's period grows with its size, and with the cell: a ring is never finer than the field can hold)
  const float period = std::max(std::clamp(0.32f * std::sqrt(radius / 0.5f), 0.25f, 0.8f), 6 * std::max(cell_, kSmallest) / std::max(speed, 0.1f));
  splashes_.push_back({x, z, radius, std::min(strength, 8.0f), clock_, period});
  ++added_;
  lastPut_ = clock_;
}

// A splash's displacement of the water by `at`: down first (its crater), then up and down again, less each
// time, over a few swings.
float Ripples::Swing(const Splash& s, double at) const {
  const float age = static_cast<float>(at - s.born);
  if (age <= 0) return 0;
  // (a larger splash stands higher, by the root of its size: its slopes, which are what shows, grow less; never more
  // than half a unit, whatever a blast asks)
  const float height = std::min(kSplash * s.strength * std::sqrt(s.radius / 0.5f), 0.5f);
  return -height * std::sin(6.2831853f * age / s.period) * std::exp(-age / (s.period * 0.6f));
}

void Ripples::AddShot(uint32_t id, Vec3 p, bool stopped, float size) {
  if (!on_ || side_ || !haveWater_ || !map_ || !guessSplashes || impactsGiven) return;
  if (p.y > level_ + 8 || p.y < level_ - 8) return;  // (high in the air, or deep: nothing to do with the surface now)
  Shot& s = shots_[id];
  const bool known = s.stamp != 0;
  const float was = s.y;
  s.y = p.y;
  s.stamp = stamp_;
  if (s.done || map_->Height(p.x, p.z) >= level_ - 0.05f) return;
  // (it stopped at the water's surface this beat - the engine stops a shot where it meets the water - or it has
  // gone down through it since the last frame)
  const bool hit = stopped && std::fabs(p.y - level_) < 0.12f;
  const bool through = known && was > level_ + 0.05f && p.y < level_ - 0.05f;
  if (!hit && !through) return;
  s.done = true;
  Add(p.x, p.z, std::clamp(0.4f + size * 0.6f, 0.4f, 1.2f), 1.0f);
}

void Ripples::AddHull(uint32_t id, Vec3 position, const Quat& orientation, Vec3 lo, Vec3 hi) {
  if (!on_ || side_ || !haveWater_ || !map_) return;
  const float bottom = position.y + lo.y, top = position.y + hi.y;
  if (bottom > level_ + 6 || top < level_ - 2) return;  // (high above the water, or deep under it)
  if (map_->Height(position.x, position.z) >= level_ - 0.02f) return;  // (on dry land: most units, not looked up)
  Hull& h = hulls_[id];
  const bool known = h.clock >= 0 && clock_ >= h.clock && clock_ - h.clock < 1.0;
  h.stamp = stamp_;
  if (!known) {
    h.place = position;
    h.bottom = bottom;
    h.clock = clock_;
    return;
  }
  // (between the field's steps what it does is added up: it is looked at again at the next step)
  if (stepsDue_ == 0) return;
  const Vec3 moved = position - h.place;
  const float seconds = std::max(static_cast<float>(clock_ - h.clock), 1e-3f);
  const float wasBottom = h.bottom;
  h.place = position;
  h.bottom = bottom;
  h.clock = clock_;
  const Vec3 middle = position + Rotate(orientation, (lo + hi) * 0.5f);
  if (map_->Height(middle.x, middle.z) >= level_ - 0.02f) return;  // (no water under it)
  if (top < level_ - 0.02f) return;                                // (under the water: a submarine)
  const float halfX = (hi.x - lo.x) * 0.5f, halfZ = (hi.z - lo.z) * 0.5f;
  // come down into the water from above (an aircraft falling, what a death throws out): a splash as large as it
  if (guessSplashes && !impactsGiven && wasBottom > level_ + 0.05f && bottom <= level_ + 0.05f) {
    const float fall = (wasBottom - bottom) / seconds;
    if (fall > 1) Add(middle.x, middle.z, std::clamp(std::max(halfX, halfZ), 0.5f, 3.0f), std::clamp(fall / 6, 0.5f, 4.0f));
  }
  // how much of it is in the water: a hull through the surface (a ship; a unit wading, by how deep), or over it
  // (a hover unit's skirt, less the higher it is)
  float immersion = 0;
  if (bottom <= level_) immersion = std::clamp((level_ - bottom) / 0.5f, 0.25f, 1.0f);
  else if (bottom < level_ + 0.6f) immersion = 0.55f * (1 - (bottom - level_) / 0.6f);
  if (immersion <= 0) return;
  // moving: its front raises the water ahead of it, its back leaves a hollow (bumps along its front and back, a
  // few across a wide hull, each about half a unit, so that it leaves fine ripples and not one broad swell)
  const float d = std::sqrt(moved.x * moved.x + moved.z * moved.z);
  if (d > 0.002f && d < 4.0f) {
    const Vec3 m{moved.x / d, 0, moved.z / d};
    Vec3 forward = Rotate(orientation, {0, 0, 1});
    forward.y = 0;
    forward = Length(forward) > 1e-3f ? Normalize(forward) : m;
    const float c = std::fabs(Dot(m, forward)), s = std::sqrt(std::max(0.0f, 1 - c * c));
    // (its reach along the way it moves and its half width across it, of the ellipse round its bounds)
    const float along = std::sqrt(halfZ * halfZ * c * c + halfX * halfX * s * s), across = std::sqrt(halfX * halfX * c * c + halfZ * halfZ * s * s);
    const Vec3 side{m.z, 0, -m.x};
    const int n = std::clamp(static_cast<int>(std::lround(across / 0.45f)), 1, 3);
    const float r = std::clamp(across / static_cast<float>(n) * 0.9f, 0.25f, 0.5f);
    // (by the bump's area: a small hull stirs small ripples, a wide one's bumps each a full share)
    const float amount = kHull * d * immersion * (r / 0.5f) * (r / 0.5f);
    for (int k = 0; k < n; ++k) {
      const float w = n == 1 ? 0.0f : 2.0f * static_cast<float>(k) / static_cast<float>(n - 1) - 1;  // -1 .. 1 across it
      const float out = across * 0.6f * w, ahead = along * 0.85f * (1 - 0.3f * w * w);  // (the shoulders behind the bow)
      Put(middle.x + m.x * ahead + side.x * out, middle.z + m.z * ahead + side.z * out, -r, amount);
      Put(middle.x - m.x * ahead + side.x * out, middle.z - m.z * ahead + side.z * out, -r, -amount);
    }
  }
  // going down through the surface (a ship sinking): the water closes over it, in rings
  const float dy = bottom - wasBottom;
  if (bottom < level_ && dy < -0.002f && dy > -2) Put(middle.x, middle.z, std::clamp(std::max(halfX, halfZ) * 0.8f, 0.4f, 2.0f), kSink * dy);
}

void Ripples::Update(const Camera& camera, int viewHeight, gfx::Texture heights, float heightUnit) {
  active_ = false;
  if (!on_ || side_ || !map_) return;
  // nothing put in for a while: the field has died out, and the water is drawn as without it
  if (entries_.empty() && splashes_.empty() && clock_ - lastPut_ > kQuiet) {
    fresh_ = true;
    return;
  }
  if (!heights || !Ready()) return;
  if (stepsDue_ <= 0) {  // (no step due this frame: the last step's field is what is shown)
    active_ = !fresh_;
    return;
  }
  // a cell's size by the camera's distance: two to three pixels where it looks
  const float pixel = 2 * camera.distance * std::tan(camera.fovY * 0.5f) / static_cast<float>(std::max(viewHeight, 1));
  const float want = pixel * 2.4f;
  float cell = cell_ > 0 ? cell_ : kSmallest;
  while (want > cell * 1.3f && cell < kLargest) cell *= 2;
  while (want < cell * 0.55f && cell > kSmallest) cell *= 0.5f;
  // the square round what the camera looks at, in whole cells
  const int ox = static_cast<int>(std::floor(camera.target.x / cell)) - kCells / 2, oz = static_cast<int>(std::floor(camera.target.z / cell)) - kCells / 2;
  const float x0 = static_cast<float>(ox) * cell, z0 = static_cast<float>(oz) * cell, side = static_cast<float>(kCells) * cell;

  static thread_local StepConstants k;  // (4 KB: not on the stack; one per thread, for worlds drawn on different threads)
  std::memset(&k, 0, sizeof k);
  const float cells[4] = {static_cast<float>(kCells), cell, x0, z0};
  std::memcpy(k.cells, cells, sizeof cells);
  const float waves = speed * static_cast<float>(kStep) / cell;
  k.wave[0] = std::min(waves * waves, 0.45f);  // (the scheme holds below 0.5)
  k.wave[1] = std::exp(-0.6931472f * static_cast<float>(kStep) / std::max(halfLife, 0.05f));
  k.wave[3] = level_;
  k.mapInfo[0] = static_cast<float>(map_->width);
  k.mapInfo[1] = static_cast<float>(map_->height);
  k.mapInfo[2] = heightUnit;
  k.mapInfo[3] = static_cast<float>(kSponge);
  device_->SetTexture(2, heights);
  const bool fresh = fresh_;
  bool carried = false;
  if (!fresh && cell != cell_) {  // (to cells of another size: the field carried over)
    k.from[0] = cell_;
    k.from[1] = static_cast<float>(originX_) * cell_;
    k.from[2] = static_cast<float>(originZ_) * cell_;
    device_->SetPipeline(carry_);
    device_->SetConstants(1, &k, static_cast<uint32_t>(offsetof(StepConstants, entries)));
    device_->SetTexture(0, now_[cur_]);
    device_->SetTexture(1, before_[cur_]);
    device_->SetStorage(0, now_[1 - cur_]);
    device_->SetStorage(1, before_[1 - cur_]);
    device_->Dispatch(kCells / 8, kCells / 8);
    cur_ = 1 - cur_;
    carried = true;
  }
  // the splashes' swings since the last step, and those that are over let go
  for (Splash& s : splashes_) {
    const float v = Swing(s, clock_);
    Put(s.x, s.z, s.radius, v - s.last);
    s.last = v;
  }
  splashes_.erase(std::remove_if(splashes_.begin(), splashes_.end(), [this](const Splash& s) { return clock_ - s.born > s.period * 0.6f * 6; }), splashes_.end());
  // the bumps in the square, each at least as large as the field can hold (its water kept), the nearest first
  // when there are more than a step takes
  std::vector<Entry> use;
  use.reserve(entries_.size());
  for (const Entry& e : entries_) {
    const float reach = std::fabs(e.radius) * 2;
    if (e.x + reach < x0 || e.z + reach < z0 || e.x - reach > x0 + side || e.z - reach > z0 + side) continue;
    const float r = std::max(std::fabs(e.radius), 1.6f * cell);
    use.push_back({e.x, e.z, e.radius < 0 ? -r : r, e.amount * (e.radius / r) * (e.radius / r)});
  }
  entries_.clear();
  if (use.size() > static_cast<size_t>(kMostEntries)) {
    const Vec3 t = camera.target;
    std::nth_element(use.begin(), use.begin() + kMostEntries, use.end(), [t](const Entry& a, const Entry& b) {
      return (a.x - t.x) * (a.x - t.x) + (a.z - t.z) * (a.z - t.z) < (b.x - t.x) * (b.x - t.x) + (b.z - t.z) * (b.z - t.z);
    });
    use.resize(kMostEntries);
  }
  lastEntries_ = static_cast<int>(use.size());
  for (size_t i = 0; i < use.size(); ++i) {
    const float b[4] = {use[i].x, use[i].z, use[i].radius, use[i].amount};
    std::memcpy(k.entries[i], b, sizeof b);
  }
  // the steps (the square moved with the first, what was put in added with it)
  device_->SetPipeline(step_);
  for (int s = 0; s < stepsDue_; ++s) {
    const bool first = s == 0;
    k.shift[0] = first && !carried && !fresh ? ox - originX_ : 0;
    k.shift[1] = first && !carried && !fresh ? oz - originZ_ : 0;
    k.shift[2] = first ? static_cast<int32_t>(use.size()) : 0;
    k.shift[3] = s == stepsDue_ - 1 ? 1 : 0;
    k.wave[2] = first && fresh ? 1.0f : 0.0f;
    device_->SetConstants(1, &k, static_cast<uint32_t>(offsetof(StepConstants, entries) + (first ? use.size() : 0) * sizeof k.entries[0]));
    device_->SetTexture(0, now_[cur_]);
    device_->SetTexture(1, before_[cur_]);
    device_->SetStorage(0, now_[1 - cur_]);
    device_->SetStorage(1, before_[1 - cur_]);
    device_->SetStorage(2, surface_);
    device_->Dispatch(kCells / 8, kCells / 8);
    cur_ = 1 - cur_;
  }
  for (int slot = 0; slot < 3; ++slot) {
    device_->SetStorage(slot, {});
    device_->SetTexture(slot, {});
  }
  originX_ = ox;
  originZ_ = oz;
  cell_ = cell;
  fresh_ = false;
  area_[0] = x0;
  area_[1] = z0;
  area_[2] = 1 / side;
  area_[3] = cell;
  active_ = true;
}

}  // namespace render
