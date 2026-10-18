#include "deep_tracks.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "bp_reader.h"
#include "models.h"
#include "textures.h"
#include "view.h"

namespace render {
namespace {

constexpr int kNearSize = 2048, kFarSize = 2048, kMapSize = 1024;
constexpr float kNearTexel = 1.0f / 64, kFarTexel = 1.0f / 16;
constexpr size_t kMostQuads = 12000;  // a frame's quads at most (stamps beyond wait for the next frame)

// Stamps (additive: each adds how much it presses down and pushes up), clears (zero) and the fading
// (multiplied), into the atlas. A stamp's quad is its shape with room round it for the berm.
const char kShader[] = R"hlsl(
struct VIn {
  float2 position : POSITION;
  float2 across : TEXCOORD0;
  float4 amounts : TEXCOORD1;  // how much it presses down, pushes up; its texels across (a picture's); unused
  float4 shape : TEXCOORD2;    // 0 a picture, 1 a disc, 2 two bands; the bands' middles, half widths; the quad's margin
  float4 region : TEXCOORD3;   // the atlas texels it may write: x0, y0, x1, y1
};
struct VOut {
  float4 position : SV_Position;
  float2 across : TEXCOORD0;
  nointerpolation float4 amounts : TEXCOORD1;
  nointerpolation float4 shape : TEXCOORD2;
  nointerpolation float4 region : TEXCOORD3;
};
VOut VS(VIn i) {
  VOut o;
  o.position = float4(i.position, 0, 1);
  o.across = i.across;
  o.amounts = i.amounts;
  o.shape = i.shape;
  o.region = i.region;
  return o;
}
bool Outside(VOut i) { return any(i.position.xy < i.region.xy) || any(i.position.xy >= i.region.zw); }

Texture2D print : register(t0);
// A print's alpha at a place of it (nothing outside the picture), at a level of its mips. A tread's piece fades
// out only to its sides: along its length it goes on into the next piece, laid end to end with it, and a fading
// there would leave a seam across the track at every join.
float PrintAlpha(float2 uv, float lod, bool tread) {
  float inside = saturate((tread ? min(uv.x, 1 - uv.x) : min(min(uv.x, uv.y), min(1 - uv.x, 1 - uv.y))) * 40);
  return inside > 0 ? print.SampleLevel(samplerLinearClamp, uv, lod).a * inside : 0;
}
float Band(float alpha) { return smoothstep(0.05, 0.3, alpha); }

float4 PSStamp(VOut i) : SV_Target {
  if (Outside(i)) discard;
  float2 p = i.across * i.shape.w;  // (in the shape's own -1..1)
  float trough, ring;
  if (i.shape.x < 0.5) {
    // a print's picture: its blurred alpha the band pressed down, its sharp alpha the cleats deeper in it, the
    // band blurred wider the berm beside it (a tread's: to its sides only, pushed out of the way it went; and the
    // piece cut sharp at its ends, where the pieces before and after it go on)
    uint w, h, levels;
    print.GetDimensions(0, w, h, levels);
    float fine = log2(max(float(w) / max(i.amounts.z, 1), 1));
    float soft = clamp(fine + 2, fine, max(log2(float(w)) - 3, fine));
    float2 uv = p * 0.5 + 0.5;
    bool tread = i.amounts.w > 0.5;
    float2 band_uv = uv;
    if (tread) {
      // a tread's piece is the strip of its picture the alpha covers (shape.y to shape.z of its height), laid end to
      // end with the next: its cleats read along the strip, its band (and the berm) across the strip's middle row,
      // at a level of the picture's mips that keeps the strip two rows high, so that it does not fade at its ends
      if (uv.y < 0 || uv.y >= 1) discard;
      uv.y = lerp(i.shape.y, i.shape.z, uv.y);
      band_uv = float2(uv.x, (i.shape.y + i.shape.z) * 0.5);
      soft = min(soft, max(log2(max((i.shape.z - i.shape.y) * float(h) * 0.5, 1)), fine));
    }
    float band = Band(PrintAlpha(band_uv, soft, tread));
    trough = band * lerp(0.45, 1.0, smoothstep(0.15, 0.6, PrintAlpha(uv, fine, tread)));
    float near = 0, far = 0;
    if (tread) {
      [unroll] for (int k = -1; k <= 1; k += 2) {
        near += Band(PrintAlpha(band_uv + float2(k * 0.06, 0), soft, true)) * 4;
        far += Band(PrintAlpha(band_uv + float2(k * 0.12, 0), soft, true)) * 4;
      }
    } else {
      [unroll] for (int k = 0; k < 8; ++k) {
        float a = k * 0.7853982;
        float2 d = float2(cos(a), sin(a));
        near += Band(PrintAlpha(uv + d * 0.06, soft, false));
        far += Band(PrintAlpha(uv + d * 0.12, soft, false));
      }
    }
    ring = saturate(((near * 0.6 + far * 0.4) / 8 - band) * 2.4);
  } else if (i.shape.x < 1.5) {  // a soft disc, a ring of berm round it
    float r = length(p);
    trough = 1 - smoothstep(0.55, 1.0, r);
    ring = smoothstep(0.8, 1.05, r) * (1 - smoothstep(1.05, 1.4, r));
  } else {  // two bands along its length, berms beside each
    float d = abs(abs(p.x) - i.shape.y), half = i.shape.z;
    float along = 1 - smoothstep(0.75, 1.0, abs(p.y));
    trough = (1 - smoothstep(half * 0.6, half, d)) * along;
    ring = smoothstep(half * 0.9, half * 1.2, d) * (1 - smoothstep(half * 1.2, half * 2.2, d)) * along;
  }
  return float4(trough * i.amounts.x, ring * i.amounts.y, 0, 1);
}

float4 PSClear(VOut i) : SV_Target {
  if (Outside(i)) discard;
  return 0;
}

// The fading: each texel multiplied by what it keeps of itself since the level was last faded. A slow fade
// keeps nearly all of it, which half floats cannot hold apart from all of it: there a texel instead loses a
// twenty-fifth at once, by chance as often as the fade asks. The chance is drawn once for the whole level
// from the count of fadings, so that neighbours fade alike (a chance of each place's own would leave a track
// grainy as it fades, its texels some hundredths apart, which its relief would show) and a texel's copies on
// the edges with it.
cbuffer Fade : register(b1) {
  float4 window;   // x, z of the level's window's first texel (whole texels of the world); its texels across; 1 = it wraps
  float4 region;   // the atlas texels of its region: x0, y0 (its copied edge's, when it wraps), x1, y1
  float4 rates;    // x: seconds since it was last faded; y: what falling snow adds to the fading (1 / s); z: rain's; w: the count
  float4 mapInfo;  // x, y: 1 / the map's width and height; z: the level's texel (world units); w: 1 = the kinds are given
};
Texture2D kinds : register(t1);  // Terrain::GroundKinds: sand, grass, snow (a layer of it), hard ground
uint FadeHash(uint x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}
float4 PSFade(VOut i) : SV_Target {
  if (any(i.position.xy < region.xy) || any(i.position.xy >= region.zw)) discard;
  int2 local = int2(i.position.xy - region.xy);
  int n = int(window.z);
  int2 world = local;
  if (window.w > 0.5) {
    int2 k = ((local - 1) % n + n) % n;  // (the copied edges: the texels of the far side)
    int2 o = int2(window.xy);
    world = o + ((k - o) % n + n) % n;
  }
  float4 kind = 0;
  if (mapInfo.w > 0.5) kind = kinds.SampleLevel(samplerLinearClamp, (world + 0.5) * mapInfo.z * mapInfo.xy, 0);
  // how fast tracks go (1 / seconds): dirt and mud in a quarter of an hour, sand blown smooth in two minutes, grass
  // risen again in four, a snow layer drifted over in ten, scuffs on rock in two; then falling snow and rain
  float rest = saturate(1 - kind.x - kind.y - kind.z - kind.w);
  float rate = rest / 900 + kind.x / 120 + kind.y / 240 + kind.z / 600 + kind.w / 120 + rates.y + rates.z;
  float keep = exp(-rates.x * rate);
  const float drop = 0.04;
  if (1 - keep < drop) {
    uint h = FadeHash(uint(rates.w) * 83492791u + 0x68e31da4u);
    keep = float(h & 0xffffffu) / 16777216.0 < (1 - keep) / drop ? 1 - drop : 1;
  }
  return float4(keep, keep, 1, 1);
}
)hlsl";

struct FadeConstants {
  float window[4], region[4], rates[4], mapInfo[4];
};

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// The rows of a print's picture its alpha covers: the first and the last row of a small level of its mips where it is
// more than a fifth (shares of the picture's height from its top). The game's tread pictures differ: some are a
// tread's whole length, some a short strip at the bottom of a large square (laid often, overlapping). DXT5 and plain
// 32-bit pictures; others: all of it.
struct AlphaSpan {
  float first = 0, last = 1;
};
AlphaSpan AlphaLength(const std::string& file) {
  if (file.size() < 128 || std::memcmp(file.data(), "DDS ", 4) != 0) return {};
  auto word = [&](size_t at) {
    uint32_t v;
    std::memcpy(&v, file.data() + at, 4);
    return v;
  };
  const uint32_t height = word(12), width = word(16), mips = std::max(word(28), 1u);
  const uint32_t flags = word(80), fourCC = word(84), bits = word(88), alphaMask = word(104);
  const bool dxt5 = (flags & 4) && fourCC == 0x35545844u;  // "DXT5"
  const bool plain = !(flags & 4) && bits == 32 && alphaMask != 0;
  if ((!dxt5 && !plain) || width == 0 || height == 0) return {};
  // the level nearest 64 high, as far as the file holds it
  size_t at = 128;
  uint32_t w = width, h = height, level = 0;
  auto bytes = [&](uint32_t lw, uint32_t lh) { return dxt5 ? static_cast<size_t>(std::max(1u, (lw + 3) / 4)) * std::max(1u, (lh + 3) / 4) * 16 : static_cast<size_t>(lw) * lh * 4; };
  while (h > 64 && level + 1 < mips && at + bytes(w, h) < file.size()) {
    at += bytes(w, h);
    w = std::max(w / 2, 1u);
    h = std::max(h / 2, 1u);
    ++level;
  }
  if (at + bytes(w, h) > file.size()) return {};
  std::vector<float> rows(h, 0.0f);
  if (dxt5) {
    const uint32_t bw = std::max(1u, (w + 3) / 4), bh = std::max(1u, (h + 3) / 4);
    for (uint32_t by = 0; by < bh; ++by)
      for (uint32_t bx = 0; bx < bw; ++bx) {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(file.data() + at + (static_cast<size_t>(by) * bw + bx) * 16);
        float a[8];
        a[0] = b[0] / 255.0f;
        a[1] = b[1] / 255.0f;
        if (b[0] > b[1]) {
          for (int k = 1; k < 7; ++k) a[k + 1] = ((7 - k) * a[0] + k * a[1]) / 7;
        } else {
          for (int k = 1; k < 5; ++k) a[k + 1] = ((5 - k) * a[0] + k * a[1]) / 5;
          a[6] = 0;
          a[7] = 1;
        }
        uint64_t indices = 0;
        for (int k = 0; k < 6; ++k) indices |= static_cast<uint64_t>(b[2 + k]) << (8 * k);
        for (uint32_t py = 0; py < 4; ++py)
          for (uint32_t px = 0; px < 4; ++px) {
            const uint32_t y = by * 4 + py;
            if (y >= h || bx * 4 + px >= w) continue;
            rows[y] = std::max(rows[y], a[(indices >> (3 * (py * 4 + px))) & 7]);
          }
      }
  } else {
    int shift = 0;
    while (shift < 32 && !((alphaMask >> shift) & 1)) ++shift;
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x) rows[y] = std::max(rows[y], ((word(at + (static_cast<size_t>(y) * w + x) * 4) & alphaMask) >> shift) / 255.0f);
  }
  int first = -1, last = -1;
  for (uint32_t y = 0; y < h; ++y)
    if (rows[y] > 0.2f) {
      if (first < 0) first = static_cast<int>(y);
      last = static_cast<int>(y);
    }
  AlphaSpan span;
  if (first < 0) return span;
  span.first = static_cast<float>(first) / static_cast<float>(h);
  span.last = std::max(static_cast<float>(last + 1) / static_cast<float>(h), span.first + 0.05f);
  return span;
}

}  // namespace

DeepTracks::~DeepTracks() {
  for (gfx::Pipeline p : {stamp_, clear_, fade_})
    if (p) device_->Destroy(p);
  if (atlas_) device_->Destroy(atlas_);
  if (buffer_) device_->Destroy(buffer_);
}

bool DeepTracks::Init(std::string* error) {
  const std::string source = std::string(kViewShaderBlock) + kShader;
  gfx::PipelineDesc pd;
  pd.name = "deep tracks: pressed";
  pd.vertexSource = pd.pixelSource = source;
  pd.pixelEntry = "PSStamp";
  pd.attrs = {{"POSITION", 0, gfx::AttrType::Float2, offsetof(Vertex, x)},
              {"TEXCOORD", 0, gfx::AttrType::Float2, offsetof(Vertex, u)},
              {"TEXCOORD", 1, gfx::AttrType::Float4, offsetof(Vertex, trough)},
              {"TEXCOORD", 2, gfx::AttrType::Float4, offsetof(Vertex, shape)},
              {"TEXCOORD", 3, gfx::AttrType::Float4, offsetof(Vertex, x0)}};
  pd.stride = sizeof(Vertex);
  pd.cull = gfx::Cull::None;
  pd.depthTest = pd.depthWrite = false;
  pd.depthTarget = false;
  pd.blend = gfx::Blend::Additive;
  pd.colorFormat = gfx::Format::RG16F;
  stamp_ = device_->CreatePipeline(pd, error);
  if (!stamp_) return false;
  pd.name = "deep tracks: cleared";
  pd.pixelEntry = "PSClear";
  pd.blend = gfx::Blend::Opaque;
  clear_ = device_->CreatePipeline(pd, error);
  if (!clear_) return false;
  pd.name = "deep tracks: faded";
  pd.pixelEntry = "PSFade";
  pd.blend = gfx::Blend::Multiply;
  fade_ = device_->CreatePipeline(pd, error);
  if (!fade_) return false;
  gfx::BufferDesc bd;
  bd.stride = sizeof(Vertex);
  bd.dynamic = true;
  buffer_ = device_->CreateBuffer(bd);
  // the atlas: level 0 and level 1 each with their copied edges, then the map's level
  levels_[0].size = kNearSize;
  levels_[0].texel = kNearTexel;
  levels_[0].wraps = true;
  levels_[0].atlasX = 0;
  levels_[1].size = kFarSize;
  levels_[1].texel = kFarTexel;
  levels_[1].wraps = true;
  levels_[1].atlasX = kNearSize + 2;
  levels_[2].size = kMapSize;
  levels_[2].wraps = false;
  levels_[2].atlasX = kNearSize + 2 + kFarSize + 2;
  atlasWidth_ = levels_[2].atlasX + kMapSize;
  atlasHeight_ = std::max({kNearSize + 2, kFarSize + 2, kMapSize});
  gfx::TargetDesc td;
  td.width = atlasWidth_;
  td.height = atlasHeight_;
  td.format = gfx::Format::RG16F;
  atlas_ = device_->CreateTarget(td);
  if (!atlas_ || !buffer_) {
    if (error) *error = "the deep tracks' field cannot be made";
    return false;
  }
  fresh_ = true;
  return true;
}

void DeepTracks::SetMap(float width, float height) {
  mapWidth_ = width;
  mapHeight_ = height;
  levels_[2].texel = std::max(std::max(width, height), 1.0f) / static_cast<float>(kMapSize);
  for (Level& l : levels_) {
    l.placed = false;
    l.fadedAt = -1;
  }
  fresh_ = true;
  stamps_.clear();
  clears_.clear();
  moving_.clear();
  feet_.clear();
  printers_.clear();
  lines_.clear();
  walkers_.clear();
  lastPressed_ = -1e30;
}

float DeepTracks::RegionX1(int level) const {
  const Level& l = levels_[level];
  return static_cast<float>(l.atlasX + l.size + (l.wraps ? 2 : 0));
}

void DeepTracks::Begin(Vec3 target, double clock) {
  ++frame_;
  clock_ = clock;
  if (!atlas_) return;
  // levels 0 and 1 round what the camera looks at, in whole texels; what comes into a window is cleared
  for (int i = 0; i < 2; ++i) {
    Level& l = levels_[i];
    const int ox = static_cast<int>(std::floor(target.x / l.texel)) - l.size / 2, oz = static_cast<int>(std::floor(target.z / l.texel)) - l.size / 2;
    if (!l.placed) {
      l.originX = ox, l.originZ = oz, l.placed = true;
      clears_.push_back({i, 1, 0, 0, 0});
      continue;
    }
    if (ox == l.originX && oz == l.originZ) continue;
    const int dx = ox - l.originX, dz = oz - l.originZ;
    if (std::abs(dx) >= l.size || std::abs(dz) >= l.size) {
      clears_.push_back({i, 1, 0, 0, 0});
    } else {
      if (dx > 0) clears_.push_back({i, l.originX + l.size, oz, ox + l.size, oz + l.size});
      if (dx < 0) clears_.push_back({i, ox, oz, l.originX, oz + l.size});
      if (dz > 0) clears_.push_back({i, ox, l.originZ + l.size, ox + l.size, oz + l.size});
      if (dz < 0) clears_.push_back({i, ox, oz, ox + l.size, l.originZ});
    }
    l.originX = ox, l.originZ = oz;
  }
  if (moving_.size() > 40000) moving_.clear();  // (ids pile up over a long game)
  if (feet_.size() > 20000) feet_.clear();
  if (printers_.size() > 40000) printers_.clear();
  if (lines_.size() > 20000) lines_.clear();
  if (walkers_.size() > 40000) walkers_.clear();
}

const std::vector<uint32_t>& DeepTracks::FootBones(const Model* model, const std::string& blueprintPath) {
  auto found = footBones_.find(model);
  if (found != footBones_.end()) return found->second;
  std::vector<uint32_t>& list = footBones_[model];
  if (!model || !Info(blueprintPath).walks) return list;  // (only a walker's)
  const std::vector<std::string>& named = Info(blueprintPath).feet;
  const size_t n = model->bones.size();
  std::vector<std::string> names(n);
  for (size_t b = 0; b < n; ++b) names[b] = Lower(model->bones[b].name);
  std::vector<uint32_t> candidates;
  // the bones the game's scripts lay footprints at
  for (const std::string& f : named)
    for (size_t b = 0; b < n; ++b)
      if (names[b] == f) candidates.push_back(static_cast<uint32_t>(b));
  const bool given = !candidates.empty();
  // else those named as feet; else the lowest bones of the legs (a bone named as a leg's that no other leg's bone
  // hangs from: the game's bots, whose legs end in a shin)
  if (candidates.empty())
    for (size_t b = 0; b < n; ++b)
      if (names[b].find("foot") != std::string::npos) candidates.push_back(static_cast<uint32_t>(b));
  if (candidates.empty()) {
    std::vector<bool> hasLeg(n, false);
    for (size_t b = 0; b < n; ++b) {
      const int up = model->bones[b].parent;
      if (names[b].find("leg") != std::string::npos && up >= 0 && static_cast<size_t>(up) < n) hasLeg[static_cast<size_t>(up)] = true;
    }
    for (size_t b = 0; b < n; ++b)
      if (names[b].find("leg") != std::string::npos && !hasLeg[b]) candidates.push_back(static_cast<uint32_t>(b));
  }
  // one a foot, low on the model at rest (not an arm's bone of the same name): of bones close together the lowest
  const Vec3 lo = model->boundsMin, hi = model->boundsMax;
  const float apart = 0.12f * std::max(hi.x - lo.x, hi.z - lo.z);
  std::sort(candidates.begin(), candidates.end(), [&](uint32_t a, uint32_t b) { return model->bones[a].position.y < model->bones[b].position.y; });
  for (uint32_t b : candidates) {
    const Vec3 p = model->bones[b].position;
    if (!given && p.y > lo.y + 0.4f * (hi.y - lo.y)) continue;
    bool near = false;
    for (uint32_t k : list) {
      const Vec3 q = model->bones[k].position;
      near = near || (p.x - q.x) * (p.x - q.x) + (p.z - q.z) * (p.z - q.z) < apart * apart;
    }
    if (!near && list.size() < 8) list.push_back(b);
  }
  if (debug_) {
    printf("render: deep tracks: %s's feet:", blueprintPath.c_str());
    for (uint32_t b : list) printf(" %s", model->bones[b].name.c_str());
    printf("\n");
  }
  return list;
}

const DeepTracks::UnitInfo& DeepTracks::Info(const std::string& blueprintPath) {
  auto found = infos_.find(blueprintPath);
  if (found != infos_.end()) return found->second;
  UnitInfo& info = infos_[blueprintPath];
  BpValue bp;
  auto file = textures_->ReadFile(blueprintPath);
  if (file && ReadBlueprintFile(*file, &bp)) {
    const std::string motion = bp["Physics"]["MotionType"].StringOr(std::string());
    info.hovers = motion == "RULEUMT_Hover" || bp["Physics"]["Elevation"].NumberOr(0) > 0.05;
    info.flies = motion == "RULEUMT_Air";
    // (a walker: footfalls of its own, or a walking animation; every land unit of the game without tread marks
    // or footfalls has one: the bots)
    info.walks = bp["Display"]["MovementEffects"]["Land"]["Footfall"].IsTable() || !bp["Display"]["AnimationWalk"].StringOr(std::string()).empty();
    for (const BpValue& b : bp["Display"]["MovementEffects"]["Land"]["Footfall"]["Bones"].items) {
      const std::string bone = b["FootBone"].StringOr(std::string());
      if (!bone.empty()) info.feet.push_back(Lower(bone));
    }
  }
  return info;
}

const DeepTracks::Picture& DeepTracks::PictureOf(const std::string& name) {
  auto found = pictures_.find(name);
  if (found != pictures_.end()) return found->second;
  Picture& p = pictures_[name];
  p.texture = picture ? picture(name) : gfx::Texture{};
  if (p.texture) {  // (the share of its length the alpha covers: its file read once more)
    std::string path = name;
    AlphaSpan span;
    if (path.empty() || path[0] != '/') {
      for (const char* folder : {"/env/common/splats/", "/env/common/decals/"}) {
        auto file = textures_->ReadFile(std::string(folder) + name + ".dds");
        if (file) {
          span = AlphaLength(*file);
          break;
        }
      }
    } else if (auto file = textures_->ReadFile(path)) {
      span = AlphaLength(*file);
    }
    p.first = span.first;
    p.last = std::min(span.last, 1.0f);
  }
  return p;
}

void DeepTracks::AddUnit(uint32_t id, const std::string& blueprintPath, Vec3 position, const Quat& orientation, Vec3 size, const Vec3* feet,
                         int footCount) {
  if (!on || !atlas_) return;
  const UnitInfo& info = Info(blueprintPath);
  walkers_[id] = info.walks;
  if (debug_ && frame_ % 60 == 0)
    printf("render: deep tracks: unit %u (%s): hovers %d flies %d walks %d, %d feet, at %.2f,%.2f, %.2f over the ground %.2f, prints %d\n", id, blueprintPath.c_str(),
           info.hovers, info.flies, info.walks, footCount, position.x, position.z, position.y, groundHeight ? groundHeight(position.x, position.z) : 0.0f,
           printers_.count(id) ? 1 : 0);
  if (info.hovers || info.flies) return;
  {  // a unit that lays prints: they are its track
    auto p = printers_.find(id);
    if (p != printers_.end() && clock_ - p->second < 3) return;
  }
  if (groundHeight) {  // (on the ground, not in the water)
    const float ground = groundHeight(position.x, position.z);
    if (ground < waterLevel - 0.05f || position.y - ground > 0.35f) return;
  }
  const float width = std::max(size.x, 0.1f), length = std::max(size.z, 0.1f);
  const float weight = std::clamp(std::sqrt(width * length) / 0.55f, 0.7f, 2.2f);
  const Vec3 forward = Rotate(orientation, {0, 0, 1});
  const float heading = std::atan2(forward.x, forward.z);
  if (info.walks) {
    // a walker's feet: a footprint where one is set down (it stops going down, as low as the lowest of them, while
    // the unit goes on); one print a step (it is set down again once it has risen)
    footCount = std::clamp(footCount, 0, 8);
    Feet& f = feet_[id];
    const bool fresh = f.seen == 0 || f.seen + 3 < frame_ || f.count != footCount;
    const float moved = std::sqrt((position.x - f.x) * (position.x - f.x) + (position.z - f.z) * (position.z - f.z));
    f.seen = frame_;
    f.x = position.x, f.z = position.z;
    f.count = footCount;
    const float lift = std::max(0.04f * size.y, 0.02f);
    float heights[8];
    float lowest = 1e30f;
    for (int k = 0; k < footCount; ++k) {
      heights[k] = feet[k].y - (groundHeight ? groundHeight(feet[k].x, feet[k].z) : position.y);
      lowest = std::min(lowest, heights[k]);
    }
    for (int k = 0; k < footCount && !fresh; ++k) {
      const float h = heights[k];
      if (h < f.height[k] - 1e-4f) {
        f.falling[k] = true;
      } else if (f.falling[k]) {  // (it stopped going down)
        f.falling[k] = false;
        if (!f.pressed[k] && f.height[k] < lowest + lift && moved > 1e-4f && moved < 30) {
          Footprint(feet[k], heading, width, weight);
          f.pressed[k] = true;
          f.pressedAt[k] = f.height[k];
          f.stepped = clock_;
        }
      }
      if (f.pressed[k] && h > f.pressedAt[k] + lift) f.pressed[k] = false;
    }
    for (int k = 0; k < footCount; ++k) f.height[k] = heights[k];
    if (clock_ - f.stepped < 1.5) return;  // (its feet are its track)
    // its feet not seen setting down (no bones for them, or a pose that does not show them): prints left and right
    // by turns, a stride apart, as it goes
    if (fresh || moved > 30) {
      f.walked = 0;
      return;
    }
    f.walked += moved;
    const float stride = std::max(0.3f * length, 0.08f);
    if (f.walked >= stride) {
      f.walked = std::fmod(f.walked, stride);
      f.side = -f.side;
      Footprint(position + Rotate(orientation, {f.side * 0.2f * width, 0, 0}), heading, width, weight);
    }
    return;
  }
  Moving& m = moving_[id];
  const bool fresh = m.seen == 0 || m.seen + 3 < frame_;
  m.seen = frame_;
  if (fresh) {
    m.x = position.x, m.z = position.z;
    return;
  }
  // a vehicle: a stamp each time it has gone a step on, the way between filled
  const float step = std::max(0.25f * length, 0.06f);
  const float dx = position.x - m.x, dz = position.z - m.z, far = std::sqrt(dx * dx + dz * dz);
  if (far < step) return;
  if (far > 30) {  // (carried or made anew elsewhere: no track between)
    m.x = position.x, m.z = position.z;
    return;
  }
  const int count = std::min(static_cast<int>(far / step), 8);
  for (int k = 1; k <= count; ++k) {
    const float t = static_cast<float>(k) / static_cast<float>(count);
    Stamp s{};
    s.x = m.x + dx * t;
    s.z = m.z + dz * t;
    s.heading = heading;
    // two bands, each place under them pressed as the whole length goes over
    s.shape = 2;
    s.halfX = 0.5f * width;
    s.halfZ = 0.45f * length;
    s.bandX = 0.68f;
    s.bandHalf = 0.17f;
    const float coverage = std::max(2 * s.halfZ * 0.9f / (far / static_cast<float>(count)), 1.0f);
    s.trough = 0.9f * weight / coverage;
    s.berm = 0.55f * weight / coverage;
    stamps_.push_back(s);
  }
  m.x = position.x, m.z = position.z;
  lastPressed_ = clock_;
}

// A walker's footprint (one it does not lay itself): a sole pressed in, the ground pushed up round it.
void DeepTracks::Footprint(Vec3 at, float heading, float width, float weight) {
  Stamp s{};
  s.x = at.x;
  s.z = at.z;
  s.heading = heading;
  s.shape = 1;
  s.halfX = 0.07f * width;
  s.halfZ = 0.1f * width;
  s.trough = 0.9f * weight;
  s.berm = 0.5f * weight;
  stamps_.push_back(s);
  lastPressed_ = clock_;
}

bool DeepTracks::AddPrint(uint32_t id, Vec3 centre, float sizeX, float sizeZ, float heading, const std::string& texture) {
  if (debug_ && printers_.count(id) == 0) printf("render: deep tracks: unit %u lays %s, %.2f x %.2f\n", id, texture.c_str(), sizeX, sizeZ);
  if (!on || !atlas_ || sizeX == 0 || sizeZ == 0) return false;
  const Picture& pic = PictureOf(texture);
  if (!pic.texture) return false;
  printers_[id] = clock_;
  lastPressed_ = clock_;
  sizeX = std::fabs(sizeX), sizeZ = std::fabs(sizeZ);
  const float weight = std::clamp(std::sqrt(sizeX * sizeZ * (pic.last - pic.first)) / 0.5f, 0.75f, 2.2f);
  Stamp s{};
  s.x = centre.x;
  s.z = centre.z;
  s.halfX = sizeX * 0.5f;
  s.halfZ = sizeZ * 0.5f;
  s.heading = heading;
  s.trough = 0.9f * weight;
  s.berm = 0.6f * weight;
  s.shape = 0;
  s.texture = pic.texture;
  // a walker's prints are footprints, each where it is laid; by the name where the unit is not known yet
  auto walker = walkers_.find(id);
  const std::string low = walker == walkers_.end() ? Lower(texture) : std::string();
  const bool foot = walker != walkers_.end() ? walker->second : low.find("print") != std::string::npos || low.find("foot") != std::string::npos;
  if (foot) {
    stamps_.push_back(s);
    return true;
  }
  // a tread's piece: the line of them it continues (the same picture, the line's end near). A piece is the strip of the
  // picture its alpha covers, where the script lays it; the line is laid a whole piece at a time, end to end from where
  // its last piece ends toward where this one's front is, each turned along the way it goes: the track has no gaps and
  // no overlaps, so the cleats' pattern runs on unbroken (pieces laid where the script lays them, every fifth of a
  // second or so, would overlap or leave gaps by the unit's speed); what is left short of a piece waits for the next
  // print.
  const float piece = std::max(sizeZ * (pic.last - pic.first), 0.02f), off = ((pic.first + pic.last) * 0.5f - 0.5f) * sizeZ;
  const float fx = std::sin(heading), fz = std::cos(heading);
  s.tread = true;
  s.bandX = pic.first;
  s.bandHalf = pic.last;
  s.halfZ = piece * 0.5f;
  s.x = centre.x + fx * off;  // (the strip's middle)
  s.z = centre.z + fz * off;
  const float frontX = s.x + fx * piece * 0.5f, frontZ = s.z + fz * piece * 0.5f;
  std::vector<Line>& lines = lines_[id];
  const float reach = 3 * sizeZ + 1.5f;
  Line* line = nullptr;
  float best = reach * reach;
  for (Line& l : lines) {
    const float dx = frontX - l.x, dz = frontZ - l.z, d2 = dx * dx + dz * dz;
    if (l.texture.id == pic.texture.id && clock_ - l.clock < 2 && d2 < best) {
      best = d2;
      line = &l;
    }
  }
  if (!line) {
    lines.erase(std::remove_if(lines.begin(), lines.end(), [&](const Line& l) { return clock_ - l.clock >= 2; }), lines.end());
    if (lines.size() < 8) lines.push_back({frontX, frontZ, clock_, pic.texture});
    stamps_.push_back(s);
    return true;
  }
  line->clock = clock_;
  float dx = frontX - line->x, dz = frontZ - line->z, far = std::sqrt(dx * dx + dz * dz);
  if (debug_ && debugStamps_ < 40)
    printf("render: deep tracks: unit %u's tread: %.3f from its line's end (a piece %.3f long, heading %.2f, way %.2f)\n", id, far, piece, heading,
           std::atan2(dx, dz));
  int laid = 0;
  while (far >= piece && laid < 40) {
    const float ux = dx / far, uz = dz / far;
    if (std::fabs(ux * fx + uz * fz) < 0.6f) {  // (the tread going sideways, as a unit turns on the spot: not laid)
      line->x = frontX, line->z = frontZ;
      break;
    }
    Stamp e = s;
    e.x = line->x + ux * piece * 0.5f;
    e.z = line->z + uz * piece * 0.5f;
    e.heading = std::atan2(ux, uz);
    stamps_.push_back(e);
    line->x += ux * piece;
    line->z += uz * piece;
    dx = frontX - line->x, dz = frontZ - line->z, far = std::sqrt(dx * dx + dz * dz);
    ++laid;
  }
  if (laid == 40) line->x = frontX, line->z = frontZ;  // (a jump: the line goes on from here)
  return true;
}

void DeepTracks::Emit(const Vertex c[4]) {
  for (int k : {0, 1, 2, 0, 2, 3}) vertices_.push_back(c[k]);
}

// A stamp into one level: its corners in the level's texels; a wrapping level takes it where it falls in its
// window and, where it reaches over an edge, again a window's width over (the copies write the copied edges too).
void DeepTracks::Quad(const Stamp& s, int level) {
  const Level& l = levels_[level];
  float halfX = s.halfX, halfZ = s.halfZ, trough = s.trough, berm = s.berm;
  int shape = s.shape;
  // the room the berm needs round the shape
  float margin = shape == 0 ? 1.35f : shape == 1 ? 1.45f : (s.bandX + 2.3f * s.bandHalf);
  margin = std::max(margin, 1.05f);
  if (level == 2 || (shape == 0 && 2 * std::max(halfX, halfZ) < 2 * l.texel)) {
    // too small for the level to hold its shape: a disc as wide as it, or a texel and a half, pressing as much in all
    const float r = std::max(std::max(halfX, halfZ), 0.75f * l.texel);
    const float area = (halfX * halfZ) / (r * r);
    trough *= std::min(area, 1.0f) * (shape == 0 ? 0.6f : 1.0f);
    berm = level == 2 ? 0 : berm * std::min(area, 1.0f) * 0.5f;
    halfX = halfZ = r;
    shape = 1;
    margin = 1.45f;
  }
  const float inv = 1 / l.texel;
  const float c = std::cos(s.heading), sn = std::sin(s.heading);
  const float ax = halfX * margin, az = halfZ * margin;
  // (the corners' offsets from the middle, in texels: as decals turn their footprint)
  const float cornerX[4] = {-1, 1, 1, -1}, cornerZ[4] = {-1, -1, 1, 1};
  float ox[4], oz[4];
  float reachX = 0, reachZ = 0;
  for (int k = 0; k < 4; ++k) {
    const float fx = cornerX[k] * ax, fz = cornerZ[k] * az;
    ox[k] = (fx * c + fz * sn) * inv;
    oz[k] = (-fx * sn + fz * c) * inv;
    reachX = std::max(reachX, std::fabs(ox[k]));
    reachZ = std::max(reachZ, std::fabs(oz[k]));
  }
  float mx = s.x * inv, mz = s.z * inv;  // the middle in whole texels of the world
  const float n = static_cast<float>(l.size);
  if (l.wraps) {
    // (outside the window: nothing)
    if (mx + reachX < static_cast<float>(l.originX) || mx - reachX > static_cast<float>(l.originX) + n || mz + reachZ < static_cast<float>(l.originZ) ||
        mz - reachZ > static_cast<float>(l.originZ) + n)
      return;
    mx = mx - n * std::floor(mx / n) + 1;  // (into the region, after its copied edge)
    mz = mz - n * std::floor(mz / n) + 1;
  }
  const float x0 = static_cast<float>(l.atlasX), x1 = RegionX1(level), y1 = static_cast<float>(l.wraps ? l.size + 2 : l.size);
  const float sx = 2.0f / static_cast<float>(atlasWidth_), sy = 2.0f / static_cast<float>(atlasHeight_);
  const float across = 2 * std::max(halfX, halfZ) * inv;
  const int copies = l.wraps ? 1 : 0;
  for (int a = -copies; a <= copies; ++a)
    for (int b = -copies; b <= copies; ++b) {
      const float px = mx + static_cast<float>(a) * n, pz = mz + static_cast<float>(b) * n;
      if (px + reachX < 0 || px - reachX > x1 - x0 || pz + reachZ < 0 || pz - reachZ > y1) continue;
      Vertex v[4];
      for (int k = 0; k < 4; ++k) {
        const float atlasX = x0 + px + ox[k], atlasY = pz + oz[k];
        v[k] = Vertex{atlasX * sx - 1, 1 - atlasY * sy, cornerX[k], cornerZ[k], trough, berm, across, s.tread ? 1.0f : 0.0f,
                      static_cast<float>(shape), s.bandX, s.bandHalf, margin, x0, 0, x1, y1};
      }
      Emit(v);
      ++stamped;
    }
}

void DeepTracks::Quads(const Stamp& s) {
  for (int level = 0; level < kLevels; ++level) Quad(s, level);
}

// Zero over whole texels of the world [x0, x1) x [z0, z1) of a wrapping level (where they fall in its region, and
// a window's width over).
void DeepTracks::ClearQuad(int level, int x0, int z0, int x1, int z1) {
  const Level& l = levels_[level];
  const float n = static_cast<float>(l.size);
  const float rx0 = static_cast<float>(l.atlasX), rx1 = RegionX1(level), ry1 = n + 2;
  const float sx = 2.0f / static_cast<float>(atlasWidth_), sy = 2.0f / static_cast<float>(atlasHeight_);
  const float w = static_cast<float>(x1 - x0), h = static_cast<float>(z1 - z0);
  const float bx = static_cast<float>(x0) - n * std::floor(static_cast<float>(x0) / n) + 1, bz = static_cast<float>(z0) - n * std::floor(static_cast<float>(z0) / n) + 1;
  for (int a = -1; a <= 1; ++a)
    for (int b = -1; b <= 1; ++b) {
      const float px = bx + static_cast<float>(a) * n, pz = bz + static_cast<float>(b) * n;
      if (px + w < 0 || px > n + 2 || pz + h < 0 || pz > n + 2) continue;
      const float ax0 = rx0 + px, ax1 = rx0 + px + w, az0 = pz, az1 = pz + h;
      Vertex v[4];
      const float xs[4] = {ax0, ax1, ax1, ax0}, zs[4] = {az0, az0, az1, az1};
      for (int k = 0; k < 4; ++k) v[k] = Vertex{xs[k] * sx - 1, 1 - zs[k] * sy, 0, 0, 0, 0, 0, 0, 3, 0, 0, 1, rx0, 0, rx1, ry1};
      Emit(v);
    }
}

void DeepTracks::RegionQuad(int level, float shape) {
  const Level& l = levels_[level];
  const float x0 = static_cast<float>(l.atlasX), x1 = RegionX1(level), y1 = static_cast<float>(l.wraps ? l.size + 2 : l.size);
  const float sx = 2.0f / static_cast<float>(atlasWidth_), sy = 2.0f / static_cast<float>(atlasHeight_);
  const float xs[4] = {x0, x1, x1, x0}, zs[4] = {0, 0, y1, y1};
  Vertex v[4];
  for (int k = 0; k < 4; ++k) v[k] = Vertex{xs[k] * sx - 1, 1 - zs[k] * sy, 0, 0, 0, 0, 0, 0, shape, 0, 0, 1, x0, 0, x1, y1};
  Emit(v);
}

void DeepTracks::Draw(float snowing, float raining, gfx::Texture kinds) {
  stamped = 0;
  faded = -1;
  if (!atlas_) return;
  if (!on) {  // (off: nothing kept of what came meanwhile; the levels round the camera cleared when it is on again)
    stamps_.clear();
    clears_.clear();
    levels_[0].placed = levels_[1].placed = false;
    return;
  }
  vertices_.clear();
  // the fading: one level a frame, each when a fifth of a second (the map's level: a second) has passed since it was
  // last faded; not at all a while after nothing has pressed (all is gone by then)
  FadeConstants fade{};
  size_t fadeEnd = 0;
  {
    const int level = static_cast<int>(fadeCount_ % kLevels);
    Level& l = levels_[level];
    if (l.fadedAt < 0 || clock_ < l.fadedAt) l.fadedAt = clock_;
    const double since = clock_ - l.fadedAt;
    ++fadeCount_;
    if (since >= (level == 2 ? 1.0 : 0.2) && clock_ - lastPressed_ < 3600 && !fresh_ && (l.placed || !l.wraps)) {
      l.fadedAt = clock_;
      faded = level;
      fade.window[0] = static_cast<float>(l.originX);
      fade.window[1] = static_cast<float>(l.originZ);
      fade.window[2] = static_cast<float>(l.size);
      fade.window[3] = l.wraps ? 1.0f : 0.0f;
      fade.region[0] = static_cast<float>(l.atlasX);
      fade.region[1] = 0;
      fade.region[2] = RegionX1(level);
      fade.region[3] = static_cast<float>(l.wraps ? l.size + 2 : l.size);
      fade.rates[0] = static_cast<float>(since);
      fade.rates[1] = std::max(snowing, 0.0f) / 90.0f;  // (heavy snow fills a track in a minute and a half)
      fade.rates[2] = std::max(raining, 0.0f) / 900.0f;
      fade.rates[3] = static_cast<float>(fadeCount_ & 0xffffff);
      fade.mapInfo[0] = 1 / std::max(mapWidth_, 1.0f);
      fade.mapInfo[1] = 1 / std::max(mapHeight_, 1.0f);
      fade.mapInfo[2] = l.texel;
      fade.mapInfo[3] = kinds ? 1.0f : 0.0f;
      RegionQuad(level, 4);
      fadeEnd = vertices_.size();
    }
  }
  // what came into the windows: zero
  for (const auto& c : clears_) {
    if (c[3] <= c[1]) RegionQuad(c[0], 3);  // (the whole region)
    else ClearQuad(c[0], c[1], c[2], c[3], c[4]);
  }
  clears_.clear();
  const size_t clearEnd = vertices_.size();
  // what pressed: by picture (the shapes made in the shader first, with no picture)
  std::stable_sort(stamps_.begin(), stamps_.end(), [](const Stamp& a, const Stamp& b) { return (a.shape ? 0u : a.texture.id) < (b.shape ? 0u : b.texture.id); });
  struct Run {
    size_t first, count;
    gfx::Texture texture;
  };
  std::vector<Run> runs;
  size_t used = 0;
  for (const Stamp& s : stamps_) {
    if (vertices_.size() / 6 >= kMostQuads) break;
    const size_t before = vertices_.size();
    Quads(s);
    ++used;
    if (vertices_.size() == before) continue;
    const gfx::Texture t = s.shape ? gfx::Texture{} : s.texture;
    if (runs.empty() || runs.back().texture.id != t.id) runs.push_back({before, 0, t});
    runs.back().count = vertices_.size() - runs.back().first;
  }
  debugStamps_ += static_cast<int>(used), debugQuads_ += stamped, debugFades_ += faded >= 0 ? 1 : 0;
  if (debug_ && frame_ % 60 == 0) {
    printf("render: deep tracks: frame %u: in the last 60 frames %d stamps, %d quads, %d fadings; level 0 at %d,%d, clock %.2f\n", frame_, debugStamps_,
           debugQuads_, debugFades_, levels_[0].originX, levels_[0].originZ, clock_);
    debugStamps_ = debugQuads_ = debugFades_ = 0;
  }
  stamps_.erase(stamps_.begin(), stamps_.begin() + static_cast<std::ptrdiff_t>(used));
  if (stamps_.size() > 20000) stamps_.clear();  // (a backlog never drawn: dropped)
  if (vertices_.empty() && !fresh_) return;
  gfx::PassDesc pass;
  pass.color = atlas_;
  pass.clearColor = fresh_;
  fresh_ = false;
  device_->BeginPass(pass);
  if (vertices_.empty()) return;
  device_->UpdateBuffer(buffer_, vertices_.data(), static_cast<uint32_t>(vertices_.size() * sizeof(Vertex)));
  device_->SetVertexBuffer(buffer_);
  if (fadeEnd > 0) {
    device_->SetPipeline(fade_);
    device_->SetConstants(1, &fade, sizeof fade);
    device_->SetTexture(1, kinds ? kinds : textures_->Black());
    device_->Draw(static_cast<uint32_t>(fadeEnd), 0);
    device_->SetTexture(1, {});
  }
  if (clearEnd > fadeEnd) {
    device_->SetPipeline(clear_);
    device_->Draw(static_cast<uint32_t>(clearEnd - fadeEnd), static_cast<uint32_t>(fadeEnd));
  }
  if (!runs.empty()) {
    device_->SetPipeline(stamp_);
    for (const Run& r : runs) {
      device_->SetTexture(0, r.texture ? r.texture : textures_->White());
      device_->Draw(static_cast<uint32_t>(r.count), static_cast<uint32_t>(r.first));
    }
    device_->SetTexture(0, {});
  }
}

TrackFieldConstants DeepTracks::Constants() const {
  TrackFieldConstants c;
  if (!atlas_ || !on) return c;
  for (int i = 0; i < kLevels; ++i) {
    const Level& l = levels_[i];
    c.level[i][0] = static_cast<float>(l.originX);
    c.level[i][1] = static_cast<float>(l.originZ);
    c.level[i][2] = 1 / l.texel;
    c.level[i][3] = static_cast<float>(l.size);
    c.atlas[i][0] = static_cast<float>(l.atlasX + (l.wraps ? 1 : 0));
    c.atlas[i][1] = l.wraps ? 1.0f : 0.0f;
    c.atlas[i][2] = l.wraps ? 1.0f : 0.0f;
    c.atlas[i][3] = l.texel;
  }
  c.info[0] = 1.0f / static_cast<float>(atlasWidth_);
  c.info[1] = 1.0f / static_cast<float>(atlasHeight_);
  c.info[2] = 1;
  c.info[3] = static_cast<float>(debugView_);
  return c;
}

}  // namespace render
