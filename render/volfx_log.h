// A debug print, off by default (MOHO64_DEBUG_VOLFX=1): which of the game's emitters are drawn twice with the volume
// explosions (volume_blasts.h). A blast's volume is drawn over the game's own effects; an emitter whose blueprint says
// VolumeReplaces is not started while volumes are on (particles.cpp). To find which others draw what the volume draws
// (a fireball, a body of smoke), this lists every emitter started within `window` seconds of a volume blast's birth (a
// nuke's: from a second before to `nukeWindow` after) and inside its radius: its blueprint, its texture and ramp, how it
// blends and how large its particles are, what the event that made it was made on (a dying unit, a shot's impact) and
// what made the blast (a flash of that size, harm to an area of that radius). At the end, and every few minutes of the
// game, a summary per emitter: how many blasts it was started in, how often it was started where no blast was (an
// explosion too small for a volume shares it), and on what.
// Lines start with "volfx" and their fields are separated by tabs (for a script to read).
//
// It only reads: nothing drawn changes, with it on or off. Every hook is one line behind VolfxLog::Get() (nullptr
// unless the variable is set): Particles::Start, Effects::Apply (what an emitter or a flash is made on, ScaleEmitter),
// World::AddFrame (what harmed an area), VolumeBlasts::Offer / OfferArea / Start / Update.
#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "linalg.h"

namespace render {

struct EmitterBlueprint;
struct Frame;
struct FrameEffectEvent;

class VolfxLog {
 public:
  // The log, or nullptr when MOHO64_DEBUG_VOLFX is not set (checked once).
  static VolfxLog* Get();
  ~VolfxLog();

  float window = 0.3f;  // seconds before or after a blast's birth an emitter counts as started with it
  float nukeWindow = 10;  // ... after a nuke's (its fireball, plume and cloud are started over its first seconds; before it: 1 s)

  // --- what made what (Effects::Apply, World::AddFrame) ---
  // The event about to start an emitter or throw a flash, and the frame it is played in (what it is made on).
  void SetOrigin(const FrameEffectEvent& event, const Frame& frame);
  void ClearOrigin() { origin_.clear(), effect_ = 0; }
  // A frame's shots, remembered by id a while: an impact's effects are made on a shot already gone from the frame.
  void SeeShots(const Frame& frame);
  // ScaleEmitter on an effect (the size the script gives it after it is made).
  void Scaled(uint32_t effect, float scale);
  // What harmed an area (the hit about to be offered to the volumes).
  void SetArea(const std::string& weapon, const std::string& projectile, const std::string& damageType, float amount);

  // --- the particles (Particles::Start; `replaced`: not started, VolumeReplaces) ---
  void Emitter(const std::string& path, const EmitterBlueprint& bp, Vec3 at, float scale, double born, bool replaced);

  // --- the volumes (VolumeBlasts) ---
  // A flash or an area's harm offered as a blast (before Start), then what Start made of it: a new blast (`fresh`), or
  // one more of the blast at `at`, born `born` (a nuke's, or the same blast told again); `radius` its radius now.
  void Offered(bool flash, float size, const std::string& ramp, Vec3 at, double born);
  void Into(Vec3 at, double born, float radius, bool nuke, bool fresh);
  // A flash or an area's harm that would have been a blast and is not one: no explosion's (Effects::Explosion: on a living
  // unit, a prop, a warp-in), or harm high in the air or under the water (World::AreaMakesVolume). Counted by what made it.
  void Declined(bool flash, float size);
  // The clock the blasts are drawn at: blasts and emitters old enough to be decided are printed and let go of.
  void Tick(double clock);

 private:
  struct Started {
    double born = 0;
    Vec3 at;
    std::string path, texture, ramp, origin;
    uint32_t effect = 0;
    float scale = 1, size = 0, lifetime = 0, particleLife = 0, count = 0, speed = 0;
    int blend = 0;
    bool flat = false, replaced = false, matched = false, sixWay = false, normals = false;
  };
  struct Blast {
    int id = 0;
    double born = 0;
    Vec3 at;
    float radius = 0;
    bool nuke = false;
    std::vector<std::string> made;  // what made it: "flash 12 ramp_fire_06 on unit:uel0303:dead", "area 4 ..."
  };
  struct Tally {  // per emitter blueprint, over the run
    int blasts = 0, startsIn = 0, starts = 0, out = 0, replaced = 0;
    float largest = 0;  // its largest size x scale seen in a blast
    std::string texture, ramp, hint;
    int blend = 0;
    std::map<std::string, int> outOn, inOn;  // started without a blast / in one: on what (top few printed)
  };
  void Print(const Blast& b);
  void Finish(Started& s);
  void Summary(const char* why);
  std::string Hint(const Started& s, float radius) const;

  std::string origin_, area_, offer_;
  uint32_t effect_ = 0;
  std::deque<Started> started_;
  std::vector<Blast> blasts_;
  std::unordered_map<std::string, Tally> tally_;
  std::unordered_map<uint32_t, std::string> shots_;  // shot id -> its blueprint, as last seen
  std::unordered_map<uint32_t, float> scales_;       // effect id -> its ScaleEmitter
  std::map<std::string, int> declined_;               // what made no blast that once would have (Declined)
  int declinedCount_ = 0;
  int nextId_ = 1, printed_ = 0;
  double clock_ = 0, lastSummary_ = 0;
};

}  // namespace render
