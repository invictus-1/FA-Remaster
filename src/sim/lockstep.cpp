// A network game's beats (sim/lockstep.h). The rules are the original's, read in its client manager and sim driver
// (FA build 3839; the net helper's notes, 2026-10-30):
//
// * The merge: CClientManagerImpl::UpdateStates (FAF 0x12900c0 / 0x53f010) runs CClientBase::UpdateState 0x53c550
//   for each client in client order and hands the sim what they wrote, then one Advance. A client's command source
//   (client+0x50) is kept from beat to beat: each beat its part starts with a SetCommandSource of it (0x53c5aa) when
//   it has one. A SetCommandSource of a source in the client's own set (client+0x30) becomes its source and is passed
//   on (0x53c711); one of another source is logged ("Client %d:%s claiming command source %d, but not authorized for
//   it.", 0x53c733), not passed on, and the client is left with no source (0x53c7d0), so everything it writes after
//   that is dropped until it names a source of its own (0x53c756). A CommandSourceTerminated is passed on and then
//   takes the source out of the client's set (0x53c787..0x53c7d0). A client that is ejected has, at its place in the
//   order, a SetCommandSource + CommandSourceTerminated written for each source left in its set (0x53c830..0x53c920).
//   Which armies a source may order is not checked here: the sim does that (Sim::OkayToMessWith 0x747320).
// * The input delay: the original closes the local client's beats on the clock (CSimDriver::ThreadRun 0x73bdf0):
//   beat k is sent once now >= (time of the last sync) + (k - its beat) * msPerBeat - net_Lag, net_Lag a console
//   variable of 500 ms (0xf58134; read only at 0x73c006, not set from the lobby or the ping). At normal speed (100 ms
//   a beat) orders written while the interface shows beat B therefore run before beat B + 6, i.e. inputDelay = 5 in
//   this class's terms (orders queued before Ready(B + 1) run before beat B + 1 + inputDelay). Ours is a fixed number
//   of beats, chosen at the launch, so that it does not depend on the clock.
// * The checksum: every sim_ChecksumPeriod (50, 0xbdbc33) beats each client puts VerifyChecksum(its digest, beat) in
//   its own stream (FinalizeSyncDispatchLocked 0x73dc94); every sim compares the clients' digests with its own
//   (Sim::VerifyChecksum 0x7487c0) and keeps the differences (SDesyncInfo, "Checksum for beat %d mismatched"); the
//   game goes on and the interface shows the desync dialog. Ours go through BeatExchange::SendChecksum beside the
//   orders, so a peer's may come before we have run that beat: it waits for ours.
#include "sim/lockstep.h"

#include <algorithm>
#include <cstring>

#include "core/log.h"
#include "sim/fingerprint.h"

namespace moho {
namespace {

constexpr int kNoSource = 0xff;
constexpr uint8_t kAdvance = 0, kSetCommandSource = 1, kCommandSourceTerminated = 2;
constexpr uint32_t kKeepBeats = 128;  // (the original keeps 128 beats of digests: sim + 0xb0, "we only have data for")

void Frame(std::string* out, uint8_t type, const std::string& payload) {
  const size_t len = payload.size() + 3;
  out->push_back(static_cast<char>(type));
  out->push_back(static_cast<char>(len & 0xff));
  out->push_back(static_cast<char>((len >> 8) & 0xff));
  out->append(payload);
}

std::string SourceMessage(int source) {
  std::string m;
  Frame(&m, kSetCommandSource, std::string(1, static_cast<char>(source)));
  return m;
}

std::string Hex(const std::string& digest) {
  static const char* const kDigits = "0123456789abcdef";
  std::string s;
  for (unsigned char c : digest) {
    s.push_back(kDigits[c >> 4]);
    s.push_back(kDigits[c & 15]);
  }
  return s;
}

}  // namespace

Lockstep::Lockstep(BeatExchange* exchange, uint8_t local, std::vector<LockstepClient> clients, uint32_t inputDelay)
    : ex_(exchange), local_(local), clients_(std::move(clients)), delay_(inputDelay) {
  std::sort(clients_.begin(), clients_.end(),
            [](const LockstepClient& a, const LockstepClient& b) { return a.client < b.client; });
  current_.assign(clients_.size(), kNoSource);
  gone_.assign(clients_.size(), false);
}

void Lockstep::Start() {
  // (the original: nothing is closed before the first sync, then beats 1 .. 1 + net_Lag / msPerBeat at once, empty)
  for (uint32_t b = 1; b <= delay_; ++b) ex_->SendLocal(b, std::string());
}

void Lockstep::Queue(const std::string& orders) { queued_ += orders; }

bool Lockstep::Ready(std::string* merged) {
  if (!sentForNext_) {
    ex_->SendLocal(next_ + delay_, queued_);
    queued_.clear();
    sentForNext_ = true;
  }
  std::vector<ClientBatch> batches;
  if (!ex_->Collect(next_, &batches)) return false;
  merged->clear();
  size_t at = 0;  // (batches come sorted by client, as clients_ is)
  for (size_t i = 0; i < clients_.size(); ++i) {
    LockstepClient& c = clients_[i];
    while (at < batches.size() && batches[at].client < c.client) {
      Logf(LogLevel::Warning, "lockstep: beat %u: a batch from client %d, who is not in the game, dropped", next_,
           batches[at].client);
      ++rejected_;
      ++at;
    }
    const ClientBatch* batch = at < batches.size() && batches[at].client == c.client ? &batches[at++] : nullptr;
    if (gone_[i]) continue;
    if (!batch) {
      // left out from this beat on (ejected): its sources terminated, at its place in the order (0x53c830)
      for (int s : c.sources) {
        *merged += SourceMessage(s);
        Frame(merged, kCommandSourceTerminated, std::string());
      }
      Logf(LogLevel::Info, "lockstep: beat %u: client %d left out, its %zu command source(s) terminated", next_,
           c.client, c.sources.size());
      c.sources.clear();
      current_[i] = kNoSource;
      gone_[i] = true;
      continue;
    }
    if (current_[i] != kNoSource) *merged += SourceMessage(current_[i]);  // 0x53c5aa
    const std::string& d = batch->bytes;
    size_t p = 0;
    while (p < d.size()) {
      if (p + 3 > d.size()) {
        Logf(LogLevel::Warning, "lockstep: beat %u: client %d's batch ends inside a message header; the rest dropped",
             next_, c.client);
        ++rejected_;
        break;
      }
      const uint8_t type = static_cast<uint8_t>(d[p]);
      size_t len = static_cast<uint8_t>(d[p + 1]) | (static_cast<size_t>(static_cast<uint8_t>(d[p + 2])) << 8);
      if (len < 3) len = 3;  // (the decoder's rule, 0x6e417a)
      if (p + len > d.size()) {
        Logf(LogLevel::Warning, "lockstep: beat %u: client %d's message at byte %zu runs past its batch; the rest "
             "dropped", next_, c.client, p);
        ++rejected_;
        break;
      }
      const size_t start = p;
      p += len;
      if (type == kAdvance) continue;  // (the beat's own: the transport counts them; the sim gets none from here)
      if (type == kSetCommandSource) {
        const int s = len > 3 ? static_cast<uint8_t>(d[start + 3]) : kNoSource;
        if (std::find(c.sources.begin(), c.sources.end(), s) != c.sources.end()) {
          current_[i] = s;
          merged->append(d, start, len);
        } else {
          Logf(LogLevel::Warning, "lockstep: beat %u: Client %d claiming command source %d, but not authorized for it.",
               next_, c.client, s);
          current_[i] = kNoSource;
          ++rejected_;
        }
        continue;
      }
      if (current_[i] == kNoSource) {  // (0x53c756: no source, the message goes nowhere)
        ++rejected_;
        continue;
      }
      merged->append(d, start, len);
      if (type == kCommandSourceTerminated) {
        c.sources.erase(std::remove(c.sources.begin(), c.sources.end(), current_[i]), c.sources.end());
        current_[i] = kNoSource;
      }
    }
  }
  for (; at < batches.size(); ++at) {
    Logf(LogLevel::Warning, "lockstep: beat %u: a batch from client %d, who is not in the game, dropped", next_,
         batches[at].client);
    ++rejected_;
  }
  ++next_;
  sentForNext_ = false;
  return true;
}

void Lockstep::AfterBeat(const std::string& digest) {
  const uint32_t beat = next_ - 1;  // (Ready moved on when it released the beat)
  if (beat > 0 && beat % kLockstepChecksumPeriod == 0) {
    if (digest.size() == 16) {
      BeatChecksum mine;
      mine.beat = beat;
      std::memcpy(mine.digest, digest.data(), 16);
      ex_->SendChecksum(mine);
      mine_.emplace_back(beat, digest);
    } else {
      Logf(LogLevel::Warning, "lockstep: beat %u: no digest (%zu bytes) to send", beat, digest.size());
    }
  }
  std::vector<std::pair<uint8_t, BeatChecksum>> got;
  ex_->TakePeerChecksums(&got);
  early_.insert(early_.end(), got.begin(), got.end());
  std::vector<std::pair<uint8_t, BeatChecksum>> later;
  for (const auto& [client, theirs] : early_) {
    if (theirs.beat > beat) {  // not run here yet
      later.emplace_back(client, theirs);
      continue;
    }
    auto it = std::find_if(mine_.begin(), mine_.end(), [&](const auto& m) { return m.first == theirs.beat; });
    if (it == mine_.end()) {
      Logf(LogLevel::Warning, "lockstep: ignoring client %d's checksum of beat %u (no digest of ours: we keep %u beats, "
           "every %u-th)", client, theirs.beat, kKeepBeats, kLockstepChecksumPeriod);
      continue;
    }
    const std::string t(reinterpret_cast<const char*>(theirs.digest), 16);
    if (t != it->second) {
      Logf(LogLevel::Warning, "lockstep: Checksum for beat %u mismatched: %s (sim) != %s (client %d).", theirs.beat,
           Hex(it->second).c_str(), Hex(t).c_str(), client);
      desyncs_.push_back(LockstepDesync{theirs.beat, client});
    } else {
      Logf(LogLevel::Info, "lockstep: beat %u: client %d's checksum is the same as ours", theirs.beat, client);
    }
  }
  early_ = std::move(later);
  // (ours are kept 128 beats, as the original's; a peer is at most the input delay and its network behind)
  mine_.erase(std::remove_if(mine_.begin(), mine_.end(), [&](const auto& m) { return m.first + kKeepBeats < beat; }),
              mine_.end());
}

std::string LockstepDigest(const Sim& sim, uint32_t beat) {
  std::string line = Fingerprint::StateLine(sim, beat);
  // the lua part (heap sizes, serials) differs between clients that look through different armies: not in it, as it
  // is not in the fingerprint's total or its peer check
  if (size_t lua = line.rfind(" lua="); lua != std::string::npos) line.resize(lua);
  // two 64-bit hashes of the line's bytes (FNV-1a, and a multiply-xorshift with another start): our own, not MD5
  uint64_t a = 0xcbf29ce484222325ull, b = 0x84222325cbf29ce4ull;
  for (unsigned char ch : line) {
    a = (a ^ ch) * 0x100000001b3ull;
    b = (b ^ ch) * 0x9e3779b97f4a7c15ull;
    b ^= b >> 31;
  }
  b ^= line.size();
  std::string out(16, '\0');
  for (int i = 0; i < 8; ++i) {
    out[i] = static_cast<char>(a >> (8 * i));
    out[8 + i] = static_cast<char>(b >> (8 * i));
  }
  return out;
}

}  // namespace moho
