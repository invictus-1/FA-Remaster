# The interface track's answers (newest first)

Answers to the other tracks' requests to the interface (CLAUDE.md: "the layer asked answers in a file of its own").
The requests themselves are in the asking track's file; each answer names it.

## render/REQUESTS.md 42: Chris's playthrough of 19 October, the interface's six items (2026-10-20)

Built as the original does them (read from the FA exe), tested, landing with ui ba69c02 / 7b25a3f / 9128bcb.

1. **Item 5, "health regen on Izanagi shows +0 while shields show +600"**: our host's fault, not the blueprint's (the
   staged MIA0401 has Defense.RegenRate 300). FAF's unit view shows `GetStat("HitpointsRegeneration")`, and our
   UserUnit:GetStat always answered the default, so every unit showed "+0/s". It now reads the unit's statistics from
   the engine's view (UnitStateView::stats, engine 13a6e16): the regen numbers, veterancy (VetLevel, VetExperience)
   and the reclaim totals are real. The Izanagi should now show "+300/s".
2. **Item 16, the mass extractor snapping from farther**: it was missing. While an extractor (or a hydrocarbon plant)
   is placed, the place under the mouse jumps onto the nearest fitting deposit within the original's reach (the
   cursor update 0x86f520, FAF's patch; ui_ExtractSnapTolerance 4 map units, capped between 20 and 90 pixels); the
   ghost and the click use that place. If Chris wants it farther than the original, that is a deviation to decide.
3. **Item 17, Alt+F2 opening the spawn tool**: the key was bound (FAF's debugKeyMap), but its console command
   PopupCreateUnitMenu was not built. Built (0x7b5a40): FAF's create-unit window at the mouse; picking a unit and
   clicking the map spawns it, for the army chosen in the window. As in the original, the window also opens without
   cheats, where spawning does nothing.
4. **Item 18, the commander's queue not on his build bar**: not reproduced: in every case tried (cheats, his own tour
   session, his recorded games' orders) FAF's panel showed the queue. Four small differences to the original were
   fixed on the way. Open question for Chris: what had he queued, and did it show after reselecting him?
5. **"Clicking on unit pulls up attack order cursor instead of default"**: a key held when the window lost the front
   stayed held for us (an Alt through an Alt+Tab or a screenshot tool), and FAF shows the attack cursor while Alt is
   held with a selection. As the original (MAUI_KeyIsDown 0x79cb70), a key counts as held only while the window is in
   front, and leaving the front lets every key go.
6. **Item 22, "load the game without any mods at all"**: the plain launchers (Open-Game-NewMap, Open-Game-Quixel)
   load none. The Izanagi tours load the Wyvern Battle Pack because his Mitsurugi mod requires it; Chris chose
   (2026-10-20) a mod-free tour beside them: `_scratch\ui\Open-Game-Tour.bat` (an Aeon CZAR on the rebuilt map, no
   mods at all). The mod units' remastered art moved out of `visuals\work` (visuals, 2026-10-20) into
   `visuals\work_mods\<mod>`, mounted only by the launchers of games with that mod (`--remasters` may be given more
   than once now).
