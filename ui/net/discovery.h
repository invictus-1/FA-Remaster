// LAN games (the original's discovery: _scratch/ui/netlobby_survey.md section 2 and 3). A host with a networked
// lobby listens on UDP port 15000 (CLobby's listener, HostGame 0x7c1bae); a browser (CDiscoveryService 0x7bf650)
// broadcasts a request there every 2 s (message 110, no payload) and the hosts answer (111: u8 11 the discovery's
// version, u8 1 the lobby protocol's, u8 0 a SupCom game, u8 the connector's protocol, u16 the game port, then the
// script's GameConfigRequested table in the engine's byte form). A host not heard from for 5 s is gone. Both are
// CMessages in a datagram, as everything else.
//
// Ours: the browser asks 127.0.0.1 too, so that a game hosted on this machine is found also where a broadcast
// does not come back to the sender. Protocol numbers: 0 None, 1 TCP, 2 UDP (NET_GetProtocolName 0x47ec90: 2 is
// "UDP"; 0 / 1 by the names' order, INFERRED).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "net/socket.h"

namespace moho::net {

constexpr uint16_t kDiscoveryPort = 15000;
uint8_t ProtocolNumber(const std::string& name);
std::string ProtocolName(uint8_t number);

// The host's side: requests in, answers out.
class DiscoveryListener {
 public:
  bool Open();  // false: the port is taken ("someone else must be hosting a game on this machine")
  void Poll(std::vector<Address>* requests);
  void Answer(const Address& to, uint8_t protocol, uint16_t gamePort, const std::string& table);

 private:
  UdpSocket sock_;
};

// The browser's side.
struct DiscoveredGame {
  int index = 0;      // the browser's own number for it, from 0
  Address address;    // the host's IP and its game port
  uint8_t protocol = 0;
  std::string table;  // the host's GameConfigRequested table, as bytes
};
class DiscoveryBrowser {
 public:
  bool Open();
  void Poll(std::vector<DiscoveredGame>* found, std::vector<DiscoveredGame>* updated, std::vector<int>* removed);
  int Count() const { return static_cast<int>(games_.size()); }
  void Reset(std::vector<int>* removed);  // every game forgotten (their RemoveGame), asking again at once

 private:
  struct Known {
    DiscoveredGame game;
    double heard = 0;
  };
  UdpSocket sock_;
  double lastAsked_ = -1e9;
  int next_ = 0;
  std::map<uint64_t, Known> games_;  // by the host's address
};

}  // namespace moho::net
