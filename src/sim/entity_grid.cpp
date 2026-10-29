// The entity grid (engine-ref rect_queries.md); see entity_grid.h.
#include "sim/entity_grid.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "sim/collision.h"
#include "sim/sim.h"
#include "sim/terrain.h"
#include "sim/units.h"

namespace moho {

namespace {
int ListOf(const Entity* e) {
  if (e->kind == Entity::Kind::Unit) return 0;
  if (e->kind == Entity::Kind::Prop) return 1;
  if (e->kind == Entity::Kind::Blip) return -1;  // (a recon blip has no primitive of its own here)
  return 2;  // shells (0x400), shields, beams and script entities (0x800): the third list
}
}  // namespace

void EntityGrid::Init(int mapW, int mapH) {
  w_ = std::max(1, mapW >> 2);
  h_ = std::max(1, mapH >> 2);
  shift_ = 0;
  while ((2 << shift_) <= w_) ++shift_;  // (bsr)
  mask_ = static_cast<uint32_t>(w_) * static_cast<uint32_t>(h_) - 1u;
  for (auto& l : lists_) l.assign(static_cast<size_t>(w_) * h_, {});
  for (uint64_t& v : version_) ++v;
}

void EntityGrid::Remove(Entity* e) {
  int k = ListOf(e);
  if (k < 0 || !e->gridW || !e->gridH) {
    e->gridW = e->gridH = 0;
    return;
  }
  ++version_[k];
  for (int z = e->gridZ0; z < e->gridZ0 + e->gridH; ++z)
    for (int x = e->gridX0; x < e->gridX0 + e->gridW; ++x) {
      auto& c = lists_[k][Cell(x, z)];
      auto it = std::find(c.begin(), c.end(), e);
      if (it != c.end()) c.erase(it);
    }
  e->gridW = e->gridH = 0;
}

void EntityGrid::Place(Entity* e, int cx0, int cz0, int w, int h) {
  int k = ListOf(e);
  if (k < 0 || !w_) return;
  if (w <= 0 || h <= 0) cx0 = cz0 = w = h = 0;
  if (cx0 == e->gridX0 && cz0 == e->gridZ0 && w == e->gridW && h == e->gridH) return;
  Remove(e);
  ++version_[k];
  e->gridX0 = static_cast<uint16_t>(cx0);
  e->gridZ0 = static_cast<uint16_t>(cz0);
  e->gridW = static_cast<uint16_t>(w);
  e->gridH = static_cast<uint16_t>(h);
  // (every cell of the span, through the mask: an entity off the map, an aircraft past its edge, is in the lists of
  // the cells its index aliases. Seen (probe mp1_7224): MP1 7223, army 3's spy plane 0x300090 (a SizeSphere of 2.7)
  // flies at x 1033 over the 1024 map; army 0's Coronas' shells cross its sphere and the original's line march finds
  // it, both hit and it dies at 7224. Ours left it out of every cell and the shells flew through it.)
  for (int z = cz0; z < cz0 + h; ++z)
    for (int x = cx0; x < cx0 + w; ++x) lists_[k][Cell(x, z)].push_back(e);
}

void EntityGrid::Gather(float x0, float z0, float x1, float z1, int mask, std::vector<Entity*>* out) {
  out->clear();
  if (!w_) return;
  // Rect2fToInt16: truncation, the far end rounded up to whole cells, at least one cell
  auto span = [](float a0, float a1, int* c0, int* n) {
    int t0 = static_cast<int>(a0), t1 = static_cast<int>(a1);
    *c0 = std::clamp(t0 >> 2, 0, 0xffff);
    *n = std::clamp(((t1 + 3) >> 2) - *c0, 1, 0xffff - *c0);
  };
  int cx0, cz0, w, h;
  span(x0, x1, &cx0, &w);
  span(z0, z1, &cz0, &h);
  int cols = std::min(w, w_ - cx0), rows = std::min(h, h_ - cz0);
  for (int r = 0; r < rows; ++r)
    for (int c = 0; c < cols; ++c) {
      size_t idx = static_cast<size_t>(cz0 + r) * w_ + (cx0 + c);
      for (int k = 0; k < 2; ++k) {
        if (!(mask & (1 << k))) continue;
        const auto& l = lists_[k][idx];
        for (auto it = l.rbegin(); it != l.rend(); ++it)
          if (!(*it)->gridMark) {
            (*it)->gridMark = true;
            out->push_back(*it);
          }
      }
    }
  for (Entity* e : *out) e->gridMark = false;
}

void EntityGrid::GatherBox(float x0, float z0, float x1, float z1, int mask, std::vector<Entity*>* out) {
  out->clear();
  if (!w_) return;
  auto span = [](float lo, float hi, int* c0, int* n) {
    int f = static_cast<int>(std::floor(lo)), c = static_cast<int>(std::ceil(hi));
    *c0 = std::clamp(f >> 2, 0, 0xffff);
    *n = std::clamp(((c + 3) >> 2) - *c0, 0, 0xffff - *c0);
  };
  int cx0, cz0, w, h;
  span(x0, x1, &cx0, &w);
  span(z0, z1, &cz0, &h);
  int cols = std::min(w, w_ - cx0), rows = std::min(h, h_ - cz0);
  for (int r = 0; r < rows; ++r)
    for (int c = 0; c < cols; ++c) {
      size_t idx = static_cast<size_t>(cz0 + r) * w_ + (cx0 + c);
      for (int k = 0; k < 2; ++k) {
        if (!(mask & (1 << k))) continue;
        const auto& l = lists_[k][idx];
        for (auto it = l.rbegin(); it != l.rend(); ++it)
          if (!(*it)->gridMark) {
            (*it)->gridMark = true;
            out->push_back(*it);
          }
      }
      // the third list, after the units and the props of the cell, each entry asked for its own kind (0x4fd177)
      if (mask & 12) {
        const auto& l = lists_[2][idx];
        for (auto it = l.rbegin(); it != l.rend(); ++it) {
          if (!(mask & ((*it)->kind == Entity::Kind::Projectile ? 4 : 8))) continue;
          if (!(*it)->gridMark) {
            (*it)->gridMark = true;
            out->push_back(*it);
          }
        }
      }
    }
  for (Entity* e : *out) e->gridMark = false;
}

void EntityGrid::GatherBoxNoMarks(float x0, float z0, float x1, float z1, int mask, std::vector<Entity*>* out) const {
  out->clear();
  if (!w_) return;
  auto span = [](float lo, float hi, int* c0, int* n) {
    int f = static_cast<int>(std::floor(lo)), c = static_cast<int>(std::ceil(hi));
    *c0 = std::clamp(f >> 2, 0, 0xffff);
    *n = std::clamp(((c + 3) >> 2) - *c0, 0, 0xffff - *c0);
  };
  int cx0, cz0, w, h;
  span(x0, x1, &cx0, &w);
  span(z0, z1, &cz0, &h);
  int cols = std::min(w, w_ - cx0), rows = std::min(h, h_ - cz0);
  // the asked lists' entries under the rect (each entity once, where it is first met): a short list is checked by a
  // look through what was found; a long one through this thread's own table (open addressing on the entity's address,
  // emptied after the call by the slots it used)
  size_t total = 0;
  for (int r = 0; r < rows; ++r)
    for (int c = 0; c < cols; ++c) {
      size_t idx = static_cast<size_t>(cz0 + r) * w_ + (cx0 + c);
      for (int k = 0; k < 2; ++k)
        if (mask & (1 << k)) total += lists_[k][idx].size();
      if (mask & 12) total += lists_[2][idx].size();
    }
  if (!total) return;
  static thread_local std::vector<const Entity*> seen;
  static thread_local std::vector<size_t> used;
  const bool table = total > 24;
  size_t m = 0;
  if (table) {
    size_t cap = 64;
    while (cap < 2 * total) cap <<= 1;
    if (seen.size() < cap) seen.assign(cap, nullptr);
    m = cap - 1;
    used.clear();
  }
  auto add = [&](Entity* e) {
    if (!table) {
      for (Entity* f : *out)
        if (f == e) return;
      out->push_back(e);
      return;
    }
    size_t hsh = (reinterpret_cast<uintptr_t>(e) >> 4) * 0x9E3779B97F4A7C15ull;
    for (size_t i = (hsh >> 20) & m;; i = (i + 1) & m) {
      if (seen[i] == e) return;
      if (!seen[i]) {
        seen[i] = e;
        used.push_back(i);
        out->push_back(e);
        return;
      }
    }
  };
  for (int r = 0; r < rows; ++r)
    for (int c = 0; c < cols; ++c) {
      size_t idx = static_cast<size_t>(cz0 + r) * w_ + (cx0 + c);
      for (int k = 0; k < 2; ++k) {
        if (!(mask & (1 << k))) continue;
        const auto& l = lists_[k][idx];
        for (auto it = l.rbegin(); it != l.rend(); ++it) add(*it);
      }
      if (mask & 12) {
        const auto& l = lists_[2][idx];
        for (auto it = l.rbegin(); it != l.rend(); ++it)
          if (mask & ((*it)->kind == Entity::Kind::Projectile ? 4 : 8)) add(*it);
      }
    }
  if (table)
    for (size_t i : used) seen[i] = nullptr;
}

void EntityGrid::GatherUnitsBox(float x0, float z0, float x1, float z1, std::vector<Entity*>* out) const {
  out->clear();
  if (!w_) return;
  auto span = [](float lo, float hi, int* c0, int* n) {  // (as GatherBox: func_AABoxToRect 0x4fcbe0)
    int f = static_cast<int>(std::floor(lo)), c = static_cast<int>(std::ceil(hi));
    *c0 = std::clamp(f >> 2, 0, 0xffff);
    *n = std::clamp(((c + 3) >> 2) - *c0, 0, 0xffff - *c0);
  };
  int cx0, cz0, w, h;
  span(x0, x1, &cx0, &w);
  span(z0, z1, &cz0, &h);
  int cols = std::min(w, w_ - cx0), rows = std::min(h, h_ - cz0);
  for (int r = 0; r < rows; ++r)
    for (int c = 0; c < cols; ++c) {
      const int cx = cx0 + c, cz = cz0 + r;
      const auto& l = lists_[0][static_cast<size_t>(cz) * w_ + cx];
      for (auto it = l.rbegin(); it != l.rend(); ++it) {
        const Entity* e = *it;
        // (an entry another cell's index wrapped onto (Cell) is taken here only if this happens to be that cell; it
        // lies outside the unit's span, and the callers' GridFindsInBox turns it away)
        if (cx != std::max<int>(e->gridX0, cx0) || cz != std::max<int>(e->gridZ0, cz0)) continue;
        out->push_back(*it);
      }
    }
}

void EntityGrid::GatherLine(float fromX, float fromZ, float toX, float toZ, std::vector<Entity*>* out) {
  out->clear();
  if (!w_) return;
  // 0x40d860: both axes mirrored so that the march runs upward, from the segment's start to its end
  const float ax = toX * 0.25f, az = toZ * 0.25f, bx = fromX * 0.25f, bz = fromZ * 0.25f;
  float sx, ex, sz, ez;
  int mx = 0, mz = 0;
  if (ax >= bx) {
    sx = bx;
    ex = ax;
  } else {
    sx = -0.0f - bx;
    ex = -0.0f - ax;
    mx = -1;
  }
  if (az >= bz) {
    sz = bz;
    ez = az;
  } else {
    sz = -0.0f - bz;
    ez = -0.0f - az;
    mz = -1;
  }
  if (!(sx == sx) || !(ex == ex) || !(sz == sz) || !(ez == ez)) return;
  const float dx = ex - sx, dz = ez - sz;
  int ix = static_cast<int>(std::floor(sx)), iz = static_cast<int>(std::floor(sz));
  for (int guard = 0; guard < 200000; ++guard) {
    if (static_cast<float>(ix) > ex || static_cast<float>(iz) > ez) break;
    const int cx = ix ^ mx, cz = iz ^ mz;
    {  // (0x4fd2a2..0x4fd2be: ((cz << shift) + cx) & mask, no bounds)
      const size_t idx = Cell(cx, cz);
      for (int k : {2, 0}) {  // (0x4fd2c1: the third list first, then the units)
        const auto& l = lists_[k][idx];
        for (auto it = l.rbegin(); it != l.rend(); ++it)
          if (!(*it)->gridMark) {
            (*it)->gridMark = true;
            out->push_back(*it);
          }
      }
    }
    // 0x4fd368: the cell border the line reaches first; on a tie the z step
    const float tx = (static_cast<float>(ix + 1) - ex) * dz, tz = (static_cast<float>(iz + 1) - ez) * dx;
    if (tz > tx) ++ix;
    else ++iz;
  }
  for (Entity* e : *out) e->gridMark = false;
}

void GridUpdate(Sim& sim, Entity* e, bool widened) {
  if (e->kind == Entity::Kind::Blip) return;
  EntityGrid& g = sim.entityGrid();
  if (!g.ready()) return;
  e->gridSeen = true;
  e->gridPos = e->position;
  e->gridOri = e->orientation;
  WorldShape ws;
  if (!GetWorldShape(e, &ws)) {
    g.Place(e, 0, 0, 0, 0);
    return;
  }
  if (widened && ws.type == ShapeType::Box && e->kind == Entity::Kind::Unit) {
    const UnitBpData* d = static_cast<Unit*>(e)->bpData;
    if (d) {
      ws.half.x = 0.5f * std::max(d->sizeX, std::fabs(d->skirtOffsetX) + d->skirtSizeX);
      ws.half.z = 0.5f * std::max(d->sizeZ, std::fabs(d->skirtOffsetZ) + d->skirtSizeZ);
    }
  }
  Vec3 mn, mx;
  ShapeBounds(ws, &mn, &mx);
  auto span = [](float lo, float hi, int* c0, int* n) {
    int f = static_cast<int>(std::floor(lo)), c = static_cast<int>(std::ceil(hi));
    *c0 = std::clamp(f >> 2, 0, 0xffff);
    *n = std::clamp(((c + 3) >> 2) - *c0, 0, 0xffff - *c0);
  };
  int cx0, cz0, w, h;
  span(mn.x, mx.x, &cx0, &w);
  span(mn.z, mx.z, &cz0, &h);
  g.Place(e, cx0, cz0, w, h);
}

void GridRemove(Sim& sim, Entity* e) { sim.entityGrid().Remove(e); }

bool GridFindsInBox(Sim& sim, const Entity* e, float x0, float z0, float x1, float z1) {
  if (!sim.entityGrid().ready()) return true;  // (no map: nothing to go by)
  if (!e->gridW || !e->gridH) return false;
  auto lo = [](float v) { return std::clamp(static_cast<int>(std::floor(v)) >> 2, 0, 0xffff); };
  auto hi = [](float v) { return (static_cast<int>(std::ceil(v)) + 3) >> 2; };
  return e->gridX0 < hi(x1) && lo(x0) < e->gridX0 + e->gridW && e->gridZ0 < hi(z1) && lo(z0) < e->gridZ0 + e->gridH;
}

void GridAdvanceCoords(Sim& sim) {
  // UpdateCollision runs here only for entities whose transform changed (AdvanceCoords 0x678f10 compares the
  // previous and the new transform, 0x67904b). A unit gets its skirt-wide cells in its constructor (Sim::CreateUnit);
  // one that a script put back on its plain box (RevertCollisionShape: an aircraft that landed, AirUnit.lua) keeps
  // the plain cells until it moves. (Seen: one blast over four parked aircraft; the order it kills them in, and so
  // the order of their explosions' random draws, is the order of their cells.)
  // The units go in the order of the dirty-entity list sim+0xa5c: AdvanceBeat walks it from its oldest link
  // (0x74a239..0x74a25f: [sim+0xa60], then each node's +4) and AdvanceCoords re-registers a unit whose transform
  // changed; in a shared cell the one re-registered later ends up in front (head insertion), which decides who a
  // steering's CheckCollisions asks first. The list keeps its order from beat to beat: an entity is linked when it is
  // made (Entity ctor 0x677c2d), SetPendingTransform 0x678ed9 and AdvanceCoords 0x6790d3 link only an entity that is
  // not on it, and Entity::SetHealth 0x679a3c..0x679a6b moves the entity to the newest end every time (no test). Read in
  // the original (oracle probe mp2_8034, MP2 8029-8034): six units stay linked with the same neighbours through all six
  // beats, the walk is not the motion stage's order (0x100071, 0x10006f, 0x100127, 0x100015, 0x10007b, 0x10005a, against
  // the entity stage's 0x100015, 0x10007b, 0x10005a, 0x100071, 0x10006f, 0x100127), and the Archer 0x10005a, which
  // regenerates (SetHealth each beat), is always the newest link; 0x10007b, hit by a shell at 8031, is newer still in
  // that beat. (Entity::Sync 0x67a0a0 would unlink a unit (+0x1b8 is 0), but no unlink shows in those beats.)
  // Seen: MP2 8031, 0x100127 and 0x10005a both enter cell (49,58); the original re-registers 0x100127 first, so
  // 0x10005a heads the cell, land scout 0x10006f's CheckCollisions of 8033 asks it before 0x100127, puts a record at +0
  // on it and brakes hard at 8034 (case 4). Ours went by the motion stage's order (0x10005a first) and drove on.
  // (Before: MP2 7087, the order by id had 0x10010d before 0x100008 in cell (30,36).)
  for (Unit* u : sim.DirtyOrder()) {
    if (u->destroyQueued) continue;
    if (u->gridSeen && std::memcmp(&u->gridPos, &u->position, sizeof(Vec3)) == 0 &&
        std::memcmp(&u->gridOri, &u->orientation, sizeof(Quat)) == 0)
      continue;
    GridUpdate(sim, u, true);
  }
  // The third list: shells, shields, script entities. Their turns set their transforms in the order they were
  // made (one entity stage), and that is the order UpdateCollision goes through them here; one whose cells change
  // goes to the head of its cells, so of two that change in one beat the younger ends up in front.
  std::vector<Entity*>& o = sim.gridScratch;
  o.clear();
  for (Projectile* p : sim.projectiles)
    if (!p->destroyQueued && (p->shape.type != ShapeType::None || p->gridW)) o.push_back(p);
  for (ShieldEntity* s : sim.shields)
    if (!s->destroyQueued && (s->shape.type != ShapeType::None || s->gridW)) o.push_back(s);
  for (Entity* s : sim.scriptEntities)
    if (!s->destroyQueued && s->kind == Entity::Kind::Entity && (s->shape.type != ShapeType::None || s->gridW))
      o.push_back(s);
  std::sort(o.begin(), o.end(), [](const Entity* a, const Entity* b) { return a->handle < b->handle; });
  for (Entity* e : o) GridUpdate(sim, e, false);
}

}  // namespace moho
