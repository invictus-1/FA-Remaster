# The interface's measurements

Before/after numbers of the interface track's performance work (docs/performance-lessons.md, ground rule 2:
commit, hardware, scenario, numbers). Newest first.

**Hardware (all rows):** AMD Ryzen 9 9950X3D, NVIDIA RTX 4090, the screen at 240 Hz, vertical sync on.

## 2026-11-12: the interface with 5,000 units (the scale gate, ui/tests/scale.lua)

A measure, nothing changed ("optimise at the end"). One scene: the suite's session_ai_cheats.lua (SCMP_009, 4
armies), 5,000 UEF light tanks made by the cheat callback, all in view; a run without a window, 1920x1080, pinned to
CPUs 16-31 (the half without the big cache), nothing else running. The test is the suite's job `scale`; it holds
each number inside a budget of three times and more, and logs each as a "TEST note". Two runs, the same to within
the spread shown.

**The interface alone (`--no-world`: no picture of the world is made):**

| State | A frame, game paused | The interface drawn once | The interface's share of a beat (*) | The simulation's beat meanwhile |
|---|---|---|---|---|
| Before the units | | | 0.8 to 1.2 ms | 0.3 ms |
| Idle, nothing selected | 2.2 ms | 5.5 to 6.1 ms | 17 ms | 18 ms |
| All 5,000 selected | +1.2 | +4.0 to +5.1 | +1.4 to +2.4 | 18 ms |
| Build mode (commander selected) | +0.3 to +0.7 | +4.4 to +6.1 | +0 | 18 ms |
| The economy overlay | +0 | +0 | +0 | 19 to 21 ms |
| All selected, 1,000 of them moving | +0.8 to +1.5 | +3.1 to +4.3 | +6.0 | 2,500 to 2,900 ms |
| The same with Shift held | +0.6 to +1.3 | +1.9 to +4.5 | +5.5 to +6.4 (the command graph itself: 1.0 ms) | 45 to 49 ms |

(*) The PC's time for the beats run, less the simulation's own time in them (`moho64ui_BeatTimes`): six frames and
what the program does for the interface after a beat.

Selecting the 5,000 (the call and the two frames after): 62 to 67 ms.

What it says: nothing in the interface grows by the square of the count any more (the command graph, 1 ms, was the
Shift freeze), and no state costs more than a few milliseconds over idle. The idle cost itself, 2.2 ms a frame with
5,000 units and nothing selected against next to nothing before, is the largest single number and is not attributed
yet. The review's reasoned 3.5 to 5 ms a frame for the economy overlay did not show (tanks have no economy numbers:
with 5,000 extractors it may); its 2.5 to 4 ms for build mode shows as 4 to 6 ms in the drawn frame.

**A correction (the first version of this section, on 3301700f, was wrong):** it gave "a beat's worth" as the PC's
whole time for a beat and read +40 to +51 ms for a big moving selection as the interface's cost. That time is the
simulation's: with a thousand tanks pushing through four thousand standing ones its beats swing from 30 ms to
2.5 s, and the rounds measured with the selection happened to fall on slower beats. With the simulation's time
taken out (the table above) the selection costs 6 ms a beat. The test measures the share now, not the whole.

**With the world's picture (the suite's usual way for a game: the picture made and read back):** a frame of the same
idle scene, game paused, takes 21 to 28 ms, of which "the picture's scene: Draw" in the binding profile is 16.3 ms.
The renderer's answer the same day: that is the read-back way's wait, not drawing work (Draw waits there for the
card to finish the frame and for the pixels to come back, on the slower cores); in a live window (his viewer,
SETONS_R, Chris's looks with ray tracing, 1920x1080, no vertical sync) the same 5,000 tanks cost about 4.3 ms a frame
over one tank, nearly all of it the card (2,995,000 triangles), under 1 ms of processor.

**The simulation with this crowd (the engine's, told to him as numbers):** ONE move order for all 5,000 tanks made
one beat take 50 s (measured with the world's picture, by the PC's clock; the click and its two frames 115 ms); with
1,000 of the 5,000 on a move order, the beats while they push through the others take 2.5 to 2.9 s each over six
beats. LIKELY the formation and the tanks' way through each other; not read.

## 2026-11-07: a skirmish's first beats inside the load (helper "startfreeze", on adf1c5d4 + its patch)

Chris's start as he plays it, measured in a window: SETONS_R, his game 18's table (his mods: Antares, Wyvern, our
reworks, our M28 variant) with its seven AIs made M28AI cheat, seed 7, 1920x1080, `--session`. The program pinned to
CPUs 16-31 (the half without the big cache: about 40% slower than Chris's), two other helpers' programs running
meanwhile; BASE and the patch one after the other, four pairs (the shader caches warm). Seconds from the launch call;
the hold and the screen from the long-frame log's stamps for BASE, the host's own line for the patch.

| | BASE (beats 2..29 after the load) | Patch (beats 1..29 in the load) |
|---|---|---|
| The load (the launch call to the load's end) | 19.01 / 11.72 / 12.79 / 11.81 | 13.74 / 12.41 / 12.00 / 10.64 |
| Held past the load (loading screen) | 4.60 / 3.50 / 3.00 / 2.80 | 0.07 / 0.07 / 0.09 / 0.06 (the screen's two frames) |
| The launch call to the game's screen | 23.61 / 15.22 / 15.79 / 14.61 | 13.81 / 12.48 / 12.09 / 10.70 |
| Beats 1..29 | 2.7-4.3 s behind the loading screen, after the load | 3.4-4.0 s on the loading thread, through 3.0-5.9 s before the map was ready |
| M28's start (beats 10, 21, 17, 16) | 0.96-1.44, 0.66-1.11, 0.36-0.60, 0.29-0.50 s | the same beats, inside the load |
| Longest frame, first 10 s of play | 183 / 136 / 120 / 111 ms | 214 (*) / 161 / 137 / 132 ms |
| Longest beat, ticks 30-130 | 94 / 74 / 103 / 75 ms | 77 / 83 / 87 / 67 ms |

The first pair's BASE was slow throughout (its loading thread 5.5 s against 3.6-3.9 s). The longest frame of the first
seconds is the first frame after the screen in both, the renderer's first-sight work (the next tier's models made
ahead, a commander's wreck, the compute pipelines' first use: 110-180 ms of "the world's picture"); with no hold after
the load it now comes 3-4 s sooner. (*) That run's first frame was 141 ms; its 214 and 180 ms frames came 8-9 s into
play, the picture's showing with nothing named. No freeze in the first 10 s of play either way.

Exactness (the patch's MOHO64UI_AHEAD_IN_LOAD=0 is BASE's way): the same session headless with the beats in the load,
after it (patch and BASE) and the in-load game's own replay played back: 1,201 fingerprint lines identical in every
part, the records byte for byte; two window games (BASE / patch, the world's picture on): 142 lines identical, the
records the same. The first picture after the load (held still by a pause): the same as BASE's but for what moves by
the clock (the surf, the pause button's pulse); before the map was given the session after the beats, a Seraphim
commander was drawn with another unit's model and icon (ui/README.md).

## 2026-11-06 evening: the held jump back without opening the map again (helper "rewind_r1", on 862b603 + its patch)
The held jump back (below) now begins the new session on the map the window shows (WorldPicture::RestartMap, render
c28c5ec: 6-11 ms) instead of opening the same map again behind. `MOHO64UI_REWIND_REOPEN=1` keeps the map opened again,
so both ran with one build, alternately; the same set-up as below (1920x1080 window, CPUs 16-31, MOHO64_PLACE=off). The
PC was quieter this evening: the same jump forward varied by under 8%.

| replay, jump | the map | click to the beat | the load through (the loading thread / the load finished) | the run ahead |
|---|---|---|---|---|
| 1v1 mp2, 5:02 -> 2:00, three pairs | opened again | 2.63 / 2.67 / 2.55 s | 1.84 / 1.90 / 1.85 s (1.43-1.50 / 1.77-1.84) | 0.66-0.75 s |
| 1v1 mp2, 5:02 -> 2:00, three pairs | RestartMap | 2.19 / 2.09 / 2.25 s | 1.45 / 1.40 / 1.47 s (1.33-1.40 / the same) | 0.66-0.74 s |
| 4v4 mp1, 20:02 -> 1:00, two pairs | opened again | 3.25 / 3.35 s | 2.35 / 2.31 s (1.53 / 2.14, 1.47 / 2.10) | 0.85 / 1.00 s |
| 4v4 mp1, 20:02 -> 1:00, two pairs | RestartMap | 2.52 / 2.45 s | 1.60 / 1.61 s (1.40 / 1.40, 1.41 / 1.41) | 0.88 / 0.80 s |
| 4v4 mp1, 20:02 -> 10:00 | opened again | 35.85 s | 2.36 s | 33.4 s (its jump forward 115.9 s) |
| 4v4 mp1, 20:02 -> 10:00 | RestartMap | 39.14 s | 1.77 s | 37.3 s (its jump forward 125.2 s: the PC 8% slower) |

So the load no longer waits for the map: it is through when the loading thread is (the rules, the simulation's start,
the game's state: ~1.4 s), 0.4-0.75 s sooner than with the map opened again on main (where the session's models now
take ~0.6 s after the loading thread, not the 2-3 s measured this morning). A short jump back is now ~0.5-0.9 s quicker
(4v4 to 1:00: 3.3 -> 2.5 s); a long one is still nearly all run ahead (the engine's snapshot is what is left).
- Exactness: the fingerprint after a RestartMap jump back (in a window) equals a straight playback's: the 1v1 at its own
  pace to 2:00 vs. forward to 4:00, back to 1:00, on to 2:00 (1201 lines identical in every part); the 4v4 forward to
  5:00 vs. forward to 6:40, back to 3:20, on to 5:00 (3001 lines, every hashed part identical; the "lua" part 1 KB apart
  on 6 beats, as with any session started over).
- The picture: the replay held at the beat (the viewer's pause asked with the jump), the camera set to the map's middle
  (zoom 520, heading held), the panels hidden, captured after 150 frames; a jump back vs. a straight run to the same beat,
  against two straight runs: 1v1 at 2:00, 7,415-16,084 pixels differ by more than 8 levels (two straight runs: 20,736);
  4v4 at 3:20, 95,405-107,098 (two straight runs: 40,741; the map opened again: 112,691). All of it is the animated
  water (shore waves, whitecaps) and the ground's wind-blown scatter, whose phase is the PC's clock since the map
  opened; nothing on units, wrecks, scars or decals.

Chris's third test: "skipping BACK reloads the whole replay". There is no snapshot of the simulation yet (the engine,
2026-11-06), so a jump back still starts the replay over and runs ahead to the beat; what changed is what the player
sees (GameSession::Rewind): the picture at the click held, dimmed, with "Rewinding to m:ss ... n%" over it, no
loading screen and no front end made anew, nothing of the world or the interface drawn while the replay runs ahead,
and the new interface first drawn at the beat. `MOHO64UI_REWIND_RELOAD=1` keeps the old way, so both were measured
with one build, one after the other. A 1920x1080 window, every look at its default, the process pinned to logical
CPUs 16-31 (the half without the big cache: the beats there run ~40% slower than on 0-15), MOHO64_PLACE=off;
script `run_measure.sh` (the replay started from the front end, a jump forward to the "from" beat, then the jump back
by moho64ui_ReplaySeek; times from the call to the beat reached). The PC was shared with the other chats' work: the
same jump forward varied by up to 20% from run to run, so the pairs matter more than single numbers.

| replay, jump | the way | click to the beat | the session's end | the load through | the run ahead |
|---|---|---|---|---|---|
| 4v4 mp1 (8 armies, 31:00), 20:02 -> 10:00 | old (restart shown) | 60.0 s | 0.30 s | 6.64 s | 5999 beats, 53.3 s |
| 4v4 mp1, 20:02 -> 10:00 | held | 51.9 s | 0.27 s | 4.99 s | 5999 beats, 46.8 s |
| the same, an earlier pair (250 ms chunks; the held run's jump forward 20% slower: the PC) | old / held | 55.4 / 65.3 s | 0.24 / 0.25 s | 3.97 / 5.67 s | 51.3 / 59.6 s |
| 4v4 mp1, 20:02 -> 1:00 | old | 9.84 s | 0.58 s | 7.50 s | 599 beats, 2.26 s |
| 4v4 mp1, 20:02 -> 1:00 | held | 7.08 s | 0.26 s | 5.15 s | 599 beats, 1.85 s |
| 1v1 mp2 (19:40), 5:02 -> 2:00, three pairs | old | 4.76 / 3.98 / 3.44 s | 0.25 / 0.14 / 0.13 s | 3.30 / 2.72 / 2.35 s | 1.40 / 1.21 / 1.04 s |
| 1v1 mp2, 5:02 -> 2:00, three pairs | held | 4.03 / 4.10 / 3.24 s | 0.07 / 0.09 / 0.08 s | 2.70 / 2.77 / 2.34 s | 1.26 / 1.26 / 0.85 s |

So the time is about what it was: the run ahead is nearly all of a long jump (the simulation's beats, ~8 ms each late
in the 4v4 on these CPUs, and every beat's sync to the new interface's scripts, 1.5-2.5 ms), and drawing was only ~5% of
it before (the last 4v4 pair: the frames' showing 3.2 s the old way, 2.0 s held, of ~50 s; the run ahead against the
same run's jump forward 0.32 old, 0.30 held). What the held way saves is small: the front end made anew (0.1-0.3 s) and the loading screen. Where the
rest goes in a load: the loading thread (rules, the simulation's start, the game's state) ~2 s, and the renderer
making the session's models for the map opening behind 2-3 s after it (on the 4v4), the same map it had open a moment
before (the renderer's R1 below). A jump forward is as before (only the held jump back takes the new path).
- Exactness: the fingerprint (MOHO64_FINGERPRINT, every beat) after a held jump back equals a straight playback's at
  every beat: the 1v1 played at its own pace to 2:00 vs. forward to 4:00, back to 1:00 held, then forward to 2:00
  (1201 lines identical in every part, headless and in a window); the 4v4 forward to 5:00 vs. forward to 6:40, back to
  3:20 held (and the old way), then forward to 5:00 (3001 lines with every hashed part identical; the "lua" part, the
  sim's Lua heap in KB, which the fingerprint keeps out of its total, differs by 1 KB on 6 beats between the process's
  first session and any started over, the old way's as the held one's).
- What is left for speed: the renderer's R1 (the same map kept for the new session: its models and pipelines are
  already made; 2-3 s of each 4v4 jump), R2 (no world prepared while hidden: the held frames' showing, 15-30 ms each),
  and the engine's snapshot of the simulation (a jump back without the run ahead).

## 2026-11-04 morning: the window's frame thread pinned to the big-cache half? No (main 751e6f3 + the switch)
In the engine's quiet window (its helpers' timing held, renderer and visuals light). Chris's big-test game (his 95-minute
8-AI game on SETONS_R of 2026-11-01, `_scratch/ui/measure_1104/bigtest_game_20261101.SCFAReplay`) played back in a 4K
window, every look at its default, `--fast-forward 18000`, then measured to beat 24000 (game minutes 30-40, ~10 min of
the PC's clock each); script `_scratch/ui/measure_1104/pin_run.sh`. The sim's beat thread pinned itself to the
big-cache processors in both runs (the engine's MOHO64_PLACE=sim default).

| frame thread | frame to frame, middle | 95% | 99% | longest | over 20 ms | world standing still |
|---|---|---|---|---|---|---|
| where Windows puts it | 11.20 ms (89 fps) | 12.93 | 13.62 | 131.1 | 16 of 53,334 | 0 |
| pinned to logical 0-15 (MOHO64UI_PIN_FRAME=1) | 16.40 ms (61 fps) | 19.25 | 21.09 | 122.2 | 786 of 40,599 | 0 |

Pinned, the frame thread shares the big-cache half with the beat thread and loses 46% (one round each, off first). So
the switch stays OFF by default and is kept for later trials (e.g. once the renderer's threads move). Also seen: this
late game now plays at 89 fps with no frame showing the world standing still (before the no-wait and fingerprint
changes Chris saw it choppy from 30 min; his own play has hovers and clicks a replay does not).

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

## 2026-10-28 evening: MOHO64UI_LEAD_BEATS=2 on trial (the same late game, the frozen build d909214)

The 14:01 run again (the same game to the last beat: fingerprints identical over all 27,007 beats), unpinned, the one
change a lead of up to two beats. Ticks 21001-27000: held frames 7,203 -> 6,612 (-8%, within the swing between
1000-tick spans: some better, some worse); the simulation, the beat's work on the window's thread (6.8 ms) and the
long frames (~16 a second, all the world's picture) the same. It does not help: the setting stays off. By the code
a beat whose whole chain (~77 ms with the fingerprint and the sounds) is well under a lead of ~130 ms should hold no
frame, so the held frames come from somewhere else: MOHO64UI_HELD_LOG=1 (a line for each held frame with where the
beats stood) is to find out. Also from the run's summary: the late game at 4K runs ~35 ms a frame in the middle
(under 30 frames a second), about 3 frames a beat, the world's picture taking most of each.

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

## The photo key (Ctrl-Alt-F12, `remaster_photo`): how long the world's picture stands still (2026-11-14)

Why: Chris's test of 2026-11-13, "Game freezes for a second". The renderer shows the world as it was while a photo's
frames are drawn (`WorldPicture::StartPhoto`: its settling frames, then the photo's), so the photo's draw time is the
time the game's picture stands still. In his game (4K full screen, upscale 0.59, a late game) the photo of twice the
view's size took ten frames of 75 to 146 ms, 0.93 s (`_scratch\bigtest\game_prev.log`, frames 546548-546557).

Measured here: a game from `session_ai.lua` at twelve seconds, headless at 3840x2160 with the world drawn, the test
template's preferences (every look on), the side build of 2026-11-14 06:10, pinned to CPUs 16-31; three photos each
way, 8 to 10 s apart; the numbers are the renderer's own (`LastPhotoNumbers`, now in the log's line "the photo's
frames: ..."). Three series: 06:06 and 06:10 (the engine's gate 164 ran until about 06:12) and 06:22 (seven of the
engine's programs were running): the CPU was shared each time, and the series differ by a factor of two; within
each series the two sizes are a factor of about three apart.

| The photo | Size | Frames | The picture stands still | Put together (on a thread) |
|---|---|---|---|---|
| twice the view, the renderer's supersampling (as until 2026-11-13; now `remaster_photo fine`) | 7680x4320, drawn once | 9 | 0.74, 0.82, 1.10 s (06:06); 0.42, 0.49, 0.91 s (06:10); 0.87, 1.00, 1.33 s (06:22) | 0.14 to 0.17 s |
| twice the view, supersample 1 (the first fix tried) | 7680x4320, drawn once | 9 | 0.92, 1.29, 1.42 s (06:06) | 0.14 to 0.15 s |
| the view's own size, drawn once (the key since 2026-11-14) | 3840x2160 | 9 | 0.15, 0.15, 0.28 s (06:10); 0.32, 0.38, 0.45 s (06:22) | 0.04 to 0.05 s |

What they say: asking for no supersampling changes nothing, because at 7680x4320 the renderer's own choice is
already to draw once (`by` 1): the cost is the size. A photo of the view's own size stands still for 0.15 to 0.45 s
here, about a third of the larger one's time in the same series. Not measured: his own late game in a window on his
screen (the log's new line will say it the next time he presses the key). What is left is the renderer's nine frames (eight settle the clouds and the light in the air at
the photo's size): fewer of them for a photo of the view's own size would bring it under a tenth of a second
(ui/REQUESTS.md 56).
