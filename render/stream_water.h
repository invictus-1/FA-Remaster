// [stream] Streams as real water (Chris, 2026-10-19, of the remastered Seton's stream: "This needs to be real water not
// just a hard texture").
//
// A map's stream is scenery the visuals track makes (its stream_build.py): a ribbon laid a little over the ground (0.04:
// the map's heights are the game's, so no bed is carved), its texture's u running from one bank (0) to the other (1)
// and its v along it, the vertices of one cross-section sharing a v; and a second, narrower ribbon of white water where
// it runs steep and at its mouth. Drawn as models they are a flat sheet with a hard edge.
//
// With World::streamWater on (the modern look), those props are left out of the models' drawing (ModelLod::stream,
// MeshRenderer::divertStream: the scene, the sun's and the lamps' shadow maps) and drawn here instead, after everything
// solid and before the sea, as water that reads the picture behind it and the scene's depth (as the sea's does,
// water.cpp):
//   depth    a bed shaped across the ribbon (none at the banks, deepest in the middle, as deep as the ribbon is wide
//            allows), the water taking red, then green from what lies under it by how much water the eye looks
//            through, and its own colour (the sea's, water.cpp) in its place where deep; the bed seen bent a little by
//            the ripples
//   shore    no hard edge: the water thins out over the last part of its width, its edge wavering, and the bank just
//            past it is darker and a little glossy, wet
//   flow     ripples (the visuals track's ripple normals, two layers carried downstream half a period apart, each
//            restarting where the other is strongest, so that a speed that varies along the stream never stretches
//            them without end) and a lace of foam, faster where the stream drops (its slope, from the mesh's heights);
//            foam where it meets what stands in it (rocks, units: the scene's depth against the water's, a ring of
//            looks round each place) and streaks downstream of them
//   light    the sky mirrored by the angle of view (and what the picture shows that way), the sun's glint on the
//            ripples ([shore] widened and dimmed by the ripples a pixel takes in, water_glint.h), the modern light
//            through the sun's and the clouds' shadows, the lamps; the bright net of light on a shallow bed; in rain,
//            rings where the drops fall and a roughened surface
//   white    the white water ribbon: drawn by this pass too (in place of its alpha-tested, swaying model): foam carried
//            downstream fast, its streaks from the ribbon's own texture, broken along and across, its sides ragged,
//            lit as the stream is; drawn after the sea (World::Render), so that it can lie on the sea where the
//            stream runs into it
//   ends     every ribbon thins out over the last of its length at both ends (no square end shows)
//   outlet   its head (where the mesh begins: on the ground, at a rock face, in a crack) spills out of the stone as white
//            water widening over its first two units, foam where its drops ease out below; the stone round the outlet
//            wet: darker, in streaks running down it, with a sheen and trickles' glints (Chris, 2026-10-20: "come out of
//            the side of the mountain")
//   cascade  a stretch that drops steeply is white water all along, the pools between its drops too, and from afar the
//            foam keeps the share of the water it covers near by: from above a cascade reads as one fall, not dashes
//   mouth    where a stream ends just above the sea (the remastered Seton's ends at the top of the beach, a step over
//            the water: Chris, 2026-10-19), its water is carried on (StreamWater::Add, each prop as it is placed) down
//            the ground the way it ran, spreading and thinning, white where it pours, until it is under the sea; and a
//            fan of foam spreads from there over the sea's surface and thins out. [shore] (2026-10-21: its white water
//            showed hard parallel streaks from mid distance, and its foam dotted rows along the lip) Over its last units
//            and the lip, the foam of both ribbons is froth churned up by the drop: clumps of three sizes laid in the
//            world's own frame, not the ribbon's, carried downstream, thicker toward the lip, broken, no bands
// What the props are (StreamWater::Add): a level whose blueprint ShaderName is "StreamWater" (the white water's
// "StreamFoam") (models.cpp; drawn as "LabPBR" and "LabPBRAlpha" otherwise). Off: they are drawn as before.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "camera.h"
#include "gfx.h"
#include "linalg.h"
#include "models.h"
#include "textures.h"
#include "view.h"

namespace render {

// The texture registers of the streams' own pass, its own (StreamWater::Draw binds them and leaves them empty after);
// below the registers every scene shader shares (t26 and up, view.h).
inline constexpr int kStreamBehindSlot = 0;   // t0: the picture as it was before the streams (one sample a pixel)
inline constexpr int kStreamDepthSlot = 1;    // t1: the scene's depth (its samples)
inline constexpr int kStreamHeightsSlot = 2;  // t2: the map's heights (terrain.h, HeightTexture)
inline constexpr int kStreamRipplesSlot = 3;  // t3: the water ribbon's ripple normals (its level's normal map)
inline constexpr int kStreamFoamSlot = 4;     // t4: the white water ribbon's streaks (its level's albedo, in alpha)

class StreamWater {
 public:
  StreamWater(gfx::Device* device, TextureCache* textures) : device_(device), textures_(textures) {}

  // The map's stream props (World::Open, once its props are made): Forget, then Add for each prop whose model's finest
  // level is a stream's (ModelLod::stream), placed by `world`; `key` what the world knows it by (its index in the map's
  // list: Draw asks whether it still stands).
  void Forget();
  void Add(const Model* model, const Mat4& world, uint32_t key);
  bool Any() const { return !placed_.empty(); }
  int Count(bool whiteWater) const;
  // [stream mouth] The sea's level (-1e9: no sea) and the ground's height at (x, z): set before Add (World::Open), for
  // carrying a stream that ends just above the sea on into it. Without them nothing is carried on.
  float seaLevel = -1e9f;
  std::function<float(float, float)> groundHeight;

  // Its pipelines, made the first time they are wanted (World::Begin, each frame it is on): false when they cannot be.
  bool Prepare();
  // Into a pass that has the scene's colour ONLY (Post::BeginColorOnly), with the picture as it was before (`behind`, one
  // sample a pixel) and the scene's depth to read, the map's heights (R16, `heightUnit` world units at 1) and size; the
  // sun's and the lamps' shadow maps and the small lights bound as for the sea. `standing`: whether the prop of that key
  // is still there (the game may remove scenery). `whiteWater`: [stream mouth] the white water alone (after the sea; `behind`
  // is not read), else the water alone (before the sea).
  void Draw(const View& view, const Camera& camera, gfx::Texture behind, gfx::Texture sceneDepth, gfx::Texture heights, float heightUnit,
            float mapWidth, float mapHeight, const std::function<bool(uint32_t)>& standing, bool whiteWater = false);

  // The look (the viewer's --stream-water-tune depth,murk,flow):
  float depth = 0.32f;  // the deepest water, in the middle, as a share of the ribbon's half width
  float murk = 6.0f;    // how much each unit of that depth hides and colours of the bed, as that many units of the sea's water
  float flow = 1.0f;    // how fast it runs, times
  // A view for checking in place of the water (the viewer's --stream-water-view): 1 its depth (white deep), 2 its flow
  // (red its speed, green its white water, blue where it meets something), 0 off.
  int view = 0;
  // Numbers of the last frame: ribbons drawn, their triangles (both kinds).
  int drawn = 0, triangles = 0;

 private:
  // A vertex of a stream's ribbon as its shaders take it (made from the mesh file by RibbonOf).
  struct Vertex {
    float position[3];  // the model's own (a mouth's plume: the world's)
    float across[3];    // toward the bank at u = 1 (unit, level)
    float along[3];     // downstream (unit, level)
    float shape[4];     // u; how far downstream (world units of the model); the half width there; the drop per unit downstream
    // [stream mouth] how far from its run's upstream start and to its run's downstream end (world units of the model,
    // along it: the ends thin out); how far it has run on past the mesh's end toward the sea (0 on the mesh, 1 at the
    // end of what is carried on); how steep its stretch is (the drop per unit downstream over three units either side: a
    // cascade's pools between its drops count as the cascade), -1 on a mouth's plume of foam on the sea, -2 on an outlet's
    // box (whose fields then say its place: Outlet)
    float more[4];
    // [shore] x: how near the lip of a mouth (CarryOn): 1 at the water ribbon's end and on what is carried on past it into
    // the sea, 0 from kLipReach upstream of it and on every stream that does not run into the sea; y, z, w unused
    float lip[4];
  };
  // A model's ribbon on the processor (RibbonOf), and where its downstream end is (the last cross-section's banks).
  struct Ribbon {
    bool ok = false;
    bool whiteWater = false;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    gfx::Texture texture;  // the ripples (the water's normal map) or the streaks (the white water's albedo)
    uint32_t endBank[2] = {0, 0};  // the downstream end's vertices at u = 0 and u = 1
    float endS = 0, endHalf = 0;
    uint32_t startBank[2] = {0, 0};  // the upstream start's (its head)
    float startS = 0, startHalf = 0;
  };
  // Each prop as placed: its own buffers (what is carried on toward the sea depends on where it lies).
  struct Placed {
    bool whiteWater = false;
    bool plume = false;  // [stream mouth] (a mouth's plume, not a prop: not counted)
    bool outlet = false;  // [stream] (an outlet's box: drawn by its own pipeline before the water; not counted)
    gfx::Buffer vertices, indices;
    uint32_t indexCount = 0;
    gfx::Texture texture;
    Mat4 world;
    uint32_t key = 0;
    Vec3 lo{0, 0, 0}, hi{0, 0, 0};  // in the world, the wet bank's margin included
  };
  const Ribbon* RibbonOf(const Model* model);
  void Place(std::vector<Vertex> vertices, const std::vector<uint32_t>& indices, gfx::Texture texture, const Mat4& world, uint32_t key, bool whiteWater);
  // [stream mouth] Carries a water ribbon placed by `world` on from its downstream end down the ground into the sea (when
  // it ends just above it), appending to `vertices` and `indices` (the model's own space); a plume of foam on the sea
  // from where it goes under (world space) into `plume`, `plumeIndices`. False: it ends elsewhere.
  // [stream] The box round a water ribbon's head in which the outlet's stone is wetted (world space; stream_water.cpp).
  void Outlet(const Ribbon& ribbon, const Mat4& world, std::vector<Vertex>* box, std::vector<uint32_t>* boxIndices) const;
  bool CarryOn(const Ribbon& ribbon, const Mat4& world, std::vector<Vertex>* vertices, std::vector<uint32_t>* indices, std::vector<Vertex>* plume,
               std::vector<uint32_t>* plumeIndices) const;
  gfx::Device* device_;
  TextureCache* textures_;
  std::unordered_map<const Model*, Ribbon> ribbons_;
  std::vector<Placed> placed_;
  gfx::Pipeline water_, outlet_, white_;
  bool tried_ = false;
};

}  // namespace render
