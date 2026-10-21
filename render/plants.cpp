#include "plants.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace render {

// (plants.h; the host defines PLANTS_PRESS and PLANTS_BURN, its two free texture registers, before it)
const char kPlantsShaderBlock[] = R"hlsl(
Texture2D plantsPress : register(PLANTS_PRESS);  // (PlantLife::Press) x, z which way laid x how hard; how hard; seconds since x how hard
Texture2D plantsBurnt : register(PLANTS_BURN);   // (PlantLife::Burnt) how hard burnt; how fresh x how hard
// k0: the field's first texel's x, z, 1 / its width, the clock; k1: 1 / the map's width and height, 1 when there is a burnt
// picture, 1 when the field is there
float4 PlantsPressAt(float2 xz, float4 k0, float4 k1) {
  [branch] if (k1.w < 0.5) return 0;
  float2 uv = (xz - k0.xy) * k0.z;
  [branch] if (any(uv < 0.004) || any(uv > 0.996)) return 0;
  return plantsPress.SampleLevel(samplerLinearClamp, uv, 0);
}
// how it looks where it is burnt: x how black (charred, the first minutes), y how brown (burnt dry, after), z how much of its
// height is gone, w embers (the first seconds); `random` its own (0..1): of a place half burnt, half of what grows burns
// (PlantsBurnt: the burnt picture at xz, how hard and how fresh x how hard; PlantsBurnLookOf: the look of that)
float2 PlantsBurnt(float2 xz, float4 k1) {
  [branch] if (k1.z < 0.5) return 0;
  return plantsBurnt.SampleLevel(samplerLinearClamp, saturate(xz * k1.xy), 0).rg;
}
float4 PlantsBurnLookOf(float2 b, float random) {
  if (b.r < 0.02) return 0;
  // (how fresh: e to the minus its age over 100 seconds)
  float fresh = b.g / b.r;
  float age = fresh > 1e-4 ? -100 * log(min(fresh, 1)) : 1e3;
  // (how hard burnt fades as it grows back: what is left of it against each one's own share; a soft step, so that a
  // patch's edge and a patch growing back are blades and plants burnt among green ones, not a line)
  float threshold = 0.08 + 0.55 * random;
  float burnt = saturate((b.r - threshold) / 0.08);
  // charred black where it burnt through, the first minutes; scorched brown at the edge, and all of it after
  float black = (1 - smoothstep(60, 240, age)) * smoothstep(0.05, 0.4, b.r - threshold);
  float low = burnt * lerp(0.55, 0.35, smoothstep(120, 600, age));  // (stubble, half as high, growing)
  return float4(burnt * black, burnt * (1 - black), low, burnt * black * (1 - smoothstep(2, 15, age)));
}
float4 PlantsBurnLook(float2 xz, float4 k0, float4 k1, float random) { return PlantsBurnLookOf(PlantsBurnt(xz, k1), random); }
// a linear colour so burnt: brown and dry, then black over it while charred
float3 PlantsBurnColour(float3 c, float4 look) {
  c = lerp(c, float3(0.055, 0.040, 0.026) + c * 0.12, look.y);
  return lerp(c, float3(0.016, 0.015, 0.014), look.x);
}
// embers' glow (linear, before the night's dimming): sparks here and there, flickering by the place and the time
float3 PlantsEmber(float ember, float2 xz, float time) {
  [branch] if (ember < 0.01) return 0;
  float spark = CloudNoise(float3(xz * 9.0, time * 1.3)) * 0.7 + CloudNoise(float3(xz * 23.0, time * 2.9)) * 0.3;
  return float3(1.0, 0.24, 0.03) * (ember * saturate((spark - 0.7) * 8) * 0.8);
}
// standing (1) or lying (0), by how far its up points to the sky (the up's y over its length)
float PlantsUpright(float up) { return saturate((up - 0.55) / 0.35); }
)hlsl";

namespace {

constexpr int kPressSize = 512;        // texels a side of the field
constexpr float kPressTexel = 0.25f;   // world units a texel
constexpr int kMostBoxes = 256;
constexpr int kBurnsADraw = 256;

// The field (a thread a texel): what was here (the last field, at this place: it lies a whole number of texels away),
// and what presses now: the most of the boxes over this place (full inside, nothing a fifth of a unit outside), laid the
// way the unit drives and out toward its sides. Pressed harder now than what is left of the last pressing (as the grass
// sees it, rising over six seconds): from now on; else as it was, a little older; forgotten after forty seconds. Kept
// times how hard (x, z the way, z how hard, w the seconds since), so that a texel between a pressed and a free one reads
// as half pressed, not as half its age.
const char kPress[] = R"hlsl(
cbuffer Press : register(b1) {
  float4 area;    // xy: the first texel's corner (world); z: a texel's size; w: texels a side
  float4 before;  // xy: where this field's first texel lies in the last one (texels); z: seconds since it; w: 1 when it is there
  float4 count;   // x: how many boxes
  float4 boxes[512];  // two each: (x, z, forward x, forward z), (half its length, half its width, 0, 0)
};
Texture2D<float4> last : register(t0);
RWTexture2D<float4> field : register(u0);
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  int n = int(area.w);
  if (int(id.x) >= n || int(id.y) >= n) return;
  float2 p = area.xy + (float2(id.xy) + 0.5) * area.z;
  float4 old = 0;
  int2 at = int2(id.xy) + int2(before.xy);
  if (before.w > 0.5 && all(at >= 0) && all(at < n)) old = last.Load(int3(at, 0));
  float pOld = old.z;
  float ageOld = pOld > 1e-3 ? old.w / pOld + before.z : 1e3;
  float best = 0;
  float2 lay = 0;
  for (uint i = 0; i < uint(count.x); ++i) {
    float4 a = boxes[i * 2], b = boxes[i * 2 + 1];
    float2 d = p - a.xy;
    if (abs(d.x) > b.x + b.y + 0.3 || abs(d.y) > b.x + b.y + 0.3) continue;
    float2 f = a.zw, s = float2(-f.y, f.x);
    float along = dot(d, f), across = dot(d, s);
    float edge = max(abs(along) - b.x, abs(across) - b.y);  // (how far outside it; below 0 inside)
    float press = saturate(1 - edge / 0.2);
    if (press > best) {
      best = press;
      lay = normalize(f + s * (sign(across) * saturate(abs(across) / max(b.y, 0.05)) * 0.8));
    }
  }
  float4 o = 0;
  if (best > 0.02 && best >= pOld * exp(-ageOld / 6.0)) o = float4(lay * best, best, 0);
  else if (pOld > 1e-3 && ageOld < 40) o = float4(old.xy, pOld, ageOld * pOld);
  field[id.xy] = o;
}
)hlsl";

struct PressConstants {
  float area[4], before[4], count[4];
  float boxes[2 * kMostBoxes][4];
};

// The burns drawn into the burnt picture (over the whole map): a soft round patch with a ragged edge each, blended over
// what is there by its strength: (how hard, how fresh x how hard) over the same. How fresh: e to the minus the age over 100
// seconds; the picture is faded every half second of the game, how hard by the growing back, how fresh by the time gone
// (so no clock is kept in half floats, which would lose seconds within the first hour).
const char kBurn[] = R"hlsl(
cbuffer Burns : register(b1) {
  float4 info;        // x, y: 1 / the map's width, height; z: how many; (the fade's pass: x what is left of how hard, y of how fresh)
  float4 burns[512];  // two each: (x, z, radius, strength), (how hard, how fresh x how hard: one laid as if earlier; a number of its own, 0)
};
struct VOut {
  float4 position : SV_Position;
  float2 at : TEXCOORD0;
  nointerpolation float4 burn : TEXCOORD1;
  nointerpolation float3 when : TEXCOORD2;
};
static const float2 kCorner[6] = {float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1)};
VOut VS(uint id : SV_VertexID) {
  VOut o;
  uint k = id / 6;
  float4 a = burns[k * 2], b = burns[k * 2 + 1];
  float2 xz = a.xy + kCorner[id % 6] * (a.z * 1.3);  // (room for the ragged edge)
  o.position = float4(xz.x * info.x * 2 - 1, 1 - xz.y * info.y * 2, 0, 1);
  o.at = xz;
  o.burn = a;
  o.when = b.xyz;
  return o;
}
float4 PS(VOut i) : SV_Target {
  float d = length(i.at - i.burn.xy) / max(i.burn.z, 0.05);
  // (its edge in and out by the noise of the place, at two sizes, each burn its own)
  float ragged = CloudNoise(float3(i.at * (1.4 / max(i.burn.z, 0.5)), i.when.z)) * 0.65 + CloudNoise(float3(i.at * 2.9, i.when.z + 5.3)) * 0.35;
  float a = i.burn.w * (1 - smoothstep(0.45, 1.0, d + (ragged - 0.5) * 0.55));
  if (a < 0.002) discard;
  return float4(i.when.x, i.when.y, 0, a);
}
// the fade (the burnt growing back, and growing older), over the whole picture: a triangle that covers it
float4 VSFade(uint id : SV_VertexID) : SV_Position {
  return float4(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0, 0, 1);
}
float4 PSFade(float4 position : SV_Position) : SV_Target { return float4(info.x, info.x * info.y, 1, 1); }
// [scars] a wreck gone (PlantLife::Heal): what burnt round it multiplied down (how hard by `when`.x, how fresh x how hard
// by `when`.y), fully within its radius, softly toward its edge
float4 PSHeal(VOut i) : SV_Target {
  float d = length(i.at - i.burn.xy) / max(i.burn.z, 0.05);
  float w = 1 - smoothstep(0.8, 1.0, d);
  if (w <= 0) discard;
  return float4(lerp(1, i.when.x, w), lerp(1, i.when.y, w), 1, 1);
}
)hlsl";

struct BurnConstants {
  float info[4];
  float burns[2 * kBurnsADraw][4];
};

// The ground where it burnt (DrawGround): after the ground and its marks, before anything stands on it, each pixel's place
// from the scene's depth (as the decals take it), and the colour there multiplied darker: ash black where it burnt through
// (the first minutes), a burnt brown at the edges and after, mottled, as the grass on it.
const char kGround[] = R"hlsl(
cbuffer GroundCamera : register(b1) {
  float4 cameraRight;  // w: tan(half the field of view)
  float4 cameraUp;     // w: aspect
  float4 cameraBack;   // w: near distance
  float4 plantsK[2];   // PlantLife::Constants
};
#ifdef MSAA
Texture2DMS<float> sceneDepth : register(t1);
float DepthAt(int2 p) { return sceneDepth.Load(p, 0); }
#else
Texture2D<float> sceneDepth : register(t1);
float DepthAt(int2 p) { return sceneDepth.Load(int3(p, 0)); }
#endif
float4 VS(uint id : SV_VertexID) : SV_Position { return float4(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0, 0, 1); }
float4 PS(float4 position : SV_Position) : SV_Target {
  float depth = DepthAt(int2(position.xy));
  if (depth <= 0) discard;  // (nothing drawn here)
  float away = cameraBack.w / depth;
  float2 ndc = float2(position.x * viewport.z * 2 - 1, 1 - position.y * viewport.w * 2);
  float3 world = cameraPosition + (cameraRight.xyz * (ndc.x * cameraRight.w * cameraUp.w) + cameraUp.xyz * (ndc.y * cameraRight.w) - cameraBack.xyz) * away;
  float2 b = PlantsBurnt(world.xz, plantsK[1]);
  if (b.r < 0.02) discard;
  float mottle = CloudNoise(float3(world.xz * 2.7, 4.1)) * 0.65 + CloudNoise(float3(world.xz * 7.3, 1.7)) * 0.35;
  float4 look = PlantsBurnLookOf(b, saturate(mottle * 1.3 - 0.15));
  // [scars] under lying snow what burnt more than a minute or so ago is snowed over (as a mark laid on the snow is, decals.h;
  // Chris, 2026-10-20: black masses on the snow round a base); what burns now has melted it and shows
  [branch] if (weather.y > 0) {
    float fresh = b.g / b.r;
    float age = fresh > 1e-4 ? -100 * log(min(fresh, 1)) : 1e3;
    look.xy *= 1 - weather.y * smoothstep(30, 120, age);
  }
  if (look.x + look.y < 0.01) discard;
  // (black with paler grey ash in patches where it burnt through; a dull burnt brown after)
  float ash = saturate(mottle * 2.2 - 1.0);
  float3 keep = lerp(float3(1, 1, 1), lerp(float3(0.6, 0.52, 0.44), float3(0.72, 0.66, 0.58), ash), look.y);
  return float4(lerp(keep, lerp(float3(0.2, 0.19, 0.18), float3(0.45, 0.43, 0.41), ash), look.x), 1);
}
)hlsl";

}  // namespace

PlantLife::~PlantLife() {
  for (gfx::Pipeline p : {pressPass_, burnSplat_, burnFade_, ground_, burnHeal_})
    if (p) device_->Destroy(p);
  for (gfx::Texture t : {press_[0], press_[1], burnt_})
    if (t) device_->Destroy(t);
}

bool PlantLife::Init() {
  if (tried_) return tried_ > 0;
  tried_ = -1;
  std::string error;
  gfx::ComputeDesc cd;
  cd.name = "plants: pressed";
  const std::string press = std::string(kViewShaderBlock) + kPress;
  cd.source = press;
  pressPass_ = device_->CreateCompute(cd, &error);
  const std::string burn = std::string(kViewShaderBlock) + kBurn;
  gfx::PipelineDesc pd;
  pd.name = "plants: burnt";
  pd.vertexSource = pd.pixelSource = burn;
  pd.cull = gfx::Cull::None;
  pd.depthTest = pd.depthWrite = false;
  pd.depthTarget = false;
  pd.blend = gfx::Blend::Alpha;
  pd.colorFormat = gfx::Format::RG16F;
  if (pressPass_) burnSplat_ = device_->CreatePipeline(pd, &error);
  pd.name = "plants: burnt, growing back";
  pd.vertexEntry = "VSFade";
  pd.pixelEntry = "PSFade";
  pd.blend = gfx::Blend::Multiply;
  if (burnSplat_) burnFade_ = device_->CreatePipeline(pd, &error);
  // (the ground where it burnt: into the scene's colour alone, over every sample, its alpha - the original look's glow - left)
  const std::string ground = std::string(gSceneSamples > 1 ? "#define MSAA 1\n" : "") + "#define PLANTS_PRESS t8\n#define PLANTS_BURN t9\n" + kViewShaderBlock +
                             kPlantsShaderBlock + kGround;
  pd.name = "plants: burnt ground";
  pd.vertexSource = pd.pixelSource = ground;
  pd.vertexEntry = "VS";
  pd.pixelEntry = "PS";
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pd.alphaWrite = false;
  if (burnFade_) ground_ = device_->CreatePipeline(pd, &error);
  if (ground_) {  // [scars] (Heal's circles: as the burns are drawn, multiplying; without it a wreck's burn grows back as any)
    gfx::PipelineDesc hd;
    hd.name = "plants: burnt, a wreck gone";
    hd.vertexSource = hd.pixelSource = burn;
    hd.pixelEntry = "PSHeal";
    hd.cull = gfx::Cull::None;
    hd.depthTest = hd.depthWrite = false;
    hd.depthTarget = false;
    hd.blend = gfx::Blend::Multiply;
    hd.colorFormat = gfx::Format::RG16F;
    std::string healError;
    burnHeal_ = device_->CreatePipeline(hd, &healError);
  }
  gfx::TargetDesc td;
  td.storage = true;
  td.width = td.height = kPressSize;
  td.format = gfx::Format::RGBA16F;
  if (ground_) {
    press_[0] = device_->CreateTarget(td);
    press_[1] = device_->CreateTarget(td);
  }
  if (!press_[0] || !press_[1]) {
    fprintf(stderr, "render: plants cannot react (%s)\n", error.empty() ? "no storage target" : error.c_str());
    return false;
  }
  tried_ = 1;
  return true;
}

void PlantLife::SetMap(float mapWidth, float mapHeight) {
  if (mapWidth != mapWidth_ || mapHeight != mapHeight_) {
    if (burnt_) device_->Destroy(burnt_);
    burnt_ = {};
  }
  mapWidth_ = mapWidth;
  mapHeight_ = mapHeight;
  burntFresh_ = true;
  pressValid_ = false;
  burns_.clear();
  fireAt_.clear();
  wrecks_.clear();
  heals_.clear();  // [scars]
  lastClock_ = fadedAt_ = blastsSeen_ = -1;
  lastBurnt_ = -1e31;
  pressedAt_ = -1;
}

void PlantLife::AddBurn(float x, float z, float radius, float strength, double born) {
  if (strength <= 0.001f || radius <= 0 || x < -radius || z < -radius || x > mapWidth_ + radius || z > mapHeight_ + radius) return;
  static const bool debug = getenv("MOHO64_DEBUG_BURNS") != nullptr;  // (each burn laid: where, how large, how strong)
  if (debug && strength > 0.05f) printf("render: plants: a burn at %.1f, %.1f, radius %.2f, strength %.2f, at %.1f s\n", x, z, radius, strength, born);
  if (burns_.size() >= 4096) burns_.erase(burns_.begin());  // (a backlog never drawn)
  burns_.push_back({x, z, radius, std::min(strength, 1.0f), born});
}

void PlantLife::AddWreck(float x, float z, float radius, double born) {
  wrecks_.erase(std::remove_if(wrecks_.begin(), wrecks_.end(), [&](const Burn& w) { return w.born < born - 30 || w.born > born + 1; }), wrecks_.end());
  for (const Burn& w : wrecks_)
    if ((w.x - x) * (w.x - x) + (w.z - z) * (w.z - z) < 0.25f) return;
  wrecks_.push_back({x, z, radius, 1, born});
  // (round it as far as it is large and a little more; burnt through)
  AddBurn(x, z, 0.3f + 1.4f * std::min(radius, 12.0f), 0.85f, born);
}

void PlantLife::Heal(float x, float z, float radius, double when) {
  if (radius <= 0 || heals_.size() >= 1024) return;
  static const bool debug = getenv("MOHO64_DEBUG_BURNS") != nullptr;  // (as AddBurn's)
  if (debug) printf("render: plants: a wreck gone at %.1f, %.1f: what burnt within %.2f goes, from %.1f s\n", x, z, radius, when);
  heals_.push_back({x, z, radius, 1, when});
}

void PlantLife::WatchBlasts(const Blasts& blasts, double clock, const std::function<float(float, float)>& ground) {
  // (each new one once, half a second after it: a scorch and a flash of one explosion merge within that time)
  const double upTo = clock - 0.5;
  if (blastsSeen_ > clock) blastsSeen_ = -1e30;  // (a game played back from earlier)
  for (const Blasts::Blast& b : blasts.Recent()) {
    if (b.born <= blastsSeen_ || b.born > upTo || b.splash || b.water) continue;
    if (!b.hot) continue;  // (a scorch mark alone, without an explosion's heat; nothing burns)
    // (the game's first quarter-minute: commanders warping in, with flashes and scorches of their own; nothing fires yet)
    if (b.born < 15) continue;
    // (on the ground: not a hit high on a unit, nor one in the air)
    if (ground && b.at.y - ground(b.at.x, b.at.z) > std::max(1.5f, 0.5f * b.size)) continue;
    // [scars] as large as its scorch and a little more (its radius 0.3 + 0.7 x the scorch across: some 1.4 times the
    // scorch's; the burn's ragged edge thins out over its outer half), sized by what made it - the scorch or crater the
    // script laid, or the shot's damage radius - and not by the flash, whose glow is one size for every missile of a kind:
    // an MML's missile (damage radius 1, a scorch under 1 across) burnt 6 across with its flash's size (Chris, 2026-10-20:
    // "those missiles are small"). A flash alone (a shot without a radius): half its size. A shell's small blast singes,
    // a big one burns through; a commander's warp-in mark burns nothing (no heat, and the first quarter-minute: above).
    const float scorch = b.scorch > 0 ? b.scorch : 0.5f * b.size;
    AddBurn(b.at.x, b.at.z, std::min(0.3f + 0.7f * scorch, 30.0f), std::clamp(0.35f + 0.5f * scorch, 0.4f, 1.0f), b.born);
  }
  blastsSeen_ = std::max(blastsSeen_, upTo);
}

void PlantLife::WatchFires(const std::vector<Particles::Hot>& hot, double clock, const std::function<float(float, float)>& ground) {
  const float dt = lastClock_ >= 0 ? static_cast<float>(std::clamp(clock - lastClock_, 0.0, 0.5)) : 0.0f;
  std::unordered_map<uint32_t, Vec3> seen;
  for (const Particles::Hot& h : hot) {
    seen[h.id] = h.position;
    if (h.moving || !h.fire) continue;  // (a warm glow that is no fire, a commander's build sparks: burns nothing)
    auto before = fireAt_.find(h.id);
    // (it burns the ground only standing still on it: not on something driving, nor high on a building)
    if (before == fireAt_.end() || Length(before->second - h.position) > 0.05f) continue;
    if (ground && h.position.y - ground(h.position.x, h.position.z) > 1.6f) continue;
    // the longer it burns, the more: a full burn in some seconds
    AddBurn(h.position.x, h.position.z, std::clamp(0.35f + 0.45f * h.size, 0.5f, 2.5f), 1 - std::exp(-dt * 0.3f), clock);
  }
  fireAt_.swap(seen);
}

void PlantLife::Prepare(const View& view, const Camera& camera, const std::vector<float>& boxes, double clock) {
  ready_ = false;
  burnsLaid = boxesPressing = 0;
  if (!Init() || mapWidth_ <= 0 || mapHeight_ <= 0) return;
  if (lastClock_ >= 0 && clock < lastClock_ - 2) {  // (a game played back from earlier: what burnt later has not yet)
    burntFresh_ = true;
    pressValid_ = false;
    fadedAt_ = -1;
    heals_.clear();  // [scars]
  }
  const float dt = lastClock_ >= 0 ? static_cast<float>(std::clamp(clock - lastClock_, 0.0, 0.5)) : 0.0f;
  lastClock_ = clock;
  // the burnt picture: as fine as two texels a world unit, at most 2048 a side
  if (!burnt_) {
    const float perUnit = std::min(2.0f, 2048.0f / std::max(mapWidth_, mapHeight_));
    gfx::TargetDesc td;
    td.width = std::max(16, static_cast<int>(std::ceil(mapWidth_ * perUnit)));
    td.height = std::max(16, static_cast<int>(std::ceil(mapHeight_ * perUnit)));
    td.format = gfx::Format::RG16F;
    burnt_ = device_->CreateTarget(td);
    burntFresh_ = true;
    if (!burnt_) {
      tried_ = -1;
      fprintf(stderr, "render: plants cannot burn (no picture for it)\n");
      return;
    }
  }
  // its burns drawn, and every half second of the game all of it faded a little (growing back, growing older)
  // (nothing burnt for half an hour: nothing to fade)
  if (fadedAt_ < 0 || clock < fadedAt_ || !Burning()) fadedAt_ = clock;
  const bool fade = clock - fadedAt_ >= 0.5;
  if (burntFresh_ || fade || !burns_.empty()) {
    gfx::PassDesc pass;
    pass.color = burnt_;
    pass.clearColor = burntFresh_;
    burntFresh_ = false;
    device_->BeginPass(pass);
    auto k = std::make_unique<BurnConstants>();
    k->info[0] = 1 / mapWidth_;
    k->info[1] = 1 / mapHeight_;
    device_->SetConstants(0, &view.constants, sizeof view.constants);
    if (fade) {
      const double gone = clock - fadedAt_;
      fadedAt_ = clock;
      const float f[4] = {static_cast<float>(std::exp(-gone / std::max(regrow, 1.0f))), static_cast<float>(std::exp(-gone / 100.0)), 0, 0};
      device_->SetPipeline(burnFade_);
      device_->SetConstants(1, f, sizeof f);
      device_->Draw(3);
      // [scars] where wrecks went (Heal): what burnt round them goes, how hard by e to the minus the time over a fifth
      // of `healing`, how fresh twice as fast (it browns first, then thins out by its patches)
      heals_.erase(std::remove_if(heals_.begin(), heals_.end(), [&](const Burn& h) { return clock - h.born > healing || h.born > clock + 1; }), heals_.end());
      if (burnHeal_ && !heals_.empty()) {
        const float hard = static_cast<float>(std::exp(-gone / std::max(0.2 * healing, 1.0))), fresh = static_cast<float>(std::exp(-gone / std::max(0.1 * healing, 1.0)));
        const size_t n = std::min(heals_.size(), static_cast<size_t>(kBurnsADraw));
        for (size_t i = 0; i < n; ++i) {
          const Burn& h = heals_[i];
          float* a = k->burns[i * 2];
          a[0] = h.x, a[1] = h.z, a[2] = h.radius, a[3] = 1;
          float* c = k->burns[i * 2 + 1];
          c[0] = hard, c[1] = hard * fresh, c[2] = 0, c[3] = 0;
        }
        device_->SetPipeline(burnHeal_);
        device_->SetConstants(1, k.get(), static_cast<uint32_t>(sizeof(float) * 4 * (1 + 2 * n)));
        device_->Draw(static_cast<uint32_t>(n) * 6);
      }
    }
    if (!burns_.empty()) device_->SetPipeline(burnSplat_);
    for (size_t from = 0; from < burns_.size(); from += kBurnsADraw) {
      const size_t n = std::min(burns_.size() - from, static_cast<size_t>(kBurnsADraw));
      for (size_t i = 0; i < n; ++i) {
        const Burn& b = burns_[from + i];
        float* a = k->burns[i * 2];
        a[0] = b.x, a[1] = b.z, a[2] = b.radius, a[3] = b.strength;
        // (one laid as if earlier: as much faded and as much older as the picture would have it by now)
        const double age = std::max(clock - b.born, 0.0);
        float* c = k->burns[i * 2 + 1];
        c[0] = static_cast<float>(std::exp(-age / std::max(regrow, 1.0f)));
        c[1] = c[0] * static_cast<float>(std::exp(-age / 100.0));
        c[2] = std::fmod(std::fabs(b.x * 12.9898f + b.z * 78.233f), 97.0f);
        c[3] = 0;
      }
      k->info[2] = static_cast<float>(n);
      device_->SetConstants(1, k.get(), static_cast<uint32_t>(sizeof(float) * 4 * (1 + 2 * n)));
      device_->Draw(static_cast<uint32_t>(n) * 6);
    }
    for (const Burn& b : burns_) lastBurnt_ = std::max(lastBurnt_, b.born);
    burnsLaid = static_cast<int>(burns_.size());
    burns_.clear();
  }
  constants_[4] = 1 / mapWidth_;
  constants_[5] = 1 / mapHeight_;
  // (nothing burnt for half an hour, nothing pressed for forty seconds: the shaders read neither, and cost nothing more)
  constants_[6] = Burning() ? 1.0f : 0.0f;
  constants_[3] = static_cast<float>(clock);
  constants_[7] = 0;
  ready_ = true;
  // the field round what the camera looks at (a cinematic camera: by how high the eye is over the ground ahead, and the
  // square ahead of the eye), its texels where the last one's were, so that what was pressed stays where it was
  const Vec3 eye = camera.Eye();
  if ((camera.cinematic ? eye.y - camera.target.y : camera.distance) > farthest) {
    pressValid_ = false;
    return;
  }
  const float reach = 0.5f * kPressSize * kPressTexel;
  float middleX, middleZ;
  FieldMiddle(camera, &middleX, &middleZ);
  const float originX = std::floor((middleX - reach) / kPressTexel) * kPressTexel;
  const float originZ = std::floor((middleZ - reach) / kPressTexel) * kPressTexel;
  auto k = std::make_unique<PressConstants>();
  k->area[0] = originX;
  k->area[1] = originZ;
  k->area[2] = kPressTexel;
  k->area[3] = static_cast<float>(kPressSize);
  k->before[0] = std::round((originX - pressOrigin_[0]) / kPressTexel);
  k->before[1] = std::round((originZ - pressOrigin_[1]) / kPressTexel);
  k->before[2] = dt;
  k->before[3] = pressValid_ ? 1.0f : 0.0f;
  const float wide = kPressSize * kPressTexel;
  int n = 0;
  for (size_t i = 0; i + 7 < boxes.size() && n < kMostBoxes; i += 8) {
    const float x = boxes[i], z = boxes[i + 1], r = boxes[i + 4] + boxes[i + 5];
    if (x + r < originX || z + r < originZ || x - r > originX + wide || z - r > originZ + wide) continue;
    std::copy(&boxes[i], &boxes[i] + 4, k->boxes[n * 2]);
    std::copy(&boxes[i + 4], &boxes[i + 4] + 4, k->boxes[n * 2 + 1]);
    ++n;
  }
  k->count[0] = static_cast<float>(n);
  boxesPressing = n;
  if (n > 0) pressedAt_ = clock;
  // (nothing pressing, nothing pressed for forty-five seconds: nothing to keep, no pass)
  if (pressedAt_ < 0 || clock - pressedAt_ >= 45) {
    pressValid_ = false;
    return;
  }
  const int last = current_;
  current_ ^= 1;
  device_->SetPipeline(pressPass_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, k.get(), static_cast<uint32_t>(sizeof(float) * 12 + sizeof(float) * 8 * std::max(n, 1)));
  device_->SetTexture(0, press_[last]);
  device_->SetStorage(0, press_[current_]);
  device_->Dispatch(kPressSize / 8, kPressSize / 8, 1);
  device_->SetStorage(0, {});
  device_->SetTexture(0, {});
  pressOrigin_[0] = originX;
  pressOrigin_[1] = originZ;
  pressValid_ = true;
  constants_[0] = originX;
  constants_[1] = originZ;
  constants_[2] = 1 / wide;
  constants_[7] = pressedAt_ >= 0 && clock - pressedAt_ < 45 ? 1.0f : 0.0f;
}

// (a cinematic camera: the square ahead of the eye; else round what it looks at)
void PlantLife::FieldMiddle(const Camera& camera, float* x, float* z) const {
  const float reach = 0.5f * kPressSize * kPressTexel;
  const Vec3 eye = camera.Eye();
  *x = camera.cinematic ? eye.x - std::sin(camera.heading) * reach * 0.6f : camera.target.x;
  *z = camera.cinematic ? eye.z - std::cos(camera.heading) * reach * 0.6f : camera.target.z;
}

bool PlantLife::FieldSquare(const Camera& camera, float square[4]) const {
  const Vec3 eye = camera.Eye();
  if ((camera.cinematic ? eye.y - camera.target.y : camera.distance) > farthest) return false;
  const float reach = 0.5f * kPressSize * kPressTexel;
  float x, z;
  FieldMiddle(camera, &x, &z);
  square[0] = x - reach - kPressTexel;
  square[1] = z - reach - kPressTexel;
  square[2] = x + reach + kPressTexel;
  square[3] = z + reach + kPressTexel;
  return true;
}

void PlantLife::DrawGround(const View& view, const Camera& camera, gfx::Texture sceneDepth) {
  if (!AnyBurnt() || !ground_ || !sceneDepth) return;
  Vec3 right, up, back;
  camera.Axes(&right, &up, &back);
  float k[20] = {right.x, right.y, right.z, std::tan(camera.fovY * 0.5f), up.x, up.y, up.z, static_cast<float>(view.width) / static_cast<float>(std::max(view.height, 1)),
                 back.x, back.y, back.z, camera.zNear};
  std::copy(constants_, constants_ + 8, k + 12);
  device_->SetPipeline(ground_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, k, sizeof k);
  device_->SetTexture(1, sceneDepth);
  device_->SetTexture(9, burnt_);
  device_->Draw(3);
  device_->SetTexture(1, {});
  device_->SetTexture(9, {});
}

}  // namespace render
