// The game's orders between two players over the UDP connector, without the interface or the simulation: client 0
// and client 1 each send their beats (the replay stream's messages, then an Advance), every beat is collected whole
// and sorted by client at both ends, an empty beat arrives empty, the checksums every 50 beats reach the
// other side, and an ejected client is left out. Build as udp_test.cpp (its header), with beat_test.cpp and
// ui/net/beat_exchange_net.cpp in place of udp_test.cpp, and -I src for sim/beat_exchange.h.
#include <chrono>
#include <cstdio>
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
  NetBeatExchange a(std::move(ca), 0, {GameClient{1, toB}}), b(std::move(cb), 1, {GameClient{0, toA}});

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
  a.Eject(1, kBeats + 1);
  std::vector<ClientBatch> alone;
  a.SendLocal(kBeats + 1, Orders(0, kBeats + 1));
  Check("an ejected client is not waited for", a.Collect(kBeats + 1, &alone) && alone.size() == 1 && alone[0].client == 0);
  std::printf("beat_test: %d failed\n", failed);
  return failed ? 1 : 0;
}
