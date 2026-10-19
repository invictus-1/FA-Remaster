#include "seasons.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/vfs.h"
#include "lighting.h"

namespace render {

const char* SeasonName(Season season) {
  switch (season) {
    case Season::Autumn: return "autumn";
    case Season::Winter: return "winter";
    default: return "summer";
  }
}

bool SeasonNamed(const std::string& name, Season* out) {
  std::string low = name;
  for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (low == "summer") *out = Season::Summer;
  else if (low == "autumn" || low == "fall") *out = Season::Autumn;
  else if (low == "winter") *out = Season::Winter;
  else return false;
  return true;
}

void SeasonLook::Read(const JsonValue& mapEntry, Season of) {
  *this = SeasonLook{};
  season = of;
  if (of == Season::Summer) return;
  const JsonValue& block = mapEntry["seasons"][SeasonName(of)];
  if (block.kind != JsonValue::Object) return;
  listed = true;
  for (const auto& [from, to] : block["materials"].members)
    if (to.kind == JsonValue::String && !to.text.empty()) materials[from] = to.text;
  cover = block["cover"].StringOr(std::string());
  mud = block["mud"].StringOr(std::string());
  for (const auto& [from, to] : block["props"].members)
    if (to.kind == JsonValue::String && !to.text.empty()) props[moho::Vfs::Normalize(from)] = to.text;
  printf("render: the %s look of the map: %zu ground materials, %s, %zu props%s\n", SeasonName(of), materials.size(),
         cover.empty() ? "summer's ground cover" : "its own ground cover", props.size(), mud.empty() ? "" : ", its own mud");
}

std::string SeasonLook::Material(const std::string& name) const {
  auto it = materials.find(name);
  return it == materials.end() ? std::string() : it->second;
}

std::string SeasonLook::Prop(const std::string& blueprint) const {
  if (props.empty()) return blueprint;
  auto it = props.find(moho::Vfs::Normalize(blueprint));
  return it == props.end() ? blueprint : it->second;
}

bool Broadleaf(const std::string& blueprint) {
  std::string name = blueprint.substr(blueprint.find_last_of("/\\") == std::string::npos ? 0 : blueprint.find_last_of("/\\") + 1);
  for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (const char* conifer : {"pine", "spruce", "conifer", "cedar", "cypress", "juniper", "yew", "fir0", "fir_", "_fir"})
    if (name.find(conifer) != std::string::npos) return false;
  for (const char* broad : {"birch", "brch", "oak", "maple", "beech", "aspen", "elm", "alder", "willow", "poplar", "linden", "chestnut", "deciduous",
                            "broadleaf"})
    if (name.find(broad) != std::string::npos) return true;
  return false;
}

namespace {

// A leaf's number gives where it is (its tree and its place on it; or its square of ground and its place in it), and
// from the time its cycle; each cycle it may come off (more often in a gust), and what it does follows from those
// numbers alone, worked out here.
const char kLeaves[] = R"hlsl(
cbuffer Leaves : register(b1) {
  float4 info;     // x the wind's strength (the trees'); y a leaf's length (world units); z world units a pixel spans at distance 1; w how far from the eye
  float4 motion;   // x how fast a leaf sinks (world units a second); y how fast a gust of 1 carries it; z the share of the places in use
  float4 mapInfo;  // x, y the map's width and height; z world units of the heights' largest value; w the water's level
  float4 area;     // (on the ground) x, y the first square's corner (world x, z); z a square's size; w squares a side
  float4 trees[TREES * 2];  // each: its foot (x, y, z) and height; its crown's radius, where the crown begins (a share of the height), its number
};
Texture2D<float> heightMap : register(t0);
Texture2D plants : register(t1);  // (on the ground) where the ground cover grows: Terrain::CoverDensity's first picture

uint LeafHash(uint x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}
float LeafRand(uint h) { return float(h & 0xffffffu) / 16777216.0; }
float LeafGround(float2 xz) {
  float2 p = clamp(xz, 0, mapInfo.xy);
  float2 f = floor(p);
  float2 t = p - f;
  int2 c = int2(f);
  int2 most = int2(mapInfo.xy);
  float a = heightMap.Load(int3(min(c, most), 0)), b = heightMap.Load(int3(min(c + int2(1, 0), most), 0));
  float d = heightMap.Load(int3(min(c + int2(0, 1), most), 0)), e = heightMap.Load(int3(min(c + int2(1, 1), most), 0));
  return lerp(lerp(a, b, t.x), lerp(d, e, t.x), t.y) * mapInfo.z;
}
// the wind the trees sway in (mesh_renderer.cpp): its way, and its strength at a place and a moment (gusts that run
// across the land with it)
static const float2 kWay = float2(0.915, 0.403);
float Gust(float2 xz, float at) { return info.x * (0.45 + 1.3 * CloudNoise(float3((xz - kWay * (at * 7)) * 0.02, at * 0.07))); }

struct LOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
  float3 normal : TEXCOORD1;
  float2 shape : TEXCOORD2;   // where on the leaf: x from its stalk (-1) to its tip (1), y across
  float3 albedo : TEXCOORD3;  // linear
};

static const float2 kCard[6] = {float2(-1, -1), float2(1, -1), float2(-1, 1), float2(-1, 1), float2(1, -1), float2(1, 1)};

// A leaf's card, its corner `corner`, at `p`: turned `yaw` about the upright, rocked `roll` about its own length and
// tipped `pitch` (lying: flat, as it fell); `shown` how much of its size it has (0: none); `c` its cycle's number;
// `fallen` 1 for one that has lain a while (browner).
LOut Card(float3 p, uint corner, uint c, float yaw, float roll, float pitch, bool lying, float shown, float fallen) {
  LOut o = (LOut)0;
  o.position = float4(0, 0, -1, 1);  // (none: behind the eye)
  float far = length(p - cameraPosition);
  if (shown <= 0 || far > info.w) return o;
  // its size: about as large as the leaves in the art round it (the crowns', the autumn ground's), at least a pixel
  // and a half; where it is drawn larger than that, fewer of them (the picture keeps about the leaves' share of it)
  float own = info.y * (0.8 + 0.45 * LeafRand(LeafHash(c + 9)));
  float size = max(own, info.z * far * 1.5);
  if (LeafRand(LeafHash(c + 10)) > (own / size) * (own / size)) return o;
  size *= shown * saturate((info.w - far) / (info.w * 0.2));
  float3 along = float3(cos(yaw), 0, sin(yaw)), across = float3(-sin(yaw), 0, cos(yaw)), up = float3(0, 1, 0);
  float3 across2 = across * cos(roll) + up * sin(roll);
  float3 normal = up * cos(roll) - across * sin(roll);
  float3 along2 = along * cos(pitch) + normal * sin(pitch);
  normal = normal * cos(pitch) - along * sin(pitch);
  // turned part of the way toward the eye: a leaf a few pixels long seen edge on (lying, from a low camera) is a sliver
  // of a pixel, and the eye takes a falling leaf's shape from its flicker, not its exact angle
  float3 toEye = normalize(cameraPosition - p);
  float3 facing = normalize(lerp(dot(normal, toEye) < 0 ? -normal : normal, toEye, lying ? 0.4 : 0.5));
  float3 side = cross(facing, along2);
  if (dot(side, side) > 1e-6) {
    across2 = normalize(side);
    along2 = cross(across2, facing);
  }
  float2 k = kCard[corner];
  float3 world = p + along2 * (k.x * size * 0.5) + across2 * (k.y * size * 0.32);
  o.world = world;
  o.position = mul(viewProj, float4(world, 1));
  o.normal = normal;  // (lit as it lies, not as turned toward the eye: a lying leaf as bright as the ground under it)
  o.shape = k;
  // its colour: the birches' gold, some yellower, some amber, a few brown; those that have lain a while browner (as a
  // screen shows colours, made linear)
  float r = min(LeafRand(LeafHash(c + 14)) + 0.3 * fallen, 0.999), r2 = LeafRand(LeafHash(c + 15));
  float3 colour = r < 0.6 ? lerp(float3(0.78, 0.58, 0.12), float3(0.88, 0.72, 0.22), r2)
                : r < 0.9 ? lerp(float3(0.70, 0.38, 0.09), float3(0.80, 0.56, 0.13), r2)
                          : lerp(float3(0.45, 0.28, 0.11), float3(0.58, 0.38, 0.14), r2);
  o.albedo = pow(colour, 2.2) * (1 - 0.45 * fallen);  // (those that have lain a while darker too, as the ground's own litter)
  return o;
}

// Off a tree: comes off its crown, flutters and spins down, carried by the wind, lies a while where it lands. (`now`: the
// time it is drawn at; the motion's pass draws it at the frame before's too)
LOut TreeLeaf(uint id, float now) {
  LOut o = (LOut)0;
  o.position = float4(0, 0, -1, 1);
  uint leaf = id / 6, corner = id % 6;
  uint tree = leaf / LEAVES, k = leaf % LEAVES;
  float4 A = trees[tree * 2], B = trees[tree * 2 + 1];
  uint h = LeafHash(uint(B.z) * 7919u + k * 104729u + 17u);
  // its cycle: long enough to fall from the top of the tree at the slowest and lie a while
  float slowest = motion.x * 0.8;
  float period = A.w / slowest + 8.5 + 3.0 * LeafRand(LeafHash(h + 1));
  float phase = now / period + LeafRand(LeafHash(h + 2));
  float since = frac(phase) * period;
  uint c = LeafHash(h ^ (uint(floor(phase)) * 2654435761u));
  // the wind at the tree when it comes off
  float gust = Gust(A.xz, now - since);
  if (LeafRand(c) > motion.z * saturate(0.35 + 0.65 * gust)) return o;  // (more come off in a gust)
  // where it comes off: somewhere on the crown, mostly toward its outside (from deep inside it would fall unseen)
  float turn = LeafRand(LeafHash(c + 1)) * 6.2831853, out_ = B.x * (0.8 + 0.3 * sqrt(LeafRand(LeafHash(c + 2))));
  float3 start = float3(A.x + cos(turn) * out_, A.y + A.w * lerp(B.y, 0.95, LeafRand(LeafHash(c + 3))), A.z + sin(turn) * out_);
  float speed = motion.x * (0.8 + 0.4 * LeafRand(LeafHash(c + 4)));
  float carry = motion.y * gust * (0.7 + 0.6 * LeafRand(LeafHash(c + 5)));
  // where it lands: the ground (or the water) under where the wind has carried it by then (once round, from the
  // tree's foot)
  float fall = max(start.y - A.y, 0.05) / speed;
  float land = max(LeafGround(start.xz + kWay * (carry * fall)), mapInfo.w);
  fall = max(start.y - land, 0.05) / speed;
  float t = min(since, fall);
  // it swings from side to side as it sinks (a falling leaf's pendulum), rising a little at each end of a swing,
  // and turns as it goes
  float w = 1.8 + 1.6 * LeafRand(LeafHash(c + 6)), ph = LeafRand(LeafHash(c + 7)) * 6.2831853;
  float amp = 0.1 * (0.6 + 0.8 * LeafRand(LeafHash(c + 8)));
  float swing = sin(w * t + ph);
  float2 side = float2(-kWay.y, kWay.x);
  float3 p = start + float3(kWay.x * carry * t, -speed * t, kWay.y * carry * t);
  p.xz += side * (amp * swing) + kWay * (amp * 0.35 * sin(2 * w * t + ph));
  p.y += amp * 0.3 * cos(2 * w * t + ph) * saturate((fall - t) * 4);
  float lying = since - fall;
  if (lying > 0) p.y = land + 0.004;
  // shown: in over its first moments, gone after it has lain seven or eight seconds
  float shown = saturate(since / 0.4) * saturate(8 - max(lying, 0));
  float yaw = LeafRand(LeafHash(c + 11)) * 6.2831853 + t * (0.6 + 1.4 * LeafRand(LeafHash(c + 12))) * (LeafRand(LeafHash(c + 13)) > 0.5 ? 1 : -1);
  float roll = lying > 0 ? 0.06 : 0.95 * swing, pitch = lying > 0 ? 0 : 0.35 * cos(w * t + ph);
  return Card(p, corner, c, yaw, roll, pitch, lying > 0, shown, 0);
}
LOut VS(uint id : SV_VertexID) { return TreeLeaf(id, time); }

// On the ground: a leaf lies where the ground cover grows; a gust takes it a few units along the wind, hopping and
// tumbling over the ground, and it lies again, and is gone. Squares round what the camera looks at, a few places in each.
LOut GroundLeaf(uint id, float now) {
  LOut o = (LOut)0;
  o.position = float4(0, 0, -1, 1);
  uint place = id / 6, corner = id % 6;
  uint square = place / PER_SQUARE, k = place % PER_SQUARE;
  uint across_ = uint(area.w);
  float2 at = area.xy + float2(square % across_, square / across_) * area.z;
  int2 cell = int2(floor(at / area.z + 0.5));
  uint h = LeafHash(uint(cell.x) * 73856093u ^ LeafHash(uint(cell.y) * 19349663u + k * 83492791u + 11u));
  float period = 7.0 + 4.0 * LeafRand(LeafHash(h + 1));
  float phase = now / period + LeafRand(LeafHash(h + 2));
  float since = frac(phase) * period;
  uint c = LeafHash(h ^ (uint(floor(phase)) * 2654435761u));
  float2 anchor = at + float2(LeafRand(LeafHash(c + 1)), LeafRand(LeafHash(c + 2))) * area.z;
  if (any(anchor < 0) || any(anchor > mapInfo.xy)) return o;
  // as many as the cover has plants there (its blades, clumps and dry stalks), more in a gust
  float4 grows = plants.SampleLevel(samplerLinearClamp, anchor / mapInfo.xy, 0);
  float go = 1.0 + 3.0 * LeafRand(LeafHash(c + 3));  // (when the gust takes it)
  float gust = Gust(anchor, now - since + go);
  if (LeafRand(c) > motion.z * saturate(grows.r * 0.5 + grows.g + grows.a) * saturate(0.3 + 0.7 * gust)) return o;
  // carried for a second or three, a few units, eased in and out, a little to one side
  float dur = 1.2 + 1.8 * LeafRand(LeafHash(c + 4));
  float dist = motion.y * gust * (2.5 + 3.0 * LeafRand(LeafHash(c + 5)));
  float u = saturate((since - go) / dur);
  float travelled = dist * u * u * (3 - 2 * u);
  float2 xz = anchor + kWay * travelled + float2(-kWay.y, kWay.x) * ((LeafRand(LeafHash(c + 6)) - 0.5) * 0.4 * dist * sin(3.14159265 * u));
  float ground = LeafGround(xz);
  if (ground < mapInfo.w + 0.02) return o;  // (none on the water)
  bool moving = u > 0 && u < 1;
  float hops = 2 + floor(3 * LeafRand(LeafHash(c + 7)));
  float hop = abs(sin(3.14159265 * hops * u)) * (1 - u) * 0.25 * saturate(dist);
  float3 p = float3(xz.x, ground + 0.004 + hop, xz.y);
  // shown: in and out at its cycle's ends, fewer toward the square's edge
  float half_ = area.z * area.w * 0.5;
  float shown = saturate(since / 0.6) * saturate((period - since) / 0.8) * saturate((1 - length(anchor - (area.xy + half_)) / half_) * 4);
  float spin = (LeafRand(LeafHash(c + 8)) > 0.5 ? 1 : -1) * (2 + 3 * LeafRand(LeafHash(c + 12)));
  float yaw = LeafRand(LeafHash(c + 11)) * 6.2831853 + spin * u * dur;
  float roll = moving ? 1.1 * sin(6.2831853 * hops * u) : 0.06, pitch = moving ? 0.5 * cos(6.2831853 * hops * u) : 0;
  return Card(p, corner, c, yaw, roll, pitch, !moving, shown, 1);
}
LOut GroundVS(uint id : SV_VertexID) { return GroundLeaf(id, time); }

// a birch leaf's outline: broadest a third of the way from its stalk, to a point at its tip, and a short stalk
bool LeafOutside(float2 shape) {
  float s = shape.x * 0.5 + 0.5;
  float wide = pow(saturate(s), 0.55) * (1 - s) * 2.3;
  return abs(shape.y) > wide && !(s < 0.1 && abs(shape.y) < 0.09);
}

// [groundfix] The motion (motion.h): each leaf drawn again as the scene drew it (meeting the depth it left), writing where
// the same corner of it was the frame before (`then`: drawn at that frame's time), both seen by the views without the
// jitter. One that was not there the frame before (its cycle just begun, or not yet shown) is taken to stand still.
cbuffer LeafMotion : register(b2) {
  row_major float4x4 motionNow;     // this frame's view without the jitter
  row_major float4x4 motionBefore;  // the frame before's
  float4 motionWhen;                // x: the frame before's time
};
struct MOut {
  float4 position : SV_Position;
  float2 shape : TEXCOORD0;
  float4 here : TEXCOORD1;
  float4 then : TEXCOORD2;
};
MOut Moved(LOut a, LOut b) {
  MOut o;
  o.position = a.position;
  o.shape = a.shape;
  o.here = mul(motionNow, float4(a.world, 1));
  bool was = b.position.w > 0 && b.position.z >= 0;  // (Card's "none" lies behind the eye: z -1, w 1)
  o.then = mul(motionBefore, float4(was ? b.world : a.world, 1));
  return o;
}
MOut MotionVS(uint id : SV_VertexID) { return Moved(TreeLeaf(id, time), TreeLeaf(id, motionWhen.x)); }
MOut GroundMotionVS(uint id : SV_VertexID) { return Moved(GroundLeaf(id, time), GroundLeaf(id, motionWhen.x)); }
float2 MotionPS(MOut i) : SV_Target {
  if (LeafOutside(i.shape)) discard;
  return (i.then.xy / i.then.w - i.here.xy / i.here.w) * float2(0.5, -0.5);
}

float4 PS(LOut i) : SV_Target {
  float s = i.shape.x * 0.5 + 0.5;
  float wide = pow(saturate(s), 0.55) * (1 - s) * 2.3;
  if (LeafOutside(i.shape)) discard;
  // (its midrib, and darker toward its edge, as a leaf drying from its rim)
  float3 albedo = i.albedo * (abs(i.shape.y) < 0.07 ? 0.82 : 1) * lerp(0.6, 1, saturate((wide - abs(i.shape.y)) / max(wide, 1e-3) * 3));
  float3 n = normalize(i.normal);
  float3 toEye = cameraPosition - i.world;
  if (dot(n, toEye) < 0) n = -n;  // (seen from either side)
  float shadow = SunShadow(i.world, n);
  // a leaf is thin: of the sun on its far side some comes through, deeper in colour
  float back = saturate(dot(-n, sunDirection));
  float3 deep = albedo * albedo / max(max(albedo.r, max(albedo.g, albedo.b)), 0.02);
  float3 through = deep * ToLinear(sunColor) * (lightMultiplier * modern.x * back * shadow * CloudShadow(i.world) * 0.5);
  return float4(ModernLight(albedo, 0, 0.6, 1, through, n, i.world, shadow), 1);
}
)hlsl";

struct LeafConstants {
  float info[4], motion[4], mapInfo[4], area[4];
  float trees[FallingLeaves::kTreesMost * 2][4];
};

float Smooth(float a, float b, float x) {
  const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}

}  // namespace

FallingLeaves::~FallingLeaves() {
  for (gfx::Pipeline p : {draw_, ground_, drawMotion_, groundMotion_})
    if (p) device_->Destroy(p);
}

void FallingLeaves::SetTrees(std::vector<Tree> trees, float mapWidth, float mapHeight, float waterLevel) {
  trees_ = std::move(trees);
  mapWidth_ = mapWidth;
  mapHeight_ = mapHeight;
  water_ = waterLevel;
  drawNow_ = groundNow_ = false;
  if (!trees_.empty()) printf("render: leaves fall from %zu of the map's trees\n", trees_.size());
}

bool FallingLeaves::Ready() {
  if (!tried_) {
    tried_ = true;
    std::string error;
    const std::string defines = "#define TREES " + std::to_string(kTreesMost) + "\n#define LEAVES " + std::to_string(kLeavesPerTree) +
                                "\n#define PER_SQUARE " + std::to_string(kPerSquare) + "\n";
    const std::string source = std::string(kViewShaderBlock) + kLightingShaderBlock + defines + kLeaves;
    gfx::PipelineDesc pd;
    pd.name = "seasons: falling leaves";
    pd.vertexSource = pd.pixelSource = source;
    pd.cull = gfx::Cull::None;
    pd.colorFormat = kSceneFormat;
    pd.samples = gSceneSamples;
    draw_ = device_->CreatePipeline(pd, &error);
    if (draw_) {
      pd.name = "seasons: leaves on the ground";
      pd.vertexEntry = "GroundVS";
      ground_ = device_->CreatePipeline(pd, &error);
    }
    if (!draw_ || !ground_) fprintf(stderr, "render: the falling leaves cannot be drawn (%s)\n", error.c_str());
  }
  return draw_ && ground_;
}

// (what both kinds share: the wind, the leaves' size, how far they are drawn)
void FallingLeaves::Common(const View& view, const Camera& camera, float rain, float snow, float farthest, float share, float length,
                           std::vector<float>* out) {
  out->assign(sizeof(LeafConstants) / sizeof(float), 0.0f);
  LeafConstants& c = *reinterpret_cast<LeafConstants*>(out->data());
  c.info[0] = 0.35f + 0.9f * rain + 0.3f * snow;  // (the trees' wind: harder in rain)
  c.info[1] = length * size;
  c.info[2] = 2 * std::tan(camera.fovY * 0.5f) / static_cast<float>(std::max(view.height, 1));
  c.info[3] = farthest;
  c.motion[0] = 0.28f * sink;
  c.motion[1] = 0.9f * drift;
  c.motion[2] = std::clamp(share, 0.0f, 1.0f);
  c.mapInfo[0] = mapWidth_;
  c.mapInfo[1] = mapHeight_;
  c.mapInfo[3] = water_;
}

bool FallingLeaves::Prepare(const View& view, const Camera& camera, float rain, float snow, const std::function<bool(uint32_t)>& standing,
                            gfx::Texture plants) {
  treesDrawn = leavesDrawn = groundPlaces = 0;
  drawNow_ = groundNow_ = false;
  // [groundfix] (the frame before's time, for the motion: none when there was no frame just before - the leaves just
  // switched on, or a gap - and when the game's time stands still, none moves)
  const float now = view.constants.time;
  beforeTime_ = now - lastTime_ > 0 && now - lastTime_ <= 0.25f ? lastTime_ : now;
  lastTime_ = now;
  if (density <= 0 || size <= 0 || (trees_.empty() && !plants)) return false;
  // fewer as the game's camera rises, none from the strategic zoom (a cinematic camera is low down)
  const float rise = camera.cinematic ? 0.0f : camera.distance;
  const float shown = 1 - Smooth(100, 180, rise);
  if (shown <= 0.01f || !Ready()) return false;
  const float farthest = std::min((camera.cinematic ? 60.0f : camera.distance) * 1.8f + 40, 200.0f);
  // off the trees: the nearest standing ones in the picture
  const float carried = 8 * drift;  // (how far downwind a leaf may get: the box a tree's leaves are seen in)
  near_.clear();
  for (size_t i = 0; i < trees_.size(); ++i) {
    const Tree& t = trees_[i];
    const Vec3 middle{t.foot.x, t.foot.y + t.height * 0.5f, t.foot.z};
    const float d = Length(middle - view.eye);
    if (d > farthest + t.height + carried) continue;
    const float r = t.crown + carried;
    if (!view.BoxVisible({t.foot.x - r, t.foot.y - 1, t.foot.z - r}, {t.foot.x + r, t.foot.y + t.height, t.foot.z + r})) continue;
    if (standing && !standing(t.key)) continue;
    near_.push_back({d, static_cast<uint32_t>(i)});
  }
  if (near_.size() > static_cast<size_t>(kTreesMost)) {
    std::nth_element(near_.begin(), near_.begin() + kTreesMost, near_.end());
    near_.resize(kTreesMost);
  }
  if (!near_.empty()) {
    Common(view, camera, rain, snow, farthest, density * shown, 0.07f, &constants_);
    LeafConstants& c = *reinterpret_cast<LeafConstants*>(constants_.data());
    for (size_t n = 0; n < near_.size(); ++n) {
      const Tree& t = trees_[near_[n].second];
      float* a = c.trees[n * 2];
      float* b = c.trees[n * 2 + 1];
      a[0] = t.foot.x;
      a[1] = t.foot.y;
      a[2] = t.foot.z;
      a[3] = t.height;
      b[0] = t.crown;
      b[1] = t.low;
      b[2] = static_cast<float>(t.key & 0xffffffu);  // (its leaves' own numbers: the same tree, the same leaves)
    }
    treesDrawn = static_cast<int>(near_.size());
    leavesDrawn = treesDrawn * kLeavesPerTree;
    drawNow_ = true;
  }
  // on the ground, where the cover grows: squares round what the camera looks at (round the eye, for a cinematic
  // camera), gone as the camera rises sooner than those off the trees
  const float low = 1 - Smooth(50, 110, rise);
  if (plants && groundDensity > 0 && low > 0.01f) {
    const Vec3 middle = camera.cinematic ? view.eye : camera.target;
    const float reach = std::clamp((camera.cinematic ? 30.0f : camera.distance) * 0.9f, 15.0f, 45.0f);
    const int side = static_cast<int>(std::ceil(2 * reach / kSquare));
    // (a little larger than those off the trees: as large as the leaves the autumn ground's own pictures hold)
    Common(view, camera, rain, snow, std::min(farthest, reach * 2.5f), groundDensity * density * low * 1.5f, 0.16f, &groundConstants_);
    LeafConstants& c = *reinterpret_cast<LeafConstants*>(groundConstants_.data());
    c.area[0] = std::floor((middle.x - reach) / kSquare) * kSquare;
    c.area[1] = std::floor((middle.z - reach) / kSquare) * kSquare;
    c.area[2] = kSquare;
    c.area[3] = static_cast<float>(side);
    groundPlaces = side * side * kPerSquare;
    plants_ = plants;
    groundNow_ = true;
  }
  return drawNow_ || groundNow_;
}

void FallingLeaves::Draw(const View& view, gfx::Texture heights, float heightUnit) {
  if (drawNow_ && draw_ && !constants_.empty()) {
    reinterpret_cast<LeafConstants*>(constants_.data())->mapInfo[2] = heightUnit;
    device_->SetPipeline(draw_);
    device_->SetConstants(0, &view.constants, sizeof view.constants);
    device_->SetConstants(1, constants_.data(), static_cast<uint32_t>(sizeof(LeafConstants)));
    device_->SetTexture(0, heights);
    device_->Draw(static_cast<uint32_t>(treesDrawn) * kLeavesPerTree * 6);
  }
  if (groundNow_ && ground_ && !groundConstants_.empty()) {
    reinterpret_cast<LeafConstants*>(groundConstants_.data())->mapInfo[2] = heightUnit;
    device_->SetPipeline(ground_);
    device_->SetConstants(0, &view.constants, sizeof view.constants);
    device_->SetConstants(1, groundConstants_.data(), static_cast<uint32_t>(sizeof(LeafConstants)));
    device_->SetTexture(0, heights);
    device_->SetTexture(1, plants_);
    device_->Draw(static_cast<uint32_t>(groundPlaces) * 6);
  }
  device_->SetTexture(0, {});
  device_->SetTexture(1, {});
}

void FallingLeaves::DrawMotion(const View& view, gfx::Texture motion, gfx::Texture depth, const Mat4& now, const Mat4& before, gfx::Texture heights,
                               float heightUnit) {
  if (!motion || !depth || (!drawNow_ && !groundNow_)) return;
  if (!motionTried_) {
    motionTried_ = true;
    std::string error;
    const std::string defines = "#define TREES " + std::to_string(kTreesMost) + "\n#define LEAVES " + std::to_string(kLeavesPerTree) +
                                "\n#define PER_SQUARE " + std::to_string(kPerSquare) + "\n";
    const std::string source = std::string(kViewShaderBlock) + kLightingShaderBlock + defines + kLeaves;
    gfx::PipelineDesc pd;
    pd.name = "seasons: falling leaves, motion";
    pd.vertexSource = pd.pixelSource = source;
    pd.vertexEntry = "MotionVS";
    pd.pixelEntry = "MotionPS";
    pd.cull = gfx::Cull::None;
    pd.colorFormat = gfx::Format::RG16F;
    pd.depthWrite = false;
    pd.depthBias = 16;  // (as the models' motion: the same placement compiled in another shader may land a hair farther)
    drawMotion_ = device_->CreatePipeline(pd, &error);
    if (drawMotion_) {
      pd.name = "seasons: leaves on the ground, motion";
      pd.vertexEntry = "GroundMotionVS";
      groundMotion_ = device_->CreatePipeline(pd, &error);
    }
    if (!drawMotion_ || !groundMotion_) fprintf(stderr, "render: the falling leaves' motion cannot be drawn (%s)\n", error.c_str());
  }
  if (!drawMotion_ || !groundMotion_) return;
  gfx::PassDesc pass;
  pass.color = motion;
  pass.depth = depth;
  pass.clearColor = pass.clearDepth = false;
  device_->BeginPass(pass);
  struct {
    Mat4 now, before;
    float when[4];
  } k{now, before, {beforeTime_, 0, 0, 0}};
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(2, &k, sizeof k);
  device_->SetTexture(0, heights);
  if (drawNow_ && !constants_.empty()) {
    reinterpret_cast<LeafConstants*>(constants_.data())->mapInfo[2] = heightUnit;
    device_->SetPipeline(drawMotion_);
    device_->SetConstants(0, &view.constants, sizeof view.constants);
    device_->SetConstants(1, constants_.data(), static_cast<uint32_t>(sizeof(LeafConstants)));
    device_->SetConstants(2, &k, sizeof k);
    device_->Draw(static_cast<uint32_t>(treesDrawn) * kLeavesPerTree * 6);
  }
  if (groundNow_ && !groundConstants_.empty()) {
    reinterpret_cast<LeafConstants*>(groundConstants_.data())->mapInfo[2] = heightUnit;
    device_->SetPipeline(groundMotion_);
    device_->SetConstants(0, &view.constants, sizeof view.constants);
    device_->SetConstants(1, groundConstants_.data(), static_cast<uint32_t>(sizeof(LeafConstants)));
    device_->SetConstants(2, &k, sizeof k);
    device_->SetTexture(1, plants_);
    device_->Draw(static_cast<uint32_t>(groundPlaces) * 6);
  }
  device_->SetTexture(0, {});
  device_->SetTexture(1, {});
}

}  // namespace render
