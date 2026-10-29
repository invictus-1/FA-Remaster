/*
** $Id: lgc.h,v 1.18 2003/02/10 17:32:50 roberto Exp roberto $
** Garbage Collector
** See Copyright Notice in lua.h
*/

#ifndef lgc_h
#define lgc_h


#include "lobject.h"


#define luaC_checkGC(L) { lua_assert(!(L->ci->state & CI_CALLING)); \
	if (G(L)->nblocks >= G(L)->GCthreshold) luaC_collectgarbage(L); }


/*
** moho64: write barriers for the incremental collector (lgcinc.c). In a classic state (G(L)->inc NULL) each is
** one test and nothing else. Every store of a collectable value into a heap object goes through one:
**   luaC_barriert(L,t)    t (a Table*) was written: a black table turns gray again (re-traversed at atomic);
**   luaC_barrierf(L,o,v)  the TObject* v was stored into the object o (a GCObject*): a white v is marked;
**   luaC_barrierfo(L,o,p) the same for a GCObject* p (a metatable, a closure's globals table);
**   luaC_barrierv(L,v)    v was stored somewhere whose colour is not at hand: v is marked (o NULL = as if black).
** Stores into a thread's stack, the registry slot, the default metatables and a thread's globals slot need none:
** those are roots, marked again at the atomic step.
*/
#define luaC_barriert(L,t)	{ if (G(L)->inc) luaC_incbarriert(L, t); }
#define luaC_barrierf(L,o,v)	{ if (G(L)->inc && iscollectable(v)) luaC_incbarrierf(L, o, gcvalue(v)); }
#define luaC_barrierfo(L,o,p)	{ if (G(L)->inc) luaC_incbarrierf(L, o, p); }
#define luaC_barrierv(L,v)	{ if (G(L)->inc && iscollectable(v)) luaC_incbarrierf(L, NULL, gcvalue(v)); }

struct Table;
void luaC_incbarriert (lua_State *L, struct Table *t);
void luaC_incbarrierf (lua_State *L, GCObject *o, GCObject *v);
void luaC_linkupval (lua_State *L, GCObject *o);
lu_byte luaC_newmark (lua_State *L, lu_byte classic);
void luaC_incfree (lua_State *L);
void luaC_incfull (lua_State *L);
void luaC_incdue (lua_State *L);
int luaC_incroom (lua_State *L, lu_mem objects);  /* moho64: `objects' more new objects keep the emergency off */
void luaC_incleave (lua_State *L);
void luaC_increvive (lua_State *L, GCObject *o);
int luaC_incsweepingstrings (lua_State *L);
void luaC_separateudata (lua_State *L);
void luaC_callGCTM (lua_State *L);
void luaC_sweep (lua_State *L, int all);
void luaC_collectgarbage (lua_State *L);
void luaC_link (lua_State *L, GCObject *o, lu_byte tt);


#endif
