// Remaster aliases of the game's emitter files (docs/deviations.md 12): an emitter path a script names in place of
// a stock one, so that each use of a shared stock file can get a look of its own in the remastered renderer.
//
// The one table of them, for the engine and the renderer alike (so the two cannot drift): EffectAliases() is a
// compiled-in list, needing no sim, so the viewer and the interface read it too.
// - The sim (Sim::Start, with Sim::fixes only) registers each alias as the stock emitter's blueprint: the same kind
//   and Lifetime under the alias's id, so CreateEmitter* on the alias makes the effect it made on the stock file
//   (no blueprint would make nothing: effects.cpp l_Factory). The sim reads nothing else of an emitter blueprint.
// - The renderer draws an alias as its stock file unless a remastered look gives the alias its own: so the
//   original's look, and every faction or use without an override, stay stock.
// - The scripts name the aliases: data/fixes/hook/lua/effecttemplates.lua writes the same paths into the templates
//   (Lua cannot read this table; Sim::Start checks that every alias found in the templates is registered).
// Paths are lower case, as the game's file system normalizes them.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace moho {

// (alias path, stock path) pairs, in a fixed order
const std::vector<std::pair<std::string, std::string>>& EffectAliases();

}  // namespace moho
