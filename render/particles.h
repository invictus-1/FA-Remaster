// Particle effects: the game's emitters (/effects/emitters/*_emit.bp) played as the original plays
// them. An emitter blueprint is a set of curves over the emitter's own time; an emitter throws out
// particles whose start values it reads from those curves; a particle, once thrown, is never touched
// again: where it is, how big and how turned, follows from its start values and its age, and the
// graphics card works that out (as the game's effects/particle.fx does). Its colour is its texture
// times a "ramp" texture read along its age.
//
// What a particle is was read from the game's particle shader. How an emitter fills a particle from
// the curves was read from the original program by the engine track (docs/engine-answers.md,
// render 9), and particles.cpp follows that account.
#pragma once
#include <cstdint>
#include <functional>
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

// One curve of an emitter blueprint: keys of (x: emitter time in ticks, y: value, z: random spread).
struct EmitterCurve {
  float xRange = 1;
  std::vector<Vec3> keys;
  // The value and the spread at emitter time x: both go linearly from key to key, and stay at the
  // first key's before it and at the last key's after it. No keys: 0.
  void At(float x, float* value, float* spread) const;
};

struct EmitterBlueprint {
  bool ok = false;
  float lifetime = 1;    // of the emitter, in ticks; negative: until it is stopped
  float repeatTime = 1;  // the curves are read at the emitter's age modulo this
  int frameCount = 1, stripCount = 1;  // the texture: frames side by side, strips one below the other
  int blendMode = 0;     // 0 alpha, 1 darkens (what is there x (1 - colour)), 2 "modulate 2x inverse",
                         // 3 adds, 4 premultiplied alpha, 5 refracts
  bool localVelocity = false, localAcceleration = false, gravity = false;
  bool alignRotation = false, alignToBone = false, flat = false, interpolateEmission = true;
  bool emitIfVisible = true, createIfVisible = false, snapToWaterline = false, onlyEmitOnWater = false;
  bool resistance = false;  // particles are slowed as by air (ResistanceCurve says how much)
  // The visuals track's flipbooks (not the original's): TextureGridAnimate runs a particle's frames
  // across the whole Framecount x Stripcount grid (left to right, then down), from its strip's first
  // frame, stopping at the last one or, with TextureGridLoop, starting again; TextureFrameBlend (and
  // every grid) blends each frame into the next instead of stepping.
  bool gridAnimate = false, gridLoop = false, frameBlend = false;
  int sortOrder = 0;
  float lodCutoff = 0;      // an emitter that hangs on something throws nothing out farther from the camera (0: no limit)
  float longestLife = 1;    // the longest a particle of it can live, in ticks
  std::string texturePath, rampPath;
  gfx::Texture texture, ramp;
  EmitterCurve xDirection, yDirection, zDirection, emitRate, particleLifetime, velocity, xAccel, yAccel, zAccel, xPos, yPos, zPos,
      size, startSize, endSize, initialRotation, rotationRate, frameRate, textureSelection, rampSelection, resistanceCurve;
};

class Particles {
 public:
  Particles(gfx::Device* device, const moho::Vfs* vfs, TextureCache* textures) : device_(device), vfs_(vfs), textures_(textures) {}
  bool Init(std::string* error);

  // The map's water, for emitters that keep to its surface: its level, and the ground's height at a
  // place. Without it there is no water.
  void SetWater(float level, std::function<float(float, float)> groundHeight);
  // Emitters started from now on read the remastered effects' files where there are some
  // (effect_files.h); false: always the game's. The world sets it from its effects look.
  bool remastered = true;

  // Starts an emitter of that blueprint ("/effects/emitters/x_emit.bp") there. Returns its handle;
  // 0 when the blueprint cannot be read (reported once). `startedAt`: when it began, if that was
  // before now (seconds, on Update's clock; negative: now). `attached`: it hangs on something (a
  // unit's bone, a shot) and the caller will Move it; only such emitters mind their LODCutoff.
  uint32_t Start(const std::string& blueprint, Vec3 position, const Quat& orientation = {}, float scale = 1, double startedAt = -1,
                 bool attached = false);
  // Where an emitter is now (one that sits on something that moves).
  void Move(uint32_t emitter, Vec3 position, const Quat& orientation);
  // What the game's scripts change on an emitter: its size (ScaleEmitter), and one of its
  // parameters by the engine's name for it (SetEmitterParam: LIFETIME, REPEATTIME, SCALE, LODCUTOFF,
  // EMITIFVISIBLE; names it does not act on are ignored).
  void SetScale(uint32_t emitter, float scale);
  void SetParam(uint32_t emitter, const std::string& name, float value);
  // A script's SetEmitterCurveParam(name, height, size) on a running emitter: that curve of this
  // emitter alone ("Y_POSITION_CURVE", "ROTATION_RATE_CURVE", ...) becomes the one value `height`
  // with the random spread `size`, for all of the emitter's time, as the original does it (a new
  // curve with that one key: docs/engine-answers.md).
  void SetCurve(uint32_t emitter, const std::string& name, float height, float size);
  // The emitter throws out no more; what it threw lives out its time.
  void Stop(uint32_t emitter);
  bool Running(uint32_t emitter) const;
  // Does that emitter glow (its particles add their light)? Then `rampPath` is its colour ramp's
  // file and `scale` how large a script has made it.
  // The scale an emitter was given (1 when it was not, or it is not running).
  float ScaleOf(uint32_t emitter) const;
  bool Glows(uint32_t emitter, std::string* rampPath, float* scale) const;
  // While hidden an emitter throws nothing out, and makes nothing up afterwards (what it hangs on
  // is not seen by whom the picture is for).
  void Hide(uint32_t emitter, bool hidden);

  // Moves time on to `now` (seconds): emitters throw out what is due, and end. With `view`, an
  // emitter whose blueprint says EmitIfVisible throws nothing out while it is out of the picture
  // (or, hanging on something, farther away than its LODCutoff); when it is seen again it makes up
  // the last ticks it missed.
  void Update(double now, const View* view = nullptr);
  // Draws every living particle into the current pass, which must be the scene's.
  // Into a pass that has the scene's colour alone: the particles test the scene's depth themselves
  // (`sceneDepth`, with `zNear` the camera's near distance), sample by sample, and fade over
  // `soften` world units where they are about to meet something solid.
  void Draw(const View& view, gfx::Texture sceneDepth, float zNear);
  float soften = 0.6f;
  // The particles that bend what is behind them (the original's blend mode 5: the shimmer over heat,
  // a shock wave's ring): after Draw, into the scene's pass, with the picture as it was just before
  // (one sample a pixel). `Bending()`: are there any, after the last Draw.
  bool Bending() const { return bending_; }
  void DrawBending(const View& view, gfx::Texture behind, gfx::Texture sceneDepth, float zNear);

  // The modern look: how much brighter than its texture a particle that adds its light is (it is
  // light, and the picture's last pass brings it back into what a screen shows).
  float glow = 2.5f;
  int liveParticles = 0;  // after the last Draw
  int liveEmitters = 0;   // after the last Update
  int quietEmitters = 0;  // of those: out of sight or too far away in the last Update
  float cpuMs = 0;        // Update and Draw on the processor, averaged over the last frames

 private:
  // As the shaders read it: five rows of four numbers.
  struct Particle {
    float position[3], rotation;
    float velocity[3], rotationRate;      // per second
    float acceleration[3], startTime;     // per second squared; seconds
    float lifetime, size, sizeRate, frameRate;
    float stripV, rampV, drag, unused;    // drag: how fast it is slowed, per second (0: not at all)
  };
  struct Set {  // the particles of one blueprint: one texture, one ramp, one way of blending
    const EmitterBlueprint* blueprint = nullptr;
    std::vector<Particle> particles;
  };
  struct Emitter {
    uint32_t id = 0;
    const EmitterBlueprint* blueprint = nullptr;
    Set* set = nullptr;
    Vec3 position, positionBefore;  // now, and at the tick before (emission is spread between the two)
    Quat orientation;
    float scale = 1;
    float lifetime = 1, repeatTime = 1, lodCutoff = 0;  // the blueprint's, or what a script set
    bool emitIfVisible = true;
    bool attached = false;
    double started = 0;
    int ticksDone = 0;   // of its own time, in tenths of a second
    float carry = 0;     // the fraction of a particle left over from the last tick
    bool stopped = false;
    bool hidden = false;
    // curves a script set on this emitter in place of its blueprint's (SetCurve)
    std::vector<std::pair<EmitterCurve EmitterBlueprint::*, EmitterCurve>> curves;
  };
  const EmitterBlueprint* Blueprint(const std::string& path);
  Emitter* Find(uint32_t id);
  void Emit(Emitter* emitter, int tick);
  float Random();  // 0..1

  gfx::Device* device_;
  const moho::Vfs* vfs_;
  TextureCache* textures_;
  gfx::Pipeline pipelines_[4];  // alpha, darkening, adding, premultiplied
  gfx::Buffer buffer_;
  std::unordered_map<std::string, std::unique_ptr<EmitterBlueprint>> blueprints_;
  std::unordered_map<const EmitterBlueprint*, std::unique_ptr<Set>> sets_;
  std::vector<Set*> order_;  // the sets in drawing order
  std::vector<Emitter> emitters_;
  std::unordered_map<uint32_t, size_t> index_;  // emitter id -> its place in emitters_
  std::vector<float> rows_;
  std::vector<std::pair<size_t, size_t>> spans_;  // per set of order_, from the last Draw: first row, particle count
  bool bending_ = false;
  void DrawSets(const View& view, bool bending, gfx::Texture behind, gfx::Texture sceneDepth, float zNear);
  uint32_t nextId_ = 1;
  uint32_t seed_ = 0x2545f491u;
  double now_ = 0;
  float updateMs_ = 0;
  bool haveWater_ = false;
  float waterLevel_ = 0;
  std::function<float(float, float)> groundHeight_;
};

}  // namespace render
