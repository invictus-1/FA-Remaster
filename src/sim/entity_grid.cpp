// The entity grid (engine-ref rect_queries.md); see entity_grid.h.
#include "sim/entity_grid.h"

#include <algorithm>
#include <cmath>

#include "sim/collision.h"
#include "sim/sim.h"
#include "sim/terrain.h"
#include "sim/units.h"

namespace moho {

namespace {
int ListOf(const Entity* e) {
  if (e->kind == Entity::Kind::Unit) return 0;
  if (e->kind == Entity::Kind::Prop) return 1;
  return -1;  // projectiles, shields, beams, script entities: not kept (GetEntitiesInRect is not bound)
}
}  // namespace

void EntityGrid::Init(int mapW, int mapH) {
  w_ = std::max(1, mapW >> 2);
  h_ = std::max(1, mapH >> 2);
  for (auto& l : lists_) l.assign(static_cast<size_t>(w_) * h_, {});
}

void EntityGrid::Remove(Entity* e) {
  int k = ListOf(e);
  if (k < 0 || !e->gridW || !e->gridH) {
    e->gridW = e->gridH = 0;
    return;
  }
  for (int z = e->gridZ0; z < e->gridZ0 + e->gridH && z < h_; ++z)
    for (int x = e->gridX0; x < e->gridX0 + e->gridW && x < w_; ++x) {
      auto& c = lists_[k][static_cast<size_t>(z) * w_ + x];
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
  e->gridX0 = static_cast<uint16_t>(cx0);
  e->gridZ0 = static_cast<uint16_t>(cz0);
  e->gridW = static_cast<uint16_t>(w);
  e->gridH = static_cast<uint16_t>(h);
  for (int z = cz0; z < cz0 + h && z < h_; ++z)
    for (int x = cx0; x < cx0 + w && x < w_; ++x) lists_[k][static_cast<size_t>(z) * w_ + x].push_back(e);
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
    }
  for (Entity* e : *out) e->gridMark = false;
}

void GridUpdate(Sim& sim, Entity* e, bool widened) {
  if (e->kind != Entity::Kind::Unit && e->kind != Entity::Kind::Prop) return;
  EntityGrid& g = sim.entityGrid();
  if (!g.ready()) return;
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

void GridAdvanceCoords(Sim& sim) {
  for (Unit* u : sim.units())
    if (!u->destroyQueued) GridUpdate(sim, u, true);
}

}  // namespace moho
