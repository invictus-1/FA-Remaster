# The interface program's switches (MOHO64UI_*)

Every environment variable `MOHO64UI_*` that moho64ui reads (`std::getenv` in `ui/**`). None is a player's
setting: a player's choices are options and preferences. They are for tests, for looking inside, and for
bringing an older way back to measure against. The program names every one that is set in its log's first
lines ("switches set in the environment"), and the lab's launcher check refuses a launcher that sets one that
is not allowed there.

This table is kept whole by a check (the lab's `switch_lint.sh`, run before every push): a switch read in the
code and missing here, or listed here and read nowhere, stops the push.

Kinds: **test** stands in for something a test cannot have (a screen, a window, a measured value) or saves
what a test compares; **look** writes something to read (a log, a profile); **old way** brings back the way
before a change, to measure the change again (no suite job runs it unless the last column names one);
**place** names a folder.

| Switch (after `MOHO64UI_`) | Kind | What it does | Set by |
|---|---|---|---|
| `AHEAD` | test | `0`: no beats behind the loading screen; `1`: those beats also in a run without a window (unset: in a window only) | suite (ahead jobs), wc_game_ab.sh |
| `AHEAD_IN_LOAD` | old way | `0`: a game's first beats after the load, not on the loading thread while the map opens | suite (ahead_after) |
| `BACKDROP_AT` | test | where along its path the front end's background camera starts (a number) | - |
| `BACKDROP_HOLD` | test | that camera stands still (pictures that can be compared) | - |
| `BEATS_HERE` | old way | a window's game runs its beats on the window's thread, not on a thread of their own | - (the measurements of October) |
| `BEAT_LOG` | look | each beat's times, our maxSP, and each wait of the interface on a running beat with the function that waited | measuring scripts |
| `BINDING_PROFILE` | look | every engine function the scripts call is timed (calls and time) | - |
| `CAPTURE_FRAME` | test | the frame (300 unless given) at which `--window-capture`'s and the second window's pictures are taken | tests/dualhead.lua's runs |
| `DEFAULT_LOG` | test | a run without a window writes the default log file, as a window does | suite (the log job) |
| `DIRTY_CHECK` | test | every frame's repaint of the changed parts is compared with a full redraw | window_checks.sh, wc_game_ab.sh |
| `DRAW_PROFILE` | look | where drawing the interface's picture goes, by kind of control | - |
| `FLIGHT_HAZE` | test | the menu flight's share of the haze (0..2; 0.4 unless given), for pictures side by side | - |
| `FLIGHT_REOPEN` | old way | the menu's map is always opened again after a game, never kept | - (the measure's other half) |
| `FORCE_MAXSP` | test | a local game's maxSP as if it had been measured | suite (spmaxsp) |
| `FULL_REDRAW` | old way | the whole interface is drawn every frame, not only where it changed | - |
| `GC` | old way | `classic`: Lua 5.0's own collector, not the incremental one stepped each frame | - |
| `HEAD1_SIZE` | test | with `HEADS=2`: the second head's size (`WxH`) | suite (dualhead) |
| `HEADS` | test | `2`: a second head in a run without a window | suite (dualhead) |
| `HELD_LOG` | look | a line for each held frame, with where the beats stood | - |
| `LOCAL_MOD_FIXES` | place | one more folder in which our fixes of mods are looked for | - |
| `LONG_FRAMES` | look | each long frame is logged with its parts and the functions that ran long | Chris's launchers (allowed: launch_switch_allow.txt) |
| `MENUFLIGHT_CAPTURES` | test | battle ticks (`600,1500,...`) at which the menu flight's picture is saved | - |
| `MENUFLIGHT_FAST` | test | `1`: the menu flight's battle runs as fast as it goes | suite (the flight jobs) |
| `MENUFLIGHT_FAST_TO` | test | fast only up to that tick | - |
| `MENUFLIGHT_FP` | test | a file (per loop) for the flight's fingerprints | suite (the flight jobs) |
| `MENUFLIGHT_GPU_MS` | test | the card's time a frame (33.3 ms unless given: 30 frames a second) over which the flight goes to Dusk | - |
| `MENUFLIGHT_PATH` | test | another camera path for the flight, without a rebuild | - |
| `MENUFLIGHT_SEED` | test | another seed for the flight's battle (1108 unless given) | - |
| `MENUFLIGHT_THREADS` | test | the threads of the flight's simulation | suite (the flight jobs) |
| `NET_BEAT_LOG` | look | a network game's beats: who was waited for and how long | suite/netlat_tools |
| `NET_EJECT_RULE` | test | `original`: the original's rule for a client that stops mid-beat, to show the desync ours avoids | net_game_run.sh |
| `NOTICE_CAPTURE` | test | the recovery notice's frame, read back from the screen and saved (`<file.png>`) | - |
| `OVERLAY_LOG` | look | the statistics overlay's lines in the log too (runs without a screen) | tests/dualhead.lua's runs |
| `PAINT_THREADS` | old way | at most n threads paint a big repaint (`1`: the frame's thread alone) | - |
| `REAL_TIME` | test | a run without a window is paced by the PC's clock, as a window is | suite/netlat_tools, the network runs |
| `RECORD_FINGERPRINT` | test | a window's game records its fingerprints too (a run without a window always does) | - |
| `REWIND_RELOAD` | old way | a replay's jump back loads the replay again, not the held session | - (the measure's other half) |
| `REWIND_REOPEN` | old way | a jump back opens the map again, not kept | - (the measure's other half) |
| `SCREENS` | test | a made-up list of screens (`2560x1440@240,1920x1080@60`) | suite (the screens jobs) |
| `SECOND_CAPTURE` | test | head 1's picture as the second window shows it, saved (`<file.png>`) | tests/dualhead.lua's runs |
| `SECOND_HEAD` | test | `1`: the second head counts as shown | suite (the screens jobs) |
| `SECOND_WINDOW` | test | `WxH`: a framed second window on the first screen (as `--second-window`) | - |
| `SHOW_ONLY_DURING_BEATS` | old way | frames during a beat only show the picture: no interface, input or camera | - |
| `SOUND` | old way | `waveout` / `waveout-small`: the old wave output (the small buffers crackle: to compare) | - |
| `SOUND_CAPTURE` | test | what the sound device plays, written to a file | - |
| `SOUND_LOG` | look | every sound played, with its loudness, category and wave | - |
| `SOUND_TOUCH` | test | `thread` / `here`: where a wave's bytes are brought in from the disk | - |
| `START_ONE_FRAME` | old way | a window's game shows its screen and runs its first beat in one frame | - |
| `STATE_HERE` | old way | the game's interface state is made on the window's thread at the load's end, not on the loading thread | - |

## Gone

| Switch | Was | Why it went |
|---|---|---|
| `MOHO64UI_LEAD_BEATS` | a trial of 2026-10-28: a beat may begin up to two beats ahead of the picture | measured, it did not help (docs/ui-benchmarks.md); removed 2026-11-12 |
| `MOHO64UI_PIN_FRAME` | a trial of 2026-11-04: the window's frame thread on the big-cache processors | measured, it lost (11.20 -> 16.40 ms a frame; docs/ui-benchmarks.md); removed 2026-11-12 |
| `MOHO64UI_SYNC_AFTER` | the old order (a beat's sync given to the scripts after the next beat began) | measured once, 3.7 -> 66.9 ms a beat at 4,000 units; nothing to measure again; removed 2026-11-12 |

## The old ways that stay, and when they can go

Each "old way" keeps code alive that no suite job runs. They stay until the measuring at the project's end
("optimise at the end") has used them or passed them by; then each goes with its code, one at a time, behind
a full suite.
