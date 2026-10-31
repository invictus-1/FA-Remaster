// The TCP connector (connector.h).
#include <chrono>
#include <deque>
#include <map>

#include "net/connector.h"
#include "net/test_delay.h"

namespace moho::net {
namespace {

using Clock = std::chrono::steady_clock;
double MsSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

constexpr uint8_t kHello = 210, kPing = 211, kPong = 212;
constexpr double kPingEveryMs = 1000.0;

class TcpConnector final : public Connector {
 public:
  bool Open(uint16_t port) {
    if (!listener_.Listen(port)) return false;
    port_ = listener_.LocalPort();
    return true;
  }
  const char* Protocol() const override { return "TCP"; }
  uint16_t LocalPort() const override { return port_; }
  void SetLocalUid(int32_t uid) override { uid_ = uid; }

  int Connect(const Address& to) override {
    const int id = next_++;
    Conn& c = conns_[id];
    c.sock = std::make_unique<TcpSocket>();
    c.connecting = true;
    c.peer = to;
    if (!c.sock->BeginConnect(to)) c.failed = true;  // (reported at the next Poll)
    return id;
  }

  void Send(int conn, const Message& m) override {
    auto it = conns_.find(conn);
    if (it != conns_.end()) AppendFramed(m, &it->second.out);
  }
  void Close(int conn) override { conns_.erase(conn); }
  void CloseAfterSend(int conn) override {
    auto it = conns_.find(conn);
    if (it != conns_.end()) it->second.closing = true;
  }

  void Poll(std::vector<ConnEvent>* out) override {
    Address from;
    while (auto s = listener_.Accept(&from)) {  // (incoming: made once its hello is in)
      const int id = next_++;
      Conn& c = conns_[id];
      c.sock = std::move(s);
      c.incoming = true;
      c.peer = from;
      SayHello(c);
    }
    for (auto it = conns_.begin(); it != conns_.end();) {
      Conn& c = it->second;
      const int id = it->first;
      bool gone = false;
      if (c.failed) {
        out->push_back(ConnEvent{ConnEvent::Failed, id});
        it = conns_.erase(it);
        continue;
      }
      if (c.connecting) {
        const int r = c.sock->CheckConnect();
        if (r < 0) {
          out->push_back(ConnEvent{ConnEvent::Failed, id});
          it = conns_.erase(it);
          continue;
        }
        if (r == 0) {
          ++it;
          continue;
        }
        c.connecting = false;
        SayHello(c);
      }
      // out first, then in
      while (!c.out.empty()) {
        const long n = c.sock->Send(c.out.data(), c.out.size());
        if (n < 0) {
          gone = true;
          break;
        }
        if (n == 0) break;
        c.out.erase(0, static_cast<size_t>(n));
      }
      char buf[8192];
      while (!gone && !c.endHeld) {
        const long n = c.sock->Receive(buf, sizeof buf);
        if (n < 0 && delay_.on()) {  // (the test's delay: the end too comes after what was read before it)
          c.held.push_back(Held{delay_.ReleaseAt(NowUs()), std::string(), true});
          c.endHeld = true;
          break;
        }
        if (n < 0) gone = true;
        if (n <= 0) break;
        if (delay_.on()) {
          c.held.push_back(Held{delay_.ReleaseAt(NowUs()), std::string(buf, static_cast<size_t>(n)), false});
          continue;
        }
        c.heard = Clock::now();
        c.split.Feed(buf, static_cast<size_t>(n));
      }
      while (!gone && !c.held.empty() && c.held.front().at <= NowUs()) {  // (MOHO64_NET_DELAY_MS: what is due now)
        Held h = std::move(c.held.front());
        c.held.pop_front();
        if (h.end) {
          gone = true;
          break;
        }
        c.heard = Clock::now();
        c.split.Feed(h.bytes.data(), h.bytes.size());
      }
      Message m;
      while (c.split.Next(&m)) {
        if (m.type == kHello) {
          MessageReader r(m.payload);
          const uint16_t port = r.U16();
          const int32_t uid = r.I32();
          if (r.Ok()) {
            c.peer.port = port;  // (its listening port; the IP is the connection's)
            c.uid = uid;
          }
          if (!c.made) {
            c.made = true;
            out->push_back(ConnEvent{ConnEvent::Made, id, c.incoming});
          }
        } else if (m.type == kPing) {
          AppendFramed(Message{kPong, m.payload}, &c.out);
        } else if (m.type == kPong) {
          MessageReader r(m.payload);
          const uint32_t sent = r.U32();
          if (r.Ok()) c.ping = static_cast<double>(NowMs() - sent);
        } else if (c.made) {
          out->push_back(ConnEvent{ConnEvent::Received, id, c.incoming, std::move(m)});
        }
      }
      if (c.split.Broken()) gone = true;
      if (!gone && c.closing && c.out.empty()) {  // (CloseAfterSend: all of it out)
        it = conns_.erase(it);
        continue;
      }
      if (!gone && c.made && MsSince(c.pinged) >= kPingEveryMs) {
        c.pinged = Clock::now();
        AppendFramed(MessageWriter().U32(NowMs()).Done(kPing), &c.out);
      }
      if (gone) {
        out->push_back(ConnEvent{c.made ? ConnEvent::Lost : ConnEvent::Failed, id, c.incoming});
        it = conns_.erase(it);
        continue;
      }
      ++it;
    }
  }

  Address PeerAddress(int conn) const override {
    auto it = conns_.find(conn);
    return it == conns_.end() ? Address{} : it->second.peer;
  }
  int32_t PeerUid(int conn) const override {
    auto it = conns_.find(conn);
    return it == conns_.end() ? -1 : it->second.uid;
  }
  double PingMs(int conn) const override {
    auto it = conns_.find(conn);
    return it == conns_.end() ? 0.0 : it->second.ping;
  }
  double QuietMs(int conn) const override {
    auto it = conns_.find(conn);
    return it == conns_.end() ? 0.0 : MsSince(it->second.heard);
  }

 private:
  struct Held {  // (MOHO64_NET_DELAY_MS: bytes read, or the end, and when they are taken in)
    int64_t at = 0;
    std::string bytes;
    bool end = false;
  };
  struct Conn {
    std::unique_ptr<TcpSocket> sock;
    bool connecting = false, incoming = false, made = false, failed = false, closing = false;
    Address peer;
    int32_t uid = -1;
    std::string out;
    MessageSplitter split;
    Clock::time_point heard = Clock::now(), pinged = Clock::now();
    double ping = 0;
    std::deque<Held> held;  // (MOHO64_NET_DELAY_MS: read, not yet taken in)
    bool endHeld = false;   // (its end read and held)
  };
  void SayHello(Conn& c) { AppendFramed(MessageWriter().U16(port_).I32(uid_).Done(kHello), &c.out); }
  static uint32_t NowMs() {
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
  }

  static int64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
  }

  TestDelay delay_{"TCP"};
  TcpSocket listener_;
  uint16_t port_ = 0;
  int32_t uid_ = -1;
  int next_ = 1;
  std::map<int, Conn> conns_;
};

}  // namespace

std::unique_ptr<Connector> MakeConnector(const std::string& protocol, uint16_t port) {
  if (protocol == "UDP") return MakeUdpConnector(port);
  if (protocol != "TCP") return nullptr;
  auto c = std::make_unique<TcpConnector>();
  if (!c->Open(port)) return nullptr;
  return c;
}

}  // namespace moho::net
