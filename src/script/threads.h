// Script threads: Lua coroutines run by the engine's tick loop, as ForkThread/WaitTicks see
// them. A thread runs until it yields; `coroutine.yield(n)` (WaitTicks) sleeps n ticks.
#pragma once
#include <cstdint>
#include <list>
#include <string>
#include <vector>

#include "script/lua.hpp"

namespace moho {

class ScriptObject;

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
  // The threads whose event has been signalled since the last call go to the END of the run order, where the
  // original's wake links them (0x409420). Called between the beat's stages, when a pass begins and before a
  // new thread joins the list (Fork).
  void WakeSignalled();
  // CTaskEvent::EventSetSignaled 0x406d90: event `o` has just been signalled; the threads waiting on it go to the
  // end of the run order now (see threads.cpp).
  void Signal(const ScriptObject* o);
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
    uint32_t waitSeq = 0;     // when it began to wait (Signal: the last to begin goes first)
    bool inWaiters = false;   // listed in waiters_
  };
  lua_State* L_;
  std::list<Thread> threads_;
  // the threads that have had a WaitFor event since WakeSignalled last looked (a few at a time, against thousands of
  // threads): WakeSignalled, now called at every Fork, walks the whole list only when one of them is signalled
  std::vector<Thread*> waiters_;
  Thread* current_ = nullptr;
  uint32_t tick_ = 0;
  uint32_t waitCount_ = 0;

  void Step(Thread& t);
  bool StillWaiting(Thread& t);
  void Release(Thread& t);
};

void RegisterThreadBindings(lua_State* L, ThreadScheduler* sched);

}  // namespace moho
