// The seam between a network game's transport (ui/net/, written by the interface chat) and the simulation.
// The original: CClientManagerImpl::UpdateStates 0x53f010 runs, for each beat b, every client's UpdateState
// 0x53c550 in client order and applies the merged messages before the beat (engine-ref command_stream.md 1.1);
// a beat whose batches are not all in waits (uimain.UpdateDisconnectDialog); every 50 beats each client's sim
// checksum is compared (ShowDesyncDialog). Survey: _scratch\ui\netlobby_survey.md.
//
// Split: the transport owns sockets, the lobby, ACK / AVAILABLE / READY, ejects, chat and game speed. The sim side
// owns the merge (source checks, command ids), the input delay and the checksum. Commands cross this seam as
// replay-stream messages ({u8 type, u16 length incl. header, payload}, without Advance), so a network game and a
// replay are the same stream to the sim.
//
// Draft v0 (2026-10-29): the shape to code against. Names may still change before step 4 (the first 2-player game).
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace moho {

// One client's messages for one beat, in the order the client wrote them.
struct ClientBatch {
  uint8_t client = 0;  // the client's index in the session, the merge order (CLobby::AssignClientIndex 0x7c4e80: human armies' owners in army order, then observers)
  std::string bytes;   // replay-stream messages, no Advance
};

// The sim's checksum of one beat (the original's, not our fingerprint).
struct BeatChecksum {
  uint32_t beat = 0;
  uint8_t digest[16] = {};
};

// Implemented by the transport. The sim thread calls every method. Implementations must be safe against their own
// network thread filling the queues meanwhile.
class BeatExchange {
 public:
  virtual ~BeatExchange() = default;

  // This client's messages for `beat` (already delayed by the input delay: the sim passes the beat they run
  // before), to send to every peer. Called once per beat, also with empty bytes (an empty batch still says
  // "nothing from me for this beat").
  virtual void SendLocal(uint32_t beat, const std::string& bytes) = 0;

  // True once every live client's batch for `beat` is in. Then fills `out` sorted by client and returns. While
  // false the beat waits (the transport drives the disconnect dialog). An ejected client is left out from the beat
  // its eject takes effect.
  virtual bool Collect(uint32_t beat, std::vector<ClientBatch>* out) = 0;

  // This client's checksum, sent every 50 beats.
  virtual void SendChecksum(const BeatChecksum& mine) = 0;

  // Peers' checksums that have arrived since the last call, for the sim to compare with its own.
  virtual void TakePeerChecksums(std::vector<std::pair<uint8_t, BeatChecksum>>* out) = 0;
};

}  // namespace moho
