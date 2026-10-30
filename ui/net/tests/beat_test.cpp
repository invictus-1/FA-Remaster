// The game's orders between two players over the UDP connector, without the interface or the simulation: client 0
// and client 1 each send their beats (the replay stream's messages, then an Advance), every beat is collected whole
// and sorted by client at both ends, an empty beat arrives empty, the checksums every 50 beats reach the
// other side; then the control messages: READY, chat both ways and to oneself, the game speed agreed (a tie goes to
// the lower client), an eject on this program's word (Eject) and by vote (RequestEject: the client left out after its
// last beat in here, the dialog's ejectedBy). Build as udp_test.cpp (its header), with beat_test.cpp,
// ui/net/beat_exchange_net.cpp and src/core/log.cpp in place of udp_test.cpp, and -I src for sim/beat_exchange.h.
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>

#include "net/beat_exchange_net.h"

using namespace moho;
using namespace moho::net;

namespace {

int failed = 0;
void Check(const char* what, bool ok) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++failed;
}

std::string Orders(uint8_t client, uint32_t beat) {  // (one message of type 20: the client and the beat)
  std::string out;
  AppendFramed(MessageWriter().U8(client).U32(beat).Done(20), &out);
  if (beat % 7 == 0) AppendFramed(MessageWriter().Str("seven").Done(21), &out);
  return out;
}

}  // namespace

int main() {
  NetStartup();
  auto ca = MakeConnector("UDP", 0), cb = MakeConnector("UDP", 0);
  if (!ca || !cb) return Check("ports open", false), 1;
  const Address atB{0x7f000001u, cb->LocalPort()}, atA{0x7f000001u, ca->LocalPort()};
  const int toB = ca->Connect(atB), toA = cb->Connect(atA);  // (both ends connect, as the lobby's peers do)
  int made = 0;
  std::vector<ConnEvent> ev;
  for (int i = 0; i < 2000 && made < 2; ++i) {
    ev.clear();
    ca->Poll(&ev), cb->Poll(&ev);
    for (const auto& e : ev) made += e.kind == ConnEvent::Made;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  Check("the two are connected", made == 2);
  NetBeatExchange a(std::move(ca), 0, {GameClient{1, toB}}, true, 0), b(std::move(cb), 1, {GameClient{0, toA}}, true, 0);

  constexpr uint32_t kBeats = 200;
  bool whole = true, sorted = true, waited = true;
  int checksumsAtA = 0, checksumsAtB = 0;
  for (uint32_t beat = 1; beat <= kBeats; ++beat) {
    a.SendLocal(beat, beat == 13 ? std::string() : Orders(0, beat));  // (beat 13: nothing from a, an empty batch)
    b.SendLocal(beat, Orders(1, beat));
    if (beat % 50 == 0) {
      BeatChecksum c;
      c.beat = beat;
      for (int k = 0; k < 16; ++k) c.digest[k] = static_cast<uint8_t>(beat + k);
      a.SendChecksum(c), b.SendChecksum(c);
    }
    std::vector<ClientBatch> atA, atB;
    const auto stop = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    bool gotA = false, gotB = false;
    while (!(gotA && gotB) && std::chrono::steady_clock::now() < stop) {
      if (!gotA) gotA = a.Collect(beat, &atA);
      if (!gotB) gotB = b.Collect(beat, &atB);
      if (!(gotA && gotB)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!gotA || !gotB) {
      waited = false;
      break;
    }
    for (auto* batches : {&atA, &atB}) {
      if (batches->size() != 2 || (*batches)[0].client != 0 || (*batches)[1].client != 1) sorted = false;
      if (batches->size() == 2) {
        if ((*batches)[0].bytes != (beat == 13 ? std::string() : Orders(0, beat))) whole = false;
        if ((*batches)[1].bytes != Orders(1, beat)) whole = false;
      }
    }
    std::vector<std::pair<uint8_t, BeatChecksum>> cs;
    a.TakePeerChecksums(&cs);
    for (const auto& [client, c] : cs) checksumsAtA += client == 1 && c.digest[3] == static_cast<uint8_t>(c.beat + 3);
    cs.clear();
    b.TakePeerChecksums(&cs);
    for (const auto& [client, c] : cs) checksumsAtB += client == 0 && c.digest[3] == static_cast<uint8_t>(c.beat + 3);
  }
  Check("every beat collected at both ends", waited);
  Check("each beat's batches sorted by client", sorted);
  Check("each batch whole (and the empty beat empty)", whole);
  // (the last checksum may still be on its way: wait a little for it)
  for (int i = 0; i < 500 && (checksumsAtA < 4 || checksumsAtB < 4); ++i) {
    std::vector<std::pair<uint8_t, BeatChecksum>> cs;
    a.TakePeerChecksums(&cs);
    checksumsAtA += static_cast<int>(cs.size());
    cs.clear();
    b.TakePeerChecksums(&cs);
    checksumsAtB += static_cast<int>(cs.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  Check("the 4 checksums reach each side", checksumsAtA == 4 && checksumsAtB == 4);

  // (until `cond`, both sides pumped, at most 5 s)
  auto until = [&](const std::function<bool()>& cond) {
    for (int i = 0; i < 2500; ++i) {
      if (cond()) return true;
      a.ReceivedBeats(1), b.ReceivedBeats(0);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return cond();
  };
  // READY
  a.SendReady();
  until([&] { return false; });
  Check("not everyone is ready while one has not said so", !a.EveryoneReady());
  b.SendReady();
  Check("then everyone is, at both ends", until([&] { return a.EveryoneReady() && b.EveryoneReady(); }));
  // chat: to the other, and to oneself (straight back)
  std::vector<std::pair<uint8_t, std::string>> heardA, heardB;
  a.SendChat(1, "to b");
  b.SendChat(0, "to a");
  a.SendChat(0, "to myself");
  until([&] {
    for (auto& c : a.TakeChats()) heardA.push_back(c);
    for (auto& c : b.TakeChats()) heardB.push_back(c);
    return heardA.size() >= 2 && heardB.size() >= 1;
  });
  auto has = [](const std::vector<std::pair<uint8_t, std::string>>& v, uint8_t from, const char* text) {
    for (const auto& c : v)
      if (c.first == from && c.second == text) return true;
    return false;
  };
  Check("chat a -> b arrives, from a", has(heardB, 0, "to b"));
  Check("chat b -> a arrives, from b", has(heardA, 1, "to a"));
  Check("chat to oneself comes back", has(heardA, 0, "to myself") && heardA.size() == 2 && heardB.size() == 1);
  // game speed: one asks, both end on it; two ask at once: the lower client's wins on both
  a.RequestRate(3);
  Check("the speed asked for here is taken here at once", a.Rate() == 3);
  Check("and there", until([&] { return b.Rate() == 3; }));
  a.RequestRate(5);
  b.RequestRate(7);
  Check("two asking at once: both end on the lower client's", until([&] { return a.Rate() == 5 && b.Rate() == 5; }) &&
                                                                  !until([&] { return a.Rate() != 5 || b.Rate() != 5; }));
  const auto notesA = a.TakeRateNotes(), notesB = b.TakeRateNotes();
  Check("the changes are noted for the interface (who, what)", !notesA.empty() && notesA.front() == std::make_pair(uint8_t{0}, 3) &&
                                                                !notesB.empty() && notesB.back() == std::make_pair(uint8_t{0}, 5));
  // b votes a out before a's next beat reaches it: b goes on alone from the beat after a's last one in here
  const uint32_t aLast = b.ReceivedBeats(0);
  b.RequestEject(0);
  const ClientLink seen = b.Link(0);
  Check("the ejected client shows as not connected, ejected by b", !seen.connected && seen.ejectedBy.size() == 1 && seen.ejectedBy[0] == 1);
  std::vector<ClientBatch> lastAtB;
  b.SendLocal(kBeats + 1, Orders(1, kBeats + 1));
  bool bAlone = true;
  for (uint32_t beat = kBeats + 1; beat <= kBeats + 1 && aLast == kBeats; ++beat)
    bAlone = until([&] { return b.Collect(beat, &lastAtB); }) && lastAtB.size() == 1 && lastAtB[0].client == 1;
  Check("a client voted out is left out after its last beat", aLast == kBeats && bAlone);
  // a leaves b out on its own word (the engine's test)
  a.Eject(1, kBeats + 1);
  std::vector<ClientBatch> alone;
  a.SendLocal(kBeats + 1, Orders(0, kBeats + 1));
  Check("an ejected client is not waited for", a.Collect(kBeats + 1, &alone) && alone.size() == 1 && alone[0].client == 0);
  std::printf("beat_test: %d failed\n", failed);
  return failed ? 1 : 0;
}
