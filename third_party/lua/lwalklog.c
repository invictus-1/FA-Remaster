/*
** moho64: a debugging switch, MOHO64_DEBUG_OBJWALKS=<file> (docs/two-copies.md, "Copies with different focus
** armies"). It counts the walks over tables (next() from the start, as pairs() and `for k, v in t' begin) whose
** order or contents can differ between two copies of a game in lockstep, by the Lua line that starts them, and
** writes the counts to the file when the program ends:
**   - a table with a key that is a table, userdata, function or thread among two or more keys of its hash part:
**     such a key's place among the others goes by its serial (ltable.c, luaH_mainposition), and copies that look
**     through different armies make different numbers of objects, so their serials, and the order, may differ;
**   - a weak table (__mode k or v), marked "WEAK": what it still holds depends on when the collector last ran,
**     and the collector runs at other moments in copies whose heaps differ.
** Each line: walks, the most object keys seen in one walk, the place. Off (the default) it costs one test of a
** flag per walk; it changes nothing the scripts see.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define lwalklog_c

#include "lua.h"

#include "lobject.h"
#include "lstate.h"
#include "ltable.h"
#include "ltm.h"


typedef struct WalkSite {
  char where[200];
  unsigned long walks;
  int maxobj;
} WalkSite;

static WalkSite *sites = NULL;
static int nsites = 0, capsites = 0;
static int walklog_on = -1;  /* -1: not looked at yet */
static const char *walklog_file = NULL;


static void walklog_write (void) {
  FILE *f = fopen(walklog_file, "w");
  int i;
  if (f == NULL) return;
  fprintf(f, "# walks\tmost object keys\twhere (MOHO64_DEBUG_OBJWALKS)\n");
  for (i = 0; i < nsites; i++)
    fprintf(f, "%lu\t%d\t%s\n", sites[i].walks, sites[i].maxobj, sites[i].where);
  fclose(f);
}


static int isobjectkey (const TObject *k) {
  return ttistable(k) || ttype(k) == LUA_TUSERDATA || ttisfunction(k) || ttype(k) == LUA_TTHREAD;
}


void luaH_notewalk (lua_State *L, Table *t) {
  int i, nobj = 0, nall = 0, weak = 0, level;
  char where[200];
  lua_Debug ar;
  if (walklog_on < 0) {
    walklog_file = getenv("MOHO64_DEBUG_OBJWALKS");
    walklog_on = walklog_file != NULL && *walklog_file != '\0';
    if (walklog_on) atexit(walklog_write);
  }
  if (!walklog_on) return;
  if (t->metatable) {
    const TObject *mode = gfasttm(G(L), t->metatable, TM_MODE);
    weak = mode && ttisstring(mode) && (strchr(svalue(mode), 'k') || strchr(svalue(mode), 'v'));
  }
  for (i = 0; i < sizenode(t); i++) {
    if (ttisnil(gval(gnode(t, i)))) continue;
    nall++;
    if (isobjectkey(gkey(gnode(t, i)))) nobj++;
  }
  if (!(weak && (nall > 0 || t->sizearray > 0)) && (nobj < 1 || nall < 2)) return;
  /* the innermost Lua function on the stack (next() itself is a C function) */
  strcpy(where, "[C]");
  for (level = 0; level < 6 && lua_getstack(L, level, &ar); level++) {
    if (!lua_getinfo(L, "Sl", &ar)) break;
    if (ar.currentline >= 0) {
      sprintf(where, "%.180s:%d", ar.short_src, ar.currentline);
      break;
    }
  }
  if (weak) {
    char w[200];
    sprintf(w, "WEAK %.190s", where);
    strcpy(where, w);
  }
  for (i = 0; i < nsites; i++)
    if (strcmp(sites[i].where, where) == 0) break;
  if (i == nsites) {
    if (nsites == capsites) {
      WalkSite *more;
      capsites = capsites ? 2 * capsites : 64;
      more = (WalkSite *)realloc(sites, capsites * sizeof(WalkSite));
      if (more == NULL) return;
      sites = more;
    }
    memset(&sites[i], 0, sizeof(WalkSite));
    strcpy(sites[i].where, where);
    nsites++;
  }
  sites[i].walks++;
  if (nobj > sites[i].maxobj) sites[i].maxobj = nobj;
}
