// The frame: a plain description of the world at one simulation tick. It is all the renderer ever
// sees of the simulation. Today a frame is filled from a tick recording; later the running simulation
// fills the same structure and hands over two consecutive ticks, which the renderer blends for display.
//
// This is the first version: what a recording holds. It will grow (bone poses, projectiles, props that
// changed, beams, emitters, decals, shields, what each army can see); additions go at the end of the
// structs so both sides can be updated one after the other.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace render {

struct FrameUnit {
  uint32_t id = 0;         // the game's entity id; bits 20 to 27 are the army index
  uint32_t blueprint = 0;  // index into Frame::blueprints
  float position[3] = {0, 0, 0};
  float orientation[4] = {0, 0, 0, 1};  // quaternion x, y, z, w
  float health = 0;
  float fractionComplete = 1;           // below 1 while under construction
  uint32_t layer = 0;                   // 1 land, 2 seabed, 4 sub, 8 water, 0x10 air, 0x20 orbit
  uint64_t states = 0;                  // the original's unit-state bits
  bool dead = false;
  bool beingBuilt = false;

  uint32_t Army() const { return (id >> 20) & 0xff; }
};

struct Frame {
  uint32_t tick = 0;                    // ten ticks per second of game time
  std::vector<FrameUnit> units;         // in entity-id order
  // Blueprint ids ("uel0201") the units refer to. The list only grows, so an index stays valid.
  const std::vector<std::string>* blueprints = nullptr;
};

// Fills `out` with the units of beat `a`, each moved the share `t` (0..1) of the way to where the
// same unit is in the next beat `b` (both in entity-id order; `b` may be null). A unit that is not
// in `b`, or is another unit there (ids are reused), stays as it is in `a`.
void BlendUnits(const FrameUnit* a, size_t countA, const FrameUnit* b, size_t countB, float t, std::vector<FrameUnit>* out);

}  // namespace render
