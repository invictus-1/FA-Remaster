// The game's orders between the players (src/sim/beat_exchange.h, the engine's seam), over the lobby's connector.
// As the original's CNetClient / CClientManagerImpl (survey: _scratch/ui/netlobby_survey.md; the stream:
// engine-ref/specs/command_stream.md 1.1): each client sends every other client its own command stream, the same
// messages a replay holds ({u8 type, u16 size incl. header, payload}), with an Advance(n) (type 0) after each beat's
// messages and its checksums as VerifyChecksum (type 3: 16 bytes MD5, i32 beat) in the stream. The receiver counts a
// client's Advances: the k-th closes that client's beat k. Not yet the original's control messages (50..57: ACK,
// AVAILABLE, READY, ejects, chat, game speed), which come with the disconnect dialog.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "net/connector.h"
#include "sim/beat_exchange.h"

namespace moho::net {

struct GameClient {
  uint8_t client = 0;  // its index in the session (the merge order: CLobby::AssignClientIndex 0x7c4e80)
  int conn = 0;        // its connection on the connector (0: none; the client counts as lost from the start)
};

// What the disconnect dialog shows of a client (GetSessionClients: connected, ping, quiet).
struct ClientLink {
  bool connected = false;
  double ping = 0, quiet = 0;  // ms (quiet: since anything was last heard from it)
};

class NetBeatExchange final : public BeatExchange {
 public:
  // The lobby's connector, handed over at the launch with every other client's connection (the lobby's peers).
  NetBeatExchange(std::unique_ptr<Connector> connector, uint8_t localClient, std::vector<GameClient> peers);

  void SendLocal(uint32_t beat, const std::string& bytes) override;
  bool Collect(uint32_t beat, std::vector<ClientBatch>* out) override;
  void SendChecksum(const BeatChecksum& mine) override;
  void TakePeerChecksums(std::vector<std::pair<uint8_t, BeatChecksum>>* out) override;

  // A client left out from `beat` on (ejected; the disconnect dialog's choice).
  void Eject(uint8_t client, uint32_t beat);
  // Clients whose connection is lost (the disconnect dialog shows them while their beats are missing).
  std::vector<uint8_t> LostClients();
  uint32_t ReceivedBeats(uint8_t client);  // the beats of `client` in so far (its Advances)
  ClientLink Link(uint8_t client);
  // The game's messages a peer sent before the lobby handed its connector over (net/lobby_core.h GameLink::early).
  void TakeEarly(std::vector<ConnEvent> early);

 private:
  struct Peer {
    GameClient who;
    std::string pending;                   // its messages since its last Advance
    std::map<uint32_t, std::string> beats;  // closed beats not yet collected
    uint32_t available = 0;                 // its Advances so far
    uint32_t ejectedFrom = UINT32_MAX;
    bool lost = false;
  };
  void Pump();  // (under mu_) the connector's events into the peers' streams
  void Handle(ConnEvent& e);  // (under mu_) one of them
  Peer* ByConn(int conn);

  std::mutex mu_;
  std::unique_ptr<Connector> connector_;
  uint8_t local_ = 0;
  std::vector<Peer> peers_;
  std::map<uint32_t, std::string> localBeats_;
  uint32_t sentBeats_ = 0;  // the Advances we have sent
  std::vector<std::pair<uint8_t, BeatChecksum>> checksums_;
};

}  // namespace moho::net
