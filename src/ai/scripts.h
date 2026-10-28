// FA Remastered's own AI: its Lua (the repository's data/ai) given to the sim state of a session that has it
// (docs/ai-architecture.md 4.1).
//
// Part of FA Remastered's engine, MIT licence (see LICENSE). Our own design and code (docs/ai-architecture.md 0).
//
// The lead's decision (2026-10-28, with the ui): the Sim takes data/ai itself, only when an army of the session plays
// our AI (AIPersonality "remastered..."), into a layer it owns, gone with it. Nothing is mounted: the host's file
// system (moho64ui shares one between its menus, the interface and the Sim, from game to game) never sees data/ai.
// - data/ai/hook/<path>: hooks of the game's scripts, joined to the sim state's extra hooks after the engine's
//   script fixes (ScriptState::AddExtraHooks), e.g. hook/lua/aibrains/index.lua adds our lobby keys to keyToBrain;
// - every other .lua file of data/ai, at its path (data/ai/lua/remastered/ai/brain.lua is /lua/remastered/ai/brain.lua):
//   the sim state's own scripts (ScriptState::SetOwnFiles), found by doscript / import and exists() only. A file the
//   game's file system already has is left out with a warning: ours never replace a game file.
// The folder is found beside the program (data/ai) or one folder up (the repository's, from build-zig/), as the
// engine's script fixes are; MOHO64_AI_DIR names another. The files are read at each session that has our AI.
#pragma once

namespace moho {

class ScriptState;

namespace ai {

// Give the state data/ai's hooks and scripts (see above). Logs what it took; false when the folder is missing.
bool JoinOurAiScripts(ScriptState& st);

}  // namespace ai
}  // namespace moho
