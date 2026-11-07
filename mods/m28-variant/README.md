# M28AI, FA Remastered variant

This folder is a **modified version of M28AI**, the AI for Supreme Commander: Forged Alliance written by
**maudlin27**, its original author: <https://github.com/maudlin27/M28AI>. It is based on M28AI v326.

## Licence

- M28AI is licensed under **CC BY-NC-SA 4.0** (Creative Commons Attribution-NonCommercial-ShareAlike 4.0
  International). The licence text is in `LICENSE` in this folder, unchanged from the original.
- Our changes to it are under the same licence, **CC BY-NC-SA 4.0**.
- Non-commercial: this variant is never sold, never put behind a paywall, and no donation is ever tied to it.
- This folder is a separate component. It is not part of FA Remastered's engine, which is under the MIT licence;
  no code of M28AI is in the engine.
- Some files of M28AI hold code by other authors under their own terms, for example Willem 'Jip' Wijnia's FAF
  utilities under the MIT licence (`lua/AI/Steam/`, `lua/AI/LOUD/`, parts of `lua/AI/M28Utilities.lua`) and code
  copied from FAF and Gas Powered Games. Their headers are kept as they were, and that code keeps its own terms.

## What was changed (FA Remastered, 2026-10-21 to 2026-10-23)

The aim of every change: the same AI making the same decisions, only faster. Tested by playing the same games with
the original M28AI v326 and with this variant in FA Remastered and comparing the whole game state every beat
(identical).

1. **Name and paths.** The folder is `mods/m28-variant`, so every import path `/mods/M28AI/...` in the Lua files
   reads `/mods/m28-variant/...` (58 files, nothing else changed in them). `mod_info.lua` has its own uid and name
   ("M28AI (FA Remastered variant)"), credits maudlin27, and conflicts with M28AI v326 (do not enable both).
   `lua/AI/M28Overseer.lua`: the variant's mod name is on M28's own list of known AI mods, next to "M28AI", so that
   it does not report itself as an unknown sim mod.
2. **Faster check of build locations.** When a building is started or finished, M28 re-checks every buildable spot
   ("segment") around it with `aiBrain:CanBuildStructureAt`, for every building size, in
   `CheckIfSegmentsStillBuildable` (`lua/AI/M28Engineer.lua`): about 15,000 checks per call on a 20 km map, and with
   16 factories upgrading at once about 200 ms in one beat. The variant remembers, per zone and spot, that the checks
   passed, and checks again only where something may have changed since: a new or moved building, flattened
   terrain, a new blacklist entry nearby, a new resource deposit. When a check runs, it runs exactly as the
   original's (same calls, same order, same removals). The original function is kept unchanged as
   `CheckIfSegmentsStillBuildableOriginal` and is used whenever anything is unusual (debug or profiling switches,
   CPU performance mode, a Scathis in the game, unexpected blueprint data).
   - New file `lua/AI/M28VariantBuildCache.lua`: the "may have changed" marks. A unit's place is marked again at
     the next two beats that check, because the engine updates a unit's collision-grid cells (which decide whose
     skirts `CanBuildStructureAt` tests) only at the end of a beat in which it was made, moved or turned.
   - `hook/lua/simInit.lua`: `FlattenMapRect`, `Warp` and `CreateResourceDeposit` also set marks.
   - `hook/lua/sim/Unit.lua`: `OnCreate` of a unit that is not mobile, and `SetPosition`, `SetOrientation`,
     `SetCollisionShape`, `RevertCollisionShape` of one, set marks; `AttachTo` / `AttachBoneTo` of one registers it,
     and each check first marks where such attached units have moved since.
   - `lua/AI/M28Engineer.lua`: a new build-location blacklist entry sets a mark.
   - `M28VariantBuildCache.bVerify` is a test switch (off): it re-runs every skipped check and logs any that would
     now fail.
3. **Cheaper small helpers** (2026-10-23, after a profile of a one-hour game):
   - `IsTableEmpty` (`lua/AI/M28Utilities.lua`, about 165 million calls an hour): the same answers, with less work
     (no `pairs` call, `type` / `next` kept as upvalues, a value that is not a table answered without a call).
     The original is kept as `IsTableEmptyOriginal`.
   - `FunctionProfiler` (`lua/AI/M28Profiler.lua`, about 96 million calls an hour) returns at once when no debug or
     profiling switch is on (the rest of it does nothing then).
4. **The zone of a position** (2026-10-24, `lua/AI/M28Map.lua`): `GetPlateauAndLandZoneReferenceFromPosition` and
   `GetClosestPlateauOrZeroAndZoneToPosition` asked FAF's navigational mesh for the hover label of the position's
   segment on every call (about 14 million times an hour, a walk down FAF's quadtree in Lua). The label of each
   segment is now kept and asked again only when the mesh is generated anew or the segment size or playable area
   change. `bVariantVerifyHoverLabel` is a test switch (off) that asks the mesh as well and logs any difference.
5. **The engine's own helpers** (2026-10-24). FA Remastered, in a game with its script fixes on, offers a few
   generic helpers of its own in the global table `Remastered` (the engine's code, not M28's). Where one gives exactly
   the answers of a small M28 function, the variant uses it; on any other engine (`Remastered` missing) the Lua
   function stays. `bVariantVerifyNative` (`lua/AI/M28Utilities.lua`) is a test switch (off) that runs both and logs
   any difference.
   - `IsTableEmpty` (`lua/AI/M28Utilities.lua`): `Remastered.MakeDeepEmptyCheck(5)`, the same deep check.
   - `FunctionProfiler` (`lua/AI/M28Profiler.lua`): `Remastered.Noop` while no debug or profiling switch is on (the
     Lua function does nothing then); `VariantRefreshProfiler` puts the Lua one back when the variant turns a switch
     on. `bVariantVerifyNoop` is its test switch (off).
   - `GetDistanceBetweenPositions` (`lua/AI/M28Utilities.lua`): `Remastered.VDist2Pos`, the same VDist2 of the same
     four reads.
6. **The walk along a shot** (2026-10-24, `IsLineBlocked` in `lua/AI/M28Logic.lua`, Lua only): the angle, its sine
   and cosine and the shot's slope are worked out once per call instead of at every step, and no table is made per
   step; each step does the same arithmetic in the same order. The original walk runs when debug messages are on, and
   with `M28Utilities.bVariantVerifyNative` both run and any difference is logged.
7. **Categories kept per call site** (2026-10-25, Lua only): 35 places that build the same category from the same
   categories every time they run (for example `categories.MOBILE - refCategoryScathis` for every unit of a list;
   about 14 of the 16 million category operations of a one-hour game) now go through
   `M28Utilities.VariantCategoryChain`, which builds the category once and gives it again while its parts are the same
   objects. Each place still evaluates in the same order and gives the same category. With
   `M28Utilities.bVariantVerifyNative` each place also builds its expression as written and logs any difference.
   Since 2026-10-31 the engine keeps these expressions itself when it can (`Remastered.MakeCategoryChain` over the
   variant's own `VariantCategoryOp`: the same cache, the same `==` on the parts, the same operator calls in the same
   order when they changed); with `bVariantVerifyNative` both run and their results are compared.

8. **The leaf of FAF's navigational mesh** (2026-10-28). Every label question to FAF's navigational mesh (Jip's
   `/lua/sim/navgenerator.lua`, MIT), and the mesh's own generation, ends in `NavGrid.FindLeafXZ`, a walk down a
   quadtree in Lua (about 2.5 million times in the first seconds of a game against M28: the mesh at 1 s, M28's map
   setup at 1.6 to 2.1 s). The engine does that walk: `Remastered.MakeNavGridLeafXZ`, the same reads and arithmetic
   as FAF's two Lua functions, which it calls itself for anything unusual. `M28Utilities.VariantSwapNavGridLeaf` puts
   it in the NavGrid class, only when the two functions are FAF's own (that file, those lines) and NavUtils is
   FAF's. With `M28Utilities.bVariantVerifyNative` both run on every call and any difference is logged.
9. **Is a unit valid** (2026-10-31, `lua/AI/M28UnitInfo.lua`, about 47 million calls an hour): `IsUnitValid` is the
   engine's field test, `Remastered.MakeFieldTest('+UnitId -Dead +GetAIBrain', IsUnitValid)`: the same three reads
   through the VM's own indexing in the same order, stopping where the Lua `and` stops; anything that is not a table
   goes to the Lua function itself. `bVariantVerifyNativeUnitInfo` (a switch of that file: M28Utilities is not loaded
   yet when it runs) runs both and logs any difference.
10. **The navigational mesh made while the game loads** (2026-11-07, new file `lua/AI/M28VariantNavAhead.lua`, one
   line in `hook/lua/simInit.lua`). M28 asks for FAF's navigational mesh (Jip's `/lua/sim/navgenerator.lua`) when its
   first brain records its start point, at 1 second of game time, and FAF's generator then took 0.8 s or more in one
   beat: the first freeze of every game against M28. The mesh is now made at the end of BeginSession, while the game
   still loads, in a second copy of FAF's module. FAF's own module is not touched until the mesh is first asked for;
   that first `Generate` then takes the copy's mesh and does what Generate does after making it (the layer data for
   the user layer, the debug module, its console lines), provided nothing the generator reads has changed (the map's
   size, type and playable area, the water ratio, the ground's heights, the resource markers); otherwise FAF's own
   Generate runs as before. The resource markers' labels it sets are put on only then. Only on FA Remastered (it needs
   the engine's `Remastered.TerrainHeightWrites`) and with an M28 AI in the game. `bVariantVerifyNavAhead` (also on
   with `M28Utilities.bVariantVerifyNative`) runs FAF's Generate anyway and compares the two meshes;
   `bVariantNavAheadForceDrop` (a test switch) always drops the copy. M28's map setup at 1.6 to 2.1 s stays where it
   was: it reads what the first beats made and is read by M28's other threads while it runs.

Every change in the Lua files is marked with a comment that starts "FA Remastered variant".

## How the variant is proven, and what that cannot promise (2026-10-31)

The standard is **every decision identical**: with the test switches on (`bVariantVerifyNative`,
`bVariantVerifyNativeUnitInfo`, `bVariantVerifyHoverLabel`, `M28VariantBuildCache.bVerify`) each changed function also
runs the original way on every call and any different answer is logged; the proof runs need 0 of them. On top of that,
the short and medium sessions (B9: 405 beats, b8: 6,000 beats) must play out identically, beat for beat, with the
original M28AI v326, with this variant, with this variant on an engine without the `Remastered` helpers ("hidden":
`MOHO64_NO_REMASTERED=1`, every Lua fallback), and with the test switches on.

A long game (B1: 36,000 beats) is not held to beat-for-beat identity between those, because it cannot be promised. Two
runs that make every decision identically can still order some of their Lua work differently: Lua tables keyed by
objects (units, tables, functions) are walked in an order that follows how many objects were made before, and the
variant (or its fallbacks, or its test switches) makes a different number of them than the original. Where such a walk
calls the engine, the engine's own order follows: FA itself moves an entity to the end of its "dirty" list on every
SetHealth / AdjustHealth (and intel call), and that list decides the order units are put back into the collision grid,
which decides the order of a weapon's candidate targets. Seen on B1 (2026-10-31, main 7a07d14): the fingerprints of
the original M28, the variant and the hidden variant were identical for 33,234 beats, while the dirty-list order
already differed from beat 30,673 (original vs variant) and 31,292 (original vs hidden); at beat 33,223 the same four
units had their health adjusted from Lua in another order, at 33,235 two of army 4's Ahwassas got their candidate
targets in another grid order and one weapon picked another target. The engine keeps this order as FA does (making it
canonical would break the original's replays); games in FA Remastered's own parallel mode sort the grid's lists by
creation serial and do not depend on it.

So the hidden mode is a check for the short and medium sessions only, and a long game against M28 is expected to be
identical between the original and the variant only while both make their Lua objects in the same order.
