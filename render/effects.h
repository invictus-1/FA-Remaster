// The game's effects in a running game: what the simulation's scripts start, change and end
// (emitters, beams, trails) is turned into particle emitters and bands here, and every effect that
// hangs on something is kept where that thing is in the frame being shown.
//
// The simulation hands over events (frame.h, FrameEffectEvent) beat by beat; they take effect when
// the picture's clock reaches their beat. An effect's place is its entity's place in the frame: a
// unit (with the bone's pose), a projectile, or a weapon's beam (bone 0 its start, 1 and 2 its end).
#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "frame.h"
#include "linalg.h"
#include "models.h"
#include "particles.h"
#include "ribbons.h"

namespace render {

class Effects {
 public:
  Effects(Particles* particles, Ribbons* ribbons, ModelCache* models) : particles_(particles), ribbons_(ribbons), models_(models) {}

  // Events in the order the simulation made them.
  void Queue(std::vector<FrameEffectEvent>&& events);
  // Lets every event up to `tick` take effect, moves what hangs on something to where that is in
  // `frame`, and lists this frame's beams. Call before Particles::Update and Ribbons::Update, whose
  // clock must be tick / 10 seconds.
  void Update(double tick, const Frame& frame);
  // Does one of the game's own effects hang on that entity (so that no stand-in is needed for it)?
  bool OnEntity(uint32_t entity) const { return hanging_.count(entity) != 0; }

  int live = 0;        // effects being kept
  int started = 0;     // since the beginning
  int notShown = 0;    // events for kinds of effect that are not drawn yet (light particles, decals)
  float cpuMs = 0;     // Update on the processor, averaged over the last frames

 private:
  enum class Kind { Emitter, Trail, Beam, TextureBeam };
  struct Effect {
    Kind kind = Kind::Emitter;
    uint32_t handle = 0;      // the emitter's or trail's
    std::string blueprint;    // beams: the blueprint, or the texture
    uint32_t entity = 0xffffffff, entity2 = 0xffffffff;
    int bone = -1, bone2 = -1;
    bool attached = false;    // follows its entity
    Vec3 offset;              // from the attach point, in its frame (OffsetEmitter)
    Vec3 position;            // where it was last
    Quat orientation;
    float length = 0, halfWidth = 0.05f;  // texture beams
    const Model* model = nullptr;         // of the unit it hangs on, once looked up (for the bone's place at rest)
  };
  void Apply(const FrameEffectEvent& event, const Frame& frame);
  void End(uint32_t id);
  // Where an entity's bone is in the frame. False when the entity is not in it.
  // `model`: where the unit's model is remembered between calls (may be null).
  bool AttachPoint(const Frame& frame, uint32_t entity, int bone, Vec3* position, Quat* orientation, const Model** model = nullptr);

  Particles* particles_;
  Ribbons* ribbons_;
  ModelCache* models_;
  std::deque<FrameEffectEvent> pending_;
  std::unordered_map<uint32_t, Effect> effects_;
  std::unordered_map<uint32_t, int> hanging_;  // entity -> how many effects hang on it
  std::unordered_map<uint32_t, uint32_t> projectileAt_, beamAt_;  // this frame: entity id -> index in the frame's lists
};

}  // namespace render
