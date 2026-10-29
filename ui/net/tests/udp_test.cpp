// The UDP connector over loopback, without the interface: a connection made and accepted, messages both ways in
// order and whole, the same with a fifth of the datagrams lost (MOHO64_NET_DROP, resends and the early-packet list),
// two ends connecting to each other at once (one link), and a close the other end hears as Lost. Build and run, from
// the repository (zlib's six files first, as C: `py -m ziglang cc -O1 -w -c third_party/zlib/<n>.c -o <obj>/<n>.o`
// for adler32 crc32 inffast inflate inftrees zutil):
//   py -m ziglang c++ -std=c++20 -O1 -I ui -I third_party/zlib ui/net/tests/udp_test.cpp ui/net/connector_udp.cpp
//      ui/net/connector_tcp.cpp ui/net/message.cpp ui/net/socket.cpp <obj>/*.o -lws2_32 -o <scratch>/udp_test.exe
// (add -target x86_64-windows-gnu on Windows, as build_ui.py does)
// Prints "ok ..." / "FAIL ..." lines and ends with "udp_test: <n> failed".
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "net/connector.h"

using namespace moho::net;

namespace {

int failed = 0;
void Check(const char* what, bool ok) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++failed;
}

struct End {
  std::unique_ptr<Connector> c;
  std::vector<ConnEvent> events;
  void Poll() { c->Poll(&events); }
  Address Here() const { return Address{0x7f000001u, c->LocalPort()}; }
};

// Polls both until `done` or `seconds` passed.
template <class F>
bool Until(End& a, End& b, double seconds, F done) {
  const auto stop = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
  while (std::chrono::steady_clock::now() < stop) {
    a.Poll(), b.Poll();
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return done();
}

int Count(const End& e, ConnEvent::Kind k) {
  int n = 0;
  for (const auto& ev : e.events) n += ev.kind == k;
  return n;
}

Message Numbered(int i) {  // (type 50 + i % 50, i in the first 4 bytes, then a size that varies)
  Message m;
  m.type = static_cast<uint8_t>(50 + i % 50);
  const size_t size = 4 + (static_cast<size_t>(i) * 37) % 300;
  m.payload.resize(size);
  for (size_t k = 0; k < size; ++k) m.payload[k] = static_cast<char>(k < 4 ? (i >> (8 * k)) : (i + k));
  return m;
}

// The received messages of `e` are 0..n-1 in order, each whole.
bool InOrder(const End& e, int n) {
  int next = 0;
  for (const auto& ev : e.events) {
    if (ev.kind != ConnEvent::Received) continue;
    const Message want = Numbered(next);
    if (ev.msg.type != want.type || ev.msg.payload != want.payload) return false;
    ++next;
  }
  return next == n;
}

void Exchange(const char* label, int n) {
  End a{MakeConnector("UDP", 0)}, b{MakeConnector("UDP", 0)};
  Check((std::string(label) + ": both ports open").c_str(), a.c && b.c);
  if (!a.c || !b.c) return;
  const int ca = a.c->Connect(b.Here());
  int cb = 0;
  Until(a, b, 5, [&] {
    for (const auto& e : b.events)
      if (e.kind == ConnEvent::Incoming && cb == 0) cb = e.conn, b.c->Accept(cb);
    return Count(a, ConnEvent::Made) == 1 && Count(b, ConnEvent::Made) == 1;
  });
  Check((std::string(label) + ": the other end hears it coming").c_str(), cb != 0);
  Check((std::string(label) + ": made at both ends").c_str(), Count(a, ConnEvent::Made) == 1 && Count(b, ConnEvent::Made) == 1);
  for (int i = 0; i < n; ++i) a.c->Send(ca, Numbered(i)), b.c->Send(cb, Numbered(i));
  const bool all = Until(a, b, 60, [&] { return Count(a, ConnEvent::Received) >= n && Count(b, ConnEvent::Received) >= n; });
  Check((std::string(label) + ": every message arrives, both ways").c_str(), all);
  Check((std::string(label) + ": in order and whole, at b").c_str(), InOrder(b, n));
  Check((std::string(label) + ": in order and whole, at a").c_str(), InOrder(a, n));
  Check((std::string(label) + ": a ping").c_str(), a.c->PingMs(ca) > 0 && a.c->PingMs(ca) < 1000);
  Check((std::string(label) + ": quiet is small").c_str(), b.c->QuietMs(cb) >= 0 && b.c->QuietMs(cb) < 5000);
  a.c->Close(ca);
  const bool lost = Until(a, b, 20, [&] { return Count(b, ConnEvent::Lost) == 1; });
  Check((std::string(label) + ": a's close is Lost at b").c_str(), lost);
  Check((std::string(label) + ": and nothing more at a").c_str(), Count(a, ConnEvent::Lost) == 0 && Count(a, ConnEvent::Failed) == 0);
}

void BothConnect() {
  End a{MakeConnector("UDP", 0)}, b{MakeConnector("UDP", 0)};
  if (!a.c || !b.c) return Check("both ends: ports open", false);
  Check("UDP: both ends connect", a.c->BothEndsConnect());
  const int ca = a.c->Connect(b.Here());
  const int cb = b.c->Connect(a.Here());
  Until(a, b, 5, [&] { return Count(a, ConnEvent::Made) >= 1 && Count(b, ConnEvent::Made) >= 1; });
  Check("both ends: one link made at each", Count(a, ConnEvent::Made) == 1 && Count(b, ConnEvent::Made) == 1);
  Check("both ends: no Incoming (each one's CONNECT met the other's)", Count(a, ConnEvent::Incoming) + Count(b, ConnEvent::Incoming) == 0);
  a.c->Send(ca, Numbered(0));
  b.c->Send(cb, Numbered(0));
  Check("both ends: a message each way",
        Until(a, b, 5, [&] { return Count(a, ConnEvent::Received) == 1 && Count(b, ConnEvent::Received) == 1; }));
  Check("both ends: nothing lost", Count(a, ConnEvent::Lost) + Count(b, ConnEvent::Lost) + Count(a, ConnEvent::Failed) +
                                       Count(b, ConnEvent::Failed) == 0);
}

void DestroyedIsLost() {  // (the lobby's Destroy: the connector goes, its thread sees the end of the stream out)
  End b{MakeConnector("UDP", 0)};
  End a{MakeConnector("UDP", 0)};
  const int ca = a.c->Connect(b.Here());
  Until(a, b, 5, [&] {
    for (const auto& e : b.events)
      if (e.kind == ConnEvent::Incoming) b.c->Accept(e.conn);
    return Count(a, ConnEvent::Made) == 1 && Count(b, ConnEvent::Made) == 1;
  });
  a.c->Send(ca, Numbered(1));
  a.c.reset();
  const auto stop = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (Count(b, ConnEvent::Lost) == 0 && std::chrono::steady_clock::now() < stop) {
    b.Poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  Check("destroyed: what was sent before still arrives", Count(b, ConnEvent::Received) == 1);
  Check("destroyed: the other end hears it as Lost", Count(b, ConnEvent::Lost) == 1);
}

}  // namespace

int main() {
  NetStartup();
  Exchange("clean", 60);
  BothConnect();
  DestroyedIsLost();
#ifdef _WIN32
  _putenv_s("MOHO64_NET_DROP", "200");
#else
  setenv("MOHO64_NET_DROP", "200", 1);
#endif
  Exchange("a fifth lost", 60);
  std::printf("udp_test: %d failed\n", failed);
  return failed ? 1 : 0;
}
