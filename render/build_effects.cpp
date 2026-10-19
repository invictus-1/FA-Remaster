#include "build_effects.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstring>

#include "effect_files.h"
#include "frame.h"
#include "lights.h"
#include "models.h"

namespace render {

// ---- the build shimmer: the mesh shader's REMASTERED_BUILD variant (mesh_renderer.cpp) ----
//
// RemasteredBuildSolid stands in for the mesh shader's BuildSolid (same answer: how much of the finished surface shows
// at a place, below 0 nothing yet, 0 a line of the lattice; and the work's own light), the variant's BuildSolid being
// a macro that calls it with the place on the model at rest (VOut's `rest`, world units on the model). It is called by
// the depth pass laid first, the shadow passes and the full shading alike, so the holes are the same in all three.
// RemasteredBuildSurface stands in for the building's surface in the modern look (Shade, `if (building)`).
// Lattice lines are anti-aliased: each line at least about a pixel wide (by the derivative of its cell coordinate),
// and where the cells come down to a few pixels across the lattice fills in to a faint glow of the shape, so that far
// off it does not crawl. The derivatives are taken first, before anything branches per pixel.
const char kBuildShaderBlock[] = R"hlsl(
// A lattice's lines in one cell coordinate `c` (lines where its fraction is a half, as the original's), `width` of a
// cell each side of a line, `dc` the coordinate's change over a pixel. x: how much of a line covers this pixel (what is
// cut away and what is not goes by it); y: how bright it is there. A line is drawn at least about a pixel wide, and as
// much fainter as it is drawn wider than it is: thin lines far off keep their light without crawling, and where the
// cells come down to a pixel or two the lattice becomes a faint even glow of the shape.
float2 BuildLines(float3 c, float3 dc, float width) {
  float3 g = abs(frac(c) - 0.5);
  float3 drawn = max(width, dc * 0.55);
  float3 on = 1 - smoothstep(drawn - dc * 0.5, drawn + dc * 0.5, g);
  float3 light = on * saturate(width / drawn);
  return float2(max(on.x, max(on.y, on.z)), max(light.x, max(light.y, light.z)));
}

float RemasteredBuildSolid(int faction, float f, float3 shape, float3 rest, out float3 work) {
  work = 0;
  // (the derivatives first, before anything that differs from pixel to pixel: the faction is the draw's)
  float dy = fwidth(shape.y);
  if (faction == 1) {
    // Aeon: liquid metal that sets from the foot up over the last two fifths, a pale blue seam where it sets
    float level = (f - 0.6) / 0.38 * 1.1 - 0.05;
    float soft = max(0.012, dy);
    float set = smoothstep(-soft, soft, level - shape.y);
    float seam = saturate(1 - abs(level - shape.y) / (soft * 2.5)) * step(f, 0.985);
    work = float3(0.35, 0.65, 1.0) * (seam * seam * 4);
    return set;
  }
  if (faction == 3) {
    // Seraphim: dark and lit from within by slow flowing veins; the surface takes over through a dissolving front with
    // a bright rim over the second half
    float veinNoise = CloudNoise(rest * 1.6) * 0.65 + CloudNoise(rest * 4.1 + 7.3) * 0.35;
    float takeNoise = CloudNoise(rest * 2.3 + 3.7) * 0.7 + CloudNoise(rest * 7.9) * 0.3;
    float dv = fwidth(veinNoise), dt = fwidth(takeNoise);
    float g = abs(frac(veinNoise * 5 - time * 0.06) - 0.5);
    float vw = max(0.05, dv * 3);
    float veins = (1 - smoothstep(vw - dv * 2.5, vw + dv * 2.5, g)) * saturate(0.05 / vw);
    float pulse = 0.75 + 0.25 * sin(time * 1.7 + veinNoise * 9);
    float take = (f - 0.5) * 2.2 - 0.1;                    // (the front: below 0 none yet, past 1 all of it)
    float edgeWidth = max(0.04, dt * 2);
    float solid = smoothstep(takeNoise - edgeWidth * 0.5, takeNoise + edgeWidth * 0.5, take);
    float rim = saturate(1 - abs(take - takeNoise) / edgeWidth) * step(0.0, take) * step(f, 0.99);
    work = float3(0.45, 0.7, 1.0) * ((0.5 + veins * 3.0 * pulse) * (1 - solid) * (1.2 - f)) + float3(1.0, 0.92, 0.75) * (rim * rim * 4);
    return solid;
  }
  if (faction == 2) {
    // Cybran: plate by plate in no order; a new plate glows red, its edges white-hot, then red, then cool
    float3 cp = shape * 6, cc = shape * 7 + shape.yzx * 7;
    float3 dp = fwidth(cp), dcy = fwidth(cc);
    float since = f * 1.08 - CloudHash(floor(cp) + 3.1);  // (above 0: this plate is in)
    if (since > 0) {
      float3 cell = frac(cp);
      float3 e = min(cell, 1 - cell);
      float border = min(e.x, min(e.y, e.z));
      float bw = max(0.04, max(dp.x, max(dp.y, dp.z)) * 1.2);
      float rimmed = 1 - smoothstep(0, bw, border);
      float heat = saturate(1 - since / 0.05);
      float3 edgeHot = lerp(float3(1.0, 0.12, 0.04), float3(1.0, 0.8, 0.6), heat * heat);
      work = float3(1.0, 0.12, 0.04) * (heat * 0.9) + edgeHot * (heat * rimmed * 3.5);
      return 1;
    }
    // where plates are missing: the red lattice, the build bots' glints crawling along it
    float2 wire = BuildLines(cc, dcy, 0.03);
    float along = dot(cc, float3(1, 1, 1)) * 0.7;
    float lane = CloudHash(floor(cc) + 1.3);
    float glint = saturate(1 - abs(frac(along - time * (0.35 + 0.4 * lane) + lane) - 0.5) * 12);
    glint *= glint * glint;
    work = float3(1.0, 0.12, 0.05) * (wire.y * (1.1 + 0.35 * sin(time * 5 + shape.y * 30))) + float3(1.0, 0.6, 0.4) * (wire.y * glint * 3);
    return wire.x > 0.3 ? 0 : -1;
  }
  // the UEF (and whatever has no look of its own): from the top down as the game's box of light sinks; the level of
  // the work a white-blue weld seam, the fresh metal just over it still glowing and cooling
  float3 cu = shape * 10;
  float3 du = fwidth(cu);
  float level = f * 1.04 - 0.02, down = 1 - shape.y;
  if (down <= level) {
    float behind = level - down;  // (how far over the level of the work, in heights)
    float seamWidth = max(0.012, dy * 1.5);
    float seam = 1 - smoothstep(0, seamWidth, behind);
    float hot = exp(-behind / 0.014) * (1 - seam) * step(f, 0.985);
    work = float3(0.55, 0.8, 1.0) * (seam * 4.5) + float3(1.0, 0.38, 0.08) * (hot * 0.7);
    return 1;
  }
  float2 wire = BuildLines(cu, du, 0.035);
  float band = saturate(1 - abs(frac(shape.y * 0.5 - time * 0.25) - 0.5) * 6);
  // (a thin scan line running up it every two seconds or so)
  float phase = frac(shape.y * 0.7 - time * 0.45);
  float scan = 1 - smoothstep(0, max(0.004, dy * 1.0), min(phase, 1 - phase));
  work = float3(0.2, 0.55, 1.0) * (wire.y * (0.8 + 1.5 * band * band)) + float3(0.6, 0.85, 1.0) * (wire.y * scan * 2.5);
  return wire.x > 0.3 ? 0 : -1;
}

// The modern look's surface of what is under construction, by its faction (in place of the mesh shader's own lines).
void RemasteredBuildSurface(int faction, float solid, float3 rest, float edge, inout float3 N, inout float3 base, inout float metallic,
                            inout float roughness, inout float3 emission, inout float3 work) {
  if (faction == 1) {
    // Aeon: liquid metal, a pale gold, its surface flowing (the normal bent by a slow noise running up it), a faint warm
    // light of its own (molten)
    float3 q = rest * 1.5 + float3(0, -time * 0.5, 0);
    float3 bend = float3(CloudNoise(q), CloudNoise(q + 11.3), CloudNoise(q + 23.1)) - 0.5;
    float3 liquidN = normalize(N + bend * 0.9);
    N = normalize(lerp(liquidN, N, solid));
    const float3 gold = float3(1.0, 0.74, 0.33);
    base = lerp(gold, base, solid);
    metallic = lerp(1, metallic, solid);
    roughness = lerp(0.08, roughness, solid);
    // (its own light dimmed with the night as the game's glowing effects are: the eye opened for the night would see it
    // as a flat yellow glow)
    emission = lerp(gold * (0.06 * (falling.w > 0 ? falling.w : 1)), emission, solid);
  } else if (faction == 3) {
    // Seraphim: dark, lit from within, more toward the outline
    base *= lerp(0.25, 1, solid);
    emission = emission * solid + work * (0.5 + 2.5 * edge * edge);
    work = 0;
  }
  emission += work * 2;
}
)hlsl";

namespace {

// ---- the sparks: moved on the card ----
// The state: two textures of kSide x kSide, read one frame and written the next (place.xyz and age in seconds;
// velocity.xyz and kind x 16 + life in seconds). A spark is alive while its age is under its life.
// Thrown sparks come in the constants, four float4 each: where it starts (CPU: where the beam meets the box of what it
// builds) and its life; its velocity (a direction at random) and kind; the beam's start; the beam's end. On the card the
// beam is followed from its start toward its end over the scene's depth, and where it first goes just behind a surface
// near where it meets what it builds (the surface it meets as the camera sees it) the spark starts there instead, sent
// off that surface.
const char kStep[] = R"hlsl(
cbuffer Sparks : register(b1) {
  float4 stepInfo;   // x: seconds to move on; y: the first slot of the sparks thrown now; z: how many; w: 1 = the state read is empty
  float4 lens;       // x: tan(fov / 2); y: width / height; z: zNear; w: 1 = the scene's depth is there
  float4 axisRight, axisUp, axisBack;  // the camera's
  float4 ground;     // x, y: the map's width and height; z: the heights' unit; w: the water's level
  float4 spawns[4 * 512];
};
Texture2D<float4> placeIn : register(t0);
Texture2D<float4> motionIn : register(t1);
Texture2D<float> heights : register(t2);
#ifdef MSAA
Texture2DMS<float> sceneDepth : register(t3);
float DepthAt(int2 p) { return sceneDepth.Load(p, 0); }
#else
Texture2D<float> sceneDepth : register(t3);
float DepthAt(int2 p) { return sceneDepth.Load(int3(p, 0)); }
#endif
RWTexture2D<float4> placeOut : register(u0);
RWTexture2D<float4> motionOut : register(u1);

// by kind (0 UEF, 1 Aeon, 2 Cybran, 3 Seraphim): x the pull downward (world units / s^2; below 0 it rises), y how much
// of its speed into a surface it keeps bouncing off, z how much of its speed along it, w how quickly the air slows it
static const float4 kPhysics[4] = {
  float4(9.0, 0.45, 0.75, 0.5),
  float4(7.0, 0.12, 0.45, 0.35),
  float4(9.0, 0.40, 0.70, 0.75),
  float4(-0.45, 0.25, 0.8, 1.6)};

float GroundAt(float2 xz) {
  float2 p = clamp(xz, 0, ground.xy);
  float2 f = floor(p);
  float2 t = p - f;
  int2 c = int2(f);
  int2 most = int2(ground.xy);
  float a = heights.Load(int3(min(c, most), 0)), b = heights.Load(int3(min(c + int2(1, 0), most), 0));
  float d = heights.Load(int3(min(c + int2(0, 1), most), 0)), e = heights.Load(int3(min(c + int2(1, 1), most), 0));
  return lerp(lerp(a, b, t.x), lerp(d, e, t.x), t.y) * ground.z;
}
float3 GroundNormal(float2 xz) {
  float hx = GroundAt(xz + float2(0.5, 0)) - GroundAt(xz - float2(0.5, 0));
  float hz = GroundAt(xz + float2(0, 0.5)) - GroundAt(xz - float2(0, 0.5));
  return normalize(float3(-hx, 1, -hz));
}
// where a place is on the screen (false: behind the eye or off the picture), and how far along the view it is
bool OnScreen(float3 p, out int2 pixel, out float away) {
  float4 c = mul(viewProj, float4(p, 1));
  pixel = int2(0, 0);
  away = c.w;
  if (c.w <= lens.z) return false;
  float2 ndc = c.xy / c.w;
  if (any(abs(ndc) >= 0.999)) return false;
  pixel = int2((ndc * float2(0.5, -0.5) + 0.5) * viewport.xy);
  return true;
}
float SceneAway(int2 p) {
  float d = DepthAt(clamp(p, int2(0, 0), int2(viewport.xy) - 1));
  return d > 0 ? lens.z / d : 1e9;
}
float3 SceneAt(int2 p) {
  float away = SceneAway(p);
  float2 ndc = float2((p.x + 0.5) * viewport.z * 2 - 1, 1 - (p.y + 0.5) * viewport.w * 2);
  return cameraPosition + (-axisBack.xyz + axisRight.xyz * (ndc.x * lens.x * lens.y) + axisUp.xyz * (ndc.y * lens.x)) * away;
}
// the scene's surface's way at a pixel, from its neighbours (whichever on each axis is nearer in depth), toward the eye
float3 SceneNormal(int2 p) {
  float3 c = SceneAt(p), l = SceneAt(p - int2(1, 0)), r = SceneAt(p + int2(1, 0)), u = SceneAt(p - int2(0, 1)), d = SceneAt(p + int2(0, 1));
  float3 dx = length(l - c) < length(r - c) ? c - l : r - c;
  float3 dy = length(u - c) < length(d - c) ? c - u : d - c;
  float3 n = cross(dx, dy);
  n = dot(n, n) > 1e-12 ? normalize(n) : float3(0, 1, 0);
  return dot(n, cameraPosition - c) < 0 ? -n : n;
}
float3 Bounce(float3 v, float3 n, float4 physics) {
  float into = dot(v, n);
  if (into >= 0) return v;
  float3 along = v - n * into;
  return along * physics.z - n * (into * physics.y);
}

[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  int2 at = int2(id.xy);
  uint slot = id.y * 64 + id.x;
  float4 place = stepInfo.w > 0.5 ? float4(0, 0, 0, 1) : placeIn.Load(int3(at, 0));
  float4 motion = stepInfo.w > 0.5 ? float4(0, 0, 0, 0) : motionIn.Load(int3(at, 0));
  uint k = (slot + 4096 - uint(stepInfo.y)) % 4096;
  if (k < uint(stepInfo.z)) {
    // thrown now
    float4 a = spawns[k * 4], b = spawns[k * 4 + 1], c = spawns[k * 4 + 2], e = spawns[k * 4 + 3];
    float3 from = c.xyz, p = a.xyz, v = b.xyz, end = e.xyz;
    float3 back = normalize(from - p + float3(0, 1e-3, 0));
    float3 off = back;
    [branch] if (lens.w > 0.5 && length(end - from) > 0.2) {
      // the beam followed toward its end: where it first goes just behind the scene's surface, it meets it there (taken
      // only near what it builds: a long low beam passes behind other things)
      for (int s = 0; s <= 24; ++s) {
        float3 q = lerp(from, end, 0.3 + 0.7 * s / 24.0);
        int2 px;
        float away;
        if (!OnScreen(q, px, away)) continue;
        float there = SceneAway(px);
        if (away > there + 0.01 && away - there < 0.6) {
          float3 at = SceneAt(px);
          if (min(length(at - p), length(at - end)) < 1.5) {
            float3 n = SceneNormal(px);
            p = at + n * 0.03;
            off = n;
          }
          break;
        }
      }
    }
    float speed = length(v);
    float3 dir = speed > 1e-4 ? v / speed : float3(0, 1, 0);
    // (sent off the surface, a little back toward the builder and up, spread round that by its own direction)
    float3 way = normalize(off * 0.9 + back * 0.3 + float3(0, 0.5, 0) + dir * 0.5);
    place = float4(p, 0);
    motion = float4(way * speed, b.w * 16 + a.w);
  } else {
    float kind = floor(motion.w / 16), life = motion.w - kind * 16;
    if (place.w < life && stepInfo.x > 0) {
      float4 physics = kPhysics[uint(clamp(kind, 0, 3))];
      float dt = stepInfo.x;
      float3 v = motion.xyz;
      v.y -= physics.x * dt;
      v *= exp(-physics.w * dt);
      float3 q = place.xyz + v * dt;
      if (q.y < ground.w) {
        place.w = life;  // (into the water: out)
      } else {
        float floorAt = GroundAt(q.xz);
        int2 px;
        float away;
        if (q.y < floorAt) {
          float3 n = GroundNormal(q.xz);
          q.y = floorAt + 0.005;
          v = Bounce(v, n, physics);
        } else if (lens.w > 0.5 && OnScreen(q, px, away)) {
          // just behind what the scene shows there (within its thickness): it met that surface
          float there = SceneAway(px);
          float thick = 0.2 + length(v) * dt * 1.5;
          if (away > there + 0.005 && away - there < thick) {
            float3 n = SceneNormal(px);
            if (dot(v, n) < 0) {
              q = SceneAt(px) + n * 0.02;
              v = Bounce(v, n, physics);
            }
          }
        }
        // (one that has come to rest lies still while it cools)
        if (dot(v, v) < 0.01 && physics.x > 0) v = 0;
        place = float4(q, place.w + dt);
        motion.xyz = v;
      }
    }
  }
  placeOut[at] = place;
  motionOut[at] = motion;
}
)hlsl";

// Each spark a short streak from where it was a moment ago to where it is, facing the camera, never under about a pixel
// wide or a pixel and a half long; its colour by its kind and its age (cooling); behind what is solid it does not show.
const char kDraw[] = R"hlsl(
Texture2D<float4> places : register(t0);
Texture2D<float4> motions : register(t1);
#ifdef MSAA
Texture2DMS<float> sceneDepth : register(t3);
float DepthAt(int2 p) { return sceneDepth.Load(p, 0); }
#else
Texture2D<float> sceneDepth : register(t3);
float DepthAt(int2 p) { return sceneDepth.Load(int3(p, 0)); }
#endif
cbuffer Look : register(b1) {
  float4 look2;  // x: zNear; y: what the sparks' light is multiplied by; z: tan(fov / 2)
};

struct VOut {
  float4 position : SV_Position;
  float3 color : COLOR;
  noperspective float4 local : TEXCOORD0;  // x: pixels along the streak from its tail, y: across it; z: its length, w: its half width (pixels)
  float away : TEXCOORD1;
  float3 world : TEXCOORD2;
};

// its light by kind and by how far through its life it is (0 new .. 1 gone), linear
float3 SparkColor(uint kind, float t, float seed, float age) {
  if (kind == 1) {  // Aeon: a drop of liquid gold, glinting as it turns
    float glint = 0.6 + 0.4 * saturate(sin(age * 23 + seed * 40));
    return float3(1.0, 0.76, 0.38) * (3.0 * pow(1 - t, 1.5) * glint);
  }
  if (kind == 3) {  // Seraphim: a mote of light, coming and going
    float bell = saturate(4 * t * (1 - t));
    return lerp(float3(0.75, 0.88, 1.0), float3(0.95, 0.8, 1.0), t) * (3.0 * bell);
  }
  // sparks: from hot to cool (the Cybran's start red-white, the UEF's blue-white like the arc)
  float3 white = kind == 2 ? float3(1.0, 0.8, 0.7) : float3(0.8, 0.9, 1.0);
  float3 c = t < 0.08 ? lerp(white * 14, float3(1.0, 0.85, 0.55) * 9, t / 0.08)
           : t < 0.35 ? lerp(float3(1.0, 0.85, 0.55) * 9, float3(1.0, 0.5, 0.15) * 4, (t - 0.08) / 0.27)
           : t < 0.75 ? lerp(float3(1.0, 0.5, 0.15) * 4, float3(0.8, 0.15, 0.03) * 1.2, (t - 0.35) / 0.4)
                      : lerp(float3(0.8, 0.15, 0.03) * 1.2, float3(0.3, 0.03, 0.01) * 0.2, (t - 0.75) / 0.25);
  if (kind == 2) c *= float3(1.0, 0.75, 0.7);
  return c;
}

VOut VS(uint id : SV_VertexID) {
  VOut o;
  o.position = float4(-2, -2, 0, 1);  // (not alive: off the picture)
  o.color = 0;
  o.local = 0;
  o.away = 0;
  o.world = 0;
  uint spark = id / 6, corner = id % 6;
  int2 at = int2(spark % 64, spark / 64);
  float4 place = places.Load(int3(at, 0)), motion = motions.Load(int3(at, 0));
  float kind = floor(motion.w / 16), life = motion.w - kind * 16;
  if (!(place.w < life) || life <= 0) return o;
  uint k = uint(clamp(kind, 0, 3));
  float t = saturate(place.w / life);
  // its streak: where it was a moment ago to where it is (the UEF's longest, the Seraphim's a dot)
  static const float kStreak[4] = {0.022, 0.01, 0.02, 0.0};
  static const float kHalf[4] = {0.006, 0.012, 0.005, 0.011};
  float3 head = place.xyz, tail = place.xyz - motion.xyz * kStreak[k];
  float4 a = mul(viewProj, float4(tail, 1)), b = mul(viewProj, float4(head, 1));
  if (a.w <= look2.x || b.w <= look2.x) return o;
  float2 half = viewport.xy * 0.5;
  float2 sa = a.xy / a.w * half, sb = b.xy / b.w * half;  // (pixels from the middle, y up)
  float2 d = sb - sa;
  float len = length(d);
  float2 dir = len > 1e-3 ? d / len : float2(1, 0);
  // (as wide as it is, in pixels at its distance, but never under 0.6 of one; a dot drawn round)
  float pixelsPerUnit = viewport.y / (2 * b.w * look2.z);
  float halfPixels = max(kHalf[k] * pixelsPerUnit, 0.6);
  if (len < 1.5) sa = sb - dir * 1.5;
  float2 side = float2(-dir.y, dir.x) * halfPixels;
  float2 cap = dir * halfPixels;
  // two triangles: tail -, tail +, head +; tail -, head +, head -
  static const float2 kCorner[6] = {float2(-1, 0), float2(1, 0), float2(1, 1), float2(-1, 0), float2(1, 1), float2(-1, 1)};
  float2 c = kCorner[corner];
  float2 s = (c.y > 0.5 ? sb + cap : sa - cap) + side * c.x;
  float w = c.y > 0.5 ? b.w : a.w;
  o.position = float4(s / half * w, 0.5 * w, w);
  o.local = float4(c.y > 0.5 ? length(sb - sa) + halfPixels : -halfPixels, c.x * halfPixels, length(sb - sa), halfPixels);
  o.away = c.y > 0.5 ? b.w : a.w;
  o.world = c.y > 0.5 ? head : tail;
  float seed = frac(float(spark) * 0.618034);
  float dim = falling.w > 0 ? falling.w : 1;  // (the night's dimming of the game's glowing effects)
  o.color = SparkColor(k, t, seed, place.w) * (look2.y * dim);
  // (smaller on the screen than a pixel's width: its light spread over the pixel it is drawn on)
  o.color *= saturate(kHalf[k] * pixelsPerUnit / 0.6);
  return o;
}

float4 PS(VOut i) : SV_Target {
  // behind what is solid there: not seen
  float d = DepthAt(int2(i.position.xy));
  float there = d > 0 ? look2.x / d : 1e9;
  if (i.away > there + 0.05) discard;
  // (a capsule round the streak: round at its ends, a dot when it does not move; brightest at its head)
  float along = clamp(i.local.x, 0, i.local.z);
  float r = length(float2(i.local.x - along, i.local.y)) / max(i.local.w, 1e-3);
  if (r >= 1) discard;
  float t = along / max(i.local.z, 1e-3);
  float3 c = i.color * ((1 - r * r) * (0.25 + 0.75 * t * t));
  // the air between it and the eye (as the beams take it)
  [branch] if (air.x > 0.5) {
    c *= AirAt(i.world).a;
  } else if (weather.z > 0) {
    c *= 1 - min(1 - exp(-length(i.world - cameraPosition) * weather.z), weather.w);
  }
  if (mist.x > 0 && air.x < 0.5) c *= 1 - MistLost(i.world);
  return float4(c, 1);
}
)hlsl";

struct StepConstants {
  float step[4];
  float lens[4];
  float axisRight[4], axisUp[4], axisBack[4];
  float ground[4];
  float spawns[4 * BuildEffects::kMostNew][4];
};

int FactionNumber(const std::string& f) {
  return f == "uef" ? 0 : f == "aeon" ? 1 : f == "cybran" ? 2 : f == "seraphim" ? 3 : -1;
}

// Where a ray from `from` along `dir` (unit length) enters a box (its middle, half sizes, turn): the distance, or
// -1 when it does not meet it (or starts inside it).
float EnterBox(Vec3 from, Vec3 dir, Vec3 centre, Vec3 half, const Quat& turn) {
  const Quat back = Conjugate(turn);
  const Vec3 o = Rotate(back, from - centre), d = Rotate(back, dir);
  float t0 = -1e30f, t1 = 1e30f;
  const float os[3] = {o.x, o.y, o.z}, ds[3] = {d.x, d.y, d.z}, hs[3] = {half.x, half.y, half.z};
  for (int a = 0; a < 3; ++a) {
    if (std::fabs(ds[a]) < 1e-6f) {
      if (std::fabs(os[a]) > hs[a]) return -1;
      continue;
    }
    float n = (-hs[a] - os[a]) / ds[a], f = (hs[a] - os[a]) / ds[a];
    if (n > f) std::swap(n, f);
    t0 = std::max(t0, n);
    t1 = std::min(t1, f);
  }
  if (t0 > t1 || t1 < 0 || t0 < 0) return -1;
  return t0;
}

}  // namespace

BuildEffects::~BuildEffects() {
  for (gfx::Texture t : {place_[0], place_[1], motion_[0], motion_[1]})
    if (t) device_->Destroy(t);
  for (gfx::Pipeline p : {step_, draw_})
    if (p) device_->Destroy(p);
}

float BuildEffects::Random() {
  random_ = random_ * 1664525u + 1013904223u;
  return static_cast<float>(random_ >> 8) * (1.0f / 16777216.0f);
}

bool BuildEffects::IsBuildBeam(const std::string& blueprint) {
  std::string b = blueprint;
  for (char& c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return b.find("build_beam") != std::string::npos;
}

void BuildEffects::Begin(double clock, bool on, bool side) {
  if (side) {
    on_ = false;
    return;
  }
  on_ = on;
  clock_ = clock;
  beams_.clear();
  sites_.clear();
  beams = lit = thrown = 0;
  spawnCount_ = 0;
  if (!on) {
    sources_.clear();
    fresh_ = true;
    drawable_ = false;
    aliveUntil_ = -1;
    lastClock_ = lastStep_ = -1;
  }
}

void BuildEffects::AddBeam(Vec3 start, Vec3 end, const std::string& faction, Vec3 glow, uint64_t key) {
  if (!on_ || beams_.size() >= 1024) return;
  beams_.push_back({start, end, glow, FactionNumber(faction), key});
}

void BuildEffects::TakeSites(const Frame& frame, ModelCache* models) {
  if (!on_ || !frame.blueprints) return;
  for (const FrameUnit& u : frame.units) {
    if (u.dead || u.fractionComplete >= 1 || !u.Seen() || u.blueprint >= frame.blueprints->size()) continue;
    const std::string& id = (*frame.blueprints)[u.blueprint];
    if (id.empty()) continue;
    const Model* model = models->Unit(id);
    if (!model || !model->ok) continue;
    Site s;
    s.turn = Quat{u.orientation[0], u.orientation[1], u.orientation[2], u.orientation[3]};
    s.faction = FactionNumber(UnitFaction(id));
    // (as the mesh shader draws it while it is built: the Seraphim's grows from a quarter of its size, the Aeon's from
    // three quarters and rises out of the ground)
    const float f = std::clamp(u.fractionComplete, 0.0f, 1.0f);
    const float grown = s.faction == 3 ? 0.25f + 0.75f * f : s.faction == 1 ? std::max(0.75f, f) : 1.0f;
    const float sunk = s.faction == 1 ? 0.8f * (1 - std::min(1.0f, 1.25f * f)) * (model->boundsMax.y - model->boundsMin.y) : 0.0f;
    const Vec3 middle = (model->boundsMin + model->boundsMax) * (0.5f * grown) - Vec3{0, sunk, 0};
    s.centre = Vec3{u.position[0], u.position[1], u.position[2]} + Rotate(s.turn, middle * model->scale);
    s.half = (model->boundsMax - model->boundsMin) * (0.5f * model->scale * grown);
    s.fraction = u.fractionComplete;
    sites_.push_back(s);
    if (sites_.size() >= 256) break;
  }
}

const BuildEffects::Site* BuildEffects::SiteOf(const Beam& b) const {
  // the unit under construction whose box the beam's end is in or nearest (within two units of it)
  const Site* best = nullptr;
  float bestAway = 2.0f;
  for (const Site& s : sites_) {
    const Vec3 local = Rotate(Conjugate(s.turn), b.end - s.centre);
    const Vec3 out{std::max(std::fabs(local.x) - s.half.x, 0.0f), std::max(std::fabs(local.y) - s.half.y, 0.0f), std::max(std::fabs(local.z) - s.half.z, 0.0f)};
    const float away = Length(out);
    if (away < bestAway) {
      bestAway = away;
      best = &s;
    }
  }
  return best;
}

void BuildEffects::AddLights(Lights* lights, float night) {
  if (!on_) return;
  beams = static_cast<int>(beams_.size());
  const double dt = lastClock_ < 0 ? 0.0 : std::clamp(clock_ - lastClock_, 0.0, 0.1);
  lastClock_ = clock_;
  // the beam ends: beams of one builder that end together are one (an Aeon's three, a commander's two)
  struct End {
    Vec3 contact, start, end, glow;
    int faction;
    uint64_t key;
    float length;
    const Site* site;
  };
  std::vector<End> ends;
  for (const Beam& b : beams_) {
    const Vec3 along = b.end - b.start;
    const float length = Length(along);
    if (length < 0.05f) continue;
    auto same = std::find_if(ends.begin(), ends.end(), [&](const End& e) { return Length(e.start - b.start) < 0.3f && Length(e.contact - b.end) < 0.6f; });
    const Site* site = SiteOf(b);
    if (same != ends.end()) {
      if (b.glow.x + b.glow.y + b.glow.z > same->glow.x + same->glow.y + same->glow.z) same->glow = b.glow;
      continue;
    }
    End e{b.end, b.start, b.end, b.glow, b.faction, b.key, length, site};
    if (e.faction < 0) e.faction = site ? site->faction : 0;
    if (e.faction < 0) e.faction = 0;
    // where the beam enters the box of what it builds (the game's end often lies inside it, or at its foot)
    if (site) {
      const Vec3 dir = along * (1 / length);
      const float t = EnterBox(b.start, dir, site->centre, site->half, site->turn);
      if (t >= 0 && t < length + 1.0f) e.contact = b.start + dir * t;
    }
    ends.push_back(e);
  }
  // each faction's light where nothing glows in the beam's blueprint (an Aeon's darkening beams)
  static const Vec3 kFactionLight[4] = {{0.45f, 0.62f, 1.0f}, {0.5f, 0.75f, 1.0f}, {1.0f, 0.28f, 0.12f}, {0.85f, 0.78f, 1.0f}};
  const float t = static_cast<float>(clock_);
  for (const End& e : ends) {
    const int f = std::clamp(e.faction, 0, 3);
    Vec3 hue = kFactionLight[f];
    const float most = std::max({e.glow.x, e.glow.y, e.glow.z});
    if (most > 0.02f) hue = Lerp(e.glow * (1 / most), Vec3{1, 1, 1}, 0.35f);  // (the beam's own colour, toward white: the hot spot)
    const float phase = static_cast<float>(e.key % 977) * 0.37f;
    // (an arc's quick flicker for the UEF and the Cybrans; a slow breath for the Aeon and the Seraphim)
    const float flicker = f == 1 || f == 3 ? 0.85f + 0.15f * std::sin(t * 4.0f + phase)
                                           : 0.72f + 0.16f * std::sin(t * 31.0f + phase) + 0.12f * std::sin(t * 47.3f + phase * 2.1f);
    const Vec3 back = Normalize(e.start - e.contact);
    const float size = e.site ? std::max({e.site->half.x, e.site->half.y, e.site->half.z}) : 1.0f;
    // (a little larger on what is large, but a spot, not a floodlight: by day a factory's beam lit half the grass round it)
    const float radius = std::clamp(2.0f + 0.2f * size, 2.0f, 4.0f);
    lights->Add(e.contact + back * 0.35f + Vec3{0, 0.1f, 0}, radius, hue * (2.0f * flicker * lightStrength * night));
    // and a faint light half way along a long beam
    if (e.length > 2.5f) lights->Add(Lerp(e.start, e.contact, 0.5f), 1.6f, hue * (0.22f * flicker * lightStrength * night));
    ++lit;
    // its sparks: a steady stream and, for the UEF's and the Cybran's, bursts now and then
    Source& s = sources_[e.key];
    s.seen = clock_;
    static const float kRate[4] = {26, 9, 38, 14};
    s.carry += static_cast<float>(dt) * kRate[f] * sparkRate;
    if ((f == 0 || f == 2) && clock_ >= s.burstAt) {
      if (s.burstAt > 0) s.carry += (6 + 10 * Random()) * sparkRate;
      s.burstAt = clock_ + 0.25 + 0.6 * Random();
    }
    while (s.carry >= 1 && spawnCount_ < kMostNew) {
      s.carry -= 1;
      static const float kSpeed[4][2] = {{1.0f, 3.0f}, {0.5f, 1.4f}, {0.9f, 2.6f}, {0.2f, 0.6f}};
      static const float kLife[4][2] = {{0.35f, 0.85f}, {0.7f, 1.3f}, {0.25f, 0.6f}, {1.0f, 1.8f}};
      // a direction at random (the card sends it off the surface it meets, round this)
      Vec3 dir{Random() * 2 - 1, Random() * 2 - 1, Random() * 2 - 1};
      if (Dot(dir, dir) < 1e-4f) dir = {0, 1, 0};
      dir = Normalize(dir);
      // (what builds something large throws them farther)
      const float speed = (kSpeed[f][0] + (kSpeed[f][1] - kSpeed[f][0]) * Random()) * std::clamp(std::sqrt(size / 0.8f), 0.75f, 1.8f);
      const float life = kLife[f][0] + (kLife[f][1] - kLife[f][0]) * Random();
      const Vec3 v = dir * speed;
      const float spawn[16] = {e.contact.x, e.contact.y, e.contact.z, life, v.x, v.y, v.z, static_cast<float>(f),
                               e.start.x,   e.start.y,   e.start.z,   1,    e.end.x, e.end.y, e.end.z, 0};
      spawns_.resize(static_cast<size_t>(spawnCount_ + 1) * 16);
      std::memcpy(&spawns_[static_cast<size_t>(spawnCount_) * 16], spawn, sizeof spawn);
      ++spawnCount_;
      aliveUntil_ = std::max(aliveUntil_, clock_ + static_cast<double>(life) + 0.1);
    }
    if (s.carry > 4) s.carry = 4;  // (a frame without room: not all owed later)
  }
  thrown = spawnCount_;
  // the sources not seen for a second are let go
  for (auto it = sources_.begin(); it != sources_.end();) it = clock_ - it->second.seen > 1.0 ? sources_.erase(it) : std::next(it);
}

bool BuildEffects::Ready() {
  if (tried_) return tried_ > 0;
  tried_ = -1;
  std::string error;
  const std::string msaa = gSceneSamples > 1 ? "#define MSAA 1\n" : "";
  const std::string step = msaa + kViewShaderBlock + kStep;
  gfx::ComputeDesc cd;
  cd.name = "build sparks: moved";
  cd.source = step;
  step_ = device_->CreateCompute(cd, &error);
  if (step_) {
    const std::string draw = msaa + kViewShaderBlock + kDraw;
    gfx::PipelineDesc pd;
    pd.name = "build sparks";
    pd.vertexSource = pd.pixelSource = draw;
    pd.cull = gfx::Cull::None;
    pd.depthTest = pd.depthWrite = false;  // (the shader tests the scene's depth itself)
    pd.depthTarget = false;
    pd.colorFormat = kSceneFormat;
    pd.samples = gSceneSamples;
    pd.blend = gfx::Blend::Additive;
    pd.alphaWrite = false;
    draw_ = device_->CreatePipeline(pd, &error);
  }
  if (draw_) {
    gfx::TargetDesc td;
    td.storage = true;
    td.width = td.height = kSide;
    td.format = gfx::Format::RGBA32F;
    for (int k = 0; k < 2; ++k) {
      place_[k] = device_->CreateTarget(td);
      motion_[k] = device_->CreateTarget(td);
    }
    if (!place_[0] || !place_[1] || !motion_[0] || !motion_[1]) error = "its storage targets cannot be made";
    else tried_ = 1;
  }
  if (tried_ < 0) fprintf(stderr, "render: the remastered build look's sparks cannot be drawn (%s)\n", error.c_str());
  return tried_ > 0;
}

void BuildEffects::Simulate(const View& view, const Camera& camera, gfx::Texture sceneDepth, gfx::Texture heights, float heightUnit, float mapWidth,
                            float mapHeight, float water) {
  if (!Busy() || !Ready()) {
    spawnCount_ = 0;
    return;
  }
  StepConstants c{};
  const double dt = std::clamp(clock_ - (lastStep_ < 0 ? clock_ : lastStep_), 0.0, 0.05);
  lastStep_ = clock_;
  c.step[0] = static_cast<float>(dt);
  c.step[1] = static_cast<float>(head_);
  c.step[2] = static_cast<float>(spawnCount_);
  c.step[3] = fresh_ ? 1.0f : 0.0f;
  c.lens[0] = std::tan(camera.fovY * 0.5f);
  c.lens[1] = static_cast<float>(view.width) / static_cast<float>(std::max(view.height, 1));
  c.lens[2] = camera.zNear;
  c.lens[3] = sceneDepth ? 1.0f : 0.0f;
  Vec3 r, u, b;
  camera.Axes(&r, &u, &b);
  const float axes[3][4] = {{r.x, r.y, r.z, 0}, {u.x, u.y, u.z, 0}, {b.x, b.y, b.z, 0}};
  std::memcpy(c.axisRight, axes[0], sizeof c.axisRight);
  std::memcpy(c.axisUp, axes[1], sizeof c.axisUp);
  std::memcpy(c.axisBack, axes[2], sizeof c.axisBack);
  c.ground[0] = mapWidth;
  c.ground[1] = mapHeight;
  c.ground[2] = heights ? heightUnit : 0.0f;
  c.ground[3] = water;
  if (spawnCount_ > 0) std::memcpy(c.spawns, spawns_.data(), static_cast<size_t>(spawnCount_) * 16 * sizeof(float));
  const int read = current_, write = 1 - current_;
  device_->SetPipeline(step_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  // (only as much of the thrown sparks as there are: the shader reads no further)
  device_->SetConstants(1, &c, static_cast<uint32_t>(offsetof(StepConstants, spawns) + static_cast<size_t>(std::max(spawnCount_, 1)) * 4 * 16));
  device_->SetTexture(0, place_[read]);
  device_->SetTexture(1, motion_[read]);
  device_->SetTexture(2, heights);
  device_->SetTexture(3, sceneDepth);
  device_->SetStorage(0, place_[write]);
  device_->SetStorage(1, motion_[write]);
  device_->Dispatch(kSide / 8, kSide / 8, 1);
  device_->SetStorage(0, {});
  device_->SetStorage(1, {});
  device_->SetTexture(0, {});
  device_->SetTexture(1, {});
  device_->SetTexture(2, {});
  device_->SetTexture(3, {});
  current_ = write;
  head_ = (head_ + spawnCount_) % kCapacity;
  spawnCount_ = 0;
  fresh_ = false;
  drawable_ = true;
}

void BuildEffects::Draw(const View& view, gfx::Texture sceneDepth, const Camera& camera) {
  if (!on_ || !drawable_ || tried_ <= 0 || clock_ >= aliveUntil_) return;
  device_->SetPipeline(draw_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  const float look[4] = {camera.zNear, 1, std::tan(camera.fovY * 0.5f), 0};
  device_->SetConstants(1, look, sizeof look);
  device_->SetTexture(0, place_[current_]);
  device_->SetTexture(1, motion_[current_]);
  device_->SetTexture(3, sceneDepth);
  device_->Draw(6 * kCapacity);
  device_->SetTexture(0, {});
  device_->SetTexture(1, {});
  device_->SetTexture(3, {});
}

}  // namespace render
