#include "wildlife.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numeric>

#include "lighting.h"
#include "scmap.h"

namespace render {
namespace {

constexpr float kHomeSquare = 32;     // the woods are counted by squares this wide (world units)
constexpr float kHomesApart = 56;     // two woods' flocks at least this far apart
constexpr int kGullFlocksMost = 24;   // flocks of gulls a map at most
constexpr float kGullsApart = 110;
constexpr float kWoodsCell = 4;       // the fireflies' picture of the woods: squares this wide
constexpr int kBirdCorners = 27;      // nine triangles a bird
constexpr float kFireflyCell = 2.5f;  // fireflies: a cell this wide holds up to kFirefliesPerCell
constexpr int kFirefliesPerCell = 3;

// The kinds of flock: 0 starlings (many, small, quick, dark), 1 crows (few, larger, slow wingbeats with long glides,
// black), 2 pigeons (grey), 3 gulls (over water; grey above, white below, black wingtips, gliding). What the
// shaders know of each is in kKinds (below); what the processor knows here: how fast it flies (calm, fleeing; world
// units a second), how wide it wheels round where it makes for, and how dark the day is when it goes to roost (the
// sun's height, the sine of its elevation: the starlings last, just after sunset).
constexpr float kCruise[4] = {3.6f, 2.8f, 3.3f, 2.6f};
constexpr float kFlee[4] = {8.5f, 6.5f, 7.5f, 6.0f};
constexpr float kWheel[4] = {6, 9, 7, 12};
constexpr float kRoost[4] = {-0.03f, 0.08f, 0.10f, 0.0f};

// What the shaders share: the kinds' sizes, wingbeats and colours, and the random numbers (the same hash as the
// ground cover's, its own seeds).
const char kKinds[] = R"hlsl(
static const float kSpan[4] = {0.32, 0.46, 0.38, 0.56};      // wingspan, world units (two to three times a real bird's)
static const float kChord[4] = {1.0, 1.15, 1.0, 0.8};       // how broad the wings are
static const float kReach[4] = {0.95, 1.0, 1.0, 1.15};      // how long
static const float kBeat[4] = {8.0, 3.4, 5.6, 2.4};          // wingbeats a second
static const float kGlide[4] = {0.2, 0.55, 0.3, 0.72};       // share of the time a calm bird glides
static const float kApart[4] = {0.45, 1.0, 0.6, 1.3};        // how close it lets the next bird come
static const float kSees[4] = {3.0, 5.5, 4.0, 7.0};          // how far round it keeps with its flock
static const float3 kTop[4] = {float3(0.035, 0.034, 0.04), float3(0.026, 0.026, 0.03), float3(0.24, 0.24, 0.26), float3(0.42, 0.44, 0.47)};
static const float3 kUnder[4] = {float3(0.04, 0.038, 0.04), float3(0.03, 0.03, 0.032), float3(0.2, 0.2, 0.21), float3(0.72, 0.72, 0.7)};
static const float3 kTip[4] = {float3(0.03, 0.03, 0.034), float3(0.022, 0.022, 0.025), float3(0.05, 0.05, 0.055), float3(0.025, 0.025, 0.028)};
static const float kRough[4] = {0.42, 0.5, 0.8, 0.7};         // (the dark ones' feathers have a sheen that catches the sun)

uint LifeHash(uint x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}
float LifeRand(uint h) { return float(h & 0xffffffu) / 16777216.0; }

cbuffer Birds : register(b1) {
  float4 info;     // x seconds this step; y the clock; z 1 = every bird placed afresh; w the water's level (very low: none)
  float4 mapInfo;  // x, y the map's width and height; z world units of the heights' largest value
  float4 shown;    // x the birds' size; y world units a pixel spans at distance 1; z the share of birds drawn; w how far from the eye
  float4 eye;      // xyz the camera's eye (also in a pass for the sun's shadow maps, whose view has an eye of its own)
  // per flock four rows: the place it makes for (xyz) and what it does (w: 0 resting, 1 aloft, 2 fleeing, 3 landing);
  // the noise that scattered it (xyz) and seconds since (w, below 0: none); its birds, kind, seconds since it began
  // what it does, its calm speed; how high it keeps over the ground, how wide it wheels, its fleeing speed, its seed
  float4 flocks[FLOCKS * 4];
  float4 drawn[FLOCKS / 4];  // the flocks drawn this frame, by number
};
Texture2D<float> heightMap : register(t4);
float LifeGround(float2 xz) {
  float2 p = clamp(xz, 0, mapInfo.xy);
  float2 f = floor(p);
  float2 t = p - f;
  int2 c = int2(f);
  int2 most = int2(mapInfo.xy);
  float a = heightMap.Load(int3(min(c, most), 0)), b = heightMap.Load(int3(min(c + int2(1, 0), most), 0));
  float d = heightMap.Load(int3(min(c + int2(0, 1), most), 0)), e = heightMap.Load(int3(min(c + int2(1, 1), most), 0));
  return lerp(lerp(a, b, t.x), lerp(d, e, t.x), t.y) * mapInfo.z;
}
uint BirdSeed(float4 C, uint i) { return LifeHash(asuint(C.w) * 977u + i * 7919u + 13u); }
)hlsl";

// The birds' step: a group of 32 threads a flock, a thread a bird. Each bird's place (xyz) and wing's phase (w), its
// velocity (xyz) and how it sits (w: 1 sat on its perch and hidden; 0..1 settling there, shrinking out of sight; 0
// flying; -1..0 just off its perch, growing into sight).
const char kStep[] = R"hlsl(
Texture2D<float4> placeIn : register(t0);
Texture2D<float4> motionIn : register(t1);
Texture2D<float4> perches : register(t2);  // where each sits
RWTexture2D<float4> placeOut : register(u0);
RWTexture2D<float4> motionOut : register(u1);
groupshared float4 sharedPlace[32];
groupshared float4 sharedMotion[32];

[numthreads(32, 1, 1)] void Step(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
  uint f = group.x, i = thread.x;
  float4 A = flocks[f * 4], S = flocks[f * 4 + 1], B = flocks[f * 4 + 2], C = flocks[f * 4 + 3];
  uint n = uint(B.x), kind = min(uint(B.y), 3u);
  int mode = int(A.w + 0.5);
  float dt = info.x;
  int3 at = int3(i, f, 0);
  float4 p = placeIn.Load(at), v = motionIn.Load(at);
  float3 perch = perches.Load(at).xyz;
  uint h = BirdSeed(C, i);
  if (info.z > 0.5) {  // placed afresh: about where its flock flies, or on its perch
    if (mode == 1 || mode == 2) {
      float a = LifeRand(h) * 6.2831853, r = sqrt(LifeRand(LifeHash(h + 1))) * (2 + 0.12 * n);
      p.xyz = A.xyz + float3(cos(a) * r, (LifeRand(LifeHash(h + 2)) - 0.5) * 2, sin(a) * r);
      v = float4(float3(-sin(a), 0, cos(a)) * B.w, 0);
    } else {
      p.xyz = perch;
      v = float4(0, 0, 0, 1);
    }
    p.w = LifeRand(LifeHash(h + 3)) * 6.2831853;
  }
  sharedPlace[i] = p;
  sharedMotion[i] = v;
  GroupMemoryBarrierWithGroupSync();
  if (i >= n || dt <= 0) {
    placeOut[at.xy] = p;
    motionOut[at.xy] = v;
    return;
  }
  // the noise reaches it (a bird nearer it goes first), and it bursts away
  float delay = length(p.xyz - S.xyz) / 28 + LifeRand(LifeHash(h + 5)) * 0.35;
  bool panic = mode == 2 && S.w >= 0 && S.w > delay;
  float3 away = p.xyz - S.xyz;
  away.y = 0;
  away = length(away) > 0.01 ? normalize(away) : normalize(float3(LifeRand(LifeHash(h + 6)) - 0.5, 0, LifeRand(LifeHash(h + 7)) - 0.5) + 1e-3);
  if (v.w > 0) {  // on its perch: off when its flock takes off (each in its own time) or the noise reaches it
    bool go = false;
    float3 launch = 0;
    if (panic) {
      go = true;
      launch = normalize(away + float3(0, 1.4, 0)) * C.z * 0.8;
    } else if (mode == 1 && B.z > LifeRand(LifeHash(h + 8)) * 4) {
      go = true;
      float a = LifeRand(LifeHash(h + 9)) * 6.2831853;
      launch = normalize(float3(cos(a), 1.6, sin(a))) * B.w * 0.7;
    }
    if (!go) {
      p.xyz = perch;
      v = float4(0, 0, 0, min(v.w + dt / 0.5, 1));
      placeOut[at.xy] = p;
      motionOut[at.xy] = v;
      return;
    }
    v = float4(launch, -1);
  }
  // its flock round it: keep apart from the nearest, fly as they fly, keep to them
  float3 apart = 0, along = 0, middle = 0;
  float count = 0;
  float near = kApart[kind], sees = kSees[kind];
  for (uint j = 0; j < n; ++j) {
    if (j == i) continue;
    float4 q = sharedPlace[j], w = sharedMotion[j];
    if (w.w > 0) continue;  // (sitting)
    float3 d = q.xyz - p.xyz;
    float dd = dot(d, d);
    if (dd < sees * sees) {
      if (dd < near * near) apart -= d / max(dd, 0.02) * near;
      along += w.xyz;
      middle += q.xyz;
      count += 1;
    }
  }
  float cruise = B.w, flee = C.z;
  bool landing = mode == 0 || mode == 3;
  float3 want;
  float most;
  float3 toPerch = perch - p.xyz;
  float flatToPerch = length(toPerch.xz);
  if (landing) {  // home to its perch, in from above
    if (length(toPerch) < 0.3) {
      p.xyz = perch;
      v = float4(0, 0, 0, 0.02);
      placeOut[at.xy] = p;
      motionOut[at.xy] = v;
      return;
    }
    float3 goal = perch + float3(0, saturate((flatToPerch - 2) / 10) * 4, 0);
    float3 toGoal = goal - p.xyz;
    float d = length(toGoal);
    want = toGoal / max(d, 1e-3) * min(cruise, d * 1.5 + 0.4);
    most = 10;
  } else if (panic) {  // away from the noise and up, then for the place away from the fight
    float3 toAim = A.xyz - p.xyz;
    float t = saturate((S.w - delay - 1.5) / 3);
    float3 burst = normalize(away + float3(0, 0.45, 0));
    want = normalize(lerp(burst, toAim / max(length(toAim), 1e-3), t) + 1e-4) * flee;
    most = 22;
  } else {  // calm: for the place its flock makes for, wheeling round it when near
    float3 toAim = A.xyz - p.xyz;
    float d = length(toAim);
    float3 flat = float3(toAim.x, 0, toAim.z) + float3(1e-3, 0, 0);
    float3 around = normalize(cross(float3(0, 1, 0), flat)) * (LifeRand(LifeHash(asuint(C.w) + 77u)) < 0.5 ? 1 : -1);
    float wheel = saturate(1.5 - length(flat) / max(C.y, 1));
    want = normalize(toAim / max(d, 1e-3) * (1 - wheel * 0.75) + around * wheel + 1e-4) * cruise;
    want.y = clamp(toAim.y, -2, 2) * 0.6;
    most = mode == 2 ? 9 : 5;
  }
  float3 steer = (want - v.xyz) * 1.6 + apart * 3;
  if (count > 0) steer += (along / count - v.xyz) * 0.9 + (middle / count - p.xyz) * 0.5;
  // above the ground, the woods' crowns and the water (landing: down to its perch near it)
  float floorY = max(LifeGround(p.xz), info.w) + C.x;
  if (landing) floorY = lerp(floorY, perch.y - 0.3, saturate(1 - flatToPerch / 6));
  if (p.y < floorY) steer.y += (floorY - p.y) * 6;
  float2 inside = clamp(p.xz, 8, mapInfo.xy - 8);
  steer.xz += (inside - p.xz) * 2;
  float s = length(steer);
  if (s > most) steer *= most / s;
  v.xyz += steer * dt;
  float speed = length(v.xyz);
  float lo = landing ? 0.2 : cruise * 0.55, hi = panic ? flee * 1.25 : cruise * 1.6;
  if (speed > hi) v.xyz *= hi / speed;
  else if (speed < lo) v.xyz = (speed > 1e-3 ? v.xyz / speed : float3(0, 0, 1)) * lo;
  p.xyz += v.xyz * dt;
  if (v.w < 0) v.w = min(v.w + dt / 0.35, 0);
  // its wings: quicker when it flees or climbs
  float beat = kBeat[kind] * (panic ? 1.5 : 1) * (v.y > 0.3 ? 1.2 : 1);
  p.w = fmod(p.w + dt * beat * 6.2831853, 6.2831853 * 16);
  placeOut[at.xy] = p;
  motionOut[at.xy] = v;
}
)hlsl";

// A bird: nine triangles, no vertex data (a bird's number and the corner's give everything). In its own frame (x to
// its side, y up, z forward), a wingspan of 1.04: a body and a tail, and each wing in two parts, the inner turned up
// or down by the flap and the outer by more, a little after it.
const char kBirds[] = R"hlsl(
Texture2D<float4> placeNow : register(t0);
Texture2D<float4> motionNow : register(t1);
Texture2D<float4> placeBefore : register(t2);
Texture2D<float4> motionBefore : register(t3);

struct VOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
  float3 normal : TEXCOORD1;  // the bird's upper side
  float2 info : TEXCOORD2;    // x its kind; y how much of a wingtip's black
};

float3 Corner(uint tri, uint k, uint kind, float inner, float outer, out float tip) {
  tip = 0;
  bool left = tri >= 6;
  uint t = left ? tri - 3 : tri;
  float3 c;
  if (t < 3) {  // the body and the tail
    static const float3 kBody[9] = {float3(0, 0.01, 0.30), float3(0.075, 0.01, 0.02), float3(0, 0, -0.22),
                                    float3(0, 0.01, 0.30), float3(0, 0, -0.22), float3(-0.075, 0.01, 0.02),
                                    float3(0, 0, -0.17), float3(0.09, 0, -0.36), float3(-0.09, 0, -0.36)};
    return kBody[t * 3 + k];
  }
  // a wing: broad at the shoulder, narrower at the elbow, to a point (the gulls' longer and narrower)
  float chord = kChord[kind], reach = kReach[kind];
  float3 sf = float3(0.05, 0, 0.13 * chord), sb = float3(0.05, 0, -0.12 * chord);
  float3 d1 = float3(cos(inner), sin(inner), 0);
  float3 ef = sf + d1 * (0.23 * reach) + float3(0, 0, -0.02), eb = sb + d1 * (0.23 * reach) + float3(0, 0, 0.05 * chord);
  float3 d2 = float3(cos(inner + outer), sin(inner + outer), 0);
  float3 wingTip = (ef + eb) * 0.5 + d2 * (0.27 * reach) + float3(0, 0, -0.07);
  uint w = (t - 3) * 3 + k;  // (shoulder front, elbow front, shoulder back; shoulder back, elbow front, elbow back; elbow front, tip, elbow back)
  c = w == 0 ? sf : w == 1 ? ef : w == 2 ? sb : w == 3 ? sb : w == 4 ? ef : w == 5 ? eb : w == 6 ? ef : w == 7 ? wingTip : eb;
  tip = w == 7 ? 1 : (w == 6 || w == 8) ? 0.25 : 0;
  if (left) c.x = -c.x;
  return c;
}

VOut VS(uint id : SV_VertexID) {
  VOut o = (VOut)0;
  o.position = float4(0, 0, -1, 1);  // (none: behind the eye)
  uint bird = id / 27, corner = id % 27;
  uint slot = bird / 32, i = bird % 32;
  uint f = uint(drawn[slot / 4][slot % 4]);
  float4 A = flocks[f * 4], B = flocks[f * 4 + 2], C = flocks[f * 4 + 3];
  uint n = uint(B.x), kind = min(uint(B.y), 3u);
  if (i >= n) return o;
  uint h = BirdSeed(C, i);
  if (LifeRand(LifeHash(h + 21)) > shown.z) return o;  // (fewer as the camera rises)
  int3 at = int3(i, f, 0);
  float4 p = placeNow.Load(at), v = motionNow.Load(at), vb = motionBefore.Load(at);
  if (v.w >= 1) return o;  // (sat, out of sight)
  float far = length(p.xyz - eye.xyz);
  if (far > shown.w) return o;
  float grown = v.w > 0 ? 1 - v.w : v.w < 0 ? 1 + v.w : 1;
  float span = kSpan[kind] * shown.x * (0.85 + 0.3 * LifeRand(LifeHash(h + 22)));
  span = max(span, shown.y * far * 2.5) * grown;  // (at least two and a half pixels across)
  // its frame: forward along its flight, banked into its turn
  float speed = length(v.xyz);
  float3 forward = speed > 0.05 ? v.xyz / speed : float3(0, 0, 1);
  float3 side = cross(float3(0, 1, 0), forward);
  side = length(side) > 0.05 ? normalize(side) : float3(1, 0, 0);
  float3 up = cross(forward, side);
  float bank = clamp(dot(v.xyz - vb.xyz, side) / max(info.x, 1e-3) * 0.12, -0.9, 0.9);
  float3 sideB = side * cos(bank) + up * sin(bank), upB = up * cos(bank) - side * sin(bank);
  // its wings: flapping, or gliding a while (more for the crows and gulls), always flapping when it climbs, flees,
  // takes off or lands
  float cycle = frac(time / (2.2 + 2.5 * LifeRand(LifeHash(h + 23))) + LifeRand(LifeHash(h + 24)));
  float flapping = saturate((cycle - kGlide[kind]) * 10) * saturate((1 - cycle) * 10);
  if (v.y > 0.25 || int(A.w + 0.5) != 1 || v.w != 0) flapping = 1;
  float amp = 0.95 * flapping;
  float inner = (kind == 3 ? 0.16 : 0.07) + amp * 0.45 * sin(p.w);
  float outer = (kind == 3 ? -0.3 : 0.0) + amp * 0.5 * sin(p.w - 0.7);
  uint tri = corner / 3, k = corner % 3;
  float tip;
  float3 c = Corner(tri, k, kind, inner, outer, tip);
  float scale = span / (0.1 + 1.0 * kReach[kind]);  // (the shape's own wingspan: 1.1 at reach 1)
  float3 world = p.xyz + (sideB * c.x + upB * c.y + forward * c.z) * scale;
  // the triangle's face, toward its upper side
  float t0, t1, t2;
  float3 a = Corner(tri, 0, kind, inner, outer, t0), b = Corner(tri, 1, kind, inner, outer, t1), d = Corner(tri, 2, kind, inner, outer, t2);
  float3 face = cross(b - a, d - a);
  face = length(face) > 1e-6 ? normalize(face) : float3(0, 1, 0);
  if (face.y < 0) face = -face;
  o.normal = normalize(sideB * face.x + upB * face.y + forward * face.z);
  o.world = world;
  o.position = mul(viewProj, float4(world, 1));
  o.info = float2(kind, tip);
  return o;
}

// (into the sun's shadow maps: the depth alone)
void ShadowPS(VOut i) {}

float4 PS(VOut i) : SV_Target {
  uint kind = uint(i.info.x + 0.5);
  float3 n = normalize(i.normal);
  float3 toEye = cameraPosition - i.world;
  bool above = dot(n, toEye) >= 0;  // (the eye sees its upper side)
  float3 albedo = lerp(above ? kTop[kind] : kUnder[kind], kTip[kind], saturate(i.info.y));
  if (!above) n = -n;
  return float4(ModernLight(albedo, 0, kRough[kind], 1, 0, n, i.world, SunShadow(i.world, n)), 1);
}
)hlsl";

struct BirdConstants {
  float info[4], mapInfo[4], shown[4], eye[4];
  float flocks[Wildlife::kFlocksMost * 4][4];
  float drawn[Wildlife::kFlocksMost / 4][4];
};

// Fireflies: six corners each, a card facing the eye; a firefly's number gives its cell and place.
const char kFireflies[] = R"hlsl(
cbuffer Fireflies : register(b1) {
  float4 area;     // x, y the first cell's corner (world x, z); z a cell's size; w cells a side
  float4 mapInfo;  // x, y the map's width and height; z world units of the heights' largest value; w the water's level
  float4 glow;     // x the share of places that hold one (the night's, the weather's); y how bright; z world units a pixel spans at distance 1; w how far from the eye
  float4 woodsAt;  // x, y the world size the woods' picture covers
};
Texture2D<float> heightMap : register(t0);
Texture2D groundColours : register(t1);  // (alpha: how grassy)
Texture2D<float> woods : register(t2);   // how thick the trees stand

uint FlyHash(uint x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}
float FlyRand(uint h) { return float(h & 0xffffffu) / 16777216.0; }
float FlyGround(float2 xz) {
  float2 p = clamp(xz, 0, mapInfo.xy);
  float2 f = floor(p);
  float2 t = p - f;
  int2 c = int2(f);
  int2 most = int2(mapInfo.xy);
  float a = heightMap.Load(int3(min(c, most), 0)), b = heightMap.Load(int3(min(c + int2(1, 0), most), 0));
  float d = heightMap.Load(int3(min(c + int2(0, 1), most), 0)), e = heightMap.Load(int3(min(c + int2(1, 1), most), 0));
  return lerp(lerp(a, b, t.x), lerp(d, e, t.x), t.y) * mapInfo.z;
}

struct FOut {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
  float3 color : TEXCOORD1;
  float3 world : TEXCOORD2;
};

static const float2 kCard[6] = {float2(-1, -1), float2(1, -1), float2(-1, 1), float2(-1, 1), float2(1, -1), float2(1, 1)};

FOut VS(uint id : SV_VertexID) {
  FOut o = (FOut)0;
  o.position = float4(0, 0, -1, 1);
  uint fly = id / 6, corner = id % 6;
  uint cell = fly / PER_CELL, k = fly % PER_CELL;
  uint side = uint(area.w);
  float2 cellAt = area.xy + float2(cell % side, cell / side) * area.z;
  int2 cellId = int2(floor(cellAt / area.z + 0.5));
  uint h = FlyHash(uint(cellId.x) * 73856093u ^ FlyHash(uint(cellId.y) * 19349663u + k * 83492791u + 5u));
  if (FlyRand(h) > glow.x) return o;  // (most places hold none)
  float2 anchor = cellAt + float2(FlyRand(FlyHash(h + 1)), FlyRand(FlyHash(h + 2))) * area.z;
  if (any(anchor < 0) || any(anchor > mapInfo.xy)) return o;
  // where they keep: along the woods' edges (in them a little), and over grass
  float2 uv = anchor / woodsAt.xy, apart = 6 / woodsAt.xy;
  float w0 = woods.SampleLevel(samplerLinearClamp, uv, 0);
  float w1 = woods.SampleLevel(samplerLinearClamp, uv + float2(apart.x, 0), 0), w2 = woods.SampleLevel(samplerLinearClamp, uv - float2(apart.x, 0), 0);
  float w3 = woods.SampleLevel(samplerLinearClamp, uv + float2(0, apart.y), 0), w4 = woods.SampleLevel(samplerLinearClamp, uv - float2(0, apart.y), 0);
  float most = max(w0, max(max(w1, w2), max(w3, w4))), least = min(w0, min(min(w1, w2), min(w3, w4)));
  float edge = saturate(most * 2) * (1 - 0.7 * saturate(least * 1.5));
  // (over open grass only in patches, a few tens of units across: a wet meadow here, nothing there)
  float grass = groundColours.SampleLevel(samplerLinearClamp, anchor / mapInfo.xy, 0).a;
  float patch = smoothstep(0.55, 0.8, CloudNoise(float3(anchor * 0.035, 3.1)));
  if (FlyRand(FlyHash(h + 3)) > saturate(edge * 0.9 + grass * patch * 0.45)) return o;
  if (FlyGround(anchor) < mapInfo.w + 0.1) return o;  // (none over the water)
  // fewer toward the square's edge
  float halfSide = area.z * area.w * 0.5;
  float thin = saturate((1 - length(anchor - (area.xy + halfSide)) / halfSide) * 4);
  // drifting slowly, low over the ground (higher in a wood)
  float r1 = FlyRand(FlyHash(h + 4)), r2 = FlyRand(FlyHash(h + 5)), r3 = FlyRand(FlyHash(h + 6));
  float2 drift = float2(sin(time * (0.21 + 0.2 * r1) + r2 * 6.28) + 0.5 * sin(time * (0.53 + 0.3 * r3) + r1 * 6.28),
                        cos(time * (0.17 + 0.2 * r2) + r3 * 6.28) + 0.5 * sin(time * (0.47 + 0.3 * r1) + r2 * 6.28)) * 0.9;
  float2 xz = anchor + drift;
  float lift = 0.2 + 0.5 * r3 + 0.7 * saturate(w0 * 2) * r1 + 0.12 * sin(time * (0.6 + 0.4 * r2) + r1 * 9);
  float3 at = float3(xz.x, FlyGround(xz) + lift, xz.y);
  float far = length(at - cameraPosition);
  if (far > glow.w) return o;
  // its glow: a flash of about half a second every few seconds, the faintest glow between
  float period = 2.5 + 4.5 * FlyRand(FlyHash(h + 7));
  float u = frac(time / period + FlyRand(FlyHash(h + 8))) * period;
  float on = u < 0.55 ? sin(3.14159265 * u / 0.55) : 0;
  on = on * on * 0.97 + 0.012;
  // (its core 0.05 across; never under a pixel and a third, the light then spread over it)
  float core = 0.05, size = max(core, glow.z * far * 0.65);
  float energy = (core / size) * (core / size);
  float3 toEye = normalize(cameraPosition - at);
  float3 right = normalize(cross(float3(0, 1, 0), toEye) + float3(1e-4, 0, 0)), up = cross(toEye, right);
  float2 c = kCard[corner];
  o.position = mul(viewProj, float4(at + (right * c.x + up * c.y) * (size * 2.5), 1));
  o.uv = c * 2.5;
  float3 hue = lerp(float3(0.45, 1.0, 0.06), float3(0.85, 1.0, 0.12), FlyRand(FlyHash(h + 9)));
  o.color = hue * (glow.y * on * energy * thin * saturate((glow.w - far) / (glow.w * 0.25)) * (falling.w > 0 ? falling.w : 1));
  o.world = at;
  return o;
}

float4 PS(FOut i) : SV_Target {
  float3 c = i.color * exp(-dot(i.uv, i.uv) * 1.6);
  // seen through the air: the light in the air's, or the haze and the mist
  [branch] if (air.x > 0.5) {
    c *= AirAt(i.world).a;
  } else {
    if (weather.z > 0) c *= 1 - min(1 - exp(-length(i.world - cameraPosition) * weather.z), weather.w);
    if (mist.x > 0) c *= 1 - MistLost(i.world);
  }
  return float4(c, 1);
}
)hlsl";

struct FireflyConstants {
  float area[4], mapInfo[4], glow[4], woodsAt[4];
};

uint32_t Mix(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

float Smooth(float a, float b, float x) {
  const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}

}  // namespace

Wildlife::~Wildlife() {
  for (gfx::Pipeline p : {step_, birdsDraw_, birdShadows_, fireflyDraw_})
    if (p) device_->Destroy(p);
  for (gfx::Texture t : {place_[0], place_[1], motion_[0], motion_[1], perchMap_, woodsMap_})
    if (t) device_->Destroy(t);
}

float Wildlife::Random(Flock& f, float lo, float hi) {
  f.random = Mix(f.random + 0x9e3779b9u);
  return lo + (hi - lo) * static_cast<float>(f.random & 0xffffffu) / 16777216.0f;
}

float Wildlife::GroundAt(float x, float z) const {
  return map_ ? map_->Height(std::clamp(x, 0.0f, mapWidth_), std::clamp(z, 0.0f, mapHeight_)) : 0;
}

void Wildlife::SetMap(const MapData* map, const std::vector<Vec4>& trees) {
  map_ = map;
  flocks_.clear();
  perches_.clear();
  woods_.clear();
  heard_.clear();
  shots_.clear();
  beams_.clear();
  birds_ = 0;
  scares = heard = 0;
  planned_ = false;
  fresh_ = true;
  lastTime_ = -1;
  now_ = 0;
  lastFlash_ = -1e9;
  // (the birds' targets, the perches and the woods' picture are the map's: made again at the next Update)
  for (gfx::Texture* t : {&place_[0], &place_[1], &motion_[0], &motion_[1], &perchMap_, &woodsMap_}) {
    if (*t) device_->Destroy(*t);
    *t = {};
  }
  if (!map || map->width <= 0 || map->height <= 0) return;
  mapWidth_ = static_cast<float>(map->width);
  mapHeight_ = static_cast<float>(map->height);
  water_ = map->water.present ? map->water.elevation : -1e9f;

  // the woods: the trees counted by squares; a flock's home where they stand thickest, the thickest first, none
  // nearer another than kHomesApart
  const int sw = std::max(1, static_cast<int>(std::ceil(mapWidth_ / kHomeSquare))), sh = std::max(1, static_cast<int>(std::ceil(mapHeight_ / kHomeSquare)));
  struct Square {
    float count = 0;
    double x = 0, z = 0, top = 0, tall = 0;
  };
  std::vector<Square> squares(static_cast<size_t>(sw) * static_cast<size_t>(sh));
  for (const Vec4& t : trees) {
    const int cx = std::clamp(static_cast<int>(t.x / kHomeSquare), 0, sw - 1), cz = std::clamp(static_cast<int>(t.z / kHomeSquare), 0, sh - 1);
    Square& s = squares[static_cast<size_t>(cz) * static_cast<size_t>(sw) + static_cast<size_t>(cx)];
    s.count += 1;
    s.x += t.x;
    s.z += t.z;
    s.top += t.y + t.w;
    s.tall += t.w;
  }
  std::vector<int> order(squares.size());
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return squares[static_cast<size_t>(a)].count > squares[static_cast<size_t>(b)].count; });
  std::vector<int> perchTrees;
  for (int k : order) {
    const Square& s = squares[static_cast<size_t>(k)];
    if (s.count < 8 || static_cast<int>(flocks_.size()) >= kFlocksMost - kGullFlocksMost) break;
    const float n = s.count;
    const Vec3 home{static_cast<float>(s.x / n), static_cast<float>(s.top / n), static_cast<float>(s.z / n)};
    bool near = false;
    for (const Flock& f : flocks_) near = near || (f.home.x - home.x) * (f.home.x - home.x) + (f.home.z - home.z) * (f.home.z - home.z) < kHomesApart * kHomesApart;
    if (near) continue;
    Flock f;
    f.home = home;
    f.random = Mix(static_cast<uint32_t>(k) * 2654435761u + 0x51f15e5u);
    f.reach = Random(f, 20, 30);
    const float which = Random(f, 0, 1);
    f.kind = which < 0.3f ? 0 : which < 0.55f ? 1 : 2;
    f.birds = f.kind == 0 ? static_cast<int>(Random(f, 18, 32.99f)) : f.kind == 1 ? static_cast<int>(Random(f, 5, 10.99f)) : static_cast<int>(Random(f, 8, 16.99f));
    // where its birds sit: together, in the crowns of the few trees round the one nearest the middle of its wood (its
    // roost; each bird a tree of them, by chance), which is where it comes home to
    perchTrees.clear();
    size_t roost = 0;
    float best = 1e30f;
    for (size_t i = 0; i < trees.size(); ++i) {
      const float dx = trees[i].x - home.x, dz = trees[i].z - home.z;
      if (dx * dx + dz * dz < best) best = dx * dx + dz * dz, roost = i;
    }
    for (size_t i = 0; i < trees.size(); ++i) {
      const float dx = trees[i].x - trees[roost].x, dz = trees[i].z - trees[roost].z;
      if (dx * dx + dz * dz < 9 * 9) perchTrees.push_back(static_cast<int>(i));
    }
    f.home.x = trees[roost].x;
    f.home.z = trees[roost].z;
    for (int b = 0; b < kBirdsMost; ++b) {
      const Vec4& t = perchTrees.empty() ? Vec4{home.x, home.y, home.z, 0} : trees[static_cast<size_t>(perchTrees[static_cast<size_t>(Random(f, 0, static_cast<float>(perchTrees.size()) - 0.01f))])];
      perches_.insert(perches_.end(), {t.x + Random(f, -0.4f, 0.4f), t.y + t.w * Random(f, 0.75f, 0.92f), t.z + Random(f, -0.4f, 0.4f), 0});
    }
    f.clearance = static_cast<float>(s.tall / n) + 2.5f;  // (over its wood's crowns)
    f.aim = f.home;
    flocks_.push_back(f);
  }
  // gulls: over water a little off a shore, the shores far apart
  const size_t woodFlocks = flocks_.size();
  if (map->water.present) {
    int gulls = 0;
    uint32_t r = 0x6a09e667u;
    for (int tries = 0; tries < 4000 && gulls < kGullFlocksMost; ++tries) {
      r = Mix(r + 0x9e3779b9u);
      const float x = 16 + static_cast<float>(r & 0xffffu) / 65536.0f * (mapWidth_ - 32);
      r = Mix(r + 0x9e3779b9u);
      const float z = 16 + static_cast<float>(r & 0xffffu) / 65536.0f * (mapHeight_ - 32);
      if (water_ - GroundAt(x, z) < 1.0f) continue;  // (not over open water)
      bool shore = false;
      for (int a = 0; a < 12 && !shore; ++a) {
        const float angle = static_cast<float>(a) * 0.5236f;
        for (float d : {15.0f, 30.0f, 45.0f}) shore = shore || GroundAt(x + std::cos(angle) * d, z + std::sin(angle) * d) > water_;
      }
      if (!shore) continue;
      bool near = false;
      for (size_t i = woodFlocks; i < flocks_.size(); ++i)
        near = near || (flocks_[i].home.x - x) * (flocks_[i].home.x - x) + (flocks_[i].home.z - z) * (flocks_[i].home.z - z) < kGullsApart * kGullsApart;
      if (near) continue;
      Flock f;
      f.home = {x, water_, z};
      f.aim = f.home;
      f.random = Mix(r ^ 0x3c6ef372u);
      f.kind = 3;
      f.reach = Random(f, 28, 40);
      f.birds = static_cast<int>(Random(f, 5, 11.99f));
      // (they rest on the water near their home)
      for (int b = 0; b < kBirdsMost; ++b) {
        Vec3 at = f.home;
        for (int k = 0; k < 8; ++k) {
          const float px = x + Random(f, -14, 14), pz = z + Random(f, -14, 14);
          if (GroundAt(px, pz) < water_ - 0.3f) {
            at = {px, water_, pz};
            break;
          }
        }
        perches_.insert(perches_.end(), {at.x, at.y, at.z, 0});
      }
      f.clearance = 1.5f;
      flocks_.push_back(f);
      ++gulls;
    }
  }
  for (Flock& f : flocks_) birds_ += f.birds;

  // the fireflies' picture of the woods: each tree's crown laid on squares of kWoodsCell, softened
  woodsW_ = std::max(1, static_cast<int>(std::ceil(mapWidth_ / kWoodsCell)));
  woodsH_ = std::max(1, static_cast<int>(std::ceil(mapHeight_ / kWoodsCell)));
  std::vector<float> thick(static_cast<size_t>(woodsW_) * static_cast<size_t>(woodsH_), 0.0f);
  for (const Vec4& t : trees) {
    const int cx = static_cast<int>(t.x / kWoodsCell), cz = static_cast<int>(t.z / kWoodsCell);
    for (int dz = -1; dz <= 1; ++dz)
      for (int dx = -1; dx <= 1; ++dx) {
        const int x = cx + dx, z = cz + dz;
        if (x < 0 || z < 0 || x >= woodsW_ || z >= woodsH_) continue;
        thick[static_cast<size_t>(z) * static_cast<size_t>(woodsW_) + static_cast<size_t>(x)] += dx == 0 && dz == 0 ? 0.5f : 0.15f;
      }
  }
  woods_.resize(thick.size());
  for (size_t i = 0; i < thick.size(); ++i) woods_[i] = static_cast<uint8_t>(std::min(thick[i], 1.0f) * 255.0f + 0.5f);
}

void Wildlife::Hear(Vec3 at, float loudness) {
  if (loudness <= 0 || heard_.size() >= 4096) return;
  heard_.push_back({at, loudness});
  ++heard;
}

void Wildlife::Listen(const Frame& frame, const FrameCombat& combat) {
  ++stamp_;
  // (only what the army the picture is for sees: birds scattering must not give away a fight in the fog of war)
  // shots: one first seen is a weapon firing; one gone since the last frame has met something (where the engine's
  // impacts are kept, they say where instead)
  for (const FrameProjectile& p : frame.projectiles) {
    const Vec3 at{p.position[0], p.position[1], p.position[2]};
    auto [it, fresh] = shots_.try_emplace(p.id);
    it->second.at = at;
    it->second.stamp = stamp_;
    it->second.seen = p.seen;
    if (fresh && p.seen && p.age < 0.5f) Hear(at, 0.5f);
  }
  for (auto it = shots_.begin(); it != shots_.end();) {
    if (it->second.stamp == stamp_) {
      ++it;
      continue;
    }
    if (!combat.impactsKept && it->second.seen) Hear(it->second.at, 1.0f);
    it = shots_.erase(it);
  }
  // beams: heard where they start and end when they come on
  for (const FrameBeam& b : frame.beams) {
    auto [it, fresh] = beams_.try_emplace(b.id);
    it->second.stamp = stamp_;
    if (fresh) {
      Hear({b.start[0], b.start[1], b.start[2]}, 0.6f);
      Hear({b.end[0], b.end[1], b.end[2]}, 0.6f);
    }
  }
  for (auto it = beams_.begin(); it != beams_.end();) it = it->second.stamp == stamp_ ? std::next(it) : beams_.erase(it);
  // the engine's impacts and the hits over an area
  if (combat.impactsKept)
    for (const FrameImpact& m : combat.impacts) {
      if (!m.seen) continue;
      const Vec3 at{m.position[0], m.position[1], m.position[2]};
      if (m.kind == FrameImpact::Crash) Hear(at, 2.5f);
      else if (m.kind == FrameImpact::Beam) Hear(at, 0.5f);
      else Hear(at, m.type == "Air" ? 0.6f : 1.0f);
    }
  for (const FrameHit& h : combat.hits)
    if (h.radius > 0 && h.seen) Hear({h.position[0], h.position[1], h.position[2]}, std::min(1 + h.radius * 0.25f, 6.0f));
}

void Wildlife::PickAim(Flock& f, float now) {
  const float a = Random(f, 0, 6.2831853f), r = std::sqrt(Random(f, 0, 1)) * f.reach * (f.returning ? 0.3f : 0.8f);
  f.aim = {f.home.x + std::cos(a) * r, 0, f.home.z + std::sin(a) * r};
  f.aim.x = std::clamp(f.aim.x, 12.0f, mapWidth_ - 12);
  f.aim.z = std::clamp(f.aim.z, 12.0f, mapHeight_ - 12);
  f.aim.y = f.kind == 3 ? std::max(water_, GroundAt(f.aim.x, f.aim.z)) + Random(f, 2, 6) : std::max(f.home.y, GroundAt(f.aim.x, f.aim.z) + 3) + Random(f, 3.5f, 9);
  f.aimUntil = now + Random(f, 5, 11);
}

void Wildlife::Scatter(Flock& f, const Noise& n, float now) {
  const Vec3 centre = f.state == Resting || f.state == Landing ? f.home : f.aim;
  const float dx = centre.x - n.at.x, dz = centre.z - n.at.z, d = std::sqrt(dx * dx + dz * dz);
  const float reach = 40 * std::sqrt(n.loudness);
  if (d > reach) return;
  if (f.state == Fleeing) {
    f.quietSince = now;  // (the fight goes on: it stays away)
    // (only a near noise, once it has had time to get away from the last, turns it anew)
    if (now - f.scareAt < 3 || d > reach * 0.6f) return;
  }
  f.state = Fleeing;
  f.since = now;
  f.scareAt = f.quietSince = now;
  f.scareFrom = n.at;
  f.settleAfter = Random(f, 22, 38);
  f.returning = false;
  Vec3 away{dx, 0, dz};
  if (d < 0.5f) {
    const float a = Random(f, 0, 6.2831853f);
    away = {std::cos(a), 0, std::sin(a)};
  } else {
    away = away * (1 / d);
  }
  const Vec3 across{-away.z, 0, away.x};
  const float far = Random(f, 45, 70), aside = Random(f, -18, 18);
  f.refuge = {centre.x + away.x * far + across.x * aside, 0, centre.z + away.z * far + across.z * aside};
  f.refuge.x = std::clamp(f.refuge.x, 16.0f, mapWidth_ - 16);
  f.refuge.z = std::clamp(f.refuge.z, 16.0f, mapHeight_ - 16);
  f.refuge.y = f.kind == 3 ? std::max(water_, GroundAt(f.refuge.x, f.refuge.z)) + Random(f, 4, 8) : std::max(f.home.y, GroundAt(f.refuge.x, f.refuge.z) + 3) + Random(f, 8, 14);
  f.aim = f.refuge;
  f.aimUntil = now + 8;
  ++scares;
  if (verbose)
    printf("render: wildlife: %.1f s: the flock at %.0f, %.0f scattered by a noise at %.0f, %.0f (loudness %.1f, %.0f away), off toward %.0f, %.0f\n", now,
           f.home.x, f.home.z, n.at.x, n.at.z, n.loudness, d, f.refuge.x, f.refuge.z);
}

// The flocks' days: resting in their wood, aloft over it a while, landing again; to roost when it grows dark;
// scattered by fighting (Scatter) and home again after a quiet while.
void Wildlife::Plan(float now, float sunHeight) {
  if (!planned_) {  // (the first states: by the hour, most flocks up by day)
    planned_ = true;
    for (Flock& f : flocks_) {
      const bool day = sunHeight > kRoost[f.kind];
      f.since = now - 100;
      if (day && Random(f, 0, 1) < 0.6f) {
        f.state = Aloft;
        f.until = now + Random(f, 10, 70);
        PickAim(f, now);
      } else {
        f.state = Resting;
        f.until = now + (day ? Random(f, 5, 60) : Random(f, 30, 90));
      }
    }
  }
  for (Flock& f : flocks_) {
    const bool day = sunHeight > kRoost[f.kind];
    switch (f.state) {
      case Resting:
        if (now >= f.until) {
          if (day) {
            f.state = Aloft;
            f.since = now;
            f.until = now + Random(f, 25, 80);
            f.returning = false;
            PickAim(f, now);
          } else {
            f.until = now + Random(f, 20, 40);
          }
        }
        break;
      case Aloft:
        if (now >= f.aimUntil) PickAim(f, now);
        if (now >= f.until || !day) {  // (time enough to come home from where it is and settle)
          const float far = std::sqrt((f.aim.x - f.home.x) * (f.aim.x - f.home.x) + (f.aim.z - f.home.z) * (f.aim.z - f.home.z));
          f.state = Landing;
          f.since = now;
          f.until = now + 16 + far / kCruise[f.kind];
          f.aim = f.home;
        }
        break;
      case Landing:
        if (now >= f.until) {
          f.state = Resting;
          f.since = now;
          f.until = now + (day ? Random(f, 25, 110) : Random(f, 40, 80));
        }
        break;
      case Fleeing:
        if (now - f.quietSince > f.settleAfter) {  // quiet a while: home again
          f.state = Aloft;
          f.since = now;
          f.returning = true;
          f.until = now + Random(f, 14, 26);
          PickAim(f, now);
        } else if (now >= f.aimUntil) {  // wheeling round its refuge
          const float a = Random(f, 0, 6.2831853f), r = Random(f, 4, 16);
          f.aim = {f.refuge.x + std::cos(a) * r, f.refuge.y + Random(f, -2, 2), f.refuge.z + std::sin(a) * r};
          f.aimUntil = now + Random(f, 4, 8);
        }
        break;
    }
  }
}

bool Wildlife::Ready() {
  if (!tried_) {
    tried_ = true;
    std::string error;
    const std::string defines = "#define FLOCKS " + std::to_string(kFlocksMost) + "\n#define PER_CELL " + std::to_string(kFirefliesPerCell) + "\n";
    const std::string step = std::string(kViewShaderBlock) + defines + kKinds + kStep;
    gfx::ComputeDesc cd;
    cd.name = "wildlife: the birds' step";
    cd.source = step;
    cd.entry = "Step";
    step_ = device_->CreateCompute(cd, &error);
    if (step_) {
      const std::string birds = std::string(kViewShaderBlock) + kLightingShaderBlock + defines + kKinds + kBirds;
      gfx::PipelineDesc pd;
      pd.name = "wildlife: birds";
      pd.vertexSource = pd.pixelSource = birds;
      pd.cull = gfx::Cull::None;
      pd.colorFormat = kSceneFormat;
      pd.samples = gSceneSamples;
      birdsDraw_ = device_->CreatePipeline(pd, &error);
      if (birdsDraw_) {  // (their shadows: as the models' are drawn into the sun's maps)
        gfx::PipelineDesc sd = pd;
        sd.name = "wildlife: birds' shadows";
        sd.pixelEntry = "ShadowPS";
        sd.depthOnly = true;
        sd.samples = 1;
        birdShadows_ = device_->CreatePipeline(sd, &error);
      }
      if (birdsDraw_) {
        const std::string flies = std::string(kViewShaderBlock) + defines + kFireflies;
        pd.name = "wildlife: fireflies";
        pd.vertexSource = pd.pixelSource = flies;
        pd.depthWrite = false;
        pd.blend = gfx::Blend::Additive;
        pd.alphaWrite = false;
        fireflyDraw_ = device_->CreatePipeline(pd, &error);
      }
    }
    made_ = step_ && birdsDraw_ && birdShadows_ && fireflyDraw_;
    if (!made_) fprintf(stderr, "render: wildlife cannot be drawn (%s)\n", error.c_str());
  }
  if (!made_ || !map_) return false;
  if (!woodsMap_ && !woods_.empty()) {  // (the map's: made the first frame after it is opened)
    gfx::TextureDesc td;
    td.width = woodsW_;
    td.height = woodsH_;
    td.format = gfx::Format::R8;
    td.data = woods_.data();
    td.dataSize = woods_.size();
    woodsMap_ = device_->CreateTexture(td);
  }
  if (!flocks_.empty() && !perchMap_) {
    const int rows = static_cast<int>(flocks_.size());
    gfx::TextureDesc td;
    td.width = kBirdsMost;
    td.height = rows;
    td.format = gfx::Format::RGBA32F;
    td.data = perches_.data();
    td.dataSize = perches_.size() * sizeof(float);
    perchMap_ = device_->CreateTexture(td);
    gfx::TargetDesc sd;
    sd.storage = true;
    sd.width = kBirdsMost;
    sd.height = rows;
    sd.format = gfx::Format::RGBA32F;
    for (int k = 0; k < 2; ++k) {
      place_[k] = device_->CreateTarget(sd);
      motion_[k] = device_->CreateTarget(sd);
    }
    fresh_ = true;
    if (verbose)
      printf("render: wildlife: %d flocks (%d of gulls), %d birds; fireflies over %dx%d squares of the woods\n", Flocks(),
             static_cast<int>(std::count_if(flocks_.begin(), flocks_.end(), [](const Flock& f) { return f.kind == 3; })), birds_, woodsW_, woodsH_);
  }
  return static_cast<bool>(woodsMap_);
}

std::vector<Wildlife::FlockInfo> Wildlife::Nearest(Vec3 at, size_t most) const {
  std::vector<FlockInfo> list;
  for (const Flock& f : flocks_) list.push_back({f.home, f.kind, static_cast<int>(f.state), f.birds});
  std::sort(list.begin(), list.end(), [at](const FlockInfo& a, const FlockInfo& b) {
    return (a.home.x - at.x) * (a.home.x - at.x) + (a.home.z - at.z) * (a.home.z - at.z) < (b.home.x - at.x) * (b.home.x - at.x) + (b.home.z - at.z) * (b.home.z - at.z);
  });
  if (list.size() > most) list.resize(most);
  return list;
}

bool Wildlife::Update(const View& view, const Camera& camera, float time, float sunHeight, float rain, float snowLying,
                      const std::vector<Particles::Flash>& flashes, gfx::Texture heights, float heightUnit, gfx::Texture groundColours) {
  drawBirds_ = drawFireflies_ = false;
  birdsDrawn = flocksDrawn = firefliesTried = 0;
  if (!map_ || !heights || !Ready()) {
    heard_.clear();
    return false;
  }
  heights_ = heights;
  ground_ = groundColours;
  // the explosions' flashes started since the last frame (on the particles' clock; one that went back - a replay
  // started over - is taken as the newest)
  double newest = -1e9;
  for (const Particles::Flash& f : flashes) newest = std::max(newest, f.born);
  if (!flashes.empty() && newest < lastFlash_ - 1) lastFlash_ = newest - 1e-6;
  for (const Particles::Flash& f : flashes)
    if (f.born > lastFlash_) Hear(f.position, std::clamp(f.radius * f.strength / 5, 0.3f, 2.5f));
  if (!flashes.empty()) lastFlash_ = std::max(lastFlash_, newest);
  // the clock: the world's, its steps no longer than a tenth of a second (a slow frame, a pause)
  const float dt = lastTime_ < 0 ? 0 : std::clamp(time - lastTime_, 0.0f, 0.1f);
  lastTime_ = time;
  now_ += dt;
  // the noises heard, those close together as one (the loudest), then the flocks they reach scattered
  if (!heard_.empty()) {
    std::vector<Noise> merged;
    for (const Noise& n : heard_) {
      bool joined = false;
      for (Noise& m : merged)
        if (std::fabs(m.at.x - n.at.x) < 10 && std::fabs(m.at.z - n.at.z) < 10) {
          if (n.loudness > m.loudness) m = n;
          joined = true;
          break;
        }
      if (!joined && merged.size() < 64) merged.push_back(n);
    }
    heard_.clear();
    for (Flock& f : flocks_)
      for (const Noise& n : merged) Scatter(f, n, now_);
  }
  if (verbose && !planned_) printf("render: wildlife: the sun's height %.3f (the sine of its elevation)\n", sunHeight);
  Plan(now_, sunHeight);

  // the constants both the step and the draw read
  birdConstants_.assign(sizeof(BirdConstants) / sizeof(float), 0.0f);
  BirdConstants& k = *reinterpret_cast<BirdConstants*>(birdConstants_.data());
  k.info[1] = now_;
  k.info[3] = water_;
  k.mapInfo[0] = mapWidth_;
  k.mapInfo[1] = mapHeight_;
  k.mapInfo[2] = heightUnit;
  const float pixel = 2 * std::tan(camera.fovY * 0.5f) / static_cast<float>(std::max(view.height, 1));
  k.shown[0] = birdScale;
  k.shown[1] = pixel;
  k.shown[2] = 1 - Smooth(85, 240, camera.distance);  // (fewer as the camera rises; none from the strategic zoom)
  k.shown[3] = camera.distance * 1.6f + 120;          // (and none farther from the eye)
  k.eye[0] = view.eye.x;
  k.eye[1] = view.eye.y;
  k.eye[2] = view.eye.z;
  for (size_t i = 0; i < flocks_.size(); ++i) {
    const Flock& f = flocks_[i];
    const float rows[4][4] = {{f.aim.x, f.aim.y, f.aim.z, static_cast<float>(f.state)},
                              {f.scareFrom.x, f.scareFrom.y, f.scareFrom.z, f.state == Fleeing ? now_ - f.scareAt : -1.0f},
                              {static_cast<float>(f.birds), static_cast<float>(f.kind), now_ - f.since, kCruise[f.kind]},
                              {f.clearance, kWheel[f.kind], kFlee[f.kind], static_cast<float>(i)}};
    std::memcpy(&k.flocks[i * 4], rows, sizeof rows);
  }
  // the birds moved on (twice when they are placed afresh, so that the step before is theirs too)
  if (!flocks_.empty() && (dt > 0 || fresh_)) {
    const uint32_t size = static_cast<uint32_t>(offsetof(BirdConstants, flocks) + flocks_.size() * 4 * sizeof k.flocks[0]);
    for (int pass = 0; pass < (fresh_ ? 2 : 1); ++pass) {
      const bool placing = fresh_ && pass == 0;
      k.info[0] = placing ? 0.0f : std::max(dt, 1.0f / 60);
      k.info[2] = placing ? 1.0f : 0.0f;
      device_->SetPipeline(step_);
      device_->SetConstants(0, &view.constants, sizeof view.constants);
      device_->SetConstants(1, &k, size);
      device_->SetTexture(0, place_[cur_]);
      device_->SetTexture(1, motion_[cur_]);
      device_->SetTexture(2, perchMap_);
      device_->SetTexture(4, heights);
      device_->SetStorage(0, place_[1 - cur_]);
      device_->SetStorage(1, motion_[1 - cur_]);
      device_->Dispatch(static_cast<uint32_t>(flocks_.size()));
      device_->SetStorage(0, {});
      device_->SetStorage(1, {});
      for (int slot = 0; slot < 5; ++slot) device_->SetTexture(slot, {});
      cur_ = 1 - cur_;
    }
    fresh_ = false;
  }
  k.info[0] = std::max(dt, 1.0f / 60);  // (the draw: the last step's length, for how the birds bank)
  k.info[2] = 0;
  // the flocks to draw: those up (or just settling), near enough and in the picture (a flock scattered or coming home
  // from it may be anywhere between its home and its refuge)
  if (k.shown[2] > 0.01f && perchMap_) {
    int n = 0;
    for (size_t i = 0; i < flocks_.size(); ++i) {
      const Flock& f = flocks_[i];
      if (f.state == Resting && now_ - f.since > 2) continue;
      Vec3 lo{std::min(f.home.x, f.aim.x), std::min(f.home.y, f.aim.y), std::min(f.home.z, f.aim.z)};
      Vec3 hi{std::max(f.home.x, f.aim.x), std::max(f.home.y, f.aim.y), std::max(f.home.z, f.aim.z)};
      if (f.state == Fleeing || f.returning)
        for (const Vec3& p : {f.refuge, f.scareFrom}) {
          lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
          hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
      lo = lo - Vec3{40, 15, 40};
      hi = hi + Vec3{40, 25, 40};
      const Vec3 nearest{std::clamp(view.eye.x, lo.x, hi.x), std::clamp(view.eye.y, lo.y, hi.y), std::clamp(view.eye.z, lo.z, hi.z)};
      if (Length(nearest - view.eye) > k.shown[3] || !view.BoxVisible(lo, hi)) continue;
      k.drawn[n / 4][n % 4] = static_cast<float>(i);
      ++n;
      birdsDrawn += f.birds;
    }
    flocksDrawn = n;
  }
  drawBirds_ = flocksDrawn > 0;
  // the fireflies: at dusk and by night (fewer in rain, none on lying snow), round what the camera looks at, gone as it
  // rises
  const float dark = std::clamp((0.05f - sunHeight) / 0.15f, 0.0f, 1.0f);  // (from about sunset, all from the end of the twilight)
  const float share = dark * fireflyDensity * (1 - snowLying) * (1 - 0.7f * std::clamp(rain, 0.0f, 1.0f)) * (1 - Smooth(90, 160, camera.distance)) * 0.3f;
  if (share > 0.005f && ground_ && woodsMap_) {
    const float reach = std::clamp(camera.distance * 0.9f, 20.0f, 75.0f);
    const int side = static_cast<int>(std::ceil(2 * reach / kFireflyCell));
    FireflyConstants c{};
    c.area[0] = std::floor((camera.target.x - reach) / kFireflyCell) * kFireflyCell;
    c.area[1] = std::floor((camera.target.z - reach) / kFireflyCell) * kFireflyCell;
    c.area[2] = kFireflyCell;
    c.area[3] = static_cast<float>(side);
    c.mapInfo[0] = mapWidth_;
    c.mapInfo[1] = mapHeight_;
    c.mapInfo[2] = heightUnit;
    c.mapInfo[3] = water_;
    c.glow[0] = std::min(share, 1.0f);
    c.glow[1] = 20 * fireflyStrength;
    c.glow[2] = pixel;
    c.glow[3] = std::min(camera.distance * 1.5f + 40, 220.0f);
    c.woodsAt[0] = static_cast<float>(woodsW_) * kWoodsCell;
    c.woodsAt[1] = static_cast<float>(woodsH_) * kWoodsCell;
    static_assert(sizeof c == sizeof fireflyConstants_);
    std::memcpy(fireflyConstants_, &c, sizeof c);
    fireflyCount_ = side * side * kFirefliesPerCell;
    firefliesTried = fireflyCount_;
    drawFireflies_ = true;
  }
  return drawBirds_ || drawFireflies_;
}

void Wildlife::DrawBirds(const View& view, bool shadow) {
  if (!drawBirds_ || birdConstants_.empty()) return;
  device_->SetPipeline(shadow ? birdShadows_ : birdsDraw_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, birdConstants_.data(), static_cast<uint32_t>(sizeof(BirdConstants)));
  device_->SetTexture(0, place_[cur_]);
  device_->SetTexture(1, motion_[cur_]);
  device_->SetTexture(2, place_[1 - cur_]);
  device_->SetTexture(3, motion_[1 - cur_]);
  device_->SetTexture(4, heights_);
  device_->Draw(static_cast<uint32_t>(flocksDrawn) * kBirdsMost * kBirdCorners);
  for (int slot = 0; slot < 5; ++slot) device_->SetTexture(slot, {});
}

void Wildlife::DrawFireflies(const View& view) {
  if (!drawFireflies_ || fireflyCount_ <= 0) return;
  device_->SetPipeline(fireflyDraw_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, fireflyConstants_, sizeof fireflyConstants_);
  device_->SetTexture(0, heights_);
  device_->SetTexture(1, ground_);
  device_->SetTexture(2, woodsMap_);
  device_->Draw(static_cast<uint32_t>(fireflyCount_) * 6);
  for (int slot = 0; slot < 3; ++slot) device_->SetTexture(slot, {});
}

}  // namespace render
