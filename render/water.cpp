#include "water.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "lighting.h"

namespace render {
namespace {

// What the original's water computes per pixel, written anew:
//   normal   = the four scrolling wave normal maps added up, flattened where the map says so
//   under    = what is below the surface, tinted toward the surface colour by depth
//   result   = under blended toward the reflected sky by the fresnel term, plus the sun's glint,
//              plus white wave crests
// The original takes "what is below" from a copy of the picture and bends it by the normal (refraction).
// Here the sheet is blended over the picture instead, which gives the same colours without the bending:
//   result = below * (1 - a) + rgb, with rgb and a worked out from the formula above.
// Still missing (for the stage that matches the original's look): the refraction, reflected units, and
// the fresnel lookup texture the original uses (the term below is the usual bias-and-power form, an
// assumption until that texture is understood).
const char kShader[] = R"hlsl(
cbuffer WaterConstants : register(b1) {
  float4 mapSize;         // width, height, 1 / width, 1 / height
  float4 surfaceColor;    // rgb, w: elevation
  float4 lerpAndFresnel;  // colour lerp min, max, fresnel bias, fresnel power
  float4 reflection;      // unit reflection, sky reflection, sun shininess, sun glow
  float4 waterSunDirection;  // the direction the sun's light travels, as the map's water settings give it
  float4 waterSunColor;
  float4 normalRepeat;
  float4 movement[4];
};
Texture2D waterMap : register(t0);
Texture2D waves[4] : register(t1);
TextureCube sky : register(t5);

struct VOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
};

VOut VS(float2 corner : POSITION) {
  VOut o;
  o.world = float3(corner.x * mapSize.x, surfaceColor.w, corner.y * mapSize.y);
  o.position = mul(viewProj, float4(o.world, 1));
  return o;
}

float4 PS(VOut i) : SV_Target {
  float2 xz = i.world.xz;
  float4 water = waterMap.Sample(samplerLinearClamp, xz * mapSize.zw);  // r: waviness, g: depth, a: no crests
  float depth = water.g;
  float3 toPixel = normalize(i.world - cameraPosition);

  float4 sum = waves[0].Sample(samplerLinear, (xz + movement[0].xy * time) * normalRepeat.x)
             + waves[1].Sample(samplerLinear, (xz + movement[1].xy * time) * normalRepeat.y)
             + waves[2].Sample(samplerLinear, (xz + movement[2].xy * time) * normalRepeat.z)
             + waves[3].Sample(samplerLinear, (xz + movement[3].xy * time) * normalRepeat.w);
  float crest = saturate(sum.a - 1);
  float3 N = normalize((2 * sum.xyz - 4).xzy);
  N = lerp(float3(0, 1, 0), N, water.r);

  float3 R = reflect(toPixel, N);
  float3 skyColor = sky.Sample(samplerLinear, R).rgb;
  float facing = saturate(dot(-toPixel, N));
  float fresnel = saturate(lerpAndFresnel.z + (1 - lerpAndFresnel.z) * pow(1 - facing, lerpAndFresnel.w));
  float3 sun = pow(saturate(dot(-R, waterSunDirection.xyz)), reflection.z) * waterSunColor.rgb * fresnel;
  float daylight = 1;
  if (look > 0.5 && skyZenith.w > 0.5) {
    // the computed sky: it is what the water mirrors, and the glint is the falling light's. (The
    // water's own colours are the map's, made for daylight: all of it is dimmed by how bright the
    // day is in the last line, so the mirrored sky is first brought up by as much.)
    daylight = skyGlow.w * lerp(1, 0.45, skyHorizon.w);  // (and less by night: its day colours would glow)
    skyColor = pow(saturate(ComputedSky(R) / (daylight * lightMultiplier * modern.x * 0.55)), 1 / 2.2);
    sun = pow(saturate(dot(R, sunDirection)), reflection.z) * sunColor * fresnel;
  }

  float tint = clamp(depth, lerpAndFresnel.x, lerpAndFresnel.y);
  float k = saturate(reflection.y * saturate(depth * 10) * fresnel);
  float3 rgb = surfaceColor.rgb * tint * (1 - k) + skyColor * k + sun;
  float through = (1 - tint) * (1 - k);
  float c = (1 - water.a) * crest;
  rgb = rgb * (1 - c) + c;
  through *= 1 - c;
  // the modern look works in linear light: the same water for now, its colours converted
  if (look > 0.5) rgb = ThroughAirTo(pow(max(rgb, 0), 2.2) * (lightMultiplier * modern.x * 0.55 * daylight), i.world);
  return float4(rgb, 1 - through);
}
)hlsl";

// The modern look's water. Per pixel:
//   behind   = the picture as it was before the water (the bed, what stands in the water), read a
//              little to the side by the waves (refraction), and what the water takes from it on
//              the way: red first, then green, by how much water lies between
//   body     = the water's own colour where it is deep, by the light that falls on it
//   mirror   = the sky, and what the picture shows in the mirrored direction (land, units: found by
//              walking along that direction through the picture's depth), by the angle of view
//   foam     = where the water is shallow (the shore, and round whatever stands in it), and the
//              waves' crests
//   caustics = the bright net the waves throw on a shallow bed in sunlight
// It writes the finished colour (nothing is blended), and says per sample of the pixel whether the
// water is in front there, since the scene's depth can only be read here, not tested by the card.
const char kModernShader[] = R"hlsl(
cbuffer WaterConstants : register(b1) {
  float4 mapSize;         // width, height, 1 / width, 1 / height
  float4 surfaceColor;    // rgb, w: elevation
  float4 lerpAndFresnel;
  float4 reflection;
  float4 waterSunDirection;
  float4 waterSunColor;
  float4 normalRepeat;
  float4 movement[4];
  float4 cameraRight;     // w: tan(half the field of view)
  float4 cameraUp;        // w: aspect
  float4 cameraBack;      // w: near distance
};
Texture2D waterMap : register(t0);
Texture2D waves[4] : register(t1);
TextureCube sky : register(t5);
Texture2D behind : register(t6);
Texture2DMS<float> sceneDepth : register(t7);

static const float kRise = 0.07;  // how far the swell carries the water above and below its level

struct VOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
};

VOut VS(float2 corner : POSITION) {
  VOut o;
  // (the sheet lies as high as the swell's crests reach; where the water stands lower just now is
  // worked out per pixel)
  o.world = float3(corner.x * mapSize.x, surfaceColor.w + kRise, corner.y * mapSize.y);
  o.position = mul(viewProj, float4(o.world, 1));
  return o;
}

// The swell: four trains of waves crossing the water, longer ones faster. x: the height; y, z: how
// it slopes along x and z.
float3 Swell(float2 xz) {
  static const float4 trains[4] = {  // the way it runs (x, z), its length, its height
    float4(0.86, 0.50, 23.0, 0.060), float4(0.60, 0.80, 13.0, 0.040), float4(0.97, -0.24, 7.5, 0.022), float4(0.31, 0.95, 4.3, 0.012)};
  float3 sum = 0;
  for (int k = 0; k < 4; ++k) {
    float turns = 6.2832 / trains[k].z;
    float phase = dot(trains[k].xy, xz) * turns - time * 0.55 * sqrt(trains[k].z) * turns;
    sum.x += trains[k].w * sin(phase);
    sum.yz += trains[k].xy * (trains[k].w * turns * cos(phase));
  }
  return sum;
}

// Foam's lace: noise of four sizes, each turned against the last so that no grid shows, the fine
// ones drifting. 0..1, mostly about a half. grain: how much water one pixel covers (world units):
// a size of noise finer than two pixels is left at its mean, or from far up every pixel would draw
// its own value and the sea would be salted with white points (Chris, 2026-10-13).
float Lace(float2 q, float grain) {
  float2 a = float2(q.x * 0.80 - q.y * 0.60, q.x * 0.60 + q.y * 0.80);
  float2 b = float2(q.x * 0.28 + q.y * 0.96, -q.x * 0.96 + q.y * 0.28);
  float t = time * 0.12;
  float4 shown = saturate(1.5 - grain * float4(2.3, 5.9, 13.7, 31.0) * 2);
  float4 n = 0.5;
  if (shown.x > 0) n.x = CloudNoise(float3(q * 2.3, t));
  if (shown.y > 0) n.y = CloudNoise(float3(a * 5.9 + t, t * 1.7));
  if (shown.z > 0) n.z = CloudNoise(float3(b * 13.7 - t * 2, t * 2.3));
  if (shown.w > 0) n.w = CloudNoise(float3(q * 31.0 + t * 3, t * 3.1));
  return dot(lerp(0.5, n, shown), float4(0.40, 0.28, 0.20, 0.12));
}

float3 WorldAt(float2 pixel, float depth) {
  float away = cameraBack.w / max(depth, 1e-6);
  float2 ndc = float2(pixel.x * viewport.z * 2 - 1, 1 - pixel.y * viewport.w * 2);
  return cameraPosition + (cameraRight.xyz * (ndc.x * cameraRight.w * cameraUp.w) + cameraUp.xyz * (ndc.y * cameraRight.w) - cameraBack.xyz) * away;
}

float4 PS(VOut i, out uint coverage : SV_Coverage) : SV_Target {
  // where in the pixel the water is in front (a nearer thing has the larger depth)
  int2 pixel = int2(i.position.xy);
  coverage = 0;
  float depthBehind = 1;
  for (int s = 0; s < SAMPLES; ++s) {
    float d = sceneDepth.Load(pixel, s);
    if (d <= i.position.z) {
      coverage |= 1u << s;
      depthBehind = min(depthBehind, d);
    }
  }
  if (coverage == 0) discard;

  float2 xz = i.world.xz;
  float4 water = waterMap.Sample(samplerLinearClamp, xz * mapSize.zw);  // r: waviness, a: no crests
  float3 toPixel = normalize(i.world - cameraPosition);
  float distance = length(i.world - cameraPosition);
  float4 sum = waves[0].Sample(samplerLinear, (xz + movement[0].xy * time) * normalRepeat.x)
             + waves[1].Sample(samplerLinear, (xz + movement[1].xy * time) * normalRepeat.y)
             + waves[2].Sample(samplerLinear, (xz + movement[2].xy * time) * normalRepeat.z)
             + waves[3].Sample(samplerLinear, (xz + movement[3].xy * time) * normalRepeat.w);
  float crest = saturate(sum.a - 1) * (1 - water.a) * 0.25;
  float3 N = normalize((2 * sum.xyz - 4).xzy);
  N = normalize(lerp(float3(0, 1, 0), N, water.r));
  // the swell: it tilts the surface (more than its true slope: seen from above, the tilt is all
  // that shows of it), calmer where the map says the water is (lakes), higher when it rains
  float3 swell = Swell(xz) * ((0.35 + 0.65 * water.r) * (1 + 1.2 * falling.x));
  N = normalize(N + float3(-swell.y, 0, -swell.z) * 2.5);
  // near the eye: fine ripples, two sizes of them crossing, which the game's wave textures are too
  // coarse to show (Chris, 2026-10-13: "if you go up close it looks very pixelated"). Each size is
  // left out where it would be finer than about two pixels (grain: the water one pixel covers).
  float grain = max(length(ddx(xz)), length(ddy(xz)));
  float2 fine = saturate(1.5 - grain * float2(4.0, 9.0) * 2);
  [branch] if (fine.x > 0) {
    const float e = 0.06;
    float t = time * 0.5;
    float2 q = xz * 4.0 + float2(t, t * 0.7);
    float h = CloudNoise(float3(q, t));
    float2 tilt = float2(CloudNoise(float3(q + float2(e, 0), t)) - h, CloudNoise(float3(q + float2(0, e), t)) - h) * (fine.x * 0.13 / e);
    if (fine.y > 0) {
      // (turned against the first, so that no grid shows)
      float2 r = float2(xz.x * 0.8 - xz.y * 0.6, xz.x * 0.6 + xz.y * 0.8) * 9.0 - float2(t * 1.3, t);
      float g = CloudNoise(float3(r, t * 1.7));
      float2 turned = float2(CloudNoise(float3(r + float2(e, 0), t * 1.7)) - g, CloudNoise(float3(r + float2(0, e), t * 1.7)) - g) * (fine.y * 0.09 / e);
      tilt += float2(turned.x * 0.8 + turned.y * 0.6, -turned.x * 0.6 + turned.y * 0.8);
    }
    N = normalize(N + float3(-tilt.x, 0, -tilt.y) * (0.4 + 0.6 * water.r));
  }
  // rain: every small square of water has a drop falling into it again and again; its ring runs
  // outward, tilting the surface, and the whole surface is roughened
  float rings = 0;
  [branch] if (falling.x > 0 && distance < 220) {
    float2 g = xz * 2.2;
    float2 cell = floor(g);
    float h = CloudHash(float3(cell, 3.1));
    float age = frac(time * (0.8 + h) + h * 9.0);
    float2 from = g - (cell + 0.2 + 0.6 * float2(CloudHash(float3(cell, 6.7)), CloudHash(float3(cell, 11.3))));
    float away = length(from);
    float ring = saturate(1 - abs(away - age * 0.5) * 7) * (1 - age) * step(h, 0.08 + 0.3 * falling.x) * saturate(1 - distance / 220);
    N = normalize(N + float3(from.x, 0, from.y) * (ring * 0.9 / max(away, 0.05)));
    rings = ring;
    N = normalize(N + float3(CloudNoise(float3(xz * 9, time * 3)) - 0.5, 0, CloudNoise(float3(xz * 9 + 5, time * 3)) - 0.5) * (0.25 * falling.x));
  }

  // the light on the water
  float shadow = SunShadow(i.world, float3(0, 1, 0)) * CloudShadow(i.world);
  float3 sunLight = ToLinear(sunColor) * (lightMultiplier * modern.x);
  float3 skyLight = skyZenith.w > 0.5 ? SkyAmbient(float3(0, 1, 0)) : sunLight * 0.3;
  float3 light = skyLight + sunLight * (saturate(sunDirection.y) * 0.6 * shadow);
  // lamps and flashes near the water: their light on its body and foam, and their glint on its waves
  float3 lamps = SmallLights(i.world, N, -toPixel, float3(0.5, 0.5, 0.5), float3(0.04, 0.04, 0.04), 0.0016, 0.1, saturate(dot(N, -toPixel)) + 1e-4);

  // what is behind, straight on and bent by the waves; the bending is left out where it would
  // bring in something that stands in front of the water
  float3 bed = WorldAt(i.position.xy, depthBehind);
  // how high the water stands here just now, against the sheet: where the bed is higher, the
  // water has run back from it (the waterline moves up and down the shore with the swell)
  // (and unevenly, by the foam's own lace: a waterline is never a ruled line)
  float lace = Lace(xz + N.xz * 0.12, grain);
  float level = clamp(swell.x * 1.2 + (lace - 0.5) * 0.05, -kRise, kRise) - kRise;
  float deep = i.world.y + level - bed.y;
  if (deep <= 0) {
    coverage = 0;
    discard;
  }
  float2 uv = i.position.xy * viewport.zw;
  float2 bent = uv + N.xz * (0.035 * saturate(deep * 1.5) / (1 + distance * 0.02));
  float depthBent = sceneDepth.Load(int2(bent * viewport.xy), 0);
  if (depthBent > i.position.z || any(bent < 0) || any(bent > 1)) {
    bent = uv;
    depthBent = depthBehind;
  }
  float3 bedBent = WorldAt(bent * viewport.xy, depthBent);
  float through = min(length(bedBent - i.world), 80);
  float deepBent = max(i.world.y + level - bedBent.y, 0);
  float3 under = behind.SampleLevel(samplerLinearClamp, bent, 0).rgb;

  // the bright net on a shallow bed in sunlight
  float2 q = bedBent.xz;
  float n1 = CloudNoise(float3(q * 1.3 + time * 0.30, time * 0.25));
  float n2 = CloudNoise(float3(q * 1.3 - time * 0.25 + 7, time * 0.20 + 3));
  float net = pow(saturate(1 - abs(n1 - n2) * 5), 4);
  under *= 1 + net * 0.6 * saturate(deepBent * 3) * exp(-deepBent * 0.35) * shadow * saturate(sunDirection.y * 3);

  // what the water takes from it, and the water's own colour in its place. The numbers are the
  // depth colours Chris chose on the remastered Seton's (the visuals track's water ramp, put on
  // record in its reply, section 15): red is gone after about 3 of depth, green after 8, blue
  // after 10; the bed is hidden by 1 - exp(-depth / 7.5); the water itself goes from a turquoise
  // over the shallows to a deep navy
  float3 kept = exp(-through * float3(1 / 3.0, 1 / 8.0, 1 / 10.0));
  float hidden = 1 - exp(-through / 7.5);
  float3 own = lerp(float3(0.027, 0.672, 0.604), float3(0.001, 0.019, 0.058), saturate(1 - exp(-through / 5.0)));
  float3 color = lerp(under * kept, own * (light + lamps) * 0.6, hidden);

  // foam: at the shore and round what stands in the water. Thick at the waterline and thinning to
  // lace over the first half unit of depth; fronts of it run in toward the land, broken by the
  // same lace; and the waves' crests
  float edge = saturate(1 - deep / 0.5);
  // (how much of the lace is foam: nearly all of it only in the last hand's breadth of water)
  float much = edge * edge * edge * 0.6 + saturate(1 - deep / 0.05) * 0.3;
  // (open water, away from every shore: only wisps of it are left, soft at their edges, on the
  // swell's crests and near the eye; from far up the sea is one blue - Chris, 2026-10-13: "too
  // sparkly ... the white should blend in more")
  float open = saturate(1 - much * 8);
  float wisps = 0.3 * saturate(swell.x * 9 + 0.3) * saturate(1.2 - distance / 150);
  float foam = smoothstep(1.05 - much * 1.1 - 0.04 * open, 1.30 - much * 1.1 + 0.14 * open, lace + 0.42) * (0.55 + 0.45 * edge) * lerp(1, wisps, open);
  float fronts = pow(saturate(sin(deep * 20 - time * 1.5 + lace * 3.0) * 0.5 + 0.5), 5) * edge;
  foam = saturate(foam + fronts * smoothstep(0.42, 0.66, lace) * 0.55 + crest * saturate(swell.x * 14 + 0.25));
  color = lerp(color, (light + lamps) * 0.85, foam * 0.94);
  color += lamps * 0.35;
  color += light * (rings * 0.04);

  // the mirror: the sky, and what the picture shows that way
  float3 R = reflect(toPixel, N);
  R.y = max(R.y, 0.02);
  R = normalize(R);
  float3 mirror = skyZenith.w > 0.5 ? ComputedSky(R) : ToLinear(sky.Sample(samplerLinear, R).rgb) * (lightMultiplier * modern.x * 0.5);
  {
    float3 p = i.world;
    float stride = 1.2;
    for (int k = 0; k < 14; ++k) {
      p += R * stride;
      stride *= 1.35;
      float4 c = mul(viewProj, float4(p, 1));
      if (c.w <= 0) break;
      float2 at = float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
      if (any(at < 0) || any(at > 1)) break;
      float there = sceneDepth.Load(int2(at * viewport.xy), 0);
      float here = c.z / c.w;
      if (there > here) {  // the picture has something nearer than this place on the way: it is what is mirrored
        float gap = cameraBack.w / here - cameraBack.w / there;
        if (gap < stride * 2 + 1) {
          float2 edge = min(at, 1 - at);
          float seen = saturate(min(edge.x, edge.y) * 10) * (1 - k / 14.0);
          mirror = lerp(mirror, behind.SampleLevel(samplerLinearClamp, at, 0).rgb, seen);
        }
        break;
      }
    }
  }
  float facing = saturate(dot(-toPixel, N));
  float fresnel = 0.02 + 0.98 * pow(1 - facing, 5);
  color = lerp(color, mirror, fresnel * (1 - foam));
  // the sun's (or the moon's) glint
  color += sunLight * (pow(saturate(dot(R, sunDirection)), 500) * 0.9 * shadow * (1 - foam));
  // at its very edge the water thins out to nothing over what is behind it
  color = lerp(behind.SampleLevel(samplerLinearClamp, uv, 0).rgb, ThroughAirTo(color, i.world), saturate(deep / 0.035));
  return float4(color, 1);
}
)hlsl";

struct WaterConstants {
  float mapSize[4];
  float surfaceColor[4];
  float lerpAndFresnel[4];
  float reflection[4];
  float sunDirection[4];
  float sunColor[4];
  float normalRepeat[4];
  float movement[4][4];
  float cameraRight[4], cameraUp[4], cameraBack[4];  // (the modern water's)
};

}  // namespace

bool Water::Load(const MapData* map, std::string* error) {
  map_ = map;
  if (!map->water.present) return true;
  if (!pipeline_) {
    std::string source = std::string(kViewShaderBlock) + kShader;
    gfx::PipelineDesc pd;
    pd.name = "water";
    pd.vertexSource = pd.pixelSource = source;
    pd.attrs = {{"POSITION", 0, gfx::AttrType::Float2, 0}};
    pd.stride = 8;
    pd.cull = gfx::Cull::None;
    pd.depthWrite = false;
    pd.blend = gfx::Blend::Premultiplied;
    pd.colorFormat = kSceneFormat;
    pd.samples = gSceneSamples;
    pipeline_ = device_->CreatePipeline(pd, error);
    if (!pipeline_) return false;
    const float corners[12] = {0, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 1};
    gfx::BufferDesc bd;
    bd.stride = 8;
    bd.size = sizeof corners;
    bd.data = corners;
    vertices_ = device_->CreateBuffer(bd);
    // the modern look's water: only with several samples a pixel (it reads the scene's depth so)
    if (gSceneSamples > 1) {
      std::string modern = "#define SAMPLES " + std::to_string(gSceneSamples) + "\n" + kViewShaderBlock + kLightingShaderBlock + kModernShader;
      pd.name = "water, modern";
      pd.vertexSource = pd.pixelSource = modern;
      pd.blend = gfx::Blend::Opaque;
      pd.depthTest = false;
      pd.depthTarget = false;
      modern_ = device_->CreatePipeline(pd, error);
      if (!modern_) return false;
    }
  }
  waterMap_ = map->waterMap.empty() ? gfx::Texture{} : textures_->FromDds(map->waterMap, "the map's water map");
  if (!waterMap_) waterMap_ = textures_->White();
  for (int i = 0; i < 4; ++i) waves_[i] = textures_->Get(map->water.waves[i].texture, textures_->FlatNormal());
  sky_ = textures_->Get(map->water.cubemap, textures_->GreyCube());
  return true;
}

void Water::Draw(const View& view) { DrawWith(view, nullptr, {}, {}); }

void Water::DrawModern(const View& view, const Camera& camera, gfx::Texture behind, gfx::Texture sceneDepth) { DrawWith(view, &camera, behind, sceneDepth); }

void Water::DrawWith(const View& view, const Camera* camera, gfx::Texture behind, gfx::Texture sceneDepth) {
  if (!map_ || !map_->water.present || !pipeline_) return;
  const MapWater& w = map_->water;
  WaterConstants c = {};
  c.mapSize[0] = static_cast<float>(map_->width);
  c.mapSize[1] = static_cast<float>(map_->height);
  c.mapSize[2] = 1.0f / static_cast<float>(map_->width);
  c.mapSize[3] = 1.0f / static_cast<float>(map_->height);
  c.surfaceColor[0] = w.surfaceColor.x;
  c.surfaceColor[1] = w.surfaceColor.y;
  c.surfaceColor[2] = w.surfaceColor.z;
  c.surfaceColor[3] = w.elevation;
  c.lerpAndFresnel[0] = w.colorLerp.x;
  c.lerpAndFresnel[1] = w.colorLerp.y;
  c.lerpAndFresnel[2] = w.fresnelBias;
  c.lerpAndFresnel[3] = w.fresnelPower;
  c.reflection[0] = w.unitReflection;
  c.reflection[1] = w.skyReflection;
  c.reflection[2] = w.sunShininess;
  c.reflection[3] = w.sunGlow;
  Vec3 sun = Normalize(w.sunDirection);
  c.sunDirection[0] = sun.x;
  c.sunDirection[1] = sun.y;
  c.sunDirection[2] = sun.z;
  c.sunColor[0] = w.sunColor.x;
  c.sunColor[1] = w.sunColor.y;
  c.sunColor[2] = w.sunColor.z;
  for (int i = 0; i < 4; ++i) {
    c.normalRepeat[i] = w.normalRepeat[i];
    c.movement[i][0] = w.waves[i].movement.x;
    c.movement[i][1] = w.waves[i].movement.y;
  }
  const bool modern = camera && modern_ && behind && sceneDepth;
  if (modern) {
    Vec3 right, up, back;
    camera->Axes(&right, &up, &back);
    const float axes[12] = {right.x, right.y, right.z, std::tan(camera->fovY * 0.5f),
                            up.x, up.y, up.z, static_cast<float>(view.width) / static_cast<float>(std::max(view.height, 1)),
                            back.x, back.y, back.z, camera->zNear};
    std::memcpy(c.cameraRight, axes, sizeof axes);
  }
  device_->SetPipeline(modern ? modern_ : pipeline_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, &c, sizeof c);
  if (modern) {
    device_->SetTexture(6, behind);
    device_->SetTexture(7, sceneDepth);
  }
  device_->SetTexture(0, waterMap_);
  for (int i = 0; i < 4; ++i) device_->SetTexture(1 + i, waves_[i]);
  device_->SetTexture(5, sky_);
  device_->SetVertexBuffer(vertices_);
  device_->Draw(6);
}

}  // namespace render
