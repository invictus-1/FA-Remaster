#include "cloud_zoom.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace render {
namespace {

// One triangle over the picture: where something was drawn (the sky's pass lays the clouds over the sky itself),
// the clouds' picture over it (made at half the size: read between its pixels). The picture is rgb the clouds'
// light, a what of the scene behind comes through: laid as premultiplied light, scene x a + rgb.
const char kShader[] = R"hlsl(
Texture2D cloudPicture : register(t0);
#ifdef MSAA
Texture2DMS<float> sceneDepth : register(t1);
float DepthAt(int2 p) { return sceneDepth.Load(p, 0); }
#else
Texture2D<float> sceneDepth : register(t1);
float DepthAt(int2 p) { return sceneDepth.Load(int3(p, 0)); }
#endif

float4 VS(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 PS(float4 position : SV_Position) : SV_Target {
  if (DepthAt(int2(position.xy)) <= 0) discard;  // the sky: its pass has laid them
  float4 c = cloudPicture.SampleLevel(samplerLinearClamp, position.xy * viewport.zw, 0);
  return float4(c.rgb, 1 - c.a);
}
)hlsl";

// The zoom's march through the layer (MarchZoom; cloud_zoom.h, MakeCloudZoomMarch).
const char kMarchShader[] = R"hlsl(
// The layer stays where it is and the eye goes into it and above it. The view is marched through the layer wherever it
// lies before what was drawn, the ground too; the clouds are thinned as the eye rises above them and round the view's
// middle (`erode`, `keep`), and fade right at the eye. Put after the clouds' own shader (clouds.cpp): their noise,
// coverage, Remap and Phase, the scene's depth, the frames before and the picture written are theirs.
cbuffer CloudZoomMarch : register(b2) {
  float4 zoomLook;   // x eaten from the outer parts everywhere, y the density kept; z, w the same round the view's middle at the parting's strongest
  float4 zoomPart;   // x, y where the parting is whole and where it is gone (half heights off the middle); z the fine detail's bite, w its size against the detail's
  float4 zoomMore;   // x clouds nearer than this fade; y the camera's near distance (the depth's); z over how far a far way into the layer fades in; w width / height
  float4 zoomGain;   // x the density's gain; y how far the lumps raise and lower the tops (in the layer's thickness); z the powder's share; w 1 / the finer coverage's reach
  float4 zoomEye;    // xyz where the eye was the frame before; w how quickly the frame before is let go as the eye moves against the clouds' distance
  float4 zoomLight;  // x the least of the falling light that reaches deep inside (light scattered on and on), y how fast it fades with depth
};
Texture2D<float> coverFine : register(t5);  // the finer coverage picture round the camera (kCloudZoomCoverReach across)
// Where clouds are (as the coverage picture has it): from the finer picture where it reaches, else the coarse one.
float CoverNear(float2 xz) {
  float2 at = (xz - coverAt.xy) * zoomGain.w + 0.5;
  if (all(at >= 0) && all(at <= 1)) return coverFine.SampleLevel(samplerLinearClamp, at, 0);
  at = (xz - coverAt.xy) * cloudSize.z + 0.5;
  if (any(at < 0) || any(at > 1)) return 0;
  return coverPicture.SampleLevel(samplerLinearClamp, at, 0);
}

// How much cloud there is at a place, seen from close: as Cloud, but heaped in lumps (their tops rise and fall by the
// shapes' and the detail's Worley cells, so that from above they are heaps of billows and not a flat sheet), denser
// (crisper outlines) and, `frayed`, with a finer detail eating into the outer parts (billows on top, wisps below).
// Then thinned: the outer parts eaten by `erode` (the edges go first: the clouds break up and shrink rather than turn
// to glass), the rest kept by `keep`.
float CloudNear(float3 p, float h, bool frayed, float erode, float keep) {
  float cover = CoverNear(p.xz);
  if (cover <= 0.002) return 0;
  float3 wind = float3(skyClouds.y, skyClouds.w * 40, skyClouds.z);
  float4 s = shapeNoise.SampleLevel(samplerLinear, (p + wind) * cloudLook.z, 0);
  float3 e = detailNoise.SampleLevel(samplerLinear, (p + wind * 1.4) * cloudLook.w, 0).rgb;
  // (1 in the middle of a lump, its cells from about 150 world units across down to 25)
  float lumps = s.b * 0.35 + s.a * 0.3 + e.r * 0.2 + e.g * 0.15;
  // the top: as high as Cloud heaps it (the more cloud, the higher), raised and lowered by the lumps, and kept inside
  // the layer (above it the march does not go: it would be cut flat), going over to nothing in a tenth of the layer
  float rise = 0.15 + 0.75 * cover;
  float top = min(rise + (lumps - 0.5) * zoomGain.y - zoomGain.y * 0.2, 0.97);
  float profile = saturate(h * 12) * saturate((top - h) * 10);
  float low = s.g * 0.625 + s.b * 0.25 + s.a * 0.125;
  float shape = Remap(s.r, low - 1, 1, 0, 1) * profile;
  float d = saturate(Remap(shape, 1 - cover * 0.6, 1, 0, 1)) * cover;
  if (d <= 0) return 0;
  if (frayed) {  // (as Cloud frays them; then the finer detail, before the gain, while the outer parts are still graded)
    float fine = e.r * 0.625 + e.g * 0.25 + e.b * 0.125;
    d = saturate(Remap(d, lerp(fine, 1 - fine, saturate(h * 4)) * 0.6, 1, 0, 1));
    [branch] if (zoomPart.z > 0 && d > 0) {
      float3 f = detailNoise.SampleLevel(samplerLinear, (p + wind * 1.7) * (cloudLook.w / zoomPart.w), 0).rgb;
      float finer = f.r * 0.625 + f.g * 0.25 + f.b * 0.125;
      d = saturate(Remap(d, lerp(finer, 1 - finer, saturate(h * 4)) * zoomPart.z * 0.5, 1, 0, 1));
    }
  }
  d = saturate(d * zoomGain.x);
  return saturate(Remap(d, erode * 0.6, 1, 0, 1)) * keep;
}

[numthreads(8, 8, 1)] void MarchZoom(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= uint2(cloudSize.xy))) return;
  float2 uv = (id.xy + 0.5) / cloudSize.xy;
  float3 d = normalize(cloudAhead.xyz + cloudRight.xyz * (uv.x * 2 - 1) + cloudUp.xyz * (1 - uv.y * 2));
  float4 result = float4(0, 0, 0, 1);
  uint fullWidth, fullHeight;
#ifdef MSAA
  uint samples;
  sceneDepth.GetDimensions(fullWidth, fullHeight, samples);
#else
  sceneDepth.GetDimensions(fullWidth, fullHeight);
#endif
  int2 full = min(int2((id.xy + 0.5) * float2(fullWidth, fullHeight) / cloudSize.xy), int2(fullWidth, fullHeight) - 2);
  float farthest = min(min(DepthAt(full), DepthAt(full + int2(1, 0))), min(DepthAt(full + int2(0, 1)), DepthAt(full + int2(1, 1))));
  // how far what was drawn lies this way (the farthest of the four pixels; the sky: no end)
  float sceneAt = farthest > 0 ? zoomMore.y / farthest / max(dot(d, cloudAhead.xyz), 1e-4) : 1e9;
  float bottom = cloudRight.w, thick = cloudUp.w, top = bottom + thick;
  // the way through the layer: from the eye when it is inside, else from where the view enters it to where it leaves
  float t0 = 0, t1 = -1;
  if (abs(d.y) > 1e-5) {
    float a = (bottom - cameraPosition.y) / d.y, b = (top - cameraPosition.y) / d.y;
    t0 = max(min(a, b), 0);
    t1 = max(a, b);
  } else if (cameraPosition.y > bottom && cameraPosition.y < top) {
    t1 = 5000;
  }
  t1 = min(min(t1, t0 + 5000), sceneAt);
  float tAt = 0;  // where the clouds seen this way are, on the whole (0: none); for the frames before
  [branch] if (t1 > t0) {
    // thinned: everywhere as the eye is high over the layer, and more round the view's middle
    float off = length(float2((uv.x * 2 - 1) * zoomMore.w, 1 - uv.y * 2));
    float middle = 1 - smoothstep(zoomPart.x, zoomPart.y, off);
    float erode = 1 - (1 - zoomLook.x) * (1 - zoomLook.z * middle), keep = zoomLook.y * lerp(1, zoomLook.w, middle);
    int steps = int(cloudLook.y);
    float dt = (t1 - t0) / steps;
    float3 body = skyBody.w > 0.5 ? skyMoon.xyz : skyBody.xyz;
    float c = dot(d, body);
    float phase = lerp(Phase(c, -0.15), Phase(c, 0.65), 0.5);
    float3 around = skyZenith.rgb * 0.75 + skyHorizon.rgb * 0.45;
    float3 light = 0;
    float through = 1, tSum = 0, tWeight = 0;
    float t = t0 + dt * cloudAhead.w;
    const bool flashing = boltLight.r + boltLight.g + boltLight.b > 0;
    for (int k = 0; k < steps && through > 0.01; ++k, t += dt) {
      float3 p = cameraPosition + d * t;
      float h = saturate((p.y - bottom) / thick);
      float cloud = CloudNear(p, h, true, erode, keep) * smoothstep(0, zoomMore.x, t);
      if (cloud <= 0.002) continue;
      // the cloud between this place and the light: seen from close, the first steps short (1.5 world units, then
      // twice as long each), so that the lumps and billows shade each other
      float toward = 0, toLight = 0;
      for (int j = 0; j < 7; ++j) {
        float span = 1.5 * exp2(j);
        float3 q = p + body * (toLight + span * 0.5);
        toLight += span;
        toward += CloudNear(q, saturate((q.y - bottom) / thick), false, erode, keep) * span;
      }
      float depthToLight = toward * cloudLook.x;
      float lit = max(exp(-depthToLight), zoomLight.x * exp(-depthToLight * zoomLight.y));
      // (thin outer parts, seen away from the light, scatter little of it toward the eye: the billows' edges darker)
      float powder = lerp(1, 1 - exp(-cloud * cloudLook.x * 40), zoomGain.z * saturate(0.5 - 0.5 * c));
      float3 scattered = cloudLight.rgb * (lit * phase * 1.1 * powder) + around * (0.12 + 0.55 * h);
      if (flashing) {
        float far = length(p - cloudBolt.xyz) / cloudBolt.w;
        scattered += boltLight.rgb * (exp(-far * 1.5) / (1 + far * far));
      }
      float sigma = cloud * cloudLook.x;
      float stepThrough = exp(-sigma * dt);
      float took = through * (1 - stepThrough);
      light += scattered * took;
      tSum += t * took;
      tWeight += took;
      through *= stepThrough;
    }
    // a far way into the layer, low toward the horizon, goes over into the haze as without the zoom; a near one is all there
    float fade = max(smoothstep(0.012, 0.12, abs(d.y)), saturate(1 - t0 / max(zoomMore.z, 1e-3)));
    tAt = tWeight > 1e-4 ? tSum / tWeight : (t0 + t1) * 0.5;
    // seen through the air on the way, as the ground under them is (ThroughAir)
    if (weather.z > 0 && tWeight > 1e-4) light = lerp(light, AirColor() * (1 - through), min(1 - exp(-tAt * weather.z), weather.w));
    result = float4(light * fade, lerp(1, through, fade));
  }
  // built up over frames: what the same clouds showed the frame before, found by where they are (the eye moves
  // through them now: found by their direction alone they would smear as it rises). Less of it the farther the eye
  // went against how far the clouds are: as it rises through them they change fast, and a frame before kept long
  // lags behind them (the edge of each frame's view then shows, carried inward frame by frame as the view widens).
  float keepNow = cloudSize.w * saturate(1 - length(cameraPosition - zoomEye.xyz) / max(tAt > 0 ? tAt : 1e4, 1) * zoomEye.w);
  [branch] if (keepNow > 0) {
    float4 clip = tAt > 0 ? mul(lastViewProj, float4(cameraPosition + d * tAt, 1)) : mul(lastViewProj, float4(d, 0));
    if (clip.w > 1e-4) {
      float2 lastUv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
      if (all(lastUv >= 0) && all(lastUv <= 1)) result = lerp(result, lastClouds.SampleLevel(samplerLinearClamp, lastUv, 0), keepNow);
    }
  }
  cloudsOut[id.xy] = result;
}
)hlsl";

float Smooth(float x) {
  x = std::clamp(x, 0.0f, 1.0f);
  return x * x * (3 - 2 * x);
}

}  // namespace

Clouds::Zoom CloudZoom::For(const Camera& camera, const Clouds& clouds, bool on) const {
  Clouds::Zoom z;
  z.on = on && !camera.cinematic;
  if (!z.on) return z;
  const float eye = camera.Eye().y, under = clouds.base, top = clouds.base + std::max(clouds.thickness, 1.0f);
  // seen from close (crisper, finer): coming in as the eye comes up to the layer, from where the march goes over to
  // the zoom's (40 under it), so that the sky's clouds do not change at once
  const float close = Smooth((eye - (under - 40)) / 40);
  z.gain = 1 + (gain - 1) * close;
  z.fine = fine * close;
  z.lumps = lumps * close;
  z.powder = powder * close;
  z.fineSize = fineSize;
  // thinned by the eye's height over the layer's top, smoothly over the band
  const float thin = Smooth((eye - top) / std::max(band, 1.0f));
  z.erode = erodeMost * thin;
  z.keep = 1 + (keepMost - 1) * thin;
  // parted round the view's middle from `partDepth` under the layer's top, whole at the top (or over 60 units)
  const float part = Smooth((eye - (top - partDepth)) / std::max(partDepth, 60.0f));
  z.partErode = partErode * part;
  z.partKeep = 1 + (partKeep - 1) * part;
  z.partFrom = partFrom;
  z.partTo = partTo;
  z.near = near;
  z.history = history;
  z.letGo = letGo;
  // the light deep inside them brighter only while the eye is in the layer (what it sees then is their insides; from
  // above, the shaded sides of their heaps keep their depth)
  const float within = close * (1 - Smooth((eye - top) / 100));
  z.inside = 0.3f + (inside - 0.3f) * within;  // (without the zoom: 0.3, 0.25)
  z.insideFade = 0.25f + (insideFade - 0.25f) * within;
  return z;
}

bool CloudZoom::Set(const std::string& list, std::string* why) {
  struct Named {
    const char* name;
    float* value;
  } named[] = {{"band", &band}, {"erode", &erodeMost}, {"keep", &keepMost}, {"partErode", &partErode}, {"partKeep", &partKeep},
               {"partDepth", &partDepth}, {"partFrom", &partFrom}, {"partTo", &partTo}, {"near", &near}, {"gain", &gain}, {"lumps", &lumps}, {"powder", &powder}, {"fine", &fine}, {"fineSize", &fineSize}, {"history", &history}, {"letGo", &letGo}, {"inside", &inside}, {"insideFade", &insideFade},
               {"lift", &lift}, {"liftMost", &liftMost}};  // (sky: lift, liftMost)
  size_t at = 0;
  while (at < list.size()) {
    size_t end = list.find(',', at);
    if (end == std::string::npos) end = list.size();
    const std::string item = list.substr(at, end - at);
    at = end + 1;
    const size_t eq = item.find('=');
    bool found = false;
    for (const Named& n : named)
      if (eq != std::string::npos && item.compare(0, eq, n.name) == 0 && eq == std::strlen(n.name)) {
        *n.value = static_cast<float>(std::atof(item.c_str() + eq + 1));
        found = true;
      }
    if (!found) {
      *why = "not a name=value of band, erode, keep, partErode, partKeep, partDepth, partFrom, partTo, near, gain, lumps, powder, fine, fineSize, history, letGo, inside, insideFade, lift, liftMost: " + item;
      return false;
    }
  }
  return true;
}

void CloudZoom::Place(Clouds* clouds, bool on, float mapWidth, float mapHeight) {
  if (ownBase_ < 0) ownBase_ = clouds->base;
  clouds->base = on ? std::clamp(lift * std::max(mapWidth, mapHeight), ownBase_, std::max(liftMost, ownBase_)) : ownBase_;
}

void CloudZoom::Lay(const View& view, gfx::Texture clouds, gfx::Texture sceneDepth) {
  if (!clouds || !sceneDepth) return;
  PrepareAhead();
  if (!pipeline_) return;
  device_->SetPipeline(pipeline_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetTexture(0, clouds);
  device_->SetTexture(1, sceneDepth);
  device_->Draw(3);
  device_->SetTexture(0, {});
  device_->SetTexture(1, {});
}

void CloudZoom::PrepareAhead() {
  if (!tried_) {  // (made the first time it is wanted, or ahead: a small pass)
    tried_ = true;
    const std::string source = std::string(gSceneSamples > 1 ? "#define MSAA 1\n" : "") + kViewShaderBlock + kShader;
    gfx::PipelineDesc pd;
    pd.name = "clouds laid over the scene (cloud zoom)";
    pd.vertexSource = pd.pixelSource = source;
    pd.cull = gfx::Cull::None;
    pd.depthTest = pd.depthWrite = false;
    pd.depthTarget = false;
    pd.blend = gfx::Blend::Premultiplied;
    pd.colorFormat = kSceneFormat;
    pd.samples = gSceneSamples;
    pd.alphaWrite = false;  // (the scene's alpha holds the original look's glow)
    std::string error;
    pipeline_ = device_->CreatePipeline(pd, &error);
    if (!pipeline_) fprintf(stderr, "render: cloud zoom: %s\n", error.c_str());
  }
}

gfx::Pipeline MakeCloudZoomMarch(gfx::Device* device, const std::string& cloudsSource) {
  const std::string source = cloudsSource + kMarchShader;
  gfx::ComputeDesc cd;
  cd.source = source;
  cd.name = "clouds: marched, zooming through them";
  cd.entry = "MarchZoom";
  std::string error;
  gfx::Pipeline p = device->CreateCompute(cd, &error);
  if (!p) fprintf(stderr, "render: cloud zoom: its march cannot be made (%s)\n", error.c_str());
  return p;
}

CloudZoomMarchNumbers CloudZoomNumbers(const Clouds::Zoom& zoom, float base, const Camera& camera, Vec3 lastEye, int width, int height) {
  CloudZoomMarchNumbers n = {};
  const float eyeY = camera.Eye().y;
  const float look[4] = {std::clamp(zoom.erode, 0.0f, 1.0f), std::clamp(zoom.keep, 0.0f, 1.0f), std::clamp(zoom.partErode, 0.0f, 1.0f),
                         std::clamp(zoom.partKeep, 0.0f, 1.0f)};
  const float part[4] = {zoom.partFrom, std::max(zoom.partTo, zoom.partFrom + 0.01f), std::max(zoom.fine, 0.0f), std::max(zoom.fineSize, 0.01f)};
  // (a far way into the layer fades in over a distance that grows from nothing as the eye comes up to 40 under it,
  // so the picture goes over from the march without the zoom smoothly)
  const float more[4] = {std::max(zoom.near, 0.01f), camera.zNear, (eyeY - (base - 40)) * 30, static_cast<float>(width) / static_cast<float>(std::max(height, 1))};
  const float gain[4] = {std::max(zoom.gain, 0.0f), std::max(zoom.lumps, 0.0f), std::clamp(zoom.powder, 0.0f, 1.0f), 1 / kCloudZoomCoverReach};
  const float eye[4] = {lastEye.x, lastEye.y, lastEye.z, std::max(zoom.letGo, 0.0f)};
  const float light[4] = {std::clamp(zoom.inside, 0.0f, 1.0f), std::max(zoom.insideFade, 0.0f), 0, 0};
  std::memcpy(n.look, look, sizeof look);
  std::memcpy(n.part, part, sizeof part);
  std::memcpy(n.more, more, sizeof more);
  std::memcpy(n.gain, gain, sizeof gain);
  std::memcpy(n.eye, eye, sizeof eye);
  std::memcpy(n.light, light, sizeof light);
  return n;
}

bool ZoomPath::Parse(const std::string& text, std::string* why) {
  keys.clear();
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find(';', at);
    if (end == std::string::npos) end = text.size();
    const std::string item = text.substr(at, end - at);
    at = end + 1;
    if (item.empty()) continue;
    const size_t colon = item.find(':');
    if (colon == std::string::npos) {
      *why = "a key without its time (t:zoom): " + item;
      return false;
    }
    Key k;
    k.time = static_cast<float>(atof(item.substr(0, colon).c_str()));
    const std::string zoom = item.substr(colon + 1);
    k.zoom = zoom == "max" ? 0.0f : static_cast<float>(atof(zoom.c_str()));
    if (zoom != "max" && k.zoom <= 0) {
      *why = "a zoom that is not a number above 0 or \"max\": " + item;
      return false;
    }
    if (!keys.empty() && k.time < keys.back().time) {
      *why = "the keys' times go back: " + item;
      return false;
    }
    keys.push_back(k);
  }
  if (keys.empty()) *why = "no keys";
  return !keys.empty();
}

void ZoomPath::Apply(float time, float mapWidth, float mapHeight, float aspect, Camera* camera) const {
  if (keys.empty()) return;
  // (the original's camera, as fa-remaster-ui's worldview.cpp has it: cam_NearZoom 5, the farthest where the map
  // takes 1 / 1.4 of the view, cam_NearPitch 40 and cam_FarPitch 89.9 degrees, cam_NearFOV 65 and cam_FarFOV 60)
  constexpr float kNearZoom = 5, kFarthestShare = 1.4f, kNearPitch = 40, kFarPitch = 89.9f, kNearFov = 65, kFarFov = 60;
  constexpr float kDegrees = 0.0174533f;
  const float farthest = std::max({(mapWidth + 4) * kFarthestShare, (mapHeight + 4) * kFarthestShare * aspect, kNearZoom * 2});
  auto zoomOf = [&](const Key& k) { return std::clamp(k.zoom > 0 ? k.zoom : farthest, kNearZoom, farthest); };
  float zoom = zoomOf(keys.front());
  if (time >= keys.back().time) {
    zoom = zoomOf(keys.back());
  } else if (time > keys.front().time) {
    size_t i = 1;
    while (i + 1 < keys.size() && keys[i].time <= time) ++i;
    const Key& a = keys[i - 1];
    const Key& b = keys[i];
    const float f = Smooth(b.time > a.time ? (time - a.time) / (b.time - a.time) : 1.0f);
    zoom = std::exp(std::log(zoomOf(a)) + (std::log(zoomOf(b)) - std::log(zoomOf(a))) * f);
  }
  const float blend = std::clamp((std::log(zoom) - std::log(kNearZoom)) / (std::log(farthest) - std::log(kNearZoom)), 0.0f, 1.0f);
  const float half = std::tan((kNearFov + blend * (kFarFov - kNearFov)) * kDegrees * 0.5f);
  camera->pitch = (kNearPitch + blend * (kFarPitch - kNearPitch)) * kDegrees;
  camera->distance = zoom / (2 * half);
  camera->fovY = 2 * std::atan(aspect > 1 ? half / aspect : half);
}

}  // namespace render
