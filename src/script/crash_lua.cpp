// A crash's Lua frames (core/crash.h CrashLuaReader): the frames of a lua_State read from the Lua core's own
// structures (third_party/lua lstate.h, lobject.h), as ldebug.c's currentline does, without calling into Lua: a crash
// may come from inside Lua itself. Every pointer is checked with CrashReadable before it is read.
#include <cstdio>
#include <cstring>

#include "core/crash.h"

extern "C" {
#include "lobject.h"
#include "lstate.h"
}

namespace moho {

namespace {

void CrashLuaFrames(void* vL, int max, void (*line)(const char* text)) {
  lua_State* L = static_cast<lua_State*>(vL);
  if (!CrashReadable(L, sizeof *L)) return;
  CallInfo* ci = L->ci;
  CallInfo* const base = L->base_ci;
  if (!ci || !base || ci < base || ci - base > 100000) return;  // (not a state's frames: nothing)
  char text[320];
  for (int n = 0; ci > base && n < max; --ci) {
    if (!CrashReadable(ci, sizeof *ci)) return;
    if (ci->state & CI_C) {  // an engine function called from Lua (a binding), between the Lua frames
      line("(an engine function)");
      ++n;
      continue;
    }
    const TObject* f = ci->base - 1;
    if (!CrashReadable(f, sizeof *f) || f->tt != LUA_TFUNCTION) continue;
    const Closure* cl = &f->value.gc->cl;
    if (!CrashReadable(cl, sizeof(LClosure)) || cl->c.isC) continue;
    const Proto* p = cl->l.p;
    if (!CrashReadable(p, sizeof *p)) continue;
    // the frame's instruction: its running luaV_execute's pc when it has one (CI_HASFRAME), else the saved one
    const Instruction* pc = ci->u.l.savedpc;
    if ((ci->state & CI_HASFRAME) && CrashReadable(ci->u.l.pc, sizeof *ci->u.l.pc)) pc = *ci->u.l.pc;
    const long rel = static_cast<long>(pc - p->code) - 1;
    int lineNo = -1;
    if (p->lineinfo && rel >= 0 && rel < p->sizelineinfo && CrashReadable(p->lineinfo + rel, sizeof(int)))
      lineNo = p->lineinfo[rel];
    char src[256] = "?";
    const TString* s = p->source;
    if (s && CrashReadable(s, sizeof *s)) {
      size_t len = s->tsv.len;
      if (len > sizeof src - 1) len = sizeof src - 1;
      const char* chars = getstr(s);
      if (len && CrashReadable(chars, len)) {
        std::memcpy(src, chars, len);
        src[len] = 0;
      }
    }
    const char* shown = src[0] == '@' ? src + 1 : src;  // (a file's chunk name is "@<path>")
    if (lineNo >= 0) std::snprintf(text, sizeof text, "%s:%d", shown, lineNo);
    else std::snprintf(text, sizeof text, "%s (entering it)", shown);  // (no instruction run yet: a call hook)
    line(text);
    ++n;
  }
}

}  // namespace

void InstallCrashLuaReader() { CrashSetLuaReader(CrashLuaFrames); }

}  // namespace moho
