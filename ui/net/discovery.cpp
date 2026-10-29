#include "net/discovery.h"

#include <chrono>

#include "net/message.h"

namespace moho::net {
namespace {

constexpr uint8_t kRequest = 110, kReply = 111;
constexpr uint8_t kDiscoveryVersion = 11, kLobbyVersion = 1;
constexpr double kAskEvery = 2.0, kGoneAfter = 5.0;

double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
uint64_t Key(const Address& a) { return (static_cast<uint64_t>(a.ip) << 16) | a.port; }

bool SendMessage(UdpSocket& s, const Address& to, const Message& m) {
  std::string bytes;
  return AppendFramed(m, &bytes) && s.SendTo(to, bytes.data(), bytes.size());
}

}  // namespace

uint8_t ProtocolNumber(const std::string& name) { return name == "TCP" ? 1 : name == "UDP" ? 2 : 0; }
std::string ProtocolName(uint8_t number) { return number == 1 ? "TCP" : number == 2 ? "UDP" : "None"; }

bool DiscoveryListener::Open() { return sock_.Bind(kDiscoveryPort, false); }

void DiscoveryListener::Poll(std::vector<Address>* requests) {
  char buf[512];  // (the original's datagrams: 512 bytes at most)
  Address from;
  for (;;) {
    const long n = sock_.ReceiveFrom(buf, sizeof buf, &from);
    if (n <= 0) break;
    MessageSplitter split;
    split.Feed(buf, static_cast<size_t>(n));
    Message m;
    if (split.Next(&m) && m.type == kRequest) requests->push_back(from);
  }
}

void DiscoveryListener::Answer(const Address& to, uint8_t protocol, uint16_t gamePort, const std::string& table) {
  SendMessage(sock_, to,
              MessageWriter().U8(kDiscoveryVersion).U8(kLobbyVersion).U8(0).U8(protocol).U16(gamePort).Bytes(table).Done(kReply));
}

bool DiscoveryBrowser::Open() { return sock_.Bind(0, true); }

void DiscoveryBrowser::Poll(std::vector<DiscoveredGame>* found, std::vector<DiscoveredGame>* updated, std::vector<int>* removed) {
  const double now = Now();
  if (now - lastAsked_ >= kAskEvery) {  // (BroadcastDiscoveryRequestTick 0x7bfa60)
    lastAsked_ = now;
    const Message ask = MessageWriter().Done(kRequest);
    // (the original broadcasts only; ours asks this PC itself when there is no network to broadcast on. Both at once
    // would list a game on this PC twice: its answers come from the network's address and from 127.0.0.1)
    if (!SendMessage(sock_, Address{kBroadcastIp, kDiscoveryPort}, ask)) SendMessage(sock_, Address{0x7F000001u, kDiscoveryPort}, ask);
  }
  char buf[0x10000];
  Address from;
  for (;;) {
    const long n = sock_.ReceiveFrom(buf, sizeof buf, &from);
    if (n <= 0) break;
    MessageSplitter split;
    split.Feed(buf, static_cast<size_t>(n));
    Message m;
    if (!split.Next(&m) || m.type != kReply) continue;
    MessageReader r(m.payload);  // (OnDatagram 0x7bfb70)
    const uint8_t version = r.U8(), lobbyVersion = r.U8(), kind = r.U8(), protocol = r.U8();
    const uint16_t port = r.U16();
    if (!r.Ok() || version != kDiscoveryVersion || lobbyVersion != kLobbyVersion || kind != 0) continue;
    const Address host{from.ip, port};
    auto it = games_.find(Key(host));
    const bool fresh = it == games_.end();
    Known& k = games_[Key(host)];
    if (fresh) k.game.index = next_++;
    k.game.address = host, k.game.protocol = protocol, k.game.table = r.Rest();
    k.heard = now;
    (fresh ? found : updated)->push_back(k.game);
  }
  for (auto it = games_.begin(); it != games_.end();) {  // (ExpireStaleDiscoveryReplies 0x7bf8d0)
    if (now - it->second.heard > kGoneAfter) {
      removed->push_back(it->second.game.index);
      it = games_.erase(it);
    } else {
      ++it;
    }
  }
}

void DiscoveryBrowser::Reset(std::vector<int>* removed) {
  for (const auto& [k, g] : games_) removed->push_back(g.game.index);
  games_.clear();
  lastAsked_ = -1e9;
}

}  // namespace moho::net
