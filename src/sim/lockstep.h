// A network game's beats (BeatExchange v1, the sim side): every client runs the same beats with the same orders.
// The host program (moho64ui, or headless moho64 in the two-process tests) owns one Lockstep per game and calls it
// in place of applying its own orders directly:
//
//   each turn of the game loop:
//     lockstep.Queue(writer.Take());               // this client's new orders (any number of calls per beat)
//     std::string merged;
//     if (!lockstep.Ready(&merged)) wait;          // a peer's batch is missing: the beat waits (disconnect dialog)
//     Record(merged); ApplyCommands(sim, merged);  // every client's orders, in client order
//     run the beat; Record(Advance);
//     lockstep.AfterBeat(fingerprintDigest);       // the checksum every 50 beats, the peers' compared
//
// The original: CClientManagerImpl::UpdateStates 0x53f010 runs, for each beat, every client's UpdateState 0x53c550
// in client order and applies the merged messages before the beat (engine-ref command_stream.md 1.1); a client's
// orders written while beat b runs are carried out `inputDelay` beats later on every client, so each client sends
// beats 1..inputDelay empty at the start. Every 50 beats each client sends its checksum (VerifyChecksum) and a
// difference shows the desync dialog. We are not network-compatible with the original or FAF (docs: the ship
// model), so the checksum is our own state digest; the merge, the delay and the order rules follow the original.
//
// v1, 2026-10-30. The transport (ui/net/beat_exchange_net.h) is the ui's; this file and lockstep.cpp are the
// engine's. Not thread-safe: the game loop's thread calls everything.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "sim/beat_exchange.h"

namespace moho {

class Sim;

struct LockstepClient {
  uint8_t client = 0;          // index in the session, the merge order: CLobby::AssignClientIndex 0x7c4e80 numbers the human armies' owners in army order, then the observers (the host is 0 only when it holds the first human army)
  std::vector<int> sources;    // the command sources (SessionDescription header sources) this client may speak for
};

struct LockstepDesync {
  uint32_t beat = 0;
  uint8_t client = 0;          // the peer whose checksum differs from ours
};

class Lockstep {
 public:
  // exchange: the transport (not owned; outlives this). local: this client's index. clients: every client of the
  // session, the local one included. inputDelay: beats between writing an order and carrying it out.
  Lockstep(BeatExchange* exchange, uint8_t local, std::vector<LockstepClient> clients, uint32_t inputDelay);

  // Sends beats 1..inputDelay empty (once, before the first beat).
  void Start();

  // This client's orders since the last call (a CommandWriter::Take(): led by SetCommandSource), kept until the next
  // send. May be empty.
  void Queue(const std::string& orders);

  // The next beat's orders. On the first call for a beat, this client's queued orders are sent for beat
  // (next + inputDelay). True when every live client's batch for the next beat is in: *merged = their messages in
  // client order, each batch checked against its client's sources (UpdateState 0x53c550: each client's part led by
  // a SetCommandSource of its current source, which it keeps from beat to beat; a SetCommandSource naming a source
  // not its own is dropped and leaves it with none, so its following messages are dropped too until it names one of
  // its own; every dropped message is counted in rejected()); the beat may then run. A client that the exchange
  // leaves out (ejected) has its sources terminated at its place in the order, once. On true, nextBeat() moves on.
  // False: wait and call again (more Queue calls in between are fine).
  bool Ready(std::string* merged);

  // After the beat Ready() released has run (paused beats too). digest: this client's state digest of that beat
  // (16 bytes: LockstepDigest below); used on beats that are a multiple of kLockstepChecksumPeriod (empty otherwise
  // is fine).
  void AfterBeat(const std::string& digest);

  uint32_t nextBeat() const { return next_; }   // the beat Ready() is waiting for (1-based)
  uint32_t rejected() const { return rejected_; }
  // Checksum differences found so far (the desync dialog's list); cleared by the caller as it shows them.
  std::vector<LockstepDesync>& desyncs() { return desyncs_; }

 private:
  BeatExchange* ex_;
  uint8_t local_;
  std::vector<LockstepClient> clients_;
  uint32_t delay_;
  uint32_t next_ = 1;
  bool sentForNext_ = false;
  std::string queued_;
  uint32_t rejected_ = 0;
  std::vector<std::pair<uint32_t, std::string>> mine_;  // our digests not yet compared (beat, digest)
  std::vector<LockstepDesync> desyncs_;
  // (v1.1, net helper) each client's command source now: UpdateState's client+0x50, kept from beat to beat (0xff:
  // none, its messages are dropped); a client whose batches stopped (ejected) has had its sources terminated
  std::vector<int> current_;
  std::vector<bool> gone_;
  // peers' checksums of beats we have not run yet (they travel beside the orders, not after them as the original's)
  std::vector<std::pair<uint8_t, BeatChecksum>> early_;
};

// The digest AfterBeat wants: 16 bytes of our own hash of the beat's state line (the line MOHO64_FINGERPRINT writes,
// sim/fingerprint.h, without its lua part), so two clients' digests differ exactly when their fingerprints do. Costs
// what one fingerprint line costs (about 1-5 ms): call it only on the beats AfterBeat uses (multiples of
// kLockstepChecksumPeriod). Needs no MOHO64_FINGERPRINT.
constexpr uint32_t kLockstepChecksumPeriod = 50;  // sim_ChecksumPeriod's default (0xbdbc33)
std::string LockstepDigest(const Sim& sim, uint32_t beat);

}  // namespace moho
