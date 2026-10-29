/*
** $Id: lapi.c,v 1.234 2003/04/03 13:35:34 roberto Exp roberto $
** Lua API
** See Copyright Notice in lua.h
*/


#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define lapi_c

#include "lua.h"

#include "lapi.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lundump.h"
#include "lvm.h"


const char lua_ident[] =
  "$Lua: " LUA_VERSION " " LUA_COPYRIGHT " $\n"
  "$Authors: " LUA_AUTHORS " $\n"
  "$URL: www.lua.org $\n";



#ifndef api_check
#define api_check(L, o)		/*{ assert(o); }*/
#endif

#define api_checknelems(L, n)	api_check(L, (n) <= (L->top - L->base))

#define api_incr_top(L)   {api_check(L, L->top < L->ci->top); L->top++;}




static TObject *negindex (lua_State *L, int idx) {
  if (idx > LUA_REGISTRYINDEX) {
    api_check(L, idx != 0 && -idx <= L->top - L->base);
    return L->top+idx;
  }
  else switch (idx) {  /* pseudo-indices */
    case LUA_REGISTRYINDEX: return registry(L);
    case LUA_GLOBALSINDEX: return gt(L);
    default: {
      TObject *func = (L->base - 1);
      idx = LUA_GLOBALSINDEX - idx;
      lua_assert(iscfunction(func));
      return (idx <= clvalue(func)->c.nupvalues)
                ? &clvalue(func)->c.upvalue[idx-1]
                : NULL;
    }
  }
}


static TObject *luaA_index (lua_State *L, int idx) {
  if (idx > 0) {
    api_check(L, idx <= L->top - L->base);
    return L->base + idx - 1;
  }
  else {
    TObject *o = negindex(L, idx);
    api_check(L, o != NULL);
    return o;
  }
}


static TObject *luaA_indexAcceptable (lua_State *L, int idx) {
  if (idx > 0) {
    TObject *o = L->base+(idx-1);
    api_check(L, idx <= L->stack_last - L->base);
    if (o >= L->top) return NULL;
    else return o;
  }
  else
    return negindex(L, idx);
}


void luaA_pushobject (lua_State *L, const TObject *o) {
  setobj2s(L->top, o);
  incr_top(L);
}


LUA_API int lua_checkstack (lua_State *L, int size) {
  int res;
  lua_lock(L);
  if ((L->top - L->base + size) > LUA_MAXCSTACK)
    res = 0;  /* stack overflow */
  else {
    luaD_checkstack(L, size);
    if (L->ci->top < L->top + size)
      L->ci->top = L->top + size;
    res = 1;
  }
  lua_unlock(L);
  return res;
}


LUA_API void lua_xmove (lua_State *from, lua_State *to, int n) {
  int i;
  lua_lock(to);
  api_checknelems(from, n);
  from->top -= n;
  for (i = 0; i < n; i++) {
    setobj2s(to->top, from->top + i);
    api_incr_top(to);
  }
  lua_unlock(to);
}


LUA_API lua_CFunction lua_atpanic (lua_State *L, lua_CFunction panicf) {
  lua_CFunction old;
  lua_lock(L);
  old = G(L)->panic;
  G(L)->panic = panicf;
  lua_unlock(L);
  return old;
}


LUA_API lua_State *lua_newthread (lua_State *L) {
  lua_State *L1;
  lua_lock(L);
  luaC_checkGC(L);
  L1 = luaE_newthread(L);
  setthvalue(L->top, L1);
  api_incr_top(L);
  lua_unlock(L);
  lua_userstateopen(L1);
  return L1;
}



/*
** basic stack manipulation
*/


LUA_API int lua_gettop (lua_State *L) {
  return (L->top - L->base);
}


LUA_API void lua_settop (lua_State *L, int idx) {
  lua_lock(L);
  if (idx >= 0) {
    api_check(L, idx <= L->stack_last - L->base);
    while (L->top < L->base + idx)
      setnilvalue(L->top++);
    L->top = L->base + idx;
  }
  else {
    api_check(L, -(idx+1) <= (L->top - L->base));
    L->top += idx+1;  /* `subtract' index (index is negative) */
  }
  lua_unlock(L);
}


LUA_API void lua_remove (lua_State *L, int idx) {
  StkId p;
  lua_lock(L);
  p = luaA_index(L, idx);
  while (++p < L->top) setobjs2s(p-1, p);
  L->top--;
  lua_unlock(L);
}


LUA_API void lua_insert (lua_State *L, int idx) {
  StkId p;
  StkId q;
  lua_lock(L);
  p = luaA_index(L, idx);
  for (q = L->top; q>p; q--) setobjs2s(q, q-1);
  setobjs2s(p, L->top);
  lua_unlock(L);
}


LUA_API void lua_replace (lua_State *L, int idx) {
  lua_lock(L);
  api_checknelems(L, 1);
  setobj(luaA_index(L, idx), L->top - 1);  /* write barrier */
  if (idx < LUA_GLOBALSINDEX)  /* a C closure's upvalue (the stack, globals and registry slots are roots) */
    luaC_barrierv(L, L->top - 1);
  L->top--;
  lua_unlock(L);
}


LUA_API void lua_pushvalue (lua_State *L, int idx) {
  lua_lock(L);
  setobj2s(L->top, luaA_index(L, idx));
  api_incr_top(L);
  lua_unlock(L);
}



/*
** access functions (stack -> C)
*/


LUA_API int lua_type (lua_State *L, int idx) {
  StkId o = luaA_indexAcceptable(L, idx);
  return (o == NULL) ? LUA_TNONE : ttype(o);
}


LUA_API const char *lua_typename (lua_State *L, int t) {
  UNUSED(L);
  return (t == LUA_TNONE) ? "no value" : luaT_typenames[t];
}


LUA_API int lua_iscfunction (lua_State *L, int idx) {
  StkId o = luaA_indexAcceptable(L, idx);
  return (o == NULL) ? 0 : iscfunction(o);
}


LUA_API int lua_isnumber (lua_State *L, int idx) {
  TObject n;
  const TObject *o = luaA_indexAcceptable(L, idx);
  return (o != NULL && tonumber(o, &n));
}


LUA_API int lua_isstring (lua_State *L, int idx) {
  int t = lua_type(L, idx);
  return (t == LUA_TSTRING || t == LUA_TNUMBER);
}


LUA_API int lua_isuserdata (lua_State *L, int idx) {
  const TObject *o = luaA_indexAcceptable(L, idx);
  return (o != NULL && (ttisuserdata(o) || ttislightuserdata(o)));
}


LUA_API int lua_rawequal (lua_State *L, int index1, int index2) {
  StkId o1 = luaA_indexAcceptable(L, index1);
  StkId o2 = luaA_indexAcceptable(L, index2);
  return (o1 == NULL || o2 == NULL) ? 0  /* index out of range */
                                    : luaO_rawequalObj(o1, o2);
}


LUA_API int lua_equal (lua_State *L, int index1, int index2) {
  StkId o1, o2;
  int i;
  lua_lock(L);  /* may call tag method */
  o1 = luaA_indexAcceptable(L, index1);
  o2 = luaA_indexAcceptable(L, index2);
  i = (o1 == NULL || o2 == NULL) ? 0  /* index out of range */
                                 : equalobj(L, o1, o2);
  lua_unlock(L);
  return i;
}


LUA_API int lua_lessthan (lua_State *L, int index1, int index2) {
  StkId o1, o2;
  int i;
  lua_lock(L);  /* may call tag method */
  o1 = luaA_indexAcceptable(L, index1);
  o2 = luaA_indexAcceptable(L, index2);
  i = (o1 == NULL || o2 == NULL) ? 0  /* index out-of-range */
                                 : luaV_lessthan(L, o1, o2);
  lua_unlock(L);
  return i;
}



LUA_API lua_Number lua_tonumber (lua_State *L, int idx) {
  TObject n;
  const TObject *o = luaA_indexAcceptable(L, idx);
  if (o != NULL && tonumber(o, &n))
    return nvalue(o);
  else
    return 0;
}


LUA_API int lua_toboolean (lua_State *L, int idx) {
  const TObject *o = luaA_indexAcceptable(L, idx);
  return (o != NULL) && !l_isfalse(o);
}


LUA_API const char *lua_tostring (lua_State *L, int idx) {
  StkId o = luaA_indexAcceptable(L, idx);
  if (o == NULL)
    return NULL;
  else if (ttisstring(o))
    return svalue(o);
  else {
    const char *s;
    lua_lock(L);  /* `luaV_tostring' may create a new string */
    s = (luaV_tostring(L, o) ? svalue(o) : NULL);
    luaC_checkGC(L);
    lua_unlock(L);
    return s;
  }
}


LUA_API size_t lua_strlen (lua_State *L, int idx) {
  StkId o = luaA_indexAcceptable(L, idx);
  if (o == NULL)
    return 0;
  else if (ttisstring(o))
    return tsvalue(o)->tsv.len;
  else {
    size_t l;
    lua_lock(L);  /* `luaV_tostring' may create a new string */
    l = (luaV_tostring(L, o) ? tsvalue(o)->tsv.len : 0);
    lua_unlock(L);
    return l;
  }
}


LUA_API lua_CFunction lua_tocfunction (lua_State *L, int idx) {
  StkId o = luaA_indexAcceptable(L, idx);
  return (o == NULL || !iscfunction(o)) ? NULL : clvalue(o)->c.f;
}


LUA_API void *lua_touserdata (lua_State *L, int idx) {
  StkId o = luaA_indexAcceptable(L, idx);
  if (o == NULL) return NULL;
  switch (ttype(o)) {
    case LUA_TUSERDATA: return (uvalue(o) + 1);
    case LUA_TLIGHTUSERDATA: return pvalue(o);
    default: return NULL;
  }
}


LUA_API lua_State *lua_tothread (lua_State *L, int idx) {
  StkId o = luaA_indexAcceptable(L, idx);
  return (o == NULL || !ttisthread(o)) ? NULL : thvalue(o);
}


LUA_API const void *lua_topointer (lua_State *L, int idx) {
  StkId o = luaA_indexAcceptable(L, idx);
  if (o == NULL) return NULL;
  else {
    switch (ttype(o)) {
      case LUA_TTABLE: return hvalue(o);
      case LUA_TFUNCTION: return clvalue(o);
      case LUA_TTHREAD: return thvalue(o);
      case LUA_TUSERDATA:
      case LUA_TLIGHTUSERDATA:
        return lua_touserdata(L, idx);
      default: return NULL;
    }
  }
}



/*
** push functions (C -> stack)
*/


LUA_API void lua_pushnil (lua_State *L) {
  lua_lock(L);
  setnilvalue(L->top);
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API void lua_pushnumber (lua_State *L, lua_Number n) {
  lua_lock(L);
  setnvalue(L->top, n);
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API void lua_pushlstring (lua_State *L, const char *s, size_t len) {
  lua_lock(L);
  luaC_checkGC(L);
  setsvalue2s(L->top, luaS_newlstr(L, s, len));
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API void lua_pushstring (lua_State *L, const char *s) {
  if (s == NULL)
    lua_pushnil(L);
  else
    lua_pushlstring(L, s, strlen(s));
}


LUA_API const char *lua_pushvfstring (lua_State *L, const char *fmt,
                                      va_list argp) {
  const char *ret;
  lua_lock(L);
  luaC_checkGC(L);
  ret = luaO_pushvfstring(L, fmt, argp);
  lua_unlock(L);
  return ret;
}


LUA_API const char *lua_pushfstring (lua_State *L, const char *fmt, ...) {
  const char *ret;
  va_list argp;
  lua_lock(L);
  luaC_checkGC(L);
  va_start(argp, fmt);
  ret = luaO_pushvfstring(L, fmt, argp);
  va_end(argp);
  lua_unlock(L);
  return ret;
}


LUA_API void lua_pushcclosure (lua_State *L, lua_CFunction fn, int n) {
  Closure *cl;
  lua_lock(L);
  luaC_checkGC(L);
  api_checknelems(L, n);
  cl = luaF_newCclosure(L, n);
  cl->c.f = fn;
  L->top -= n;
  while (n--)
    setobj2n(&cl->c.upvalue[n], L->top+n);
  setclvalue(L->top, cl);
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API void lua_pushboolean (lua_State *L, int b) {
  lua_lock(L);
  setbvalue(L->top, (b != 0));  /* ensure that true is 1 */
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API void lua_pushlightuserdata (lua_State *L, void *p) {
  lua_lock(L);
  setpvalue(L->top, p);
  api_incr_top(L);
  lua_unlock(L);
}



/*
** get functions (Lua -> stack)
*/


LUA_API void lua_gettable (lua_State *L, int idx) {
  StkId t;
  lua_lock(L);
  t = luaA_index(L, idx);
  setobj2s(L->top - 1, luaV_gettable(L, t, L->top - 1, 0));
  lua_unlock(L);
}


LUA_API void lua_rawget (lua_State *L, int idx) {
  StkId t;
  lua_lock(L);
  t = luaA_index(L, idx);
  api_check(L, ttistable(t));
  setobj2s(L->top - 1, luaH_get(hvalue(t), L->top - 1));
  lua_unlock(L);
}


LUA_API void *lua_getextra (lua_State *L, int i) {
  return G(L)->extra[i];
}

LUA_API void lua_setextra (lua_State *L, int i, void *p) {
  G(L)->extra[i] = p;
}

LUA_API void lua_rawgetcobject (lua_State *L, int idx) {
  StkId t;
  lua_lock(L);
  t = luaA_index(L, idx);
  api_check(L, ttistable(t));
  setobj2s(L->top, luaH_getstr(hvalue(t), G(L)->cobjkey));
  api_incr_top(L);
  lua_unlock(L);
}

LUA_API void lua_pushcobjectkey (lua_State *L) {
  lua_lock(L);
  setsvalue2s(L->top, G(L)->cobjkey);
  api_incr_top(L);
  lua_unlock(L);
}

/*
** moho64: t[k] (raw) for a C string key, pushed, for a host's views of the simulation (src/sim/sim_view.cpp)
** that run between beats as often as the host likes. lua_pushstring makes the key's string if it is new and
** runs the collector's check (luaC_checkGC) either way, so a host's reads could move a collection to another
** moment or change the heap; this makes and checks nothing. A key the state has no string for is in no
** table: nil.
*/
LUA_API void lua_gpg_rawgetstr (lua_State *L, int idx, const char *k) {
  StkId t;
  TString *ts;
  lua_lock(L);
  t = luaA_index(L, idx);
  api_check(L, ttistable(t));
  ts = luaS_gpg_find(L, k, strlen(k));
  if (ts == NULL) setnilvalue(L->top);
  else setobj2s(L->top, luaH_getstr(hvalue(t), ts));
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API void lua_gpg_checkgc (lua_State *L) {
  lua_lock(L);
  luaC_checkGC(L);
  lua_unlock(L);
}


/*
** moho64: luaL_getn / luaL_setn (lauxlib.c) made here on the table itself instead of through ten API calls each
** (table.insert: 412 ns, table.getn: 348 ns a call in a one-hour game; ~25 M calls). The same steps in the same order
** with the same effects: each lua_pushliteral(L, "n") is its luaC_checkGC and luaS_newlstr; every value the API steps
** would have put on the stack is written to the same slot above the top (those slots are read by nobody, but a
** later collection inside the caller's frame marks them: the same objects stay reachable for the same time); the
** numbers are read as checkint reads them (lua_tonumber, then lua_isnumber: a numeric string counts); the count is
** the same walk (t[1], t[2], ... raw, until a nil) on the array part and then the hash part. The registry's `sizes'
** table (ARRAYSIZE_REF) missing, they answer -2 / 0 and leave the rest to lauxlib's own code, which makes it there.
*/
static int gpg_checkint (const TObject *o) {  /* lauxlib.c checkint */
  TObject tmp;
  lua_Number x = 0;
  int isnum = 0;
  if (ttisnumber(o)) { x = nvalue(o); isnum = 1; }
  else if ((o = luaV_tonumber(o, &tmp)) != NULL) { x = nvalue(o); isnum = 1; }
  {
    int n = (int)x;
    if (n == 0 && !isnum) n = -1;
    return n;
  }
}

/* t.n as luaL_getn / luaL_setn ask for it (lua_pushliteral(L, "n"); lua_rawget(L, t)): the value is left in s */
static void gpg_getfield_n (lua_State *L, int idx) {
  StkId s;
  Table *h;
  luaC_checkGC(L);
  s = L->top;
  setsvalue2s(s, luaS_newliteral(L, "n"));
  h = hvalue(luaA_index(L, idx));
  setobj2s(s, luaH_getstr(h, tsvalue(s)));
}

/* luaL_getn's lookups and count; -2: the sizes table is missing (its first slot holds the nil the API read) */
LUA_API int lua_gpg_getn (lua_State *L, int idx, int sizesref) {
  StkId s;
  Table *h;
  int n;
  lua_lock(L);
  gpg_getfield_n(L, idx);
  s = L->top;
  if ((n = gpg_checkint(s)) >= 0) { lua_unlock(L); return n; }
  setobj2s(s, luaH_getnum(hvalue(registry(L)), sizesref));  /* getsizes: lua_rawgeti(registry, ref) */
  if (ttisnil(s)) { lua_unlock(L); return -2; }
  setobj2s(s + 1, luaA_index(L, idx));  /* lua_pushvalue(L, t); lua_rawget(L, -2) */
  setobj2s(s + 1, luaH_get(hvalue(s), s + 1));
  if ((n = gpg_checkint(s + 1)) >= 0) { lua_unlock(L); return n; }
  h = hvalue(luaA_index(L, idx));
  for (n = 1; ; n++) {  /* lua_rawgeti(L, t, n) until nil: luaH_getnum (array part, then hash part) */
    const TObject *e = (n <= h->sizearray) ? &h->array[n - 1] : luaH_getnum(h, n);
    if (ttisnil(e)) break;
  }
  setnilvalue(s);  /* (the last lua_rawgeti's nil) */
  lua_unlock(L);
  return n - 1;
}

/* luaL_setn's steps; 0: the sizes table is missing and nothing past getsizes' first read was done */
LUA_API int lua_gpg_setn (lua_State *L, int idx, int n, int sizesref) {
  StkId s;
  Table *h;
  lua_lock(L);
  gpg_getfield_n(L, idx);
  s = L->top;
  if (gpg_checkint(s) >= 0) {  /* t.n = n: lua_pushliteral(L, "n"); lua_pushnumber(L, n); lua_rawset(L, t) */
    luaC_checkGC(L);
    s = L->top;
    setsvalue2s(s, luaS_newliteral(L, "n"));
    setnvalue(s + 1, (lua_Number)n);
    h = hvalue(luaA_index(L, idx));
    setobj2t(luaH_set(L, h, s), s + 1);
    luaC_barriert(L, h);
    lua_unlock(L);
    return 1;
  }
  setobj2s(s, luaH_getnum(hvalue(registry(L)), sizesref));  /* getsizes */
  if (ttisnil(s)) { lua_unlock(L); return 0; }
  setobj2s(s + 1, luaA_index(L, idx));  /* sizes[t] = n: lua_pushvalue(L, t); lua_pushnumber(L, n); lua_rawset(L, -3) */
  setnvalue(s + 2, (lua_Number)n);
  h = hvalue(s);
  setobj2t(luaH_set(L, h, s + 1), s + 2);
  luaC_barriert(L, h);
  lua_unlock(L);
  return 1;
}


LUA_API void lua_gpg_setnum3 (lua_State *L, int idx, lua_Number a, lua_Number b, lua_Number c) {
  Table *t;
  lua_lock(L);
  t = hvalue(luaA_index(L, idx));
  setnvalue(luaH_setnum(L, t, 1), a);
  setnvalue(luaH_setnum(L, t, 2), b);
  setnvalue(luaH_setnum(L, t, 3), c);
  luaC_barriert(L, t);
  lua_unlock(L);
}


/*
** moho64: the largest positive number among the keys of the table at idx that hold a value (0: none), as a walk with
** lua_next finds it (the array part's keys 1 .. sizearray, then each node's; the walk's note made the same:
** luaH_notewalk once, at its first step), without the walk's pushes and its look-up of each key it steps from.
** (moho::ObjectCount, src/script/lua.hpp: the original's lua_getn 0x90ad30 takes this when t.n is no number.)
*/
LUA_API lua_Number lua_gpg_maxnumkey (lua_State *L, int idx) {
  Table *h;
  lua_Number best = 0;
  int i;
  lua_lock(L);
  h = hvalue(luaA_index(L, idx));
  luaH_notewalk(L, h);
  for (i = h->sizearray; i > 0; i--)
    if (!ttisnil(&h->array[i - 1])) {
      best = cast(lua_Number, i);
      break;
    }
  for (i = sizenode(h) - 1; i >= 0; i--) {
    Node *n = gnode(h, i);
    if (!ttisnil(gval(n)) && ttisnumber(gkey(n)) && nvalue(gkey(n)) > best) best = nvalue(gkey(n));
  }
  lua_unlock(L);
  return best;
}


/*
** moho64: what lua_rawgeti(L, idx, n) would push, read in place: its type (as lua_type gives it); a string's text and
** length (*s, *len: lua_tostring / lua_strlen); a table's `_c_object' field as lua_rawgetcobject + lua_touserdata
** read it (*box: a userdata's block, a light userdata's pointer, else NULL). Makes, pushes and checks nothing.
*/
LUA_API int lua_gpg_peeki (lua_State *L, int idx, int n, const char **s, size_t *len, void **box) {
  const TObject *o;
  int t;
  lua_lock(L);
  o = luaH_getnum(hvalue(luaA_index(L, idx)), n);
  t = ttype(o);
  if (t == LUA_TSTRING) {
    *s = svalue(o);
    *len = tsvalue(o)->tsv.len;
  }
  else if (t == LUA_TTABLE) {
    const TObject *c = luaH_getstr(hvalue(o), G(L)->cobjkey);
    *box = ttisuserdata(c) ? cast(void *, uvalue(c) + 1) : ttislightuserdata(c) ? pvalue(c) : NULL;
  }
  lua_unlock(L);
  return t;
}


/*
** moho64: dst[k] = src[ix[k-1]] (raw) for k = 1 .. n, the same stores n pairs lua_rawgeti(L, src, ix) +
** lua_rawseti(L, dst, k) make (luaH_getnum, luaH_setnum, the table's write barrier: once, a gray table stays gray
** for the rest), into dst's array part made at once first (luaH_gpg_arrayfor: dst a new table, the values not nil).
** No collector check falls between those pairs, so the heap is the same after them.
*/
LUA_API void lua_gpg_copyi (lua_State *L, int dst, int src, const int *ix, int n) {
  Table *d, *s;
  int k;
  if (n <= 0) return;
  lua_lock(L);
  d = hvalue(luaA_index(L, dst));
  s = hvalue(luaA_index(L, src));
  luaH_gpg_arrayfor(L, d, n);
  for (k = 0; k < n; k++) {
    const TObject *v = luaH_getnum(s, ix[k]);
    setobj2t(luaH_setnum(L, d, k + 1), v);
  }
  luaC_barriert(L, d);
  lua_unlock(L);
}


/* moho64: luaH_gpg_arrayfor on the table at idx (a new table that n stores t[1] .. t[n] will fill) */
LUA_API void lua_gpg_arrayfor (lua_State *L, int idx, int n) {
  lua_lock(L);
  luaH_gpg_arrayfor(L, hvalue(luaA_index(L, idx)), n);
  lua_unlock(L);
}


/*
** moho64: 1 when no luaC_checkGC can collect while a binding makes at most `tables' tables with `slots' array slots
** and `nodes' hash nodes in all (their growth's reallocations included) and at most `strings' new strings of `chars'
** bytes in all: the heap stays below the threshold (a classic state; luaS_resize's new bucket array included) and no
** new object reaches the emergency count (an incremental state: luaC_newmark). Such a binding may then make its
** tables at their final sizes at once (luaH_new, lua_gpg_arrayfor) instead of growing them: each check in between
** is one that does nothing either way, the same objects are made in the same order, and the heap holds the same
** blocks at the end, so the collector's moments stay where they were.
*/
LUA_API int lua_gpg_nocollect (lua_State *L, int tables, int slots, int nodes, int strings, size_t chars) {
  global_State *g = G(L);
  lu_mem bytes;
  int r;
  lua_lock(L);
  bytes = cast(lu_mem, tables) * sizeof(Table) + 2 * cast(lu_mem, slots) * sizeof(TObject) +
          2 * cast(lu_mem, nodes) * sizeof(Node) + cast(lu_mem, strings) * sizestring(0) + cast(lu_mem, chars);
  if (strings > 0) bytes += 3 * cast(lu_mem, g->strt.size) * sizeof(GCObject *);  /* (new array before the old freed) */
  r = strings < g->strt.size && g->nblocks < g->GCthreshold && bytes < g->GCthreshold - g->nblocks;
  if (r && g->inc) r = luaC_incroom(L, cast(lu_mem, tables) + cast(lu_mem, strings));
  lua_unlock(L);
  return r;
}


LUA_API unsigned int lua_gpg_strhash (const char *s, size_t len) {
  return cast(unsigned int, luaS_gpg_hash(s, len));
}


LUA_API void lua_rawgeti (lua_State *L, int idx, int n) {
  StkId o;
  lua_lock(L);
  o = luaA_index(L, idx);
  api_check(L, ttistable(o));
  setobj2s(L->top, luaH_getnum(hvalue(o), n));
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API void lua_newtable (lua_State *L) {
  lua_lock(L);
  luaC_checkGC(L);
  sethvalue(L->top, luaH_new(L, 0, 0));
  api_incr_top(L);
  lua_unlock(L);
}


/* moho64: a new table with room for narray array items and 2^lnhash hash items */
LUA_API void lua_newtablesized (lua_State *L, int narray, int lnhash) {
  lua_lock(L);
  luaC_checkGC(L);
  sethvalue(L->top, luaH_new(L, narray, lnhash));
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API int lua_getmetatable (lua_State *L, int objindex) {
  const TObject *obj;
  Table *mt = NULL;
  int res;
  lua_lock(L);
  obj = luaA_indexAcceptable(L, objindex);
  if (obj != NULL) {
    switch (ttype(obj)) {
      case LUA_TTABLE:
        mt = hvalue(obj)->metatable;
        break;
      case LUA_TUSERDATA:
        mt = uvalue(obj)->uv.metatable;
        break;
      default:  /* GPG/LuaPlus: default metatable of the basic type */
        mt = ttistable(&G(L)->defmeta[ttype(obj)]) ? hvalue(&G(L)->defmeta[ttype(obj)]) : NULL;
        break;
    }
  }
  if (mt == NULL || mt == hvalue(defaultmeta(L)))
    res = 0;
  else {
    sethvalue(L->top, mt);
    api_incr_top(L);
    res = 1;
  }
  lua_unlock(L);
  return res;
}


LUA_API void lua_getfenv (lua_State *L, int idx) {
  StkId o;
  lua_lock(L);
  o = luaA_index(L, idx);
  setobj2s(L->top, isLfunction(o) ? &clvalue(o)->l.g : gt(L));
  api_incr_top(L);
  lua_unlock(L);
}


/*
** set functions (stack -> Lua)
*/


LUA_API void lua_settable (lua_State *L, int idx) {
  StkId t;
  lua_lock(L);
  api_checknelems(L, 2);
  t = luaA_index(L, idx);
  luaV_settable(L, t, L->top - 2, L->top - 1);
  L->top -= 2;  /* pop index and value */
  lua_unlock(L);
}


LUA_API void lua_rawset (lua_State *L, int idx) {
  StkId t;
  lua_lock(L);
  api_checknelems(L, 2);
  t = luaA_index(L, idx);
  api_check(L, ttistable(t));
  setobj2t(luaH_set(L, hvalue(t), L->top-2), L->top-1);  /* write barrier */
  luaC_barriert(L, hvalue(t));
  L->top -= 2;
  lua_unlock(L);
}


LUA_API void lua_rawseti (lua_State *L, int idx, int n) {
  StkId o;
  lua_lock(L);
  api_checknelems(L, 1);
  o = luaA_index(L, idx);
  api_check(L, ttistable(o));
  setobj2t(luaH_setnum(L, hvalue(o), n), L->top-1);  /* write barrier */
  luaC_barriert(L, hvalue(o));
  L->top--;
  lua_unlock(L);
}


LUA_API int lua_setmetatable (lua_State *L, int objindex) {
  TObject *obj, *mt;
  int res = 1;
  lua_lock(L);
  api_checknelems(L, 1);
  obj = luaA_index(L, objindex);
  mt = (!ttisnil(L->top - 1)) ? L->top - 1 : defaultmeta(L);
  api_check(L, ttistable(mt));
  switch (ttype(obj)) {
    case LUA_TTABLE: {
      hvalue(obj)->metatable = hvalue(mt);  /* write barrier */
      luaC_barrierfo(L, gcvalue(obj), valtogco(hvalue(mt)));
      break;
    }
    case LUA_TUSERDATA: {
      uvalue(obj)->uv.metatable = hvalue(mt);  /* write barrier */
      luaC_barrierfo(L, gcvalue(obj), valtogco(hvalue(mt)));
      break;
    }
    default: {
      res = 0;  /* cannot set */
      break;
    }
  }
  L->top--;
  lua_unlock(L);
  return res;
}


LUA_API int lua_setfenv (lua_State *L, int idx) {
  StkId o;
  int res = 0;
  lua_lock(L);
  api_checknelems(L, 1);
  o = luaA_index(L, idx);
  L->top--;
  api_check(L, ttistable(L->top));
  if (isLfunction(o)) {
    res = 1;
    clvalue(o)->l.g = *(L->top);
    luaC_barrierf(L, gcvalue(o), L->top);
  }
  lua_unlock(L);
  return res;
}


/*
** `load' and `call' functions (run Lua code)
*/

LUA_API void lua_call (lua_State *L, int nargs, int nresults) {
  StkId func;
  lua_lock(L);
  api_checknelems(L, nargs+1);
  func = L->top - (nargs+1);
  luaD_call(L, func, nresults);
  lua_unlock(L);
}



/*
** Execute a protected call.
*/
struct CallS {  /* data to `f_call' */
  StkId func;
  int nresults;
};


static void f_call (lua_State *L, void *ud) {
  struct CallS *c = cast(struct CallS *, ud);
  luaD_call(L, c->func, c->nresults);
}



/* moho64 (MOHO64_PROFILE only): timing hooks around the API's entry points, set by the host's profiler
   (src/core/profile.cpp); kind 0 = lua_pcall, 1 = lua_resume. NULL (the default): nothing is called. They never
   touch the state. */
void (*lua_gpg_prof_enter)(lua_State *L, int kind) = NULL;
/* lstate.h lua_lock: set by src/core/workers.cpp while a parallel task runs on this thread */
#if defined(_MSC_VER) && !defined(__clang__)
__declspec(thread) int lua_gpg_in_task = 0;
#else
__thread int lua_gpg_in_task = 0;
#endif
/* ... and what lua_lock calls when it is set: a parallel task called the Lua API, which the tasks never may */
void lua_gpg_task_used_lua (lua_State *L) {
  (void)L;
  fprintf(stderr, "moho64: a parallel task called the Lua API (src/core/workers.h: tasks never touch a Lua state)\n");
  fflush(stderr);
  abort();
}
void (*lua_gpg_prof_exit)(lua_State *L) = NULL;
/* (and a collection's start (0) and end (1): lgc.c luaC_collectgarbage; MOHO64_PROFILE_HITCH only) */
void (*lua_gpg_prof_gc)(lua_State *L, int end) = NULL;

LUA_API int lua_pcall (lua_State *L, int nargs, int nresults, int errfunc) {
  struct CallS c;
  int status;
  ptrdiff_t func;
  if (lua_gpg_prof_enter) lua_gpg_prof_enter(L, 0);
  lua_lock(L);
  func = (errfunc == 0) ? 0 : savestack(L, luaA_index(L, errfunc));
  c.func = L->top - (nargs+1);  /* function to be called */
  c.nresults = nresults;
  status = luaD_pcall(L, f_call, &c, savestack(L, c.func), func);
  lua_unlock(L);
  if (lua_gpg_prof_exit) lua_gpg_prof_exit(L);
  return status;
}


/*
** Execute a protected C call.
*/
struct CCallS {  /* data to `f_Ccall' */
  lua_CFunction func;
  void *ud;
};


static void f_Ccall (lua_State *L, void *ud) {
  struct CCallS *c = cast(struct CCallS *, ud);
  Closure *cl;
  cl = luaF_newCclosure(L, 0);
  cl->c.f = c->func;
  setclvalue(L->top, cl);  /* push function */
  incr_top(L);
  setpvalue(L->top, c->ud);  /* push only argument */
  incr_top(L);
  luaD_call(L, L->top - 2, 0);
}


LUA_API int lua_cpcall (lua_State *L, lua_CFunction func, void *ud) {
  struct CCallS c;
  int status;
  lua_lock(L);
  c.func = func;
  c.ud = ud;
  status = luaD_pcall(L, f_Ccall, &c, savestack(L, L->top), 0);
  lua_unlock(L);
  return status;
}


LUA_API int lua_load (lua_State *L, lua_Chunkreader reader, void *data,
                      const char *chunkname) {
  ZIO z;
  int status;
  int c;
  lua_lock(L);
  if (!chunkname) chunkname = "?";
  luaZ_init(&z, reader, data, chunkname);
  c = luaZ_lookahead(&z);
  status = luaD_protectedparser(L, &z, (c == LUA_SIGNATURE[0]));
  lua_unlock(L);
  return status;
}


LUA_API int lua_dump (lua_State *L, lua_Chunkwriter writer, void *data) {
  int status;
  TObject *o;
  lua_lock(L);
  api_checknelems(L, 1);
  o = L->top - 1;
  if (isLfunction(o) && clvalue(o)->l.nupvalues == 0) {
    luaU_dump(L, clvalue(o)->l.p, writer, data);
    status = 1;
  }
  else
    status = 0;
  lua_unlock(L);
  return status;
}


/*
** Garbage-collection functions
*/

/* GC values are expressed in Kbytes: #bytes/2^10 */
#define GCscalel(x)		((x)>>10)
#define GCscale(x)		(cast(int, GCscalel(x)))
#define GCunscale(x)		(cast(lu_mem, x)<<10)

LUA_API int lua_getgcthreshold (lua_State *L) {
  int threshold;
  lua_lock(L);
  threshold = GCscale(G(L)->GCthreshold);
  lua_unlock(L);
  return threshold;
}

/* moho64: the serial the state's next table, userdata, function or thread takes (lobject.h), i.e. how many it has
** made. Only read (the two-copies fingerprint, src/sim/fingerprint.h). */
LUA_API unsigned int lua_getnextserial (lua_State *L) {
  return G(L)->nextserial;
}

/* moho64: the heap and the collector's threshold in bytes (lua_getgccount / lua_getgcthreshold give KB). Only
** read. */
LUA_API void lua_gpg_gcbytes (lua_State *L, size_t *inuse, size_t *threshold) {
  *inuse = cast(size_t, G(L)->nblocks);
  *threshold = cast(size_t, G(L)->GCthreshold);
}

/* moho64, for a check only (src/app/main.cpp MOHO64_GC_DUE): the collector's threshold set to the heap, so
** that a collection is due at the state's next check, without checking here (lua_setgcthreshold checks). */
LUA_API void lua_gpg_gcdue (lua_State *L) {
  if (G(L)->inc) {  /* the incremental collector: the cycle is finished at the next step */
    luaC_incdue(L);
    return;
  }
  G(L)->GCthreshold = G(L)->nblocks;
}

LUA_API int lua_getgccount (lua_State *L) {
  int count;
  lua_lock(L);
  count = GCscale(G(L)->nblocks);
  lua_unlock(L);
  return count;
}

LUA_API void lua_setgcthreshold (lua_State *L, int newthreshold) {
  lua_lock(L);
  if (G(L)->inc) {  /* moho64: collectgarbage() in an incremental state finishes the cycle now */
    luaC_incfull(L);
    lua_unlock(L);
    return;
  }
  if (cast(lu_mem, newthreshold) > GCscalel(MAX_LUMEM))
    G(L)->GCthreshold = MAX_LUMEM;
  else
    G(L)->GCthreshold = GCunscale(newthreshold);
  luaC_checkGC(L);
  lua_unlock(L);
}


/*
** miscellaneous functions
*/


LUA_API const char *lua_version (void) {
  return LUA_VERSION;
}


LUA_API int lua_error (lua_State *L) {
  lua_lock(L);
  api_checknelems(L, 1);
  luaG_errormsg(L);
  lua_unlock(L);
  return 0;  /* to avoid warnings */
}


LUA_API int lua_next (lua_State *L, int idx) {
  StkId t;
  int more;
  lua_lock(L);
  t = luaA_index(L, idx);
  api_check(L, ttistable(t));
  more = luaH_next(L, hvalue(t), L->top - 1);
  if (more) {
    api_incr_top(L);
  }
  else  /* no more elements */
    L->top -= 1;  /* remove key */
  lua_unlock(L);
  return more;
}


LUA_API void lua_concat (lua_State *L, int n) {
  lua_lock(L);
  luaC_checkGC(L);
  api_checknelems(L, n);
  if (n >= 2) {
    luaV_concat(L, n, L->top - L->base - 1);
    L->top -= (n-1);
  }
  else if (n == 0) {  /* push empty string */
    setsvalue2s(L->top, luaS_newlstr(L, NULL, 0));
    api_incr_top(L);
  }
  /* else n == 1; nothing to do */
  lua_unlock(L);
}


LUA_API void *lua_newuserdata (lua_State *L, size_t size) {
  Udata *u;
  lua_lock(L);
  luaC_checkGC(L);
  u = luaS_newudata(L, size);
  setuvalue(L->top, u);
  api_incr_top(L);
  lua_unlock(L);
  return u + 1;
}


LUA_API int lua_pushupvalues (lua_State *L) {
  Closure *func;
  int n, i;
  lua_lock(L);
  api_check(L, iscfunction(L->base - 1));
  func = clvalue(L->base - 1);
  n = func->c.nupvalues;
  luaD_checkstack(L, n + LUA_MINSTACK);
  for (i=0; i<n; i++) {
    setobj2s(L->top, &func->c.upvalue[i]);
    L->top++;
  }
  lua_unlock(L);
  return n;
}


static const char *aux_upvalue (lua_State *L, int funcindex, int n,
                                TObject **val) {
  Closure *f;
  StkId fi = luaA_index(L, funcindex);
  if (!ttisfunction(fi)) return NULL;
  f = clvalue(fi);
  if (f->c.isC) {
    if (n > f->c.nupvalues) return NULL;
    *val = &f->c.upvalue[n-1];
    return "";
  }
  else {
    Proto *p = f->l.p;
    if (n > p->sizeupvalues) return NULL;
    *val = f->l.upvals[n-1]->v;
    return getstr(p->upvalues[n-1]);
  }
}


LUA_API const char *lua_getupvalue (lua_State *L, int funcindex, int n) {
  const char *name;
  TObject *val;
  lua_lock(L);
  name = aux_upvalue(L, funcindex, n, &val);
  if (name) {
    setobj2s(L->top, val);
    api_incr_top(L);
  }
  lua_unlock(L);
  return name;
}


LUA_API const char *lua_setupvalue (lua_State *L, int funcindex, int n) {
  const char *name;
  TObject *val;
  lua_lock(L);
  api_checknelems(L, 1);
  name = aux_upvalue(L, funcindex, n, &val);
  if (name) {
    L->top--;
    setobj(val, L->top);  /* write barrier */
    luaC_barrierv(L, L->top);  /* (a C closure's slot or a Lua closure's UpVal: the value is marked) */
  }
  lua_unlock(L);
  return name;
}



/*
** GPG/LuaPlus: default metatables for the basic (non-table, non-userdata) types
*/
LUA_API void lua_getdefaultmetatable (lua_State *L, int type) {
  Table *mt;
  lua_lock(L);
  api_check(L, type >= 0 && type <= NUM_TAGS);
  (void)mt;
  setobj2s(L->top, &G(L)->defmeta[type]);
  api_incr_top(L);
  lua_unlock(L);
}

LUA_API void lua_setdefaultmetatable (lua_State *L, int type) {
  lua_lock(L);
  api_checknelems(L, 1);
  api_check(L, type >= 0 && type <= NUM_TAGS && type != LUA_TTABLE && type != LUA_TUSERDATA);
  setobj(&G(L)->defmeta[type], L->top - 1);
  L->top--;
  lua_unlock(L);
}


/* GPG/LuaPlus: debug.allocatedsize(v) - bytes allocated for a value, as the original reports
   them for a 32-bit build (table header 40 bytes, 8 per array slot, 20 per hash node; the
   shared empty node is not counted). Used by repr() in log output. */
LUA_API size_t lua_gpg_allocatedsize (lua_State *L, int idx) {
  StkId o = luaA_index(L, idx);
  switch (ttype(o)) {
    case LUA_TTABLE: {
      Table *h = hvalue(o);
      size_t n = 40 + 8 * (size_t)h->sizearray;
      if (h->node != G(L)->dummynode) n += 20 * ((size_t)1 << h->lsizenode);
      return n;
    }
    case LUA_TSTRING: return 16 + tsvalue(o)->tsv.len + 1;
    case LUA_TUSERDATA: return 16 + uvalue(o)->uv.len;
    default: return 0;
  }
}
