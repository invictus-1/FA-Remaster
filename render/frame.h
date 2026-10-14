// The frame: a plain description of the world at one simulation tick. It is all the renderer ever
// sees of the simulation. Today a frame is filled from a tick recording; later the running simulation
// fills the same structure and hands over two consecutive ticks, which the renderer blends for display.
//
// A recording gives the units only; the running simulation also gives bone poses, projectiles and
// beams. It will grow further (props that changed, emitters, decals, shields, what each army can
// see); additions go at the end of the structs so both sides can be updated one after the other.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace render {

// What the army the picture is for knows of a place of the map (the sight grid, SimFrames::TakeSight).
enum SightBits : uint8_t {
  kSightRadar = 1,    // radar or omni covers it now
  kSightSonar = 2,    // sonar covers it now (which tells of what is on or in water)
  kSightEver = 0x40,  // it has been seen at some time
  kSightNow = 0x80,   // it is seen now
};

struct FrameUnit {
  uint32_t id = 0;         // the game's entity id; bits 20 to 27 are the army index
  uint32_t blueprint = 0;  // index into Frame::blueprints
  float position[3] = {0, 0, 0};
  float orientation[4] = {0, 0, 0, 1};  // quaternion x, y, z, w
  float health = 0, maxHealth = 0;
  float fractionComplete = 1;           // below 1 while under construction
  uint32_t layer = 0;                   // 1 land, 2 seabed, 4 sub, 8 water, 0x10 air, 0x20 orbit
  uint64_t states = 0;                  // the original's unit-state bits
  bool dead = false;
  bool beingBuilt = false;
  // How far its work is (a build, an upgrade, an enhancement), 0..1; 0: none. And whether the
  // player has paused it.
  float workProgress = 0;
  // Its shield, when it carries one (a generator's bubble, a personal shield): how much of it is left,
  // 0..1, as the shield's script last said (0: down, or none).
  float shieldRatio = 0;
  bool paused = false;
  // What the army the picture is for knows of it (the engine's ReconBits: 1 radar, 2 sonar, 4 omni,
  // 8 seen now, 0x10 seen at some time, ...). 0xff: the picture is for no one army, everything shows.
  uint8_t recon = 0xff;
  // The mesh blueprint its script set on it in place of its own (index into Frame::blueprints; 0:
  // none). A personal shield sets the unit's shielded mesh while it is up.
  uint32_t mesh = 0;
  bool Seen() const { return (recon & 8) != 0; }
  bool Blip() const { return (recon & 8) == 0 && (recon & 7) != 0; }  // known by radar, sonar or omni only
  bool SeenBefore() const { return (recon & 8) == 0 && (recon & 0x10) != 0; }  // not seen now, but it was
  // Its bones that do not stand at rest, as spans of Frame::bones: at this frame's beat, and at the
  // beat after it (the picture shows the pose Frame::blend of the way from one to the other).
  // Both empty: the unit stands in its model's rest pose.
  uint32_t firstBone = 0, boneCount = 0;
  uint32_t nextFirstBone = 0, nextBoneCount = 0;

  uint32_t Army() const { return (id >> 20) & 0xff; }
};

// One bone of a unit that is not at rest: its place and rotation in the unit's own frame, composed
// down the chain of bones from the unit's origin; positions in world units (the model's scale applied).
struct FrameBone {
  uint32_t bone = 0;  // index in the skeleton of the unit's mesh; a unit's bones are listed in rising order
  float position[3] = {0, 0, 0};
  float orientation[4] = {0, 0, 0, 1};
  bool hidden = false;  // the script has hidden this bone: what follows it is not drawn
};

struct FrameProjectile {
  uint32_t id = 0;
  uint32_t blueprint = 0;  // index into Frame::blueprints (a projectile's is its file's path)
  uint32_t army = 0;       // from 0; 0xff: none
  float position[3] = {0, 0, 0};
  float orientation[4] = {0, 0, 0, 1};
  float velocity[3] = {0, 0, 0};  // world units per second
  bool seen = true;               // by the army the picture is for
  // The size its mesh is drawn at, per axis, as the simulation holds it (the engine's
  // ProjectileView::size, docs/engine-answers.md, render 16): the blueprint's UniformScale at
  // launch, grown by its scale velocity every beat, or whatever a script's SetScale / SetDrawScale
  // wrote in its place (the UEF's box of light is sized to what is built in it). The blueprint's
  // UniformScale is NOT applied on top. sized = false: not told (a recording); then the
  // blueprint's own scale is used. growth: what the scale velocity adds a second (between beats).
  bool sized = false;
  float size[3] = {1, 1, 1};
  float growth[3] = {0, 0, 0};
  float age = 0;  // seconds since it was first seen (what fades with time goes by it)
};

// Something a script made that has a mesh and is neither a unit, a projectile nor a prop: a shield's
// bubble, a mark, a piece of a death (the engine's EntityView).
struct FrameEntity {
  uint32_t id = 0;
  uint32_t mesh = 0;     // index into Frame::blueprints: the mesh blueprint's id ("/effects/entities/shield01/shield01_mesh")
  uint32_t army = 0xff;  // from 0; 0xff: none
  float position[3] = {0, 0, 0};
  float orientation[4] = {0, 0, 0, 1};
  float scale[3] = {1, 1, 1};  // the engine's scale x draw scale (a shield's size is its draw scale)
  bool shield = false;
  bool seen = true;            // by the army the picture is for (its place's answer)
};

// How the scenery changed in a beat (the engine's PropEvent): a prop made after the start (wreckage),
// one whose mesh or place changed (a burnt tree, a tree knocked over), one that is gone. A prop of
// the map is named by its index in the map file's list of props.
struct FramePropEvent {
  enum Kind { Create, Change, Destroy } kind = Create;
  uint32_t tick = 0;  // the beat it was told in
  uint32_t id = 0;
  int mapIndex = -1;  // -1: made after the start
  std::string blueprint, mesh;  // mesh: what a script set in place of the blueprint's own ("": nothing)
  float position[3] = {0, 0, 0};
  float orientation[4] = {0, 0, 0, 1};
  float scale[3] = {1, 1, 1};  // the engine's scale x draw scale
};

// A weapon's beam while it is on.
struct FrameBeam {
  uint32_t id = 0;
  uint32_t army = 0;
  float start[3] = {0, 0, 0};
  float end[3] = {0, 0, 0};
};

// What a script did with an effect in one beat: the engine's EffectEvent (src/sim/sim_view.h,
// which says what each function fills), copied out.
struct FrameEffectEvent {
  enum Kind { Create, Call, Destroy } kind = Create;
  uint32_t tick = 0;        // the beat it happened in
  uint32_t id = 0;          // the effect's own number
  std::string name;         // the function ("CreateEmitterAtBone") or the method ("ScaleEmitter")
  std::string blueprint;    // Create: the emitter / beam / trail blueprint, or the texture
  int army = 0;
  uint32_t entity = 0xffffffff, entity2 = 0xffffffff;  // what it is on, and a beam's other end (0xffffffff: none)
  int bone = -1, bone2 = -1;
  float position[3] = {0, 0, 0};  // Create: where the attach point was
  // Create, with a second entity: where that far end was (the engine says it also for an entity
  // that has no mesh and so is in no list: the hubs of an adjacency's link)
  bool hasPosition2 = false;
  float position2[3] = {0, 0, 0};
  bool seen = true;               // Create: the army the picture is for sees that place
  std::vector<float> numbers;
  std::string text, text2;
};

// How a blueprint looks, as the simulation has it after the game's scripts and the mods changed it
// (the engine's BlueprintDisplay, copied out): what the renderer otherwise reads from .bp files.
struct BlueprintLook {
  struct Level {
    std::string mesh, albedo, normals, specular, secondary, shader;  // virtual paths; empty: not set
    float cutoff = 0;                                                // LODCutoff (0: not set)
  };
  std::string id;   // as a frame's blueprint names say it (a unit's id, a projectile's or prop's .bp path)
  int kind = 0;     // 0 unit, 1 projectile, 2 prop
  float scale = 1;  // Display.UniformScale
  std::vector<Level> levels;
  // units only; 0 or empty: the blueprint does not say
  std::string icon;
  int iconPriority = 0;
  float iconFadeIn = 0;
  float selectionX = 0, selectionZ = 0;
  float lifeBarSize = 0, lifeBarHeight = 0, lifeBarOffset = 0;  // a unit's life bar, world units (0: not said)
};

struct Frame {
  uint32_t tick = 0;                    // ten ticks per second of game time
  std::vector<FrameUnit> units;         // in entity-id order
  std::vector<FrameBone> bones;         // what the units' spans point into
  float blend = 0;                      // how far the moment shown is from this beat to the next, 0..1
  std::vector<FrameProjectile> projectiles;  // moved on to the moment shown
  std::vector<FrameBeam> beams;
  std::vector<FrameEntity> entities;         // moved on to the moment shown
  // Blueprint ids the units ("uel0201") and projectiles ("/projectiles/.../x_proj.bp") refer to.
  // The list only grows, so an index stays valid.
  const std::vector<std::string>* blueprints = nullptr;
};

// Fills `out` with the units of beat `a`, each moved the share `t` (0..1) of the way to where the
// same unit is in the next beat `b` (both in entity-id order; `b` may be null). A unit that is not
// in `b`, or is another unit there (ids are reused), stays as it is in `a`.
// Bones: a unit keeps `a`'s span and gets, as its next span, `b`'s moved by `nextBoneOffset` (where
// the caller puts `b`'s bones in the frame's list behind `a`'s); without `b`, its own span again.
void BlendUnits(const FrameUnit* a, size_t countA, const FrameUnit* b, size_t countB, float t, std::vector<FrameUnit>* out,
                uint32_t nextBoneOffset = 0);

}  // namespace render
