// The game's orders between the players (beat_exchange_net.h).
#include "net/beat_exchange_net.h"

#include <algorithm>
#include <cstring>

namespace moho::net {
namespace {

constexpr uint8_t kAdvance = 0, kVerifyChecksum = 3;  // (the replay stream's types, command_stream.md 3)

Message AdvanceMessage(uint32_t n) { return MessageWriter().U32(n).Done(kAdvance); }

}  // namespace

NetBeatExchange::NetBeatExchange(std::unique_ptr<Connector> connector, uint8_t localClient, std::vector<GameClient> peers)
    : connector_(std::move(connector)), local_(localClient) {
  for (const GameClient& g : peers) {
    Peer p;
    p.who = g;
    p.lost = g.conn <= 0;  // (a client of the session with no connection: the original's SPeer made Disconnected, 0x7c4f09)
    peers_.push_back(std::move(p));
  }
  std::sort(peers_.begin(), peers_.end(), [](const Peer& a, const Peer& b) { return a.who.client < b.who.client; });
}

NetBeatExchange::Peer* NetBeatExchange::ByConn(int conn) {
  for (Peer& p : peers_)
    if (p.who.conn == conn) return &p;
  return nullptr;
}

// Each beat's messages, then its Advance, to every peer (empty beats skipped by SendLocal are Advanced over first).
void NetBeatExchange::SendLocal(uint32_t beat, const std::string& bytes) {
  std::lock_guard<std::mutex> g(mu_);
  if (beat <= sentBeats_) return;  // (each beat once, in order)
  std::vector<Message> out;
  if (beat - 1 > sentBeats_) out.push_back(AdvanceMessage(beat - 1 - sentBeats_));
  MessageSplitter split;
  split.Feed(bytes.data(), bytes.size());
  Message m;
  while (split.Next(&m)) out.push_back(m);
  out.push_back(AdvanceMessage(1));
  for (const Peer& p : peers_)
    if (!p.lost && p.ejectedFrom == UINT32_MAX)
      for (const Message& msg : out) connector_->Send(p.who.conn, msg);
  localBeats_[beat] = bytes;
  sentBeats_ = beat;
  Pump();  // (Poll also sends what was written: the UDP connector's Push)
}

void NetBeatExchange::SendChecksum(const BeatChecksum& mine) {
  std::lock_guard<std::mutex> g(mu_);
  const Message m = MessageWriter().Bytes(std::string(reinterpret_cast<const char*>(mine.digest), 16)).I32(static_cast<int32_t>(mine.beat)).Done(kVerifyChecksum);
  for (const Peer& p : peers_)
    if (!p.lost && p.ejectedFrom == UINT32_MAX) connector_->Send(p.who.conn, m);
}

void NetBeatExchange::Pump() {
  std::vector<ConnEvent> events;
  connector_->Poll(&events);
  for (ConnEvent& e : events) Handle(e);
}

void NetBeatExchange::TakeEarly(std::vector<ConnEvent> early) {
  std::lock_guard<std::mutex> g(mu_);
  for (ConnEvent& e : early) Handle(e);
}

void NetBeatExchange::Handle(ConnEvent& e) {
  Peer* p = ByConn(e.conn);
  if (!p) return;
  if (e.kind == ConnEvent::Lost || e.kind == ConnEvent::Failed) {
    p->lost = true;
    return;
  }
  if (e.kind != ConnEvent::Received) return;
  const Message& m = e.msg;
  if (m.type == kAdvance) {  // (UpdateState 0x53c7ec: availableBeat += n, not forwarded)
    MessageReader r(m.payload);
    const uint32_t n = r.U32();
    if (!r.Ok()) return;
    for (uint32_t i = 0; i < n; ++i) {
      p->beats[p->available + 1 + i] = i == 0 ? std::move(p->pending) : std::string();
      p->pending.clear();
    }
    p->available += n;
  } else if (m.type == kVerifyChecksum) {
    MessageReader r(m.payload);
    BeatChecksum c;
    const std::string digest = r.Bytes(16);
    c.beat = static_cast<uint32_t>(r.I32());
    if (!r.Ok() || digest.size() != 16) return;
    std::memcpy(c.digest, digest.data(), 16);
    checksums_.emplace_back(p->who.client, c);
  } else {
    AppendFramed(m, &p->pending);
  }
}

bool NetBeatExchange::Collect(uint32_t beat, std::vector<ClientBatch>* out) {
  std::lock_guard<std::mutex> g(mu_);
  Pump();
  for (const Peer& p : peers_)
    if (beat < p.ejectedFrom && p.available < beat) return false;
  if (sentBeats_ < beat) return false;
  out->clear();
  auto local = localBeats_.find(beat);
  bool localDone = false;
  for (Peer& p : peers_) {
    if (!localDone && local_ < p.who.client) {
      out->push_back(ClientBatch{local_, local != localBeats_.end() ? local->second : std::string()});
      localDone = true;
    }
    if (beat >= p.ejectedFrom) continue;
    auto it = p.beats.find(beat);
    out->push_back(ClientBatch{p.who.client, it != p.beats.end() ? std::move(it->second) : std::string()});
    if (it != p.beats.end()) p.beats.erase(it);
  }
  if (!localDone) out->push_back(ClientBatch{local_, local != localBeats_.end() ? local->second : std::string()});
  if (local != localBeats_.end()) localBeats_.erase(local);
  return true;
}

void NetBeatExchange::TakePeerChecksums(std::vector<std::pair<uint8_t, BeatChecksum>>* out) {
  std::lock_guard<std::mutex> g(mu_);
  Pump();
  for (auto& c : checksums_) out->push_back(c);
  checksums_.clear();
}

void NetBeatExchange::Eject(uint8_t client, uint32_t beat) {
  std::lock_guard<std::mutex> g(mu_);
  for (Peer& p : peers_)
    if (p.who.client == client) p.ejectedFrom = std::min(p.ejectedFrom, beat);
}

std::vector<uint8_t> NetBeatExchange::LostClients() {
  std::lock_guard<std::mutex> g(mu_);
  Pump();
  std::vector<uint8_t> out;
  for (const Peer& p : peers_)
    if (p.lost && p.ejectedFrom == UINT32_MAX) out.push_back(p.who.client);
  return out;
}

uint32_t NetBeatExchange::ReceivedBeats(uint8_t client) {
  std::lock_guard<std::mutex> g(mu_);
  Pump();
  for (const Peer& p : peers_)
    if (p.who.client == client) return p.available;
  return client == local_ ? sentBeats_ : 0;
}

ClientLink NetBeatExchange::Link(uint8_t client) {
  std::lock_guard<std::mutex> g(mu_);
  ClientLink l;
  if (client == local_) {
    l.connected = true;
    return l;
  }
  for (const Peer& p : peers_) {
    if (p.who.client != client) continue;
    l.connected = !p.lost && p.ejectedFrom == UINT32_MAX;
    if (p.who.conn > 0) l.ping = connector_->PingMs(p.who.conn), l.quiet = std::max(0.0, connector_->QuietMs(p.who.conn));
  }
  return l;
}

}  // namespace moho::net
