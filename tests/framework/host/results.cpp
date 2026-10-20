#include "tests/framework/host/results.h"
#include <cmath>
#include <stdexcept>
#include <utility>

#include "script/script_state.h"

namespace moho::ui::testing {
namespace {
std::string Quote(const std::string& text) {
  const char* hex = "0123456789abcdef";
  std::string out = "\"";
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
    else out += c;
  }
  return out + '"';
}
const char* Bool(bool value) { return value ? "true" : "false"; }
bool Boolean(lua_State* L, int index) {
  luaL_checktype(L, index, LUA_TBOOLEAN);
  return lua_toboolean(L, index) != 0;
}
int Id(lua_State* L, int index) {
  luaL_checktype(L, index, LUA_TNUMBER);
  double n = luaL_checknumber(L, index);
  if (!std::isfinite(n) || n < 1 || n > 1000000 || n != std::floor(n)) luaL_error(L, "invalid result identifier");
  return static_cast<int>(n);
}
lua_State* Root(lua_State* L) { return ScriptState::From(L)->L(); }
}

Results::Results(const std::string& path, std::string run, std::string name, std::string kind)
    : run_(std::move(run)), name_(std::move(name)), kind_(std::move(kind)) {
  if (!path.empty()) {
    file_.open(path, std::ios::out | std::ios::trunc);
    if (!file_) throw std::runtime_error("cannot open structured test results: " + path);
  }
  Emit("host_start", 0, "{\"kind\":" + Quote(kind_) + "}");
}
void Results::Emit(const std::string& event, int stage, const std::string& data) {
  ++sequence_;
  if (file_.is_open()) {
    file_ << "{\"version\":1,\"run\":" << Quote(run_) << ",\"case\":" << Quote(name_)
          << ",\"sequence\":" << sequence_ << ",\"event\":" << Quote(event)
          << ",\"stage\":" << stage << ",\"data\":" << data << "}\n";
    file_.flush();
    if (!file_) failed_ = true;
  }
}
void Results::Error(const std::string& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  failed_ = true;
  Emit("error", 0, "{\"message\":" + Quote(message) + "}");
}
bool Results::Failed() {
  std::lock_guard<std::mutex> lock(mutex_);
  return failed_;
}
bool Results::ReadyForGame() {
  std::lock_guard<std::mutex> lock(mutex_);
  return prepared_;
}
void Results::Track(lua_State* L, lua_State* thread, bool root) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& state = states_.at(Root(L));
  if (!state.stage || threads_.count(thread)) return;
  Thread t{++nextThread_, state.stage, root};
  threads_.emplace(thread, t);
  Emit("thread_start", t.stage, "{\"thread\":" + std::to_string(t.id) + "}");
}
int Results::Begin(lua_State* L, const std::string& name, bool terminal) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& state = states_.at(Root(L));
    if (complete_ || (state.stage && !stages_.at(state.stage).ended))
      throw std::runtime_error("stage started before the previous stage ended");
    state.stage = ++nextStage_;
    stages_.emplace(state.stage, Stage{"", terminal, false});
    Emit("stage_start", state.stage, "{\"name\":" + Quote(name) + ",\"terminal\":" + Bool(terminal) + "}");
  }
  if (auto* scheduler = ThreadScheduler::From(L); scheduler && scheduler->Current() == L) Track(L, L, true);
  return states_.at(Root(L)).stage;
}
void Results::End(lua_State* L) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& stage = stages_.at(states_.at(Root(L)).stage);
  if (stage.terminal) throw std::runtime_error("terminal stages require suite completion");
  stage.status = "completed";
}
int Results::StartSuite(lua_State* L, const std::string& name) {
  if (!states_.at(Root(L)).stage) Begin(L, "execute", true);
  std::lock_guard<std::mutex> lock(mutex_);
  int stage = states_.at(Root(L)).stage;
  if (stages_.at(stage).ended) throw std::runtime_error("suite started after stage completion");
  int id = ++nextSuite_;
  suites_.emplace(id, Suite{stage});
  Emit("suite_start", stage, "{\"suite\":" + std::to_string(id) + ",\"name\":" + Quote(name) + "}");
  return id;
}
void Results::Check(int id, const std::string& label, bool ok, bool cleanup, const std::string& reason) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& suite = suites_.at(id);
  if (suite.ended) throw std::runtime_error("assertion after suite completion");
  if (!cleanup) ++suite.checks;
  failed_ = failed_ || !ok;
  std::string data = "{\"suite\":" + std::to_string(id) + ",\"label\":" + Quote(label) + ",\"ok\":" + Bool(ok);
  if (cleanup) data += ",\"reason\":" + Quote(reason);
  Emit(cleanup ? "cleanup" : "assertion", suite.stage, data + "}");
}
void Results::Finish(int id, const std::string& status, const std::string& reason) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& suite = suites_.at(id);
  if (suite.ended || (status != "passed" && status != "failed" && status != "skipped"))
    throw std::runtime_error("invalid suite completion");
  if (!suite.checks && status == "passed") throw std::runtime_error("suite executed no checks");
  suite.ended = true;
  failed_ = failed_ || status == "failed";
  skipped_ = skipped_ || status == "skipped";
  Emit("suite_end", suite.stage, "{\"suite\":" + std::to_string(id) + ",\"status\":" + Quote(status) + ",\"reason\":" + Quote(reason) + "}");
  auto& stage = stages_.at(suite.stage);
  if (stage.terminal || skipped_) stage.status = status;
}
void Results::Checkpoint(int id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& suite = suites_.at(id);
  if (suite.ended) throw std::runtime_error("checkpoint after suite completion");
  suite.ended = true;
  auto& stage = stages_.at(suite.stage);
  stage.status = stage.terminal ? "checkpoint" : "completed";
  Emit("suite_checkpoint", suite.stage, "{\"suite\":" + std::to_string(id) + "}");
}
int Results::Pending(lua_State* L) {
  std::lock_guard<std::mutex> lock(mutex_);
  int stage = states_.at(Root(L)).stage, count = 0;
  for (const auto& [co, t] : threads_) if (t.stage == stage && co != L && !t.root) ++count;
  return count;
}
void Results::Released(lua_State* co, ThreadScheduler::Outcome outcome) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = threads_.find(co);
  if (found == threads_.end()) return;
  const auto t = found->second;
  threads_.erase(found);
  const char* status = outcome == ThreadScheduler::Outcome::Completed ? "completed" :
                       outcome == ThreadScheduler::Outcome::Failed ? "failed" : "cancelled";
  if (outcome != ThreadScheduler::Outcome::Completed) {
    failed_ = true;
    Emit("error", t.stage, "{\"message\":\"test thread did not complete\"}");
  }
  Emit("thread_end", t.stage, "{\"thread\":" + std::to_string(t.id) + ",\"status\":" + Quote(status) + "}");
  Advance();
}
void Results::Advance() {
  for (auto& [id, stage] : stages_) {
    if (stage.ended || stage.status.empty()) continue;
    bool pending = false;
    for (const auto& [co, thread] : threads_) if (thread.stage == id) pending = true;
    for (const auto& [sid, suite] : suites_) if (suite.stage == id && !suite.ended) pending = true;
    if (pending) continue;
    stage.ended = true;
    Emit("stage_end", id, "{\"status\":" + Quote(stage.status == "checkpoint" ? "checkpoint" : "completed") + "}");
    if ((stage.terminal && stage.status != "checkpoint") || skipped_) {
      if (complete_) { failed_ = true; Emit("error", id, "{\"message\":\"duplicate case completion\"}"); }
      else {
        complete_ = true;
        Emit("case_complete", id, "{\"status\":" + Quote(failed_ ? "failed" : skipped_ ? "skipped" : "passed") + "}");
      }
    }
  }
}
void Results::Abort(lua_State* L, const char* mode) {
  auto& state = states_.at(L);
  if (state.abort == LUA_NOREF) return;
  int top = lua_gettop(L);
  lua_pushcfunction(L, ScriptTraceback);
  lua_rawgeti(L, LUA_REGISTRYINDEX, state.abort);
  lua_pushstring(L, mode);
  if (lua_pcall(L, 1, 0, top + 1)) LogScriptError(lua_tostring(L, -1) ? lua_tostring(L, -1) : "cleanup failed");
  lua_settop(L, top);
}
void Results::Poll(lua_State* L) {
  bool failed = Failed();
  if (failed || skipped_) {
    if (failed) Abort(L, "failure");
    int top = lua_gettop(L);
    lua_getglobal(L, "ExitApplication");
    if (lua_isfunction(L, -1)) lua_pcall(L, 0, 0, 0);
    lua_settop(L, top);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  Advance();
}
void Results::CloseState(lua_State* L) {
  Abort(L, "close");
  auto& state = states_.at(L);
  if (state.abort != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, state.abort);
  state.abort = LUA_NOREF;
}
int Results::Exit(int returncode) {
  std::lock_guard<std::mutex> lock(mutex_);
  Advance();
  if (kind_ == "lua" && !complete_) {
    failed_ = true;
    Emit("error", 0, "{\"message\":\"test exited without case completion\"}");
  }
  int status = returncode ? returncode : failed_ ? 1 : 0;
  Emit("host_exit", 0, "{\"returncode\":" + std::to_string(status) + ",\"pending\":" + std::to_string(threads_.size()) + "}");
  return status;
}
Results& Results::From(lua_State* L) { return *static_cast<Results*>(lua_touserdata(L, lua_upvalueindex(1))); }
int Results::BeginBinding(lua_State* L) {
  try { lua_pushnumber(L, From(L).Begin(L, luaL_checkstring(L, 1), Boolean(L, 2))); return 1; }
  catch (const std::exception& e) { From(L).Error(e.what()); return luaL_error(L, "%s", e.what()); }
}
int Results::EndBinding(lua_State* L) {
  try { From(L).End(L); return 0; } catch (const std::exception& e) { From(L).Error(e.what()); return luaL_error(L, "%s", e.what()); }
}
int Results::SuiteBinding(lua_State* L) {
  try { lua_pushnumber(L, From(L).StartSuite(L, luaL_checkstring(L, 1))); return 1; }
  catch (const std::exception& e) { From(L).Error(e.what()); return luaL_error(L, "%s", e.what()); }
}
int Results::CheckBinding(lua_State* L) {
  try { From(L).Check(Id(L, 1), luaL_checkstring(L, 2), Boolean(L, 3), Boolean(L, 4), luaL_optstring(L, 5, "")); return 0; }
  catch (const std::exception& e) { From(L).Error(e.what()); return luaL_error(L, "%s", e.what()); }
}
int Results::FinishBinding(lua_State* L) {
  try { From(L).Finish(Id(L, 1), luaL_checkstring(L, 2), luaL_optstring(L, 3, "")); return 0; }
  catch (const std::exception& e) { From(L).Error(e.what()); return luaL_error(L, "%s", e.what()); }
}
int Results::CheckpointBinding(lua_State* L) {
  try { From(L).Checkpoint(Id(L, 1)); return 0; }
  catch (const std::exception& e) { From(L).Error(e.what()); return luaL_error(L, "%s", e.what()); }
}
int Results::TrackBinding(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTHREAD);
  From(L).Track(L, lua_tothread(L, 1)); return 0;
}
int Results::PendingBinding(lua_State* L) { lua_pushnumber(L, From(L).Pending(L)); return 1; }
int Results::FailedBinding(lua_State* L) { lua_pushboolean(L, From(L).Failed()); return 1; }
int Results::StageEndedBinding(lua_State* L) {
  int id = Id(L, 1);
  auto& record = From(L);
  std::lock_guard<std::mutex> lock(record.mutex_);
  auto found = record.stages_.find(id);
  lua_pushboolean(L, found != record.stages_.end() && found->second.ended); return 1;
}
int Results::AbortBinding(lua_State* L) {
  luaL_checktype(L, 1, LUA_TFUNCTION);
  auto& state = From(L).states_.at(Root(L));
  if (state.abort != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, state.abort);
  lua_pushvalue(L, 1);
  state.abort = luaL_ref(L, LUA_REGISTRYINDEX); return 0;
}
int Results::PreparedBinding(lua_State* L) {
  auto& record = From(L);
  std::lock_guard<std::mutex> lock(record.mutex_);
  record.prepared_ = true;
  return 0;
}
void Results::Register(lua_State* L) {
  states_[L] = State{};
  ThreadScheduler::From(L)->onRelease = [this](lua_State* co, ThreadScheduler::Outcome outcome) { Released(co, outcome); };
  lua_getglobal(L, "moho64ui_TestHost");
  struct Binding { const char* name; lua_CFunction fn; };
  for (const auto& binding : {Binding{"begin_stage", BeginBinding}, {"end_stage", EndBinding}, {"stage_ended", StageEndedBinding},
                             {"suite", SuiteBinding}, {"check", CheckBinding}, {"finish", FinishBinding},
                             {"checkpoint", CheckpointBinding}, {"track", TrackBinding}, {"pending", PendingBinding},
                             {"on_abort", AbortBinding}, {"failed", FailedBinding}, {"prepared", PreparedBinding}}) {
    lua_pushstring(L, binding.name); lua_pushlightuserdata(L, this); lua_pushcclosure(L, binding.fn, 1); lua_rawset(L, -3);
  }
  lua_pop(L, 1);
}
}
