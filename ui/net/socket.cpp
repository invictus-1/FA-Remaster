#include "net/socket.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using SockLen = int;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using SockLen = socklen_t;
#endif

namespace moho::net {
namespace {

#ifdef _WIN32
using Native = SOCKET;
Native N(intptr_t fd) { return static_cast<Native>(fd); }
bool WouldBlock() {
  const int e = WSAGetLastError();
  return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
}
void CloseNative(Native s) { closesocket(s); }
bool SetNonBlocking(Native s) {
  u_long on = 1;
  return ioctlsocket(s, FIONBIO, &on) == 0;
}
#else
using Native = int;
Native N(intptr_t fd) { return static_cast<Native>(fd); }
bool WouldBlock() { return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS; }
void CloseNative(Native s) { close(s); }
bool SetNonBlocking(Native s) { return fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK) == 0; }
#endif

sockaddr_in ToSockaddr(const Address& a) {
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(a.ip);
  sa.sin_port = htons(a.port);
  return sa;
}

}  // namespace

bool NetStartup() {
#ifdef _WIN32
  static const bool ok = [] {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return ok;
#else
  return true;
#endif
}

std::string Address::IpString() const {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%u.%u.%u.%u", (ip >> 24) & 255u, (ip >> 16) & 255u, (ip >> 8) & 255u, ip & 255u);
  return buf;
}

std::string Address::ToString() const { return IpString() + ":" + std::to_string(port); }

bool Address::Parse(const std::string& text, uint16_t defaultPort, Address* out) {
  if (!NetStartup()) return false;
  std::string host = text;
  uint16_t port = defaultPort;
  const size_t colon = text.rfind(':');
  if (colon != std::string::npos) {
    host = text.substr(0, colon);
    const std::string p = text.substr(colon + 1);
    if (p.empty() || p.find_first_not_of("0123456789") != std::string::npos) return false;
    const unsigned long v = std::strtoul(p.c_str(), nullptr, 10);
    if (v > 65535) return false;
    port = static_cast<uint16_t>(v);
  }
  if (host.empty()) return false;
  addrinfo hints{};
  hints.ai_family = AF_INET;
  addrinfo* found = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || !found) return false;
  const sockaddr_in* sa = reinterpret_cast<const sockaddr_in*>(found->ai_addr);
  out->ip = ntohl(sa->sin_addr.s_addr);
  out->port = port;
  freeaddrinfo(found);
  return true;
}

TcpSocket::~TcpSocket() { Close(); }

void TcpSocket::Close() {
  if (fd_ != kNone) CloseNative(N(fd_));
  fd_ = kNone;
}

bool TcpSocket::MakeSocket() {
  if (!NetStartup()) return false;
  Close();
  Native s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
  if (s == INVALID_SOCKET) return false;
#else
  if (s < 0) return false;
#endif
  if (!SetNonBlocking(s)) {
    CloseNative(s);
    return false;
  }
  int one = 1;  // (lobby messages are small and wanted at once)
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
  fd_ = static_cast<intptr_t>(s);
  return true;
}

bool TcpSocket::Listen(uint16_t port) {
  if (!MakeSocket()) return false;
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(INADDR_ANY);
  sa.sin_port = htons(port);
  if (bind(N(fd_), reinterpret_cast<const sockaddr*>(&sa), sizeof sa) != 0 || listen(N(fd_), 16) != 0) {
    Close();
    return false;
  }
  return true;
}

std::unique_ptr<TcpSocket> TcpSocket::Accept(Address* from) {
  if (fd_ == kNone) return nullptr;
  sockaddr_in sa{};
  SockLen len = sizeof sa;
  Native s = accept(N(fd_), reinterpret_cast<sockaddr*>(&sa), &len);
#ifdef _WIN32
  if (s == INVALID_SOCKET) return nullptr;
#else
  if (s < 0) return nullptr;
#endif
  SetNonBlocking(s);
  int one = 1;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
  auto out = std::make_unique<TcpSocket>();
  out->fd_ = static_cast<intptr_t>(s);
  if (from) from->ip = ntohl(sa.sin_addr.s_addr), from->port = ntohs(sa.sin_port);
  return out;
}

bool TcpSocket::BeginConnect(const Address& to) {
  if (!MakeSocket()) return false;
  const sockaddr_in sa = ToSockaddr(to);
  if (connect(N(fd_), reinterpret_cast<const sockaddr*>(&sa), sizeof sa) == 0) return true;
  if (WouldBlock()) return true;
  Close();
  return false;
}

int TcpSocket::CheckConnect() {
  if (fd_ == kNone) return -1;
  fd_set w, e;
  FD_ZERO(&w);
  FD_ZERO(&e);
  FD_SET(N(fd_), &w);
  FD_SET(N(fd_), &e);
  timeval none{0, 0};
  const int n = select(static_cast<int>(fd_ + 1), nullptr, &w, &e, &none);
  if (n < 0) return -1;
  if (n == 0) return 0;
  if (FD_ISSET(N(fd_), &e)) return -1;
  int err = 0;
  SockLen len = sizeof err;
  getsockopt(N(fd_), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
  return err == 0 ? 1 : -1;
}

long TcpSocket::Send(const void* data, size_t size) {
  if (fd_ == kNone) return -1;
  if (size == 0) return 0;
#ifdef _WIN32
  const int n = send(N(fd_), static_cast<const char*>(data), static_cast<int>(size), 0);
#else
  const long n = send(N(fd_), data, size, MSG_NOSIGNAL);
#endif
  if (n >= 0) return n;
  return WouldBlock() ? 0 : -1;
}

long TcpSocket::Receive(void* data, size_t size) {
  if (fd_ == kNone) return -1;
#ifdef _WIN32
  const int n = recv(N(fd_), static_cast<char*>(data), static_cast<int>(size), 0);
#else
  const long n = recv(N(fd_), data, size, 0);
#endif
  if (n > 0) return n;
  if (n == 0) return -1;  // (the other side closed)
  return WouldBlock() ? 0 : -1;
}

UdpSocket::~UdpSocket() { Close(); }

void UdpSocket::Close() {
  if (fd_ != kNone) CloseNative(N(fd_));
  fd_ = kNone;
}

bool UdpSocket::Bind(uint16_t port, bool broadcast) {
  if (!NetStartup()) return false;
  Close();
  Native s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
  if (s == INVALID_SOCKET) return false;
#else
  if (s < 0) return false;
#endif
  SetNonBlocking(s);
  int one = 1;
  if (broadcast) setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one), sizeof one);
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(INADDR_ANY);
  sa.sin_port = htons(port);
  if (bind(s, reinterpret_cast<const sockaddr*>(&sa), sizeof sa) != 0) {
    CloseNative(s);
    return false;
  }
  fd_ = static_cast<intptr_t>(s);
  return true;
}

bool UdpSocket::SendTo(const Address& to, const void* data, size_t size) {
  if (fd_ == kNone) return false;
  const sockaddr_in sa = ToSockaddr(to);
  return sendto(N(fd_), static_cast<const char*>(data), static_cast<int>(size), 0, reinterpret_cast<const sockaddr*>(&sa),
                sizeof sa) == static_cast<int>(size);
}

long UdpSocket::ReceiveFrom(void* data, size_t size, Address* from) {
  if (fd_ == kNone) return -1;
  sockaddr_in sa{};
  SockLen len = sizeof sa;
  const long n = recvfrom(N(fd_), static_cast<char*>(data), static_cast<int>(size), 0, reinterpret_cast<sockaddr*>(&sa), &len);
  if (n >= 0) {
    if (from) from->ip = ntohl(sa.sin_addr.s_addr), from->port = ntohs(sa.sin_port);
    return n;
  }
#ifdef _WIN32
  if (WSAGetLastError() == WSAECONNRESET) return 0;  // (Windows reports a port nobody listened on as an error here)
#endif
  return WouldBlock() ? 0 : -1;
}

uint16_t UdpSocket::LocalPort() const {
  if (fd_ == kNone) return 0;
  sockaddr_in sa{};
  SockLen len = sizeof sa;
  if (getsockname(N(fd_), reinterpret_cast<sockaddr*>(&sa), &len) != 0) return 0;
  return ntohs(sa.sin_port);
}

uint16_t TcpSocket::LocalPort() const {
  if (fd_ == kNone) return 0;
  sockaddr_in sa{};
  SockLen len = sizeof sa;
  if (getsockname(N(fd_), reinterpret_cast<sockaddr*>(&sa), &len) != 0) return 0;
  return ntohs(sa.sin_port);
}

}  // namespace moho::net
