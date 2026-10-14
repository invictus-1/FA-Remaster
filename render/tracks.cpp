#include "tracks.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace render {
namespace {

constexpr int kSize = 2048;  // the picture's side: half a world unit a pixel on a map of 1024

// A disc that is full in its middle and soft at its rim, laid over what is there by its alpha.
const char kShader[] = R"hlsl(
struct VIn {
  float2 position : POSITION;
  float4 disc : TEXCOORD0;  // u, v across the disc; the value written; how much of it
};
struct VOut {
  float4 position : SV_Position;
  float4 disc : TEXCOORD0;
};
VOut VS(VIn i) {
  VOut o;
  o.position = float4(i.position, 0, 1);
  o.disc = i.disc;
  return o;
}
float4 PS(VOut i) : SV_Target {
  float rim = 1 - smoothstep(0.15, 1.0, length(i.disc.xy));
  return float4(i.disc.z, i.disc.z, i.disc.z, i.disc.w * rim);
}
)hlsl";

}  // namespace

bool Tracks::Init(std::string* error) {
  gfx::PipelineDesc pd;
  pd.name = "tracks";
  pd.vertexSource = pd.pixelSource = kShader;
  pd.attrs = {{"POSITION", 0, gfx::AttrType::Float2, offsetof(Vertex, x)}, {"TEXCOORD", 0, gfx::AttrType::Float4, offsetof(Vertex, u)}};
  pd.stride = sizeof(Vertex);
  pd.cull = gfx::Cull::None;
  pd.depthTest = pd.depthWrite = false;
  pd.depthTarget = false;
  pd.blend = gfx::Blend::Alpha;
  pd.colorFormat = gfx::Format::RGBA8;
  pipeline_ = device_->CreatePipeline(pd, error);
  if (!pipeline_) return false;
  gfx::BufferDesc bd;
  bd.stride = sizeof(Vertex);
  bd.dynamic = true;
  buffer_ = device_->CreateBuffer(bd);
  return true;
}

void Tracks::SetMap(float width, float height) {
  width_ = width;
  height_ = height;
  if (!map_) {
    gfx::TargetDesc td;
    td.width = td.height = kSize;
    td.format = gfx::Format::RGBA8;
    map_ = device_->CreateTarget(td);
  }
  fresh_ = true;  // (cleared when it is first drawn into: a pass cannot be begun outside a frame)
  trodden_ = false;
}

void Tracks::Add(uint32_t id, Vec3 position, float radius) {
  if (!on || radius <= 0 || width_ <= 0) return;
  if (printers_.count(id)) return;  // (it lays its own prints)
  // (never narrower than a pixel and a half of the picture: a smaller disc falls between its
  // pixels and the track comes out as a row of beads)
  radius = std::max(radius * 1.3f, 2.2f * width_ / static_cast<float>(kSize));  // (and wider by what its soft rim takes)
  Last& last = last_[id];
  // the way since the frame before, in steps of half a radius (not a jump: a unit carried or newly made)
  if (last.frame + 1 == frame_) {
    const float dx = position.x - last.x, dz = position.z - last.z, far = std::sqrt(dx * dx + dz * dz);
    const int steps = far < 25 ? std::min(static_cast<int>(far / (radius * 0.5f)), 60) : 0;
    for (int k = 1; k <= steps; ++k) {
      const float t = static_cast<float>(k) / static_cast<float>(steps + 1);
      stamps_.push_back({last.x + dx * t, last.z + dz * t, radius});
    }
  }
  stamps_.push_back({position.x, position.z, radius});
  last = {position.x, position.z, frame_};
  if (last_.size() > 40000) last_.clear();  // (ids pile up over a long game)
}

void Tracks::AddPrint(uint32_t id, Vec3 centre, float sizeX, float sizeZ) {
  if (!on || width_ <= 0 || sizeX <= 0 || sizeZ <= 0) return;
  printers_[id] = frame_;
  if (printers_.size() > 40000) printers_.clear();
  // (a soft disc a little larger than the print, and no smaller than the picture can hold)
  const float radius = std::max(std::max(sizeX, sizeZ) * 0.9f, 1.8f * width_ / static_cast<float>(kSize));
  if (prints_.size() < 20000) prints_.push_back({centre.x, centre.z, radius});
}

void Tracks::Draw(float covering, bool clear) {
  if (!map_ || width_ <= 0 || height_ <= 0) return;
  if (clear || !on) {
    prints_.clear();
    if (trodden_ || fresh_) {  // (cleared to nothing by beginning a pass that clears)
      gfx::PassDesc pass;
      pass.color = map_;
      device_->BeginPass(pass);
      trodden_ = fresh_ = false;
    }
    return;
  }
  vertices_.clear();
  auto quad = [&](float x0, float y0, float x1, float y1, bool disc, float value, float alpha) {
    const float e = disc ? 1.0f : 0.0f;
    const Vertex a{x0, y0, -e, -e, value, alpha}, b{x1, y0, e, -e, value, alpha}, c{x1, y1, e, e, value, alpha}, d{x0, y1, -e, e, value, alpha};
    for (const Vertex& v : {a, b, c, a, c, d}) vertices_.push_back(v);
  };
  // new snow first: all of the picture a little toward untrodden; then what treads now
  if (covering > 0 && trodden_) quad(-1, -1, 1, 1, false, 0, std::min(covering, 1.0f));
  auto stamp = [&](const Stamp& s, float alpha) {
    // the map's x, z to the picture's -1..1 (its top row is the map's z = 0)
    const float cx = s.x / width_ * 2 - 1, cy = 1 - s.z / height_ * 2;
    const float rx = s.radius / width_ * 2, ry = s.radius / height_ * 2;
    quad(cx - rx, cy - ry, cx + rx, cy + ry, true, 1, alpha);
  };
  for (const Stamp& s : stamps_) stamp(s, 0.6f);
  for (const Stamp& s : prints_) stamp(s, 0.75f);
  if (!stamps_.empty() || !prints_.empty()) trodden_ = true;
  prints_.clear();
  if (vertices_.empty() && !fresh_) return;
  gfx::PassDesc pass;
  pass.color = map_;
  pass.clearColor = fresh_;
  fresh_ = false;
  device_->BeginPass(pass);
  if (vertices_.empty()) return;
  device_->SetPipeline(pipeline_);
  device_->UpdateBuffer(buffer_, vertices_.data(), static_cast<uint32_t>(vertices_.size() * sizeof(Vertex)));
  device_->SetVertexBuffer(buffer_);
  device_->Draw(static_cast<uint32_t>(vertices_.size()));
}

}  // namespace render
