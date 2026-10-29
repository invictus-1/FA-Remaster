// Plain sockets for the lobby and, later, the game's network (ui/net/: no interface dependencies, so the engine can
// build it into its headless moho64 for two-process lockstep tests; agreed 2026-10-28). Non-blocking, polled by their
// owner each frame. IPv4, as the original's connectors (the peers' addresses are a u32 and a u16 in its lobby
// messages: SPeer +0x28 / +0x2c).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace moho::net {

// The platform's sockets made ready once (Winsock's WSAStartup); false when they cannot be.
bool NetStartup();

struct Address {
  uint32_t ip = 0;    // host order
  uint16_t port = 0;  // host order
  std::string ToString() const;  // "a.b.c.d:port"
  std::string IpString() const;  // "a.b.c.d"
  // "host:port" or "host" (then `defaultPort`); the host a name or dotted quad, resolved to its first IPv4 address
  // (the original's ValidateIPAddress 0x7c8360 resolves too). False when it cannot be.
  static bool Parse(const std::string& text, uint16_t defaultPort, Address* out);
  bool operator==(const Address& o) const { return ip == o.ip && port == o.port; }
};

// A TCP socket: a listener, or one stream (outgoing, or accepted by a listener).
class TcpSocket {
 public:
  TcpSocket() = default;
  ~TcpSocket();
  TcpSocket(const TcpSocket&) = delete;
  TcpSocket& operator=(const TcpSocket&) = delete;

  bool Listen(uint16_t port);  // on every interface; 0: a port the system chooses
  std::unique_ptr<TcpSocket> Accept(Address* from);  // nullptr when nobody waits
  bool BeginConnect(const Address& to);  // non-blocking; then CheckConnect
  int CheckConnect();  // 1 connected, 0 still under way, -1 failed
  long Send(const void* data, size_t size);  // bytes taken (0: none now), -1: the stream is gone
  long Receive(void* data, size_t size);     // bytes read (0: none now), -1: closed or broken
  uint16_t LocalPort() const;
  bool Valid() const { return fd_ != kNone; }
  void Close();

 private:
  static constexpr intptr_t kNone = -1;
  intptr_t fd_ = kNone;
  bool MakeSocket();
};

// A UDP socket (the lobby's discovery: the host listens on port 15000, a browser broadcasts to it; the UDP
// connector's one socket, connector_udp.cpp).
class UdpSocket {
 public:
  UdpSocket() = default;
  ~UdpSocket();
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;

  bool Bind(uint16_t port, bool broadcast);  // on every interface; 0: a port the system chooses
  bool SendTo(const Address& to, const void* data, size_t size);
  long ReceiveFrom(void* data, size_t size, Address* from);  // bytes (0: none now), -1: broken
  int Wait(int timeoutMs);  // until a datagram waits (1) or `timeoutMs` passed (0; < 0: no limit); -1 broken
  uint16_t LocalPort() const;
  bool Valid() const { return fd_ != kNone; }
  void Close();

 private:
  static constexpr intptr_t kNone = -1;
  intptr_t fd_ = kNone;
};

constexpr uint32_t kBroadcastIp = 0xFFFFFFFFu;  // 255.255.255.255

}  // namespace moho::net
