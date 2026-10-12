// Particle effects: the game's emitters (/effects/emitters/*_emit.bp) played as the original plays
// them. An emitter blueprint is a set of curves over the emitter's own time; an emitter throws out
// particles whose start values it reads from those curves; a particle, once thrown, is never touched
// again: where it is, how big and how turned, follows from its start values and its age, and the
// graphics card works that out (as the game's effects/particle.fx does). Its colour is its texture
// times a "ramp" texture read along its age.
//
// What a particle is was read from the game's particle shader. How an emitter fills a particle from
// the curves is in the original program, not in the game's files: what this file does there is a
// reading of the 2,835 emitter blueprints, marked GUESS where it is one (render/REQUESTS.md, 9).
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

// One curve of an emitter blueprint: keys of (x: emitter time, y: value, z: random spread).
struct EmitterCurve {
  float xRange = 1;
  std::vector<Vec3> keys;
  // The value and the spread at emitter time x (linear between keys, the end keys' beyond them).
  void At(float x, float* value, float* spread) const;
};

struct EmitterBlueprint {
  bool ok = false;
  float lifetime = 1;    // of the emitter, in ticks (GUESS); negative: until it is stopped
  float repeatTime = 1;  // the curves are read at the emitter's age modulo this (GUESS)
  int frameCount = 1, stripCount = 1;  // the texture: frames side by side, strips one below the other
  int blendMode = 0;     // 0 alpha, 1 darkens (what is there x (1 - colour)), 2 "modulate 2x inverse",
                         // 3 adds, 4 premultiplied alpha, 5 refracts (GUESS, from the textures' names)
  bool localVelocity = false, localAcceleration = false, gravity = false;
  bool alignRotation = false, alignToBone = false, flat = false, interpolateEmission = true;
  int sortOrder = 0;
  float lodCutoff = 1e9f;  // farther from the camera than this, the emitter throws nothing out
  std::string texturePath, rampPath;
  gfx::Texture texture, ramp;
  int rampRows = 1;  // the ramp texture's height: one colour ramp per row
  EmitterCurve xDirection, yDirection, zDirection, emitRate, particleLifetime, velocity, xAccel, yAccel, zAccel, xPos, yPos, zPos,
      startSize, endSize, initialRotation, rotationRate, frameRate, textureSelection, rampSelection;
};

class Particles {
 public:
  Particles(gfx::Device* device, const moho::Vfs* vfs, TextureCache* textures) : device_(device), vfs_(vfs), textures_(textures) {}
  bool Init(std::string* error);

  // Starts an emitter of that blueprint ("/effects/emitters/x_emit.bp") there. Returns its handle;
  // 0 when the blueprint cannot be read (reported once). `startedAt`: when it began, if that was
  // before now (seconds, on Update's clock; negative: now).
  uint32_t Start(const std::string& blueprint, Vec3 position, const Quat& orientation = {}, float scale = 1, double startedAt = -1);
  // Where an emitter is now (one that sits on something that moves).
  void Move(uint32_t emitter, Vec3 position, const Quat& orientation);
  // What the game's scripts change on an emitter: its size (ScaleEmitter), and how long it runs,
  // in ticks (the LIFETIME parameter; negative: until stopped).
  void SetScale(uint32_t emitter, float scale);
  void SetLifetime(uint32_t emitter, float ticks);
  // The emitter throws out no more; what it threw lives out its time.
  void Stop(uint32_t emitter);
  bool Running(uint32_t emitter) const;

  // Moves time on to `now` (seconds): emitters throw out what is due, and end. With `view`, an
  // emitter that is out of the picture, or farther from the camera than its blueprint's LODCutoff,
  // throws nothing out meanwhile (as in the original; what it would have thrown is not made up later).
  void Update(double now, const View* view = nullptr);
  // Draws every living particle into the current pass, which must be the scene's.
  void Draw(const View& view);

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
    float stripV, rampV, stripHeight, frameSize;
  };
  struct Set {  // the particles of one blueprint: one texture, one ramp, one way of blending
    const EmitterBlueprint* blueprint = nullptr;
    std::vector<Particle> particles;
  };
  struct Emitter {
    uint32_t id = 0;
    const EmitterBlueprint* blueprint = nullptr;
    Set* set = nullptr;
    Vec3 position;
    Quat orientation;
    float scale = 1;
    float lifetime = 1;  // the blueprint's, or what a script set
    double started = 0;
    int ticksDone = 0;   // of its own time, in tenths of a second
    float carry = 0;     // the fraction of a particle left over from the last tick
    bool stopped = false;
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
  uint32_t nextId_ = 1;
  uint32_t seed_ = 0x2545f491u;
  double now_ = 0;
  float updateMs_ = 0;
};

}  // namespace render
