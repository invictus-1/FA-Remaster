#include "script/threads.h"

#include <algorithm>
#include <cctype>
#include <string>

#include "core/log.h"
#include "core/profile.h"
#include "script/script_state.h"
#include "script/script_handle.h"

extern "C" {
extern void (*lua_gpg_resume_error_hook)(lua_State* L, int status);
}

namespace moho {
namespace {

const char kSchedKey = 0;
// traceback captured when a coroutine fails; one per OS thread, since the sim's and the interface's schedulers
// may run on different threads (an interface host may tick the sim on a worker)
thread_local std::string g_threadError;

// Build "message\nstack traceback:\n\t..." from the failing coroutine before it unwinds.
void OnResumeError(lua_State* co, int) {
  const char* msg = lua_isstring(co, -1) ? lua_tostring(co, -1) : "(error object is not a string)";
  g_threadError = msg;
  g_threadError += "\nstack traceback:";
  lua_Debug ar;
  for (int level = 0; lua_getstack(co, level, &ar); ++level) {
    lua_getinfo(co, "Snl", &ar);
    g_threadError += "\n        ";
    g_threadError += ar.short_src;
    if (ar.currentline > 0) g_threadError += "(" + std::to_string(ar.currentline) + ")";
    g_threadError += ": in ";
    if (ar.name) g_threadError += std::string("function `") + ar.name + "'";
    else if (*ar.what == 'm') g_threadError += "main chunk";
    else if (*ar.what == 'C') g_threadError += "?";
    else g_threadError += "function <" + std::string(ar.short_src) + ":" + std::to_string(ar.linedefined) + ">";
  }
}

int l_ForkThread(lua_State* L) {
  ThreadScheduler* s = ThreadScheduler::From(L);
  if (!s) return luaL_error(L, "ForkThread: no thread scheduler in this state");
  luaL_checktype(L, 1, LUA_TFUNCTION);
  s->Fork(L, 1, lua_gettop(L) - 1);
  return 1;
}

int l_KillThread(lua_State* L) {
  ThreadScheduler* s = ThreadScheduler::From(L);
  lua_State* co = lua_tothread(L, 1);
  if (!s || !co) return 0;
  // exe 0x4ca060: the thread's tasks are dropped (0x409190) and the call returns. A thread that kills itself
  // (a trash bag holding its own thread, destroyed from inside it) keeps running until it next yields, and is
  // never resumed: the Monkeylord's ambient-effect thread still draws its random wait after its own clean-up.
  s->Kill(co);
  return 0;
}

int l_CurrentThread(lua_State* L) {
  ThreadScheduler* s = ThreadScheduler::From(L);
  lua_State* co = s ? s->Current() : nullptr;
  if (!co || co != L) {
    lua_pushnil(L);
    return 1;
  }
  s->PushCurrent(L);
  return 1;
}

int l_SuspendCurrentThread(lua_State* L) {
  ThreadScheduler* s = ThreadScheduler::From(L);
  if (!s || s->Current() != L) return luaL_error(L, "SuspendCurrentThread: not called from a thread");
  s->SuspendCurrent();
  return lua_yield(L, 0);
}

int l_ResumeThread(lua_State* L) {
  ThreadScheduler* s = ThreadScheduler::From(L);
  lua_State* co = lua_tothread(L, 1);
  if (s && co) s->Resume(co);
  return 0;
}

// WaitFor(event): wait until an engine event (economy event, ...) is signalled. Objects that are
// not events yet (manipulators, effects: no animation in the headless sim) end the wait at the
// next tick.
int l_WaitFor(lua_State* L) {
  ThreadScheduler* s = ThreadScheduler::From(L);
  if (!s || s->Current() != L) return luaL_error(L, "WaitFor: not called from a thread");
  ScriptObject* o = GetObject(L, 1);
  if (o && o->EventState() == 1) return 0;
  if (o && o->EventState() == 0) {
    lua_pushvalue(L, 1);
    s->WaitCurrentOn(luaL_ref(L, LUA_REGISTRYINDEX));
    lua_settop(L, 0);
    lua_pushnumber(L, 2);
    return lua_yield(L, 1);
  }
  lua_settop(L, 0);
  lua_pushnumber(L, 2);  // = WaitTicks(2): resume next tick
  return lua_yield(L, 1);
}

}  // namespace

ThreadScheduler::~ThreadScheduler() {
  for (auto& t : threads_) Release(t);
}

ThreadScheduler* ThreadScheduler::From(lua_State* L) {
  lua_pushlightuserdata(L, const_cast<char*>(&kSchedKey));
  lua_rawget(L, LUA_REGISTRYINDEX);
  auto* s = static_cast<ThreadScheduler*>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  return s;
}

void ThreadScheduler::Fork(lua_State* caller, int fn, int nargs) {
  int profSource = -1;
  std::string profLabel;
  if (prof::On()) {  // MOHO64_PROFILE: the thread's kind for the Lua timer, from its function's chunk name
    lua_Debug ar;
    lua_pushvalue(caller, fn);
    if (lua_getinfo(caller, ">S", &ar) && ar.source) {
      std::string s = ar.source;
      // (lower case, and '/' for '\\': a chunk read from the host's disk is named by its host path)
      std::transform(s.begin(), s.end(), s.begin(),
                     [](unsigned char c) { return c == '\\' ? '/' : static_cast<char>(std::tolower(c)); });
      auto has = [&](const char* k) { return s.find(k) != std::string::npos; };
      if (prof::Hitch()) profLabel = std::string("thread ") + ar.short_src + ":" + std::to_string(ar.linedefined);
      profSource = has("/ai/") || has("aibrain") || has("platoon") ? prof::kThreadAI
                   : has("unit") || has("weapon") || has("projectile") || has("effect") ? prof::kThreadUnit
                                                                                         : prof::kThreadOther;
    }
  }
  lua_State* co = lua_newthread(caller);  // pushed on caller
  prof::InstallLuaHooks(co);  // (MOHO64_PROFILE_LUASAMPLE / _LUACALLS only: Lua 5.0's threads inherit no hook)
  lua_pushvalue(caller, -1);
  int ref = luaL_ref(caller, LUA_REGISTRYINDEX);
  for (int i = 0; i <= nargs; ++i) {
    lua_pushvalue(caller, fn + i);
    lua_xmove(caller, co, 1);
  }
  Thread t;
  t.co = co;
  t.ref = ref;
  t.pendingArgs = nargs;
  t.profSource = profSource;
  t.profLabel = std::move(profLabel);
  // A new thread runs in the next pass over the threads: a thread forked by a running thread runs
  // later in the same pass, after the threads already due (probe v7: forked at 1603 from a thread,
  // first runs at 1603 after its parent yields; the exe's CTaskThread joins the stage's list
  // ahead of the pass position). Forked between passes (engine callbacks) it runs in the next pass.
  t.wakeTick = current_ ? tick_ : tick_ + 1;
  // ForkThread 0x4c9d80 makes the CTaskThread at once (0x4c9e8f -> 0x409050, linked at the stage's new end,
  // 0x4090b0-0x4090bb), and an event's signal links a thread waiting on it at that same end at the moment of the
  // signal (CTaskEvent::EventSetSignaled 0x406d90, 0x406dfc-0x406e07), e.g. a rotator reaching its target speed in
  // its unit's beat (CRotateManipulator::MoveManipulator 0x643b75). Ours finds the signalled events only when
  // WakeSignalled looks, so the threads whose event has been signalled by now go to the end before the new one
  // does. Seen: Economy 4776, an Aeon mass fabricator's rotator (uab1104_script.lua:87 WaitFor) stopped in the
  // fab's beat, then an Othuy's beam, made later, forked its ScorchThread in its OnImpact; at 4777 the original
  // ran the fab's thread first (its Random(120, 300) before the scorch's Random()), ours after: the fab drew
  // another number for its Goal and its next turn came at 4889 instead of 4885.
  WakeSignalled();
  threads_.push_back(t);
}

void ThreadScheduler::PushCurrent(lua_State* L) {
  if (current_ && current_->ref != LUA_NOREF) lua_rawgeti(L, LUA_REGISTRYINDEX, current_->ref);
  else lua_pushnil(L);
}

void ThreadScheduler::Release(Thread& t) {
  if (t.inWaiters) {
    t.inWaiters = false;
    for (size_t i = 0; i < waiters_.size(); ++i)
      if (waiters_[i] == &t) {
        waiters_[i] = waiters_.back();
        waiters_.pop_back();
        break;
      }
  }
  if (t.ref != LUA_NOREF) luaL_unref(L_, LUA_REGISTRYINDEX, t.ref);
  t.ref = LUA_NOREF;
  if (t.waitRef != LUA_NOREF) luaL_unref(L_, LUA_REGISTRYINDEX, t.waitRef);
  t.waitRef = LUA_NOREF;
}

void ThreadScheduler::WaitCurrentOn(int ref) {
  if (!current_) return;
  if (current_->waitRef != LUA_NOREF) luaL_unref(L_, LUA_REGISTRYINDEX, current_->waitRef);
  current_->waitRef = ref;
  current_->waitSeq = ++waitCount_;
  if (!current_->inWaiters) {
    current_->inWaiters = true;
    waiters_.push_back(current_);
  }
}

bool ThreadScheduler::StillWaiting(Thread& t) {
  if (t.waitRef == LUA_NOREF) return false;
  lua_rawgeti(L_, LUA_REGISTRYINDEX, t.waitRef);
  ScriptObject* o = GetObject(L_, -1);
  lua_pop(L_, 1);
  if (o && o->EventState() == 0) return true;
  luaL_unref(L_, LUA_REGISTRYINDEX, t.waitRef);
  t.waitRef = LUA_NOREF;
  return false;
}

bool ThreadScheduler::Kill(lua_State* co) {
  for (auto& t : threads_)
    if (t.co == co) {
      t.dead = true;
      return true;
    }
  return false;
}

bool ThreadScheduler::Resume(lua_State* co) {
  for (auto& t : threads_)
    if (t.co == co && t.suspended) {
      t.suspended = false;
      t.wakeTick = tick_ + 1;
      return true;
    }
  return false;
}

void ThreadScheduler::Step(Thread& t) {
  // set once for the process (the sim's and the interface's schedulers may run on different threads)
  static const bool hooked = (lua_gpg_resume_error_hook = OnResumeError, true);
  (void)hooked;
  Thread* prev = current_;
  current_ = &t;
  int nargs = t.started ? 0 : t.pendingArgs;
  t.started = true;
  g_threadError.clear();
  if (t.profSource >= 0) prof::NextSource(t.profSource);  // (MOHO64_PROFILE only)
  if (prof::Hitch()) prof::NextLabel(t.profLabel);
  int rc = lua_resume(t.co, nargs);
  current_ = prev;
  if (rc != 0) {
    if (g_threadError.empty())  // an error that did not pass luaG_errormsg (memory, ...)
      g_threadError = lua_isstring(t.co, -1) ? lua_tostring(t.co, -1) : "(error object is not a string)";
    // CLuaTask::Execute 0x4c9700 logs the failed resume with Warnf: "Error running lua script: %s" (0xe09354), or,
    // when the thread was destroyed during this run (the flag the task points at, ebp-0x11), "Error running lua
    // script from destroyed thread: %s" (0xe09320). Step runs only live threads, so t.dead here = killed itself.
    g_threadError.insert(0, t.dead ? "Error running lua script from destroyed thread: " : "Error running lua script: ");
    for (size_t a = 0, b; a <= g_threadError.size(); a = b + 1) {
      b = g_threadError.find('\n', a);
      if (b == std::string::npos) b = g_threadError.size();
      LogWrite(LogLevel::Warning, g_threadError.substr(a, b - a));
    }
    t.dead = true;
    return;
  }
  lua_Debug ar;
  if (lua_getstack(t.co, 0, &ar) == 0) {  // returned: no active frame
    t.dead = true;
    return;
  }
  if (t.dead || t.suspended) {
    lua_settop(t.co, 0);
    return;
  }
  // coroutine.yield(n) / WaitTicks(n): resume n-1 ticks later, at least the next tick (the
  // original: WaitTicks(1) +1, WaitTicks(5) +4, WaitSeconds(1) = WaitTicks(11) +10).
  // yield() without a number suspends the thread for good.
  if (lua_gettop(t.co) < 1 || !lua_isnumber(t.co, 1)) {
    lua_settop(t.co, 0);
    t.suspended = true;
    return;
  }
  double n = lua_tonumber(t.co, 1);
  lua_settop(t.co, 0);
  // yield(0) / WaitTicks(0): the task runs again at once, in the same dispatch. The count is an integer: a
  // fraction below 1 is 0 too (seen: FAF's Projectile.RetargetThread does WaitTicks(0.5) and the original has
  // already set the new lifetime in the Lua pass in which that thread first runs)
  if (static_cast<int>(n) <= 0) {
    t.wakeTick = tick_;
    t.again = true;
    return;
  }
  int wait = static_cast<int>(n) - 1;
  if (wait < 1) wait = 1;
  t.wakeTick = tick_ + static_cast<uint32_t>(wait);
}

// WaitFor in the original (0x4ca910) puts a CWaitForTask on the thread; its Execute 0x4ca660 takes the thread off
// the stage's run list while the event is not signalled (0x406e20 / 0x4093e0: the stage's second list), and the
// event's wake 0x409420 links it in again at the END of the run list. So a thread that really waited runs after
// every thread that was in the list when its event fired, from then on. Seen: a sinking ship's animation thread
// (WaitFor(sinkAnim), then StopSink = true) was made before the unit's DeathThread, whose SeabedWatcher looks at
// StopSink every beat; in the original the watcher runs first in the beat after the animation's end and sees the
// flag one beat later, so the wreck is destroyed a beat later than a fixed order makes it.
void ThreadScheduler::WakeSignalled() {
  if (threads_.empty()) return;
  {  // (a quick look first: the walk below moves only a waiting thread whose event is signalled or gone)
    bool any = false;
    for (size_t i = 0; i < waiters_.size();) {
      Thread* w = waiters_[i];
      if (w->waitRef == LUA_NOREF) {  // (no longer waiting)
        w->inWaiters = false;
        waiters_[i] = waiters_.back();
        waiters_.pop_back();
        continue;
      }
      if (!any && !w->dead && w != current_) {
        lua_rawgeti(L_, LUA_REGISTRYINDEX, w->waitRef);
        ScriptObject* o = GetObject(L_, -1);
        lua_pop(L_, 1);
        any = !(o && o->EventState() == 0);  // = !StillWaiting, without its clean-up
      }
      ++i;
    }
    if (!any) return;
  }
  const auto last = std::prev(threads_.end());
  for (auto it = threads_.begin();;) {
    const bool atLast = it == last;
    const auto next = std::next(it);
    Thread& t = *it;
    if (!t.dead && t.waitRef != LUA_NOREF && &t != current_ && !StillWaiting(t) && !atLast)
      threads_.splice(threads_.end(), threads_, it);
    if (atLast) break;
    it = next;
  }
}

// CTaskEvent::EventSetSignaled 0x406d90 (read): when an event is signalled, each thread waiting on it is taken off
// the stage's waiting list and linked at the END of its run list there and then (0x406dfc..0x406e07), the event's
// waiters walked from the back ([event+0xc] first), so the thread that began to wait last goes first. The
// manipulators signal in their MoveManipulator, at their place in their unit's beat (rotator 0x643b75, slider
// 0x647488, animator 0x63fb45, ...). WakeSignalled, which moves the signalled waiters in the run order's own order
// at fixed points, put two waiters signalled in one entity stage in the wrong order whenever the run order did
// not already have them in signal order. Seen: MP3, two Cybran radar dish threads (FAF CRadarUnit DishBehavior:
// WaitFor(rotator), then `if Random() < chance then WaitTicks(11)`, chances 0.4 / 0.5 / 0.6) are woken in the
// same beat and draw at 4703; ours ran them in the other order, so they drew each other's numbers. The probe
// (mp3_rand) shows the original's dish that draws at 4765 drawing again at 4827 (no WaitTicks(11)); ours' dish of
// 4765 had waited and drew at 4837, and the random stream went apart at 4827.
void ThreadScheduler::Signal(const ScriptObject* o) {
  if (!o || waiters_.empty()) return;
  std::vector<Thread*> hit;
  for (Thread* w : waiters_) {
    if (w->dead || w == current_ || w->waitRef == LUA_NOREF) continue;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, w->waitRef);
    const ScriptObject* p = GetObject(L_, -1);
    lua_pop(L_, 1);
    if (p == o) hit.push_back(w);
  }
  if (hit.empty()) return;
  std::sort(hit.begin(), hit.end(), [](const Thread* a, const Thread* b) { return a->waitSeq > b->waitSeq; });
  for (Thread* w : hit) {
    luaL_unref(L_, LUA_REGISTRYINDEX, w->waitRef);
    w->waitRef = LUA_NOREF;  // (no longer waiting: WakeSignalled and RunTick leave it where it is now)
    for (auto it = threads_.begin(); it != threads_.end(); ++it)
      if (&*it == w) {
        threads_.splice(threads_.end(), threads_, it);
        break;
      }
  }
}

void ThreadScheduler::RunTick(uint32_t tick) {
  tick_ = tick;
  WakeSignalled();
  // Threads forked during this pass are appended and run in the same pass.
  for (auto it = threads_.begin(); it != threads_.end();) {
    Thread& t = *it;
    if (!t.dead && !t.suspended && t.wakeTick <= tick) {
      const bool waited = t.waitRef != LUA_NOREF;
      if (StillWaiting(t)) {
        t.wakeTick = tick + 1;
      } else if (waited && std::next(it) != threads_.end()) {
        // (its event was signalled while this pass runs, by a thread before it: it goes to the pass's end)
        const auto next = std::next(it);
        threads_.splice(threads_.end(), threads_, it);
        it = next;
        continue;
      } else {
        Step(t);
        for (int n = 0; t.again && !t.dead && !t.suspended && n < 100000; ++n) {
          t.again = false;
          if (StillWaiting(t)) {
            t.wakeTick = tick + 1;
            break;
          }
          Step(t);
        }
        t.again = false;
      }
    }
    if (t.dead) {
      Release(t);
      it = threads_.erase(it);
    } else {
      ++it;
    }
  }
}

void RegisterThreadBindings(lua_State* L, ThreadScheduler* sched) {
  lua_pushlightuserdata(L, const_cast<char*>(&kSchedKey));
  lua_pushlightuserdata(L, sched);
  lua_rawset(L, LUA_REGISTRYINDEX);
  lua_register(L, "ForkThread", l_ForkThread);
  lua_register(L, "KillThread", l_KillThread);
  lua_register(L, "CurrentThread", l_CurrentThread);
  lua_register(L, "SuspendCurrentThread", l_SuspendCurrentThread);
  lua_register(L, "ResumeThread", l_ResumeThread);
  lua_register(L, "WaitFor", l_WaitFor);
}

}  // namespace moho
