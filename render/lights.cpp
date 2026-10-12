#include "lights.h"

#include <algorithm>
#include <cmath>

#include "core/vfs.h"
#include "dds.h"

namespace render {
namespace {

constexpr int kMaxCellsAcross = 96;  // the grid is at most this many squares each way
constexpr float kSmallestCell = 6;   // world units

}  // namespace

Lights::Lights(gfx::Device* device, const moho::Vfs* vfs) : device_(device), vfs_(vfs) {
  gfx::BufferDesc bd;
  bd.kind = gfx::BufferKind::Shader;
  bd.stride = 16;
  bd.dynamic = true;
  buffer_ = device_->CreateBuffer(bd);
}

void Lights::Begin(double now) {
  now_ = now;
  lights_.clear();
}

void Lights::Add(Vec3 position, float radius, Vec3 color) {
  if (radius > 0) lights_.push_back({position, radius, color});
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

void Lights::Flash(const std::string& ramp, Vec3 position, float size, double born, float seconds) {
  if (size <= 0 || seconds <= 0) return;
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
    const Vec3 color = Lerp(ramp[i], ramp[j], t - static_cast<float>(i)) * strength;
    if (color.x + color.y + color.z > 0.003f) lights_.push_back({f.position, f.size * reach, color});
  }
  count = static_cast<int>(lights_.size());
  ViewConstants& c = view->constants;
  for (float& v : c.lightGrid) v = 0;
  for (float& v : c.lightGridSize) v = 0;

  rows_.clear();
  if (lights_.empty()) {
    rows_.assign(4, 0.0f);  // the shaders' buffer is never left unset
    device_->UpdateBuffer(buffer_, rows_.data(), static_cast<uint32_t>(rows_.size() * sizeof(float)));
    return;
  }
  // the grid covers the ground all the lights reach, in squares no smaller than kSmallestCell
  float x0 = 1e30f, z0 = 1e30f, x1 = -1e30f, z1 = -1e30f;
  for (const Light& l : lights_) {
    x0 = std::min(x0, l.position.x - l.radius);
    z0 = std::min(z0, l.position.z - l.radius);
    x1 = std::max(x1, l.position.x + l.radius);
    z1 = std::max(z1, l.position.z + l.radius);
  }
  const float cell = std::max(kSmallestCell, std::max(x1 - x0, z1 - z0) / static_cast<float>(kMaxCellsAcross));
  const int cols = std::max(1, static_cast<int>(std::ceil((x1 - x0) / cell))), rows = std::max(1, static_cast<int>(std::ceil((z1 - z0) / cell)));
  const size_t cells = static_cast<size_t>(cols) * static_cast<size_t>(rows);
  cellCount_.assign(cells, 0);
  auto span = [&](const Light& l, int* cx0, int* cx1, int* cz0, int* cz1) {
    *cx0 = std::clamp(static_cast<int>((l.position.x - l.radius - x0) / cell), 0, cols - 1);
    *cx1 = std::clamp(static_cast<int>((l.position.x + l.radius - x0) / cell), 0, cols - 1);
    *cz0 = std::clamp(static_cast<int>((l.position.z - l.radius - z0) / cell), 0, rows - 1);
    *cz1 = std::clamp(static_cast<int>((l.position.z + l.radius - z0) / cell), 0, rows - 1);
  };
  for (const Light& l : lights_) {
    int a, b, c0, d;
    span(l, &a, &b, &c0, &d);
    for (int z = c0; z <= d; ++z)
      for (int x = a; x <= b; ++x) ++cellCount_[static_cast<size_t>(z) * static_cast<size_t>(cols) + static_cast<size_t>(x)];
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
  // the buffer, in rows of four numbers: one row per square (where its list begins, how long it
  // is), then the lists (four light numbers a row), then two rows per light
  const size_t listRows = (total + 3) / 4;
  const size_t firstList = cells, firstLight = cells + listRows;
  rows_.assign((firstLight + lights_.size() * 2) * 4, 0.0f);
  for (size_t i = 0; i < cells; ++i) {
    rows_[i * 4] = static_cast<float>(firstList * 4 + cellFirst_[i]);  // counted in numbers, not rows
    rows_[i * 4 + 1] = static_cast<float>(cellCount_[i]);
  }
  for (uint32_t i = 0; i < total; ++i) rows_[firstList * 4 + i] = static_cast<float>(indices_[i]);
  for (size_t n = 0; n < lights_.size(); ++n) {
    float* p = &rows_[(firstLight + n * 2) * 4];
    p[0] = lights_[n].position.x;
    p[1] = lights_[n].position.y;
    p[2] = lights_[n].position.z;
    p[3] = lights_[n].radius;
    p[4] = lights_[n].color.x;
    p[5] = lights_[n].color.y;
    p[6] = lights_[n].color.z;
  }
  device_->UpdateBuffer(buffer_, rows_.data(), static_cast<uint32_t>(rows_.size() * sizeof(float)));
  c.lightGrid[0] = x0;
  c.lightGrid[1] = z0;
  c.lightGrid[2] = 1 / cell;
  c.lightGrid[3] = 1;
  c.lightGridSize[0] = static_cast<float>(cols);
  c.lightGridSize[1] = static_cast<float>(rows);
  c.lightGridSize[2] = static_cast<float>(firstLight);
}

void Lights::Bind() { device_->SetShaderBuffer(buffer_, 1); }

}  // namespace render
