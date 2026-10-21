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
  // Here the mask also has 4: shells (0x400) and 8: the rest of the third list (0x800), which a cell gives after
  // its units and props (0x4fd000).
  void GatherBox(float x0, float z0, float x1, float z1, int mask, std::vector<Entity*>* out);
  // The cells a segment passes, from its start to its end, and in each cell the third list (shells, shields,
  // script entities: what has a collision primitive and is no unit and no prop) and then the units; props are not
  // asked. Each entity once, where it is first met. (COGrid's line query 0x4fd200, behind
  // GetEntityCollisionsInLine 0x7229c0: a shell's and a beam's candidates, in the order they are asked.)
  void GatherLine(float fromX, float fromZ, float toX, float toZ, std::vector<Entity*>* out);
  bool ready() const { return w_ > 0; }

 private:
  int w_ = 0, h_ = 0;
  // EntityOccupationManager 0x4fcd20: shift = bsr(W) (+0xc), mask = W * H - 1 (+8). A cell's list is at
  // ((z << shift) + x) & mask, and an entity's own cells (CollisionShape::Add 0x4fd420 / Remove 0x4fd490) and the line
  // march (0x4fd200) take that index for any cell, the map's or not: a cell east of the last column is a cell of the
  // next row, one south of the last row one of the first. Only the rect queries (0x4fd000) clip to W and H.
  int shift_ = 0;
  uint32_t mask_ = 0;
  size_t Cell(int x, int z) const {
    return static_cast<size_t>(((static_cast<uint32_t>(z) << shift_) + static_cast<uint32_t>(x)) & mask_);
  }
  // per cell; the back is the head. 0: units (0x100), 1: props (0x200), 2: shells (0x400) and shields, beams,
  // script entities (0x800)
  std::vector<std::vector<Entity*>> lists_[3];
};

// Entity::UpdateCollision / UpdateRect: the entity's cell span from its collision primitive (none: no
// cell). widened: a unit's box half-extents in X and Z become half max(Size, |SkirtOffset| + SkirtSize)
// (Unit::UpdateCollision, also in the Unit constructor); else the plain primitive (shape changes from Lua).
void GridUpdate(Sim& sim, Entity* e, bool widened);
void GridRemove(Sim& sim, Entity* e);
// Step 13 of the beat (Entity::AdvanceCoords): every unit's widened span, and the span of everything in the third
// list, in the order they were made (the order their turns set their transforms in).
void GridAdvanceCoords(Sim& sim);
// Whether a grid query of the box [mn, mx] finds the entity: the cells it is registered in (its span as the grid
// has it: skirt-wide for a unit unless a script set its shape since it last moved, as of the last AdvanceCoords)
// against the box's cells (func_AABoxToRect 0x4fcbe0). GatherUnmarkedUnitsInBox 0x721c00 and CollectEntitiesInBox
// 0x721dc0 see only these before any box is compared.
bool GridFindsInBox(Sim& sim, const Entity* e, float x0, float z0, float x1, float z1);

}  // namespace moho
