/*
** moho64: the collector's mode, and the incremental collector.
** Copyright (C) 1994-2003 Tecgraf, PUC-Rio (Lua 5.0; MIT licence, see lua.h); the moho64 changes are under the
** same licence.
**
** A Lua state runs one of two collectors, chosen once, right after lua_open:
** - classic: Lua 5.0's stop-the-world mark and sweep (lgc.c), unchanged; the default;
** - incremental (this file): a tri-colour collector after the DESIGN of Lua 5.1's (by R. Ierusalimschy, L. H. de
**   Figueiredo, W. Celes; MIT licence): two whites, so that a sweep can stop part-way; gray lists; write barriers
**   (lgc.h); an atomic step that re-marks the roots, the threads and the tables written since they were traversed,
**   and clears the weak tables. What is NOT 5.1's: it is written for 5.0's objects (lgc.c's traversals and weak
**   clearing, kept as they are); it is stepped only at the end of a beat (lua_gpg_gcstep), never in the middle of a
**   script except in an emergency; and its budget and trigger count OBJECTS, never bytes or time, so that every
**   machine and platform collects at the same moments (gc_design.md).
**   From Lua 5.2's DESIGN (same authors and licence): tables written since their traversal are traversed again
**   before the atomic step, not only at it (regraytables). Ours, not Lua's: weak tables are listed in an array
**   (weakv) and are otherwise tables like any other (black, barriers), so that the atomic step only checks their mode
**   and clears them; and finalisers (__gc) are not run (the sim's scripts have none; the sweep reports one).
**
** The colours (the `marked' byte, incremental states only):
**   bit 0 / bit 1   white 0 / white 1 (the current white is the one new objects get; the other one, after an
**                   atomic step, means dead)
**   bit 2           black (traversed; neither white bit)
**   neither         gray (on a gray list)
**   bit 3           tables: weak keys; udata: finalized
**   bit 4           tables: weak values
**   bit 5           fixed (never collected: reserved words, metamethod names, the memory error message)
**   bit 6           tables: in this cycle's weak array
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define lgcinc_c

#include "lua.h"

#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"


#define WHITE0		(1<<0)
#define WHITE1		(1<<1)
#define WHITES		(WHITE0 | WHITE1)
#define BLACK		(1<<2)
#define KEYWEAK		(1<<3)
#define FINALIZED	(1<<3)
#define VALUEWEAK	(1<<4)
#define FIXED		(1<<5)
#define WEAKLISTED	(1<<6)  /* tables: in this cycle's weak array (cleared by the sweep) */
#define CLASSICFIX	(1<<4)  /* lstring.h luaS_fix in a classic state */

enum { GCSpause, GCSpropagate, GCSsweepstring, GCSsweep };

typedef struct GCInc {
  lu_byte state;
  lu_byte currentwhite;
  int sweepstr;           /* the next string bucket to sweep */
  GCObject **sweepgc;     /* where the sweep of rootgc stands */
  GCObject **sweepud;     /* ... and of rootudata */
  GCObject *gray;         /* gray tables, closures, threads and protos to traverse */
  GCObject *grayagain;    /* threads, and black tables written since (re-traversed at the atomic step) */
  GCObject **weakv;       /* the weak tables traversed in this cycle, once each (cleared at the atomic step); an
                          ** array, so that a weak table's gclist serves the gray lists as any table's does */
  size_t nweak, capweak;
  lu_mem nobjects;        /* collectable objects in the heap, garbage included (strings and open upvalues too) */
  lu_mem made;            /* objects made since the last step */
  lu_mem estimate;        /* objects alive at the last atomic step (those then counted, less those its sweep freed) */
  lu_mem prevestimate;    /* ... and at the one before (the `hard' limit counts from the larger) */
  lu_mem atatomic, freed; /* the count at the atomic step; what the sweep has freed since */
  lu_mem debt;            /* units a capped step left over */
  int pause, stepmul, minstep, maxstep, hard;  /* the tuning (gc_design.md 4); MOHO64_GC_* at the switch */
  lu_mem ceiling;         /* the emergency's absolute object count */
  int finishnow;          /* finish the cycle at the next step (collectgarbage, MOHO64_GC_DUE) */
  int loading;            /* no beat has run yet: while the scripts load, a full cycle each time the objects double */
  lu_mem rebacks;         /* tables a barrier turned gray again in this cycle (statistics) */
  int verify;             /* MOHO64_GC_VERIFY: check at every atomic step that nothing reachable is white */
  /* statistics (lua_gpg_gcstats) */
  lu_mem cycles, lastunits, maxunits, lastatomic, maxatomic, verifyfails, gcobjects;
  lu_mem lastregray, maxregray, lastrebacks, maxrebacks;  /* the atomic step's grayagain part: units, tables */
  int phases;             /* MOHO64_GC_PHASES: time and slots of each part of the atomic step, on stderr (statistics) */
  lu_mem visits;          /* table and stack slots looked at (statistics, MOHO64_GC_PHASES) */
} GCInc;

#define G_(L)		G(L)
#define INC(L)		(G(L)->inc)
#define otherwhite(g)	((g)->inc->currentwhite ^ WHITES)
#define iswhite(o)	((o)->gch.marked & WHITES)
#define isblack(o)	((o)->gch.marked & BLACK)
#define isdead(g,o)	(((o)->gch.marked & otherwhite(g) & WHITES) && !((o)->gch.marked & FIXED))
#define white2gray(o)	((o)->gch.marked &= cast(lu_byte, ~WHITES))
#define gray2black(o)	((o)->gch.marked |= BLACK)
#define black2gray(o)	((o)->gch.marked &= cast(lu_byte, ~BLACK))
#define makewhite(g,o) \
  ((o)->gch.marked = cast(lu_byte, ((o)->gch.marked & ~(WHITES | BLACK)) | (g)->inc->currentwhite))


/* ---- marking ---- */

static void reallymark (global_State *g, GCObject *o);

#define markobj(g,o)	{ if (iscollectable(o) && iswhite(gcvalue(o))) reallymark(g, gcvalue(o)); }
#define markgco(g,o)	{ if ((o) != NULL && iswhite(valtogco(o))) reallymark(g, valtogco(o)); }

static void reallymark (global_State *g, GCObject *o) {
  GCInc *inc = g->inc;
  white2gray(o);
  switch (o->gch.tt) {
    case LUA_TSTRING: gray2black(o); break;
    case LUA_TUSERDATA: {
      gray2black(o);
      markgco(g, gcotou(o)->uv.metatable);
      break;
    }
    case LUA_TUPVAL: {  /* a closed upvalue (an open one is not in the heap's lists and keeps its colour) */
      UpVal *uv = gcotouv(o);
      gray2black(o);
      markobj(g, uv->v);
      break;
    }
    case LUA_TFUNCTION: gcotocl(o)->c.gclist = inc->gray; inc->gray = o; break;
    case LUA_TTABLE: gcotoh(o)->gclist = inc->gray; inc->gray = o; break;
    case LUA_TTHREAD: gcototh(o)->gclist = inc->gray; inc->gray = o; break;
    case LUA_TPROTO: gcotop(o)->gclist = inc->gray; inc->gray = o; break;
    default: lua_assert(0);
  }
}

static void weakpush (GCInc *inc, GCObject *o) {
  if (inc->nweak == inc->capweak) {
    size_t cap = inc->capweak ? inc->capweak * 2 : 1024;
    GCObject **v = (GCObject **)realloc(inc->weakv, cap * sizeof(GCObject *));  /* (outside Lua's byte count) */
    if (v == NULL) { fprintf(stderr, "moho64 gc: out of memory for the weak tables\n"); abort(); }
    inc->weakv = v;
    inc->capweak = cap;
  }
  inc->weakv[inc->nweak++] = o;
}

/* a table: as lgc.c's traversetable (weak modes the same). A weak table is also entered in the weak array (once a
** cycle; cleared at the atomic step) and otherwise is a table like any other: it turns black, and a write turns it
** gray again (luaC_incbarriert), so that its strong part is traversed again before the atomic step, or at it. */
static lu_mem traversetable (global_State *g, Table *h) {
  GCInc *inc = g->inc;
  int i;
  int weakkey = 0, weakvalue = 0;
  const TObject *mode;
  markgco(g, h->metatable);
  mode = gfasttm(g, h->metatable, TM_MODE);
  h->marked &= cast(lu_byte, ~(KEYWEAK | VALUEWEAK));
  if (mode && ttisstring(mode)) {
    weakkey = (strchr(svalue(mode), 'k') != NULL);
    weakvalue = (strchr(svalue(mode), 'v') != NULL);
    if (weakkey || weakvalue) {
      h->marked |= cast(lu_byte, (weakkey ? KEYWEAK : 0) | (weakvalue ? VALUEWEAK : 0));
      if (!(h->marked & WEAKLISTED)) {
        h->marked |= WEAKLISTED;
        weakpush(inc, valtogco(h));
      }
    }
  }
  if (!weakvalue) {
    i = h->sizearray;
    while (i--) markobj(g, &h->array[i]);
  }
  i = sizenode(h);
  while (i--) {
    Node *n = gnode(h, i);
    if (!ttisnil(gval(n))) {
      if (!weakkey) markobj(g, gkey(n));
      if (!weakvalue) markobj(g, gval(n));
    }
  }
  gray2black(valtogco(h));
  inc->visits += (lu_mem)(h->sizearray + sizenode(h));
  return 1 + (lu_mem)(h->sizearray + sizenode(h)) / 16;
}

static lu_mem traverseclosure (global_State *g, Closure *cl) {
  int i;
  if (cl->c.isC) {
    for (i = 0; i < cl->c.nupvalues; i++) markobj(g, &cl->c.upvalue[i]);
  }
  else {
    markobj(g, &cl->l.g);
    markgco(g, cl->l.p);
    for (i = 0; i < cl->l.nupvalues; i++) {
      UpVal *uv = cl->l.upvals[i];
      if (uv->v == &uv->value) markgco(g, uv);  /* closed: in the heap */
      /* (open: its value is in a stack, marked with the thread) */
    }
  }
  gray2black(valtogco(cl));
  return 1 + cl->c.nupvalues / 8;
}

static lu_mem traverseproto (global_State *g, Proto *f) {
  int i;
  markgco(g, f->source);
  for (i = 0; i < f->sizek; i++) markobj(g, f->k + i);
  for (i = 0; i < f->sizeupvalues; i++) markgco(g, f->upvalues[i]);
  for (i = 0; i < f->sizep; i++) markgco(g, f->p[i]);
  for (i = 0; i < f->sizelocvars; i++) markgco(g, f->locvars[i].varname);
  gray2black(valtogco(f));
  return 1 + (lu_mem)(f->sizek + f->sizep + f->sizelocvars) / 16;
}

/* a thread: its globals and the live part of its stack. It stays gray (on grayagain): re-traversed at the atomic
** step, where lgc.c's clearing above the top and stack shrinking also happen (atomic != 0). */
static lu_mem traversethread (lua_State *L, global_State *g, lua_State *th, int atomic) {
  StkId o, lim;
  CallInfo *ci;
  markobj(g, gt(th));
  lim = th->top;
  for (ci = th->base_ci; ci <= th->ci; ci++)
    if (!(ci->state & CI_C) && lim < ci->top) lim = ci->top;
  for (o = th->stack; o < th->top; o++) markobj(g, o);
  g->inc->visits += (lu_mem)(lim - th->stack) + 1;
  if (atomic) {
    for (; o <= lim; o++) setnilvalue(o);
    {  /* lgc.c checkstacksizes */
      int used = th->ci - th->base_ci;
      if (4*used < th->size_ci && 2*BASIC_CI_SIZE < th->size_ci) luaD_reallocCI(th, th->size_ci/2);
      used = lim - th->stack;
      if (4*used < th->stacksize && 2*(BASIC_STACK_SIZE+EXTRA_STACK) < th->stacksize)
        luaD_reallocstack(th, th->stacksize/2);
    }
  }
  UNUSED(L);
  return 1 + (lu_mem)(th->top - th->stack) / 16;
}

/* traverse one gray object; returns its cost in units */
static lu_mem propagateone (lua_State *L, global_State *g, int atomic) {
  GCInc *inc = g->inc;
  GCObject *o = inc->gray;
  switch (o->gch.tt) {
    case LUA_TTABLE: inc->gray = gcotoh(o)->gclist; return traversetable(g, gcotoh(o));
    case LUA_TFUNCTION: inc->gray = gcotocl(o)->c.gclist; return traverseclosure(g, gcotocl(o));
    case LUA_TPROTO: inc->gray = gcotop(o)->gclist; return traverseproto(g, gcotop(o));
    case LUA_TTHREAD: {
      lua_State *th = gcototh(o);
      inc->gray = th->gclist;
      if (!atomic) {  /* stays gray: once more at the atomic step */
        th->gclist = inc->grayagain;
        inc->grayagain = o;
      }
      return traversethread(L, g, th, atomic);
    }
    default: lua_assert(0); return 1;
  }
}

static lu_mem propagateall (lua_State *L, global_State *g, int atomic) {
  lu_mem u = 0;
  while (g->inc->gray) u += propagateone(L, g, atomic);
  return u;
}

/* the roots' values. The main thread is no heap object (not in rootgc; black and fixed, so that no value that
** names it puts it on a list): it is traversed directly, at the start and again at the atomic step. */
static lu_mem markroots (lua_State *L, global_State *g, int atomic) {
  int t;
  markobj(g, defaultmeta(L));
  markobj(g, registry(L));
  for (t = 0; t <= NUM_TAGS; t++) markobj(g, &g->defmeta[t]);
  if (L != g->mainthread) markgco(g, L);  /* (an emergency inside a coroutine) */
  return traversethread(L, g, g->mainthread, atomic);
}


/* ---- weak tables (lgc.c's clearing, unchanged in effect) ---- */

static int valmarked (const TObject *o) {
  if (ttisstring(o)) {  /* strings are values, never weak: keep them */
    GCObject *s = gcvalue(o);
    if (iswhite(s)) { white2gray(s); gray2black(s); }
  }
  return !iscollectable(o) || !iswhite(gcvalue(o));
}

static void removekey (Node *n) {
  setnilvalue(gval(n));
  if (iscollectable(gkey(n))) setttype(gkey(n), LUA_TNONE);  /* dead key */
}

static void clearweak (GCInc *inc) {
  size_t k;
  for (k = 0; k < inc->nweak; k++) {
    Table *h = gcotoh(inc->weakv[k]);
    int i;
    if (!(h->marked & (KEYWEAK | VALUEWEAK))) continue;  /* (no longer weak: traversed again at the atomic step) */
    if (h->marked & VALUEWEAK) {
      i = h->sizearray;
      while (i--) {
        TObject *o = &h->array[i];
        if (!valmarked(o)) setnilvalue(o);
      }
    }
    i = sizenode(h);
    while (i--) {
      Node *n = gnode(h, i);
      if (ttisnil(gval(n))) continue;
      if (((h->marked & KEYWEAK) && !valmarked(gkey(n))) || ((h->marked & VALUEWEAK) && !valmarked(gval(n))))
        removekey(n);
    }
  }
  inc->nweak = 0;
}


/* ---- the barriers (lgc.h) ---- */

void luaC_incbarriert (lua_State *L, struct Table *t) {
  GCInc *inc = INC(L);
  if (inc->state == GCSpropagate && isblack(valtogco(t))) {
    inc->rebacks++;
    black2gray(valtogco(t));
    t->gclist = inc->grayagain;
    inc->grayagain = valtogco(t);
  }
}

void luaC_incbarrierf (lua_State *L, GCObject *o, GCObject *v) {
  global_State *g = G(L);
  if (g->inc->state == GCSpropagate && iswhite(v) && (o == NULL || isblack(o))) reallymark(g, v);
}

/* an upvalue closes (lfunc.c luaF_close) and enters the heap: while marking it is made black and its value marked
** (closures already traversed point at it); otherwise it gets the current white (a sweep frees the other one) */
void luaC_linkupval (lua_State *L, GCObject *o) {
  global_State *g = G(L);
  if (!g->inc) {
    luaC_link(L, o, LUA_TUPVAL);
    return;
  }
  luaC_link(L, o, LUA_TUPVAL);  /* the current white, counted */
  if (g->inc->state == GCSpropagate) reallymark(g, o);
}

/* the mark of a new object (classic: udata 1 = "not finalized" in lgc.c's sense) */
lu_byte luaC_newmark (lua_State *L, lu_byte udata) {
  GCInc *inc = INC(L);
  UNUSED(udata);
  inc->nobjects++;
  inc->made++;
  if (inc->nobjects >= inc->ceiling ||  /* the emergency, at the next luaC_checkGC */
      (inc->loading && inc->nobjects >= 2 * inc->estimate))  /* (loading: as classic's threshold, twice what is
                                                                ** alive, by count; never by `pause', which a check
                                                                ** may set to 100: then every new object would start
                                                                ** a cycle) */
    G(L)->GCthreshold = 0;
  return inc->currentwhite;
}


/* ---- sweeping ---- */

static void freeobj (lua_State *L, GCObject *o) {
  switch (o->gch.tt) {
    case LUA_TPROTO: luaF_freeproto(L, gcotop(o)); break;
    case LUA_TFUNCTION: luaF_freeclosure(L, gcotocl(o)); break;
    case LUA_TUPVAL: luaM_freelem(L, gcotouv(o)); break;
    case LUA_TTABLE: luaH_free(L, gcotoh(o)); break;
    case LUA_TTHREAD: luaE_freethread(L, gcototh(o)); break;
    case LUA_TSTRING: luaM_free(L, o, sizestring(gcotots(o)->tsv.len)); break;
    case LUA_TUSERDATA: luaM_free(L, o, sizeudata(gcotou(o)->uv.len)); break;
    default: lua_assert(0);
  }
}

/* sweep up to `count' objects of the list at *p; returns where it stopped (NULL at the end) */
static GCObject **sweeplist (lua_State *L, GCObject **p, lu_mem count, lu_mem *done) {
  global_State *g = G(L);
  GCObject *curr;
  while ((curr = *p) != NULL && *done < count) {
    (*done)++;
    if (isdead(g, curr)) {
      if (curr->gch.tt == LUA_TUSERDATA && !(curr->gch.marked & FINALIZED) &&
          fasttm(L, gcotou(curr)->uv.metatable, TM_GC) != NULL)  /* (its metatable: swept later, with rootgc) */
        fprintf(stderr, "moho64 gc: a userdata with __gc: its finaliser is not run by the incremental collector\n");
      *p = curr->gch.next;
      g->inc->nobjects--;
      g->inc->freed++;
      freeobj(L, curr);
    }
    else {
      if (curr->gch.tt == LUA_TTABLE) curr->gch.marked &= cast(lu_byte, ~(KEYWEAK | VALUEWEAK | WEAKLISTED));
      makewhite(g, curr);
      p = &curr->gch.next;
    }
  }
  return curr == NULL ? NULL : p;
}

static void checksizes (lua_State *L) {  /* lgc.c checkSizes, without the threshold */
  global_State *g = G(L);
  if (g->strt.nuse < cast(ls_nstr, g->strt.size/4) && g->strt.size > MINSTRTABSIZE*2)
    luaS_resize(L, g->strt.size/2);
  if (luaZ_sizebuffer(&g->buff) > LUA_MINBUFFER*2)
    luaZ_resizebuffer(L, &g->buff, luaZ_sizebuffer(&g->buff) / 2);
}


/* ---- the verification (MOHO64_GC_VERIFY): a side walk from the roots by lgc.c's rules ---- */

typedef struct PtrSet { GCObject **slot; size_t size, n; } PtrSet;

static int ps_add (PtrSet *s, GCObject *o) {  /* 1 if new */
  size_t i;
  if ((s->n + 1) * 2 > s->size) {
    PtrSet t;
    size_t j;
    t.size = s->size ? s->size * 2 : 1u << 16;
    t.n = 0;
    t.slot = (GCObject **)calloc(t.size, sizeof(GCObject *));
    for (j = 0; j < s->size; j++) if (s->slot[j]) ps_add(&t, s->slot[j]);
    free(s->slot);
    *s = t;
  }
  i = ((size_t)o >> 4) * 2654435761u % s->size;
  while (s->slot[i]) {
    if (s->slot[i] == o) return 0;
    i = (i + 1) % s->size;
  }
  s->slot[i] = o;
  s->n++;
  return 1;
}

typedef struct Verify { PtrSet seen; GCObject **stack; size_t n, cap; lu_mem bad; } Verify;

static void v_push (global_State *g, Verify *v, GCObject *o) {
  if (o == NULL || !ps_add(&v->seen, o)) return;
  if (iswhite(o) && !(o->gch.marked & FIXED)) {
    if (v->bad < 8)
      fprintf(stderr, "moho64 gc verify: a reachable object is white (type %d, marked 0x%x)\n", o->gch.tt, o->gch.marked);
    v->bad++;
  }
  if (v->n == v->cap) {
    v->cap = v->cap ? v->cap * 2 : 1u << 16;
    v->stack = (GCObject **)realloc(v->stack, v->cap * sizeof(GCObject *));
  }
  v->stack[v->n++] = o;
}
#define v_pushv(g,v,o)	{ if (iscollectable(o)) v_push(g, v, gcvalue(o)); }

static lu_mem verify (lua_State *L, global_State *g) {
  Verify v;
  int t;
  memset(&v, 0, sizeof v);
  v_pushv(g, &v, defaultmeta(L));
  v_pushv(g, &v, registry(L));
  for (t = 0; t <= NUM_TAGS; t++) v_pushv(g, &v, &g->defmeta[t]);
  v_push(g, &v, valtogco(g->mainthread));
  while (v.n) {
    GCObject *o = v.stack[--v.n];
    int i;
    switch (o->gch.tt) {
      case LUA_TUSERDATA: v_push(g, &v, valtogco(gcotou(o)->uv.metatable)); break;
      case LUA_TUPVAL: v_pushv(g, &v, gcotouv(o)->v); break;
      case LUA_TTABLE: {
        Table *h = gcotoh(o);
        const TObject *mode;
        int wk = 0, wv = 0;
        if (h->metatable) v_push(g, &v, valtogco(h->metatable));
        mode = gfasttm(g, h->metatable, TM_MODE);
        if (mode && ttisstring(mode)) {
          wk = strchr(svalue(mode), 'k') != NULL;
          wv = strchr(svalue(mode), 'v') != NULL;
        }
        if (!wv) for (i = 0; i < h->sizearray; i++) v_pushv(g, &v, &h->array[i]);
        for (i = 0; i < sizenode(h); i++) {
          Node *n = gnode(h, i);
          if (ttisnil(gval(n))) continue;
          if (!wk || ttisstring(gkey(n))) v_pushv(g, &v, gkey(n));
          if (!wv || ttisstring(gval(n))) v_pushv(g, &v, gval(n));
        }
        break;
      }
      case LUA_TFUNCTION: {
        Closure *cl = gcotocl(o);
        if (cl->c.isC) { for (i = 0; i < cl->c.nupvalues; i++) v_pushv(g, &v, &cl->c.upvalue[i]); }
        else {
          v_pushv(g, &v, &cl->l.g);
          v_push(g, &v, valtogco(cl->l.p));
          for (i = 0; i < cl->l.nupvalues; i++) {
            UpVal *uv = cl->l.upvals[i];
            if (uv->v == &uv->value) v_push(g, &v, valtogco(uv));
          }
        }
        break;
      }
      case LUA_TTHREAD: {
        lua_State *th = gcototh(o);
        StkId s;
        v_pushv(g, &v, gt(th));
        for (s = th->stack; s < th->top; s++) v_pushv(g, &v, s);
        break;
      }
      case LUA_TPROTO: {
        Proto *f = gcotop(o);
        if (f->source) v_push(g, &v, valtogco(f->source));
        for (i = 0; i < f->sizek; i++) v_pushv(g, &v, f->k + i);
        for (i = 0; i < f->sizeupvalues; i++) if (f->upvalues[i]) v_push(g, &v, valtogco(f->upvalues[i]));
        for (i = 0; i < f->sizep; i++) v_push(g, &v, valtogco(f->p[i]));
        for (i = 0; i < f->sizelocvars; i++) if (f->locvars[i].varname) v_push(g, &v, valtogco(f->locvars[i].varname));
        break;
      }
      default: break;
    }
  }
  free(v.seen.slot);
  free(v.stack);
  return v.bad;
}


/* ---- the cycle ---- */

/* a table's weak mode now, as traversetable reads it (KEYWEAK | VALUEWEAK) */
static int weakmode (global_State *g, Table *h) {
  const TObject *mode = gfasttm(g, h->metatable, TM_MODE);
  int m = 0;
  if (mode && ttisstring(mode)) {
    if (strchr(svalue(mode), 'k') != NULL) m |= KEYWEAK;
    if (strchr(svalue(mode), 'v') != NULL) m |= VALUEWEAK;
  }
  return m;
}

static void startcycle (lua_State *L, global_State *g) {
  GCInc *inc = g->inc;
  inc->gray = inc->grayagain = NULL;
  inc->nweak = 0;
  markroots(L, g, 0);
  inc->state = GCSpropagate;
}

/* MOHO64_GC_PHASES (statistics only): wall time in microseconds, units and slots looked at, per part */
#define NPH 8
typedef struct Phases { double t0; double us[NPH]; lu_mem vis[NPH], un[NPH], n[NPH]; int k; } Phases;
static double nowus (void) {
  struct timespec ts;
  timespec_get(&ts, TIME_UTC);
  return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}
static void ph_begin (GCInc *inc, Phases *p) {
  memset(p, 0, sizeof *p);
  if (inc->phases) { inc->visits = 0; p->t0 = nowus(); }
}
static void ph_mark (GCInc *inc, Phases *p, lu_mem units, lu_mem n) {
  if (inc->phases && p->k < NPH - 1) {
    double t = nowus();
    p->us[p->k] = t - p->t0; p->vis[p->k] = inc->visits; p->un[p->k] = units; p->n[p->k] = n;
    p->t0 = t; inc->visits = 0; p->k++;
  }
}

/* The atomic step: what is still gray, the roots, then grayagain (the threads, and the tables written since the last
** pre-pass); the weak tables whose mode changed are traversed again; then lgc.c's clearing of the weak tables (udata
** with a __gc: none in the sim's scripts, reported by the sweep); then the white flips and the sweep begins.
** B1 (Seton's, M28 armies; 2.6 M objects, 56 k weak tables): ~7 k units, ~10 ms, of it the clearing ~6 ms. */
static lu_mem atomic (lua_State *L, global_State *g) {
  GCInc *inc = g->inc;
  lu_mem u = 0, pu;
  Phases ph;
  ph_begin(inc, &ph);
  pu = propagateall(L, g, 1);  /* (an emergency: what is still gray) */
  u += pu;
  ph_mark(inc, &ph, pu, 0);
  pu = markroots(L, g, 1);
  pu += propagateall(L, g, 1);
  u += pu;
  ph_mark(inc, &ph, pu, 0);
  if (inc->phases) {  /* (statistics: the threads and the tables on grayagain) */
    GCObject *o = inc->grayagain;
    while (o) {
      if (o->gch.tt == LUA_TTABLE) { ph.vis[NPH-1]++; o = gcotoh(o)->gclist; }
      else { ph.n[NPH-1]++; o = gcototh(o)->gclist; }
    }
  }
  inc->gray = inc->grayagain;  /* threads, and tables written since they were traversed */
  inc->grayagain = NULL;
  {
    lu_mem r = propagateall(L, g, 1);
    u += r;
    inc->lastregray = r;
    if (r > inc->maxregray) inc->maxregray = r;
    inc->lastrebacks = inc->rebacks;
    if (inc->rebacks > inc->maxrebacks) inc->maxrebacks = inc->rebacks;
    inc->rebacks = 0;
    ph_mark(inc, &ph, r, 0);
  }
  {  /* weak tables: those written since their traversal were traversed again with the gray tables above; one
     ** whose mode changed since (a new metatable, or a __mode written) is traversed again now, its mode read again */
    size_t k;
    Table *lastmt = NULL;
    int lastm = -1;
    for (k = 0; k < inc->nweak; k++) {
      Table *h = gcotoh(inc->weakv[k]);
      int m;
      if (h->metatable == lastmt && lastm >= 0) m = lastm;  /* (most share a few metatables, unchanged meanwhile) */
      else { m = weakmode(g, h); lastmt = h->metatable; lastm = m; }
      if (m != (h->marked & (KEYWEAK | VALUEWEAK))) {
        black2gray(valtogco(h));
        h->gclist = inc->gray;
        inc->gray = valtogco(h);
        u += propagateall(L, g, 1);
      }
    }
    ph_mark(inc, &ph, 0, inc->nweak);
  }
  /* (udata with a __gc: none in the sim's scripts; the sweep reports one, see sweeplist) */
  ph_mark(inc, &ph, 0, 0);
  if (inc->phases) {  /* (statistics: the slots clearweak looks at) */
    size_t k;
    for (k = 0; k < inc->nweak; k++)
      inc->visits += (lu_mem)(gcotoh(inc->weakv[k])->sizearray + sizenode(gcotoh(inc->weakv[k])));
  }
  clearweak(inc);
  ph_mark(inc, &ph, 0, 0);
  /* (the check: after the clearing, which marks the strings a weak table keeps; before the flip) */
  if (inc->verify) {
    lu_mem bad = verify(L, g);
    inc->verifyfails += bad;
    if (bad) fprintf(stderr, "moho64 gc verify: cycle %lu: %lu reachable objects white\n",
                     (unsigned long)inc->cycles, (unsigned long)bad);
  }
  inc->atatomic = inc->nobjects;
  inc->freed = 0;
  inc->currentwhite = cast(lu_byte, otherwhite(g));  /* the flip: what is still the old white is dead */
  inc->sweepstr = 0;
  inc->sweepgc = &g->rootgc;
  inc->sweepud = &g->rootudata;
  inc->state = GCSsweepstring;
  if (inc->phases)
    fprintf(stderr, "moho64 gc phases: cycle %lu objects %lu units %lu threads %lu written-tables %lu |"
            " left %.0fus %lu/%lu | roots %.0fus %lu/%lu | grayagain %.0fus %lu/%lu | weak(%lu) %.0fus %lu/%lu |"
            " udata %.0fus | clear %.0fus %lu\n",
            (unsigned long)inc->cycles, (unsigned long)inc->nobjects, (unsigned long)u, (unsigned long)ph.n[NPH-1],
            (unsigned long)ph.vis[NPH-1],
            ph.us[0], (unsigned long)ph.un[0], (unsigned long)ph.vis[0],
            ph.us[1], (unsigned long)ph.un[1], (unsigned long)ph.vis[1],
            ph.us[2], (unsigned long)ph.un[2], (unsigned long)ph.vis[2],
            (unsigned long)ph.n[3], ph.us[3], (unsigned long)ph.un[3], (unsigned long)ph.vis[3],
            ph.us[4], ph.us[5], (unsigned long)ph.vis[5]);
  return u;
}

/* The tables a barrier turned gray again go back on the gray list now, not at the atomic step (Lua 5.2's
** design re-traverses them early too): then the atomic step re-traverses only what was written in its own beat. A
** script AI writes the same big tables every beat (M28's maps and zone tables: B1, up to 170 k of the atomic step's
** 210 k units). Threads stay on grayagain: their stacks are re-traversed at the atomic step in any case.
** Returns the number of tables moved. */
static int regraytables (GCInc *inc) {
  GCObject *o = inc->grayagain, *threads = NULL;
  int n = 0;
  while (o) {
    GCObject *next;
    if (o->gch.tt == LUA_TTABLE) {
      next = gcotoh(o)->gclist;
      gcotoh(o)->gclist = inc->gray;
      inc->gray = o;
      n++;
    }
    else {
      lua_State *th = gcototh(o);
      next = th->gclist;
      th->gclist = threads;
      threads = o;
    }
    o = next;
  }
  inc->grayagain = threads;
  return n;
}

/* one piece of work; returns its units (0: the marking is done and waits for an atomic point) */
static lu_mem singlestep (lua_State *L, global_State *g, lu_mem budget, int point) {
  GCInc *inc = g->inc;
  switch (inc->state) {
    case GCSpropagate: {
      if (inc->gray) return propagateone(L, g, 0);
      if (regraytables(inc)) return 1;  /* (then traversed by the next steps) */
      if (!point) return 0;  /* the atomic step only on the original's collection beats (tick % 70 == 0) */
      {
        lu_mem u = atomic(L, g);
        inc->lastatomic = u;
        if (u > inc->maxatomic) inc->maxatomic = u;
        return u;
      }
    }
    case GCSsweepstring: {
      lu_mem done = 0;
      while (inc->sweepstr < g->strt.size && done < budget) {
        GCObject **p = &g->strt.hash[inc->sweepstr];
        lu_mem before = inc->nobjects;
        lu_mem n = 0;
        while (sweeplist(L, p, (lu_mem)-1, &n) != NULL) {}
        g->strt.nuse -= cast(ls_nstr, before - inc->nobjects);
        done += 1 + n;
        inc->sweepstr++;
      }
      if (inc->sweepstr >= g->strt.size) inc->state = GCSsweep;
      return done ? done : 1;
    }
    case GCSsweep: {
      lu_mem done = 0;
      if (inc->sweepud) inc->sweepud = sweeplist(L, inc->sweepud, budget, &done);
      if (!inc->sweepud && inc->sweepgc) inc->sweepgc = sweeplist(L, inc->sweepgc, budget, &done);
      if (!inc->sweepud && !inc->sweepgc) {  /* the cycle is over */
        checksizes(L);
        /* (not the count now: what was made during the sweep is not known to be alive, and counting it would raise
        ** every later trigger, as Lua 5.1's estimate also leaves it out) */
        inc->prevestimate = inc->estimate;
        inc->estimate = inc->atatomic - inc->freed;
        inc->cycles++;
        inc->state = GCSpause;
      }
      return done ? done : 1;
    }
    default: return 0;
  }
}

static lu_mem finishcycle (lua_State *L, global_State *g) {
  lu_mem u = 0;
  if (g->inc->state == GCSpause) startcycle(L, g);
  while (g->inc->state != GCSpause) u += singlestep(L, g, (lu_mem)-1, 1);
  return u;
}


/* ---- entry points ---- */

extern void (*lua_gpg_prof_gc)(lua_State *L, int end);  /* lapi.c (MOHO64_PROFILE_HITCH only) */

static int envint (const char *name, int def) {
  const char *s = getenv(name);
  return s && *s ? atoi(s) : def;
}

LUA_API int lua_gpg_setgcmode (lua_State *L, int mode) {
  global_State *g = G(L);
  GCInc *inc;
  GCObject *o;
  int i;
  if (mode == 0) return g->inc == NULL;
  if (g->inc) return 1;
  inc = (GCInc *)calloc(1, sizeof(GCInc));
  if (!inc) return 0;
  inc->currentwhite = WHITE0;
  /* pause 300 / hard 600 (were 200 / 400): Chris's game 2 (27817 ticks, M28 variant, fixes on), both at once on a
  ** quiet PC (2026-10-28, pure C++ phases equal to 0.01 s), state identical over 29036 beats: collector cycles by
  ** tick 25200 45 vs 72, mean 0.94 vs 1.34 ms a beat, destroy+sync 38.4 vs 51.4 s (about -0.47 ms a tick), beats over
  ** 5 / 10 / 20 / 50 ms 716 / 42 / 2 / 0 vs 1357 / 55 / 0 / 0, longest beat 28.6 vs 19.9 ms, no step past `hard' in
  ** either. The price: peak working set 2290 vs 1954 MB (+340 MB). */
  inc->pause = envint("MOHO64_GC_PAUSE", 300);
  inc->stepmul = envint("MOHO64_GC_STEPMUL", 1000);  /* (B1 at 9000: 200 left the heap at 699 MB, 1000 at 325 MB) */
  inc->minstep = envint("MOHO64_GC_MINSTEP", 2000);
  inc->maxstep = envint("MOHO64_GC_MAXSTEP", 60000);  /* (B1: a marking unit ~0.13 us, a sweep unit ~0.08 us) */
  inc->hard = envint("MOHO64_GC_HARD", 600);  /* (twice the pause, as 400 was to 200) */
  inc->ceiling = (lu_mem)envint("MOHO64_GC_CEILING", 64 << 20);
  inc->verify = envint("MOHO64_GC_VERIFY", 0);
  inc->phases = envint("MOHO64_GC_PHASES", 0);
  g->inc = inc;
  /* every object so far (made by lua_open and the libraries) takes the current white; lgc.c's fixed strings stay
  ** fixed; udata keep lgc.c's "finalized" (its bit 1 clear) */
  for (o = g->rootgc; o; o = o->gch.next) { o->gch.marked = WHITE0; inc->nobjects++; }
  for (o = g->rootudata; o; o = o->gch.next) {
    o->gch.marked = cast(lu_byte, WHITE0 | ((o->gch.marked & (1<<1)) ? 0 : FINALIZED));
    inc->nobjects++;
  }
  for (i = 0; i < g->strt.size; i++)
    for (o = g->strt.hash[i]; o; o = o->gch.next) {
      o->gch.marked = cast(lu_byte, WHITE0 | ((o->gch.marked & CLASSICFIX) ? FIXED : 0));
      inc->nobjects++;
    }
  g->mainthread->marked = cast(lu_byte, BLACK | FIXED);
  inc->estimate = inc->prevestimate = inc->nobjects;
  inc->loading = 1;
  inc->state = GCSpause;
  g->GCthreshold = MAX_LUMEM;  /* luaC_checkGC never fires but for the emergency (luaC_newmark) */
  return 1;
}

/* The step at the end of a beat (where the original's AdvanceBeat step 19 collects, every 70 ticks).
** - A cycle starts on any beat once the objects reach `pause' % of those alive after the last cycle.
** - Marking proceeds by the budget: `stepmul' % of the objects made since the last step, at least `minstep', at most
**   `maxstep' (the rest carried as debt).
** - The atomic step runs only on a point beat (tick % 70 == 0), the first one after the marking is done: weak entries
**   then vanish only on beats where the original also collected. The sweep follows over the next beats.
** - The step that runs the atomic step ends with it (the sweep starts at the next beat).
** - Past `hard' % (of the larger of the last two estimates): the marking is behind; the budget is 4 x `maxstep' until
**   the cycle ends (no cycle at once: B1's one mid-game such cycle cost 104 ms).
** - Asked to finish (collectgarbage, MOHO64_GC_DUE): the marking is finished now, and the cycle is completed at the
**   next point beat. Only the memory emergency (luaC_incemergency) completes a cycle elsewhere. */
LUA_API void lua_gpg_gcstep (lua_State *L, int point) {
  global_State *g = G(L);
  GCInc *inc = g->inc;
  lu_mem want, budget, done = 0, hardbase;
  int worked = 0, urgent, behind, st0 = 0;
  lu_mem cyc0 = 0;
  double t0 = 0;
  if (inc == NULL) {  /* classic: the original's AdvanceBeat step 19, a full collection every 70 ticks */
    if (point) lua_setgcthreshold(L, 0);
    return;
  }
  inc->loading = 0;
  want = inc->made * (lu_mem)inc->stepmul / 100;
  if (want < (lu_mem)inc->minstep) want = (lu_mem)inc->minstep;
  want += inc->debt;
  budget = want > (lu_mem)inc->maxstep ? (lu_mem)inc->maxstep : want;
  inc->debt = want - budget;
  inc->made = 0;
  if (inc->state == GCSpause) {
    if (!inc->finishnow && inc->nobjects < inc->estimate / 100 * (lu_mem)inc->pause) {
      inc->debt = 0;
      inc->lastunits = 0;
      return;
    }
    startcycle(L, g);
  }
  if (lua_gpg_prof_gc) lua_gpg_prof_gc(L, 0);
  worked = 1;
  st0 = inc->state;
  cyc0 = inc->cycles;
  if (inc->phases) t0 = nowus();
  hardbase = inc->estimate > inc->prevestimate ? inc->estimate : inc->prevestimate;
  urgent = inc->finishnow;
  behind = !urgent && inc->nobjects >= hardbase / 100 * (lu_mem)inc->hard;
  if (urgent && point) {
    done = finishcycle(L, g);  /* the rest of the cycle now */
    inc->finishnow = 0;
    inc->debt = 0;
  }
  else {
    if (urgent) budget = (lu_mem)-1;  /* the marking now; the atomic step at the next point */
    else if (behind) budget = 4 * (lu_mem)inc->maxstep;
    while (done < budget && inc->state != GCSpause) {
      int before = inc->state;
      lu_mem u = singlestep(L, g, budget - done, point);
      if (u == 0) break;  /* marked; waiting for a point beat */
      done += u;
      if (before == GCSpropagate && inc->state != GCSpropagate) break;  /* the atomic step: enough for one beat */
    }
    if (inc->state == GCSpause || urgent || behind) inc->debt = 0;
  }
  inc->lastunits = done;
  if (done > inc->maxunits) inc->maxunits = done;
  if (inc->phases) {  /* (statistics: a step over 5 ms) */
    double us = nowus() - t0;
    if (us > 5000)
      fprintf(stderr, "moho64 gc step: %.0fus units %lu budget %lu state %d->%d point %d urgent %d cycle-ended %d\n",
              us, (unsigned long)done, (unsigned long)budget, st0, inc->state, point, urgent ? 1 : behind ? 2 : 0,
              inc->cycles != cyc0);
  }
  if (worked && lua_gpg_prof_gc) lua_gpg_prof_gc(L, 1);
}

/* lua_setgcthreshold (collectgarbage) and lua_gpg_gcdue in an incremental state: the cycle is finished now / at the
** next step */
void luaC_incfull (lua_State *L) {
  finishcycle(L, G(L));
  G(L)->GCthreshold = MAX_LUMEM;
}

void luaC_incdue (lua_State *L) { INC(L)->finishnow = 1; }

/* moho64: whether `objects' more new objects leave luaC_newmark's emergency off (it sets the threshold to 0 at the
** object that reaches the ceiling, or, while loading, twice the estimate): lapi.c lua_gpg_nocollect */
int luaC_incroom (lua_State *L, lu_mem objects) {
  GCInc *inc = INC(L);
  return inc->nobjects + objects < inc->ceiling && !(inc->loading && inc->nobjects + objects >= 2 * inc->estimate);
}

/* luaC_checkGC fired in an incremental state: the emergency (the object ceiling, luaC_newmark) */
void luaC_incemergency (lua_State *L) {
  global_State *g = G(L);
  if (lua_gpg_prof_gc) lua_gpg_prof_gc(L, 0);
  finishcycle(L, g);
  g->GCthreshold = MAX_LUMEM;
  if (g->inc->nobjects >= g->inc->ceiling) g->inc->ceiling = g->inc->nobjects * 2;  /* (not again at once) */
  if (lua_gpg_prof_gc) lua_gpg_prof_gc(L, 1);
}

/* lua_close: back to lgc.c's marks, so that its closing code (separate udata, call __gc, sweep all) runs as ever */
void luaC_incleave (lua_State *L) {
  global_State *g = G(L);
  GCObject *o;
  int i;
  if (!g->inc) return;
  for (o = g->rootgc; o; o = o->gch.next) o->gch.marked = 0;
  for (o = g->rootudata; o; o = o->gch.next) o->gch.marked = cast(lu_byte, (o->gch.marked & FINALIZED) ? 0 : (1<<1));
  for (i = 0; i < g->strt.size; i++)
    for (o = g->strt.hash[i]; o; o = o->gch.next) o->gch.marked = cast(lu_byte, (o->gch.marked & FIXED) ? CLASSICFIX : 0);
  g->mainthread->marked = 0;
  free(g->inc->weakv);
  free(g->inc);
  g->inc = NULL;
}

void luaC_incfree (lua_State *L) {
  if (G(L)->inc) { free(G(L)->inc->weakv); free(G(L)->inc); }
  G(L)->inc = NULL;
}

/* a string found in the string table while dead (swept later this cycle) is alive again (lstring.c) */
void luaC_increvive (lua_State *L, GCObject *o) {
  global_State *g = G(L);
  if (isdead(g, o)) makewhite(g, o);
}

int luaC_incsweepingstrings (lua_State *L) { return G(L)->inc && G(L)->inc->state == GCSsweepstring; }

LUA_API void lua_gpg_gcstats (lua_State *L, lua_GCStats *s) {
  GCInc *inc = G(L)->inc;
  memset(s, 0, sizeof *s);
  if (!inc) return;
  s->incremental = 1;
  s->state = inc->state;
  s->objects = inc->nobjects;
  s->estimate = inc->estimate;
  s->cycles = inc->cycles;
  s->lastunits = inc->lastunits;
  s->maxunits = inc->maxunits;
  s->lastatomic = inc->lastatomic;
  s->maxatomic = inc->maxatomic;
  s->verifyfails = inc->verifyfails;
  s->lastregray = inc->lastregray;
  s->maxregray = inc->maxregray;
  s->lastrebacks = inc->lastrebacks;
  s->maxrebacks = inc->maxrebacks;
}
