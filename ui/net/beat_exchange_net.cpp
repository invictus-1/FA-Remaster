// The game's orders between the players (beat_exchange_net.h).
#include "net/beat_exchange_net.h"

#include <algorithm>
#include <cstring>

#include "core/log.h"

namespace moho::net {
namespace {

constexpr uint8_t kAdvance = 0, kVerifyChecksum = 3;  // (the replay stream's types, command_stream.md 3)
constexpr uint8_t kLastStream = 49;                   // (0..49: the stream; CNetClient's ranges, 0x53bb60)
constexpr uint8_t kAck = 50, kDispatched = 51, kAvailable = 52, kReady = 53, kEject = 54, kChat = 55, kSpeed = 56,
                  kMaxSimRate = 57;

Message AdvanceMessage(uint32_t n) { return MessageWriter().U32(n).Done(kAdvance); }

}  // namespace

NetBeatExchange::NetBeatExchange(std::unique_ptr<Connector> connector, uint8_t localClient, std::vector<GameClient> peers,
                                 bool speedAdjustable, int rate)
    : connector_(std::move(connector)), local_(localClient), adjustable_(speedAdjustable), rate_(rate) {
  Client me;
  me.who = GameClient{localClient, 0};
  me.local = true;
  clients_.push_back(std::move(me));
  for (const GameClient& g : peers) {
    Client p;
    p.who = g;
    p.lost = g.conn <= 0;  // (a client of the session with no connection: the original's SPeer made Disconnected, 0x7c4f09)
    clients_.push_back(std::move(p));
  }
  std::sort(clients_.begin(), clients_.end(), [](const Client& a, const Client& b) { return a.who.client < b.who.client; });
}

NetBeatExchange::Client* NetBeatExchange::ByConn(int conn) {
  for (Client& c : clients_)
    if (!c.local && c.who.conn == conn) return &c;
  return nullptr;
}

NetBeatExchange::Client* NetBeatExchange::ByIndex(uint8_t client) {
  for (Client& c : clients_)
    if (c.who.client == client) return &c;
  return nullptr;
}

// A peer we still talk to: not lost, not ejected (Eject closes the link: CNetClient's slot 14, 0x53de20, drops its
// connection, and its Process 0x53dd60 then sends nothing).
bool NetBeatExchange::Reachable(const Client& c) const {
  return !c.local && !frozen_ && !c.lost && !c.ejected && c.who.conn > 0;
}

void NetBeatExchange::SendTo(const Client& c, const Message& m) {
  if (Reachable(c)) connector_->Send(c.who.conn, m);
}

// The original's broadcast (CClientManagerImpl vtable slot 17, 0x53e990): every client object's Process; a peer's
// sends the message to it, the local one's handles it here as from this client (CLocalClient::Process 0x53d190).
void NetBeatExchange::Broadcast(const Message& m) {
  if (frozen_) return;
  for (const Client& c : clients_) SendTo(c, m);
  Process(Local(), m);
}

// Each beat's messages, then its Advance, to every peer (empty beats skipped by SendLocal are Advanced over first);
// then our own ACK of it, as the original's local client takes in its own Advance (0x53bfe0).
void NetBeatExchange::SendLocal(uint32_t beat, const std::string& bytes) {
  std::lock_guard<std::mutex> g(mu_);
  if (beat <= sentBeats_ || frozen_) return;  // (each beat once, in order)
  std::vector<Message> out;
  if (beat - 1 > sentBeats_) out.push_back(AdvanceMessage(beat - 1 - sentBeats_));
  MessageSplitter split;
  split.Feed(bytes.data(), bytes.size());
  Message m;
  while (split.Next(&m)) out.push_back(m);
  out.push_back(AdvanceMessage(1));
  for (const Client& c : clients_)
    for (const Message& msg : out) SendTo(c, msg);
  localBeats_[beat] = bytes;
  sentBeats_ = beat;
  Local().available = beat;
  Broadcast(MessageWriter().U8(local_).I32(static_cast<int32_t>(beat)).Done(kAck));
  Pump();  // (Poll also sends what was written: the UDP connector's Push)
}

void NetBeatExchange::SendChecksum(const BeatChecksum& mine) {
  std::lock_guard<std::mutex> g(mu_);
  const Message m = MessageWriter().Bytes(std::string(reinterpret_cast<const char*>(mine.digest), 16)).I32(static_cast<int32_t>(mine.beat)).Done(kVerifyChecksum);
  for (const Client& c : clients_) SendTo(c, m);
}

void NetBeatExchange::Pump() {
  if (frozen_) return;
  std::vector<ConnEvent> events;
  connector_->Poll(&events);
  for (ConnEvent& e : events) Handle(e);
}

void NetBeatExchange::TakeEarly(std::vector<ConnEvent> early) {
  std::lock_guard<std::mutex> g(mu_);
  for (ConnEvent& e : early) Handle(e);
}

void NetBeatExchange::Handle(ConnEvent& e) {
  Client* c = ByConn(e.conn);
  // (a client ejected by our vote: its link is closed, nothing more comes from it; one left out by Eject(client, beat)
  // still sends its beats before that)
  if (!c || (c->ejected && c->forcedFrom == UINT32_MAX)) return;
  if (e.kind == ConnEvent::Lost || e.kind == ConnEvent::Failed) {
    // The original's 202 / 203 (the connection ended with an error / cleanly, 0x53c2c3): the client's Eject at once
    // (slot 10, CClientBase::Eject 0x53cb10: this program's vote, as EjectSessionClient's), then the interface's
    // NoteDisconnect (slot 0, 0x88b810: "<LOC Engine0002>%s disconnected."). Every other client loses its link too and
    // does the same, so the vote is whole without the dialog. (A client only gone quiet, its link still up, goes
    // through FAF's disconnect dialog instead.) Chris, 2026-10-30: "Just keep faf behaviour".
    if (c->lost) return;
    c->lost = true;
    EjectLocked(*c);
    disconnects_.push_back(c->who.client);
    return;
  }
  if (e.kind != ConnEvent::Received) return;
  c->heard = std::chrono::steady_clock::now();
  Process(*c, e.msg);
}

// One message of client `from` (CClientBase::Process 0x53bf30; `from` is the local client for our own broadcasts).
void NetBeatExchange::Process(Client& from, const Message& m) {
  MessageReader r(m.payload);
  if (m.type == kAdvance) {  // (UpdateState 0x53c7ec: availableBeat += n, not forwarded; Process 0x53bfda: then an ACK)
    if (from.local) return;  // (ours are counted in SendLocal)
    const uint32_t n = r.U32();
    if (!r.Ok()) return;
    for (uint32_t i = 0; i < n; ++i) {
      from.beats[from.available + 1 + i] = i == 0 ? std::move(from.pending) : std::string();
      from.pending.clear();
    }
    from.available += n;
    Broadcast(MessageWriter().U8(from.who.client).I32(static_cast<int32_t>(from.available)).Done(kAck));
    return;
  }
  if (m.type == kVerifyChecksum) {
    BeatChecksum c;
    const std::string digest = r.Bytes(16);
    c.beat = static_cast<uint32_t>(r.I32());
    if (!r.Ok() || digest.size() != 16) return;
    std::memcpy(c.digest, digest.data(), 16);
    checksums_.emplace_back(from.who.client, c);
    return;
  }
  if (m.type <= kLastStream) {
    if (!from.local) AppendFramed(m, &from.pending);
    return;
  }
  switch (m.type) {
    case kAck: {  // (0x53c0fa)
      const uint8_t about = r.U8();
      const int32_t n = r.I32();
      if (!r.Ok()) return;
      if (!ByIndex(about)) {
        Logf(LogLevel::Info, "CClientBase::Process(): ignoring ACK from client %d for invalid client %d.", from.who.client, about);
        return;
      }
      uint32_t& prev = from.acks[about];
      if (n - static_cast<int32_t>(prev) <= 0) {
        Logf(LogLevel::Info, "CClientBase::Process(): ignoring out of sequence ACK from client %d for %d (beat=%d, prevack=%d)",
             from.who.client, about, n, static_cast<int32_t>(prev));
        return;
      }
      prev = static_cast<uint32_t>(n);
      return;
    }
    case kDispatched:
    case kAvailable: {  // (0x53c170 / 0x53c1b0)
      const int32_t n = r.I32();
      if (!r.Ok()) return;
      uint32_t& prev = m.type == kDispatched ? from.dispatched : from.availableBeat;
      if (n - static_cast<int32_t>(prev) < 0) {
        Logf(LogLevel::Info, "CClientBase::Process(): ignoring out of sequence %s message from client %d (beat=%d, prev=%d)",
             m.type == kDispatched ? "DISPATCHED" : "AVAILABLE", from.who.client, n, static_cast<int32_t>(prev));
        return;
      }
      prev = static_cast<uint32_t>(n);
      return;
    }
    case kReady:  // (0x53c1f0)
      from.ready = true;
      return;
    case kEject: {  // (0x53c1f9 -> HandleIncomingEjectRequest 0x53f440)
      const uint8_t target = r.U8();
      const int32_t n = r.I32();
      if (!r.Ok()) return;
      Client* t = ByIndex(target);
      if (!t) {
        Logf(LogLevel::Info, "Ignoring eject request from %d for invalid client index %u", from.who.client, target);
        return;
      }
      // AddOrUpdateEjectRequest 0x53cbb0: one request per requester, the smaller beat kept
      auto it = std::find_if(t->requests.begin(), t->requests.end(), [&](const auto& q) { return q.first == from.who.client; });
      if (it == t->requests.end()) t->requests.emplace_back(from.who.client, static_cast<uint32_t>(n));
      else if (static_cast<int32_t>(n - it->second) < 0) it->second = static_cast<uint32_t>(n);
      Logf(LogLevel::Info, "net: client %d asks to eject client %d after its beat %d", from.who.client, target, n);
      return;  // (the interface hears nothing of it: CWldUiInterface's slot 1, 0x88b870, does nothing)
    }
    case kChat:  // (0x53c228: the message's bytes to the interface, CWldUiInterface::ReceiveChat 0x88b880)
      chats_.emplace_back(from.who.client, m.payload);
      return;
    case kSpeed: {  // (0x53c285: only where the game's speed is adjustable, client+0x28 -> +0x454)
      const int32_t serial = r.I32();
      const int32_t rate = r.I32();
      if (!r.Ok() || !adjustable_) return;
      // ApplyIncomingGameSpeedRequest 0x53e810: a newer request wins; of two with the same number the lower client's
      if (serial > static_cast<int32_t>(rateSerial_) ||
          (serial == static_cast<int32_t>(rateSerial_) && from.who.client < rateFrom_)) {
        rateSerial_ = static_cast<uint32_t>(serial);
        rateFrom_ = from.who.client;
        rate_ = rate;
        rateNotes_.emplace_back(from.who.client, rate);
      }
      return;
    }
    case kMaxSimRate:  // (0x53c2b2)
      from.maxSP = r.I32();
      return;
    default:
      Logf(LogLevel::Info, "CClientBase::Queue(): Ignoring message type %d", m.type);
  }
}

// GetMostExpiredEjectRequest 0x53cc20: its beats in here, or the smallest any client asked to eject it after.
uint32_t NetBeatExchange::LastBeatOf(const Client& c) const {
  if (c.forcedFrom != UINT32_MAX) return c.forcedFrom - 1;
  uint32_t last = c.available;
  for (const auto& q : c.requests) last = std::min(last, q.second);
  return last;
}

// IsReadyForBeat 0x53c3e0: client `c` has acknowledged every client's part of `beat` (a client left out is not
// waited for; an ejected client only up to its last beat). A client that is out or ejected is ready for anything.
// The original skips an ejected client from its last beat on (beat >= last, 0x53c44a); ours from the beat after it
// (beat > last), so that its last beat too is held until everyone has it. INFERRED: the original's way lets a client
// run that last beat with the ejected client's orders while another, who never got them, leaves it out (a desync
// that can only happen with three or more clients); ours waits for the slower one's vote instead.
bool NetBeatExchange::ReadyForBeat(const Client& c, uint32_t beat) const {
  if (c.done || c.ejected) return true;
  for (const Client& of : clients_) {
    if (of.done) continue;
    if (of.ejected && beat > LastBeatOf(of)) continue;
    auto it = c.acks.find(of.who.client);
    if (it == c.acks.end() || it->second < beat) return false;
  }
  return true;
}

// DoBeat 0x53eb5c: the available beat moves on while every client is ready for the next (EveryoneResponsiveSince
// 0x53ef30); when it moved, AVAILABLE goes to everyone.
void NetBeatExchange::UpdateAvailable() {
  const uint32_t before = availableBeat_;
  for (;;) {
    bool all = true;
    for (const Client& c : clients_) all = all && ReadyForBeat(c, availableBeat_ + 1);
    if (!all) break;
    ++availableBeat_;
  }
  if (availableBeat_ != before) Broadcast(MessageWriter().I32(static_cast<int32_t>(availableBeat_)).Done(kAvailable));
}

bool NetBeatExchange::Collect(uint32_t beat, std::vector<ClientBatch>* out) {
  std::lock_guard<std::mutex> g(mu_);
  Pump();
  UpdateAvailable();
  if (beat > availableBeat_ || beat > sentBeats_) return false;
  auto leftOut = [&](const Client& c) { return c.done || (c.ejected && beat > LastBeatOf(c)); };
  for (const Client& c : clients_)  // (implied by our own acknowledgements; kept as a guard)
    if (!c.local && !leftOut(c) && c.available < beat) return false;
  out->clear();
  for (Client& c : clients_) {
    if (c.local) {
      auto local = localBeats_.find(beat);
      out->push_back(ClientBatch{local_, local != localBeats_.end() ? local->second : std::string()});
      if (local != localBeats_.end()) localBeats_.erase(local);
      continue;
    }
    if (!c.done && leftOut(c)) {
      // UpdateState 0x53c5f6: an ejected client's beats end here; the lockstep terminates its command sources
      c.done = true;
      Logf(LogLevel::Info, "net: client %d is left out from beat %u on (ejected after its beat %u)", c.who.client, beat,
           LastBeatOf(c));
    }
    if (c.done) continue;
    auto it = c.beats.find(beat);
    out->push_back(ClientBatch{c.who.client, it != c.beats.end() ? std::move(it->second) : std::string()});
    if (it != c.beats.end()) c.beats.erase(it);
  }
  dispatched_ = beat;
  Broadcast(MessageWriter().I32(static_cast<int32_t>(beat)).Done(kDispatched));  // (UpdateStates, FAF 0x129015d)
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
  Client* c = ByIndex(client);
  if (!c || c->local) return;
  c->forcedFrom = std::min(c->forcedFrom, beat);
  c->ejected = c->ready = true;  // (nothing more is sent to it: the engine's test makes the eject when that beat comes)
}

void NetBeatExchange::RequestEject(uint8_t client) {
  std::lock_guard<std::mutex> g(mu_);
  if (Client* t = ByIndex(client)) EjectLocked(*t);
}

void NetBeatExchange::EjectLocked(Client& c) {
  Client* t = &c;
  const uint8_t client = c.who.client;
  if (t->local || t->ejected) return;  // (0x53cb56: once)
  t->ready = true;
  if (t->who.conn > 0 && !t->lost && !frozen_) connector_->CloseAfterSend(t->who.conn);  // (slot 14, 0x53de20)
  t->ejected = true;
  const uint32_t n = t->available;
  Logf(LogLevel::Info, "net: this program ejects client %d after its beat %u", client, n);
  // ProcessEject 0x53f2c0: the request to everyone (here too), then the requests the ejected client made are void
  Broadcast(MessageWriter().U8(client).I32(static_cast<int32_t>(n)).Done(kEject));
  for (Client& c : clients_)
    c.requests.erase(std::remove_if(c.requests.begin(), c.requests.end(), [&](const auto& q) { return q.first == client; }),
                     c.requests.end());
}

void NetBeatExchange::SendReady() {
  std::lock_guard<std::mutex> g(mu_);
  if (readySent_) return;
  readySent_ = true;
  Broadcast(Message{kReady, std::string()});
  Pump();
}

bool NetBeatExchange::EveryoneReady() {
  std::lock_guard<std::mutex> g(mu_);
  Pump();
  if (!everyoneReady_ && readySent_) {  // (DoBeat 0x53ea93: once every client's flag is up, for good)
    bool all = true;
    for (const Client& c : clients_) all = all && c.ready;
    everyoneReady_ = all;
  }
  return everyoneReady_;
}

void NetBeatExchange::SendChat(uint8_t client, const std::string& bytes) {
  std::lock_guard<std::mutex> g(mu_);
  Client* c = ByIndex(client);
  if (!c || frozen_) return;
  const Message m{kChat, bytes};
  if (c->local) Process(*c, m);
  else SendTo(*c, m);
}

std::vector<std::pair<uint8_t, std::string>> NetBeatExchange::TakeChats() {
  std::lock_guard<std::mutex> g(mu_);
  Pump();
  std::vector<std::pair<uint8_t, std::string>> out;
  out.swap(chats_);
  return out;
}

void NetBeatExchange::RequestRate(int rate) {
  std::lock_guard<std::mutex> g(mu_);
  if (!adjustable_) return;  // (0x53e5b4)
  Broadcast(MessageWriter().I32(static_cast<int32_t>(rateSerial_ + 1)).I32(rate).Done(kSpeed));
}

int NetBeatExchange::Rate() {
  std::lock_guard<std::mutex> g(mu_);
  return rate_;
}

std::vector<std::pair<uint8_t, int>> NetBeatExchange::TakeRateNotes() {
  std::lock_guard<std::mutex> g(mu_);
  std::vector<std::pair<uint8_t, int>> out;
  out.swap(rateNotes_);
  return out;
}

std::vector<uint8_t> NetBeatExchange::TakeDisconnects() {
  std::lock_guard<std::mutex> g(mu_);
  std::vector<uint8_t> out;
  out.swap(disconnects_);
  return out;
}

std::vector<uint8_t> NetBeatExchange::LostClients() {
  std::lock_guard<std::mutex> g(mu_);
  Pump();
  std::vector<uint8_t> out;
  for (const Client& c : clients_)
    if (!c.local && c.lost && !c.ejected) out.push_back(c.who.client);
  return out;
}

uint32_t NetBeatExchange::ReceivedBeats(uint8_t client) {
  std::lock_guard<std::mutex> g(mu_);
  Pump();
  const Client* c = ByIndex(client);
  return c ? c->available : 0;
}

ClientLink NetBeatExchange::Link(uint8_t client) {
  std::lock_guard<std::mutex> g(mu_);
  ClientLink l;
  const Client* c = ByIndex(client);
  if (!c) return l;
  for (const auto& q : c->requests) l.ejectedBy.push_back(q.first);
  l.maxSP = c->maxSP;
  if (c->local) {  // (CLocalClient's ping and quiet: 0, 0x53ba00 / 0x53ba10)
    l.connected = true;
    return l;
  }
  l.connected = !c->ejected;
  if (!l.connected) {  // (its connection closed by the eject: ping 0, quiet -1, CNetClient 0x53dc8f / 0x53dd1f)
    l.quiet = -1;
  } else if (c->lost || frozen_) {  // (a client with no connection from the start: quiet since the game began, so that
                                    // the disconnect dialog shows it; INFERRED, the original's case is not read)
    l.quiet = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c->heard).count();
  } else {
    l.ping = connector_->PingMs(c->who.conn);
    l.quiet = std::max(0.0, connector_->QuietMs(c->who.conn));
  }
  return l;
}

void NetBeatExchange::Service() {
  std::lock_guard<std::mutex> g(mu_);
  Pump();
}

void NetBeatExchange::FreezeForTests() {
  std::lock_guard<std::mutex> g(mu_);
  frozen_ = true;
}

}  // namespace moho::net
