// The lobby's network side (the original's CLobby, FA exe 0x7c0970 .. 0x7cbd20; docs in _scratch/ui/netlobby_survey.md
// section 3): the host, the joiners, every pair of players connected directly (a full mesh), and the lobby messages
// 100..107 between them. No Lua here (ui/net/ stays free of the interface): the interface's binding
// (ui/user/lobby.cpp) turns the hooks below into the script's callbacks and the script's tables into ScriptData bytes.
//
// Messages (CLobby::ReceiveMessage 0x7c62f0; the payloads as read there):
//   100 Join             joiner -> host: str name, i32 uid (-1 unless given)
//   101 Rejected         host -> one: str reason ("LobbyFull", or an eject's reason)
//   102 Welcome          host -> joiner: str host name, i32 host uid, i32 the joiner's uid, str its name made valid,
//                        u32 the host's nonce (the session's seed)
//   103 NewPeer          host -> both of a new pair: str name, u32 IPv4, u16 port, i32 uid
//   104 DeletePeer       host -> all: i32 uid
//   105 EstablishedPeers each -> each: i32 uid of each peer it is established with, then i32 -1
//   106 ScriptData       broadcast: the Lua table's bytes        107 ScriptData to one: the same
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "net/connector.h"

namespace moho::net {

struct LobbyHooks {
  std::function<void()> hosting;
  std::function<void(const std::string& reason)> connectionFailed;
  std::function<void(int localUid, const std::string& localName, int hostUid)> connectionToHostEstablished;
  std::function<void(const std::string& bytes, int senderUid, const std::string& senderName)> dataReceived;
  std::function<void(const std::string& name, int uid)> peerDisconnected;
  std::function<void(const std::string& reason)> ejected;
  std::function<void(const std::string& text)> systemMessage;
  std::function<void(int uid, const std::vector<int>& established)> establishedPeers;
};

// The launch's hand-over (CLobby::LaunchGame 0x7c38c0: the client manager is made with the lobby's connector,
// CLIENT_CreateClientManager at 0x7c4a8e, and each peer's connection goes over to it, the lobby's receivers removed
// from it, RemoveLinkage 0x7c4b5d / 0x7c4b89): the connector, every peer's connection by uid, and the game's own
// messages (types below 100: the command stream 0..49, the control messages 50..59) a peer sent before this
// lobby launched (INFERRED: the original's receivers for those are only pushed by CNetClient, so the lobby drops
// them; we keep them, as a fast peer's first beats may come in the frame before the launch).
struct GameLink {
  std::unique_ptr<Connector> connector;
  int localUid = -1;
  uint32_t nonce = 0;                // (the session's seed: the host's, or from its Welcome)
  std::map<int, int> connByUid;      // every peer that has a connection
  std::vector<ConnEvent> early;
};

struct PeerInfo {  // (GetPeer / GetPeers: SPeer::ToLua 0x7c2950)
  std::string name;
  int uid = -1;
  std::string status;  // Unknown, Connecting, Connected, Pending, WaitingJoin, Established, Disconnected
  double ping = 0, quiet = 0;
  std::vector<int> established;
};

class LobbyCore {
 public:
  LobbyCore(std::unique_ptr<Connector> connector, std::string name, int uid, int maxConnections);
  LobbyHooks hooks;

  int Uid() const { return uid_; }
  const std::string& Name() const { return name_; }
  bool IsHost() const { return host_; }
  uint16_t Port() const { return connector_ ? connector_->LocalPort() : 0; }
  uint32_t Nonce() const { return nonce_; }  // the host's (as host, or from its Welcome)

  void Host();
  void Join(const Address& host);
  void ConnectToPeer(const Address& at, const std::string& name, int uid);
  void DisconnectFromPeer(int uid);
  bool EjectPeer(int uid, const std::string& reason);  // host only
  void Broadcast(const std::string& bytes);
  bool SendTo(int uid, const std::string& bytes);  // false: no such peer
  std::string MakeValidPlayerName(int uid, const std::string& name) const;
  std::vector<PeerInfo> Peers() const;
  bool HasPeer(int uid) const { return peers_.count(uid) != 0; }

  void Pump();  // each frame: the connector's events, the lobby's messages

  // A network lobby: a connector and at least one peer (the original's LaunchGame asks the connector,
  // lobby+0x78 vtable+8; INFERRED: a host alone on TCP / UDP plays a local game, as with "None").
  bool Networked() const { return connector_ && !peers_.empty(); }
  // The launch: the network goes to the game (GameLink); the lobby has none after it.
  GameLink TakeForGame();

 private:
  enum class State { Unknown, Connecting, Connected, Pending, WaitingJoin, Established, Disconnected };
  struct Peer {
    std::string name;
    int uid = -1;
    Address address;
    State state = State::Unknown;
    int conn = 0;
    std::vector<int> established;
  };
  static const char* StateName(State s);
  Peer* PeerByConn(int conn);
  void Send(Peer& p, const Message& m);
  void OnIncoming(int conn);
  void OnMade(int conn, bool incoming);
  void OnFailed(int conn);
  void OnLost(int conn);
  void OnMessage(int conn, const Message& m);
  void Remove(int uid, bool tellScript);
  void TellEstablished();
  Message NewPeerMessage(const Peer& p) const;
  void SendInfo(Peer& to, const Peer& about);

  std::unique_ptr<Connector> connector_;
  std::string name_;
  int uid_ = -1;
  int maxConnections_ = 16;
  bool host_ = false, joining_ = false;
  int hostConn_ = 0;  // (joiner: the connection to the host)
  int hostUid_ = -1;
  int nextUid_ = 0;
  uint32_t nonce_ = 0;
  std::map<int, Peer> peers_;            // by uid
  std::map<int, int> pending_;           // incoming connections, not joined yet: conn -> 0
  std::map<int, int> outgoingPeers_;     // conn -> the uid it is for (ConnectToPeer)
  std::set<int> lastEstablished_;
  std::vector<ConnEvent> early_;  // (the game's messages heard before the launch: GameLink)
};

}  // namespace moho::net
