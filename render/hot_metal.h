// Hot metal (World::hotMetal; renderer item 12, docs/render-lighting.md): what a battle leaves is hot for a while.
//  - A fresh wreck's metal glows where it broke and on its thin parts, cooling over a minute or two by the colour a hot
//    body has at its temperature (black body: orange, then red, then gone; not a fade of one colour), in the surface's
//    own light (its emission), so the glow round what is brighter than the screen catches it. A faint small light comes
//    from the hottest few while they are hot.
//  - A fresh scorch's or crater's middle glows on the ground and cools the same way, seconds to a minute by its size.
//  - A cooling wreck smoulders: a thin trail of the game's own smoke (its particle system) rises from it, thinning as it
//    cools, and stops when it is cold.
// All of it is timed from when the wreck or the mark appeared, by the game's clock (a paused game's metal stays hot).
// The modern look's only. With it off nothing of this is drawn and nothing else changes.
//
// How a wreck carries its heat to the model shader: in MeshRenderer::Add's `tint` (the instance's bones.yzw), which a
// ghost fills with a colour; a hot wreck's is (-0.25, -age, -seconds) (Tint): its sum is below 0, so no ghost's tint is
// laid over it, and its first number is above -0.5, so it is no tank's treads either (mesh_renderer.cpp, VS and PS).
// The shader's HotMetalGlow (kHotMetalShaderBlock) works out every place's temperature from those two numbers.
//
// Where a wreck "broke":
//  - The visuals track's own wrecks (ModelCache::RemasteredWreck): read from their textures, as her wreck kit paints them
//    (visuals/tools/wreckkit.py, burn): a shell's crater in the hull is a near-black hole (albedo about 0.02, roughness
//    0.92, no metal) with a torn rim of bare steel (metal 0.85, roughness 0.42) round it; nowhere else is a wreck's
//    surface that smooth and metallic, or that rough, dark and dull. The holes are the hottest and cool last (the
//    inside shows through them), the torn rims, thin, cool sooner.
//  - The game's own wrecks (the unit's whole model in the burnt look): the same kind of craters made up in the model's
//    own space (cells a fifth of its height, a crater in some of them, ragged), weighted toward where the burnt look's
//    soot lies thickest. An inference: the game's model has no broken parts.
//  - Thin parts on both (barrels, antennas, fins, torn edges of a broken mesh): where the surface turns sharply for the
//    model's size (how fast its normal changes against its place from pixel to pixel); hot at first, cooling fastest.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "linalg.h"

namespace render {

class Lights;
class Particles;

class HotMetal {
 public:
  // --- tuning ---
  // How long the hottest part of a wreck as large as a light tank (its bounding sphere's radius 0.7 world units) glows
  // at all, in seconds; larger wrecks longer (by the radius to the power 0.35: an experimental about two minutes).
  float wreckSeconds = 55;
  // How long a scorch or crater a world unit across glows; larger ones longer (to the power 0.8), at most a minute.
  float craterSeconds = 4.5f;
  // At most this many small lights from hot things at once (the hottest near what the camera looks at), and how
  // bright one is at the start (as Lights::Add's colour: 1 lights a surface as the sun does).
  int mostLights = 3;
  float lightStrength = 0.8f;
  // The thin smoke a cooling wreck gives off: the game's own emitter ("" none; the trail a damaged unit trails), its
  // particles a tick at the start (its blueprint's 8; it thins to a third as the wreck cools), and from how many wrecks
  // at most (those nearest what the camera looks at: forty trails close up cost about 0.1 ms at 1440p).
  std::string smokeEmitter = "/effects/emitters/destruction_damaged_smoke_01_emit.bp";
  float smokeRate = 4;
  int mostSmoke = 16;

  // How long a wreck of that radius (world units) glows, and a scorch or crater of that size across.
  float WreckSeconds(float radius) const;
  float CraterSeconds(float across) const;
  // The instance's tint for a hot wreck (above).
  static Vec3 Tint(float age, float seconds) { return {-0.25f, -age, -seconds}; }
  // The hottest part's temperature (kelvin) at that age of a thing that glows `seconds`; as the shaders have it.
  static float CoreKelvin(float age, float seconds);
  // A black body's colour at that temperature, linear, as bright as the shaders draw its glow (luminance 1 at 1250 K).
  static Vec3 BlackBody(float kelvin);

  // --- a frame ---
  // Begin: a new frame at the game's clock `now` (seconds).
  void Begin(double now);
  // When a wreck the host draws itself (the effects stand's) was first seen, by its own number: now, the first time it
  // is asked for; forgotten once a frame goes by without it.
  double FirstSeen(uint32_t key);
  // Something glowing this frame: a wreck (`smoke`: it smoulders) or a crater, its middle and size, its age and how
  // long it glows. `key`: the same every frame (its smoke goes by it).
  void Glowing(uint64_t key, Vec3 centre, float radius, float age, float seconds, bool smoke);
  // After the frame's last Glowing and before the lights are built: the few lights, and the smoke started, thinned
  // and stopped. `target`: what the camera looks at; `night`: Lights::night.
  void End(Lights* lights, Particles* particles, Vec3 target, float night);
  // Everything let go of (the option switched off): the smoke stopped.
  void Off(Particles* particles);
  int glowing = 0, lit = 0, smoking = 0;  // of the last frame

  // The shaders' part (HLSL, after kViewShaderBlock): HotMetalColour(kelvin), HotMetalKelvin(...), HotMetalGlow(...).
  static const char* ShaderBlock();

 private:
  double now_ = 0;
  uint32_t frame_ = 0;
  struct Seen {
    double first = 0;
    uint32_t frame = 0;
  };
  std::unordered_map<uint32_t, Seen> seen_;
  struct Glow {
    uint64_t key;
    Vec3 centre;
    float radius, age, seconds;
    bool smoke;
  };
  std::vector<Glow> glows_;
  struct Smoke {
    uint32_t emitter = 0;
    int rate = -1;       // the emission set last (tenths of a particle a tick)
    uint32_t frame = 0;  // the frame it was last glowing in
  };
  std::unordered_map<uint64_t, Smoke> smoke_;
};

}  // namespace render
