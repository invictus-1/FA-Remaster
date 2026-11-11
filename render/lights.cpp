#include "lights.h"
#include "marks.h"

#include <algorithm>
#include <array>
#include <chrono>  // [lightshade]
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "core/vfs.h"
#include "dds.h"
#include "scmap.h"  // [lightshade] (the map's heights)
#include "volfx_log.h"  // (MOHO64_DEBUG_VOLFX)

namespace render {
namespace {

// (unitsfx) The squares' size: kSmallestCell times a power of the square root of two, the smallest that keeps the grid
// within kMaxCellsAcross each way (before: 96 across whatever the lights spread over, which made squares of nine to ten
// world units on a whole map's lights, more than 48 lights in a square over a lit base at night, the farthest left out
// and the square dark; and the squares moved every frame with the lights at the map's far ends, so the dark squares
// jumped about - Chris's black squares over building units at night, 2026-10-20).
constexpr int kMaxCellsAcross = 256;  // the grid is at most this many squares each way
constexpr float kSmallestCell = 3;    // world units (about a unit's lamp's reach)
constexpr int kMostInSquare = 48;     // the lights a pixel looks at, at most (lighting.h, and the shaders reading the grid)
// [lightshade] A ground table's squares each way by its light's reach (a square kShadeCell world units wide or less
// while 128 are enough) and its rows in the buffer (a row that says where it lies, then two heights a square, four
// numbers a row); how far its light may move, in squares, before it is made anew; how far over its place a light is
// taken to stand at least (lighting.h's kRayGroundRise: the rays raise their ways as much); and how far apart, at
// most, the ground is read along the way from a square to the next (world units; four readings at most).
constexpr float kShadeCell = 4;
constexpr int ShadeCells(float radius) { return 2 * radius <= 32 * kShadeCell ? 32 : 2 * radius <= 64 * kShadeCell ? 64 : 128; }
constexpr size_t ShadeRows(int cells) { return 1 + static_cast<size_t>(cells) * static_cast<size_t>(cells) / 2; }
constexpr float kShadeKept = 0.125f;
constexpr float kShadeRise = 0.35f;
constexpr float kShadeStep = 1.5f;

}  // namespace

Lights::Lights(gfx::Device* device, const moho::Vfs* vfs) : device_(device), vfs_(vfs) {
  gfx::BufferDesc bd;
  bd.kind = gfx::BufferKind::Shader;
  bd.stride = 16;
  bd.dynamic = true;
  buffer_ = device_->CreateBuffer(bd);
  gfx::TargetDesc td;
  td.width = td.height = kLampShadowSize;
  td.layers = kLampShadows;
  td.format = gfx::Format::D32;
  shadowMap_ = device_->CreateTarget(td);
}

gfx::PassDesc Lights::ShadowPass(int i) const {
  gfx::PassDesc p;
  p.depth = shadowMap_;
  p.depthLayer = i;
  return p;
}

int Lights::PlaceShadows(const Camera& camera, const View& view) {
  shadowCount_ = 0;
  // the beams near enough to what the camera looks at to be seen, nearest first
  struct Near {
    float away;
    size_t light;
  };
  std::vector<Near> near;
  for (size_t n = 0; n < lights_.size(); ++n) {
    const Light& l = lights_[n];
    if (l.cosEdge < -1.5f || l.radius < 1.5f || l.color.x + l.color.y + l.color.z < 0.02f) continue;
    // (over the ground: a host's camera target need not lie at the ground's height)
    float away = std::hypot(l.position.x - camera.target.x, l.position.z - camera.target.z);
    if (away - l.radius > camera.distance * 1.3f) continue;
    // [nightlight] (steady: one that had a map the frame before counts a quarter nearer)
    if (steady && l.key && std::find(shadowKeys_.begin(), shadowKeys_.end(), l.key) != shadowKeys_.end()) away *= 0.75f;
    near.push_back({away, n});
  }
  const size_t count = std::min(near.size(), static_cast<size_t>(kLampShadows));
  std::partial_sort(near.begin(), near.begin() + static_cast<std::ptrdiff_t>(count), near.end(), [](const Near& a, const Near& b) { return a.away < b.away; });
  {  // [nightlight] (the beams that have one now, for the next frame; and how many had none before)
    shadowSwaps = 0;
    std::vector<uint64_t> now;
    for (size_t k = 0; k < count; ++k) {
      const uint64_t key = lights_[near[k].light].key;
      if (!key) continue;
      now.push_back(key);
      if (std::find(shadowKeys_.begin(), shadowKeys_.end(), key) == shadowKeys_.end()) ++shadowSwaps;
    }
    shadowKeys_.swap(now);
  }
  for (size_t k = 0; k < count; ++k) {
    Light& l = lights_[near[k].light];
    l.shadow = static_cast<int>(k);
    // the lamp's eye: it looks along its beam, as wide as the beam opens (and a little more, for
    // the lookups at the map's edge)
    const Vec3 back = l.direction * -1.0f;
    const Vec3 reference = std::fabs(back.y) > 0.99f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    const Vec3 right = Normalize(Cross(reference, back));
    const Vec3 up = Cross(back, right);
    const float opening = std::min(2 * std::acos(std::clamp(l.cosEdge, -1.0f, 1.0f)) + 0.1f, 2.6f);
    const Mat4 lampViewProj = Mat4::Perspective(opening, 1, 0.05f) * Mat4::View(l.position, right, up, back);
    View& v = shadowViews_[k];
    v = view;
    v.SetMatrix(lampViewProj);
    v.constants.shadowsOn = 0;
    v.coveredHalf = 0;
    v.reachFrom = l.position;
    v.reach = l.radius;
    v.carrier = l.carrier;
    v.hasCarrier = l.hasCarrier;
    v.constants.viewport[0] = v.constants.viewport[1] = static_cast<float>(kLampShadowSize);
    v.constants.viewport[2] = v.constants.viewport[3] = 1.0f / static_cast<float>(kLampShadowSize);
    // from the projection's -w..w (y up) to the map's 0..w (y down)
    const Mat4 toMap = {{{0.5f, 0, 0, 0.5f}, {0, -0.5f, 0, 0.5f}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
    shadowMatrix_[k] = toMap * lampViewProj;
  }
  shadowCount_ = static_cast<int>(count);
  return shadowCount_;
}

void Lights::Begin(double now) {
  now_ = now;
  lights_.clear();
  hulls_.clear();  // [hullshade]
}

bool Lights::LargeReach(float* x0, float* z0, float* x1, float* z1) const {  // [hullshade]
  *x0 = *z0 = 1e30f;
  *x1 = *z1 = -1e30f;
  auto take = [&](Vec3 at, float radius) {
    *x0 = std::min(*x0, at.x - radius);
    *z0 = std::min(*z0, at.z - radius);
    *x1 = std::max(*x1, at.x + radius);
    *z1 = std::max(*z1, at.z + radius);
  };
  for (const Light& l : lights_)
    if (l.cosEdge < -1.5f && l.radius >= shadeFrom) take(l.position, l.radius);
  for (const FlashLight& f : flashes_)
    if (f.size * reach >= shadeFrom && now_ >= f.born && now_ - f.born < static_cast<double>(f.seconds)) take(f.position, f.size * reach);
  return *x1 >= *x0;
}

// (a light with a place, reach or colour that is not a number is none: one such light, kept by the bounce light's and
// the air's histories, would turn the picture black for good)
static bool Finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

void Lights::Add(Vec3 position, float radius, Vec3 color, const Vec3* origin, float air) {
  if (!(radius > 0) || !std::isfinite(radius) || !Finite(position) || !Finite(color)) return;
  lights_.push_back(Light{position, radius, color, 1, {}, -2, {}});
  lights_.back().air = air;
  if (origin) {
    lights_.back().origin = *origin;
    lights_.back().hasOrigin = true;
  }
}

Vec3 Lights::RampGlow(const std::string& ramp) {
  const std::vector<Vec3>& colors = *Ramp(ramp);
  Vec3 best{0, 0, 0};
  for (const Vec3& c : colors)
    if (c.x + c.y + c.z > best.x + best.y + best.z) best = c;
  return best;
}

void Lights::AddBeam(Vec3 position, float radius, Vec3 color, Vec3 direction, float coneDegrees, float glow, const Vec3* carrier, const Vec3* origin,
                     uint64_t key) {
  if (!(radius > 0) || !std::isfinite(radius) || !Finite(position) || !Finite(color) || !Finite(direction)) return;
  Light light{position, radius, color, 1, {}, -2, {}};
  light.glow = glow;
  light.key = key;
  if (carrier) {
    light.carrier = *carrier;
    light.hasCarrier = true;
  }
  if (origin) {
    light.origin = *origin;
    light.hasOrigin = true;
  }
  light.direction = Normalize(direction);
  light.cosEdge = std::cos(std::clamp(coneDegrees, 1.0f, 179.0f) * 0.5f * 3.14159265f / 180);
  lights_.push_back(light);
}

// A ramp texture's colours along its length (its middle row), each multiplied by its alpha: what a
// light particle's glow is multiplied with over its life.
const std::vector<Vec3>* Lights::Ramp(const std::string& path) {
  auto known = ramps_.find(path);
  if (known != ramps_.end()) return &known->second;
  std::vector<Vec3>& ramp = ramps_[path];
  DdsImage image;
  std::vector<uint8_t> pixels;
  std::string error;
  auto file = vfs_->ReadFile(path);
  if (file && ParseDds(*file, &image, &error) && DecodeRgba(image, 0, &pixels) && image.width > 0) {
    const size_t row = static_cast<size_t>(image.height / 2) * static_cast<size_t>(image.width) * 4;
    for (int x = 0; x < image.width; ++x) {
      const uint8_t* p = &pixels[row + static_cast<size_t>(x) * 4];
      const float a = static_cast<float>(p[3]) / 255.0f;
      // (the texture's colours are what a screen shows; lights are counted in linear light)
      ramp.push_back({std::pow(static_cast<float>(p[0]) / 255.0f, 2.2f) * a, std::pow(static_cast<float>(p[1]) / 255.0f, 2.2f) * a,
                      std::pow(static_cast<float>(p[2]) / 255.0f, 2.2f) * a});
    }
  }
  if (ramp.empty()) ramp.push_back({1, 1, 1});
  return &ramp;
}

bool Lights::HasRamp(const std::string& ramp) const { return vfs_->Exists(ramp); }  // [warpin]

void Lights::Flash(const std::string& ramp, Vec3 position, float size, double born, float seconds, const FlashFrom& from) {
  // (the same test as Add's: a place or size that is not a number reaches the grid's bounds and the lights' buffer)
  if (!(size > 0) || !(seconds > 0) || !std::isfinite(size) || !std::isfinite(seconds) || !Finite(position)) return;
  if (onFlash && from.explosion) onFlash(ramp, position, size, born, seconds, from);  // (volume explosions)
  else if (VolfxLog* log = VolfxLog::Get(); log && onFlash) log->Declined(true, size, "no explosion");  // (MOHO64_DEBUG_VOLFX)
  flashes_.push_back({Ramp(ramp), position, size, born, seconds, from.explosion});
}

void Lights::GroundChanged(float x0, float z0, float x1, float z1) {  // [lightshade]
  shades_.erase(std::remove_if(shades_.begin(), shades_.end(),
                               [&](const Shade& s) {
                                 return s.position.x + s.radius >= x0 && s.position.x - s.radius <= x1 && s.position.z + s.radius >= z0 &&
                                        s.position.z - s.radius <= z1;
                               }),
                shades_.end());
}

// [lightshade] A light's ground table (lights.h): for each square of n x n laid over the ground the light reaches, the
// height below which nothing of the light is seen from there (`lines`' first number of the square) and the height
// above which all of it is (the second). The light is taken as a line standing from `low` (its place, or the ground
// there, and kShadeRise) to `high` (shadeSize of its reach more, shadeTall at most): from a square, the steepest slope
// of the ground on the way to the light's foot is what hides its foot, the steepest to its head what hides all of it.
// The squares are worked out in rings outward from the light: a square takes the two slopes of the place on the ring
// before that lies on its way to the light (between that ring's squares), which give its two heights, and then adds
// the ground from there to its own middle for the squares behind it. So the ground under a square does not count for
// that square (a slope the light falls on never shadows itself, however coarse the squares), each square costs a few
// readings of the map whatever the reach, and a narrow shadow widens and softens a little with every ring, as a
// shadow far behind what casts it does. The ground within shadeBall of the reach round the light (shadeBallMost at
// most) is not read. (The ground at its nearest point, not between four: the readings lie a point or more apart anyway.)
void Lights::ShadeTable(Vec3 at, float radius, int n, float* lines, const float* raised) {
  const int half = n / 2;
  constexpr float kNone = -1e4f;  // (no ground in the way: a slope, and a height, no place lies under)
  const MapData& map = *ground;
  const float cell = 2 * radius / static_cast<float>(n), ball = std::min(radius * shadeBall, shadeBallMost);
  const float low = std::max(at.y, map.Height(at.x, at.z)) + kShadeRise, high = low + std::min(radius * shadeSize, shadeTall);
  shadeSlopes_.resize(static_cast<size_t>(n) * static_cast<size_t>(n) * 2);
  float* slopes = shadeSlopes_.data();  // each square's two slopes, its own ground in: to the light's foot, to its head
  // (the map's point nearest a place, past its edges the edge's: MapData::HeightAt, read here in place - a table of 128
  // squares each way reads the map some 60,000 times)
  const uint16_t* heights = map.heights.data();
  const int across = map.width + 1, wide = map.width, deep = map.height;
  const float unit = map.heightScale;
  auto groundAt = [&](float x, float z) {
    const int ix = std::clamp(static_cast<int>(x + 0.5f), 0, wide), iz = std::clamp(static_cast<int>(z + 0.5f), 0, deep);
    float h = static_cast<float>(heights[static_cast<size_t>(iz) * static_cast<size_t>(across) + static_cast<size_t>(ix)]) * unit;
    if (raised) {  // [hullshade] (what stands in the table's square the place lies in)
      const int gi = static_cast<int>(std::floor((x - at.x) / cell)) + half, gj = static_cast<int>(std::floor((z - at.z) / cell)) + half;
      if (gi >= 0 && gi < n && gj >= 0 && gj < n) h = std::max(h, raised[gj * n + gi]);
    }
    return h;
  };
  for (int ring = 0; ring < half; ++ring) {
    const int a = half - 1 - ring, b = half + ring;  // (the ring's squares: those on the border of a..b each way)
    for (int j = a; j <= b; ++j) {
      const bool whole = j == a || j == b;
      for (int i = a; i <= b; i += whole ? 1 : b - a) {
        const float ci = static_cast<float>(i) + 0.5f - static_cast<float>(half), cj = static_cast<float>(j) + 0.5f - static_cast<float>(half);
        const float u = ci * cell, v = cj * cell;  // (the square's middle from the light, world units)
        float in[2] = {kNone, kNone};
        float keep = 0;  // (the share of the way from the light at which the ring before is crossed)
        if (ring > 0) {
          keep = (static_cast<float>(ring) - 0.5f) / (static_cast<float>(ring) + 0.5f);
          const float fi = ci * keep + static_cast<float>(half) - 0.5f, fj = cj * keep + static_cast<float>(half) - 0.5f;
          const int i0 = std::clamp(static_cast<int>(std::floor(fi)), a + 1, b - 1), i1 = std::min(i0 + 1, b - 1);
          const int j0 = std::clamp(static_cast<int>(std::floor(fj)), a + 1, b - 1), j1 = std::min(j0 + 1, b - 1);
          const float ti = std::clamp(fi - static_cast<float>(i0), 0.0f, 1.0f), tj = std::clamp(fj - static_cast<float>(j0), 0.0f, 1.0f);
          for (int k = 0; k < 2; ++k) {
            const float s00 = slopes[(j0 * n + i0) * 2 + k], s10 = slopes[(j0 * n + i1) * 2 + k];
            const float s01 = slopes[(j1 * n + i0) * 2 + k], s11 = slopes[(j1 * n + i1) * 2 + k];
            const float near = s00 + (s10 - s00) * ti, far = s01 + (s11 - s01) * ti;
            in[k] = near + (far - near) * tj;
          }
        }
        const float away = std::sqrt(u * u + v * v);
        float* out = &lines[(j * n + i) * 2];
        out[0] = std::max(high + away * in[1], kNone);
        out[1] = std::max(low + away * in[0], kNone);
        // the ground from the ring before to this square's middle, for the squares behind it
        const int steps = std::clamp(static_cast<int>(std::ceil(away * (1 - keep) / kShadeStep)), 1, 4);
        for (int k = 1; k <= steps; ++k) {
          const float t = keep + (1 - keep) * static_cast<float>(k) / static_cast<float>(steps);
          const float d = away * t;
          if (d < ball || d < 1e-3f) continue;
          const float h = groundAt(at.x + u * t, at.z + v * t), per = 1 / d;
          in[0] = std::max(in[0], (h - low) * per);
          in[1] = std::max(in[1], (h - high) * per);
        }
        slopes[(j * n + i) * 2] = in[0];
        slopes[(j * n + i) * 2 + 1] = in[1];
      }
    }
  }
}

void Lights::Build(View* view) {
  // the flashes alive now, each in its ramp's colour of the moment
  flashes_.erase(std::remove_if(flashes_.begin(), flashes_.end(), [&](const FlashLight& f) { return now_ - f.born >= static_cast<double>(f.seconds); }),
                 flashes_.end());
  for (const FlashLight& f : flashes_) {
    if (now_ < f.born) continue;
    const std::vector<Vec3>& ramp = *f.ramp;
    const float t = static_cast<float>((now_ - f.born) / static_cast<double>(f.seconds)) * static_cast<float>(ramp.size() - 1);
    const size_t i = std::min(static_cast<size_t>(t), ramp.size() - 1), j = std::min(i + 1, ramp.size() - 1);
    const Vec3 color = Lerp(ramp[i], ramp[j], t - static_cast<float>(i)) * (strength * night);
    const float radius = f.explosion ? f.size * reach : std::min(f.size * reach, calmFlashReach);  // [calmflash]
    if (color.x + color.y + color.z > 0.003f) lights_.push_back(Light{f.position, radius, color, 1, {}, -2, {}});
  }
  count = static_cast<int>(lights_.size());
  shaded = shadeMade = shadeLeftOut = 0;  // [lightshade]
  ViewConstants& c = view->constants;
  for (float& v : c.lightGrid) v = 0;
  for (float& v : c.lightGridSize) v = 0;
  if (shadowCount_ == 0)
    for (Light& l : lights_) l.shadow = -1;

  rows_.clear();
  if (lights_.empty()) {
    rows_.assign(4, 0.0f);  // the shaders' buffer is never left unset
    device_->UpdateBuffer(buffer_, rows_.data(), static_cast<uint32_t>(rows_.size() * sizeof(float)));
    return;
  }
  // the grid covers the ground all the lights reach, in squares no smaller than kSmallestCell (lay: laid over the lights
  // as they are and each square's lights counted; true when a square has more than a pixel looks at)
  float x0 = 0, z0 = 0, cell = kSmallestCell;
  int cols = 1, rows = 1;
  size_t cells = 1;
  auto span = [&](const Light& l, int* cx0, int* cx1, int* cz0, int* cz1) {
    *cx0 = std::clamp(static_cast<int>((l.position.x - l.radius - x0) / cell), 0, cols - 1);
    *cx1 = std::clamp(static_cast<int>((l.position.x + l.radius - x0) / cell), 0, cols - 1);
    *cz0 = std::clamp(static_cast<int>((l.position.z - l.radius - z0) / cell), 0, rows - 1);
    *cz1 = std::clamp(static_cast<int>((l.position.z + l.radius - z0) / cell), 0, rows - 1);
  };
  auto lay = [&]() {
    x0 = 1e30f, z0 = 1e30f;
    float x1 = -1e30f, z1 = -1e30f;
    for (const Light& l : lights_) {
      x0 = std::min(x0, l.position.x - l.radius);
      z0 = std::min(z0, l.position.z - l.radius);
      x1 = std::max(x1, l.position.x + l.radius);
      z1 = std::max(z1, l.position.z + l.radius);
    }
    // (unitsfx) the squares' size from a ladder (kSmallestCell times powers of the square root of two), kept from frame to
    // frame while it still fits and is not more than a step too coarse; the grid's corner on a multiple of it, so that a
    // square is the same piece of ground from frame to frame while the size stays
    const float wide = std::max(x1 - x0, z1 - z0);
    int step = 0;
    while (kSmallestCell * std::exp2(0.5f * static_cast<float>(step)) * static_cast<float>(kMaxCellsAcross) < wide && step < 40) ++step;
    if (cellStep_ < step || cellStep_ > step + 1) cellStep_ = step;
    cell = kSmallestCell * std::exp2(0.5f * static_cast<float>(cellStep_));
    x0 = std::floor(x0 / cell) * cell;
    z0 = std::floor(z0 / cell) * cell;
    cols = std::clamp(static_cast<int>(std::ceil((x1 - x0) / cell)), 1, kMaxCellsAcross + 1);
    rows = std::clamp(static_cast<int>(std::ceil((z1 - z0) / cell)), 1, kMaxCellsAcross + 1);
    cells = static_cast<size_t>(cols) * static_cast<size_t>(rows);
    cellCount_.assign(cells, 0);
    for (const Light& l : lights_) {
      int a, b, c0, d;
      span(l, &a, &b, &c0, &d);
      for (int z = c0; z <= d; ++z)
        for (int x = a; x <= b; ++x) ++cellCount_[static_cast<size_t>(z) * static_cast<size_t>(cols) + static_cast<size_t>(x)];
    }
    bool over = false;
    uint32_t fullest = 0;
    for (uint32_t n : cellCount_) over = over || n > static_cast<uint32_t>(kMostInSquare), fullest = std::max(fullest, n);
    Mark("small lights in one square of the grid", fullest, kMostInSquare);
    return over;
  };
  merged = 0;
  if (lay()) {
    // [unitfx] Crowded squares (Chris, 2026-10-21: "shadow squares" when a private mod's flagship fires its missiles and guns: a hundred
    // and more shots' glows and hits' flashes where its salvos land, up to 192 lights reaching one square). A square keeps
    // only the kMostInSquare lights that light it most, so where its neighbour keeps a light it does not, the pool of light
    // steps at the square's edge. Where squares are that crowded, the lights that crowd them are first merged: lights to
    // all sides of about the same reach (steps of a quarter) lying within a cube a share of that reach across (a quarter,
    // then a half, a whole, two, until no square is over) become one at their middle (by brightness), as bright as all
    // of them together, reaching as far as the farthest of them did and a little more as they lay apart. While the cubes
    // are small only lights riding on the same model are merged (a lamp's ray-traced shadow passes the model it rides
    // on: lighting.h, RayLightSeen); from a whole reach on, any, and the merged light passes the model of its brightest
    // light that rode on one. Many flashes of one salvo's hits are about one light already, and light the ground almost
    // exactly as they did; a crowd that stays too dense is lit a little coarser, with no square's edge in it. Squares
    // that are not crowded keep their lights as they were (and a frame with none over keeps every light as it was).
    for (const float share : {0.25f, 0.5f, 1.0f, 2.0f}) {
      // (the lights that reach an over-full square: lamps' beams and lights with a shadow map of their own stay as they are)
      std::vector<std::array<int64_t, 7>> keys(lights_.size());
      std::vector<uint32_t> order;
      for (uint32_t n = 0; n < lights_.size(); ++n) {
        const Light& l = lights_[n];
        if (l.cosEdge > -1.5f || l.shadow >= 0) continue;
        int a, b, c0, d;
        span(l, &a, &b, &c0, &d);
        bool crowded = false;
        for (int z = c0; z <= d && !crowded; ++z)
          for (int x = a; x <= b && !crowded; ++x)
            crowded = cellCount_[static_cast<size_t>(z) * static_cast<size_t>(cols) + static_cast<size_t>(x)] > static_cast<uint32_t>(kMostInSquare);
        if (!crowded) continue;
        const int64_t reachStep = static_cast<int64_t>(std::floor(std::log(std::max(l.radius, 1e-3f)) / std::log(1.25f)));
        const float side = share * std::pow(1.25f, static_cast<float>(reachStep + 1));
        const bool byModel = share < 1 && l.hasOrigin;
        keys[n] = {reachStep, static_cast<int64_t>(std::floor(l.position.x / side)), static_cast<int64_t>(std::floor(l.position.y / side)),
                   static_cast<int64_t>(std::floor(l.position.z / side)), share < 1 && l.hasOrigin ? 1 : 0,
                   byModel ? static_cast<int64_t>(std::lround(l.origin.x * 64)) : 0, byModel ? static_cast<int64_t>(std::lround(l.origin.z * 64)) : 0};
        order.push_back(n);
      }
      std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return keys[a] < keys[b]; });
      // each group of more than one into its first light (in the list's order), the others left out
      std::vector<uint8_t> gone(lights_.size(), 0);
      int joined = 0;
      for (size_t i = 0; i < order.size();) {
        size_t j = i + 1;
        while (j < order.size() && keys[order[j]] == keys[order[i]]) ++j;
        if (j - i > 1) {
          uint32_t first = order[i];
          for (size_t k = i; k < j; ++k) first = std::min(first, order[k]);
          Vec3 at{0, 0, 0}, colour{0, 0, 0}, flux{0, 0, 0};  // ([mergeflux] flux: each light's colour times its reach squared)
          float weight = 0, farthest = 0, brightestRider = -1, inAir = 0;  // ([nightlight] inAir: their air shares by brightness)
          const Light* rider = nullptr;  // (the brightest that rides on a model)
          for (size_t k = i; k < j; ++k) {
            const Light& l = lights_[order[k]];
            const float w = 0.3f * l.color.x + 0.6f * l.color.y + 0.1f * l.color.z + 1e-6f;
            at = at + l.position * w;
            colour = colour + l.color;
            flux = flux + l.color * (l.radius * l.radius);
            weight += w;
            inAir += l.air * w;
            farthest = std::max(farthest, l.radius);
            if (l.hasOrigin && w > brightestRider) brightestRider = w, rider = &l;
          }
          const bool rides = rider != nullptr;
          const Vec3 riddenOn = rides ? rider->origin : Vec3{0, 0, 0};
          at = at * (1 / weight);
          float spread = 0;
          for (size_t k = i; k < j; ++k) {
            const Light& l = lights_[order[k]];
            spread += (0.3f * l.color.x + 0.6f * l.color.y + 0.1f * l.color.z + 1e-6f) * Length(l.position - at);
          }
          Light& kept = lights_[first];
          kept.position = at;
          kept.radius = farthest + spread / weight;
          // [mergeflux] (Chris's big test of 2026-11-01: "random bright red flash when upgrading goes away and comes back",
          // "random orange flashes" on factories, a red pool in the air between four factories) as much light in all as
          // the lights it stands for, not as bright at its middle as all of them there: the light a light lays on the
          // ground goes with its colour times its reach squared, so the merged one's colour is theirs so counted over its
          // own reach squared. Before, all their colours: four factories' glows a reach apart merged into one four times
          // as bright between them, which came and went as the squares round them went over and under kMostInSquare.
          // Lights on one place (a salvo's hits) merge almost as before (their reach and spread about the same).
          kept.color = mergeFlux ? flux * (1 / (kept.radius * kept.radius)) : colour;
          kept.hasOrigin = rides;
          kept.origin = riddenOn;
          kept.air = inAir / weight;
          for (size_t k = i; k < j; ++k)
            if (order[k] != first) gone[order[k]] = 1;
          joined += static_cast<int>(j - i) - 1;
        }
        i = j;
      }
      if (joined == 0) continue;
      size_t to = 0;
      for (size_t n = 0; n < lights_.size(); ++n)
        if (!gone[n]) lights_[to++] = lights_[n];
      lights_.resize(to);
      merged += joined;
      mergedMost = std::max(mergedMost, merged);
      if (!lay()) break;
    }
  }
  cellFirst_.assign(cells, 0);
  uint32_t total = 0;
  for (size_t i = 0; i < cells; ++i) {
    cellFirst_[i] = total;
    total += cellCount_[i];
    cellCount_[i] = 0;
  }
  indices_.assign(total, 0);
  for (uint32_t n = 0; n < lights_.size(); ++n) {
    int a, b, c0, d;
    span(lights_[n], &a, &b, &c0, &d);
    for (int z = c0; z <= d; ++z)
      for (int x = a; x <= b; ++x) {
        const size_t at = static_cast<size_t>(z) * static_cast<size_t>(cols) + static_cast<size_t>(x);
        indices_[cellFirst_[at] + cellCount_[at]++] = n;
      }
  }
  // a square with more lights than a pixel looks at (kMostInSquare, lighting.h): the ones that light it most come first
  // (their brightness by how much of it reaches the square's nearest point), so that what is left out is what the
  // square would miss least; before (unitsfx), the nearest to its middle, which left out a bright lamp's light a little
  // farther off and darkened the square
  leftOut = 0;
  for (int z = 0; z < rows; ++z)
    for (int x = 0; x < cols; ++x) {
      const size_t at = static_cast<size_t>(z) * static_cast<size_t>(cols) + static_cast<size_t>(x);
      if (cellCount_[at] <= static_cast<uint32_t>(kMostInSquare)) continue;
      leftOut += static_cast<int>(cellCount_[at]) - kMostInSquare;
      const float mx = x0 + (static_cast<float>(x) + 0.5f) * cell, mz = z0 + (static_cast<float>(z) + 0.5f) * cell;
      auto reaches = [&](uint32_t n) {
        const Light& l = lights_[n];
        const float dx = std::max(std::fabs(l.position.x - mx) - 0.5f * cell, 0.0f), dz = std::max(std::fabs(l.position.z - mz) - 0.5f * cell, 0.0f);
        const float far2 = (dx * dx + dz * dz) / (l.radius * l.radius);
        if (far2 >= 1) return 0.0f;
        const float fade = (1 - far2) * (1 - far2) / (1 + 4 * far2);
        return (0.3f * l.color.x + 0.6f * l.color.y + 0.1f * l.color.z) * fade;
      };
      uint32_t* list = indices_.data() + cellFirst_[at];
      std::nth_element(list, list + kMostInSquare, list + cellCount_[at], [&](uint32_t a, uint32_t b) { return reaches(a) > reaches(b); });
    }
  // [lightshade] the larger lights' ground tables (lights.h): which lights get one, each its own from a frame before
  // (a light that stands where it stood) or made now
  struct Shaded {
    uint32_t light;
    float strength;
    float throws;
    int cells;
    size_t table;  // (in shades_)
    bool hull = false;  // [hullshade] (its second table goes into the buffer too)
  };
  std::vector<Shaded> shadedNow;
  if (shade && ground && !ground->heights.empty()) {
    ++shadeBuild_;
    if (shadeHeights_ != ground->heights.data()) {  // (another map opened)
      shades_.clear();
      shadeHeights_ = ground->heights.data();
    }
    // (what a light throws in all: its colour's brightness times its reach squared)
    auto throws = [&](uint32_t n) {
      const Light& l = lights_[n];
      return (0.3f * l.color.x + 0.6f * l.color.y + 0.1f * l.color.z) * l.radius * l.radius;
    };
    std::vector<uint32_t> big;
    for (uint32_t n = 0; n < lights_.size(); ++n)
      if (lights_[n].cosEdge < -1.5f && lights_[n].radius >= shadeFrom) big.push_back(n);
    float cut = 0;  // (more than kShadeMost: what the first one left out throws)
    if (big.size() > static_cast<size_t>(kShadeMost)) {
      std::nth_element(big.begin(), big.begin() + kShadeMost, big.end(), [&](uint32_t a, uint32_t b) {
        const float ta = throws(a), tb = throws(b);
        return ta != tb ? ta > tb : a < b;
      });
      cut = throws(big[kShadeMost]);
      shadeLeftOut = static_cast<int>(big.size()) - kShadeMost;
      big.resize(kShadeMost);
      std::sort(big.begin(), big.end());
    }
    for (uint32_t n : big) {
      const Light& l = lights_[n];
      float strength = std::clamp((l.radius - shadeFrom) / std::max(shadeFull - shadeFrom, 1e-3f), 0.0f, 1.0f);
      strength = strength * strength * (3 - 2 * strength);
      // (more than kShadeMost: a light that throws little more than the first one left out is shadowed by as little, so
      // that none pops between shadowed and not as the lights' colours change their order from frame to frame)
      if (cut > 0) strength *= std::clamp(throws(n) / cut - 1, 0.0f, 1.0f);
      if (!(strength > 0)) continue;
      shadedNow.push_back({n, strength, throws(n), ShadeCells(l.radius), 0, false});
    }
    // (more rows than kShadeRowsMost: the lights that throw least take coarser tables, until they fit)
    size_t need = 0;
    for (const Shaded& s : shadedNow) need += ShadeRows(s.cells);
    while (need > static_cast<size_t>(kShadeRowsMost)) {
      Shaded* least = nullptr;
      for (Shaded& s : shadedNow)
        if (s.cells > 32 && (!least || s.throws < least->throws)) least = &s;
      if (!least) break;
      need -= ShadeRows(least->cells) - ShadeRows(least->cells / 2);
      least->cells /= 2;
    }
    static const bool debug = std::getenv("MOHO64_DEBUG_LIGHTSHADE") != nullptr;
    for (Shaded& now : shadedNow) {
      const Light& l = lights_[now.light];
      // its table of a frame before: the one made nearest to where it stands now, within kShadeKept of a square (and of
      // the same reach, within a fiftieth)
      const float cell = 2 * l.radius / static_cast<float>(now.cells);
      size_t table = shades_.size();
      float nearest = kShadeKept * cell;
      for (size_t k = 0; k < shades_.size(); ++k) {
        const Shade& s = shades_[k];
        if (s.cells != now.cells || std::fabs(s.radius - l.radius) > 0.02f * l.radius) continue;
        const float moved = Length(s.position - l.position);
        if (moved <= nearest) {
          nearest = moved;
          table = k;
        }
      }
      if (table == shades_.size()) {
        // (kept: twice kShadeMost; then the one not read for the longest makes room, never one of this Build)
        if (shades_.size() < static_cast<size_t>(2 * kShadeMost)) {
          shades_.emplace_back();
        } else {
          table = 0;
          for (size_t k = 1; k < shades_.size(); ++k)
            if (shades_[k].used < shades_[table].used) table = k;
        }
        Shade& s = shades_[table];
        s.position = l.position;
        s.radius = l.radius;
        s.cells = now.cells;
        s.hullKey = 0;  // [hullshade] (its second table was another light's)
        s.lines.resize(ShadeRows(now.cells) * 4);
        // (its first row: where its middle lies, its squares' width and their number each way; light_shade.h reads by it)
        s.lines[0] = l.position.x;
        s.lines[1] = l.position.z;
        s.lines[2] = cell;
        s.lines[3] = static_cast<float>(now.cells);
        const auto began = std::chrono::steady_clock::now();
        ShadeTable(l.position, l.radius, now.cells, s.lines.data() + 4);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
        shadeMs += ms;
        ++shadeMade;
        ++shadeMadeAll;
        if (debug)
          fprintf(stderr, "render: [lightshade] build %llu: a table of %d squares each way for the light at %.1f, %.1f, %.1f (the ground there %.1f), reach %.1f, colour %.2f %.2f %.2f, in %.3f ms\n",
                  static_cast<unsigned long long>(shadeBuild_), now.cells, l.position.x, l.position.y, l.position.z, ground->Height(l.position.x, l.position.z),
                  l.radius, l.color.x, l.color.y, l.color.z, ms);
      }
      shades_[table].used = shadeBuild_;
      now.table = table;
    }
    shaded = static_cast<int>(shadedNow.size());
    shadedMost = std::max(shadedMost, shaded);
    shadeLeftOutMost = std::max(shadeLeftOutMost, shadeLeftOut);
    // [hullshade] the second tables (lights.h): for each light with a table, the boxes that count for it (in its reach,
    // wide enough for its squares, the light not in them nor riding on them), laid over the table's squares; made anew
    // only when the boxes are others than the kept table's (their places to an eighth of a square)
    hullTables = 0;
    if (hulls && !hulls_.empty() && !shadedNow.empty()) {
      size_t rows = 0;
      for (const Shaded& s : shadedNow) rows += ShadeRows(s.cells);
      // (the boxes by squares of kHullCell world units over the ground they stand on; a light then goes through the
      // squares its reach and the largest box's half touch)
      constexpr float kHullCell = 16;
      float gx0 = 1e30f, gz0 = 1e30f, gx1 = -1e30f, gz1 = -1e30f, widest = 0;
      for (const Hull& b : hulls_) {
        gx0 = std::min(gx0, b.x), gz0 = std::min(gz0, b.z), gx1 = std::max(gx1, b.x), gz1 = std::max(gz1, b.z);
        widest = std::max(widest, b.halfLength + b.halfWidth);
      }
      const int gcols = std::clamp(static_cast<int>((gx1 - gx0) / kHullCell) + 1, 1, 512), grows = std::clamp(static_cast<int>((gz1 - gz0) / kHullCell) + 1, 1, 512);
      auto squareOf = [&](const Hull& b) {
        const int cx = std::clamp(static_cast<int>((b.x - gx0) / kHullCell), 0, gcols - 1), cz = std::clamp(static_cast<int>((b.z - gz0) / kHullCell), 0, grows - 1);
        return static_cast<size_t>(cz) * static_cast<size_t>(gcols) + static_cast<size_t>(cx);
      };
      hullFirst_.assign(static_cast<size_t>(gcols) * static_cast<size_t>(grows) + 1, 0);
      for (const Hull& b : hulls_) ++hullFirst_[squareOf(b) + 1];
      for (size_t k = 1; k < hullFirst_.size(); ++k) hullFirst_[k] += hullFirst_[k - 1];
      hullItems_.resize(hulls_.size());
      {
        std::vector<uint32_t> at(hullFirst_.begin(), hullFirst_.end() - 1);
        for (uint32_t k = 0; k < hulls_.size(); ++k) hullItems_[at[squareOf(hulls_[k])]++] = k;
      }
      auto near = [&](Vec3 at, float radius, const std::function<void(const Hull&)>& f) {
        const float r = radius + widest;
        const int cx0 = std::clamp(static_cast<int>(std::floor((at.x - r - gx0) / kHullCell)), 0, gcols - 1);
        const int cx1 = std::clamp(static_cast<int>(std::floor((at.x + r - gx0) / kHullCell)), 0, gcols - 1);
        const int cz0 = std::clamp(static_cast<int>(std::floor((at.z - r - gz0) / kHullCell)), 0, grows - 1);
        const int cz1 = std::clamp(static_cast<int>(std::floor((at.z + r - gz0) / kHullCell)), 0, grows - 1);
        if (at.x + r < gx0 || at.z + r < gz0 || at.x - r > gx1 + kHullCell || at.z - r > gz1 + kHullCell) return;
        for (int cz = cz0; cz <= cz1; ++cz)
          for (int cx = cx0; cx <= cx1; ++cx) {
            const size_t sq = static_cast<size_t>(cz) * static_cast<size_t>(gcols) + static_cast<size_t>(cx);
            for (uint32_t k = hullFirst_[sq]; k < hullFirst_[sq + 1]; ++k) f(hulls_[hullItems_[k]]);
          }
      };
      for (Shaded& now : shadedNow) {
        Shade& s = shades_[now.table];
        const Light& l = lights_[now.light];
        const int n = now.cells, half = n / 2;
        const float cell = 2 * s.radius / static_cast<float>(n);
        const float low = std::max(s.position.y, ground->Height(s.position.x, s.position.z));
        uint64_t key = 0;
        bool any = false;
        auto counts = [&](const Hull& b) {
          if (std::min(b.halfLength, b.halfWidth) < hullLeast * cell) return false;
          const float dx = b.x - s.position.x, dz = b.z - s.position.z, r = b.halfLength + b.halfWidth;
          if (std::fabs(dx) - r > s.radius || std::fabs(dz) - r > s.radius) return false;
          // (the light, or the model it rides on, in the box: a fire on a building, a unit's own glow)
          auto inside = [&](float px, float pz) {
            const float ex = px - b.x, ez = pz - b.z;
            return std::fabs(ex * b.forwardX + ez * b.forwardZ) < b.halfLength + 0.5f && std::fabs(ez * b.forwardX - ex * b.forwardZ) < b.halfWidth + 0.5f;
          };
          if (inside(s.position.x, s.position.z) && b.top > low - 1) return false;
          if (l.hasOrigin && inside(l.origin.x, l.origin.z)) return false;
          return true;
        };
        near(s.position, s.radius, [&](const Hull& b) {
          if (!counts(b)) return;
          any = true;
          // (the boxes' sum, each by its place to an eighth of a square, its turn, size and top: whatever their order)
          const float q = 8 / cell;
          const int64_t v[7] = {std::llround(b.x * q), std::llround(b.z * q), std::llround(b.forwardX * 64), std::llround(b.forwardZ * 64),
                                std::llround(b.halfLength * q), std::llround(b.halfWidth * q), std::llround(b.top * 8)};
          uint64_t h = 1469598103934665603ull;
          for (int64_t part : v) h = (h ^ static_cast<uint64_t>(part)) * 1099511628211ull;
          key += h | 1;
        });
        if (!any) {
          s.hullKey = 0;
          continue;
        }
        if (rows + ShadeRows(n) > static_cast<size_t>(kShadeRowsMost)) continue;  // (no room in the buffer: the first table alone)
        if (key == 0) key = 1;
        if (s.hullKey != key || s.hullLines.size() != ShadeRows(n) * 4) {
          shadeRaised_.assign(static_cast<size_t>(n) * static_cast<size_t>(n), -1e30f);
          near(s.position, s.radius, [&](const Hull& b) {
            if (!counts(b)) return;
            const float r = b.halfLength + b.halfWidth;
            const int i0 = std::clamp(static_cast<int>(std::floor((b.x - r - s.position.x) / cell)) + half, 0, n - 1);
            const int i1 = std::clamp(static_cast<int>(std::floor((b.x + r - s.position.x) / cell)) + half, 0, n - 1);
            const int j0 = std::clamp(static_cast<int>(std::floor((b.z - r - s.position.z) / cell)) + half, 0, n - 1);
            const int j1 = std::clamp(static_cast<int>(std::floor((b.z + r - s.position.z) / cell)) + half, 0, n - 1);
            for (int j = j0; j <= j1; ++j)
              for (int i = i0; i <= i1; ++i) {
                // (the square's middle in the box's own ways; how much of the square the box covers, each way)
                const float px = s.position.x + (static_cast<float>(i - half) + 0.5f) * cell, pz = s.position.z + (static_cast<float>(j - half) + 0.5f) * cell;
                const float ex = px - b.x, ez = pz - b.z;
                const float along = std::fabs(ex * b.forwardX + ez * b.forwardZ), sideways = std::fabs(ez * b.forwardX - ex * b.forwardZ);
                const float covered = std::clamp((b.halfLength + 0.5f * cell - along) / cell, 0.0f, 1.0f) *
                                      std::clamp((b.halfWidth + 0.5f * cell - sideways) / cell, 0.0f, 1.0f);
                if (covered <= 0) continue;
                const float under = ground->Height(px, pz);
                if (b.top <= under) continue;
                float& to = shadeRaised_[static_cast<size_t>(j) * static_cast<size_t>(n) + static_cast<size_t>(i)];
                to = std::max(to, under + (b.top - under) * covered);
              }
          });
          s.hullLines.resize(ShadeRows(n) * 4);
          std::memcpy(s.hullLines.data(), s.lines.data(), 4 * sizeof(float));  // (its first row: as the first table's)
          const auto began = std::chrono::steady_clock::now();
          ShadeTable(s.position, s.radius, n, s.hullLines.data() + 4, shadeRaised_.data());
          shadeMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
          s.hullKey = key;
          ++hullMadeAll;
          if (debug)
            fprintf(stderr, "render: [hullshade] build %llu: a second table of %d squares each way for the light at %.1f, %.1f, %.1f, reach %.1f\n",
                    static_cast<unsigned long long>(shadeBuild_), n, s.position.x, s.position.y, s.position.z, s.radius);
        }
        now.hull = true;
        rows += ShadeRows(n);
        ++hullTables;
      }
      hullTablesMost = std::max(hullTablesMost, hullTables);
    }
  }
  // the buffer, in rows of four numbers: one row per square (where its list begins, how long it
  // is), then the lists (four light numbers a row), then four rows per light, then four rows per
  // lamp's shadow map (its matrix), then each ground table's rows ([lightshade])
  const size_t listRows = (total + 3) / 4;
  const size_t firstList = cells, firstLight = cells + listRows, firstMatrix = firstLight + lights_.size() * 4;
  const size_t firstTable = firstMatrix + static_cast<size_t>(shadowCount_) * 4;
  size_t tableRows = 0;
  for (const Shaded& s : shadedNow) tableRows += ShadeRows(s.cells) * (s.hull ? 2 : 1);  // ([hullshade] and the second tables)
  rows_.assign((firstTable + tableRows) * 4, 0.0f);
  for (size_t i = 0; i < cells; ++i) {
    rows_[i * 4] = static_cast<float>(firstList * 4 + cellFirst_[i]);  // counted in numbers, not rows
    rows_[i * 4 + 1] = static_cast<float>(cellCount_[i]);
  }
  for (uint32_t i = 0; i < total; ++i) rows_[firstList * 4 + i] = static_cast<float>(indices_[i]);
  for (size_t n = 0; n < lights_.size(); ++n) {
    float* p = &rows_[(firstLight + n * 4) * 4];
    p[0] = lights_[n].position.x;
    p[1] = lights_[n].position.y;
    p[2] = lights_[n].position.z;
    p[3] = lights_[n].radius;
    p[4] = lights_[n].color.x;
    p[5] = lights_[n].color.y;
    p[6] = lights_[n].color.z;
    p[7] = lights_[n].cosEdge < -1.5f ? lights_[n].air : lights_[n].glow;  // ([nightlight] a light to all sides: its share in the air)
    p[8] = lights_[n].direction.x;
    p[9] = lights_[n].direction.y;
    p[10] = lights_[n].direction.z;
    // ([hullshade] a light to all sides: -2 less the most its own ball is for the rays; -2 itself: no most, as before)
    p[11] = lights_[n].cosEdge < -1.5f && hulls && shade ? -2 - std::max(rayBallMost, 0.0f) : lights_[n].cosEdge;
    p[12] = static_cast<float>(lights_[n].shadow);
    // (without a shadow map: -2 and where the model it rides on stands, for the rays, lighting.h's RayLightSeen)
    if (lights_[n].shadow < 0 && lights_[n].hasOrigin) {
      p[12] = -2;
      p[13] = lights_[n].origin.x;
      p[14] = lights_[n].origin.y;
      p[15] = lights_[n].origin.z;
    }
  }
  for (int k = 0; k < shadowCount_; ++k)
    std::memcpy(&rows_[(firstMatrix + static_cast<size_t>(k) * 4) * 4], shadowMatrix_[k].m, 16 * sizeof(float));
  // [lightshade] the ground tables, and in each one's light's third row (a light to all sides: unused till now) the row
  // it begins at and how strongly it counts
  size_t tableAt = firstTable;
  for (const Shaded& s : shadedNow) {
    std::memcpy(&rows_[tableAt * 4], shades_[s.table].lines.data(), ShadeRows(s.cells) * 4 * sizeof(float));
    float* p = &rows_[(firstLight + static_cast<size_t>(s.light) * 4) * 4];
    p[8] = static_cast<float>(tableAt);
    p[9] = s.strength;
    tableAt += ShadeRows(s.cells);
    if (s.hull) {  // [hullshade] (its second table: the ground with what stands on it)
      std::memcpy(&rows_[tableAt * 4], shades_[s.table].hullLines.data(), ShadeRows(s.cells) * 4 * sizeof(float));
      p[10] = static_cast<float>(tableAt);
      tableAt += ShadeRows(s.cells);
    }
  }
  shadowCount_ = 0;  // (placed anew every frame, or not at all)
  device_->UpdateBuffer(buffer_, rows_.data(), static_cast<uint32_t>(rows_.size() * sizeof(float)));
  c.lightGrid[0] = x0;
  c.lightGrid[1] = z0;
  c.lightGrid[2] = 1 / cell;
  c.lightGrid[3] = 1;
  c.lightGridSize[0] = static_cast<float>(cols);
  c.lightGridSize[1] = static_cast<float>(rows);
  c.lightGridSize[2] = static_cast<float>(firstLight);
  c.lightGridSize[3] = static_cast<float>(firstMatrix);
}

void Lights::Bind() { device_->SetShaderBuffer(buffer_, 1); }

}  // namespace render
