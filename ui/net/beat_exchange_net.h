// The game's orders between the players (src/sim/beat_exchange.h, the engine's seam), over the lobby's connector.
// As the original's CNetClient / CClientManagerImpl (survey: _scratch/ui/netlobby_survey.md; the stream:
// engine-ref/specs/command_stream.md 1.1): each client sends every other client its own command stream, the same
// messages a replay holds ({u8 type, u16 size incl. header, payload}), with an Advance(n) (type 0) after each beat's
// messages and its checksums as VerifyChecksum (type 3: 16 bytes MD5, i32 beat) in the stream. The receiver counts a
// client's Advances: the k-th closes that client's beat k.
//
// Beside the stream, the original's control messages (CClientBase::Process 0x53bf30, its jump table 0x53c31c; each
// "broadcast" goes to every other client and is also processed here as if it came from this client, as the original's
// CClientManagerImpl 0x53e990 hands it to every client object, the local one among them, CLocalClient::Process
// 0x53d190):
//   50 ACK {u8 client, i32 n}: "I have that client's beats up to n", broadcast on every Advance taken in (0x53bfe0).
//      A beat is handed to the sim only once every client still in the game has acknowledged every client's part of it
//      (CClientManagerImpl::DoBeat 0x53ea30: EveryoneResponsiveSince 0x53ef30 / IsReadyForBeat 0x53c3e0), so that an
//      eject is carried out at the same beat everywhere.
//   51 DISPATCHED {i32 beat} (UpdateStates, FAF's 0x12900c0), 52 AVAILABLE {i32 beat} (DoBeat 0x53eb91): kept, shown
//      nowhere (the original's bottleneck report goes to GPGNet only).
//   53 READY {} (0x53e4b0, at the end of DoInitializing 0x88c664): the game's screen is up; the beats wait until every
//      client is ready (IsEveryoneReady 0x53e560; DoPostInitializing 0x88c6d0 / DoWaiting 0x88c750).
//   54 EJECT {u8 client, i32 n}: "eject that client after its beat n" (ProcessEject 0x53f2c0).
//   55 CHAT {the message} (CClientBase::ReceiveChat 0x53c9a0 sends, Process 0x53c228 hands it to the interface).
//   56 GAME SPEED {i32 serial, i32 rate} (SetSimRate 0x53e590, ApplyIncomingGameSpeedRequest 0x53e810).
//   57 {i32}: the sender's own most sim speed ("maxSP", client+0xd4; FAF's sim driver sends it, 0x129004e).
#pragma once

#include <chrono>
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

// What the disconnect dialog shows of a client (GetSessionClients, CWldUiInterface 0x88d540).
struct ClientLink {
  bool connected = false;          // not ejected by this program (vtable slot 1, CClientBase::NoEjectionPending 0x53c960)
  double ping = 0, quiet = 0;      // ms (quiet: since anything was last heard from it; -1: no connection, 0x53dd1f)
  std::vector<uint8_t> ejectedBy;  // the clients that asked to eject it, in the order they asked (CollectEjecters 0x53cc60)
  int maxSP = 50;                  // its message 57 (client+0xd4, 50 until one comes: CClientBase ctor 0x53be07)
};

class NetBeatExchange final : public BeatExchange {
 public:
  // The lobby's connector, handed over at the launch with every other client's connection (the lobby's peers).
  // speedAdjustable / rate: the game's option (CClientManagerImpl ctor 0x53dfcf / 0x53dfe1: +0x454, +0x450).
  NetBeatExchange(std::unique_ptr<Connector> connector, uint8_t localClient, std::vector<GameClient> peers,
                  bool speedAdjustable = false, int rate = 0);

  void SendLocal(uint32_t beat, const std::string& bytes) override;
  bool Collect(uint32_t beat, std::vector<ClientBatch>* out) override;
  void SendChecksum(const BeatChecksum& mine) override;
  void TakePeerChecksums(std::vector<std::pair<uint8_t, BeatChecksum>>* out) override;

  // A client left out from `beat` on, on this program's word alone (no vote; the engine's two-copies test,
  // MOHO64_NET_EJECT: every copy calls it with the same beat).
  void Eject(uint8_t client, uint32_t beat);
  // EjectSessionClient (CClientBase::Eject 0x53cb10): this program's vote to eject `client`. Its connection is closed,
  // it counts as ready, and every other client hears "eject it after its beat n", n = its beats in here so far
  // (ProcessEject 0x53f2c0); the eject requests it made itself are dropped. It is left out after the smallest n any
  // client asked for (GetMostExpiredEjectRequest 0x53cc20), on every client the same, once every client still in the
  // game has asked (until then the beats wait: a client that did not ask still waits for its beats).
  void RequestEject(uint8_t client);
  // READY: this program's game screen is up (0x53e4b0); EveryoneReady: every client is ready or ejected (0x53e560).
  void SendReady();
  bool EveryoneReady();
  // CHAT to one client (this one too: then it comes back through TakeChats, as the original's local client hears it).
  void SendChat(uint8_t client, const std::string& bytes);
  std::vector<std::pair<uint8_t, std::string>> TakeChats();  // (sender, message), oldest first
  // GAME SPEED: a request to every client (SetSimRate 0x53e590; nothing when the game's speed is not adjustable), the
  // rate agreed (the newest request, the lower client index on a tie: 0x53e810; GetSimRateRequested 0x53e7e0) and who
  // changed it to what, for the interface (uimain.NoteGameSpeedChanged, through CWldUiInterface 0x88b960).
  void RequestRate(int rate);
  int Rate();
  std::vector<std::pair<uint8_t, int>> TakeRateNotes();
  // Clients whose connection ended since the last call (the original's "%s disconnected.", 0x88b9d0). A connection
  // that ends ejects its client at once, as the original's messages 202 / 203 do (0x53c2c3).
  std::vector<uint8_t> TakeDisconnects();

  // Clients whose connection is lost and who are not ejected (since a lost connection ejects at once: only one that
  // Eject(client, beat) left out keeps its link, so this is empty but for a client with no connection from the start).
  std::vector<uint8_t> LostClients();
  uint32_t ReceivedBeats(uint8_t client);  // the beats of `client` in so far (its Advances)
  // Every client's part of `beat` is in (or the client is left out), as last taken in: the connector is not polled
  // (a test's timing log asks it, GameSession::NetBeatOrders, and must not change when things are taken in).
  bool OrdersIn(uint32_t beat);
  ClientLink Link(uint8_t client);
  // The game's messages a peer sent before the lobby handed its connector over (net/lobby_core.h GameLink::early).
  void TakeEarly(std::vector<ConnEvent> early);
  // The connector's events taken in now (while the game loads: the TCP connector answers its pings only when polled,
  // and the others would see this program go quiet).
  void Service();
  // A test's switch: from now on nothing is sent or taken in (the connector is no longer polled), as a program that
  // stopped answering.
  void FreezeForTests();

 private:
  struct Client {
    GameClient who;
    bool local = false;
    std::string pending;                    // (a peer's) messages since its last Advance
    std::map<uint32_t, std::string> beats;  // (a peer's) closed beats not yet collected
    uint32_t available = 0;                 // its Advances in so far (client+0xa0)
    std::map<uint8_t, uint32_t> acks;       // what it has acknowledged of each client's beats (client+0xb0)
    uint32_t dispatched = 0, availableBeat = 0;  // its DISPATCHED / AVAILABLE (client+0xbc / +0xa8)
    bool ready = false;                     // READY, or ejected (client+0x54)
    bool ejected = false;                   // ejected by this program (client+0xc0)
    bool done = false;                      // left out of the beats from now on (client+0xc1)
    uint32_t forcedFrom = UINT32_MAX;       // Eject(client, beat)
    std::vector<std::pair<uint8_t, uint32_t>> requests;  // eject requests against it: (requester, n) (client+0xc4)
    int maxSP = 50;
    bool lost = false;                      // its connection ended
    std::chrono::steady_clock::time_point heard = std::chrono::steady_clock::now();  // last message from it
  };
  void Pump();                                   // (under mu_) the connector's events in
  void Handle(ConnEvent& e);                     // (under mu_) one of them
  void Process(Client& from, const Message& m);  // (under mu_) one message of a client (the local one: our own)
  void Broadcast(const Message& m);              // (under mu_) to every client, this one too
  void SendTo(const Client& c, const Message& m);
  bool Reachable(const Client& c) const;
  uint32_t LastBeatOf(const Client& c) const;    // the beat after which an ejected client is left out
  bool ReadyForBeat(const Client& c, uint32_t beat) const;
  void UpdateAvailable();
  void EjectLocked(Client& c);                   // (under mu_) RequestEject's work
  Client* ByConn(int conn);
  Client* ByIndex(uint8_t client);
  Client& Local() { return *ByIndex(local_); }

  std::mutex mu_;
  std::unique_ptr<Connector> connector_;
  uint8_t local_ = 0;
  std::vector<Client> clients_;  // (every client, this one too, sorted by index)
  std::map<uint32_t, std::string> localBeats_;
  uint32_t sentBeats_ = 0;       // the Advances we have sent
  uint32_t availableBeat_ = 0;   // every client has acknowledged every client's beats up to it (+0x43c)
  uint32_t dispatched_ = 0;      // beats handed to the sim (+0x438)
  bool readySent_ = false, everyoneReady_ = false;  // (+0x434, +0x435)
  bool adjustable_ = false;      // (+0x454)
  uint32_t rateSerial_ = 0;      // (+0x448)
  uint8_t rateFrom_ = 0xff;      // (+0x44c)
  int rate_ = 0;                 // (+0x450)
  bool frozen_ = false;
  std::vector<std::pair<uint8_t, BeatChecksum>> checksums_;
  std::vector<std::pair<uint8_t, std::string>> chats_;
  std::vector<std::pair<uint8_t, int>> rateNotes_;
  std::vector<uint8_t> disconnects_;
};

}  // namespace moho::net
