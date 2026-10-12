// Orders into a running simulation: the messages of the command stream (engine-ref command_stream.md), written
// the way the original's client writes them. A host that plays the game collects the player's orders of one beat
// here and applies them before the next beat (ApplyCommands); the bytes are exactly what a replay's stream
// holds, so the same code carries out a replayed and a played order, and a played game can be saved as a replay
// by writing these bytes (with an Advance per beat) after the session's header.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "sim/commands.h"

namespace moho {

class Sim;

class CommandWriter {
 public:
  // source: the command source the host speaks for (the index into the session's sources that the player's
  // army lists: SessionDescription.header.armies[focusArmy].source). Every batch starts by naming it.
  explicit CommandWriter(int source) : source_(source) {}

  // A fresh command id for IssueCommand: the source in the top byte, a running number below (Sim::
  // ValidateNewCommandId 0x7491c0 refuses ids of another source).
  uint32_t NewCommandId();

  void RequestPause();
  void Resume();
  void SingleStep();
  void CreateUnit(int army0, const std::string& blueprint, float x, float z, float heading);  // cheats
  void CreateProp(const std::string& blueprint, float x, float y, float z);                   // cheats
  void DestroyEntity(uint32_t entity);
  // key / value as the interface sends them: ToggleScriptBit "<n>", SetFireState "<n>", SetAutoMode /
  // SetAutoSurfaceMode / SetRepeatQueue / SetPaused "true" | "false", CustomName "<text>".
  void ProcessInfoPair(uint32_t entity, const std::string& key, const std::string& value);
  // The order `data` for `units` (entity ids); clear empties their queues first (no shift key). data.id is
  // given a fresh id when left at 0xffffffff. Returns the command's id (for the calls below).
  uint32_t IssueCommand(const std::vector<uint32_t>& units, StreamIssueData data, bool clear);
  void IncreaseCommandCount(uint32_t command, int32_t delta);
  void DecreaseCommandCount(uint32_t command, int32_t delta);
  void SetCommandTarget(uint32_t command, const StreamTarget& target);
  void SetCommandType(uint32_t command, int type);
  void SetCommandCells(uint32_t command, const std::vector<std::pair<int16_t, int16_t>>& cells, float x, float y,
                       float z);
  void RemoveCommandFromQueue(uint32_t command, uint32_t unit);
  // luaValue: one Lua value in the engine's byte form (SerializeLua); empty = nil.
  void ExecuteLuaInSim(const std::string& function, const std::string& luaValue);  // cheats
  // SimCallback: /lua/SimCallbacks.lua's DoCallback(name, data, units or nil).
  void LuaSimCallback(const std::string& name, const std::string& luaValue, const std::vector<uint32_t>& units);
  void EndGame();

  bool empty() const { return body_.empty(); }
  // The batch so far, led by SetCommandSource(source) (empty if nothing was written), and start a new one.
  std::string Take();

  // For a stream that is kept (a replay's body): the Advance that separates two beats' messages.
  static std::string AdvanceMessage(uint32_t beats = 1);

 private:
  void Message(int type, const std::string& payload);
  int source_;
  uint32_t nextId_ = 1;
  std::string body_;
};

// Carry out every message of `bytes` now, in order (a host: between two beats). Advance messages are skipped.
// unhandled (optional) counts the kinds of message the engine reads but does not carry out yet.
// Returns false if a message was malformed.
bool ApplyCommands(Sim& sim, const std::string& bytes, std::map<std::string, int>* unhandled = nullptr);

}  // namespace moho
