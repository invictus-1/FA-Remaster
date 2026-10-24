/*
** moho64: the collector's mode and the incremental collector's entry points.
** Copyright (C) 1994-2003 Tecgraf, PUC-Rio (Lua 5.0; MIT licence, see lua.h); the moho64 changes are under the
** same licence.
**
** A Lua state runs one of two collectors, chosen once, right after lua_open:
** - classic: Lua 5.0's stop-the-world mark and sweep (lgc.c), unchanged; the default;
** - incremental: a tri-colour collector after the design of Lua 5.1's (two whites, gray lists, write barriers, an
**   atomic step), stepped only at the end of a beat with a budget counted in objects (gc_design.md).
**
** Stage 1 (this file today): the switch and the entry points exist; only the classic mode is accepted, and every
** entry point does nothing in a classic state. The barrier calls (lgc.h) are in place at every store site.
*/

#include <stdlib.h>

#define lgcinc_c

#include "lua.h"

#include "lgc.h"
#include "lobject.h"
#include "lstate.h"


LUA_API int lua_gpg_setgcmode (lua_State *L, int mode) {
  return mode == 0 && G(L)->inc == NULL;  /* only the classic collector so far */
}


LUA_API void lua_gpg_gcstep (lua_State *L) {
  if (G(L)->inc == NULL) return;  /* classic: collections happen where the threshold falls */
}


void luaC_incbarriert (lua_State *L, struct Table *t) { UNUSED(L); UNUSED(t); }

void luaC_incbarrierf (lua_State *L, GCObject *o, GCObject *v) { UNUSED(L); UNUSED(o); UNUSED(v); }

void luaC_linkupval (lua_State *L, GCObject *o) { luaC_link(L, o, LUA_TUPVAL); }

lu_byte luaC_newmark (lua_State *L, lu_byte classic) { UNUSED(L); return classic ? (1<<1) : 0; }

void luaC_incemergency (lua_State *L) { UNUSED(L); }

void luaC_incfree (lua_State *L) {
  if (G(L)->inc) free(G(L)->inc);
  G(L)->inc = NULL;
}
