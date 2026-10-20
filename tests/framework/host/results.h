#pragma once
#include <fstream>
#include <map>
#include <mutex>
#include <string>

#include "script/threads.h"

namespace moho::ui::testing {

class Results {
 public:
  Results(const std::string& path, std::string run, std::string name, std::string kind);
  void Register(lua_State* L);
  void Poll(lua_State* L);
  void CloseState(lua_State* L);
  void Error(const std::string& message);
  int Exit(int returncode);
  bool Failed();
  bool ReadyForGame();

 private:
  struct Stage { std::string status; bool terminal = true, ended = false; };
  struct Suite { int stage; int checks = 0; bool ended = false; };
  struct Thread { int id, stage; bool root = false; };
  struct State { int stage = 0, abort = LUA_NOREF; };
  std::ofstream file_;
  std::string run_, name_, kind_;
  std::mutex mutex_;
  std::map<lua_State*, State> states_;
  std::map<int, Stage> stages_;
  std::map<int, Suite> suites_;
  std::map<lua_State*, Thread> threads_;
  int sequence_ = 0, nextStage_ = 0, nextSuite_ = 0, nextThread_ = 0;
  bool failed_ = false, complete_ = false, skipped_ = false, prepared_ = false;

  void Emit(const std::string& event, int stage, const std::string& data);
  int Begin(lua_State* L, const std::string& name, bool terminal);
  void End(lua_State* L);
  int StartSuite(lua_State* L, const std::string& name);
  void Check(int suite, const std::string& label, bool ok, bool cleanup, const std::string& reason);
  void Finish(int suite, const std::string& status, const std::string& reason);
  void Checkpoint(int suite);
  void Track(lua_State* L, lua_State* thread, bool root = false);
  int Pending(lua_State* L);
  void Released(lua_State* thread, ThreadScheduler::Outcome outcome);
  void Abort(lua_State* L, const char* mode);
  void Advance();

  static Results& From(lua_State* L);
  static int BeginBinding(lua_State* L);
  static int EndBinding(lua_State* L);
  static int SuiteBinding(lua_State* L);
  static int CheckBinding(lua_State* L);
  static int FinishBinding(lua_State* L);
  static int CheckpointBinding(lua_State* L);
  static int TrackBinding(lua_State* L);
  static int PendingBinding(lua_State* L);
  static int AbortBinding(lua_State* L);
  static int FailedBinding(lua_State* L);
  static int StageEndedBinding(lua_State* L);
  static int PreparedBinding(lua_State* L);
};
}
