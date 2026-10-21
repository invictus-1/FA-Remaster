#include "blasts.h"

#include <cmath>
#include <cstdio>
#include <string>

#include "lighting.h"

namespace render {

// (kept in step with Blasts::Write: three float4s a blast)
const char kBlastShaderBlock[] = R"hlsl(
cbuffer Blasts : register(b1) {
  float4 blastHead;      // x: how many blasts follow
  float4 blastList[96];  // per blast: (x, z, reach, grass's push), (age, the wave's speed, the ground's height, trees' push),
                         // (size, dust 0..1, a number of its own, 0)
};
float2 BlastPush(float2 xz, float tree) {
  float2 push = 0;
  uint n = (uint)blastHead.x;
  // (grass: down within a second, swinging past upright at a beat; trees: slower and less)
  float decay = lerp(0.32, 0.85, tree), beat = lerp(8.0, 4.2, tree), rise = lerp(0.035, 0.12, tree);
  [loop] for (uint i = 0; i < n; ++i) {
    float4 a = blastList[i * 3], b = blastList[i * 3 + 1];
    float2 off = xz - a.xy;
    float d = length(off);
    float since = b.x - d / b.y;  // how long ago the wave passed here
    if (d >= a.z || since <= 0) continue;
    float x = d / a.z;
    float fall = (1 - x) * (1 - x) * (1 + 2 * x);  // (smooth, nothing at the reach)
    float swing = saturate(since / rise) * exp(-since / decay) * cos(since * beat);
    push += off * (lerp(a.w, b.w, tree) * fall * swing / max(d, 0.05));
  }
  return push;
}
)hlsl";

namespace {

// The grass's field: over the grass's square (GroundCover::Area), the waves' push at each texel. A thread a texel.
const char kField[] = R"hlsl(
cbuffer FieldArea : register(b2) {
  float4 area;  // the first cell's x, z; a cell's size; cells a side
};
RWTexture2D<float2> field : register(u0);
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  field.GetDimensions(w, h);
  if (id.x >= w || id.y >= h) return;
  float2 p = area.xy + (id.xy + 0.5) / float2(w, h) * (area.z * area.w);
  field[id.xy] = BlastPush(p, 0);
}
)hlsl";
constexpr int kFieldSize = 256;

// The dust: for each blast that kicks some up, kPuffs soft puffs round a ring that runs out with its wave and slows,
// low over the ground, each a card facing the camera; coloured as the ground under it and lit as the ground is (the
// sun softly, as smoke lets light through, and the sky), seen through the air as the game's smoke is; faded where it
// meets the ground or a unit (as the particles are, by the scene's depth). Six corners a puff, no vertex data.
const char kDust[] = R"hlsl(
cbuffer Dust : register(b2) {
  float4 right;    // the camera's right; w: the camera's near distance (the scene's depth is that over the distance)
  float4 upward;   // the camera's up; w: world units of the heights' largest value
  float4 mapInfo;  // xy: the map's width and height; z: the water's level; w: puffs a blast
};
Texture2D<float> heightMap : register(t0);
Texture2D groundColours : register(t1);  // Terrain::GroundColours: rgb the ground's colour (as stored)
#ifdef MSAA
Texture2DMS<float> sceneDepth : register(t3);
#else
Texture2D<float> sceneDepth : register(t3);
#endif

uint DustHash(uint x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}
float DustRand(uint h) { return float(h & 0xffffffu) / 16777216.0; }
float DustGround(float2 xz) {
  float2 p = clamp(xz, 0, mapInfo.xy);
  float2 f = floor(p);
  float2 t = p - f;
  int2 c = int2(f);
  int2 most = int2(mapInfo.xy);
  float a = heightMap.Load(int3(min(c, most), 0)), b = heightMap.Load(int3(min(c + int2(1, 0), most), 0));
  float d = heightMap.Load(int3(min(c + int2(0, 1), most), 0)), e = heightMap.Load(int3(min(c + int2(1, 1), most), 0));
  return lerp(lerp(a, b, t.x), lerp(d, e, t.x), t.y) * upward.w;
}

struct VOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
  float2 uv : TEXCOORD1;      // -1..1 across the card
  float3 colour : TEXCOORD2;  // lit, linear
  float4 shade : TEXCOORD3;   // x: how much shows (0..1); y: over how many world units it fades where it meets something; z: its
                              // noise's seed; w: its size (half the card's width)
  float3 centre : TEXCOORD4;
  float sunLit : TEXCOORD5;   // the sun's shadow and the clouds' at its middle
};

static const float2 kCorners[6] = {float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1)};

VOut VS(uint id : SV_VertexID) {
  VOut o = (VOut)0;
  o.position = float4(0, 0, -1, 1);  // (no puff: behind the eye)
  uint puffs = uint(mapInfo.w);
  uint blast = id / (puffs * 6), puff = (id / 6) % puffs;
  float2 corner = kCorners[id % 6];
  float4 a = blastList[blast * 3], b = blastList[blast * 3 + 1], c = blastList[blast * 3 + 2];
  float size = c.x, dust = c.y, age = b.x;
  if (dust <= 0 || age <= 0) return o;
  float life = min(1.4 + 0.3 * size, 3.2);
  if (age >= life) return o;
  // (enough that they overlap round the ring, which reads as one ring of dust and not as a ring of dots)
  uint count = uint(clamp(10 + 10 * size, 10, float(puffs)) * saturate(0.5 + dust));
  if (puff >= count) return o;
  uint h = DustHash(asuint(c.z) * 977u + puff * 7919u + 13u);
  // its place on the ring: the ring runs out as fast as the wave and slows to a stop, a little short of where the
  // wave dies; every other puff a little inside it
  float far = min(1.0 + 2.2 * size, 20.0);
  float ring = far * (1 - exp(-age * b.y / far));
  float angle = (puff + 0.6 * DustRand(h)) * 6.2831853 / count;
  float reach = ring * ((puff & 1) ? 0.55 + 0.25 * DustRand(DustHash(h + 1)) : 0.85 + 0.3 * DustRand(DustHash(h + 2)));
  float2 xz = a.xy + float2(cos(angle), sin(angle)) * reach;
  if (any(xz < 0) || any(xz > mapInfo.xy)) return o;
  float ground = DustGround(xz);
  if (ground < mapInfo.z + 0.05) return o;  // (none off water)
  float t = age / life;
  // low and soft: small at first, growing as it spreads; never high (readability: it must not hide units)
  float puffSize = min(0.3 + 0.22 * size, 1.1) * (0.75 + 0.8 * sqrt(t)) * (0.8 + 0.4 * DustRand(DustHash(h + 3)));
  float lift = min(puffSize * 0.4 + age * (0.12 + 0.04 * size), 1.0);
  float3 centre = float3(xz.x, ground + lift, xz.y);
  // (turned a little each, so the cards do not all show the same face)
  float turn = DustRand(DustHash(h + 4)) * 6.2832 + age * (DustRand(DustHash(h + 5)) - 0.5);
  float2 tc = float2(corner.x * cos(turn) - corner.y * sin(turn), corner.x * sin(turn) + corner.y * cos(turn));
  float3 p = centre + (right.xyz * tc.x + upward.xyz * tc.y) * puffSize;
  o.position = mul(viewProj, float4(p, 1));
  o.world = p;
  o.centre = centre;
  o.uv = corner;
  // as much of it shows: kicked up over a third of a second, thinning out
  o.shade.x = dust * 0.55 * smoothstep(0, 0.3, age) * pow(1 - t, 1.5);
  o.shade.y = puffSize * 0.6;
  o.shade.z = DustRand(DustHash(h + 6)) * 17;
  o.shade.w = puffSize;
  o.sunLit = SunShadow(centre, float3(0, 1, 0)) * CloudShadow(centre);
  // its colour: the ground's there, paler and greyer (dust is the dry top of it); where the ground is grassy, the earth
  // under the grass more than the grass (GroundColours' alpha: how grassy)
  float4 ground4 = groundColours.SampleLevel(samplerLinearClamp, xz / mapInfo.xy, 0);
  float3 g = lerp(ToLinear(ground4.rgb), ToLinear(float3(0.43, 0.37, 0.28)), ground4.a * 0.7);
  g = lerp(g, dot(g, float3(0.3, 0.6, 0.1)), 0.25) * 1.15 + 0.01;
  o.colour = g;
  return o;
}

float Meets(VOut i, float soft, out uint coverage) {
  coverage = 0;
  float nearest = 1e9;
#ifdef MSAA
  int2 pixel = int2(i.position.xy);
  for (uint s = 0; s < SAMPLES; ++s) {
    float d = sceneDepth.Load(pixel, s);
    float gap = (d > 0 ? right.w / d : 1e9) - i.position.w;
    if (gap > 0) {
      coverage |= 1u << s;
      nearest = min(nearest, gap);
    }
  }
#else
  float d = sceneDepth.Load(int3(i.position.xy, 0));
  float gap = (d > 0 ? right.w / d : 1e9) - i.position.w;
  if (gap > 0) {
    coverage = 1;
    nearest = gap;
  }
#endif
  return coverage == 0 ? 0 : saturate(nearest / max(soft, 1e-3));
}

float4 PS(VOut i, out uint coverage : SV_Coverage) : SV_Target {
  float r2 = dot(i.uv, i.uv);
  if (r2 >= 1) discard;
  float meets = Meets(i, i.shade.y, coverage);
  if (meets <= 0) discard;
  // a soft round puff, billowing (a noise over it)
  float body = (1 - r2) * (1 - r2);
  float billow = CloudNoise(float3(i.uv * 1.6 + i.shade.z, i.shade.z)) * 0.6 + CloudNoise(float3(i.uv * 3.7 - i.shade.z, 2.3)) * 0.4;
  float alpha = saturate(body * (0.35 + 0.9 * billow) * i.shade.x) * meets;
  // lit as the ground: the sun softly (smoke lets it through) on the side that faces it, the sky from all round
  float3 toward = normalize(cameraPosition - i.centre);
  float3 n = normalize(i.world - i.centre + toward * (sqrt(saturate(1 - r2)) * i.shade.w));  // (a ball's)
  float sunSide = saturate((dot(n, sunDirection) + 0.35) / 1.35);
  float shadow = i.sunLit;
  float3 skyDiffuse = skyZenith.w > 0.5 ? SkyAmbient(n) : ToLinear(environmentSky.SampleLevel(samplerLinear, n, 8).rgb);
  float3 light = ToLinear(sunColor) * (lightMultiplier * modern.x) * (sunSide * lerp(0.35, 1, shadow)) + (skyDiffuse + ToLinear(shadowFill) * 0.5) * modern.y;
  float3 c = ThroughAirTo(i.colour * light, i.world);
  return float4(c, alpha);
}
)hlsl";

}  // namespace

Blasts::~Blasts() {
  for (gfx::Pipeline p : {fieldPass_, dust_})
    if (p) device_->Destroy(p);
  if (field_) device_->Destroy(field_);
}

bool Blasts::Init() {
  if (tried_) return tried_ > 0;
  tried_ = -1;
  std::string error;
  gfx::ComputeDesc cd;
  cd.name = "blasts: the grass's field";
  const std::string field = std::string(kViewShaderBlock) + kBlastShaderBlock + kField;
  cd.source = field;
  fieldPass_ = device_->CreateCompute(cd, &error);
  gfx::PipelineDesc pd;
  pd.name = "blasts: dust";
  const std::string dust = std::string(gSceneSamples > 1 ? "#define MSAA 1\n" : "") + "#define SAMPLES " + std::to_string(gSceneSamples) + "\n" +
                           kViewShaderBlock + kLightingShaderBlock + kBlastShaderBlock + kDust;
  pd.vertexSource = pd.pixelSource = dust;
  pd.cull = gfx::Cull::None;
  pd.depthTest = pd.depthWrite = false;  // (the shader tests the scene's depth itself)
  pd.depthTarget = false;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pd.blend = gfx::Blend::Alpha;
  pd.alphaWrite = false;  // (the original look's glow is left as it is, as the particles leave it)
  if (fieldPass_) dust_ = device_->CreatePipeline(pd, &error);
  gfx::TargetDesc td;
  td.storage = true;
  td.width = td.height = kFieldSize;
  td.format = gfx::Format::RG16F;
  if (dust_) field_ = device_->CreateTarget(td);
  if (!field_) {
    fprintf(stderr, "render: blasts cannot be drawn (%s)\n", error.empty() ? "no storage target" : error.c_str());
    return false;
  }
  tried_ = 1;
  return true;
}

void Blasts::Add(Vec3 at, float size, double born, bool splash, bool hot, float scorch) {
  // one explosion's scorch and flash (and the decal and splat a death may lay) are one blast: near in place and time
  for (auto it = recent_.rbegin(); it != recent_.rend() && !splash; ++it) {
    Blast& b = *it;
    if (b.born < born - 0.5) break;  // (the newest last: older ones are farther back)
    if (b.splash || std::fabs(b.born - born) > 0.4) continue;
    const float dx = b.at.x - at.x, dz = b.at.z - at.z;
    if (dx * dx + dz * dz > std::pow(std::max(0.8f, 0.6f * std::max(b.size, size)), 2.0f)) continue;
    b.size = std::max(b.size, size);
    b.born = std::min(b.born, born);
    b.hot = b.hot || hot;
    b.scorch = std::max(b.scorch, scorch);  // [scars]
    return;
  }
  Blast b;
  b.at = at;
  b.size = size;
  b.born = born;
  b.splash = splash;
  b.hot = hot;
  b.scorch = scorch;  // [scars]
  b.water = groundHeight && groundHeight(at.x, at.z) < waterLevel - 0.05f;
  recent_.push_back(b);
  if (fresh_.size() >= 256) fresh_.erase(fresh_.begin());
  fresh_.push_back(b);
  ++told;
  while (recent_.size() > kKept || (!recent_.empty() && recent_.front().born < born - 30)) recent_.pop_front();
}

void Blasts::FromFlash(Vec3 at, float glowSize, double born, uint32_t entity) {
  if (entity != 0xffffffffu) {
    flashes_.push_back({entity, born});
    while (flashes_.size() > 512 || flashes_.front().second < born - 10) flashes_.pop_front();
  }
  // (the game's death makes its flash 2 to 3 times the unit's radius; a scorch is about the radius across: the same
  // size for both of one death. The smallest flashes - a muzzle's, a spark's - shake nothing.)
  const float size = 0.4f * glowSize;
  if (size >= 0.3f) Add(at, std::min(size, 60.0f), born, false, true);
}

bool Blasts::FlashedOn(uint32_t entity, double since) const {
  for (auto it = flashes_.rbegin(); it != flashes_.rend() && it->second >= since; ++it)
    if (it->first == entity) return true;
  return false;
}

void Blasts::FromScar(Vec3 at, float sizeX, float sizeZ, double born) {
  const float size = 0.5f * (std::fabs(sizeX) + std::fabs(sizeZ));
  if (size >= 0.2f) Add(at, std::min(size, 60.0f), born, false, false, size);
}

void Blasts::FromImpact(Vec3 at, float damageRadius, float damage, double born) {
  const float size = damageRadius > 0 ? 0.85f * damageRadius * std::clamp(0.01f * damage, 0.25f, 1.0f) : std::min(0.15f + damage / 800, 0.6f);
  if (size >= 0.2f) Add(at, std::min(size, 60.0f), born, false, true, size);
}

float Blasts::Reach(float size) const { return std::min(2.5f + 5 * size, 60.0f); }
float Blasts::Speed(float size) const { return std::min(9 + 4 * size, 45.0f); }

float Blasts::Lasts(const Blast& b) const {
  const float front = Reach(b.size) / Speed(b.size);  // (the wave's way out)
  const float waves = front + 3.2f, dust = b.size >= dustFrom ? std::min(1.4f + 0.3f * b.size, 3.2f) : 0.0f;
  return std::max(wavesNow_ ? waves : 0.0f, dustNow_ ? dust : 0.0f);
}

void Blasts::Write(const Blast& b, double clock, float* out) const {
  const float reach = Reach(b.size), speed = Speed(b.size);
  out[0] = b.at.x;
  out[1] = b.at.z;
  out[2] = reach;
  out[3] = std::clamp(0.75f + 0.25f * b.size, 0.75f, 1.3f);  // (grass: nearly flat by the blast, for any blast)
  out[4] = static_cast<float>(clock - b.born);
  out[5] = speed;
  out[6] = b.at.y;
  out[7] = std::clamp(0.03f + 0.08f * b.size, 0.0f, 0.35f);  // (trees: a tank's death sways them a little, a big blast more)
  out[8] = b.size;
  out[9] = dustNow_ && b.size >= dustFrom && !b.water ? std::clamp((b.size - 0.5f) / 1.5f, 0.2f, 1.0f) : 0.0f;
  out[10] = static_cast<float>(static_cast<uint32_t>(b.born * 1000.0) % 100000u);  // (its own number, for the dust's puffs)
  out[11] = 0;
}

void Blasts::Prepare(double clock, const View& view, Vec3 focus, bool waves, bool dust) {
  active_ = dustBlasts_ = 0;
  // (a game started over, or played back from earlier: what blew up later has not yet)
  if (lastClock_ >= 0 && clock < lastClock_ - 2) {
    recent_.clear();
    flashes_.clear();
  }
  lastClock_ = clock;
  wavesNow_ = waves;
  dustNow_ = dust;
  if (!waves && !dust) {
    gpu_[0] = 0;
    return;
  }
  Init();  // (its shaders made now, the first frame it is wanted, not at the first blast)
  // the blasts under way that reach into the view, the nearest to what the camera looks at first
  std::vector<std::pair<float, const Blast*>> under;
  for (const Blast& b : recent_) {
    if (b.splash) continue;
    const double age = clock - b.born;
    if (age < 0 || age > Lasts(b)) continue;
    if (!waves && b.size < dustFrom) continue;
    const float reach = Reach(b.size);
    if (!view.BoxVisible({b.at.x - reach, b.at.y - 5, b.at.z - reach}, {b.at.x + reach, b.at.y + 25, b.at.z + reach})) continue;
    const float dx = b.at.x - focus.x, dz = b.at.z - focus.z;
    under.push_back({std::max(std::sqrt(dx * dx + dz * dz) - reach, 0.0f), &b});
  }
  if (under.size() > static_cast<size_t>(kMost)) {
    std::partial_sort(under.begin(), under.begin() + kMost, under.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    under.resize(kMost);
  }
  for (const auto& u : under) {
    Write(*u.second, clock, &gpu_[4 + 12 * static_cast<size_t>(active_)]);
    if (gpu_[4 + 12 * static_cast<size_t>(active_) + 9] > 0) ++dustBlasts_;
    ++active_;
  }
  gpu_[0] = static_cast<float>(active_);
}

bool Blasts::PrepareField(const float area[4]) {
  if (!wavesNow_ || !active_ || !Init()) return false;
  device_->SetPipeline(fieldPass_);
  device_->SetConstants(1, gpu_.data(), ConstantsSize());
  device_->SetConstants(2, area, sizeof(float) * 4);
  device_->SetStorage(0, field_);
  device_->Dispatch(kFieldSize / 8, kFieldSize / 8, 1);
  device_->SetStorage(0, {});
  return true;
}

void Blasts::DrawDust(const View& view, gfx::Texture sceneDepth, float zNear, gfx::Texture heights, float heightUnit, gfx::Texture groundColours,
                      float mapWidth, float mapHeight) {
  dustPuffs = 0;
  if (!dustNow_ || !dustBlasts_ || !heights || !groundColours || !Init()) return;
  constexpr int kPuffs = 48;
  // the camera's sideways and upward directions, from the rows of the view-projection matrix (as the particles take them)
  const Mat4& m = view.constants.viewProj;
  const Vec3 r = Normalize(Vec3{m.m[0][0], m.m[0][1], m.m[0][2]}), u = Normalize(Vec3{m.m[1][0], m.m[1][1], m.m[1][2]});
  const float k[12] = {r.x, r.y, r.z, zNear, u.x, u.y, u.z, heightUnit, mapWidth, mapHeight, waterLevel, static_cast<float>(kPuffs)};
  device_->SetPipeline(dust_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, gpu_.data(), ConstantsSize());
  device_->SetConstants(2, k, sizeof k);
  device_->SetTexture(0, heights);
  device_->SetTexture(1, groundColours);
  device_->SetTexture(3, sceneDepth);
  dustPuffs = active_ * kPuffs;
  device_->Draw(static_cast<uint32_t>(dustPuffs) * 6);
  device_->SetTexture(0, {});
  device_->SetTexture(1, {});
  device_->SetTexture(3, {});
}

}  // namespace render
