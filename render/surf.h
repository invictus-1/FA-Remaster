// Surf and sea spray (World::surf; the modern look; Chris approved it 2026-10-18, the visuals session's proposal 3:
// "waves breaking on the rocky shores, spray off ships' bows and on the rocks in storms"). To be seen only: nothing of
// it goes back to the game.
//
//  - Breaking waves on the map's shores (the water's shader, water.cpp built with SURF, reads kSurfShaderBlock): trains
//    of waves run in toward every shore, their crests along it (by the distance to the waterline), growing, shortening
//    and slowing as the water shallows. Where the water is shallow enough for a wave's height it breaks: its face and
//    crest go white and a trail of foam is left behind it as the white water runs on to the shore, where it runs up
//    the beach (the water's edge carried up the slope: the water's level is raised there) and drains back. On steep
//    rock a wave breaks at its foot in a burst of white that boils and drains away. How high the waves are: by how
//    rough the sea is (the weather's wind: a still day laps, a storm pounds), by how open the water in front of the
//    shore is (none on ponds or in narrow inlets) and wave by wave (they come in sets).
//  - Spray: where waves hit rock (steep ground at the waterline), a burst of spray thrown up the rock as each wave
//    arrives, more and higher the rougher the sea, a mist drifting off with the wind; and ships at speed throw spray
//    off their bows (by their speed and how rough the sea is; in a rough sea a burst as the bow meets a wave). Puffs lit
//    as the lit smoke is (the sun through the shadows, the sky round about, the small lights), seen through the air.
//
// Where the shores are is read once from the map's heights, the first frame the option is on (MakeField): a field over
// the map of the signed distance to the waterline, the water's depth, how steep the nearest shore is and how open the
// water before it is (the share of open water round about), handed to the water's shader as a texture (t9 of the
// water's pass, kSurfFieldSlot); and the places on rocky shores that throw spray. The map's own shores only: past the
// map's edge the surf fades out (the sea beyond the edge is the horizon's).
//
// The waves' timing is the same on the card and on the processor (SurfOffset and SurfStrength below, Surf::Offset and
// Surf::Strength; the phase handed over each frame), so a burst of spray goes up as the wave the water shows meets the
// rock. Readable: the spray is thin and low, and the surf's white water fades where something stands in it (a unit
// wading ashore keeps its outline).
//
// Off: nothing is made or done, and the water's shader is built without SURF (the same source as before).
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "bp_reader.h"
#include "frame.h"
#include "gfx.h"
#include "linalg.h"
#include "view.h"

namespace render {

struct MapData;
struct Model;

// The water's pass binds its own textures t0 to t8 (water.cpp); the shore field is the next of its own, t9. (Not a new
// shared slot: both backends have 44, t0 to t43, and in the Vulkan backend a t44 would take the binding of b0.)
inline constexpr int kSurfFieldSlot = 9;

// HLSL: the water's modern shader takes it when built with SURF (after the view's and the lighting's blocks, before
// its own code). The water's constants it needs are handed to SurfAt (water.cpp: surfArea, surfLook, surfLook2; the
// check view's surfLook3 to SurfView).
inline const char kSurfShaderBlock[] = R"hlsl(
// The shore field (Surf::MakeField): r the signed distance to the waterline (world units; over the water +, over land
// -); g the water's depth by the map's heights; b how steep the nearest shore is (0 a beach .. 1 rock); a how open the
// water before that shore is (0 a pond or an inlet .. 1 the open sea).
Texture2D surfField : register(t9);  // (kSurfFieldSlot)

// The waves' timing, the same as the processor's (Surf::Offset, Surf::Strength): keep the two alike.
// How far ahead or behind its neighbours along the shore a place's waves are (cycles): slowly over the map, so that
// the crests do not follow the shore exactly and do not arrive everywhere at once.
float SurfOffset(float2 xz) {
  return 0.55 * sin(dot(xz, float2(0.0211, 0.0137))) + 0.35 * sin(dot(xz, float2(-0.0173, 0.0291)) + 1.7) +
         0.20 * sin(dot(xz, float2(0.047, -0.031)) + 4.1);
}
// How big one wave is (0.3 .. 1): `n` which crest (counted as the phase is), at a place; bigger ones come in sets.
float SurfStrength(float n, float2 xz) {
  float s = 0.5 + 0.5 * sin(n * 0.897 + dot(xz, float2(0.013, 0.009)));
  float t = 0.5 + 0.5 * sin(n * 2.31 + dot(xz, float2(-0.029, 0.041)) + 2.0);
  return 0.3 + 0.7 * (0.65 * s * s + 0.35 * t);
}

// The surf at a place on the water (or on the beach just above it).
struct SurfHere {
  float lift;    // how far the water stands over its level here now (world units): the waves near the shore, their run-up
  float2 tilt;   // what the waves' slopes add to the surface's normal (x, z)
  float white;   // how much white water there is (0..1), before the foam's lace breaks it up
  float ground;  // the height of the map's ground here (for telling something that stands in the water)
  float shown;   // how much of the surf there is here at all (0: none, the water as without it)
};

static const float kSurfRise = 0.45;  // how much higher the water's sheet lies with the surf on (its run-up comes so high)
static const float kSurfFar = 30;      // the waves' trains begin this far out from the shore (world units) ...
static const float kSurfWarp = 1.3;    // ... and are this much shorter again at the shore, shortening over ...
static const float kSurfWarpReach = 8; // ... about this far

// `xz` where the line of sight meets the water at its level; `grain` how much water one pixel covers (world units);
// `level` the water's level; `mapSize` the map's width and height; `area`: the field's first texel's corner (world x,
// z) and 1 / its width and height (world units); `look`: x the waves' height near the shore, y their phase (cycles),
// z their length out at sea, w the run-up's height; `look2`: x how rough the sea is (0..1), y the depth a wave breaks
// at (a share of its height), z the share of a wave's period its run-up lasts, w how much white water (1 as tuned).
SurfHere SurfAt(float2 xz, float grain, float level, float2 mapSize, float4 area, float4 look, float4 look2) {
  SurfHere s = (SurfHere)0;
  float2 uv = (xz - area.xy) * area.zw;
  float4 f = surfField.SampleLevel(samplerLinearClamp, uv, 0);
  s.ground = level - f.g;
  // (on the map's own shores: fading out past its edge)
  float2 inside = min(xz, mapSize.xy - xz);
  float open = f.a * saturate(1 + min(inside.x, inside.y) / 16);
  float d = f.r;
  s.shown = open * saturate((kSurfFar - d) / 10) * saturate((d + 6) / 2);
  [branch] if (s.shown <= 0) return s;
  float steep = f.b;
  float rough = look2.x;
  // which way the sea lies (the distance's slope, from the field's neighbours a world unit off)
  float2 seaward = float2(surfField.SampleLevel(samplerLinearClamp, uv + float2(area.z, 0), 0).r,
                          surfField.SampleLevel(samplerLinearClamp, uv + float2(0, area.w), 0).r) - d;
  seaward *= 1 / max(length(seaward), 1e-4);

  // the waves running in: their phase (cycles) by the distance, made shorter toward the shore (warped), the clock's
  // phase and the place's offset; n counts the crests, w goes from 0 at crest n to 1 at crest n + 1, the next out to sea
  float dd = max(d, 0);
  float near = exp(-dd / kSurfWarpReach);
  float warped = dd + kSurfWarp * kSurfWarpReach * (1 - near);
  float perUnit = (1 + kSurfWarp * near) / look.z;  // cycles per world unit here
  float c = warped / look.z + look.y + SurfOffset(xz);
  // (the crests bent a little more, at a smaller scale, away from the shore: at the waterline the waves arrive as the
  // processor has them, for the spray)
  c += 0.36 * (CloudNoise(float3(xz * 0.045, 1.3)) - 0.5) * saturate(dd / 4);
  float n = floor(c), w = c - n;
  // a wave's profile: out at sea a long, even swell; toward the shore a steep face (the last share fw of the cycle
  // before crest n + 1) and a long back behind crest n, each rounded into the crest and the trough (no crease, which the
  // light would draw as a ruled line); h 0 .. 1 at the crest, dh its slope per cycle
  float fw = lerp(0.5, lerp(0.28, 0.16, steep), saturate(1.25 - dd / 12));
  bool face = w > 1 - fw;
  float u = face ? (w - (1 - fw)) / fw : w / (1 - fw);
  float h = face ? u * u * (3 - 2 * u) : 1 - u * u * (3 - 2 * u);
  float dh = (face ? 6 : -6) * u * (1 - u) / (face ? fw : 1 - fw);
  float S = SurfStrength(face ? n + 1 : n, xz);
  // its height: the sea's (look.x) by how open, growing as it shoals; where the water is shallow enough for it, it
  // breaks, and carries on lower as white water
  float crest = look.x * open * S * (1 + 0.6 * exp(-dd / 6));
  float breakAt = crest * look2.y + 0.03;  // (the depth it breaks at)
  float broken = smoothstep(breakAt * 1.6, breakAt * 0.7, f.g);
  // (and, whatever the depth, over a band along the shore that widens the rougher the sea: Seton's shores are steep, and
  // the surf of a storm should read from the game's height; narrower on rock, where the water deepens at once)
  float zone = (0.8 + 7 * rough * rough) * open * lerp(1, 0.55, steep) * (0.65 + 0.7 * CloudNoise(float3(xz * 0.06, 2.9)));
  broken = max(broken, smoothstep(zone * 1.25, zone * 0.6, d));
  float high = crest * lerp(1, 0.55, broken);
  // (fine waves that a pixel cannot show: their mean, a level sea, and no lines of white that would flicker)
  float lines = saturate(1 / (perUnit * max(grain, 1e-4)) * 0.25 - 0.5);
  s.lift = high * (h - 0.3) * lines;
  // (the tilt overdone near the shore, as the swell's: from above it is what shows of them; gentler out at sea, where
  // the swell's own is enough)
  s.tilt = -seaward * (high * dh * perUnit * lerp(0.8, 2.5, exp(-dd / 8)) * lines);
  // white water: a broken wave's face is white from its foot up (the leading edge of the white water running in), and
  // behind its crest a trail of foam thins out into streaks along the crests, lying where they were left (slowly
  // changing); between the waves a little old foam lies, the more the rougher the sea
  // (the streaks: a noise fine across the shore, by the distance from it, and coarse along it)
  float streakShown = saturate(1.5 - grain * 6);  // (streaks finer than about two pixels: their mean)
  float streaks = lerp(0.5, CloudNoise(float3(xz * 0.6, d * 3.2 + look.y * 0.4)), streakShown);
  float faceWhite = face ? smoothstep(1 - fw, 1 - fw * 0.45, w) : 0;
  float trail = face ? 0 : exp(-w * lerp(8, 4.5, rough)) * (0.25 + 1.2 * streaks);
  // (each wave breaks along stretches of its crest, not all of it: by a noise of its own along the shore)
  float crestN = face ? n + 1 : n;
  float stretches = saturate(CloudNoise(float3(xz * 0.15, crestN * 0.71)) * 2.4 - 0.5);
  float whiteLines = broken * saturate(S * 1.5) * max(faceWhite, 0.85 * trail) * lerp(1, stretches, 0.75 * saturate(d / 2));
  float white = (lerp(broken * 0.25, whiteLines, lines) + broken * (0.05 + 0.2 * rough) * streaks) * (0.45 + 0.55 * rough);

  // at the shore: the wave's water runs up the beach and drains back (n0, w0: the phase of the waves arriving here);
  // quickly up, slowly down; less far up steep rock, where it surges and boils instead
  float c0 = look.y + SurfOffset(xz);
  float n0 = floor(c0), w0 = c0 - n0;
  float S0 = SurfStrength(n0, xz);
  float tau = w0 / look2.z;
  float run = tau < 1 ? sin(3.14159 * pow(tau, 0.7)) : 0;
  float runUp = look.w * open * S0 * run * lerp(1, 0.55, steep);
  float shore = saturate(1 - d / 2.5);
  s.lift = lerp(s.lift, runUp, shore) * saturate((d + 6) / 2);
  // the sheet running up is foam at first, clearing as it drains (the water's own foam whitens its edge)
  white = max(white, shore * saturate(1 - d / 1.2) * saturate(S0 * 1.5) * open * run * lerp(0.8, 0.25, saturate(tau * 1.6)));
  // on rock: a burst of white where the wave strikes, boiling at the foot of it and draining away
  float strike = steep * S0 * open * (exp(-w0 * 6) * saturate(1 - d / 2) + 0.35 * exp(-w0 * 1.5) * saturate(1 - d / 4));
  white = max(white, strike * (0.5 + 0.5 * rough));
  s.white = saturate(white) * look2.w;
  s.lift = clamp(s.lift, -0.2, kSurfRise);
  return s;
}

// A view for checking (Surf::view): 1 the field, 2 the waves.
float3 SurfView(float2 xz, SurfHere s, float4 area, float which) {
  float4 f = surfField.SampleLevel(samplerLinearClamp, (xz - area.xy) * area.zw, 0);
  if (which < 1.5) return float3(f.b, f.a, f.r > 0 && f.r < 30 && frac(f.r / 8) < 0.5 ? 1 : 0.3);
  return float3(saturate(s.lift * 8), s.white, saturate(-s.lift * 8)) * (0.25 + 0.75 * s.shown);
}

// The surf's foam at a place: its white water broken up by the foam's lace (water.cpp, Lace), as the water's own foam is.
float SurfFoam(SurfHere s, float lace) {
  return smoothstep(1.02 - s.white * 1.1, 1.22 - s.white * 1.1, lace + 0.42) * saturate(s.white * 2.5) * s.shown;
}
)hlsl";

class Surf {
 public:
  explicit Surf(gfx::Device* device) : device_(device) {}
  ~Surf();

  // The map whose shores it is (Water::Load); its field is worked out the first frame the option is on.
  void SetMap(const MapData* map);
  // A unit's blueprint by its id ("ues0103"), read once for each kind of unit (World sets it): which units are ships
  // and how fast they go.
  std::function<bool(const std::string&, BpValue*)> readBlueprint;

  // --- each frame ---
  // At the frame's start (World::Begin): `time` the water's clock (seconds; its waves move on while a game is paused, as
  // the water's do); `on` the option and what it needs (the modern look, a map with water); `side` a frame drawn for
  // something else (the minimap): no surf in it, and nothing moves on; `rough` how rough the sea is (0 still .. 1 a
  // storm: the weather's wind); `wind` the wind near the ground (world units a second, x and z: the spray drifts with
  // it); `eye` where the camera is.
  void Begin(float time, bool on, bool side, float rough, const float wind[2], Vec3 eye);
  // A unit drawn this frame (alive, built, seen): a ship at speed throws spray off its bow.
  void AddShip(uint32_t id, const std::string& blueprint, const Model* model, Vec3 position, const Quat& orientation);
  // Before the scene's pass: spray thrown on the rocks where a wave arrives now, what is over let go of, the rest
  // handed to the card (its shader made the first time).
  void Prepare(const View& view);
  // The spray, into the scene's colour alone (it tests the scene's depth itself): after the water, before the game's
  // own particles. `lit`: lit smoke is on (the small lights bound and the smoke's shadow there to read).
  void DrawSpray(const View& view, gfx::Texture sceneDepth, float zNear, bool lit);
  bool AnySpray() const { return drawn_ > 0; }

  // For the water's shader: whether it shows the surf this frame, the field, and its numbers (water.cpp's constants
  // surfArea, surfLook, surfLook2, surfLook3).
  bool Active() const { return on_ && !side_ && field_; }
  gfx::Texture Field() const { return field_; }
  void Constants(float area[4], float look[4], float look2[4], float look3[4]) const;

  // The waves' timing (the shader's SurfOffset, SurfStrength): how far ahead a place's waves are (cycles), how big
  // wave n is at a place.
  static float Offset(float x, float z);
  static float Strength(float n, float x, float z);

  // Tuning (viewer --surf-*): how much white water (1 as tuned), how much spray, and the depth a wave breaks at as a
  // share of its height.
  float strength = 1, sprayStrength = 1, breakDepth = 2.0f;
  // A view for checking (viewer --surf-view n): the water shows 1 the field (red how steep the shore is, green how open
  // the water, blue bands of four world units of distance from the waterline), 2 the waves (red where the water stands
  // up, blue down, green the white water); 0 off.
  int view = 0;
  // The most puffs kept (the oldest let go first), and how far from the eye spray is still thrown.
  static constexpr int kMostPuffs = 3000;
  float farthest = 420;

  // Numbers for the log.
  int Spots() const { return static_cast<int>(spots_.size()); }
  int Puffs() const { return static_cast<int>(puffs_.size()); }
  int Drawn() const { return drawn_; }
  int bursts = 0, ships = 0;  // rock bursts thrown since the map was opened; ships throwing spray in the last frame
  float fieldMs = 0;          // how long working out the field took

 private:
  struct Spot {  // a place on a rocky shore that throws spray
    float x, z;        // on the waterline
    float toLandX, toLandZ;  // level, length 1
    float steep, open;
    double lastCrest = -1e9;  // the crest that last arrived here
  };
  struct Puff {
    Vec3 at;        // where it was thrown
    Vec3 velocity;  // as thrown
    float born = 0, life = 1, size0 = 0.2f, size1 = 1, strength = 0.3f;
    int kind = 0;   // 0 spray (drops, falling back), 1 mist (drifting off with the wind), 2 a ship's bow spray (as 0)
    float seed = 0;
  };
  struct Ship {
    Vec3 place, velocity;
    float seen = -1, carry = 0;
    float slam = 0;  // where it is in its pitching (cycles)
  };
  struct Kind {
    bool ship = false;
    float maxSpeed = 5;
  };
  const Kind& KindOf(const std::string& blueprint);
  bool MakeField();
  bool Init();  // (the spray's shader, the first time it is wanted)
  float Random();
  void Throw(const Puff& p);
  void Burst(Spot& spot, float amount);

  gfx::Device* device_;
  const MapData* map_ = nullptr;
  bool tried_ = false;      // the field worked out (or found to need none)
  gfx::Texture field_;
  float area_[4] = {0, 0, 1, 1};
  std::vector<Spot> spots_;
  bool on_ = false, side_ = false;
  float time_ = 0, lastTime_ = -1;
  double phase_ = 0;        // the waves' phase (cycles), moved on by the clock at the sea's period
  float rough_ = -1;        // how rough the sea is (eased toward what Begin is told)
  float wind_[2] = {0, 0};
  Vec3 eye_;
  float level_ = 0;
  std::deque<Puff> puffs_;
  std::unordered_map<std::string, Kind> kinds_;
  std::unordered_map<uint32_t, Ship> ships_;
  uint32_t seed_ = 0x2545f491u;
  int shaderTried_ = 0;  // 0 not yet, 1 made, -1 it cannot be
  gfx::Pipeline pipeline_;
  gfx::Buffer rows_;
  std::vector<float> rowData_;
  int drawn_ = 0;
  // MOHO64_DEBUG_SURF=1: what the field holds; 2: and the spray's counts every second; 3: every puff of spray drawn
  // solid red, to see where it is; 4: in its colour, as much as shows of it before its shape and the drops' noise
  int debug_ = 0;
  float debugAt_ = 0;
};

// A test of it (the viewer's --surf-test): ships of given kinds driven round loops over deep water near a place, as a
// game's frame (World::AddFrame draws them; their wakes, ripples and bow spray as a game's ships'), at their top speed.
class SurfTest {
 public:
  // `spec`: "ues0103=2,ues0201=1" (blueprint ids and how many), round `at` within `radius`; `depth`: the water's depth
  // at a place (more than 1.5 all round a loop for it to be used). False with the reason when none can be driven.
  bool Init(const std::string& spec, Vec3 at, float radius, const std::function<bool(const std::string&, BpValue*)>& readBlueprint,
            const std::function<float(float, float)>& depth, float waterLevel, std::string* error);
  const Frame& Update(double clock);
  std::string Describe() const { return describe_; }

 private:
  struct Driver {
    uint32_t blueprint = 0;
    float x = 0, z = 0, radius = 10, speed = 5, angle = 0, turn = 1;
  };
  std::vector<std::string> blueprints_;
  std::vector<Driver> drivers_;
  float level_ = 0;
  Frame frame_;
  std::string describe_;
};

}  // namespace render
