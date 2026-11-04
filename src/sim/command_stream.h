// The replay command stream (engine-ref command_stream.md): messages {u8 type, u16 length (incl. the header),
// payload}. Messages between two Advances are applied, in order, just before the next beat; a paused beat still
// consumes its Advance.
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace moho {

class Sim;

class CommandStream {
 public:
  CommandStream(const std::string& data, size_t offset) : d_(data), p_(offset) {}
  // Apply messages until the next Advance (true: a beat is due) or the end of the stream (false).
  bool ApplyUntilAdvance(Sim& sim);
  bool AtEnd() const { return p_ >= d_.size(); }
  uint32_t beats() const { return beats_; }        // Advances read so far
  uint32_t messages() const { return messages_; }
  bool hadError() const { return error_; }
  const std::map<std::string, int>& unhandled() const { return unhandled_; }

  uint32_t dropped() const { return dropped_; }   // messages the replay client did not pass on (ReplayClientPasses)

 private:
  void Apply(Sim& sim, int type, const std::string& payload);
  bool ReplayClientPasses(Sim& sim, int type, const std::string& payload);
  const std::string& d_;
  size_t p_;
  uint32_t beats_ = 0;
  uint32_t messages_ = 0;
  uint32_t dropped_ = 0;
  // A replay is played by the original's CReplayClient (0x53ba50), whose messages go through CClientBase::UpdateState
  // 0x53c550 like any client's: its command source (client+0x50, 0xff at the start: the ctor's argument, 0x53ba74),
  // the sources it may still name (client+0x30: the header's, less every one that was terminated), and the beat's
  // pass-on flag (UpdateState's local at esp+0x16)
  int clientSource_ = 0xff;
  bool forward_ = false;
  std::vector<bool> clientGone_;  // per header source: terminated
  bool error_ = false;
  std::map<std::string, int> unhandled_;  // message kinds we read but do not carry out (logged at the end)
};

}  // namespace moho
