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

## 2026-10-23: the sound's wave banks mapped into memory (helper "banks", on 01d2b7c)

The first cue of a wave bank read the whole .xwb on the frame (13-15 MB for a faction's land, air or buildings). Now
every bank of /sounds is mapped into memory when the sound starts, as the original's sound engine does
(sound_system.cpp, at LoadFolder), and a wave's bytes are brought in from the disk on the sound system's own thread
the first time a cue plays it; the mixer takes the wave once its first part is in.

| | Before | After |
|---|---|---|
| a bank's first use on the frame, headless, 8 faction banks (warm file cache) | 15-22 ms each | 0.01-0.08 ms |
| the same with the wave's bytes touched on the frame (the runs without a window) | - | 0.05-0.8 ms, one 30.8 ms (a wave not in the file cache) |
| the same, cold (the hitches helper's numbers) | 13-163 ms | (only the wave, on the thread) |
| lab 4v4 replay, 3840x2160, frames whose sounds part was over 10 ms | 9 (13.9-31.7 ms) | 0 (longest 2.7 ms) |
| the same at 1920x1080 | 9 (longest 34.6 ms) | 0 (longest 2.6 ms) |
| the program's start: the banks taken | - | 25-78 ms (89 sound banks read, 78 wave banks mapped: 406 MB of files) |
| memory of the waves in the 4v4 | the banks read whole (a first try: 310 MB) | 17.7 MB of waves touched (the system's file cache) |

The 4v4: `LaunchReplaySession` of `replays/mp/mp1_4v4_scmp009_20261016.SCFAReplay` from the front end,
`--fast-forward 6000 --no-sound`, 2400 frames, `MOHO64UI_LONG_FRAMES=16`; the parts counted are "(beat: its
sounds)". The machine was busy with other sessions' runs, more during the "before" runs (whole frames vary; the
sounds parts are what is compared).

## 2026-10-23: the interface's hitches, menu to game at 4K (helper "hitches", on 5b6b332)

Scenario: the lab's 4v4 replay (`replays/mp/mp1_4v4_scmp009_20261016.SCFAReplay`) launched from the main menu
(`LaunchReplaySession`), a 3840x2160 window, 7200 frames from the menu through the loading screen into the game,
`MOHO64UI_LONG_FRAMES=16` (new: every frame over 16 ms logged with its parts and what ran long in it). The machine
was busy (other sessions' runs), so single frames vary; the counts below are each a hitch's own.

| Where | Before | After |
|---|---|---|
| main menu, the background's first live picture | 116-166 ms (its 32 MB kept picture written, and each picture converted, under the lock the menu's frame waits for) | none over 16 ms |
| main menu, first frame (the kept picture read) | 54 ms of it | read on a thread |
| loading screen at 4K (the faction's film) | 12 frames in 2.9 s, each 133-150 ms (the film stretched in software 115-127 ms, decoded 15-22 ms) | 160-250 frames; a film picture's frame about 20 ms (painted 8, decoded on a thread) |
| the "IN TRANSIT" cover fading at the game's start | 12 frames of 165-200 ms over 2 s | none over 16 ms (painted 22 ms with bands alone; its filtered picture kept: under 2 ms) |
| leaving a game to the menu | the menu film's 42 MB read again (30-55 ms) | its size kept from the first read |

What did it: a big repaint painted in bands of rows on up to 8 threads (`MOHO64UI_PAINT_THREADS`, draw/recorder.cpp);
the stretched-picture filter four channels at a time (SSE2, the same bytes) with its per-row and per-column work done
once; a big stretched picture drawn whole in two frames running keeps its filtered texels (only blended after); the
menu background's pictures swapped, not copied, outside its lock; big films decoded a frame ahead on a thread.

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
