// [grime] Battle grime, visible damage and weather dirt on units (the modern look; Chris, 2026-10-23: "100% yes", the
// visuals session's section 65 and the renderer's answer in docs/render-answers.md, "To visuals, section 65"). Two player
// options, off by default: World::unitGrime (grime and the weather's dirt) and World::unitDamage (damage shown by health).
// They build on the first versions in unit_wear.h (battle scars, snow on units, wet and muddy units): one state per unit
// (UnitWear's), one shader path (the model shader's, after the leaves' light and before the marks).
//
// Battle grime: dust, caked mud and soot gather on the clean paint the longer a unit fights and drives. Its fighting (hits
// taken, its own firing, blasts near it: UnitWear::Watch, each beat) gathers soot and dust; what it drives through
// (UnitWear::Dirt, each frame: mud or soft wet ground a crust of mud, dry dirt and sand a coat of dust) gathers the rest.
// Rain washes dust off in minutes, soot and the crust slowly; a unit from the factory is clean. Repair leaves it as it is
// (field units stay dirty).
// Weather dirt: in rain, mud splashes up its lower body from soft ground (the more as it drives; it dries on and flakes off
// over a quarter of an hour) and its crust goes dark and glossy; where snow lies, frost on its top surfaces and in the
// sheltered creases of its upper body (none on a unit hot from fighting or burning).
// Visible damage: as its health drops, dents, burns and torn plates show, in one smooth reveal by its damage order (the
// mask's B: a texel shows where B is above its health, over a soft edge); what was lost in the last seconds glows where
// the damage set says it is hot. Repaired, the damage goes as its health comes back, and the first three tenths of the
// worst it lived through stay as trophies: patched (the dents filled to a third, fresh paint over them), as the battle
// scars keep theirs mended (what reads best: a veteran looks it, but whole).
//
// The art (the visuals session's; section 66 for the first unit, UEL0001):
//  - MASKS in the unit's secondary texture, uv0, linear: R its crease shading as before, G where grime gathers, B the order
//    damage shows (rank-equalised: the share of texels above h is 1 - h), A where weather dirt sits (mud splash low down,
//    frost in sheltered creases up high). Read with the crease shading: no texture read more. File:
//    <ID>_lab_masks.dds beside the unit's _lab_ textures (it takes _lab_ao's place while the options are on).
//  - The DAMAGE SET, in the unit's own UVs: <ID>_lab_damage_albedo.dds (colour as painted, sRGB; alpha how hot, linear) and
//    <ID>_lab_damage_normal.dds (the units' layout: x in alpha, y in green).
//  - The SHARED TILING LAYERS, one texture array for all units (grime layers, t24), read in the unit's UVs at a scale per
//    set, two slices a set: its albedo (RGB its colour as painted, sRGB; A its height, linear: where it lies thick shows
//    first as it builds) and its normal (G and A as the units', R its roughness, B unused). Sets in order: dust, mud (caked),
//    soot, frost, wetmud. Files, all of one size, format and number of levels: <root>/_unitgrime/export/moho64/<set>_albedo.dds
//    and <set>_normal.dds (mounted at kRemasteredUnitGrime, remasters.h). Until all ten are there, stand-ins drawn here
//    (MakeGrimeLayers: tiling noise of each kind, 256 a side).
//  A unit without masks (every original unit, and remastered ones until theirs are made): grime and frost by its height,
//  its facing and its crease shading, and the damage order a noise over the model at rest; without a damage set, its damage
//  a stand-in of scorched and scuffed paint with bare metal and hot spots.
// Its look per unit rides in the bone buffer after its marks and its dirt (WearLook::grimeRows); a model's level says in its
// kind5.z which art it has (MeshRenderer: a quarter for the masks, a half for the damage set).
#pragma once
#include "gfx.h"

namespace render {

class TextureCache;

// The shared tiling layers' array (the visuals session's files where all ten are there, else stand-ins).
gfx::Texture MakeGrimeLayers(gfx::Device* device, TextureCache* textures);

// The model shader's part: GrimeSurface, before WearSurface (its text in unit_grime.cpp). Also the two values the first
// versions read from it: grimeTaken (WearSurface leaves its own patches of grime to it) and grimeSplash (the mask's A, where
// DirtSurface's fresh mud lies; -1 without masks).
extern const char kGrimeShaderBlock[];

}  // namespace render
