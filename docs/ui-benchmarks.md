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

**A caveat for all of 2026-10-28's numbers (the engine, that evening):** the 9950X3D has two CCDs, one with the 3D
V-cache; a moho64 run on the other ran ~40% slower in every phase. None of today's runs was pinned (Windows placed the
threads), so a difference between two runs may partly be where they ran. From the next pair on: pinned to logical
processors 0-15, or the CCD logged.

## 2026-10-28 afternoon: the late game again, after the host changes (on 1026ec5)

The morning's game again (its table and seed 1392934588, no orders), the same full-screen 4K window and switches,
on main 1026ec5 (engine batches 22 and 23 + the beat's sounds decided on the beats' thread, the lead up to a whole
beat, the sync to the scripts still before the next beat): fast-forwarded to tick 21000, then real time to 27000.
Script `_scratch/ui/measure_1028/run_late.sh`, compared with `compare.py` over the same ticks. The game is the
morning's until beat 21695 (the fingerprints), then the engine's batches make it its own.

| ticks 21001-27000 | morning (9f12685) | afternoon (1026ec5) |
|---|---|---|
| sim per beat, mean / 95% | 47.3 / 62.5 ms | 45.5 / 61.1 ms |
| with the picture's copy, mean / 95% | 59.4 / 75.8 ms | 56.8 / 73.4 ms |
| beats of 100 ms or more (longest) | 9 (117 ms) | 70 (248 ms) |
| the beat's work on the window's thread, mean | 11.5 ms (sounds 6.7, sync 4.7) | 6.8 ms (sounds 2.0, sync 4.7) |
| frames held by the simulation | 11,225 (187 per 100 beats) | 7,203 (120 per 100 beats) |
| long frames (33 ms+) a second | 16.0, all the world's picture | 16.4, all but 12 the world's picture |

- Held frames a third fewer; the window's thread's share of a beat 11.5 -> 6.8 ms.
- A first try at noon gave the sync to the scripts once the next beat was begun: the scripts' sync handlers then
  waited on that beat (3.7 -> 66.9 ms a beat, held 33 -> 84 per 100 beats over 21001-22300); undone (227a351).
- Still held about one frame a beat: the frames take 30-40 ms in the late game (the world's picture), and a beat
  begins only in a frame and only once the picture has reached the newest beat, so it often begins a frame late.
  Next idea (not done): let a heavy beat begin as soon as the last is through, up to two beats ahead of the picture
  (the renderer keeps six), the lead learnt as now; orders then wait up to a beat longer in a heavy game only.
- The afternoon's long beats come every 10 ticks (one game second) from about tick 25000, ~200 ms each: M28's own
  air logic (ManageAirAAUnits once a second, quadratic in a big late air fight), not a regression: the engine's
  helper played late.fp back exactly headless, and batches 22, 23 and the old collector settings all play the same
  game with the same spikes. The fix goes into our M28 variant.

## 2026-10-28: a live 45-minute game at native 4K, as Chris plays now (the engine's item 20 follow-up)

Build: main 9f12685 (engine batch 21: the collector's spikes gone) + the first beats behind the loading screen
(36d43a0). Scenario: a live game from Chris's snapshot game 2's table (Seton's Clutch Remastered, 10 armies: 8 M28AI
variant AIx, Chris's army idle, no orders), full screen 3840x2160, D3D12, the simulation on all cores, the record and
its fingerprint on the beats' thread (`--replay-out`), `MOHO64UI_LONG_FRAMES=33 MOHO64UI_BEAT_LOG=1`, the PC quiet
(the engine's slot). Script: `_scratch/ui/measure_1028/run_live.sh` + `perf_live.lua`; read with `longframes.py` and
`phases.py` there. Ran to 45:01 of game time (tick 27012, 4,772 units) in 2728.5 s.

- Frames: 157,223 (57.6 a second on average); frame to frame 13.32 ms in the middle, 39.2 at 95%, 50.6 at 99%, 143.6
  the longest; 23% over 20 ms. Per frame 17.35 ms: the world's picture 13.69, the host's step 1.14, the interface's
  drawing 0.78, its scripts 0.40.
- Long frames (33 ms or more): 14,601. In 14,587 the world's picture is the biggest part; the host 2, the interface's
  scripts 1, waits 6, input 1. None of 250 ms or more, 36 of 100-250 ms.
- The beats (27,011 on their thread): the simulation 25.9 ms each (95%: 52.8), with the picture's copy 33.1 (65.2);
  then here on the window's thread, per beat, 10.1 ms on average (95%: 14.2) in the frames that carried one: its
  sounds 5.9, the sync 4.1 (the last 9 minutes 11.8, the sounds 6.9). The fingerprint no longer counts here.

| ticks | beats | sim mean | sim 95% | with picture 95% | frames held | beats of 100 ms+ |
|---|---|---|---|---|---|---|
| 1-30 (behind the loading screen from 2) | 29 | 85.9 | 591.8 | 592.5 | 0 | 4 |
| 31-3000 | 2970 | 6.2 | 11.4 | 14.3 | 4 | 0 |
| 3001-9000 | 6000 | 13.3 | 19.2 | 24.2 | 1 | 0 |
| 9001-15000 | 6000 | 18.4 | 25.2 | 31.6 | 1 | 0 |
| 15001-21000 | 6000 | 34.2 | 47.5 | 58.0 | 102 | 0 |
| 21001-27012 | 6012 | 47.3 | 62.5 | 75.8 | 11,266 | 10 (the longest 216 ms) |

- Frames held by the simulation (the world standing still): 11,374 of 157,223, all but 108 in the last ten minutes,
  about a third of the frames there. The beats are not over 100 ms there, but a beat's whole chain is: the
  simulation and the picture's copy (95%: 76 ms) on the beats' thread, its 10-12 ms here, and up to a frame's wait
  (a beat starts only in a frame), while it may start at most 80 ms early (GameSession::FinishBeat's lead cap).
- The start: the world loaded in 6.8 s; beats 2-29 behind the loading screen took tick 10 938 ms, 16 257, 17 352,
  21 592 (about 2.1 s hidden in the load); no frame held after the screen came. Five frames of 33-40 ms right after
  it waited ~30 ms each on the beat: the beats' early start had learnt from those slow beats (80 ms at once); FIXED
  after this run (the lead learnt afresh once they are through). One frame at 10.9 s had 93.7 ms of one beat's work
  here (about tick 48, the commanders' arrival); at 13.1 s FAF's StartupSequence selecting the commander took 20 ms
  (GetUnitCommandData 15 ms).
- Ideas (not done): the beat's sounds (6-7 ms late) and its sync work off the window's thread; a lead cap nearer a
  whole beat. The rest is the world's picture (the renderer) and the late simulation (the engine).

## 2026-10-25: the last long frames of a launch at 4K (helper "stutters3", on a1060c5)

Scenario: as below (the lead's measuring script, a 3840x2160 window, D3D12, `MOHO64UI_LONG_FRAMES=16`, 9000 frames),
BASE and the change one after the other, twice (2026-10-25 morning; earlier pairs the evening before agree). The
machine was busy (other sessions' engine runs): BASE's own numbers vary 30-50 % between runs.

| Where | Before | After |
|---|---|---|
| the frame the load ends in | 131-167 ms (CreateGameInterface 84-106, the setup's sync and the first beat 25-36) | two frames, 31-32 and 36-38 ms (CreateGameInterface 33-35; the first one SetupUI, StartGameUI, the setup's sync) |
| the lobby's first frame: the faction painting's first draw | 96-118 ms (its decode) | gone from the frame (decoded ahead; its copy to the card ~3.5 ms) |
| the game's first frame: the "IN TRANSIT" cover's first draw | 4-6 ms (its decode) | gone (decoded ahead) |
| CLobby:HostGame | 167-169 ms | 135-188 ms (unchanged: FAF's lobby scripts) |

What did it:
- The files the game's screen imports (134 of FAF's /lua/ files, the list in ui/app/interface_scripts.h) are compiled
  on the loading thread into the game's state after SessionInit.lua; the state's `doscript` (the engine's own,
  line for line) takes such a chunk once instead of reading and compiling the file again. CreateGameInterface
  loaded 137 files (1.6 MB) and their reading and compiling were 38-49 ms of its ~71 ms on a quiet run.
- In a window the start runs in two halves: SetupUI, StartGameUI, the setup's sync and beat 1 begun on the beats'
  thread in one frame (the loading screen still up); once that beat is through (a frame or two), the loading screen
  goes and StopLoadingDialog and CreateGameInterface run (the original's order, DoInitializing 0x88c3f0, a frame or
  two between its beat and StopLoadingDialog). The first beat's simulation collection (10-14 ms) is off the frames.
- The lobby's faction paintings (5 files, ~8 MB each) are decoded on a thread from the program's start, the factions'
  covers (4 files) on the loading thread (`DecodeTexturesAhead`, draw/draw.h).
- GetTopmostDepth reads each control's Depth as the original does (0x796680, LazyVar reading 0x783840: the value
  kept, evaluated only when there is none); no measurable change (5.5 ms in HostGame, the depths are new there).

Steady state, the 4v4 replay (mp1) at 3840x2160 (`perf.lua`, `--fast-forward 6000 --frames 30000`, every frame of at
least 1 ms logged; BASE and the change back to back, then the final build once more): the frames are the world
picture's (the renderer's "showing" was the largest part of all but 2-11 of the ~17,700-23,700 frames over 16 ms of
the ~28,700 counted; middle 18-23 ms a frame, 17-21 ms of it the world's picture). The interface's own parts a frame:
scripts 0.7-0.8 ms, drawing 1.2-1.4, host 0.8-1.2 (BASE and the change alike). Of the 11 frames in BASE whose largest
part was the interface's, 3 were the window's input (17-72 ms in the system's message pump) and 8 the scripts, all of
them waiting for a beat on its own thread that held a 35-57 ms collection of the simulation's Lua state, for chat: `SessionSendChatMessage` (947 calls in the run: FAF's scripts send in a replay too) asked the
simulation only whether there is one, and the delivery asked it for the sender's name. Both now take that from the
session's state and the last beat's copy: in the ~290 frames with such a collection on the beats' thread the scripts'
part was at most 35-50 ms before, 4.8 ms after; no frame over 16 ms is the interface's but 2 (a beat's sounds
after a beat, 20 ms, once; a drawing of 26 ms once).

Left: HostGame's frame (FAF's lobby scripts: InternalCreateBitmap x2639 20-26 ms, SetText x392 10, InternalCreateGroup
x963 6.5, Hide x607 5, mostly the scripts' own lazy variables and callbacks inside them; a full Lua collection 18-27 ms;
the map preview's first read 15 ms); leaving a game (one frame of 150-370 ms); the game's first frame's scripts (15-19
ms, its threads' first tick).

## 2026-10-24: the interface's long frames left at 4K (helper "stutters2", on 1994b04)

Scenario: the lead's measuring script (main menu 8 s, Skirmish, Launch Game, the loading screen, the game 8 s, back to
the menu), a 3840x2160 and a 1920x1080 window, D3D12, `MOHO64UI_LONG_FRAMES=16`, 9000 frames; BASE and the change run
one after the other, twice at 4K. The machine was busy (other sessions' runs): single frames vary, frames whose time
went to the window's input or the world's picture are not the interface's and are left out of the counts. "Frames
over 16 ms": those whose own work (without the pace's wait) was over 16 ms.

| Where | Before | After |
|---|---|---|
| the frame the load ends in, 4K | 347-459 ms (the game's state and its rules 254, CreateGameInterface 103, SessionInit.lua 64, Lua collections 19) | 84-92 ms (CreateGameInterface 56-62) |
| the same, 1080p | 345 ms | 87 ms |
| the lobby's opening, 4K | 30-32 frames of 16-38 ms (the faction painting and full-window fills painted again every frame) | none |
| the "IN TRANSIT" cover fading, 4K | 10-33 frames of 16-42 ms | 2: its first two (the filtered picture, before it is kept) |
| CLobby:HostGame, 4K | 165-233 ms | 131-150 ms |

What did it:
- The game's interface state is made on the loading thread right after the simulation's start, its rules copied
  into it and /lua/SessionInit.lua run there, as the original's "Map loader" task does (func_WorldSessionUserLoad
  0x885de0); a full Lua collection there too. On the loading thread it takes 180-250 ms of the load (the state 12-18,
  its rules 160-180, SessionInit.lua 50-60, the collection 11-19), unseen behind the loading screen.
- The interface's texture sizes are read ahead on a thread of their own from the program's start
  (maui/texture_info.h): CMauiBitmap:SetNewTexture inside CreateGameInterface 30.7 ms -> 2 ms (265 calls), inside
  HostGame 32-37 ms -> 5.5 ms (1882 calls); a map file's preview picture is kept (the lobby read the whole .scmap per
  call).
- The lobby's faction painting goes to the graphics card (OfferToCard in the front end too): it lay over the main
  menu's veil and a full-window fill fading in, which made every frame of the lobby's opening a full repaint.
- The kept stretched picture's blend and the fills four pixels at a time (SSE2, the same bytes as before: checked
  against the plain code on 160 million cases): the cover's band 5-9 ms -> 1-2 ms.

Left: CreateGameInterface itself (FAF's scripts, 56-62 ms at 4K on the busy machine); HostGame's frame (FAF's lobby
scripts, 130-150 ms, and the faction painting's first decode, 57-72 ms); the cover's first two frames.

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
