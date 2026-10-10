// The entity grid (the o-grid's entity hash, engine-ref rect_queries.md): 4x4-world-unit cells, each
// with a list of units and a list of props. An entity is registered in every cell its collision AABB
// touches (a unit's box widened to its skirt); it is re-registered, at the head of each cell's list,
// only when that cell span changes. GetUnitsInRect / GetReclaimablesInRect return everything
// registered in the cells a rect touches, in grid order (rows by z, columns by x, units before props,
// the most recently registered first).
#pragma once
#include <cstdint>
#include <vector>

namespace moho {

class Sim;
class Entity;

class EntityGrid {
 public:
  // cells: the map's size in world units / 4 (heightfield vertices - 1, >> 2)
  void Init(int mapW, int mapH);
  // Set an entity's cell span; re-registers only if it changed (w or h 0: in no cell).
  void Place(Entity* e, int cx0, int cz0, int w, int h);
  void Remove(Entity* e);
  // Everything registered in the cells the rect touches (mask 1: units, 2: props), each once, in grid
  // order (Rect2fToInt16 + GatherUnmarked).
  void Gather(float x0, float z0, float x1, float z1, int mask, std::vector<Entity*>* out);
  // COGrid::CollectEntitiesInBox's cell span (func_AABoxToRect 0x4fcbe0: floor / ceil, like an entity's own span).
  void GatherBox(float x0, float z0, float x1, float z1, int mask, std::vector<Entity*>* out);
  bool ready() const { return w_ > 0; }

 private:
  int w_ = 0, h_ = 0;
  std::vector<std::vector<Entity*>> lists_[2];  // per cell; the back is the head
};

// Entity::UpdateCollision / UpdateRect: the entity's cell span from its collision primitive (none: no
// cell). widened: a unit's box half-extents in X and Z become half max(Size, |SkirtOffset| + SkirtSize)
// (Unit::UpdateCollision); else the plain primitive (shape changes from Lua, creation).
void GridUpdate(Sim& sim, Entity* e, bool widened);
void GridRemove(Sim& sim, Entity* e);
// Step 13 of the beat (Entity::AdvanceCoords): every unit's widened span.
void GridAdvanceCoords(Sim& sim);

}  // namespace moho
