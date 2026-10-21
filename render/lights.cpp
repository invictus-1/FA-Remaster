#include "lights.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "core/vfs.h"
#include "dds.h"
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
    const float away = std::hypot(l.position.x - camera.target.x, l.position.z - camera.target.z);
    if (away - l.radius > camera.distance * 1.3f) continue;
    near.push_back({away, n});
  }
  const size_t count = std::min(near.size(), static_cast<size_t>(kLampShadows));
  std::partial_sort(near.begin(), near.begin() + static_cast<std::ptrdiff_t>(count), near.end(), [](const Near& a, const Near& b) { return a.away < b.away; });
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
}

void Lights::Add(Vec3 position, float radius, Vec3 color, const Vec3* origin) {
  if (radius <= 0) return;
  lights_.push_back(Light{position, radius, color, 1, {}, -2, {}});
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

void Lights::AddBeam(Vec3 position, float radius, Vec3 color, Vec3 direction, float coneDegrees, float glow, const Vec3* carrier, const Vec3* origin) {
  if (radius <= 0) return;
  Light light{position, radius, color, 1, {}, -2, {}};
  light.glow = glow;
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

void Lights::Flash(const std::string& ramp, Vec3 position, float size, double born, float seconds, const FlashFrom& from) {
  if (size <= 0 || seconds <= 0) return;
  if (onFlash && from.explosion) onFlash(ramp, position, size, born, seconds, from);  // (volume explosions)
  else if (VolfxLog* log = VolfxLog::Get(); log && onFlash) log->Declined(true, size, "no explosion");  // (MOHO64_DEBUG_VOLFX)
  flashes_.push_back({Ramp(ramp), position, size, born, seconds});
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
    if (color.x + color.y + color.z > 0.003f) lights_.push_back(Light{f.position, f.size * reach, color, 1, {}, -2, {}});
  }
  count = static_cast<int>(lights_.size());
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
    for (uint32_t n : cellCount_) over = over || n > static_cast<uint32_t>(kMostInSquare);
    return over;
  };
  merged = 0;
  if (lay()) {
    // [unitfx] Crowded squares (Chris, 2026-10-21: "shadow squares" when the Izanagi fires its missiles and guns: a hundred
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
          Vec3 at{0, 0, 0}, colour{0, 0, 0};
          float weight = 0, farthest = 0, brightestRider = -1;
          const Light* rider = nullptr;  // (the brightest that rides on a model)
          for (size_t k = i; k < j; ++k) {
            const Light& l = lights_[order[k]];
            const float w = 0.3f * l.color.x + 0.6f * l.color.y + 0.1f * l.color.z + 1e-6f;
            at = at + l.position * w;
            colour = colour + l.color;
            weight += w;
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
          kept.color = colour;
          kept.radius = farthest + spread / weight;
          kept.hasOrigin = rides;
          kept.origin = riddenOn;
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
  for (int z = 0; z < rows; ++z)
    for (int x = 0; x < cols; ++x) {
      const size_t at = static_cast<size_t>(z) * static_cast<size_t>(cols) + static_cast<size_t>(x);
      if (cellCount_[at] <= static_cast<uint32_t>(kMostInSquare)) continue;
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
  // the buffer, in rows of four numbers: one row per square (where its list begins, how long it
  // is), then the lists (four light numbers a row), then four rows per light, then four rows per
  // lamp's shadow map (its matrix)
  const size_t listRows = (total + 3) / 4;
  const size_t firstList = cells, firstLight = cells + listRows, firstMatrix = firstLight + lights_.size() * 4;
  rows_.assign((firstMatrix + static_cast<size_t>(shadowCount_) * 4) * 4, 0.0f);
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
    p[7] = lights_[n].glow;
    p[8] = lights_[n].direction.x;
    p[9] = lights_[n].direction.y;
    p[10] = lights_[n].direction.z;
    p[11] = lights_[n].cosEdge;
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
