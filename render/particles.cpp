#include "particles.h"

#include "effect_files.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "bp_reader.h"
#include "core/vfs.h"
#include "dds.h"
#include "lighting.h"

namespace render {
namespace {

constexpr float kTick = 0.1f;  // seconds: the emitter blueprints count time in simulation ticks
constexpr float kPuffSeconds = 1.5f;  // how long a fresh explosion's hot air bends what is behind it (heat shimmer)

// A particle's place, size and turn from its start values and its age; its colour from its texture
// (one frame of one strip) times its ramp read along its age. Written after the game's
// effects/particle.fx (WorldVS, WorldVSAlign, WorldPS), with times in seconds.
const char kShader[] = R"hlsl(
cbuffer ParticleSet : register(b1) {
  float4 cameraRight;   // w: the time now, seconds
  float4 cameraUp;      // w: how it lies - 0 faces the camera, 1 flat on the ground, 2 stretched along its
                        // flight, 3 stretched along its start direction without moving (aligned to a bone)
  float4 options;       // x: first row of the set's particles, y: 1 = the texture has frames or strips,
                        // z: how the colour leaves (0 alpha, 1 darkening, 2 adding, 3 premultiplied,
                        // 4: it has none of its own and bends what is behind it),
                        // w: brightness of what glows in the modern look
  float4 sheet;         // x: 1 / frames, y: 1 / strips, z: 1 = particles are slowed as by air,
                        // w: the camera's near distance (the scene's depth is that over the distance)
  float4 meeting;       // x: over how many world units a particle fades where it meets something solid
  float4 grid;          // x: 1 = frames run across the whole grid, y: 1 = and start again after the last,
                        // z: frames in the grid, w: 1 = each frame blends into the next
  float4 extra;         // x: 1 = lit through a normal map, y: how far it bends what is behind (x the
                        // original's), z: 1 = it stops on the ground (the row's last number: its height)
#if LIT_SMOKE
  float4 lit;           // lit smoke (World::litSmoke): x 1 = this set is lit as a volume; y how dense it is to
                        // the light going through it; z how strongly the small lights light it; w (the smoke's
                        // shadow pass) what its optical depth is multiplied by
  float4 six;           // x 1 = six-way light maps (SixWayTexture), y how bright what they say glows is
#endif
};
StructuredBuffer<float4> particleRows : register(t0, space1);
Texture2D particleTexture : register(t0);
Texture2D rampTexture : register(t1);
Texture2D behindTexture : register(t2);  // (bending particles only)
Texture2D particleNormals : register(t4);  // (lit particles only)
#if LIT_SMOKE
Texture2D sixWayTexture : register(t5);  // (six-way light maps' second texture: left, bottom, front; emission)
#endif
#ifdef MSAA
Texture2DMS<float> sceneDepth : register(t3);
#else
Texture2D<float> sceneDepth : register(t3);
#endif

struct VOut {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
  float2 rampUv : TEXCOORD1;
  float3 next : TEXCOORD3;  // the next frame's place in the texture, and how far toward it (blended frames)
  float3 across : TEXCOORD4;  // the texture's right and down in the world (lit particles)
  float3 down : TEXCOORD5;
  float3 world : TEXCOORD2;
#if LIT_SMOKE
  float4 light : TEXCOORD6;  // lit smoke: rgb the small lights' light where it is, a what of the sun's comes through
#endif
};

#if LIT_SMOKE
// The small lights at a place in smoke (lights.h; as lighting.h's SmallLights, a lamp's beam through its shadow):
// smoke sends light on to all sides about evenly, so only how near each light is counts.
float3 SmokeSmallLights(float3 p) {
  float3 sum = 0;
  [branch] if (lightGrid.w > 0.5) {
    float2 g = (p.xz - lightGrid.xy) * lightGrid.z;
    [branch] if (all(g >= 0) && all(g < lightGridSize.xy)) {
      float4 square = lightData[uint(g.y) * uint(lightGridSize.x) + uint(g.x)];
      uint first = uint(square.x), count = min(uint(square.y), 48u);
      for (uint s = 0; s < count; ++s) {
        uint at = first + s;
        uint which = uint(lightData[at >> 2][at & 3]);
        float4 where = lightData[uint(lightGridSize.z) + which * 4];
        float3 toLight = where.xyz - p;
        float far2 = dot(toLight, toLight) / (where.w * where.w);
        if (far2 >= 1) continue;
        float fade = (1 - far2) * (1 - far2) / (1 + 4 * far2);
        float4 colour = lightData[uint(lightGridSize.z) + which * 4 + 1];
        float4 beam = lightData[uint(lightGridSize.z) + which * 4 + 2];
        if (beam.w > -1.5) {  // a beam: within its cone, through its lamp's shadow (and a share round the lamp)
          float3 l = normalize(toLight);
          float off = sqrt(saturate((1 - dot(-l, beam.xyz)) / max(1 - beam.w, 1e-4)));
          float inside = 1 - smoothstep(0.7, 1.0, off);
          float map = lightData[uint(lightGridSize.z) + which * 4 + 3].x;
          float reaches = inside;
          [branch] if (map > -0.5 && inside > 0) reaches *= LampShadow(p, l, uint(map));
          fade = (1 - far2) * (1 - far2) / (1 + 1.5 * far2) * reaches + fade * colour.w * saturate(1 - far2 * 25);
        }
        // (in smoke, the smoke between the light and the place takes its share: a fire lights the smoke round it,
        // not a whole pall)
        sum += colour.rgb * (fade * exp(-0.2 * lit.y * sqrt(dot(toLight, toLight))));
      }
    }
  }
  return sum;
}
#endif

VOut VS(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
  uint at = (uint)options.x + instance * 5;
  float4 p0 = particleRows[at], p1 = particleRows[at + 1], p2 = particleRows[at + 2], p3 = particleRows[at + 3], p4 = particleRows[at + 4];
  float t = cameraRight.w - p2.w;
  float age = t / p3.x;
  VOut o;
  o.position = float4(0, 0, 0, 1);
  o.uv = 0;
  o.rampUv = 0;
  o.next = 0;
  o.across = float3(1, 0, 0);
  o.down = float3(0, 0, 1);
#if LIT_SMOKE
  o.light = float4(0, 0, 0, 1);
#endif
  if (t < 0 || age >= 1) return o;  // not thrown yet, or over: a quad of no size

  const float2 corners[6] = {float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1)};
  float2 corner = corners[vertex];
  // where it is and which way it flies: thrown, pulled by its acceleration, and (some) slowed as by air
  float3 position = p0.xyz, flight = p1.xyz;
  if (cameraUp.w < 2.5) {
    if (sheet.z > 0.5 && p4.z > 0) {
      float r = p4.z, fade = exp(-r * t);
      position += (p2.xyz / (r * r) - p1.xyz / r) * (fade - 1) + p2.xyz * (t / r);
      flight = (p1.xyz - p2.xyz / r) * fade + p2.xyz / r;
    } else {
      position += p1.xyz * t + 0.5 * p2.xyz * t * t;
      flight = p1.xyz + p2.xyz * t;
      if (extra.z > 0.5) {
        // stops where its fall meets the ground: p0.y + v t + a t^2 / 2 = ground
        float qa = 0.5 * p2.y, qb = p1.y, qc = p0.y - p4.w, hit = -1;
        if (abs(qa) < 1e-5) {
          if (qb < 0) hit = -qc / qb;
        } else {
          float disc = qb * qb - 4 * qa * qc;
          if (disc >= 0) {
            float s = sqrt(disc), r1 = (-qb - s) / (2 * qa), r2 = (-qb + s) / (2 * qa);
            hit = min(r1 > 0 ? r1 : 1e9, r2 > 0 ? r2 : 1e9);
          }
        }
        if (hit > 0 && hit < 1e8 && t > hit) {
          position = p0.xyz + p1.xyz * hit + 0.5 * p2.xyz * hit * hit;
          flight = p1.xyz + p2.xyz * hit;
        }
      }
    }
  }
#if LIT_SMOKE
  float3 middle = position;
#endif
  float size = p3.y + p3.z * t;
  if (cameraUp.w > 1.5) {
    // stretched along its flight: across by what is square to the flight and to the line of sight
    float3 along = normalize(flight + float3(0, 1e-5, 0));
    float3 across = normalize(cross(along, cameraPosition - position));
    position += (across * corner.x + along * corner.y) * size;
    o.across = across;
    o.down = along;
  } else {
    float s, c;
    sincos(p0.w + p1.w * t, s, c);
    float2 turned = float2(corner.x * c - corner.y * s, corner.x * s + corner.y * c);
    float3 r = cameraUp.w > 0.5 ? float3(1, 0, 0) : cameraRight.xyz, u = cameraUp.w > 0.5 ? float3(0, 0, 1) : cameraUp.xyz;
    position += (turned.x * r + turned.y * u) * size;
    o.across = c * r + s * u;
    o.down = -s * r + c * u;
  }
  o.position = mul(viewProj, float4(position, 1));
  o.world = position;
  o.uv = (corner + 1) * 0.5;
#if LIT_SMOKE
  [flatten] if (six.x > 0.5) {
    // six-way light maps are shown the way up they were rendered: the game's particles show a texture's first row
    // at the bottom (corner -1 is the texture's top, as in its particle.fx), which a bake from Blender would have
    // to turn over for; each frame is turned within its own cell, and `down` with it (the texture's down)
    o.uv.y = 1 - o.uv.y;
    o.down = -o.down;
  }
#endif
  if (options.y > 0.5) {
    float2 cell = o.uv;
    float passed = p3.w * t;  // frames passed
    if (grid.x > 0.5) {
      // across the whole grid, from the first frame of the particle's strip
      float frames = round(1 / sheet.x), total = grid.z;
      float n = round(p4.x / sheet.y) * frames + passed;
      float n0 = floor(n), n1 = n0 + 1;
      if (grid.y > 0.5) {
        n0 = fmod(n0, total);
        n1 = fmod(n1, total);
      } else {
        n0 = min(n0, total - 1);
        n1 = min(n1, total - 1);
      }
      o.uv = (cell + float2(fmod(n0, frames), floor(n0 / frames))) * sheet.xy;
      o.next = float3((cell + float2(fmod(n1, frames), floor(n1 / frames))) * sheet.xy, grid.w > 0.5 ? frac(n) : 0);
    } else {
      o.uv.x = (cell.x + floor(passed)) * sheet.x;
      o.uv.y = cell.y * sheet.y + p4.x;
      o.next = float3(o.uv.x + sheet.x, o.uv.y, grid.w > 0.5 ? frac(passed) : 0);
    }
  }
  o.rampUv = float2(age, p4.y);
#if LIT_SMOKE
  [branch] if (lit.x > 0.5) {
    // the light where this corner of the puff is (a little in from the corner, where the puff is): the sun's
    // through the shadow maps, the clouds' and the smoke's own shadow, and the small lights'
    float3 p = lerp(middle, o.world, 0.6);
    float sun = SunShadowMaps(p, float3(0, 0, 0)) * CloudShadow(p);
    [branch] if (smokeShadow.w > 0.5) {
      // (the plume's own shadow, as light scattered on and on inside smoke has it: some reaches deep; looked up on the
      // puff's sunny side, so that it is the other smoke's shadow: its own thickness toward the sun the pixel shader
      // reads from its texture)
      float s = SmokeShadowAt(p + sunDirection * (size * 0.6), 0.6);
      sun *= (s + 0.6 * pow(s, 0.25)) / 1.6;
    }
    o.light = float4(SmokeSmallLights(p), sun);
  }
#endif
  return o;
}

// How much of the particle shows at this pixel: of the pixel's samples, those where nothing solid is
// in front of it (`coverage`); and less where it is about to meet something, so that it fades into
// the ground and into units instead of being cut off along a hard line.
float Meets(VOut i, out uint coverage) {
  coverage = 0;
  float nearest = 1e9;
#ifdef MSAA
  int2 pixel = int2(i.position.xy);
  for (uint s = 0; s < SAMPLES; ++s) {
    float d = sceneDepth.Load(pixel, s);
    float gap = (d > 0 ? sheet.w / d : 1e9) - i.position.w;
    if (gap > 0) {
      coverage |= 1u << s;
      nearest = min(nearest, gap);
    }
  }
#else
  float d = sceneDepth.Load(int3(i.position.xy, 0));
  float gap = (d > 0 ? sheet.w / d : 1e9) - i.position.w;
  if (gap > 0) {
    coverage = 1;
    nearest = gap;
  }
#endif
  return coverage == 0 ? 0 : saturate(nearest / max(meeting.x, 1e-3));
}

// The particle's texture: its frame, blended into the next one where frames blend.
float4 Frame(VOut i) {
  float4 c = particleTexture.Sample(samplerLinear, i.uv);
  if (i.next.z > 0) c = lerp(c, particleTexture.Sample(samplerLinear, i.next.xy), i.next.z);
  return c;
}

#if LIT_SMOKE
// The texture's alpha along `step` from `uv`, three steps, kept within the frame's cell (from `low`, `cell` wide);
// `dx`, `dy` the uv's change across a pixel (read outside any branch).
float AlphaAlong(float2 uv, float2 step, float2 cell, float2 dx, float2 dy) {
  float2 low = floor(uv / cell) * cell, a = low + cell * 0.01, b = low + cell * 0.99;
  return particleTexture.SampleGrad(samplerLinear, clamp(uv + step, a, b), dx, dy).a +
         particleTexture.SampleGrad(samplerLinear, clamp(uv + step * 2, a, b), dx, dy).a +
         particleTexture.SampleGrad(samplerLinear, clamp(uv + step * 3, a, b), dx, dy).a;
}
// The sun's and the sky's colours as lit smoke takes them: their hue, as bright as white light (the smoke's texture
// is its colour as the day shows it; the light only shares that out); the sky's half-way to white.
float3 SmokeTint(float3 c) { return max(c, 1e-4) / max(dot(max(c, 1e-4), float3(0.2126, 0.7152, 0.0722)), 1e-4); }
float3 SmokeSunTint() { return SmokeTint(pow(max(sunColor, 0.001), 2.2)); }
float3 SmokeSkyTint() {
  return skyZenith.w > 0.5 ? lerp(float3(1, 1, 1), SmokeTint(skyZenith.rgb * 0.8 + skyHorizon.rgb * 0.2), 0.4) : float3(1, 1, 1);
}

// Smoke and dust lit as a volume (World::litSmoke). `day`: its colour as today's modern look has it (linear, by the
// day's brightness); `own`: its colour as light, before that; `c`: its colour and alpha as the texture and ramp give
// them. The sun's light that reaches a place in the puff is what the corner's light says comes through the shadows,
// times what comes through the puff itself on the way: its alpha read toward the sun across the texture (where the
// sun is to its side), or its thickness here (where the sun is behind it); light from the sun is sent on a little
// more forward than back, so thin smoke before the sun brightens. The sky lights it from round about, less where
// it is dense; the small lights as near as they are, less through a dense puff. What glows in a warm colour of its own (a fireball drawn
// covering, the first moments of a burning wreck's plume) keeps its own brightness.
float3 LitSmoke(VOut i, float3 day, float3 own, float4 c) {
  float3 L = sunDirection, V = normalize(cameraPosition - i.world);
  float3 out_ = normalize(cross(i.down, i.across));
  if (dot(out_, V) < 0) out_ = -out_;
  float2 inPlane = float2(dot(L, i.across), dot(L, i.down));  // (the texture's u runs along `across`, v along `down`)
  float side = saturate(dot(inPlane, inPlane));
  float2 cell = options.y > 0.5 ? sheet.xy : float2(1, 1);
  float2 step = inPlane * rsqrt(max(side, 1e-6)) * cell * 0.12;
  float2 dx = ddx(i.uv) * exp2(temporal.x), dy = ddy(i.uv) * exp2(temporal.x);  // (the upscaler's bias, as Sample has it)
  float toward = 0;
  [branch] if (side > 0.02) {  // (the sun nearly behind the puff or before it: its thickness here says it all)
    toward = AlphaAlong(i.uv, step, cell, dx, dy);
    if (i.next.z > 0) toward = lerp(toward, AlphaAlong(i.next.xy, step, cell, dx, dy), i.next.z);
  }
  float ramp = rampTexture.Sample(samplerLinearClamp, i.rampUv).a;
  float through = c.a * lerp(0.2, 1.0, saturate(0.5 - 0.5 * dot(L, out_)));
  float dense = lit.y * (side * toward * ramp / 3 + (1 - side) * through);
  float sun = i.light.a * (exp(-dense) + 0.6 * exp(-0.25 * dense)) / 1.6;  // (and inside the puff the same)
  float mu = dot(-L, V), g = 0.3;
  // (a third of the way to one scattering's phase: light scattered on and on inside a puff goes every way, and
  // seen with the sun behind the eye, as an overhead camera mostly is, smoke should not turn dull)
  float phase = lerp(1, (1 - g * g) / pow(max(1 + g * g - 2 * g * mu, 1e-4), 1.5), 0.3);
  // (its sunny side brighter than unlit, its far side darker)
  float3 shade = SmokeSkyTint() * (0.45 * (1 - 0.3 * saturate(c.a * lit.y * 0.5))) + SmokeSunTint() * (1.05 * sun * phase);
  float glows = saturate((c.r - c.b) / max(c.r, 1e-3) * 2 - 0.8);
  // (the small lights' light reaches the face the eye sees through part of the puff: a dense puff glows less on it
  // than a thin one, so a pall round a fire glows at its thin edges and near the flames, not all through)
  return day * lerp(shade, 1, glows) + own * i.light.rgb * (lit.z * exp(-0.6 * lit.y * c.a));
}

// A flipbook baked with light from six directions (particles.h, SixWayTexture): the light its frame gives back
// toward the eye where it is lit from `l` (toward the light, in the world), from its two textures' frames: `a`
// (the blueprint's Texture: lit from the right, from the top, from behind; alpha) and `b` (SixWayTexture: from
// the left, from below, from the front; what glows), each direction's map weighed by the square of the light's
// share along it (the six weights add up to 1). Right and top are the frame's own as rendered (`across`, and the
// texture's top, which lies along -`down`: upright, the vertex shader turns six-way frames back); front faces the eye.
// The maps are stored as a screen shows colours (gamma 2.2) and read back to light.
float SixWayLight(VOut i, float3 out_, float3 l, float3 a, float3 b) {
  float3 d = float3(dot(l, i.across), -dot(l, i.down), dot(l, out_));
  float3 w = d * d;
  return w.x * (d.x > 0 ? a.r : b.r) + w.y * (d.y > 0 ? a.g : b.g) + w.z * (d.z > 0 ? b.b : a.b);
}
// What glows in it, by the second texture's alpha (how hot it is, 0..1): dark red, orange, then yellow-white.
float3 SixWayFire(float hot) {
  float3 c = lerp(lerp(float3(0.5, 0.05, 0.01), float3(1.0, 0.36, 0.06), smoothstep(0.0, 0.5, hot)), float3(1.0, 0.82, 0.55), smoothstep(0.5, 1.0, hot));
  return c * (hot * hot);
}
float4 FrameSix(VOut i) {
  float4 c = sixWayTexture.Sample(samplerLinear, i.uv);
  if (i.next.z > 0) c = lerp(c, sixWayTexture.Sample(samplerLinear, i.next.xy), i.next.z);
  return c;
}
// Its colour, light (modern) or as the screen shows it (the original look, the map's sun unshadowed): the ramp's
// colour lit by the sun as the maps say for its direction, through what shadows it (the corner's light), by the
// sky round about as all six say on average, by the small lights; and what glows.
float3 SixWay(VOut i, bool modern) {
  float3 V = normalize(cameraPosition - i.world), out_ = normalize(cross(i.down, i.across));
  if (dot(out_, V) < 0) out_ = -out_;
  float4 a = Frame(i), b = FrameSix(i);
  float3 la = pow(max(a.rgb, 0), 2.2), lb = pow(max(b.rgb, 0), 2.2);
  float3 albedo = pow(max(rampTexture.Sample(samplerLinearClamp, i.rampUv).rgb, 0), 2.2);
  float sun = SixWayLight(i, out_, sunDirection, la, lb);
  float round = (la.r + la.g + la.b + lb.r + lb.g + lb.b) / 6;
  float above = lerp(round, SixWayLight(i, out_, float3(0, 1, 0), la, lb), 0.5);  // (the sky: more from above)
  if (!modern) return pow(max(albedo * (0.45 * above + 1.05 * sun) + SixWayFire(b.a) * six.y, 0), 1 / 2.2);
  float day = skyZenith.w > 0.5 ? skyGlow.w : 1;
  return albedo * (day * (SmokeSkyTint() * (0.45 * above) + SmokeSunTint() * (1.05 * sun * i.light.a)) + i.light.rgb * (round * lit.z)) +
         SixWayFire(b.a) * (six.y * options.w * (falling.w > 0 ? falling.w : 1));
}

// The smoke's shadow pass (smoke.h): what of the sun's light the particle takes away here, as optical depth, with
// its height along the light and the height squared, to be added up.
float4 ShadowPS(VOut i) : SV_Target {
  float4 c = Frame(i) * rampTexture.Sample(samplerLinearClamp, i.rampUv);
  float a = options.z > 0.5 && options.z < 1.5 ? dot(saturate(c.rgb), 1.0 / 3) : saturate(c.a);  // (darkening: what it takes)
  float tau = -log(1 - min(a, 0.95)) * lit.w;
  if (tau < 1e-3) discard;
  float h = dot(smokeMatrix[2], float4(i.world, 1));
  return float4(tau, tau * h, tau * h * h, 1);
}
#endif

float4 PS(VOut i, out uint coverage : SV_Coverage) : SV_Target {
  float meets = Meets(i, coverage);
  if (meets <= 0) discard;
  if (options.z > 3.5) {
    // it bends what is behind it: the picture as it was, taken a little to the side, by the
    // texture's red and green (the original's measure: half a hundredth of the picture at most);
    // how much of that shows, by the texture's alpha and the ramp's
    float4 bend = Frame(i);
    float shows = bend.a * rampTexture.Sample(samplerLinearClamp, i.rampUv).a;
    float2 at = i.position.xy * viewport.zw + 0.005 * extra.y * (2 * bend.rg - 1);
    return float4(behindTexture.SampleLevel(samplerLinearClamp, at, 0).rgb, shows * meets);
  }
  float4 c = Frame(i) * rampTexture.Sample(samplerLinearClamp, i.rampUv);
  if (options.z > 0.5 && options.z < 1.5) return float4(saturate(c.rgb) * meets, 1);  // darkening: what is there x (1 - this)
  if (look > 0.5) {
#if LIT_SMOKE
    float4 given = c;
    float3 own = pow(max(c.rgb, 0), 2.2);
#endif
    // the modern look's picture is linear light; what adds its light is given more of it, to glow
    // (what does not add light, smoke and dust, is lit by the day: darker in a computed night)
    c.rgb = pow(max(c.rgb, 0), 2.2) * (options.z > 1.5 && options.z < 2.5 ? options.w * (falling.w > 0 ? falling.w : 1) : skyZenith.w > 0.5 ? skyGlow.w : 1);
#if LIT_SMOKE
    [branch] if (six.x > 0.5) {
      c.rgb = SixWay(i, true) * (options.z > 2.5 ? c.a : 1);  // (premultiplied: the colour carries its alpha)
    } else if (lit.x > 0.5) {
      c.rgb = LitSmoke(i, c.rgb, own, given);
    } else {
#endif
    [branch] if (extra.x > 0.5 && (options.z < 0.5 || options.z > 2.5)) {
      // smoke and dust lit through its normal map: the sun on the side that faces it (softly: smoke
      // lets light through), the sky from above, around the brightness it had unlit
      float3 t = particleNormals.Sample(samplerLinear, i.uv).xyz * 2 - 1;
      float3 out_ = normalize(cross(i.down, i.across));
      if (dot(out_, cameraPosition - i.world) < 0) out_ = -out_;
      float3 n = normalize(i.across * t.x - i.down * t.y + out_ * max(t.z, 0.05));
      float sunSide = saturate((dot(n, sunDirection) + 0.35) / 1.35);
      float3 sunTint = pow(max(sunColor, 0.001), 2.2);
      sunTint /= max(max(sunTint.r, sunTint.g), max(sunTint.b, 1e-3));
      float above = saturate(n.y * 0.5 + 0.5);
      c.rgb *= 0.45 + 0.85 * sunSide * sunTint + 0.25 * above;
    }
#if LIT_SMOKE
    }
#endif
    [branch] if (air.x > 0.5) {  // light in the air (air.h): what comes through of its own, and the air's light before it
      float4 a = AirAt(i.world);
      c.rgb = options.z > 1.5 && options.z < 2.5 ? c.rgb * a.a : c.rgb * a.a + a.rgb * (options.z > 2.5 ? c.a : 1);
    } else {
      if (weather.z > 0) {  // thick air between it and the eye: its own light is lost in it, its colour goes to the air's
        float lost = min(1 - exp(-i.position.w * weather.z), weather.w);
        c.rgb = options.z > 1.5 && options.z < 2.5 ? c.rgb * (1 - lost) : lerp(c.rgb, AirColor() * (options.z > 2.5 ? c.a : 1), lost);
      }
      if (mist.x > 0) {  // and the mist on low ground
        float lost = MistLost(i.world);
        c.rgb = options.z > 1.5 && options.z < 2.5 ? c.rgb * (1 - lost) : lerp(c.rgb, MistColor() * (options.z > 2.5 ? c.a : 1), lost);
      }
    }
  }
#if LIT_SMOKE
  else if (six.x > 0.5) {
    c.rgb = SixWay(i, false) * (options.z > 2.5 ? c.a : 1);
  }
#endif
  if (options.z > 2.5) c *= meets;  // (premultiplied: the colour carries its alpha)
  else c.a *= meets;
  return c;
}
)hlsl";

// Heat shimmer (World::heatShimmer): over what is hot, a patch of air that bends what is behind it, as the
// particles that bend (blend mode 5) do it: the picture as it was, taken a little to the side by two noises that
// rise with the hot air, laid over by how strong the bending is there. Strongest just over the source and fading
// up and to the sides; a few pixels at most, so that it reads as air and never swims.
const char kHeatShader[] = R"hlsl(
cbuffer HeatSet : register(b1) {
  float4 heat;  // x: the most it bends, in pixels of a picture 1080 high; y: the camera's near distance; z: how fast the hot air rises
};
StructuredBuffer<float4> heatRows : register(t0, space1);  // two rows each: place, size; strength, seed
Texture2D behindTexture : register(t2);
#ifdef MSAA
Texture2DMS<float> sceneDepth : register(t3);
#else
Texture2D<float> sceneDepth : register(t3);
#endif

struct HOut {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;    // -1..1 across, -1 at the source .. 1 at the top
  float4 info : TEXCOORD1;  // strength, seed, the most it bends here (pixels), its size
};

HOut HeatVS(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
  float4 a = heatRows[instance * 2], b = heatRows[instance * 2 + 1];
  const float2 corners[6] = {float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1)};
  float2 corner = corners[vertex];
  float3 right = normalize(viewProj[0].xyz), up = normalize(viewProj[1].xyz);
  // (facing the camera, standing on the source and reaching up over it)
  float3 middle = a.xyz + float3(0, a.w * 0.9, 0);
  HOut o;
  o.position = mul(viewProj, float4(middle + right * (corner.x * a.w * 1.2) + up * (corner.y * a.w * 1.5), 1));
  o.uv = corner;
  // how many pixels a world unit is there: the bend is a share of the source's size, a few pixels at most
  float4 m = mul(viewProj, float4(middle, 1)), n = mul(viewProj, float4(middle + up, 1));
  float perUnit = length((n.xy / max(n.w, 1e-3) - m.xy / max(m.w, 1e-3)) * viewport.xy * 0.5);
  o.info = float4(b.x, b.y, min(heat.x * viewport.y / 1080, 0.04 * a.w * perUnit), a.w);
  return o;
}

float4 HeatPS(HOut i, out uint coverage : SV_Coverage) : SV_Target {
  // only over what lies behind the hot air: of the pixel's samples, those where nothing solid is in front
  coverage = 0;
#ifdef MSAA
  int2 pixel = int2(i.position.xy);
  for (uint s = 0; s < SAMPLES; ++s) {
    float d = sceneDepth.Load(pixel, s);
    if ((d > 0 ? heat.y / d : 1e9) > i.position.w) coverage |= 1u << s;
  }
#else
  float d = sceneDepth.Load(int3(i.position.xy, 0));
  if ((d > 0 ? heat.y / d : 1e9) > i.position.w) coverage = 1;
#endif
  if (coverage == 0) discard;
  float across = saturate(1 - i.uv.x * i.uv.x);
  float strength = across * across * smoothstep(-1, -0.55, i.uv.y) * (1 - smoothstep(-0.3, 1, i.uv.y)) * i.info.x;
  if (strength < 0.01 || i.info.z < 0.05) discard;
  // two noises over the patch, rising with the air (in the patch's own measure: a larger fire has larger waves)
  float3 q = float3(i.uv * float2(5.0, 4.0) - float2(0, time * heat.z), i.info.y + time * 0.6);
  float2 bend = float2(CloudNoise(q), CloudNoise(q + float3(5.2, 1.3, 7.7))) * 2 - 1;
  float2 at = i.position.xy * viewport.zw + bend * (i.info.z * strength) * viewport.zw;
  return float4(behindTexture.SampleLevel(samplerLinearClamp, at, 0).rgb, saturate(strength * 1.5));
}
)hlsl";

float Number(const BpValue& v, float fallback) { return static_cast<float>(v.NumberOr(fallback)); }
bool Flag(const BpValue& v, bool fallback) { return v.kind == BpValue::Bool ? v.boolean : fallback; }

// The colour a particle texture shows on average, as light (linear; each texel weighed by its alpha): with its
// ramp's, the colour of a fire's glow (Particles::HotEmitters). White where the texture cannot be read.
Vec3 TextureGlow(const moho::Vfs& vfs, const std::string& path) {
  DdsImage image;
  std::vector<uint8_t> pixels;
  std::string error;
  auto file = vfs.ReadFile(path);
  if (!file || !ParseDds(*file, &image, &error) || !DecodeRgba(image, 0, &pixels) || pixels.empty()) return {1, 1, 1};
  double r = 0, g = 0, b = 0;
  const size_t count = pixels.size() / 4, stride = std::max<size_t>(1, count / 4096);
  for (size_t i = 0; i < count; i += stride) {
    const uint8_t* p = &pixels[i * 4];
    const double a = p[3] / 255.0;
    r += std::pow(p[0] / 255.0, 2.2) * a;
    g += std::pow(p[1] / 255.0, 2.2) * a;
    b += std::pow(p[2] / 255.0, 2.2) * a;
  }
  const double most = std::max({r, g, b, 1e-9});
  return {static_cast<float>(r / most), static_cast<float>(g / most), static_cast<float>(b / most)};
}

void ReadCurve(const BpValue& v, EmitterCurve* out) {
  out->xRange = Number(v["XRange"], 1);
  for (const BpValue& key : v["Keys"].items) out->keys.push_back({Number(key["x"], 0), Number(key["y"], 0), Number(key["z"], 0)});
  std::stable_sort(out->keys.begin(), out->keys.end(), [](Vec3 a, Vec3 b) { return a.x < b.x; });
}

}  // namespace

void EmitterCurve::At(float x, float* value, float* spread) const {
  *value = *spread = 0;
  if (keys.empty()) return;
  size_t i = 0;
  while (i + 1 < keys.size() && keys[i + 1].x <= x) ++i;
  const Vec3& a = keys[i];
  if (x <= a.x || i + 1 == keys.size()) {
    *value = a.y;
    *spread = a.z;
    return;
  }
  const Vec3& b = keys[i + 1];
  const float t = (x - a.x) / (b.x - a.x);
  *value = a.y + (b.y - a.y) * t;
  *spread = a.z + (b.z - a.z) * t;
}

bool Particles::Init(std::string* error) {
  std::string source = std::string(gSceneSamples > 1 ? "#define MSAA 1\n" : "") + "#define SAMPLES " + std::to_string(gSceneSamples) + "\n" + kViewShaderBlock + kShader;
  gfx::PipelineDesc pd;
  pd.vertexSource = pd.pixelSource = source;
  pd.cull = gfx::Cull::None;
  pd.depthTest = pd.depthWrite = false;  // (the shader tests the scene's depth itself)
  pd.depthTarget = false;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  const struct {
    const char* name;
    gfx::Blend blend;
  } kinds[4] = {{"particles, alpha", gfx::Blend::Alpha},
                {"particles, darkening", gfx::Blend::InverseModulate},
                {"particles, adding", gfx::Blend::Additive},
                {"particles, premultiplied", gfx::Blend::Premultiplied}};
  for (int i = 0; i < 4; ++i) {
    pd.name = kinds[i].name;
    pd.blend = kinds[i].blend;
    pd.alphaWrite = false;  // (the original's particles leave the glow as it is)
    pipelines_[i] = device_->CreatePipeline(pd, error);
    if (!pipelines_[i]) return false;
  }
  gfx::BufferDesc bd;
  bd.kind = gfx::BufferKind::Shader;
  bd.stride = 16;
  bd.dynamic = true;
  buffer_ = device_->CreateBuffer(bd);
  heatBuffer_ = device_->CreateBuffer(bd);
  litTried_ = litMade_ = heatTried_ = false;  // (lit smoke's and the heat's pipelines made again when wanted: the samples may differ)
  return true;
}

// The lit smoke's pipelines and the smoke's shadow pass's (World::litSmoke; the particles' shader with LIT_SMOKE and
// the lighting block), the first time they are wanted; where they cannot be made, smoke stays as it was.
bool Particles::LitReady() {
  if (litTried_) return litMade_;
  litTried_ = true;
  std::string error;
  const std::string source = std::string("#define LIT_SMOKE 1\n") + (gSceneSamples > 1 ? "#define MSAA 1\n" : "") + "#define SAMPLES " +
                             std::to_string(gSceneSamples) + "\n" + kViewShaderBlock + kLightingShaderBlock + kShader;
  gfx::PipelineDesc pd;
  pd.vertexSource = pd.pixelSource = source;
  pd.cull = gfx::Cull::None;
  pd.depthTest = pd.depthWrite = false;
  pd.depthTarget = false;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pd.alphaWrite = false;
  pd.name = "particles, lit smoke, alpha";
  pd.blend = gfx::Blend::Alpha;
  litPipelines_[0] = device_->CreatePipeline(pd, &error);
  pd.name = "particles, lit smoke, premultiplied";
  pd.blend = gfx::Blend::Premultiplied;
  if (litPipelines_[0]) litPipelines_[1] = device_->CreatePipeline(pd, &error);
  // the smoke as the sun sees it: added up into the smoke's shadow map (one sample a texel)
  pd.name = "particles, the smoke's shadow";
  pd.pixelEntry = "ShadowPS";
  pd.blend = gfx::Blend::Additive;
  pd.colorFormat = gfx::Format::RGBA16F;
  pd.samples = 1;
  pd.alphaWrite = true;
  if (litPipelines_[1]) shadowPipeline_ = device_->CreatePipeline(pd, &error);
  litMade_ = litPipelines_[0] && litPipelines_[1] && shadowPipeline_;
  if (!litMade_) fprintf(stderr, "render: lit smoke cannot be drawn (%s)\n", error.c_str());
  return litMade_;
}

bool Particles::HeatReady() {
  if (heatTried_) return static_cast<bool>(heatPipeline_);
  heatTried_ = true;
  std::string error;
  const std::string source = std::string(gSceneSamples > 1 ? "#define MSAA 1\n" : "") + "#define SAMPLES " + std::to_string(gSceneSamples) + "\n" +
                             kViewShaderBlock + kHeatShader;
  gfx::PipelineDesc pd;
  pd.name = "heat shimmer";
  pd.vertexSource = pd.pixelSource = source;
  pd.vertexEntry = "HeatVS";
  pd.pixelEntry = "HeatPS";
  pd.cull = gfx::Cull::None;
  pd.depthTest = pd.depthWrite = false;
  pd.depthTarget = false;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pd.blend = gfx::Blend::Alpha;
  pd.alphaWrite = false;
  heatPipeline_ = device_->CreatePipeline(pd, &error);
  if (!heatPipeline_) fprintf(stderr, "render: heat shimmer cannot be drawn (%s)\n", error.c_str());
  return static_cast<bool>(heatPipeline_);
}

void Particles::SetWater(float level, std::function<float(float, float)> groundHeight) {
  haveWater_ = true;
  waterLevel_ = level;
  groundHeight_ = std::move(groundHeight);
}

float Particles::Random() {
  seed_ ^= seed_ << 13;
  seed_ ^= seed_ >> 17;
  seed_ ^= seed_ << 5;
  return static_cast<float>(seed_ >> 8) * (1.0f / 16777216.0f);
}

const EmitterBlueprint* Particles::Blueprint(const std::string& path, const std::string& faction) {
  // (each look its own: the remastered one may take a remastered texture for the game's blueprint;
  // and in it a faction its own blueprint where the visuals track has made one - where it has not, the
  // faction plays the same blueprint as no faction)
  std::string key = moho::Vfs::Normalize(path) + (remastered ? "|remastered" : "");
  bool own = false;
  if (remastered && !faction.empty()) {
    const std::string asked = key + "|" + faction;
    auto there = factionFiles_.find(asked);
    if (there == factionFiles_.end()) there = factionFiles_.emplace(asked, vfs_->Exists(FactionEffectFile(path, faction))).first;
    if (there->second) {
      key = asked;
      own = true;
    }
  }
  auto known = blueprints_.find(key);
  if (known != blueprints_.end()) return known->second.get();
  EmitterBlueprint* bp = blueprints_.emplace(key, std::make_unique<EmitterBlueprint>()).first->second.get();
  {  // (a fire by its name: flames, embers, burning; never a build's sparks, a warp-in's, an engine's or an exhaust's glow)
    const std::string name = moho::Vfs::Normalize(path);
    auto has = [&](const char* word) { return name.find(word) != std::string::npos; };
    bp->fire = (has("fire") || has("flame") || has("burn") || has("ember") || has("napalm")) && !has("build") &&
               !has("teleport") && !has("afterburn") && !has("exhaust") && !has("engine") && !has("thrust");
  }
  const std::string file = own ? FactionEffectFile(path, faction) : moho::Vfs::Normalize(EffectFile(*vfs_, path, remastered));
  auto text = vfs_->ReadFile(file);
  BpValue v;
  if (!text || !ReadBlueprintFile(*text, &v)) {
    fprintf(stderr, "render: emitter %s: %s\n", file.c_str(), text ? "the blueprint cannot be read" : "no such blueprint");
    return bp;
  }
  bp->lifetime = Number(v["Lifetime"], 1);
  bp->repeatTime = Number(v["Repeattime"], 1);
  bp->frameCount = std::max(1, static_cast<int>(Number(v["TextureFramecount"], 1)));
  bp->stripCount = std::max(1, static_cast<int>(Number(v["TextureStripcount"], 1)));
  bp->blendMode = static_cast<int>(Number(v["Blendmode"], 0));
  bp->localVelocity = Flag(v["LocalVelocity"], false);
  bp->localAcceleration = Flag(v["LocalAcceleration"], false);
  bp->gravity = Flag(v["Gravity"], false);
  bp->alignRotation = Flag(v["AlignRotation"], false);
  bp->alignToBone = Flag(v["AlignToBone"], false);
  bp->flat = Flag(v["Flat"], false);
  bp->interpolateEmission = Flag(v["InterpolateEmission"], true);
  bp->emitIfVisible = Flag(v["EmitIfVisible"], true);
  bp->createIfVisible = Flag(v["CreateIfVisible"], false);
  bp->snapToWaterline = Flag(v["SnapToWaterline"], false);
  bp->onlyEmitOnWater = Flag(v["OnlyEmitOnWater"], false);
  bp->resistance = Flag(v["ParticleResistance"], false);
  bp->gridAnimate = Flag(v["TextureGridAnimate"], false);
  bp->gridLoop = Flag(v["TextureGridLoop"], false);
  bp->frameBlend = Flag(v["TextureFrameBlend"], false) || bp->gridAnimate;
  bp->normalPath = v["NormalTexture"].StringOr(std::string());
  bp->sixWayPath = v["SixWayTexture"].StringOr(std::string());
  bp->sixWayEmission = Number(v["SixWayEmission"], 1);
  bp->refractionScale = Number(v["RefractionScale"], 1);
  bp->collideGround = Flag(v["CollideGround"], false);
  bp->keepAtLowQuality = Flag(v["KeepAtLowQuality"], false);
  bp->lightRadius = Number(v["LightRadius"], 0);
  bp->lightSeconds = Number(v["LightSeconds"], 0);
  bp->lightStrength = Number(v["LightStrength"], 1);
  bp->volumeReplaced = Flag(v["VolumeReplaces"], false);
  bp->sortOrder = static_cast<int>(Number(v["SortOrder"], 0));
  bp->lodCutoff = std::max(0.0f, Number(v["LODCutoff"], 0));
  bp->texturePath = v["Texture"].StringOr(std::string());
  bp->rampPath = v["RampTexture"].StringOr(std::string());
  const struct {
    const char* name;
    EmitterCurve* curve;
  } curves[] = {{"XDirectionCurve", &bp->xDirection}, {"YDirectionCurve", &bp->yDirection}, {"ZDirectionCurve", &bp->zDirection},
                {"EmitRateCurve", &bp->emitRate}, {"LifetimeCurve", &bp->particleLifetime}, {"VelocityCurve", &bp->velocity},
                {"XAccelCurve", &bp->xAccel}, {"YAccelCurve", &bp->yAccel}, {"ZAccelCurve", &bp->zAccel},
                {"XPosCurve", &bp->xPos}, {"YPosCurve", &bp->yPos}, {"ZPosCurve", &bp->zPos},
                {"SizeCurve", &bp->size}, {"StartSizeCurve", &bp->startSize}, {"EndSizeCurve", &bp->endSize},
                {"InitialRotationCurve", &bp->initialRotation}, {"RotationRateCurve", &bp->rotationRate},
                {"FrameRateCurve", &bp->frameRate}, {"TextureSelectionCurve", &bp->textureSelection},
                {"RampSelectionCurve", &bp->rampSelection}, {"ResistanceCurve", &bp->resistanceCurve}};
  for (const auto& c : curves) ReadCurve(v[c.name], c.curve);
  bp->longestLife = 1;
  for (const Vec3& key : bp->particleLifetime.keys) bp->longestLife = std::max(bp->longestLife, key.y + std::fabs(key.z) * 0.5f);
  bp->texture = textures_->Get(EffectFile(*vfs_, bp->texturePath, remastered), textures_->White());
  bp->ramp = bp->rampPath.empty() ? textures_->White() : textures_->Get(EffectFile(*vfs_, bp->rampPath, remastered), textures_->White());
  bp->normals = bp->normalPath.empty() ? textures_->FlatNormal() : textures_->Get(EffectFile(*vfs_, bp->normalPath, remastered), textures_->FlatNormal());
  if (!bp->sixWayPath.empty()) bp->sixWay = textures_->Get(EffectFile(*vfs_, bp->sixWayPath, remastered), textures_->White());
  for (const EmitterCurve* c : {&bp->size, &bp->startSize, &bp->endSize})
    for (const Vec3& key : c->keys) bp->largestSize = std::max(bp->largestSize, key.y + std::fabs(key.z) * 0.5f);
  bp->ok = true;
  return bp;
}

uint32_t Particles::Start(const std::string& blueprint, Vec3 position, const Quat& orientation, float scale, double startedAt, bool attached,
                          const std::string& faction) {
  const EmitterBlueprint* bp = Blueprint(blueprint, faction);
  if (!bp->ok) return 0;
  if (volumesOn && bp->volumeReplaced) return 0;  // (volume explosions: its volume is drawn in its place)
  std::unique_ptr<Set>& set = sets_[bp];
  if (!set) {
    set = std::make_unique<Set>();
    set->blueprint = bp;
    order_.push_back(set.get());
    // by the blueprints' sort order; within it what adds its light after what covers or darkens
    std::stable_sort(order_.begin(), order_.end(), [](const Set* a, const Set* b) {
      if (a->blueprint->sortOrder != b->blueprint->sortOrder) return a->blueprint->sortOrder < b->blueprint->sortOrder;
      return (a->blueprint->blendMode == 3) < (b->blueprint->blendMode == 3);
    });
  }
  Emitter e;
  e.id = nextId_++;
  e.blueprint = bp;
  e.set = set.get();
  e.position = e.positionBefore = position;
  e.orientation = orientation;
  e.scale = scale;
  e.lifetime = bp->lifetime;
  e.repeatTime = bp->repeatTime;
  e.lodCutoff = bp->lodCutoff;
  e.emitIfVisible = bp->emitIfVisible;
  e.attached = attached;
  e.started = startedAt >= 0 ? startedAt : now_;
  index_[e.id] = emitters_.size();
  emitters_.push_back(e);
  // the remastered look: a flash or an explosion lights what is around it for a moment. By
  // default a short emitter (three seconds at most) whose particles add their light, as large as
  // they are; the blueprint may say otherwise (LightRadius, LightSeconds, LightStrength)
  const bool adds = (bp->blendMode == 2 || bp->blendMode == 3) && !bp->rampPath.empty();
  const bool brief = bp->lifetime > 0 && bp->lifetime <= 30;
  if (remastered && bp->lightStrength > 0 && (bp->lightRadius > 0 || (adds && brief)) && flashes.size() < 400) {
    Flash f;
    f.position = position;
    f.born = e.started;
    f.radius = bp->lightRadius > 0 ? bp->lightRadius * scale : std::clamp(bp->largestSize * scale * 3.0f, 1.5f, 14.0f);
    f.seconds = bp->lightSeconds > 0 ? bp->lightSeconds : std::clamp(bp->longestLife * kTick, 0.12f, 0.8f);
    f.strength = bp->lightStrength;
    f.ramp = EffectFile(*vfs_, bp->rampPath, remastered);
    if (heat && puffs_.size() < 256) puffs_.push_back({position, e.started, f.radius});  // (and hot air, a moment longer)
    flashes.push_back(std::move(f));
  }
  return e.id;
}

Particles::Emitter* Particles::Find(uint32_t id) {
  auto it = index_.find(id);
  return it == index_.end() ? nullptr : &emitters_[it->second];
}

bool Particles::Glows(uint32_t emitter, std::string* rampPath, float* scale) const {
  auto it = index_.find(emitter);
  if (it == index_.end()) return false;
  const Emitter& e = emitters_[it->second];
  if (!e.blueprint || (e.blueprint->blendMode != 2 && e.blueprint->blendMode != 3) || e.blueprint->rampPath.empty()) return false;
  *rampPath = e.blueprint->rampPath;
  *scale = e.scale;
  return true;
}

float Particles::ScaleOf(uint32_t emitter) const {
  auto it = index_.find(emitter);
  return it == index_.end() ? 1.0f : emitters_[it->second].scale;
}

void Particles::SetScale(uint32_t emitter, float scale) {
  if (Emitter* e = Find(emitter)) e->scale = scale;
}

void Particles::SetParam(uint32_t emitter, const std::string& name, float value) {
  Emitter* e = Find(emitter);
  if (!e) return;
  std::string n = name;
  for (char& c : n) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (n == "LIFETIME") e->lifetime = value;
  else if (n == "REPEATTIME") e->repeatTime = value;
  else if (n == "SCALE") e->scale = value;
  else if (n == "LODCUTOFF") e->lodCutoff = std::max(0.0f, value);
  else if (n == "EMITIFVISIBLE") e->emitIfVisible = value > 0;
}

void Particles::SetCurve(uint32_t emitter, const std::string& name, float height, float size) {
  Emitter* e = Find(emitter);
  if (!e) return;
  std::string n = name;
  for (char& c : n) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (n.size() > 6 && n.compare(n.size() - 6, 6, "_CURVE") == 0) n.resize(n.size() - 6);
  static const struct {
    const char* name;
    EmitterCurve EmitterBlueprint::* curve;
  } kCurves[] = {{"XDIR", &EmitterBlueprint::xDirection}, {"YDIR", &EmitterBlueprint::yDirection}, {"ZDIR", &EmitterBlueprint::zDirection},
                 {"EMITRATE", &EmitterBlueprint::emitRate}, {"LIFETIME", &EmitterBlueprint::particleLifetime}, {"VELOCITY", &EmitterBlueprint::velocity},
                 {"X_ACCEL", &EmitterBlueprint::xAccel}, {"Y_ACCEL", &EmitterBlueprint::yAccel}, {"Z_ACCEL", &EmitterBlueprint::zAccel},
                 {"RESISTANCE", &EmitterBlueprint::resistanceCurve}, {"SIZE", &EmitterBlueprint::size},
                 {"X_POSITION", &EmitterBlueprint::xPos}, {"Y_POSITION", &EmitterBlueprint::yPos}, {"Z_POSITION", &EmitterBlueprint::zPos},
                 {"BEGINSIZE", &EmitterBlueprint::startSize}, {"ENDSIZE", &EmitterBlueprint::endSize},
                 {"ROTATION", &EmitterBlueprint::initialRotation}, {"ROTATION_RATE", &EmitterBlueprint::rotationRate},
                 {"FRAMERATE", &EmitterBlueprint::frameRate}, {"TEXTURESELECTION", &EmitterBlueprint::textureSelection},
                 {"RAMPSELECTION", &EmitterBlueprint::rampSelection}};
  for (const auto& known : kCurves) {
    if (n != known.name) continue;
    EmitterCurve set;
    set.xRange = (e->blueprint->*known.curve).xRange;
    set.keys.push_back({0, height, size});
    for (auto& have : e->curves)
      if (have.first == known.curve) {
        have.second = set;
        return;
      }
    e->curves.emplace_back(known.curve, set);
    return;
  }
}

void Particles::Move(uint32_t emitter, Vec3 position, const Quat& orientation) {
  if (Emitter* e = Find(emitter)) {
    e->position = position;
    e->orientation = orientation;
  }
}

void Particles::Stop(uint32_t emitter) {
  if (Emitter* e = Find(emitter)) e->stopped = true;
}

bool Particles::Running(uint32_t emitter) const {
  auto it = index_.find(emitter);
  return it != index_.end() && !emitters_[it->second].stopped;
}

// What an emitter throws out in one tick of its own time, as the original does it
// (docs/engine-answers.md, render 9): the curves are read at the emitter's age modulo its repeat
// time, each reading with its own random spread; so many particles a tick, the fraction carried on;
// with InterpolateEmission the tick's particles are spread over the tick, in time and along the
// emitter's way.
void Particles::Emit(Emitter* emitter, int tick) {
  const EmitterBlueprint& bp = *emitter->blueprint;
  auto curveTime = [&](float age) {
    if (emitter->repeatTime <= 0) return age;
    const float x = std::fmod(age, emitter->repeatTime);
    return x < 0 ? x + emitter->repeatTime : x;
  };
  float x = curveTime(static_cast<float>(tick));
  auto read = [&](const EmitterCurve& ofBlueprint) {
    const EmitterCurve* curve = &ofBlueprint;
    for (const auto& set : emitter->curves)  // (one a script set on this emitter takes the blueprint's place)
      if (&(bp.*set.first) == curve) {
        curve = &set.second;
        break;
      }
    float value, spread;
    curve->At(x, &value, &spread);
    return value + (Random() - 0.5f) * spread;
  };
  const float share = bp.keepAtLowQuality ? 1.0f : std::clamp(quality, 0.0f, 1.0f);
  const float wanted = std::max(0.0f, read(bp.emitRate)) * share + emitter->carry;
  const int count = static_cast<int>(wanted);
  emitter->carry = wanted - static_cast<float>(count);
  const double tickStart = emitter->started + static_cast<double>(tick) * kTick;
  const float scale = emitter->scale;
  const Vec3 forward = Rotate(emitter->orientation, {0, 0, 1});
  const bool animated = bp.frameCount > 1 || bp.stripCount > 1;
  for (int i = 0; i < count; ++i) {
    const float fraction = bp.interpolateEmission ? static_cast<float>(i) / static_cast<float>(count) : 0;
    x = curveTime(static_cast<float>(tick) + fraction);
    const float life = read(bp.particleLifetime) * kTick;
    if (life <= 0) continue;
    // where it starts: its place in the emitter's own frame, then anywhere on the emitter's disc
    // (a level disc, denser toward the middle)
    const Vec3 origin = bp.interpolateEmission ? Lerp(emitter->positionBefore, emitter->position, fraction) : emitter->position;
    Vec3 position = origin + Rotate(emitter->orientation, Vec3{read(bp.xPos), read(bp.yPos), read(bp.zPos)} * scale);
    const float disc = read(bp.size) * scale;
    if (disc != 0) {
      const Vec3 way = Normalize(Vec3{Random() - 0.5f, 0, Random() - 0.5f});
      position += way * ((Random() - 0.5f) * disc);
    }
    Vec3 direction = Vec3{read(bp.xDirection), read(bp.yDirection), read(bp.zDirection)} * scale;  // (not made unit length)
    if (bp.localVelocity) direction = Rotate(emitter->orientation, direction);
    Vec3 velocity = direction * (read(bp.velocity) / kTick);
    Vec3 acceleration = Vec3{read(bp.xAccel), read(bp.yAccel), read(bp.zAccel)} * scale;
    if (bp.localAcceleration) acceleration = Rotate(emitter->orientation, acceleration);
    if (bp.gravity) acceleration.y -= 0.02f;  // per tick squared
    acceleration = acceleration * (1 / (kTick * kTick));
    float rotation = read(bp.initialRotation) * (3.14159265f / 180);
    if (bp.alignToBone && !bp.alignRotation) {
      if (bp.flat) rotation = std::atan2(-forward.x, forward.z);  // lying flat, turned as the bone is
      else velocity = forward;                                    // standing along the bone, where it is
    }
    // the water's surface
    if (haveWater_) {
      if (bp.onlyEmitOnWater) {
        if (groundHeight_ && groundHeight_(position.x, position.z) > waterLevel_) continue;
        position.y = waterLevel_ + 0.01f;
      } else if (bp.snapToWaterline) {
        if (bp.sortOrder < -101) position.y = std::min(position.y, waterLevel_ - 0.01f);
        else position.y = std::max(position.y, waterLevel_ + 0.01f);
      }
    }
    const float size0 = read(bp.startSize) * scale, size1 = read(bp.endSize) * scale;
    Particle p;
    p.position[0] = position.x;
    p.position[1] = position.y;
    p.position[2] = position.z;
    p.rotation = rotation;
    p.velocity[0] = velocity.x;
    p.velocity[1] = velocity.y;
    p.velocity[2] = velocity.z;
    p.rotationRate = read(bp.rotationRate) * (3.14159265f / 180) / kTick;
    p.acceleration[0] = acceleration.x;
    p.acceleration[1] = acceleration.y;
    p.acceleration[2] = acceleration.z;
    p.startTime = static_cast<float>(tickStart + static_cast<double>(fraction * kTick));
    p.lifetime = life;
    p.size = size0;
    p.sizeRate = (size1 - size0) / life;
    p.frameRate = read(bp.frameRate) / kTick;
    p.stripV = animated ? std::floor(read(bp.textureSelection)) / static_cast<float>(bp.stripCount) : 0;
    p.rampV = read(bp.rampSelection);  // (as it is: the ramp texture is read at this height, held at its edges)
    p.drag = bp.resistance ? std::max(0.0f, read(bp.resistanceCurve)) / kTick : 0;
    p.unused = bp.collideGround ? (groundHeight_ ? groundHeight_(position.x, position.z) : emitter->position.y) : 0;
    emitter->set->particles.push_back(p);
  }
  emitter->positionBefore = emitter->position;
}

void Particles::Hide(uint32_t emitter, bool hidden) {
  Emitter* e = Find(emitter);
  if (!e || e->hidden == hidden) return;
  e->hidden = hidden;
  e->positionBefore = e->position;  // (seen again: it starts where it is, not along the way it came unseen)
}

void Particles::Update(double now, const View* view) {
  const auto t0 = std::chrono::steady_clock::now();
  now_ = now;
  gathered_ = false;  // (a new frame: what was gathered for the last one is old)
  quietEmitters = 0;
  flashes.erase(std::remove_if(flashes.begin(), flashes.end(), [now](const Flash& f) { return now - f.born > f.seconds || now < f.born - 1; }),
                flashes.end());
  puffs_.erase(std::remove_if(puffs_.begin(), puffs_.end(), [now](const Puff& p) { return now - p.born > kPuffSeconds || now < p.born - 1; }),
               puffs_.end());
  for (Emitter& e : emitters_) {
    // every whole tick of the emitter's own time that has begun, up to its lifetime
    const int due = static_cast<int>((now - e.started) / kTick) + 1;
    const int last = e.lifetime < 0 ? due : std::min(due, std::max(1, static_cast<int>(std::ceil(e.lifetime))));
    // one that is behind makes up only its last ticks: at most 24, and no more than a particle lives
    const int makeUp = std::min(24, static_cast<int>(std::ceil(e.blueprint->longestLife)));
    if (last - e.ticksDone > makeUp + 1) e.ticksDone = last - makeUp - 1;
    if (e.hidden) {  // what it hangs on is not seen: nothing now, and nothing made up later
      ++quietEmitters;
      e.ticksDone = last;
      if (e.lifetime >= 0 && due > last) e.stopped = true;
      continue;
    }
    if (view && e.emitIfVisible && e.ticksDone < last) {
      const float margin = 8 * std::max(1.0f, e.scale);  // how far its particles may be from it and still be seen
      const Vec3 r{margin, margin, margin};
      const bool tooFar = e.attached && e.lodCutoff > 0 && Length(e.position - view->eye) > e.lodCutoff;
      if (tooFar || !view->BoxVisible(e.position - r, e.position + r)) {
        ++quietEmitters;
        if (e.blueprint->createIfVisible && e.ticksDone == 0) e.stopped = true;  // never seen: never there
        if (e.lifetime >= 0 && due > last) e.stopped = true;                     // its time ran out unseen
        continue;  // (its ticks wait; the last of them are made up when it is seen again)
      }
    }
    while (!e.stopped && e.ticksDone < last) Emit(&e, e.ticksDone++);
    if (e.lifetime >= 0 && e.ticksDone >= last && due > last) e.stopped = true;
  }
  if (std::any_of(emitters_.begin(), emitters_.end(), [](const Emitter& e) { return e.stopped; })) {
    emitters_.erase(std::remove_if(emitters_.begin(), emitters_.end(), [](const Emitter& e) { return e.stopped; }), emitters_.end());
    index_.clear();
    for (size_t i = 0; i < emitters_.size(); ++i) index_[emitters_[i].id] = i;
  }
  liveEmitters = static_cast<int>(emitters_.size());
  updateMs_ = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void Particles::Gather() {
  const auto t0 = std::chrono::steady_clock::now();
  gathered_ = true;
  // drop what is over, and gather the rest in one buffer, set after set
  const float now = static_cast<float>(now_);
  rows_.clear();
  liveParticles = 0;
  spans_.clear();
  bending_ = false;
  for (Set* set : order_) {
    std::vector<Particle>& ps = set->particles;
    ps.erase(std::remove_if(ps.begin(), ps.end(), [now](const Particle& p) { return now - p.startTime >= p.lifetime; }), ps.end());
    spans_.push_back({rows_.size() / 4, ps.size()});
    if (ps.empty()) continue;
    if (set->blueprint->blendMode == 5) bending_ = true;
    const float* from = reinterpret_cast<const float*>(ps.data());
    rows_.insert(rows_.end(), from, from + ps.size() * (sizeof(Particle) / sizeof(float)));
    liveParticles += static_cast<int>(ps.size());
  }
  if (liveParticles) device_->UpdateBuffer(buffer_, rows_.data(), static_cast<uint32_t>(rows_.size() * sizeof(float)));
  gatherMs_ = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void Particles::Draw(const View& view, gfx::Texture sceneDepth, float zNear) {
  // (lit smoke's pipelines made in the first frame it is on, smoke or not: made when the first smoke rose, they held
  // up that frame of a fight by a tenth of a second or so)
  if (litSmoke) LitReady();
  const auto t0 = std::chrono::steady_clock::now();
  if (!gathered_) Gather();  // (unless the smoke's shadow pass did it already this frame)
  gathered_ = false;
  if (!liveParticles) {
    cpuMs = cpuMs * 0.95f + updateMs_ * 0.05f;
    return;
  }
  DrawSets(view, 0, {}, sceneDepth, zNear);
  const float ms = updateMs_ + gatherMs_ + std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
  cpuMs = cpuMs == 0 ? ms : cpuMs * 0.95f + ms * 0.05f;
}

// Whether a set's particles cover what is behind them as smoke and dust do (lit smoke: lit as a volume; the smoke's
// shadow: they cast it): covering or darkening, not adding light or bending, and not lying flat (a ring on the
// water, the original's dark patch under an explosion, which is its shadow already).
bool Particles::Smoky(const EmitterBlueprint& bp, bool darkening) {
  const bool covers = bp.blendMode == 0 || bp.blendMode == 4 || (darkening && bp.blendMode == 1);
  return covers && !(bp.flat && !bp.alignRotation);
}

void Particles::DrawSmokeShadow(const View& sunView) {
  if (!gathered_) Gather();
  if (!liveParticles || !LitReady()) return;
  DrawSets(sunView, 2, {}, {}, 0);
}

void Particles::HotEmitters(Vec3 near, size_t most, std::vector<Hot>* out) {
  out->clear();
  if (!rampGlow) return;
  for (const Emitter& e : emitters_) {
    const EmitterBlueprint& bp = *e.blueprint;
    if (e.stopped || e.hidden || (bp.blendMode != 2 && bp.blendMode != 3) || bp.rampPath.empty()) continue;
    if (bp.lifetime > 0 && bp.lifetime <= 30) continue;  // (a short burst: an explosion's, which throws a flash)
    if (bp.warm < 0) {  // (the first time: is its glow's colour, its ramp's times its texture's, that of something hot?)
      const Vec3 ramp = rampGlow(EffectFile(*vfs_, bp.rampPath, remastered));
      const Vec3 hue = TextureGlow(*vfs_, EffectFile(*vfs_, bp.texturePath, remastered));
      const Vec3 c{ramp.x * hue.x, ramp.y * hue.y, ramp.z * hue.z};
      bp.warm = c.x > 0.04f && c.x >= c.z * 1.4f && c.x >= c.y * 0.9f ? 1 : 0;
      bp.warmColor = c;
    }
    if (!bp.warm) continue;
    const float size = bp.largestSize * e.scale;
    if (size < 0.15f) continue;  // (a spark of a lamp)
    out->push_back({e.position, size, bp.warmColor, e.id, Length(e.position - e.positionBefore) > 0.6f, bp.fire});
  }
  if (out->size() > most) {
    std::nth_element(out->begin(), out->begin() + static_cast<std::ptrdiff_t>(most), out->end(),
                     [near](const Hot& a, const Hot& b) { return Length(a.position - near) < Length(b.position - near); });
    out->resize(most);
  }
}

bool Particles::PrepareHeat(const View& view) {
  heatCount_ = 0;
  if (!HeatReady()) return false;
  std::vector<Hot> hot;
  HotEmitters(view.eye, 128, &hot);
  heatRows_.clear();
  const float perPixel = 2 * std::tan(view.fovY * 0.5f) / static_cast<float>(std::max(view.height, 1));  // (world units a pixel at distance 1)
  auto add = [&](Vec3 at, float size, float strength, float seed) {
    const float away = Length(at - view.eye);
    // (far off, or a few pixels across: nothing a player would see bend)
    if (away > 220 || size < away * perPixel * 6 || strength < 0.02f || heatCount_ >= 384) return;
    if (!view.BoxVisible(at - Vec3{size * 1.5f, size, size * 1.5f}, at + Vec3{size * 1.5f, size * 3, size * 1.5f})) return;
    heatRows_.insert(heatRows_.end(), {at.x, at.y, at.z, size, strength, seed, 0, 0});
    ++heatCount_;
  };
  // (the hot air over a fire about as large as its flames, at most 3 units: a burning wreck's glow particles are larger
  // than its flames, and a patch as large as they are bent most of a close camera's picture)
  for (const Hot& h : hot) add(h.position, std::clamp(h.size, 0.3f, 3.0f), h.moving ? 0.5f : 1.0f, static_cast<float>(h.id % 97));
  for (const Puff& p : puffs_) {
    const float age = static_cast<float>(now_ - p.born) / kPuffSeconds;
    if (age < 0 || age >= 1) continue;
    // (a fresh explosion's hot air: it swells and fades over a second and a half)
    add(p.position, std::clamp(p.radius * 0.3f, 0.4f, 3.0f) * (1 + 0.6f * age), 1.3f * (1 - age) * (1 - age), static_cast<float>(p.born * 13.0));
  }
  if (!heatCount_) return false;
  device_->UpdateBuffer(heatBuffer_, heatRows_.data(), static_cast<uint32_t>(heatRows_.size() * sizeof(float)));
  return true;
}

void Particles::DrawHeat(const View& view, gfx::Texture behind, gfx::Texture sceneDepth, float zNear) {
  if (!heatCount_ || !behind) return;
  device_->SetPipeline(heatPipeline_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  const float constants[4] = {heatBend, zNear, heatRise, 0};
  device_->SetConstants(1, constants, sizeof constants);
  device_->SetShaderBuffer(heatBuffer_);
  device_->SetTexture(2, behind);
  device_->SetTexture(3, sceneDepth);
  device_->Draw(6, 0, static_cast<uint32_t>(heatCount_));
}

void Particles::DrawBending(const View& view, gfx::Texture behind, gfx::Texture sceneDepth, float zNear) {
  if (bending_ && behind) DrawSets(view, 1, behind, sceneDepth, zNear);
}

// The sets of the last Draw: those that bend what is behind them (`pass` 1), or all the others (0), or (2) those
// that cast the smoke's shadow, into its map.
void Particles::DrawSets(const View& view, int pass, gfx::Texture behind, gfx::Texture sceneDepth, float zNear) {
  const bool bending = pass == 1, shadow = pass == 2;
  const bool litNow = pass == 0 && litSmoke && LitReady();
  const float now = static_cast<float>(now_);
  const std::vector<std::pair<size_t, size_t>>& spans = spans_;
  // the camera's sideways and upward directions, from the rows of the view-projection matrix
  const Mat4& m = view.constants.viewProj;
  const Vec3 right = Normalize(Vec3{m.m[0][0], m.m[0][1], m.m[0][2]}), up = Normalize(Vec3{m.m[1][0], m.m[1][1], m.m[1][2]});
  int bound = -1;
  for (size_t i = 0; i < order_.size(); ++i) {
    if (!spans[i].second) continue;
    const EmitterBlueprint& bp = *order_[i]->blueprint;
    if (shadow ? !Smoky(bp, true) : (bp.blendMode == 5) != bending) continue;
    // 0 alpha, 1 darkening, 2 adding, 3 premultiplied ("modulate 2x inverse" is drawn as adding);
    // 4: bending what is behind it, laid over by its alpha
    const int kind = bending ? 4 : bp.blendMode == 1 ? 1 : (bp.blendMode == 2 || bp.blendMode == 3) ? 2 : bp.blendMode == 4 ? 3 : 0;
    // (lit smoke: covering sets drawn through the lit pipelines, numbered after the others)
    // (and six-way maps' sets always, in either look: their colours are light maps, which only the lit shader reads)
    const bool sixWay = pass == 0 && bp.sixWay && (kind == 0 || kind == 3) && LitReady();
    const bool litSet = sixWay || (litNow && (kind == 0 || kind == 3) && Smoky(bp, false));
    const int pipeline = shadow ? 6 : litSet ? 4 + (kind == 3 ? 1 : 0) : kind == 4 ? 0 : kind;
    if (pipeline != bound) {
      device_->SetPipeline(shadow ? shadowPipeline_ : litSet ? litPipelines_[kind == 3 ? 1 : 0] : pipelines_[kind == 4 ? 0 : kind]);
      if (bending) device_->SetTexture(2, behind);
      if (!shadow) device_->SetTexture(3, sceneDepth);
      device_->SetConstants(0, &view.constants, sizeof view.constants);
      device_->SetShaderBuffer(buffer_);
      bound = pipeline;
    }
    // which of the original's vertex paths: Align, else AlignToBone (Flat with Flat), else Flat, else plain
    const float shape = bp.alignRotation ? 2.0f : bp.alignToBone ? (bp.flat ? 1.0f : 3.0f) : bp.flat ? 1.0f : 0.0f;
    const float constants[36] = {right.x, right.y, right.z, now,
                                 up.x, up.y, up.z, shape,
                                 static_cast<float>(spans[i].first), bp.frameCount > 1 || bp.stripCount > 1 ? 1.0f : 0.0f, static_cast<float>(kind), glow,
                                 1 / static_cast<float>(bp.frameCount), 1 / static_cast<float>(bp.stripCount), bp.resistance ? 1.0f : 0.0f, zNear,
                                 soften, 0, 0, 0,
                                 bp.gridAnimate ? 1.0f : 0.0f, bp.gridLoop ? 1.0f : 0.0f,
                                 static_cast<float>(bp.frameCount * bp.stripCount), bp.frameBlend ? 1.0f : 0.0f,
                                 bp.normalPath.empty() ? 0.0f : 1.0f, bp.refractionScale, bp.collideGround ? 1.0f : 0.0f, 0,
                                 // (lit smoke: lit as a volume; the smoke's shadow: how much it casts)
                                 litSet && litNow ? 1.0f : 0.0f, smokeDensity, smokeSmallLights, shadow ? 1.0f : 0.0f,
                                 // (six-way frames turned upright in the smoke's shadow too, so that it has their shape)
                                 sixWay || (shadow && bp.sixWay) ? 1.0f : 0.0f, bp.sixWayEmission, 0, 0};
    // (the unlit pipelines read the first 28)
    device_->SetConstants(1, constants, litSet || shadow ? sizeof constants : 28 * sizeof(float));
    device_->SetTexture(0, bp.texture);
    device_->SetTexture(1, bp.ramp);
    device_->SetTexture(4, bp.normals);
    if (sixWay) device_->SetTexture(5, bp.sixWay);
    device_->Draw(6, 0, static_cast<uint32_t>(spans[i].second));
  }
}

}  // namespace render
