// The UDP connector (connector.h): the original's CNetUDPConnector / CNetUDPConnection (NetUDPConn.cpp), its
// packets on the wire as the original's, so that a peer running the original's connector understands ours. Read in
// full in docs/ui-net-udp.md (every number below has its address there); the sections named here are that file's.
//
// As the original: one socket, one thread of the connector's own ("CNetUDPConnector for port %d", 0x489ffd) that
// receives, acks, resends and keeps alive; the owner's calls and the thread share the state under one lock. Ours:
// the thread waits on the socket (select) and is woken by a one-byte datagram to itself (a "short packet", ignored);
// the original waits on a WSA event. Each connection is a reliable, ordered byte stream of the owner's messages,
// raw-deflated (one stream per direction for the connection's life) when the sender says so in its CONNECT / ANSWER
// (ours: see kCompression).
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <thread>

#include <zlib.h>

#include "net/connector.h"
#include "net/test_delay.h"

namespace moho::net {
namespace {

// Packet types (section 2).
constexpr uint8_t kConnect = 0, kAnswer = 1, kData = 4, kAck = 5, kKeepAlive = 6, kGoodbye = 7, kNat = 8;
constexpr size_t kHeader = 15, kMaxDatagram = 512, kMaxPayload = kMaxDatagram - kHeader;  // 497
constexpr size_t kConnectSize = kHeader + 45, kAnswerSize = kHeader + 77;
constexpr uint32_t kProtocolVersion = 2;
// Times in microseconds (section 8), the console variables' defaults (section 8's table).
constexpr int64_t kHandshakeRepeat = 1000000, kTimeout = 10000000, kKeepAliveEvery = 2000000;
constexpr int64_t kAckDelay = 25000, kSendDelay = 25000;
constexpr double kMinResendMs = 100, kMaxResendMs = 1000, kResendPingMultiplier = 1.0, kResendBiasMs = 25;
constexpr double kMaxSendRate = 2048, kMaxBacklog = 2048;  // bytes per second, bytes
constexpr int kWindow = 32, kHistory = 128, kPingSamples = 24;
// net_CompressionMethod: the original's default is 1 (deflate our sends). Ours sends 0 (none, a setting the original
// has too) until the repository's zlib has its deflate half (third_party/zlib holds only inflate's files); what a
// peer sends deflated is inflated either way, so an original peer at its default still works with ours. Build with
// MOHO64_NET_DEFLATE=1 (and zlib's deflate.c / trees.c) for the original's 1.
#ifndef MOHO64_NET_DEFLATE
#define MOHO64_NET_DEFLATE 0
#endif
constexpr uint8_t kCompression = MOHO64_NET_DEFLATE ? 1 : 0;
// Ours: how long a destroyed connector's thread keeps going to see its goodbyes out (the original's lives until its
// last connection is done; INFERRED, not read).
constexpr int64_t kShutdownLinger = 3000000;

enum State { Pending, Connecting, Answering, Established, TimedOut, Errored };

uint16_t Get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t Get32(const uint8_t* p) { return Get16(p) | static_cast<uint32_t>(Get16(p + 2)) << 16; }
uint64_t Get64(const uint8_t* p) { return Get32(p) | static_cast<uint64_t>(Get32(p + 4)) << 32; }
void Put16(uint8_t* p, uint16_t v) { p[0] = static_cast<uint8_t>(v), p[1] = static_cast<uint8_t>(v >> 8); }
void Put32(uint8_t* p, uint32_t v) { Put16(p, static_cast<uint16_t>(v)), Put16(p + 2, static_cast<uint16_t>(v >> 16)); }
void Put64(uint8_t* p, uint64_t v) { Put32(p, static_cast<uint32_t>(v)), Put32(p + 4, static_cast<uint32_t>(v >> 32)); }
int16_t Diff(uint16_t a, uint16_t b) { return static_cast<int16_t>(static_cast<uint16_t>(a - b)); }  // (a - b, wrapping)

// The header (section 1).
struct Header {
  uint8_t type = 0;
  uint32_t mask = 0;
  uint16_t serial = 0, inResponseTo = 0, seq = 0, expected = 0, length = 0;
  static Header Read(const uint8_t* p) {
    Header h;
    h.type = p[0], h.mask = Get32(p + 1), h.serial = Get16(p + 5), h.inResponseTo = Get16(p + 7);
    h.seq = Get16(p + 9), h.expected = Get16(p + 11), h.length = Get16(p + 13);
    return h;
  }
};

struct Packet {
  std::vector<uint8_t> bytes;  // the whole datagram
  int64_t recvAt = 0;          // (received: GetTime at receipt)
  int64_t resendAt = 0;        // (an unacked DATA)
  int sends = 0;
  Header h() const { return Header::Read(bytes.data()); }
};

// GetTime 0x489f30: microseconds since 1601 (the system's clock once, then a steady one), never the same twice.
class Clock {
 public:
  Clock() {
    using namespace std::chrono;
    // (1601-01-01 to 1970-01-01: 11644473600 s)
    base_ = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count() + 11644473600LL * 1000000;
    start_ = steady_clock::now();
  }
  int64_t Now() {
    using namespace std::chrono;
    int64_t t = base_ + duration_cast<microseconds>(steady_clock::now() - start_).count();
    if (t <= last_) t = last_ + 1;
    return last_ = t;
  }

 private:
  int64_t base_ = 0, last_ = 0;
  std::chrono::steady_clock::time_point start_;
};

struct Conn {
  int id = 0;
  Address addr;
  State state = Pending;
  bool incoming = false;
  bool visible = true;   // (the owner still has it: not Closed by it, no Lost told)
  bool madeTold = false;
  bool closed = false, finished = false;  // conn+0xe40 / +0xe41
  bool outputShutdown = false, sentShutdown = false, inputEnded = false;
  uint16_t serial = 1, inResponseTo = 0;
  uint16_t nextSeq = 0, remoteExpected = 0, expected = 0;
  uint32_t mask = 0;
  int64_t lastSend = 0, lastRecv = 0, keepAliveStamp = 0, sendBy = 0, peerTime = 0;
  bool heard = false;
  uint8_t peerCompression = 0;
  std::array<uint8_t, 32> peerNonce{};
  // out: the owner's messages as bytes (deflated when on), cut into DATA as they go
  z_stream deflater{};
  bool deflating = false, needsFlush = false;
  std::string outPipe;
  size_t flushed = 0;
  std::list<Packet> unacked;  // by resend time
  // in
  z_stream inflater{};
  bool inflating = false;
  std::list<Packet> early;  // by sequence
  MessageSplitter split;
  // ping
  std::array<std::pair<uint16_t, int64_t>, kHistory> history{};
  std::vector<double> samples;
  double ping = 0;
  double backlog = 0;

  ~Conn() {
#if MOHO64_NET_DEFLATE
    if (deflating) deflateEnd(&deflater);
#endif
    if (inflating) inflateEnd(&inflater);
  }
};

class UdpCore {
 public:
  ~UdpCore() { conns_.clear(); }

  bool Open(uint16_t port) {
    if (!sock_.Bind(port, false)) return false;
    port_ = sock_.LocalPort();
    if (const char* d = std::getenv("MOHO64_NET_DROP")) dropPerMille_ = std::atoi(d);  // (tests: lost packets)
    thread_ = std::thread([this] { Run(); });
    return true;
  }
  // The owner's connector is gone (Destroy 0x489d20): every connection closed (its goodbye still goes out), no new
  // ones, and the thread ends by itself when they are done. `self` keeps the core alive until then.
  void Shutdown(std::shared_ptr<UdpCore> self) {
    {
      std::lock_guard<std::mutex> g(mu_);
      shuttingDown_ = true;
      shutdownAt_ = clock_.Now();
      for (auto& [id, c] : conns_) {
        c->visible = false, ScheduleDestroy(*c);
        c->flushed = c->outPipe.size();  // (as a Push: not held for a send deadline an ack just set)
      }
      // Ours: the ends of the streams out at once, on the caller's thread, so that a program that quits right
      // after (the lobby's Destroy, then ExitApplication) is still heard leaving; the thread then waits for the
      // acks and sends the goodbyes. (The original leaves all of it to its thread.)
      SendPass(clock_.Now());
      self_ = std::move(self);
      Wake();
      thread_.detach();  // (under the lock: the thread cannot end, and free us, before)
    }
  }

  uint16_t Port() const { return port_; }

  int Connect(const Address& to) {
    std::lock_guard<std::mutex> g(mu_);
    // Connect 0x48b2b0: an incoming one waiting from there is taken; a live one to there is replaced.
    for (auto& [id, c] : conns_) {
      if (!(c->addr == to) || c->state == Errored) continue;
      if (c->state == Pending && !c->closed) {
        c->state = Answering, c->incoming = false, c->visible = true;
        Wake();
        return id;
      }
      Fail(*c);  // (the original asserts on a TimedOut one here, line 0x95b; ours replaces it too)
    }
    Conn& c = Make(to, Connecting);
    c.incoming = false;
    Wake();
    return c.id;
  }
  void Accept(int conn) {
    std::lock_guard<std::mutex> g(mu_);
    if (Conn* c = Find(conn); c && c->state == Pending && !c->closed) c->state = Answering;
    Wake();
  }
  void Send(int conn, const Message& m) {
    std::lock_guard<std::mutex> g(mu_);
    Conn* c = Find(conn);
    if (!c || !c->visible || c->outputShutdown) return;
    std::string bytes;
    if (!AppendFramed(m, &bytes)) return;
    Write(*c, bytes);
  }
  void Close(int conn) {
    std::lock_guard<std::mutex> g(mu_);
    if (Conn* c = Find(conn)) c->visible = false, ScheduleDestroy(*c);
    Wake();
  }
  // Pull 0x48b5c0 / Push 0x48b7f0 from the owner: what happened since, and what it wrote sent now.
  void Poll(std::vector<ConnEvent>* out) {
    std::lock_guard<std::mutex> g(mu_);
    bool waiting = false;
    for (auto& [id, c] : conns_) {
      if (c->outputShutdown) continue;
      Flush(*c);
      c->flushed = c->outPipe.size();
      waiting = waiting || c->flushed > 0;
    }
    for (ConnEvent& e : events_) out->push_back(std::move(e));
    events_.clear();
    if (waiting) Wake();
  }
  Address PeerAddress(int conn) const {
    std::lock_guard<std::mutex> g(mu_);
    const Conn* c = Find(conn);
    return c ? c->addr : Address{};
  }
  double PingMs(int conn) const {
    std::lock_guard<std::mutex> g(mu_);
    const Conn* c = Find(conn);
    return c ? c->ping : 0.0;
  }
  // GetTime 0x489590: ms since anything was heard, -1 before that.
  double QuietMs(int conn) const {
    std::lock_guard<std::mutex> g(mu_);
    const Conn* c = Find(conn);
    if (!c || !c->heard) return -1.0;
    return static_cast<double>(clock_.Now() - c->lastRecv) * 0.001;
  }

 private:
  Conn* Find(int id) {
    auto it = conns_.find(id);
    return it == conns_.end() ? nullptr : it->second.get();
  }
  const Conn* Find(int id) const {
    auto it = conns_.find(id);
    return it == conns_.end() ? nullptr : it->second.get();
  }
  Conn& Make(const Address& to, State s) {
    auto c = std::make_unique<Conn>();
    c->id = next_++;
    c->addr = to;
    c->state = s;
    // ctor 0x485d30: our sends deflated (net_CompressionMethod 1; raw deflate, window 2^14, default level, 0x957432)
#if MOHO64_NET_DEFLATE
    if (kCompression == 1 && deflateInit2(&c->deflater, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -14, 8, Z_DEFAULT_STRATEGY) == Z_OK)
      c->deflating = true;
#endif
    Conn& r = *c;
    conns_[r.id] = std::move(c);
    return r;
  }
  void Event(Conn& c, ConnEvent::Kind k, Message m = {}) {
    if (!c.visible) return;
    events_.push_back(ConnEvent{k, c.id, c.incoming, std::move(m)});
  }
  // To Errored: its input ended (DispatchFromInput then says 202; ours: Lost, or Failed if it was never made).
  void Fail(Conn& c) {
    c.state = Errored;
    c.inputEnded = true;
    Event(c, c.madeTold ? ConnEvent::Lost : ConnEvent::Failed);
    c.visible = false;
    ScheduleDestroy(c);
  }

  // --- out ----------------------------------------------------------------------------------------------------

  void Deflate(Conn& c, const char* data, size_t size, int flush) {
    char buf[4096];
#if MOHO64_NET_DEFLATE
    c.deflater.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data));
    c.deflater.avail_in = static_cast<uInt>(size);
    do {
      c.deflater.next_out = reinterpret_cast<Bytef*>(buf);
      c.deflater.avail_out = sizeof buf;
      deflate(&c.deflater, flush);
      c.outPipe.append(buf, sizeof buf - c.deflater.avail_out);
    } while (c.deflater.avail_out == 0);
#else
    (void)buf, (void)data, (void)size, (void)flush;
#endif
  }
  // Write 0x489130: the message's bytes into the outgoing stream; sent by the send deadline at the latest.
  void Write(Conn& c, const std::string& bytes) {
    if (c.deflating) Deflate(c, bytes.data(), bytes.size(), Z_NO_FLUSH);
    else c.outPipe += bytes;
    if (c.sendBy == 0) c.sendBy = clock_.Now() + kSendDelay;
    c.needsFlush = true;
    Wake();
  }
  void Flush(Conn& c) {  // (Z_SYNC_FLUSH, 0x957884)
    if (c.needsFlush && c.deflating) Deflate(c, nullptr, 0, Z_SYNC_FLUSH);
    c.needsFlush = false;
  }
  // Close 0x4893f0 / ScheduleDestroy 0x489660: the stream finished (Z_FINISH), then the end-of-stream DATA and a
  // GOODBYE once it is acked.
  void ScheduleDestroy(Conn& c) {
    if (!c.outputShutdown) {
      Flush(c);
      if (c.deflating) Deflate(c, nullptr, 0, Z_FINISH);
      c.outputShutdown = true;
    }
    c.closed = true;
  }

  std::vector<uint8_t> NewPacket(Conn& c, uint8_t type, size_t payload) {  // NewPacket 0x488b20
    std::vector<uint8_t> b(kHeader + payload);
    b[0] = type;
    if (type != kConnect && type != kAnswer) {
      Put32(&b[1], c.mask);
      Put16(&b[9], c.nextSeq);
      Put16(&b[11], c.expected);
    }
    Put16(&b[13], static_cast<uint16_t>(payload));
    return b;
  }
  std::vector<uint8_t> NewHandshake(Conn& c, uint8_t type) {  // NewConnectPacket 0x488810 / NewAnswerPacket 0x4888c0
    std::vector<uint8_t> b = NewPacket(c, type, (type == kConnect ? kConnectSize : kAnswerSize) - kHeader);
    Put32(&b[15], kProtocolVersion);
    Put64(&b[19], static_cast<uint64_t>(clock_.Now()));
    b[27] = c.deflating ? 1 : 0;
    // 28..59: our nonce, zero (conn+0x454 is never written after the ctor's zeroing); ANSWER: 60..91 theirs
    if (type == kAnswer) std::memcpy(&b[60], c.peerNonce.data(), 32);
    return b;
  }

  static double ResendDelayMs(const Conn& c, int sends) {  // CalcResendDelay 0x488170
    double ms = std::nearbyint((c.ping * kResendPingMultiplier + kResendBiasMs) * static_cast<double>(1 << std::min(sends, 3)));
    return std::max(std::min(ms, kMaxResendMs), kMinResendMs);
  }

  // SendPacket 0x488d80: the serial and our answer to them stamped on, out, and the stamps after it.
  void SendPacket(Conn& c, std::vector<uint8_t> b, int64_t now, Packet* resend = nullptr) {
    Put16(&b[5], c.serial);
    Put16(&b[7], c.inResponseTo);
    sock_.SendTo(c.addr, b.data(), b.size());
    c.history[c.serial % kHistory] = {c.serial, now};
    ++c.serial;
    c.inResponseTo = 0;
    c.sendBy = c.outPipe.empty() ? 0 : now + kSendDelay;
    c.backlog = Backlog(c, now) + static_cast<double>(b.size());
    c.lastSend = now;
    if (b[0] != kAck) c.keepAliveStamp = now;
    if (b[0] != kData) return;
    Packet p;
    if (resend) p = std::move(*resend);
    p.bytes = std::move(b);
    p.resendAt = now + static_cast<int64_t>(ResendDelayMs(c, p.sends) * 1000);
    ++p.sends;
    auto at = std::find_if(c.unacked.begin(), c.unacked.end(), [&](const Packet& q) { return q.resendAt > p.resendAt; });
    c.unacked.insert(at, std::move(p));
  }
  double Backlog(const Conn& c, int64_t now) const {  // GetSentTime 0x488260: drained at the send rate
    return std::max(0.0, std::trunc(c.backlog - static_cast<double>(now - c.lastSend) * kMaxSendRate * 1e-6));
  }

  bool HasPacketWaiting(Conn& c, int64_t now) {  // 0x488730
    if (Diff(c.nextSeq, c.remoteExpected) > kWindow) return false;
    if (c.outPipe.empty() && c.outputShutdown && !c.sentShutdown) return true;
    if (now > c.sendBy || c.flushed > 0 || now >= c.keepAliveStamp + kKeepAliveEvery) {
      Flush(c);
      return !c.outPipe.empty();
    }
    return c.outPipe.size() >= kMaxPayload;
  }
  void SendNewData(Conn& c, int64_t now) {  // ReadPacket 0x488980: the next slice of the stream, or its end (length 0)
    const size_t n = std::min(c.outPipe.size(), kMaxPayload);
    std::vector<uint8_t> b = NewPacket(c, kData, n);
    std::memcpy(&b[kHeader], c.outPipe.data(), n);
    c.outPipe.erase(0, n);
    c.flushed = c.flushed > n ? c.flushed - n : 0;
    if (n == 0) c.sentShutdown = true;
    ++c.nextSeq;
    SendPacket(c, std::move(b), now);
  }

  // SendData 0x488300: at most one packet now; the time it wants to be called again (< 0: never).
  int64_t SendData(Conn& c, int64_t now, bool* sent) {
    *sent = false;
    if (c.closed && c.state != Established) c.finished = true;
    if (c.finished) return -1;
    switch (c.state) {
      case Pending: return -1;
      case Connecting:
      case Answering:
        if (c.inResponseTo != 0 || now - c.lastSend > kHandshakeRepeat) {
          SendPacket(c, NewHandshake(c, c.state == Connecting ? kConnect : kAnswer), now);
          *sent = true;
        }
        return c.lastSend + kHandshakeRepeat;
      case Established:
        if (now - c.lastRecv > kTimeout) {  // ("connection timed out.")
          c.state = TimedOut;
          return SendData(c, now, sent);
        }
        if (!c.unacked.empty() && c.unacked.front().resendAt <= now) {
          Packet p = std::move(c.unacked.front());
          c.unacked.pop_front();
          std::vector<uint8_t> b = std::move(p.bytes);
          SendPacket(c, std::move(b), now, &p);
          *sent = true;
          return now;
        }
        if (HasPacketWaiting(c, now)) {
          SendNewData(c, now);
          *sent = true;
          return now;
        }
        if (c.closed && c.sentShutdown && c.unacked.empty()) {  // (one GOODBYE, never resent)
          SendPacket(c, NewPacket(c, kGoodbye, 0), now);
          c.finished = true;
          *sent = true;
          return -1;
        }
        [[fallthrough]];
      case TimedOut:
        if (now >= c.keepAliveStamp + kKeepAliveEvery) {
          SendPacket(c, NewPacket(c, kKeepAlive, 0), now);
          *sent = true;
          return now;
        }
        if (c.state == Established && c.inResponseTo != 0 && now >= c.sendBy) {
          SendPacket(c, NewPacket(c, kAck, 0), now);
          *sent = true;
          return now;
        }
        {
          int64_t wake = c.keepAliveStamp + kKeepAliveEvery;
          if (c.state == Established) {
            if (!c.unacked.empty()) wake = std::min(wake, c.unacked.front().resendAt);
            if (c.sendBy != 0 && (c.inResponseTo != 0 || !c.outPipe.empty() || c.needsFlush)) wake = std::min(wake, c.sendBy);
            wake = std::min(wake, c.lastRecv + kTimeout + 1);
          }
          return wake;
        }
      case Errored: c.finished = true; return -1;
    }
    return -1;
  }

  // The send pass (0x48ac40), each connection within its rate; the time the thread should look again (< 0: none).
  int64_t SendPass(int64_t now) {
    int64_t wake = -1;
    auto earliest = [&](int64_t t) {
      if (t >= 0 && (wake < 0 || t < wake)) wake = t;
    };
    for (auto it = conns_.begin(); it != conns_.end();) {
      Conn& c = *it->second;
      for (int guard = 0; guard < 64; ++guard) {
        const double backlog = Backlog(c, now);
        if (backlog > kMaxBacklog) {
          earliest(now + static_cast<int64_t>((backlog - kMaxBacklog) * 1e6 / kMaxSendRate) + 1);
          break;
        }
        bool sent = false;
        const int64_t t = SendData(c, now, &sent);
        if (!sent || t != now) {
          earliest(t);
          break;
        }
      }
      if (c.finished && !c.visible) {  // (Pull deletes it, 0x48b764)
        it = conns_.erase(it);
        continue;
      }
      ++it;
    }
    return wake;
  }

  // --- in -----------------------------------------------------------------------------------------------------

  // CreateFilterStream 0x486910: made (201 first in its input), their deflate undone if they use it.
  void Establish(Conn& c) {
    c.state = Established;
    if (c.peerCompression == 1 && !c.inflating) c.inflating = inflateInit2(&c.inflater, -14) == Z_OK;
    if (!c.madeTold) {
      c.madeTold = true;
      Event(c, ConnEvent::Made);
    }
  }
  void Adopt(Conn& c, const Packet& p) {  // AdoptPacket 0x488220
    c.inResponseTo = p.h().serial;
    if (c.sendBy == 0) c.sendBy = p.recvAt + kAckDelay;
  }
  void Heard(Conn& c, int64_t at) { c.lastRecv = at, c.heard = true; }
  void PingSample(Conn& c, const Header& h, int64_t now) {  // UpdatePingInfoFromPacket 0x4874c0: median of 24
    if (h.inResponseTo == 0) return;
    const auto& [serial, at] = c.history[h.inResponseTo % kHistory];
    if (serial != h.inResponseTo) return;
    if (static_cast<int>(c.samples.size()) == kPingSamples) c.samples.erase(c.samples.begin());
    c.samples.push_back(static_cast<double>(now - at) * 0.001);
    std::vector<double> s = c.samples;
    std::sort(s.begin(), s.end());
    c.ping = s[s.size() / 2];
  }

  // CNetUDPConnection::ProcessConnect 0x486380; false: the peer restarted (this one Errored, a new one wanted).
  bool ProcessConnect(Conn& c, const Packet& p) {
    const uint8_t* b = p.bytes.data();
    const int64_t time = static_cast<int64_t>(Get64(b + 19));
    if (time <= c.peerTime) return true;  // ("ignoring obsolete CONNECT")
    if (b[27] >= 2) return true;
    if (c.state == Established || c.state == TimedOut) {
      Fail(c);
      return false;
    }
    std::memcpy(c.peerNonce.data(), b + 28, 32);
    c.peerTime = time, c.peerCompression = b[27];
    Heard(c, p.recvAt);
    Adopt(c, p);
    if (c.state == Connecting) c.state = Answering;
    return true;
  }
  void ProcessAnswer(Conn& c, const Packet& p, int64_t now) {  // 0x4865e0
    const uint8_t* b = p.bytes.data();
    if (p.bytes.size() != kAnswerSize) return;
    if (c.state != Connecting && c.state != Answering) return;
    const int64_t time = static_cast<int64_t>(Get64(b + 19));
    if (time <= c.peerTime) return;  // ("ignoring obsolete ANSWER")
    static const std::array<uint8_t, 32> ours{};
    if (std::memcmp(b + 60, ours.data(), 32) != 0) return;  // ("ANSWER with wrong receiver nonce")
    if (b[27] >= 2) return;
    PingSample(c, p.h(), now);
    Heard(c, p.recvAt);
    std::memcpy(c.peerNonce.data(), b + 28, 32);
    c.peerTime = time, c.peerCompression = b[27];
    Establish(c);
    Adopt(c, p);
  }
  // ProcessAckInternal 0x486b10: the header's acks; false when the packet is to be dropped.
  bool ProcessAcks(Conn& c, const Packet& p, int64_t now) {
    switch (c.state) {
      case Answering: Establish(c); break;
      case TimedOut: c.state = Established; break;
      case Established: break;
      default: return false;
    }
    const Header h = p.h();
    if (Diff(c.nextSeq, h.expected) < 0) return false;  // ("acknowledgement of data we haven't sent")
    c.unacked.remove_if([&](const Packet& q) {
      const int d = Diff(q.h().seq, h.expected);
      return d < 0 || (d >= 1 && d <= kWindow && (h.mask >> (d - 1) & 1));
    });
    if (Diff(h.expected, c.remoteExpected) > 0) c.remoteExpected = h.expected;
    PingSample(c, h, now);
    return true;
  }
  void Inflate(Conn& c, const uint8_t* data, size_t size) {
    if (!c.inflating) {
      c.split.Feed(reinterpret_cast<const char*>(data), size);
      return;
    }
    char buf[4096];
    c.inflater.next_in = const_cast<Bytef*>(data);
    c.inflater.avail_in = static_cast<uInt>(size);
    do {
      c.inflater.next_out = reinterpret_cast<Bytef*>(buf);
      c.inflater.avail_out = sizeof buf;
      const int r = inflate(&c.inflater, Z_SYNC_FLUSH);
      c.split.Feed(buf, sizeof buf - c.inflater.avail_out);
      if (r != Z_OK && r != Z_BUF_ERROR) break;  // (Z_STREAM_END: their Z_FINISH; errors: the split sees junk)
    } while (c.inflater.avail_out == 0);
  }
  void ProcessData(Conn& c, Packet p, int64_t now) {  // 0x486db0
    Adopt(c, p);
    const Header h = p.h();
    const int d = Diff(h.seq, c.expected);
    if (d < 0 || d > kWindow || c.inputEnded) return;  // (old; too far in the future; after the end)
    auto at = c.early.begin();
    while (at != c.early.end() && Diff(at->h().seq, h.seq) < 0) ++at;
    if (at != c.early.end() && at->h().seq == h.seq) return;  // ("repeat of future DATA")
    Heard(c, p.recvAt);
    c.early.insert(at, std::move(p));
    bool delivered = false;
    while (!c.early.empty() && c.early.front().h().seq == c.expected && !c.inputEnded) {
      const Packet& q = c.early.front();
      if (q.bytes.size() > kHeader) Inflate(c, q.bytes.data() + kHeader, q.bytes.size() - kHeader);
      else c.inputEnded = true;
      ++c.expected;
      c.early.pop_front();
      delivered = true;
    }
    c.mask = 0;
    for (const Packet& q : c.early) {
      const int e = Diff(q.h().seq, c.expected);
      if (e >= 1 && e <= kWindow) c.mask |= 1u << (e - 1);
    }
    if (delivered) Dispatch(c);
  }
  // DispatchFromInput 0x4876a0: whole messages to the owner; at the input's end, Lost (202 / 203).
  void Dispatch(Conn& c) {
    Message m;
    while (c.split.Next(&m)) Event(c, ConnEvent::Received, std::move(m));
    if (c.split.Broken() && !c.inputEnded) {
      Fail(c);
      return;
    }
    if (c.inputEnded && c.visible) {
      Event(c, ConnEvent::Lost);
      c.visible = false;
      ScheduleDestroy(c);
    }
  }
  void ProcessGoodbye(Conn& c) {  // 0x487370: before their end of stream, an error
    if (!c.inputEnded) Fail(c);
  }

  Conn* Live(const Address& a) {
    for (auto& [id, c] : conns_)
      if (c->addr == a && c->state != Errored) return c.get();
    return nullptr;
  }

  // ReceiveData 0x48a280: every datagram waiting. (MOHO64_NET_DELAY_MS, a test's switch: each held first, then taken
  // in once its time has come, in the order received; test_delay.h.)
  void ReceiveAll() {
    uint8_t buf[kMaxDatagram];
    for (;;) {
      Address from;
      const long n = sock_.ReceiveFrom(buf, sizeof buf, &from);
      if (n <= 0) break;
      if (dropPerMille_ > 0 && n >= static_cast<long>(kHeader)) {
        dropSeed_ = dropSeed_ * 1103515245u + 12345u;
        if ((dropSeed_ >> 16) % 1000 < static_cast<uint32_t>(dropPerMille_)) continue;
      }
      if (delay_.on() && n >= static_cast<long>(kHeader)) {
        held_.push_back(Held{delay_.ReleaseAt(clock_.Now()), from, std::vector<uint8_t>(buf, buf + n)});
        continue;
      }
      Take(from, buf, n, clock_.Now());
    }
    while (!held_.empty() && held_.front().at <= clock_.Now()) {
      Held h = std::move(held_.front());
      held_.pop_front();
      Take(h.from, h.bytes.data(), static_cast<long>(h.bytes.size()), clock_.Now());
    }
  }
  // One datagram taken in (received at `now`).
  void Take(const Address& from, const uint8_t* buf, long n, int64_t now) {
    if (buf[0] == kNat) return;  // (ours has no NAT traversal provider)
    if (static_cast<size_t>(n) < kHeader) return;  // ("ignoring short packet"; our wake-ups too)
    Packet p;
    p.bytes.assign(buf, buf + n);
    p.recvAt = now;
    const Header h = p.h();
    if (h.type >= 9 || h.length != static_cast<size_t>(n) - kHeader) return;
    if (h.type == kConnect) {  // CNetUDPConnector::ProcessConnect 0x48aa40
      if (p.bytes.size() != kConnectSize || Get32(&buf[15]) != kProtocolVersion) return;
      Conn* c = Live(from);
      if (c && ProcessConnect(*c, p)) return;
      if (shuttingDown_) return;
      Conn& fresh = Make(from, Pending);
      fresh.incoming = true;
      ProcessConnect(fresh, p);
      Event(fresh, ConnEvent::Incoming);
      return;
    }
    Conn* c = Live(from);
    if (!c) return;  // ("ignoring packet of type %d from unknown host")
    switch (h.type) {
      case kAnswer: ProcessAnswer(*c, p, now); break;
      case kData:
        if (ProcessAcks(*c, p, now)) ProcessData(*c, std::move(p), now);
        break;
      case kAck:
        if (ProcessAcks(*c, p, now)) Heard(*c, p.recvAt);
        break;
      case kKeepAlive:
        if (ProcessAcks(*c, p, now)) Heard(*c, p.recvAt), Adopt(*c, p);
        break;
      case kGoodbye:
        if (ProcessAcks(*c, p, now)) Heard(*c, p.recvAt), ProcessGoodbye(*c);
        break;
      default: break;  // ("ignoring unimplemented packet of type %d")
    }
  }

  // The thread (0x489f90): receive, send, wait for the socket or the next thing due.
  void Run() {
    for (;;) {
      int waitMs = -1;
      {
        std::lock_guard<std::mutex> g(mu_);
        ReceiveAll();
        const int64_t now = clock_.Now();
        int64_t wake = SendPass(now);
        if (!held_.empty() && (wake < 0 || held_.front().at < wake)) wake = held_.front().at;  // (the test's delay)
        if (shuttingDown_ && (conns_.empty() || now - shutdownAt_ > kShutdownLinger)) break;
        if (wake >= 0) waitMs = static_cast<int>(std::clamp<int64_t>((wake - now + 999) / 1000, 0, 60000));
        if (shuttingDown_) waitMs = waitMs < 0 ? 50 : std::min(waitMs, 50);
      }
      if (sock_.Wait(waitMs) < 0) {
        std::lock_guard<std::mutex> g(mu_);
        if (shuttingDown_) break;
      }
    }
    std::shared_ptr<UdpCore> self;
    {
      std::lock_guard<std::mutex> g(mu_);
      self = std::move(self_);
    }
    sock_.Close();
    self.reset();  // (the last reference: the core goes as its thread ends; nothing of it is touched after)
  }
  void Wake() {  // (a one-byte datagram to our own port wakes the thread's select)
    static const uint8_t poke = 0xff;
    sock_.SendTo(Address{0x7f000001u, port_}, &poke, 1);
  }

  mutable std::mutex mu_;
  UdpSocket sock_;
  uint16_t port_ = 0;
  std::thread thread_;
  mutable Clock clock_;
  std::map<int, std::unique_ptr<Conn>> conns_;
  std::vector<ConnEvent> events_;
  int next_ = 1;
  bool shuttingDown_ = false;
  int64_t shutdownAt_ = 0;
  int dropPerMille_ = 0;
  uint32_t dropSeed_ = 1;
  // MOHO64_NET_DELAY_MS / MOHO64_NET_JITTER_MS (a test's switch): datagrams received and not yet taken in, by time
  struct Held {
    int64_t at = 0;
    Address from;
    std::vector<uint8_t> bytes;
  };
  TestDelay delay_{"UDP"};
  std::deque<Held> held_;
  std::shared_ptr<UdpCore> self_;
};

class UdpConnector final : public Connector {
 public:
  explicit UdpConnector(std::shared_ptr<UdpCore> core) : core_(std::move(core)) {}
  ~UdpConnector() override { core_->Shutdown(core_); }
  const char* Protocol() const override { return "UDP"; }
  uint16_t LocalPort() const override { return core_->Port(); }
  void SetLocalUid(int32_t) override {}
  int Connect(const Address& to) override { return core_->Connect(to); }
  void Send(int conn, const Message& m) override { core_->Send(conn, m); }
  void Close(int conn) override { core_->Close(conn); }
  void CloseAfterSend(int conn) override { core_->Close(conn); }  // (a close sends what is queued first anyway)
  void Poll(std::vector<ConnEvent>* out) override { core_->Poll(out); }
  Address PeerAddress(int conn) const override { return core_->PeerAddress(conn); }
  int32_t PeerUid(int) const override { return -1; }
  double PingMs(int conn) const override { return core_->PingMs(conn); }
  double QuietMs(int conn) const override { return core_->QuietMs(conn); }
  void Accept(int conn) override { core_->Accept(conn); }
  bool BothEndsConnect() const override { return true; }

 private:
  std::shared_ptr<UdpCore> core_;
};

}  // namespace

std::unique_ptr<Connector> MakeUdpConnector(uint16_t port) {
  auto core = std::make_shared<UdpCore>();
  if (!core->Open(port)) return nullptr;
  return std::make_unique<UdpConnector>(std::move(core));
}

}  // namespace moho::net
