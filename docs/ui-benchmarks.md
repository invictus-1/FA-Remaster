# The interface's measurements

Before/after numbers of the interface track's performance work (docs/performance-lessons.md, ground rule 2:
commit, hardware, scenario, numbers). Newest first.

**Hardware (all rows):** AMD Ryzen 9 9950X3D, NVIDIA RTX 4090, the screen at 240 Hz, vertical sync on.

**Scenario L (late eight-army game):** the renderer's set-up `_scratch/render/config_m28_setons_r.lua` (one player
and seven M28 AIx armies on the rebuilt Seton's Clutch), `--seed 7 --fast-forward 6000` (ten game-minutes in), a
1600x900 window, the player's commander selected (the game's start selects it), the camera where the game puts it.
Then 300 frames of warm-up and 600 counted frames. Script: the interface chat's scratchpad `late_measure.sh` /
`late_next.sh <tick> <size> <tag> [env]`. The log lines are `moho64ui: window: ...` at the end of the run.

**Scenario S (a big selection; a stand-in for the engine's B8 until it exists):** `_scratch/ui/session_ai_cheats.lua
--seed 7`, a 1600x900 window, the script `_scratch/ui/scale_2000/scale.lua`: at 3 s the cheat callback makes 2,000
uel0201 for the player beside the commander, all selected by category, 20 s steady, then a box selection by the mouse
and a move order by a right click. `MOHO64UI_BINDING_PROFILE=1` logs the costliest engine functions the scripts
called (time and calls).

## 2026-10-16: scenario L at 2560x1440 (d1df774 era)

Frame to frame 4.16 / 4.92 / 5.68 / 6.52 ms (middle / 95% / 99% / longest), none over 20 ms; the interface's
scripts 0.25 ms, drawing 0.64 ms a frame; the world's picture 3.12 ms (with vertical sync's wait); a beat 14.4 ms of
simulation, 2.9 ms of picture copy (on its own thread).

## 2026-10-16: the interface running during a beat (69c7623, landed 0de3111)

Scenario L, `MOHO64UI_SHOW_ONLY_DURING_BEATS=1` (frames during a beat only show the picture) against the default
(the interface, input and camera run during a beat too; what is asked every frame answers from copies), same build:

| Frame to frame | Show-only frames | Interface live |
|---|---|---|
| middle | 4.16 ms | 4.17 ms |
| 95% | 5.14 ms | 4.25 ms |
| 99% | 5.88 ms | 5.05 ms |
| longest | 7.36 ms | 5.88 ms |
| frames with the world standing still | 1 of 631 | 0 of 631 |

Waits for a beat left (MOHO64UI_BEAT_LOG) in three busy windowed games (the game test, build templates, scenario S):
2 in all, both one-off (a selection by category). Before the copies: 114 in GetEconomyTotals alone in scenario L.

## 2026-10-16: the cursor's order and the range rings with 2,000 selected (scenario S; 227b4ca, landed f4e220e)

| Part (a call) | Before | After |
|---|---|---|
| CUIWorldView:GetRightMouseButtonOrder | 2.78 ms | 0.60 ms |
| (its RightClickOrder: the selection's caps from UnitView, engine 26) | most of it | 0.006 ms |
| (its reclaim test, engine 27 + the kept answer) | ~1 ms | 0.005 ms |
| (the world picture's scene made for the frame: the range rings) | 0.85 ms | 0.40 ms |

The rest of the cursor's cost was the world picture's scene being made for the frame (the first world query of a
frame makes it): with 2,000 selected mostly their range rings, now worked out per blueprint, not per unit.

## 2026-10-15: engine functions with 2,000 selected (scenario S; before: 39df633 with the profiler, after: the commit that brings this table)

| Engine function | Before (a call) | After (a call) |
|---|---|---|
| CUIWorldView:GetRightMouseButtonOrder (the cursor's script) | 9.78 ms | 2.78 ms (the selection's caps kept for the beat) |
| UISelectionByCategory (2,000 units) | 259.3 ms | 25.2 ms |
| GetIdleFactories | 0.785 ms | 0.031 ms |
| GetArmyAvatars | 0.515 ms | 0.075 ms |
| GetIdleEngineers | 0.434 ms | 0.036 ms |

The category tests looked up each unit's blueprint and the category by name for every unit; now a unit keeps its
blueprint's record and a loop finds the category once. Still open: GetRightMouseButtonOrder asks every selected
unit's state of the simulation once a beat (about 10 ms a beat with 2,000): the engine's request 26 (command caps in
the per-beat UnitView) removes it. Order latency (scenario S): every one of the 2,000 had the move order at the
first look, 0.1 s (two beats) after the click.
Also seen: with 2,000 tanks the simulation takes 525-885 ms on one beat in every 10 to 60 (the engine's: parked for
its benchmark phase; first suspect Lua 5.0's stop-the-world collector).

## 2026-10-15: the game's beats on their own thread (58e1c35, landed b12d8c7)

Scenario L, `MOHO64UI_BEATS_HERE=1` (before) against the default (after), same build:

| | Before | After |
|---|---|---|
| Frame to frame, middle | 5.41 ms | 4.10 ms |
| 95% | 33.03 ms | 6.92 ms |
| 99% | 53.92 ms | 8.81 ms |
| Longest | 85.97 ms | 10.28 ms |
| Frames over 20 ms | 50 of 631 | 0 of 631 |
| Frames with the world standing still (next beat late) | 0 | 12 of 631 |
| A beat: simulation / picture's copy / sounds | 26.6 / 4.3 / 1.3 ms | 27.4 / 4.0 / 1.7 ms |

A beat's Sim::Tick and WorldPicture::Beat run on a thread of their own, started early by about as long as the
last beats took; frames shown meanwhile only show the picture (the interface, input and everything touching the
simulation wait for the beat). Input waits up to one beat's time (about 25 ms here, a few ms early in a game).

## 2026-10-15: the interface drawn only where it changed (02394a7, landed b63b407)

Scenario L, `MOHO64UI_FULL_REDRAW=1`-equivalent build before (e4b44a7) and after:

| | Before | After |
|---|---|---|
| Drawing the interface, per frame | 8.18 ms | 0.48 ms |
| of it bitmaps (374 a frame) | 8.05 ms | 0.36 ms (recording only) |
| The frame in all (average) | 16.85 ms | 5.45 ms |
| Showing it (renderer's DrawToWindow, upload) | 3.90 ms | 2.98 ms |
| Pixels painted | 100% | 1.3% |

Checked: 1050 frames of scenario L and 13,100 frames of three windowed test games (`MOHO64UI_DIRTY_CHECK=1`),
every frame the same as a full redraw, pixel for pixel.

Early game for comparison (2026-10-15, before the change, the rebuilt Seton's at start): 1600x900 4.3 ms a frame
(the interface's drawing 2.1 ms), 3840x2160 9.0 ms (3.7 ms).
