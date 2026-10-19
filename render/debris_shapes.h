// The shapes of the debris (debris.h): a small library of irregular fragments, made once from a fixed seed (the same
// pieces every run): plates torn out of a hull, some bent; panels with a fold; bits of girder and strut with ragged
// ends; small jagged shards; and lumps of broken concrete for buildings. Each is a few dozen flat triangles, its middle
// at the origin, one world unit across at its widest (a piece is drawn at its size).
//
// A shape is two lists of triangles: its skin, what was the unit's outside (drawn in the unit's paint), and its torn
// faces, what the break laid bare (a plate's thin rims, a girder's broken ends, a lump's fresh fractures; drawn in a
// material of their own, debris.cpp). The skin's texture places are the piece's own, about -0.5..0.5 across it and
// laid as the surface lay before it was bent, so a plate's paint runs on over its fold: Debris lays them onto a patch
// of the unit's texture.
#pragma once
#include <cstdint>
#include <vector>

#include "models.h"

namespace render {

struct DebrisShape {
  enum Kind : uint8_t { Plate, Panel, Girder, Strut, Shard, Lump };
  Kind kind = Plate;
  // three corners to a triangle, counter-clockwise seen from outside, each with its face's normal and the tangents of
  // its texture places (as a mesh file's)
  std::vector<ScmVertex> skin, torn;
  Vec3 lo, hi;  // the box round it
};

// The library, in this order: kDebrisPlates torn plates, then the panels, girders, the strut, the shards, the lumps.
inline constexpr int kDebrisShapes = 16;
std::vector<DebrisShape> MakeDebrisShapes();

// Which of the library a piece of a unit is, by what the unit is (its blueprint id's third letter: 'b' a building,
// 'a' an aircraft, anything else a vehicle or a ship) and two random numbers in 0..1: a building's are mostly lumps
// of concrete and girders, an aircraft's thin plates and shards, a vehicle's or a ship's plates, panels and girders.
int PickDebrisShape(char kind, float random, float random2);

}  // namespace render
