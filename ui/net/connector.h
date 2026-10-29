// The connector: one local port, from which this player makes and accepts connections to the others, each a reliable,
// ordered stream of messages (the original's INetConnector / INetConnection: CNetTCPConnector NET_MakeTCPConnector
// 0x4849a0, CNetUDPConnector NET_MakeUDPConnector 0x48bbe0). Polled by its owner; what happened comes back as events.
//
// The UDP connector is the original's (its packets, handshake, acks and resends, read in full:
// docs/ui-net-udp.md); it has no hello, so a peer's address is its datagrams' source and its uid unknown.
//
// The TCP connector (ours in its details, INFERRED from the classes' shape): messages back to back on the stream,
// and on every new connection both ends first send a hello of the connector's own (message 210: the port it listens
// on, its uid), so that a peer's listening address and uid are known as the connection is made: the original's
// lobby messages carry no port for a joiner (its Join is name and uid only), its UDP connector learns it from the
// datagrams' source. Messages 211 / 212 (ping / its answer, each second) give the connection's ping and how long ago
// it was heard from (the lobby's GetPeer: ping, quiet). None of the three reaches the connector's owner.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "net/message.h"
#include "net/socket.h"

namespace moho::net {

// Incoming (UDP only): someone at PeerAddress(conn) wants a connection; it waits until the owner calls Accept(conn),
// or Connect to that address (which takes it), or Close(conn). TCP's are accepted at once (Made, incoming).
struct ConnEvent {
  enum Kind { Made, Failed, Lost, Received, Incoming } kind = Made;
  int conn = 0;
  bool incoming = false;  // (Made: the other end connected to us)
  Message msg;            // (Received)
};

class Connector {
 public:
  virtual ~Connector() = default;
  virtual const char* Protocol() const = 0;  // "TCP", "UDP"
  virtual uint16_t LocalPort() const = 0;
  virtual void SetLocalUid(int32_t uid) = 0;  // (the hello's uid from now on: a joiner learns its own from the host)
  virtual int Connect(const Address& to) = 0;  // a new connection's id, under way (Made or Failed follows)
  virtual void Send(int conn, const Message& m) = 0;
  virtual void Close(int conn) = 0;  // at once, quietly (no event)
  virtual void CloseAfterSend(int conn) = 0;  // once what is queued for it has gone out, quietly
  virtual void Poll(std::vector<ConnEvent>* out) = 0;
  virtual Address PeerAddress(int conn) const = 0;  // where the other end listens (its IP, its game port)
  virtual int32_t PeerUid(int conn) const = 0;      // the uid its hello gave (-1: none yet)
  virtual double PingMs(int conn) const = 0;
  virtual double QuietMs(int conn) const = 0;  // since anything was last heard from it (-1: never)
  virtual void Accept(int /*conn*/) {}         // (an Incoming one)
  // True when both ends of a pair should Connect (UDP: the two CONNECTs make one link); false when one connects and
  // the other is connected to (TCP: of a pair, the lower uid connects).
  virtual bool BothEndsConnect() const { return false; }
};

// The connector for a protocol name ("TCP", or "UDP": the original's packets, connector_udp.cpp), listening on
// `port` (0: a port the system chooses). nullptr when the port cannot be opened (the original's InternalCreateLobby
// then returns nil).
std::unique_ptr<Connector> MakeConnector(const std::string& protocol, uint16_t port);
std::unique_ptr<Connector> MakeUdpConnector(uint16_t port);

}  // namespace moho::net
