#include "script/threads.h"

#include <string>

#include "core/log.h"
#include "script/script_state.h"

extern "C" {
extern void (*lua_gpg_resume_error_hook)(lua_State* L, int status);
}

namespace moho {
namespace {

const char kSchedKey = 0;
std::string g_threadError;  // traceback captured when a coroutine fails

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
  bool self = co == L;
  s->Kill(co);
  if (self) return lua_yield(L, 0);
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

// WaitFor(event): wait until an engine event (economy event, script task, effect) is done.
// TODO(M4): real events; until they exist, wait one tick.
int l_WaitFor(lua_State* L) {
  ThreadScheduler* s = ThreadScheduler::From(L);
  if (!s || s->Current() != L) return luaL_error(L, "WaitFor: not called from a thread");
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
  lua_State* co = lua_newthread(caller);  // pushed on caller
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
  t.wakeTick = tick_ + 1;  // a new thread first runs on the next tick (probe: forked at 0, runs at 1)
  threads_.push_back(t);
}

void ThreadScheduler::PushCurrent(lua_State* L) {
  if (current_ && current_->ref != LUA_NOREF) lua_rawgeti(L, LUA_REGISTRYINDEX, current_->ref);
  else lua_pushnil(L);
}

void ThreadScheduler::Release(Thread& t) {
  if (t.ref != LUA_NOREF) luaL_unref(L_, LUA_REGISTRYINDEX, t.ref);
  t.ref = LUA_NOREF;
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
  lua_gpg_resume_error_hook = OnResumeError;
  Thread* prev = current_;
  current_ = &t;
  int nargs = t.started ? 0 : t.pendingArgs;
  t.started = true;
  int rc = lua_resume(t.co, nargs);
  current_ = prev;
  if (rc != 0) {
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
  int wait = static_cast<int>(lua_tonumber(t.co, 1)) - 1;
  if (wait < 1) wait = 1;
  lua_settop(t.co, 0);
  t.wakeTick = tick_ + static_cast<uint32_t>(wait);
}

void ThreadScheduler::RunTick(uint32_t tick) {
  tick_ = tick;
  // Threads forked during this pass are appended and run in the same pass.
  for (auto it = threads_.begin(); it != threads_.end();) {
    Thread& t = *it;
    if (!t.dead && !t.suspended && t.wakeTick <= tick) Step(t);
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
