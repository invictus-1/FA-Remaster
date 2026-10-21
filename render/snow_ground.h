// [snow ground] The lying snow shaded by what stands round it (World::snowGround; terrain.h, Terrain::snowGround, whose
// shelter reads this): Chris, 2026-10-20, "when it snows the map looks like a blanket when some spots should be
// darker/lighter cause of the way the trees, rocks, mountains, hills are".
//
// The map's props as the snow's shelter sees them, laid once over the map (the shelter's size: two texels a world unit):
//   red: the share of the sky that solid things beside a place take from it (rocks, stumps, wrecks, buildings' props; a
//        tree's crown at half, light comes through it): each seen from the place as a disc of its footprint standing its
//        height, taking the part of the sky it hides (how wide it looks, the share of the ways round the place, times how
//        high it rises against the sky there, sin^2 of its angle as the sky's light on flat ground goes), the props
//        together as each takes from what the others leave;
//   green: the trees' crowns over the place (1 under the middle of a crown, 0 past its edge): the snow they catch, the
//        sky they hide;
//   blue: how much deeper or shallower the wind lays the snow round them (0.5: as elsewhere): a tail of it in each one's lee,
//        a scoop on its windward side, a well under a tree's crown (`wind`: the way it blows, x and z).
// The game's later changes to the props (a tree felled, a rock destroyed) are not followed: the map's props as it opens.
#pragma once
#include <cstdint>
#include <vector>

#include "gfx.h"

namespace render {

struct SnowProp {
  float x, z;    // where it stands (world units)
  float height;  // how high it stands over the ground there
  float radius;  // its footprint's (a tree: its crown's)
  bool tree;     // a tree: its crown over the ground, half as solid
};

// The raster (RGBA16F, `width` x `height` over the map's `mapWidth` x `mapHeight` world units). None: no props, or it cannot be made.
gfx::Texture SnowPropsRaster(gfx::Device* device, const std::vector<SnowProp>& props, float mapWidth, float mapHeight, int width, int height,
                             const float wind[2]);
// [snowhitch] Its texels alone (four half floats each, rows from the map's z = 0; empty: no props), worked out over the
// processor's cores (`spread`: Jobs) or on the calling thread alone (a thread of its own, ahead of the snow: World::SnowAhead);
// the same texels either way. And the raster made from them (none: empty).
std::vector<uint16_t> SnowPropsTexels(const std::vector<SnowProp>& props, float mapWidth, float mapHeight, int width, int height,
                                      const float wind[2], bool spread);
gfx::Texture SnowPropsTexture(gfx::Device* device, const std::vector<uint16_t>& texels, int width, int height);

}  // namespace render
