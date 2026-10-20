#include "cover_bare.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "view.h"

namespace render {
namespace {

// The picture (a thread a texel): the streams' (laid on the processor, its margin with it), and where the table's
// meadow (Terrain::CoverDensity's second picture, alpha: the blades its layers give, before the trees change them) is
// markedly thinner than round about it, on two rings, the nearer and the farther, whichever is thicker: a trail or a
// bare patch through the meadow. A wide stretch of thinner cover is as thin round about as in it: nothing bare there.
const char kBarePass[] = R"hlsl(
cbuffer Bare : register(b1) {
  float4 info;  // x, y: the map's width and height (world units); z: 1 = the table's picture is read; w: how fast a trail is bare
  float4 more;  // x: how far round about the table is read (world units); y: 1 = the map's cover mask is read
};
Texture2D<float> streams : register(t0);  // the streams' picture: 1 on a ribbon and its margin
Texture2D table : register(t1);           // Terrain::CoverDensity's second picture: alpha the meadow's blades
Texture2D<float> coverMask : register(t2);  // Terrain::CoverMask (where there is no table: with one, it is in the table)
RWTexture2D<float> bare : register(u0);
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  bare.GetDimensions(w, h);
  if (id.x >= w || id.y >= h) return;
  float2 uv = (id.xy + 0.5) / float2(w, h);
  float b = streams.SampleLevel(samplerLinearClamp, uv, 0);
  [branch] if (info.z > 0.5) {
    float here = table.SampleLevel(samplerLinearClamp, uv, 0).a;
    float2 reach = more.x / info.xy;
    float nearer = 0, farther = 0;
    [unroll] for (int k = 0; k < 8; ++k) {
      float a = k * 0.7853982 + 0.3927;
      float2 way = float2(cos(a), sin(a)) * reach;
      nearer += table.SampleLevel(samplerLinearClamp, uv + way * 0.5, 0).a;
      farther += table.SampleLevel(samplerLinearClamp, uv + way, 0).a;
    }
    float usual = max(nearer, farther) / 8;
    b = max(b, saturate((usual - here - 0.1) * info.w) * saturate(usual * 2));
  }
  [branch] if (more.y > 0.5) b = max(b, saturate(coverMask.SampleLevel(samplerLinearClamp, uv, 0)));
  bare[id.xy] = b;
}
)hlsl";

struct BareConstants {
  float info[4], more[4];
};

}  // namespace

CoverBare::~CoverBare() {
  for (gfx::Texture t : {streamTexture_, picture_})
    if (t) device_->Destroy(t);
  if (pass_) device_->Destroy(pass_);
}

void CoverBare::SetMap(float mapWidth, float mapHeight) {
  mapWidth_ = mapWidth;
  mapHeight_ = mapHeight;
  width_ = std::max(1, static_cast<int>(std::ceil(mapWidth * 2)));
  height_ = std::max(1, static_cast<int>(std::ceil(mapHeight * 2)));
  streams_.clear();
  streamsChanged_ = true;
  streamsLaid = trianglesLaid = 0;
  for (gfx::Texture* t : {&streamTexture_, &picture_})
    if (*t) {
      device_->Destroy(*t);
      *t = {};
    }
  made_ = withDensity_ = withMask_ = false;
}

void CoverBare::AddStream(const Model* model, const Mat4& world, TextureCache* textures) {
  if (!model || model->lods.empty() || !model->lods[0].stream || model->lods[0].streamMesh.empty() || !textures || width_ <= 1) return;
  // its mesh's positions and triangles (an .scm file, as StreamWater reads it)
  auto file = textures->ReadFile(model->lods[0].streamMesh);
  if (!file || file->size() < 48 || file->compare(0, 4, "MODL") != 0) return;
  const std::string& f = *file;
  int32_t h[11];
  std::memcpy(h, f.data() + 4, sizeof h);
  const int64_t vertexOffset = h[3], vertexCount = h[5], indexOffset = h[6], indexCount = h[7];
  if (h[0] != 5 && h[0] != 6) return;
  const int64_t indexBytes = h[0] == 6 ? 4 : 2;
  if (vertexCount <= 0 || indexCount < 3 || vertexOffset < 0 || indexOffset < 0 ||
      vertexOffset + vertexCount * static_cast<int64_t>(sizeof(ScmVertex)) > static_cast<int64_t>(f.size()) ||
      indexOffset + indexCount * indexBytes > static_cast<int64_t>(f.size()))
    return;
  // each vertex where it lies on the map, in the picture's texels
  std::vector<float> at(static_cast<size_t>(vertexCount) * 2);
  for (int64_t k = 0; k < vertexCount; ++k) {
    ScmVertex v;
    std::memcpy(&v, f.data() + vertexOffset + k * static_cast<int64_t>(sizeof(ScmVertex)), sizeof v);
    const float x = world.m[0][0] * v.position[0] + world.m[0][1] * v.position[1] + world.m[0][2] * v.position[2] + world.m[0][3];
    const float z = world.m[2][0] * v.position[0] + world.m[2][1] * v.position[1] + world.m[2][2] * v.position[2] + world.m[2][3];
    at[static_cast<size_t>(k) * 2] = x * 2;
    at[static_cast<size_t>(k) * 2 + 1] = z * 2;
  }
  if (streams_.empty()) streams_.assign(static_cast<size_t>(width_) * static_cast<size_t>(height_), 0);
  // each triangle laid in: the texels whose middle it holds
  auto index = [&](int64_t k) -> int64_t {
    if (indexBytes == 4) {
      uint32_t i;
      std::memcpy(&i, f.data() + indexOffset + k * 4, 4);
      return i;
    }
    uint16_t i;
    std::memcpy(&i, f.data() + indexOffset + k * 2, 2);
    return i;
  };
  for (int64_t t = 0; t + 2 < indexCount; t += 3) {
    const int64_t a = index(t), b = index(t + 1), c = index(t + 2);
    if (a >= vertexCount || b >= vertexCount || c >= vertexCount) continue;
    const float ax = at[a * 2], az = at[a * 2 + 1], bx = at[b * 2], bz = at[b * 2 + 1], cx = at[c * 2], cz = at[c * 2 + 1];
    const float area = (bx - ax) * (cz - az) - (bz - az) * (cx - ax);
    if (std::fabs(area) < 1e-9f) continue;
    const int x0 = std::max(0, static_cast<int>(std::floor(std::min({ax, bx, cx}))));
    const int x1 = std::min(width_ - 1, static_cast<int>(std::ceil(std::max({ax, bx, cx}))));
    const int z0 = std::max(0, static_cast<int>(std::floor(std::min({az, bz, cz}))));
    const int z1 = std::min(height_ - 1, static_cast<int>(std::ceil(std::max({az, bz, cz}))));
    for (int z = z0; z <= z1; ++z)
      for (int x = x0; x <= x1; ++x) {
        const float px = static_cast<float>(x) + 0.5f, pz = static_cast<float>(z) + 0.5f;
        const float w0 = ((bx - px) * (cz - pz) - (bz - pz) * (cx - px)) / area;
        const float w1 = ((cx - px) * (az - pz) - (cz - pz) * (ax - px)) / area;
        const float w2 = 1 - w0 - w1;
        if (w0 >= 0 && w1 >= 0 && w2 >= 0) streams_[static_cast<size_t>(z) * static_cast<size_t>(width_) + static_cast<size_t>(x)] = 255;
      }
    ++trianglesLaid;
  }
  ++streamsLaid;
  streamsChanged_ = true;
}

gfx::Texture CoverBare::Picture(gfx::Texture density, gfx::Texture coverMask, bool remade) {
  if (width_ <= 1 || height_ <= 1) return {};
  if (made_ && !streamsChanged_ && !remade && withDensity_ == static_cast<bool>(density) && withMask_ == static_cast<bool>(coverMask)) return picture_;
  if (tried_ == 0) {
    tried_ = -1;
    gfx::ComputeDesc cd;
    cd.name = "ground cover: where it is bare";
    const std::string source = std::string(kViewShaderBlock) + kBarePass;
    cd.source = source;
    std::string error;
    pass_ = device_->CreateCompute(cd, &error);
    if (!pass_) {
      fprintf(stderr, "render: the ground cover cannot be kept off the streams and trails (%s)\n", error.c_str());
      return {};
    }
    tried_ = 1;
  }
  if (tried_ < 0) return {};
  if (streamsChanged_) {
    // the streams' picture: the ribbons as laid, and round them a margin that thins out over `margin` world units (each
    // texel near a ribbon by the nearest of its texels, within the margin)
    if (streamTexture_) device_->Destroy(streamTexture_);
    streamTexture_ = {};
    std::vector<uint8_t> laid(static_cast<size_t>(width_) * static_cast<size_t>(height_), 0);
    if (!streams_.empty()) {
      const int reach = std::max(1, static_cast<int>(std::ceil(margin * 2)));
      for (int z = 0; z < height_; ++z)
        for (int x = 0; x < width_; ++x) {
          if (!streams_[static_cast<size_t>(z) * static_cast<size_t>(width_) + static_cast<size_t>(x)]) continue;
          for (int dz = -reach; dz <= reach; ++dz)
            for (int dx = -reach; dx <= reach; ++dx) {
              const int tx = x + dx, tz = z + dz;
              if (tx < 0 || tz < 0 || tx >= width_ || tz >= height_) continue;
              const float d = std::sqrt(static_cast<float>(dx * dx + dz * dz)) * 0.5f;  // (world units)
              const float v = std::clamp(1 - d / std::max(margin, 0.01f), 0.0f, 1.0f);
              uint8_t& t = laid[static_cast<size_t>(tz) * static_cast<size_t>(width_) + static_cast<size_t>(tx)];
              t = std::max(t, static_cast<uint8_t>(std::lround(v * 255)));
            }
        }
    }
    gfx::TextureDesc td;
    td.width = width_;
    td.height = height_;
    td.format = gfx::Format::R8;
    td.data = laid.data();
    td.dataSize = laid.size();
    streamTexture_ = device_->CreateTexture(td);
    streamsChanged_ = false;
  }
  if (!picture_) {
    gfx::TargetDesc td;
    td.storage = true;
    td.width = width_;
    td.height = height_;
    td.format = gfx::Format::R8;
    picture_ = device_->CreateTarget(td);
  }
  if (!picture_ || !streamTexture_) return {};
  BareConstants k{};
  k.info[0] = mapWidth_;
  k.info[1] = mapHeight_;
  // (the trails found from the table only where the map has no cover mask: where it has one, its painter has drawn them,
  // and it is in the table already; without a table the mask is laid in here)
  k.info[2] = density && !coverMask && trails > 0 ? 1.0f : 0.0f;
  k.info[3] = trails;
  k.more[0] = around;
  k.more[1] = coverMask && !density ? 1.0f : 0.0f;
  device_->SetPipeline(pass_);
  device_->SetConstants(1, &k, sizeof k);
  device_->SetTexture(0, streamTexture_);
  device_->SetTexture(1, density ? density : streamTexture_);
  device_->SetTexture(2, coverMask ? coverMask : streamTexture_);
  device_->SetStorage(0, picture_);
  device_->Dispatch(static_cast<uint32_t>((width_ + 7) / 8), static_cast<uint32_t>((height_ + 7) / 8), 1);
  device_->SetStorage(0, {});
  device_->SetTexture(0, {});
  device_->SetTexture(1, {});
  device_->SetTexture(2, {});
  made_ = true;
  withDensity_ = static_cast<bool>(density);
  withMask_ = static_cast<bool>(coverMask);
  return picture_;
}

}  // namespace render
