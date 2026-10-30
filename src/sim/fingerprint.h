// The per-beat fingerprint (MOHO64_FINGERPRINT=<file>): one text line per beat with a hash of the simulation's
// state after that beat and one hash per part of it, so that two copies of one game can be compared beat by beat
// (tools/twocopies.py, docs/two-copies.md). Multiplayer in this game is lockstep: every player's machine runs the
// whole simulation from the same orders, so two copies fed the same orders must stay identical on every beat;
// whatever depends on the copy itself (an address, a hash of one, memory never written, the clock, a thread's
// timing, the order a folder lists its files in) breaks that, and the first beat whose line differs names it.
//
// It only reads the state, and nothing reads it back: on or off, every result of the simulation is the same. Off
// (the default) it costs one test of a null pointer per beat in the host; on, about 1 ms a beat in the 13 test
// replays and 5 ms in an eight-army game on a map with 34,000 props (the props are most of it).
//
// The file (text, one line per beat; with a peer, flushed every beat):
//   # moho64 fingerprint 1 seed <seed> armies <n>
//   <beat> <tick> <total> <part>=<hash> <part>=<hash> ...
//   # end <beats>                      (the run ended normally)
//   # differs <beat>                   (the peer's line of that beat differs: see below)
// <beat> counts every beat the host ran, paused ones included (0 = right after Start); <tick> is the game tick.
// Hashes are 64-bit, in hex. Floats are hashed by their bits; every part is hashed in entity-id order (or, where
// there is no id, in an order the simulation itself keeps: creation, army index), never in a container's or an
// address's order. The parts:
//   rng      the sim's random stream: the session seed and the number of draws (the stream is MT19937 seeded once
//            and every draw goes through Sim::NextUInt32, so the two give its whole state)
//   sim      the sim's own: the path searches waiting their turn (unit ids, in order), the next command id, the
//            command source, the pause, the playable rect, the numbers of live commands (not the focus army: in a
//            multiplayer game each player's copy looks through its own)
//   lua      (last, and not a hash) <KB>:<KB>:<serial>: the sim state's Lua heap and its collector threshold
//            (lua_getgccount / lua_getgcthreshold) and the serial its next table, userdata, function or thread
//            takes (lua_getnextserial: how many it has made). Kept out of <total> and of the peer check: copies
//            that look through different armies make different Sync tables, and a log line may land in one
//            copy only, so these may differ between two copies whose games do not (docs/two-copies.md)
//   a<N>     army N (1-based): its economy, statistics, unit cap and count, alliances, platoons and squads
//   u<N>     army N's units: id, blueprint, position, orientation, health, build fraction, layer, states, flags,
//            motion velocity, command queue and rally queue, weapons' targets and clocks, economy figures, recon
//   proj     projectiles: id, blueprint, army, position, orientation, velocity, target, damage, expiry, bounces
//   props    props: id, blueprint, position, orientation, health, build (reclaim) fraction
//   ents     every other entity (shields, beams, blips, the scripts' own): id, kind, position, orientation, health
// <total> hashes every part's hash (lua's is not one).
//
// Two copies running at once can check each other as they go (MOHO64_FINGERPRINT_PEER=<the other copy's file>):
// after writing its own line, a copy waits for the peer's line of the same beat (up to
// MOHO64_FINGERPRINT_WAIT seconds, default 600, of the peer writing nothing) and compares the two. At the first
// beat they differ, both copies write the full state of that beat beside their file (<file>.detail: one line per
// army, unit, projectile, prop and entity, floats with 9 digits, which give a float's exact value) and stop. A
// peer that has ended (# end) is not waited for. MOHO64_FINGERPRINT_DETAIL=<beat>[,<beat>...] writes the same
// listing at the beats named, to <file>.detail.<beat> (one copy alone, e.g. a divergence that repeats from run to
// run).
#pragma once
#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace moho {

class Sim;
class Prop;

class Fingerprint {
 public:
  // From the environment (see above); nullptr when MOHO64_FINGERPRINT is not set. The file is MOHO64_FINGERPRINT
  // + suffix (moho64 --together: ".2" for its second copy).
  static std::unique_ptr<Fingerprint> FromEnvironment(const std::string& suffix = std::string());
  ~Fingerprint();
  // The state after beat `beat` (0: after Start). False: the peer's line of this beat differs (the detail is
  // written; the host should stop).
  bool Beat(const Sim& sim, uint32_t beat);
  // The run ended after `beats` beats.
  void End(uint32_t beats);
  // The line Beat writes for the state after beat `beat`, made without a Fingerprint (no MOHO64_FINGERPRINT needed;
  // the props are sorted afresh, so a call costs a little more than Beat's): sim/lockstep.h LockstepDigest.
  static std::string StateLine(const Sim& sim, uint32_t beat);

 private:
  std::string Line(const Sim& sim, uint32_t beat) const;
  // The props in id order, sorted again only when the props list no longer holds the same entities in the same order
  // (handles are never given out twice).
  const std::vector<const Prop*>& PropsById(const Sim& sim) const;
  void WriteDetail(const Sim& sim, uint32_t beat, const std::string& path, const char* why) const;
  bool PeerLine(uint32_t beat, std::string* line);  // false: the peer ended or went silent
  FILE* out_ = nullptr;
  std::string path_;
  bool headerDone_ = false;
  FILE* peer_ = nullptr;
  std::string peerPath_;
  std::string peerBuf_;  // what was read of the peer's file past its last whole line
  bool peerGone_ = false;
  double waitSeconds_ = 600;
  std::set<uint32_t> detailBeats_;
  uint32_t beats_ = 0;
  double hashSeconds_ = 0;
  mutable std::vector<uint32_t> propHandles_;  // (PropsById: the props list's handles when it last sorted)
  mutable std::vector<const Prop*> propsById_;
};

}  // namespace moho
