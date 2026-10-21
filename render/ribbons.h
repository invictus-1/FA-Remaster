// Beams and trails: bands of texture that always turn their face to the camera.
//
// A beam (the game's BeamBlueprint: a laser, a tractor beam, a tracer) is one band from a start to
// an end, its texture sliding along it, its colour running from a start colour to an end colour.
// A trail (TrailEmitterBlueprint: the streak behind a missile or an aircraft's wing tip) is a band
// through the places its emitter has been, each place fading out along a ramp texture as it ages.
//
// What the vertices and textures are for was read from the game's effects/particle.fx (BeamVS,
// TrailVS and their pixel shaders). How the original fills them from the blueprints was read from
// the original program by the engine track (docs/engine-answers.md, render 10).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "gfx.h"
#include "linalg.h"
#include "textures.h"
#include "view.h"

namespace moho {
class Vfs;
}

namespace render {

class Ribbons {
 public:
  Ribbons(gfx::Device* device, const moho::Vfs* vfs, TextureCache* textures) : device_(device), vfs_(vfs), textures_(textures) {}
  bool Init(std::string* error);
  // Beams and trails read the remastered effects' files where there are some (effect_files.h);
  // false: always the game's. The world sets it from its effects look.
  bool remastered = true;

  // A beam of that blueprint ("/effects/emitters/x_beam_01_emit.bp") from `start` to `end`, for
  // this frame. False when the blueprint cannot be read (reported once).
  // `faction` (here and below; "": none): whose the thing it is made on is. In the remastered look the
  // blueprint is read first from the visuals track's folder of that faction (effect_files.h).
  bool Beam(const std::string& blueprint, Vec3 start, Vec3 end, const std::string& faction = std::string());
  // The same from `start` along `direction` (unit length), as long as the blueprint says.
  bool BeamAlong(const std::string& blueprint, Vec3 start, Vec3 direction, const std::string& faction = std::string());
  // A beam of a texture alone (the scripts' CreateAttachedBeam), adding its light, for this frame.
  void TextureBeam(const std::string& texture, Vec3 start, Vec3 end, float halfWidth);
  // A plain glowing band without a blueprint, for this frame: what stands in where the game's own
  // effect is not known yet. `color` is rgb in 0..1; `width` the whole width in world units.
  void PlainBeam(Vec3 start, Vec3 end, Vec3 color, float width);
  // Does that beam or trail blueprint add its light? Then a trail says its colour ramp's file and
  // a beam its own colour (as the blueprint gives it, by its alpha).
  bool Glows(const std::string& blueprint, std::string* rampPath, Vec3* color, const std::string& faction = std::string());

  // Starts a trail of that blueprint at a place; Move it every frame to where its emitter is.
  // Returns its handle; 0 when the blueprint cannot be read.
  uint32_t StartTrail(const std::string& blueprint, Vec3 position, const std::string& faction = std::string());
  void MoveTrail(uint32_t trail, Vec3 position);
  // The trail grows no more; what is there fades out as it would have.
  void StopTrail(uint32_t trail);

  // Moves time on to `now` (seconds).
  void Update(double now);
  // Draws this frame's beams and all trails into the current pass, which must be the scene's, and
  // forgets the beams.
  void Draw(const View& view);

  float glow = 2.5f;  // as Particles::glow: the modern look's brightness of what adds its light
  // Tracers (modern look): a shell's trail that the game's files give as a plain white line (a trail
  // that adds its light and whose colour ramp is one of the "ramp_white" ones: tank shells, gauss
  // rounds) is drawn hot instead: white at the shell, through yellow and orange to a dull red where
  // it ends, wider at the shell and never thinner than a pixel. false: as the files say.
  bool tracers = true;
  // (photofix) A photo's frame: this many of its pixels make one of the window's (a tracer's least width is the
  // window's pixel, as the window shows it); 1 in the game's frames.
  float pixelScale = 1;
  int drawnBands = 0; // beams and trails in the last Draw

 private:
  struct Kind {  // a beam or trail blueprint
    bool ok = false, trail = false;
    gfx::Texture texture, ramp;
    int blend = 2;                 // 0 alpha, 1 darkening, 2 adding, 3 premultiplied
    float startColor[4] = {1, 1, 1, 1}, endColor[4] = {1, 1, 1, 1};  // beams
    float length = 10, halfWidth = 0.05f;
    float repeatRate = 0;          // beams: how often the texture repeats per world unit (0: once over the beam)
    float uShift = 0, vShift = 0;  // how fast the texture slides, per second
    float life = 1;                // trails: how long a place stays, seconds
    std::string rampPath;          // trails: the colour ramp's file
    bool hot = false;              // trails: a plain white one that adds its light (see `tracers`)
  };
  struct Vertex {
    float position[3];
    float uv[2];
    float rampUv[2];
    uint32_t color;
  };
  struct Batch {  // what one draw call draws
    const Kind* kind;
    std::vector<Vertex> vertices;
  };
  struct TrailPoint {
    Vec3 position;
    double born;
    float travelled;  // how far the emitter had gone since the trail began
  };
  struct Trail {
    uint32_t id = 0;
    const Kind* kind = nullptr;
    std::vector<TrailPoint> points;  // oldest first
    Vec3 head;
    float travelled = 0;  // by the head
    double started = 0;
    double headAt = 0;  // (trails) when the emitter was last moved to the head (StopTrail lays it there as the last place)
    bool stopped = false;
  };
  const Kind* Blueprint(const std::string& path, const std::string& faction = std::string());
  Trail* Find(uint32_t id);
  Batch* BatchFor(const Kind* kind);
  // One camera-facing quad from a to b, `half` wide to either side.
  void Band(Batch* batch, const View& view, Vec3 a, Vec3 b, float halfA, float halfB, const float uvA[2], const float uvB[2],
            const float rampA[2], const float rampB[2], uint32_t colorA, uint32_t colorB);

  gfx::Device* device_;
  const moho::Vfs* vfs_;
  TextureCache* textures_;
  gfx::Pipeline pipelines_[4], trailPipelines_[4];
  gfx::Buffer buffer_;
  std::unordered_map<std::string, std::unique_ptr<Kind>> kinds_;
  std::unordered_map<std::string, bool> factionFiles_;  // "<path>|remastered|<faction>" -> has the faction its own file
  Kind plain_;
  struct PendingBeam {
    const Kind* kind;
    Vec3 start, end;
    Vec3 color;       // plain beams
    float halfWidth;
  };
  std::vector<PendingBeam> beams_;
  std::vector<Trail> trails_;
  std::vector<Batch> batches_;
  std::vector<Vertex> all_;
  uint32_t nextId_ = 1;
  double now_ = 0;
};

}  // namespace render
