// Script threads: Lua coroutines run by the engine's tick loop, as ForkThread/WaitTicks see
// them. A thread runs until it yields; `coroutine.yield(n)` (WaitTicks) sleeps n ticks.
#pragma once
#include <cstdint>
#include <list>
#include <string>

#include "script/lua.hpp"

namespace moho {

class ThreadScheduler {
 public:
  explicit ThreadScheduler(lua_State* L) : L_(L) {}
  ~ThreadScheduler();

  // Create a thread for the function at `caller` stack index `fn` with `nargs` arguments
  // after it. Pushes the new coroutine on `caller`. It first runs on the next RunTick(), or later
  // in the current one when a running thread forks it.
  void Fork(lua_State* caller, int fn, int nargs);

  // Run every thread that is due at tick `tick`, including threads forked while running.
  void RunTick(uint32_t tick);

  bool Kill(lua_State* co);              // returns true if co was a scheduler thread
  bool Resume(lua_State* co);            // wake a suspended thread
  void SuspendCurrent() { if (current_) current_->suspended = true; }
  // WaitFor: the running thread waits until the event (registry ref) is signalled.
  void WaitCurrentOn(int ref);
  void PushCurrent(lua_State* L);        // the running thread object, or nil
  lua_State* Current() const { return current_ ? current_->co : nullptr; }
  size_t Count() const { return threads_.size(); }

  static ThreadScheduler* From(lua_State* L);

 private:
  struct Thread {
    lua_State* co = nullptr;
    int ref = LUA_NOREF;
    int pendingArgs = 0;   // arguments waiting for the first resume
    uint32_t wakeTick = 0;
    bool started = false;
    bool suspended = false;
    bool dead = false;
    bool again = false;       // yielded 0: run again in this dispatch
    int waitRef = LUA_NOREF;  // WaitFor(event)
  };
  lua_State* L_;
  std::list<Thread> threads_;
  Thread* current_ = nullptr;
  uint32_t tick_ = 0;

  void Step(Thread& t);
  bool StillWaiting(Thread& t);
  void Release(Thread& t);
};

void RegisterThreadBindings(lua_State* L, ThreadScheduler* sched);

}  // namespace moho
