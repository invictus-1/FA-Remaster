#include "net/lobby_core.h"

#include <algorithm>
#include <cctype>
#include <chrono>

namespace moho::net {
namespace {

constexpr uint8_t kJoin = 100, kRejected = 101, kWelcome = 102, kNewPeer = 103, kDeletePeer = 104, kEstablishedPeers = 105,
                  kScriptBroadcast = 106, kScriptOne = 107;

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

LobbyCore::LobbyCore(std::unique_ptr<Connector> connector, std::string name, int uid, int maxConnections)
    : connector_(std::move(connector)), name_(std::move(name)), uid_(uid), maxConnections_(maxConnections) {
  if (connector_) connector_->SetLocalUid(uid_);
}

const char* LobbyCore::StateName(State s) {  // (the original's names, 0x7c0560 / table 0xdfeb14)
  switch (s) {
    case State::Unknown: return "Unknown";
    case State::Connecting: return "Connecting";
    case State::Connected: return "Connected";
    case State::Pending: return "Pending";
    case State::WaitingJoin: return "WaitingJoin";
    case State::Established: return "Established";
    case State::Disconnected: return "Disconnected";
  }
  return "Unknown";
}

// HostGame (0x7c1b20): our uid the next free one when none was given (0 for the first), the session's nonce from the
// host's timer.
void LobbyCore::Host() {
  host_ = true;
  if (uid_ == -1) uid_ = nextUid_++;
  else nextUid_ = std::max(nextUid_, uid_ + 1);
  if (connector_) connector_->SetLocalUid(uid_);
  nonce_ = static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  if (hooks.hosting) hooks.hosting();
}

// JoinGame (0x7c1da0): the connection to the host; Join goes once it is made (OnConnectionMade 0x7c5ca0).
void LobbyCore::Join(const Address& host) {
  joining_ = true;
  if (hooks.systemMessage) hooks.systemMessage("Connecting to game host...");
  if (!connector_) {
    if (hooks.connectionFailed) hooks.connectionFailed("HostLeft");
    return;
  }
  hostConn_ = connector_->Connect(host);
}

// ConnectToPeer (0x7c7190): a peer the host told us of (NewPeer), or the script's own call. UDP: both ends connect
// (the original's connector makes one link of both ends' CONNECT packets, and Connect takes one waiting from there);
// TCP (ours): of a pair, the one with the lower uid connects, the other waits for it.
void LobbyCore::ConnectToPeer(const Address& at, const std::string& name, int uid) {
  if (uid == uid_ || peers_.count(uid)) return;
  Peer& p = peers_[uid];
  p.name = name, p.uid = uid, p.address = at;
  p.state = State::Connecting;
  if (connector_ && (uid_ < uid || connector_->BothEndsConnect())) {
    p.conn = connector_->Connect(at);
    outgoingPeers_[p.conn] = uid;
  }
}

void LobbyCore::DisconnectFromPeer(int uid) { Remove(uid, true); }

// EjectPeer (0x7c7990, host only): Rejected(reason) to them, DeletePeer to everyone (KickPeer 0x7c7ac0).
bool LobbyCore::EjectPeer(int uid, const std::string& reason) {
  if (!host_) return false;
  auto it = peers_.find(uid);
  if (it == peers_.end()) return false;
  Send(it->second, MessageWriter().Str(reason).Done(kRejected));
  if (connector_ && it->second.conn) connector_->CloseAfterSend(it->second.conn), it->second.conn = 0;  // (Rejected first)
  const Message del = MessageWriter().I32(uid).Done(kDeletePeer);
  for (auto& [u, p] : peers_)
    if (u != uid) Send(p, del);
  Remove(uid, true);
  return true;
}

void LobbyCore::Broadcast(const std::string& bytes) {
  const Message m = MessageWriter().Bytes(bytes).Done(kScriptBroadcast);
  for (auto& [u, p] : peers_)
    if (p.state == State::Established) Send(p, m);
}

bool LobbyCore::SendTo(int uid, const std::string& bytes) {
  auto it = peers_.find(uid);
  if (it == peers_.end()) return false;
  Send(it->second, MessageWriter().Bytes(bytes).Done(kScriptOne));
  return true;
}

// MakeValidPlayerName (0x7c1720): unlike every other player's name, compared without case; a number on a clash.
std::string LobbyCore::MakeValidPlayerName(int uid, const std::string& name) const {
  auto taken = [&](const std::string& n) {
    if (uid != uid_ && Lower(n) == Lower(name_)) return true;
    for (const auto& [u, p] : peers_)
      if (u != uid && Lower(p.name) == Lower(n)) return true;
    return false;
  };
  if (!taken(name)) return name;
  for (int i = 1;; ++i) {
    const std::string n = name + std::to_string(i);
    if (!taken(n)) return n;
  }
}

std::vector<PeerInfo> LobbyCore::Peers() const {
  std::vector<PeerInfo> out;
  for (const auto& [u, p] : peers_) {
    PeerInfo i;
    i.name = p.name, i.uid = u, i.status = StateName(p.state), i.established = p.established;
    if (connector_ && p.conn) i.ping = connector_->PingMs(p.conn), i.quiet = connector_->QuietMs(p.conn);
    out.push_back(i);
  }
  return out;
}

LobbyCore::Peer* LobbyCore::PeerByConn(int conn) {
  for (auto& [u, p] : peers_)
    if (p.conn == conn) return &p;
  return nullptr;
}

void LobbyCore::Send(Peer& p, const Message& m) {
  if (connector_ && p.conn) connector_->Send(p.conn, m);
}

void LobbyCore::Pump() {
  if (!connector_) return;
  std::vector<ConnEvent> events;
  connector_->Poll(&events);
  for (ConnEvent& e : events) {
    switch (e.kind) {
      case ConnEvent::Made: OnMade(e.conn, e.incoming); break;
      case ConnEvent::Failed: OnFailed(e.conn); break;
      case ConnEvent::Lost: OnLost(e.conn); break;
      case ConnEvent::Received: OnMessage(e.conn, e.msg); break;
      case ConnEvent::Incoming: OnIncoming(e.conn); break;
    }
  }
  TellEstablished();
}

// Someone connecting over UDP (the original's lobby takes them with FindNextAddress 0x48b410 and Accept): at the
// host, anyone (a joiner; its Join follows); at a joiner, a peer we know at that address. INFERRED: an unknown one is
// left waiting (our ConnectToPeer takes it once the host's NewPeer names it), not rejected.
void LobbyCore::OnIncoming(int conn) {
  if (host_) {
    connector_->Accept(conn);
    return;
  }
  const Address from = connector_->PeerAddress(conn);
  for (auto& [u, p] : peers_) {
    if (p.state == State::Connecting && p.conn == 0 && p.address == from) {
      p.conn = conn;
      connector_->Accept(conn);
      return;
    }
  }
}

void LobbyCore::OnMade(int conn, bool incoming) {
  if (joining_ && conn == hostConn_) {  // (to the host: our Join)
    connector_->Send(conn, MessageWriter().Str(name_).I32(uid_).Done(kJoin));
    return;
  }
  if (!incoming) {  // (to a peer we connected to)
    auto it = outgoingPeers_.find(conn);
    if (it != outgoingPeers_.end()) {
      auto p = peers_.find(it->second);
      if (p != peers_.end()) p->second.state = State::Established;
      outgoingPeers_.erase(it);
    }
    return;
  }
  // Incoming: a peer we expect (its hello's uid, a NewPeer we had; UDP: the one we accepted it for), or, at the
  // host, a joiner (waits for its Join).
  const int uid = connector_->PeerUid(conn);
  auto p = uid >= 0 ? peers_.find(uid) : peers_.end();
  Peer* known = p != peers_.end() ? &p->second : PeerByConn(conn);
  if (!host_ && known && known->state == State::Connecting) {
    known->conn = conn;
    known->state = State::Established;
    return;
  }
  if (host_) {
    pending_[conn] = 0;
    return;
  }
  connector_->Close(conn);  // (nobody we know of)
}

void LobbyCore::OnFailed(int conn) {
  if (joining_ && conn == hostConn_) {  // ("connection to master failed -- giving up.")
    joining_ = false, hostConn_ = 0;
    if (hooks.connectionFailed) hooks.connectionFailed("HostLeft");
    return;
  }
  auto it = outgoingPeers_.find(conn);
  if (it != outgoingPeers_.end()) {  // (INFERRED: the original retries a peer; here it stays Connecting until removed)
    auto p = peers_.find(it->second);
    if (p != peers_.end()) p->second.conn = 0;
    outgoingPeers_.erase(it);
  }
  pending_.erase(conn);
}

// A connection lost (Reconnect 0x7c5ed0): the host lost -> ConnectionFailed("HostLeft"); at the host, the lost peer
// is ejected ("Disconnected") and everyone told; between two joiners, the host's DeletePeer decides.
void LobbyCore::OnLost(int conn) {
  pending_.erase(conn);
  if (!host_ && conn == hostConn_) {
    hostConn_ = 0;
    if (Peer* h = PeerByConn(conn)) h->conn = 0, h->state = State::Disconnected;
    if (hooks.connectionFailed) hooks.connectionFailed("HostLeft");
    return;
  }
  Peer* p = PeerByConn(conn);
  if (!p) return;
  p->conn = 0;
  if (host_) {
    const int uid = p->uid;
    if (hooks.systemMessage) hooks.systemMessage(p->name + " disconnected.");
    const Message del = MessageWriter().I32(uid).Done(kDeletePeer);
    for (auto& [u, q] : peers_)
      if (u != uid) Send(q, del);
    Remove(uid, true);
  } else {
    p->state = State::Disconnected;  // ("waiting for them to reconnect")
  }
}

void LobbyCore::OnMessage(int conn, const Message& m) {
  MessageReader r(m.payload);
  switch (m.type) {
    case kJoin: {  // OnJoin 0x7c64c0
      if (!host_ || !pending_.count(conn)) return;
      pending_.erase(conn);
      const std::string name = r.Str();
      int uid = r.I32();
      if (!r.Ok()) return;
      int count = 1;  // (us)
      for (const auto& [u, p] : peers_)
        if (p.state != State::Disconnected) ++count;
      if (count >= maxConnections_) {
        connector_->Send(conn, MessageWriter().Str("LobbyFull").Done(kRejected));
        connector_->CloseAfterSend(conn);
        return;
      }
      if (uid < 0 || uid == uid_ || peers_.count(uid)) uid = nextUid_++;  // ("LOBBY: assigning uid %d")
      else nextUid_ = std::max(nextUid_, uid + 1);
      const std::string valid = MakeValidPlayerName(uid, name);
      Peer& p = peers_[uid];
      p.name = valid, p.uid = uid, p.conn = conn, p.state = State::Established;
      p.address = connector_->PeerAddress(conn);
      connector_->Send(conn, MessageWriter().Str(name_).I32(uid_).I32(uid).Str(valid).U32(nonce_).Done(kWelcome));
      for (auto& [u, q] : peers_) {  // (SendInfoTo 0x7c8070: the newcomer of each other, each other of the newcomer)
        if (u == uid || q.state != State::Established) continue;
        SendInfo(p, q);
        SendInfo(q, p);
      }
      if (hooks.systemMessage) hooks.systemMessage(valid + " has joined.");  // INFERRED wording
      return;
    }
    case kRejected: {  // OnRejected 0x7c6ad0: only from the host
      if (host_ || conn != hostConn_) return;
      const std::string reason = r.Str();
      joining_ = false;
      if (hooks.ejected) hooks.ejected(reason);
      return;
    }
    case kWelcome: {  // OnWelcome 0x7c6bd0
      if (host_ || conn != hostConn_) return;
      const std::string hostName = r.Str();
      const int hostUid = r.I32();
      const int myUid = r.I32();
      const std::string myName = r.Str();
      const uint32_t nonce = r.U32();
      if (!r.Ok()) return;
      joining_ = false;
      uid_ = myUid, name_ = myName, nonce_ = nonce, hostUid_ = hostUid;
      connector_->SetLocalUid(uid_);
      Peer& h = peers_[hostUid];
      h.name = hostName, h.uid = hostUid, h.conn = conn, h.state = State::Established;
      h.address = connector_->PeerAddress(conn);
      if (hooks.connectionToHostEstablished) hooks.connectionToHostEstablished(uid_, name_, hostUid);
      if (hooks.systemMessage) hooks.systemMessage("Connection to " + hostName + " established.");
      return;
    }
    case kNewPeer: {  // OnNewPeer 0x7c7010: only from the host
      if (host_ || conn != hostConn_) return;
      const std::string name = r.Str();
      Address a;
      a.ip = r.U32();
      a.port = r.U16();
      const int uid = r.I32();
      if (!r.Ok()) return;
      ConnectToPeer(a, name, uid);
      return;
    }
    case kDeletePeer: {  // OnDeletePeer 0x7c76a0
      if (host_ || conn != hostConn_) return;
      const int uid = r.I32();
      if (!r.Ok()) return;
      if (uid == uid_) {  // ("we've been ejected")
        if (hooks.ejected) hooks.ejected("KickedByHost");
        return;
      }
      Remove(uid, true);
      return;
    }
    case kEstablishedPeers: {  // OnEstablishedPeers 0x7c7c10
      Peer* p = PeerByConn(conn);
      if (!p) return;
      std::vector<int> list;
      for (;;) {
        const int u = r.I32();
        if (!r.Ok() || u == -1) break;
        list.push_back(u);
      }
      p->established = list;
      if (hooks.establishedPeers) hooks.establishedPeers(p->uid, list);
      return;
    }
    case kScriptBroadcast:
    case kScriptOne: {  // OnScriptData 0x7c6ee0
      Peer* p = PeerByConn(conn);
      if (!p) return;
      if (hooks.dataReceived) hooks.dataReceived(m.payload, p->uid, p->name);
      return;
    }
    default:
      if (m.type < 100) early_.push_back(ConnEvent{ConnEvent::Received, conn, false, m});  // (GameLink)
      return;
  }
}

GameLink LobbyCore::TakeForGame() {
  Pump();  // (what came in up to now: the lobby's messages handled, the game's kept)
  GameLink link;
  link.localUid = uid_;
  link.nonce = nonce_;
  for (const auto& [u, p] : peers_)
    if (p.conn) link.connByUid[u] = p.conn;
  link.early = std::move(early_);
  early_.clear();
  link.connector = std::move(connector_);
  return link;
}

void LobbyCore::Remove(int uid, bool tellScript) {
  auto it = peers_.find(uid);
  if (it == peers_.end()) return;
  const std::string name = it->second.name;
  if (connector_ && it->second.conn) connector_->Close(it->second.conn);
  outgoingPeers_.erase(it->second.conn);
  peers_.erase(it);
  if (tellScript && hooks.peerDisconnected) hooks.peerDisconnected(name, uid);
}

// EstablishedPeers to each established peer whenever ours change (the original's PushTask 0x7c56b0 sends them as it
// pushes; INFERRED: on change, not every frame).
void LobbyCore::TellEstablished() {
  std::set<int> now;
  for (const auto& [u, p] : peers_)
    if (p.state == State::Established) now.insert(u);
  if (now == lastEstablished_) return;
  lastEstablished_ = now;
  MessageWriter w;
  for (int u : now) w.I32(u);
  w.I32(-1);
  const Message m = w.Done(kEstablishedPeers);
  for (auto& [u, p] : peers_)
    if (p.state == State::Established) Send(p, m);
}

Message LobbyCore::NewPeerMessage(const Peer& p) const {
  return MessageWriter().Str(p.name).U32(p.address.ip).U16(p.address.port).I32(p.uid).Done(kNewPeer);
}

void LobbyCore::SendInfo(Peer& to, const Peer& about) { Send(to, NewPeerMessage(about)); }

}  // namespace moho::net
