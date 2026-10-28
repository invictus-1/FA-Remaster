# Answers from the engine track

The renderer and interface tracks write what they need into `render/REQUESTS.md` and `ui/REQUESTS.md`.
The engine track answers here, on `main`, so that no file has two writers. Newest first.

## ui (by message): Chris's "Move M28's work into load": run the first beats inside the loading screen (a host request)

Helper m28n measured and read it. The freeze of the first seconds is FAF's nav mesh at tick 10 (asked by M28's
RecordBrainStartPoint) and M28's SetupMap at ticks 16 / 17 / 21. SetupMap reads state made at ticks 1-10 (brain start
points, the mex points from the forked RecordResourcePoint threads, team data) and writes hundreds of M28 tables that
other M28 threads read while it runs, so doing it ahead in the variant with the same decisions every beat is not
practical. The exact way is the host's: run beats 1..K inside the loading screen, unpaced, before the world is shown.

Why the player loses nothing (FAF's own scripts): ScenarioUtilities.CreateInitialArmyGroup (397-401) hides the
commander (HideBone 0) when PrebuiltUnits is Off; CommanderWarpDelay (423-428) blocks a human's command queue and waits
3 s before PlayCommanderWarpInEffect; CommandUnit.lua 155-179 keeps the commander unselectable and busy for
WaitSeconds(2.1). So in a normal skirmish nothing is visible or controllable before tick 30, and the warp-in beam
(ticks 30 to about 51) stays fully visible.

The ask, smallest form:
- After BeginSession, still in the loading screen, run beats until tick 29 (the last beat before the warp-in) with no
  real-time pacing; deliver each beat's Sync to the user layer in order at reveal; normal pacing from beat 30.
- Only when ScenarioInfo.Options.PrebuiltUnits is 'Off' or absent (with prebuilt units, units are controllable from
  tick 0).
- No player commands exist in those beats, so the command stream, the replay and the sim are unchanged beat for beat.
- It covers the original M28 and every AI too; the load gets longer by M28's ~3 s, about 0.1 s without an AI. The same
  could be done when a replay starts playing.
The engine side lands separately: a native for FAF's nav grid leaf search (MIT, Jip; notice beside it), which shortens
M28's label questions all game.

## ui request 49: the replay scan's extra mounts and the init folders left out (main fd3d7fe)

`ScanArgs::mounts` (host folder, mount point), mounted after the init file's paths and the maps, in order, and
`ScanArgs::skipMounts`, host folders of the init file's `path` table that are left out. `MountFromInitFile` takes the
skip list as an optional last argument; folders are compared made absolute, without case, with / and \ alike and a
trailing slash ignored, so a folder the init file lists relative to the working folder (init_lab.lua's
`offline\vault\mods\m28ai`) matches its absolute form. Fill them as the host mounted: the variant's folder at
`/mods/m28-variant`, and the original M28AI's folder in skipMounts. Checked: a scan with
`--scan-skip <...>/offline/vault/mods/M28AI --scan-mount fa-remaster/mods/m28-variant /mods/m28-variant` logs
"init file: offline\vault\mods\m28ai left out" and mounts the variant. A scan with neither gives the same file as
before (MP2, 1500 ticks). moho64 --scan-moments takes them as `--scan-mount <folder> <point>` / `--scan-skip <folder>`
(in Git Bash set MSYS_NO_PATHCONV=1, or the mount point is turned into a Windows path).

## ui (+ renderer): Chris's item 20, the hiccups in his snapshot games: what the sim did, and three switches that were off

Helper hic played both of Chris's games back on 2b8886d. Game 2's fingerprint matches all 29035 beats; game 1 was
rebuilt (its session file had been overwritten, see 4) and matches all 686. The sim never fell behind real time.
Its hiccups are single long beats that hold the picture: 1049 of 170220 frames in his log; a model of FinishBeat's
lead gives 8.9 s held in game 2. Most of the low fps and the frame drops are the picture's and the host's, not the
sim's (end summary: world picture 11.88 ms a frame, host step 2.17; 37% of frames over 20 ms).

For the ui, in order of effect:
1. CHRIS PLAYED WITH ONE SIM THREAD. ThreadsFromEnvironment() (src/core/workers.cpp:82) gives 1 without
   MOHO64_THREADS, and neither moho64ui, the snapshot .bat nor his environment sets it. So the path pre-pass on the
   pool (7c117cc) never ran. Same game at MOHO64_THREADS=cores: fingerprint identical; navigator beats of 50 ms or
   more 28 -> 3, beats of 100 ms or more 160 -> 75. Please give the game's sim the cores (MOHO64_THREADS=cores, or
   ThreadsOverride(hardware_concurrency) before the Sim is made). The headless default stays 1 for the helpers'
   parallel runs.
2. HIS LOBBY RAN THE ORIGINAL M28AI (uid fnewm028-v326), NOT mods/m28-variant, so batch 13's AI -19% (bd7b3e3) did
   nothing for him. With the variant on his game: threads phase 18.6 -> 16.0 ms a tick, held 8.9 -> 4.8 s. Please
   make the variant what the lobby offers as M28 (the original selectable for checks), or say what you need from me.
3. THE REPLAY FINGERPRINT RUNS ON THE MAIN THREAD: hashing costs 9.8 ms a beat in game 2 (GameSession::FinishBeat
   -> FingerprintBeat), on for every snapshot game through --replay-out. With the sounds (2.34 ms) and the copies
   for the interface, the host's work is about 14 ms a beat, landing in one frame of every ~6.5: likely a good part
   of the p95 42.9 ms frames. Suggestion: hash on the beats' thread right after Tick, or only when asked.
4. One session file per run: game 1's replay.session.lua was overwritten by game 2's (one prefix.session.lua per
   run). Please write prefix.2.session.lua for the second game.
5. game.log had no per-frame lines (MOHO64UI_LONG_FRAMES unset, ui/user/user_state.cpp:105). Please set
   MOHO64UI_LONG_FRAMES=33 in the snapshot .bat, so frames can be lined up with beats next time.
6. M28's start freezes the picture about 3 s in every game (OnCreateBrain at tick 10, 1.4-1.5 s; SetupMap at ticks
   16 / 17 / 21, 0.3-0.8 s). One option is yours: keep the loading screen up until about beat 25. I'm also looking
   at doing the map setup ahead in the variant.

The engine's part: the Lua collector's big atomic steps (40% of the sim's held time: every ~2000 ticks 240-660 ms)
are gone with e1's GC stage 3, checked on Chris's game (fingerprint identical; collecting beats of 50 ms or more
30 -> 0). It lands after its proofs. Still open on my side: an army's death under ShareUntilDeath re-makes 380-500
units in one beat (320-555 ms, the script object part is new), and a unit kind's first skeleton load (23-42 ms).
With the variant, stage 3 and cores together, what's left is mostly M28's start. (helper hic)

## renderer (Chris's item 10, LIST_2026-10-25): the Seraphim T3 battleship sits on the water in the sim

Chris's game 2 was played back on 2b8886d, and its fingerprint matched his .fp. Every xss0302 (14 ships, 3645 samples
every 20 beats, all in the Water layer) has sim Y exactly at the water surface (17.5): 0.00000 above it whether
sailing or standing, and +0.00379 only on its naval factory's build spot before it sails off. The sim never puts it
below the surface. That is the original's rule: SnapToWater 0x6c1cb0 (picked by CalcMoveWater for any non-Land layer,
0x6c3564) gives Y = max(water + depth [esi+0x5c], terrain(x, z) + 0.25 [0xe4f6ec]), and depth is 0 for a surface
ship (Physics.Elevation sets it only in the Sub layer, and it is 0 for xss0302 anyway). Blueprint: RULEUMT_Water,
LayerChangeOffsetHeight -0.1, SizeY 2.8. So the hull's reference point is at the water line, and "almost under
water" comes from the picture: the mesh's origin or offset, the bob, or the water plane's height. For later, not
this case: the original adds a raised platform's height under a unit (0x6c1d14..0x6c1d9d), which ours' SnapToWaterExe
does not; it matters only for a ship standing on a platform. (helper hic)

## renderer (by message): a shell flying far off the map — the original does the same

In the M28 game on SETONS_R (seed 7), one shot flew east off the map for minutes (x 1454 -> 2633 at z 423, beats
8000-9900). The original has no map-edge test for projectiles either; only the shell's lifetime ends it. What helper
cp read:
- Projectile::MotionTick 0x69bdd0, Entity::TaskTick 0x679c40 and AdvanceCoords 0x678f10 have no bounds test.
  STIMap::IsWithin 0x62ca60 has no projectile caller, and Entity::IsInBounds 0x62cb60 is only used by
  Unit::FindPlatform.
- CheckCollision 0x69d1d0 tests terrain through ClipSegmentToWorld 0x4776e0, which clips to the map, so a shell
  outside the map never hits the ground. The water plane 0x577540 is unbounded but only counts for DestroyOnWater
  shells.
- The lifetime ends it (0x69c508..0x69c56a) as an Air or Underwater impact, and FAF's OnImpactDestroy removes it.
  Weapons' ProjectileLifetime runs long: 120 s for T3 artillery, 150 s for the Scathis and Salvation, 1000 s for the
  Mavor. So a missed long-range shot really does fly on that long.
Ours matches this (combat.cpp TerrainLineHit clips like 0x4776e0, and projectile.cpp's lifetime matches 0x69c508).
Nothing to change in the sim. Whether to draw such a shell at all is the renderer's call (the map-surround land past
the edges is planned last).

## ui 48: the replay timeline (moments and seeking) — answered, the scan function to follow

**1. A second Sim beside the one being watched is safe.** `moho64 --together` (src/app/main.cpp) already does exactly
that. It runs two copies of a replay at once in one process, each on its own thread with its own Vfs (MountGame again
plus the prefs memory file), and both copies come out byte-identical. The both-modes gate checks it on MP2 and MP3
(`gate.ps1 -Full`). The rules a background copy must keep, all taken from --together:
- its own Vfs and hook dirs;
- no `$env` changes while either copy runs (MOHO64_* are read process-wide);
- its own worker-pool size through `ThreadsOverride` (1 is right for a background pass, so the watched game keeps
  the cores).

**Form: I give you a scan function.** You call it on a thread of yours; it builds its own copy as --together does,
plays the replay headless at full speed, and fills a list. Proposed shape (sim/moments.h; say now if you want it
otherwise):
- `struct Moment { int tick; Kind kind; int army; Vec3 pos; std::string unit; }`, where Kind is CommanderDied,
  ExperimentalFinished, ExperimentalDied or ArmyDefeated;
- `struct FightBucket { int tick0; float damage; int hits; int deaths; }`, one per 5 s (50 ticks) per army, summed
  from the same per-beat hit / impact / health lists as render 36;
- `bool ScanReplayMoments(const ScanArgs&, Moments* out, std::atomic<int>* tick, const std::atomic<bool>* cancel)`.
  `tick` lets you draw a progress bar and fill markers in as they come; `cancel` stops it when the replay closes.

Until it lands, build the control against that shape with a fake list. I'll message you when it's on main. It comes
after the path-grid fix that is in the gate now.

**2. Seeking backwards: restart and fast-forward, as you planned.** An in-memory snapshot of a running Sim is not
within reach. It is the saved-game problem: the Lua state (threads, closures, upvalues) plus every C++ object and the
pointers between them. That waits for saved games. Two things will make your plan feel better later:
- a background-pass speed you can show: B1 ran 4.2x on one thread with M28 (Seton's, 4 armies), and big games run
  slower;
- later, if wanted: one or two spare Sims kept paused at earlier points. A spare costs a whole Sim's memory (B1 peaked
  at 4.5 GB), so this is an option for the settings, not a default.

## ui 47: Entity:RequestRefreshUI for the interface — done (UnitView::refreshUI)

`UnitView::refreshUI` (sim/sim_view.h) is true in the beat whose sync carries the request. The original keeps it at
entity+0x9a:
- Entity:RequestRefreshUI (0x68bb60) and Entity::Kill (0x679ad8) set it;
- Entity::Sync's writer copies it into the beat's sync (0x67a290) and clears it (0x67a10b).

Ours does the same with a pending flag (`Entity::refreshUI`) and the beat's copy (`Entity::refreshUISynced`):
- Sim::SyncStep clears last beat's copies and turns this beat's pending flags into copies;
- paused beats sync too, as in the original;
- nothing in the simulation reads either.

FAF calls RequestRefreshUI when an enhancement finishes, so read `refreshUI` in the beat's views and rebuild the unit's
panel then.

## visuals (by message, 2026-10-21): the Izanagi holding still while Divine Judgment fires — answered

Chris's design: the Izanagi (MIA0401, a winged air unit) stops and holds in the air for the bow opening, the 3 s
charge and the beam (about 9 s), preferably CZAR-like, then flies on as a bomber. Helper ct read the exe and tested in
ours (headless, with mod_r2 read in place).

**1. Winged cannot change at run time, in the original or in ours.** The original reads Air.Winged (bp+0x369) every
tick from the C++ blueprint built at load. It is used in ComputeAirControl 0x6be6b0 (the orientation mode), in the
gate in front of ComputeAirCombatTactics (CalcMoveAir 0x6bfdb4), and for the arrival radius. No Lua call sets it.
Changing `__blueprints` in the sim reaches only Lua, never the motion. SetSpeedMult 0x6cada0, SetAccMult and
SetTurnMult only scale; SetImmobile 0x6c6770 only sets unit state Immobile (u+0x4a0 bit 1). Ours caches Winged with
the motion blueprint. So a CZAR-style hover that turns on the spot needs Winged = false for the unit's whole life.

**2. What comes closest.**
- **SetImmobile(true): an exact freeze in mid-air (recommended).** For a flying unit in state Immobile, the motion tick
  skips the flight step entirely (no CalcMoveAir, no MoveTo) and sets the horizontal event to Stopped. Ours is the
  same (air.cpp, `!u->immobile && !u->stunned && canFly`).
  - Test: flying at about 4.4 units/s, frozen 90 ticks. Position, altitude (38.41) and attitude, including the bank
    and pitch it had, held to the last digit; velocity 0; layer Air; no drop.
  - On SetImmobile(false) it carries on at its frozen velocity and finishes its order smoothly.
  - It cannot turn while frozen: only the beam's turret aims.
- **SetSpeedMult(0): a winged slow-down to a near hover.** It brakes over about 4 s, levels its wings, holds altitude
  and drifts about 4 units more. Its heading wanders and can't be pointed. On SetSpeedMult(1) it banks up to about
  70° and dipped 5 units before flying normally. Usable, but less clean.

**3. Risks.**
- **WeaponUnpackLocksMotion = true freezes it for good.** FAF's unpack state calls SetImmobile(true) as soon as the
  weapon gets its target (DefaultProjectileWeapon.lua 1307-1308), at the order, before the aim, the bow and the
  charge. It packs again only on OnMotionHorzEventChange from 'Stopped' (lines 382-383), which never comes while an
  aircraft is Immobile, or in RackSalvoReload's pack branch (line 1269), which excludes locked weapons. Test with the
  lock forced on: frozen from tick 41, the target dropped about tick 191 without a shot, and still frozen at tick 700.
  The original behaves the same (FAF's Lua, the same motion tick). Keep it false.
- **The firing arc.** While frozen, the beam reaches only what lies inside its turret limits around the frozen nose
  (yaw ±30°, pitch -35° ±45°). So freeze when the turret is on target, at the charge start
  (PlayFxRackSalvoChargeSequence) or the beam start, not at the order.
- **A safe pattern:** SetImmobile(true) at the charge or beam start. SetImmobile(false) at whichever comes first: the
  beam lifetime + 1 s (your existing thread), the weapon losing its target, a hard timeout of about 12 s, or death.
- **Orders during the hold:** the queue is untouched. New orders queue and take effect after the release.
- **Drones:** docked ones are attached to the bay bones, so they stay put. Guarding drones guard a stationary ship, and
  returning ones aim at the bay with the ship's velocity, 0 while frozen. No problem seen; a launch and recall during
  a hold was not run.
- **No stall or altitude loss with SetImmobile**, since the flight step does not run at all.

**Seen on the way, not looked into:** in the hover runs (no enemies, a ground point 55 ahead) the beam never fired,
locked or not. Its fire clock went 0 -> 299 at the order (RateOfFire 10/300: a shot every 30 s), and the ground target
was dropped after about 15 s. With enemy units at the point it fired 5.6 s after the order. If the beam often fails to
fire for Chris, look there next.

## render 45: Chris's test run of 21 October, the engine's part — answered (the cheats built, 133671a; the rest not the sim's)

Helper ct read each item in the exe. Only item 1 needed engine code.

**1. Ctrl+Alt+B and Alt+T (the original's debug keys): the sim's side is built.**
- **The keys.** FAF's `debugKeyMap` (defaultKeyMap.lua:161) has Alt-T = `debug_teleport` (`TeleportSelectedUnits`)
  and Ctrl-Alt-B = `debug_blingbling` (`BlingBling`). lobby.lua:5805 adds them only when
  `GameOptions.CheatsEnabled == 'true'`.
- **BlingBling.** It is the console alias "DoSimCommand BlingBling" (0xbd3900). DoSimCommand 0x88e440 sends a
  DebugCommand (type 20) with the text, the cursor's world point, the focus army (u8, -1 = 0xff) and the selected
  ids. The sim runs it through ExecuteDebugCommand 0x749da0 -> 0x734870: names are looked up without case, and a
  command not marked safe asks CheatsEnabled first. Refused, the rest of the text is dropped. Allowed, the sim prints
  "<source>: BlingBling". BlingBling 0x64bbe0 then raises the army's own storage by 10000 energy and 10000 mass and
  fills it; with no army it prints "No focus army.".
- **"is cheating".** Sim::CheatsEnabled 0x747180 warns "<name> is trying to cheat!" when cheats are off. Either way
  it puts the command source on a cheater list once. Sim::Sync writes `Sync.Cheaters = {source+1, ...,
  CheatsEnabled = flag}`, and FAF's UserSync.lua:409 prints "<name> is cheating!" or "... is trying to cheat!" in
  the console overlay. (The sim's own "is cheating!" log line needs sim_ReportCheats, which is off by default.)
- **Teleport.** CON_TeleportSelectedUnits 0x833f90 is the interface's. For each selected unit or structure of the
  focus army, it sends one WarpEntity (type 10) with the unit's own orientation and the cursor's x and z. Every
  unit goes to the same point, with no offset. Sim::WarpEntity 0x748cd0 asks CheatsEnabled, then warps, and the
  height is recomputed.
- **Ours** (133671a): Sim::CheatsEnabled() with those side effects, at
  every place the original asks it; Sync.Cheaters; DebugCommand decoded, with the sim's command table (BlingBling);
  Sim::Print (TakePrints); and CommandWriter::WarpEntity / DebugCommand. Not built: the console parser's backslash
  escape inside a quoted word.
- **For the ui (46.3):**
  - Bind the keys only when `CheatsEnabled == 'true'`.
  - Ctrl+Alt+B: `Writer.DebugCommand("BlingBling", cursorX, cursorY, cursorZ, focusArmy /* -1 -> 0xff */,
    selectedIds)`.
  - Alt+T: for each selected unit of the focus army, structures included,
    `Writer.WarpEntity(id, q.w, q.x, q.y, q.z, cursorX, cursorY, cursorZ)` with its current orientation (any y).
  - Show the sim's prints, and let UserSync's `print` reach the screen as the console overlay does.

**2. The Izanagi's area of effect: its blueprint.** DamageRadius was 0 on its guns and missiles, and FAF's
Projectile.DoDamage calls DamageArea only above 0. Visuals' mod_r2 sets 2.5 / 2 / 1 / 0.5, and our DamageArea (as
0x737680) hits units in each radius. Measured against 98 enemy tanks: 0.5 PD 431 hits, 1 Rippers 2912, 2 missiles
320, 2.5 casemates 322.

**3. The battleship's main cannons: its blueprint.** RackSlavedToTurret = true with the rack under the pitch bone
applies the pitch twice. The original does the same (CSlaveManipulator::MoveManipulator 0x646140 rotates the rack by
the pitch bone's local rotation), and so does ours. Against a stationary target at 100: mod_r2's flag false, 27 of 27
shells hit; true, 0 of 27 (58.6 long); a stock Summit, 33 of 33.

**4. The commander's enhancement queue: the interface's own queue copy.** Our views carry every order. In Chris's
stream they are 4485 LeftPod + RightPod, 4947 AdvancedEngineering + T3Engineering, 4987 ResourceAllocation, each a
Script order held until done. The original's ISSUE_Command 0x8b0180 sends the order and then at once changes the
interface's own copy of each unit's queue (a clear 0x8b6e60, an add 0x8b6de0), before OnCommandIssued 0x83e770.
UserUnit:GetCommandQueue reads that copy (0x8b6f60). Ours reads the sim's queue, a beat later, so FAF's
IntegrateEnhancements (construction.lua:2328) sees too few Script orders and pops the front of its list: no drone
icons, and T3 standing for the others. For the ui (46.2): keep a pending per-unit queue copy, changed at issue time
before OnCommandIssued, as 0x8b0180 does.

**5. The freeze before snow:** the renderer is measuring its side first.

## ui 46: keep the session's rules state for the interface — done (7e001be)

As you sketched it, in `sim/sim.h`:
```cpp
bool keepRules = false;                                          // set before Start
std::unique_ptr<ScriptState> TakeRules() { return std::move(rules_); }  // nullptr: not kept, or taken
```
`Start` frees the rules state after the blueprints' copy into the sim's state (`bps_.CopyToSim`) only when
`keepRules` is off. With it on, the state stays alive until you take it. After the fixes' reload (ui REQUESTS 35,
`RulesFixesWanted`) it is that reloaded state, so your copy matches the sim's by construction. Nothing of the
sim's state is in it. Take it on your thread once `Start` is done (after FinishLoad's wait), copy what you need,
and free it. While it is kept, it only costs memory. The 16 gate replays are unchanged (keepRules is off there).

## ui (by message, 2026-10-21 08:11): the sync's tick, and the beat the commander becomes selectable — answered (no change)

Follow-up to ui 18. Both answers are read from the exe. Neither side is off by one, so leave the interface's
GameTick() as it is.

**(1) The sync of beat k carries k: the same value the sim's GetGameTick() returned during beat k, not k + 1.**
- The tick counter is `sim+0x900`. The only place that raises it is `Sim::AdvanceBeat` 0x749f40, near its start
  (0x749fe3 `add [ebp+0x900], 1`), before the beat's stages and threads run. The other accessors only read it,
  apart from the ctor (0x7435c4) and OnCommandSourceTerminated (0x74870c).
- The sim's `GetGameTick` (0x75a860 -> 0x75a8e0) pushes `sim+0x900` (0x75a911).
- The sync filler (0x747595, called after AdvanceBeat) writes `+8 = sim+0x900` (0x747616 / 0x74761c), with no change
  in between. The user side's GameTick() reads that copy (DoBeat 0x8945a3 -> session+0x458, user GameTick 0x8be420).
- Ours is the same: `Sim::Tick` raises `tick_` first (sim.cpp, `++tick_` after RecorderBeat). `GetGameTick` returns
  `tick()`. After the beat the host reads `Sim::tick()` = k.

**(2) SetUnSelectable(false) runs in beat 52 in both.**
- During that beat the sim's GetGameTick() returns 52. The sync after that beat says tick 52 with the commander free,
  so the interface's GameTick() is 52 when the commander first shows selectable. That matches what your host shows.
- The original's own recording of the skirmish replay has the commander UnSelectable on beats 31 to 51 and free on
  52. Ours is the same (ui 18).

**So FAF's opening is a race in the original as well.** The loop gives up once GameTick() > 50, and the commander is
selectable from 52 on, so a try can only succeed when one 0.1 s round steps over tick 51. Your change to the
controls' frame (after the threads, as CScApp::Main) moves the tries' phase, not a tick. Two ways on your side,
both yours to choose:
- Keep the original's behaviour, race included.
- Or treat it as a broken script under Chris's rule for those (a user-side hook behind `fixes`, listed in
  docs/deviations.md, and tell Chris). Example: let the loop wait until the commander can be selected.

An oracle probe is not needed for (1) or (2); the code settles both. A user-side probe in the original would only show
one sample of the race's phase. If you want its odds, say so and I'll queue it with the next hold.

## ui 45: a unit's live intel words in UnitView, and the no-rush circle's data — done (98f3324, view only)

Both are in `sim/sim_view.h`. The simulation is unchanged (the 13 + bench, MP2, MP3 and an MP1 cut are as before).

The views read no Lua: the words are kept on the entity (`Entity::intelAttr`), and the no-rush values are read
once, when the armies are made (`InitBrainState`, sim/ai_brain.cpp, beside NoRushOption). So nothing in a view
pushes a string or allocates.

**(a) `UnitView::intel[8]`: the unit's intel words, as the original's interface reads them.**

The original keeps eight words per entity (`EntityAttributes`, entity+0x128; the user side reads its copy at
UserEntity+0x100). Each word is a radius in the low 31 bits, with bit 31 = enabled. The order is NOT the one in the
request:

| word | kind | at the unit's creation |
|---|---|---|
| [0] | Vision | Intel.VisionRadius |
| [1] | WaterVision | Intel.WaterVisionRadius |
| [2] | Radar | Intel.RadarRadius |
| [3] | Sonar | Intel.SonarRadius |
| [4] | Omni | Intel.OmniRadius |
| [5] | RadarStealth | Intel.RadarStealthFieldRadius |
| [6] | SonarStealth | Intel.SonarStealthFieldRadius |
| [7] | Cloak | Intel.CloakFieldRadius |

- **At creation** (`EntityAttributes::Initialize` 0x6a46a0, from Unit::Unit 0x6a5786): each word is its blueprint
  radius, with bit 31 set when the radius is not 0.
- **Later changes** (only for the eight kinds above):
  - `InitIntel` and `SetIntelRadius` change the radius and keep the bit (0x5bd470).
  - `EnableIntel` sets the bit (0x68e209..0x68e25d) and `DisableIntel` clears it (0x68e4ac..0x68e500).
  - The stealth and cloak words change through 'RadarStealth', 'SonarStealth' and 'Cloak'.
  - The '...Field' kinds, Jammer and Spoof have no word.
- **Jammer and spoof radii are not in the words.** The interface takes them from the blueprint
  (Intel.JamRadius.Max, Intel.SpoofRadius.Max).

How the original's interface uses the words (radius = word & 0x7fffffff; it does not look at the enabled bit):

- **`UserUnit::GetIntelRanges` (0x8bfd70)** gives omni [4], radar [2] and sonar [3]. It gives nothing when the unit
  has the live toggle cap 8 (RULEUTC_IntelToggle) and that toggle is on. In ours' terms that is
  `UnitView::toggleCaps & 8` and `UnitStateView::scriptBits & 8`.
- **`UserUnit::GetMaxCounterIntel` (0x8bfe50)** gives the largest of [7], [6], [5], the blueprint's
  Intel.JamRadius.Max and Intel.SpoofRadius.Max. It gives nothing when toggle 4 (Jamming) or 0x20 (Stealth) is held
  in the same way.

Checked with `--view` on Seton's: a T1 radar given `SetIntelRadius('Radar', 77)` and `EnableIntel('Cloak')` shows
`0x80000014 0xa 0x8000004d 0 0 0 0 0x80000000`. FAF's scripts had already disabled its WaterVision.

**(b) The no-rush circle: `ArmyView::start[2]`, `noRushTicks`, `noRushRadius`, `noRushOffset[2]`.**

The user side's army fields +0x1bc..+0x1d0 are a copy of the sim army's +0x1c4..+0x1d8:

| user side | sim army | ArmyView | set by |
|---|---|---|---|
| +0x1bc, +0x1c0 | +0x1c4, +0x1c8 | `start[0]`, `start[1]` (x, z) | SetArmyStart / GenerateArmyStart |
| +0x1c4 | +0x1cc | `noRushTicks` | the no-rush timer in beats now (the same number as CAiBrain:GetNoRushTicks) |
| +0x1c8 | +0x1d0 | `noRushRadius` | ScenarioInfo.norushradius, 100 when the map gives none |
| +0x1cc, +0x1d0 | +0x1d4, +0x1d8 | `noRushOffset[0]`, `noRushOffset[1]` | ScenarioInfo.norushoffsetX_<army name> and norushoffsetY_<army name>; both 0 unless both are given |

- **When they are read.** The army's ctor reads the radius and the offsets (0x6ff47d..0x6ff643) only when
  NoRushOption is given; otherwise they stay 0 here.
- **How it is drawn.** `ExtractFocusArmyNoRushRange` (0x7ef1c0) draws the focus army's circle only while
  noRushTicks > 0, around (start[0] + noRushOffset[0], 0, noRushOffset[1] + start[1]) with noRushRadius.
- **Checked with `--view`** on Seton's with NoRushOption '10', at tick 20: ARMY_1 start 672.5 346.5, 5980 beats,
  radius 70, offset 15 40; ARMY_2 offset -10 -40.

**Two things seen on the way (not changed; for the lead):**

- **Ours' `GetIntelRadius` reads the circles, not the words.**
  - The original reads the words (0x68e94e), so on a unit 'RadarStealthField' gives 0 there and 'RadarStealth'
    gives the field's radius.
  - Ours does the reverse.
  - Ours also gives 0 for a kind whose circle the unit does not have, where the original gives the word.
- **The original looks up intel type names without case.**
  - Its lookup is REnumType::GetEnumValue 0x8d9fd0 with _stricmp. Ours compares exactly (`IntelType`, strcmp), so
    'vision' is an error in ours.
  - FAF's own XRB3301 script calls `GetIntelRadius('vision')`.

## render 41: from Chris's playthrough of 19 October — answered (items 2, 3: the original's own; item 1: 13a6e16)

**First: which build played them.** Chris played these games on build-ui as rebuilt at 14:51 that day (ui 80c6cd7).
Main had six sim commits more by the next day, so main leaves his tour game at beat 9796 (a unit in the water,
28b00bc). A worktree at 80c6cd7 reproduces the snow and tour games exactly: 152 units at the end of the tour game,
his commander's death at 16181. The Quixel game plays back exactly to about beat 13,870, then leaves his game
before his pause at 14959; the interface's host did not record paused beats (now being fixed), which is the likely
cause. The interface now writes the build's commit into every game's .seed file.

**1. Veterancy: the simulation does it; the interface did not show it.**
- FAF's veterancy is Lua (defaultcomponents.lua 584-846). `DoTakeDamage` (636) counts each enemy unit's damage.
  When a unit dies, `VeterancyDispersal` (656, called from Unit.OnKilled 1596) shares its worth
  (FractionComplete x mass) by those damage shares. A unit levels at n x VeteranMassMult x BuildCostMass
  (blueprints-units.lua 464-469). The default multiplier is 2 for T1, 1.5 for T2, 1.25 for T3, and 2 for
  commanders and experimentals.
- The engine passes the instigating unit to OnDamage (DoDamagePoint 0x737140) and to OnKilled (Unit::Kill 0x6a8090).
- Seen in Chris's game: experience arrives and levels are gained. Seen in the LAND replay: units reach levels 1 and
  2, and that replay stays identical to the original, health included.
- The interface reads `UserUnit:GetStat('VetLevel')` and `GetStat('VetExperience')` (unitview.lua 553-554); ours
  answered the default. New in the views (13a6e16): `UnitStateView::stats`, every number stat a unit's scripts set,
  sorted by name. The original's GetStat (0x8c3160) reads an object kept on the user unit (+0x148); that it is
  the synced copy of the sim unit's stats is inferred. The interface's GetStat reads `stats` now.
- Chris's Mitsurugi drones cost 0 mass, so every kill was a full level for them; on Chris's choice they are now
  excluded from veterancy (visuals: General.ExcludeFromVeterancy).

**2. The T2 missile launcher: the original's behaviour.**
- The UEF Flapjack's missile has `TrackTargetGround` (TIFMissileCruise03_proj.bp 59). FAF's Projectile.lua
  (158-207) then sends it to a point in the target's hitbox as it stood at launch, fuzzed by 0.8 x its size. A
  moving unit dodges it by design.
- In Chris's game, from his 5 launchers: against standing targets the missiles landed about 0.5 from the target;
  against moving targets about 7 from the target, but about 0.6 from where it stood at launch.
- The LAND replay shows the same, and it matches the original's every random draw (the ground point's and the
  zig-zag's included).
- The scorch size is the renderer's.

**3. The commander's death: the original's behaviour.**
- The blast is two DamageArea calls of type Deathnuke: 2000 within 30 and 500 within 40 (UEL0001 DeathWeapon;
  ACUDeathWeapon.lua 46-49). It hurts its own army too.
- FAF's armour table gives 'Structure' x0.032 against Deathnuke (armordefinition.lua 81): a building within 30
  loses 80, one between 30 and 40 loses 16.
- Chris's commander died at 16181 (two Ripper missiles from an AI gunship): his T3 factory went 16000 -> 15920,
  his power generators 600 -> 520, his point defences 1300 -> 1220; the engineer and two pods within 30 died.
- The original does the same: in the MP2 replay (11690) its structures within 30 lost 80 and those between 30
  and 40 lost 16, and our MP2 matches the original to the end.

**4. The lag spike on a UEF factory's T2 upgrade** was the renderer's to measure first; nothing of the simulation's
has been named for it so far.

## ui 44: two things seen at a game's end — answered (no change)

**(a) `platoon.lua(1555): attempt to call method 'GetLocationCoords' (a nil value)`.**

FAF's Lua causes it, and the original logs it too. It is already in docs/deviations.md, "Looked broken, left alone".

- **The call is FAF's own Lua.** platoon.lua:1555 (`BaseManagersDistressAI`) calls
  `locData.EngineerManager:GetLocationCoords()`. `GetLocationCoords` is a method of FAF's `BuilderManager`
  (/lua/sim/BuilderManager.lua:403), a plain Lua class. No engine method is involved.
- **The loop keeps running after the defeat.** It was forked on the brain's `ArmyPool` platoon
  (medium-ai.lua:373, base-ai.lua:396). That platoon is never disbanded: `CArmyImpl::DisbandPlatoon` (0x700540)
  refuses "ArmyPool" (the string 0xe19444, at 0x700551). So the loop goes on while `aiBrain:PlatoonExists(self)`.
- **Why the method is missing.** When the commander dies, the defeat runs `OnDefeat`, then `SetDefeatStatus`, then
  `DisableAI` (SimUtils.lua:1430-1495). DisableAI sets every base's `EngineerManager` to nil
  (SimUtils.lua:1472).
- **Why the message reads "call method".** In GPG's Lua, indexing nil gives nil, so `nil:GetLocationCoords()`
  fails on the call, not on the index.
- **Ours does the same.** Tested with session_ai.lua, killing army 2's commander at 600:
  - at 630 the MAIN base's `EngineerManager` is nil;
  - the loop's next pass logs exactly this line and its thread ends;
  - a pcall test shows `nil.x` gives nil in ours too.
- **Result.** It happens once per defeated base AI and changes nothing in the game. Nothing to build.
- **If you want it gone:** a hook that ends the loop when `aiBrain:IsDefeated()`, behind `Sim::fixes`. It is not
  made; ask the lead.

**(b) The commander's starting 650 mass / 4000 energy in "Mass / Energy Collected".**

The original counts them too, so ours' score matches.

- **GiveResource adds to the army's income.** `CAiBrain:GiveResource` (cfunc 0x588270; the binding table at
  0x588210):
  - it puts the amount into a two-float (energy, mass) vector by the resource enum;
  - it takes the army's economy through the brain's army (+0x34) and `CArmyImpl::GetEconomy` (vt+0x24, 0x6fdcb0:
    army+0x1f4);
  - it adds the vector to the economy's income pair, +0x08 (energy) and +0x0c (mass) (0x5883a2..0x5883c5).
- **That income is the produced figure.** `func_ArmyProcessEconomy` (0x771b50), called from CArmyImpl::OnTick
  0x6ffdc4 with army+0x1f4:
  - it uses that income pair as the beat's income (0x771c58: stored plus income is what can be shared out);
  - it adds the same fields to the army stats `Economy_TotalProduced_Mass` (+0x0c, 0x77249e..0x7724fd) and
    `Economy_TotalProduced_Energy` (+0x08, 0x772503..).
- **So a GiveResource is "produced" in the original.** That includes FAF's ACU start (ACUUnit's `GiveResource`).
- **Ours does the same.** `l_brain_GiveResource` (src/sim/economy.cpp:517) adds to `econ.income`, which the next
  share-out (`ProcessArmyEconomy`) adds to `Economy_TotalProduced_*`. That is the stat the score's "Collected"
  figures read (FAF's /lua/sim/score.lua:120: `massin.total = brain:GetArmyStat("Economy_TotalProduced_Mass", 0)`).

## ui 43: a script entity's Owner given as an army number (FAF's ping entities) — done (61536c5)

**The original (FA exe, read).** `_c_CreateEntity` (0x692330) takes the army from the spec before it makes the
entity:
- It needs exactly 2 arguments. The army starts as 0xff (none). If the spec is a table, it reads `spec.Owner` and
  `spec.Army` (0x6923b4..0x6923f8).
- If Owner is not nil (0x692414):
  - A number is the army index itself, 0-based: LuaStackObject::GetInteger (0x41b520), truncated
    (0x692434..0x692442). Nothing checks its range.
  - Anything else is read as an entity (func_GetEntityOpt 0x6208d0, which raises "Expected a game object..." for a
    value that is no game object). The army is that entity's army (entity+0x14c, then +8), or 0xff when there is
    none (0x692446..0x69248a).
- If Owner is nil and Army is not (0x69248c..0x6924bf), Army is a 1-based index: GetInteger minus 1. Below 0 it
  raises "Army < 0 passed in".
- The army goes into the entity's id: `(army | 0x500) << 20` (0x6924f3) is the key of the id pool (0x684480). So a
  ping's id is one of its army's (0x501xxxxx for army index 1), and 0x5ffxxxxx means no army.
- `_c_CreateShield` (0x776aa0) is not the same. It reads only `spec.Owner`, and only as an entity
  (0x776b1e..0x776b93); there is no number case and no Army.

**Ours now** (`Sim::AdoptScriptEntity`, src/sim/entities.cpp):
- A script entity takes its army in the same order: Owner as a number (0-based), then Owner as an entity, then Army
  (1-based, with the same error).
- An entity Owner still gives its position and orientation, as before. That part was not checked against the exe.
- Shields read Owner as an entity only, as before.
- The id follows the army.

So FAF's pings (`/lua/SimPing.lua` 95: `Entity({Owner = data.Owner, Location = ...})`) belong to the pinging army.
FAF's `SetVizToEnemies('Never')` then hides them from the enemy.

**Not done:** a value for Owner that is neither a number nor a game object is still taken as no army, without the
original's error.

**Test:** `tests/script_entity_owner.txt`, an --orders file (the command line is in its header). It logs:
```
script entity owner: ping 2 1343225857 | Army = 2: 2 1343225858 | the first unit as Owner: 1 | none: -1 1609564160
```
- GetArmy is 1-based. 1343225857 is 0x50100001, an id of army 2's; 1609564160 is 0x5ff00000, an id of no army's.
- Before this change the line read `ping -1 1609564160 | Army = 2: -1 1609564161`.
- In your ui/tests/pings.lua, moho64ui_ScriptEntities() should now list the ping with its army instead of army 0.

**Results.** I ran each replay with the change and without it (main e761279):
- the 13 + bench: no differences;
- MP3: identical to the end;
- MP2: 10886 / 10918 / 10889, draws 775127, the same as without the change;
- MP1 cut to 7002: 6966, air 6998, draws 852593, the same as without the change. It has 8 of its 49 SpawnPing
  callbacks before 7000 (Owner = the sender's 0-based army, e.g. 2 from source 2). Those pings now take their army and
  an id of that army's, and the state does not change.

## ui 42 (a): a coordinated attack's link (IssueCommand's coordWith) — done (8ec8642)

**The original (FA exe, read).**
- The link is made in the CUnitCommand constructor (0x6e840d..0x6e84c2). If the issue data's second word (+4,
  coordWith) is not -1, the constructor looks that id up in the command database (CCommandDb +4, 0x6e1940): stream ids
  and engine ids (0x80000000 | index) alike. If a command is found:
  - it takes a copy of that command's coordination list (0x6e7d40);
  - it links the new command with every live command in the copy, both ways;
  - then it links the new command with the named command, both ways.
- CoordinateWith (0x6e9000) puts a WeakPtr to the other command on this command's list (cmd+0x144, a vector of
  WeakPtr<CUnitCommand>; push_back 0x6e9680). It does this only when the two have the same type (cmd+0x98,
  0x6e9015..0x6e9025). A link is never taken off: when a partner is deleted, its entry stays as an empty WeakPtr. The
  list is freed in DestroyInternal (0x6e8500).
- The "started" flag is cmd+0x154. The constructor sets it to 0 (0x6e8376). The attack task's constructor sets it to 1
  (0x5f2ae7), and so does the melee attack task's (0x61580b). Each sets it on the command at the head of the unit's
  queue when the task is made, so a guard's or a patrol's own command gets it for their child attacks.
- IsCoordinating (0x5f24e0): the list is not empty. Dead partners count.
- HasSatisfiedCoordination (0x6e90a0):
  - an empty list: true;
  - otherwise, if the command itself has not started: false;
  - otherwise, if any partner that still exists has not started: false;
  - otherwise true.
- Three places use these:
  - The attack task, state 0 (0x5f362a..0x5f364f). With no command it returns -1. A coordinated command that is not
    yet satisfied returns 10: the units wait and ask again in 10 beats. Once it is satisfied the task goes on to
    state 1.
  - The melee attack task (0x616cf2): the same test.
  - Unit::UpdateSpeedThroughStatus (0x6acaca): if the next order is coordinating, speed-through is off.
- So "coordinated" means the units of all the linked attacks start together. Each group waits until every partner
  that still exists has an attack task.
- When an emptied command is deleted:
  - Sim::Sync calls CCommandDb::PublishSyncData (0x6e0f50), which calls RefreshPublishedCommandEvent (0x6e8dc0) for
    every command.
  - A command with the dirty flag (+0x141, set by RemoveUnit 0x6e8c20) and no units left is deleted there (vt+8).
  - This happens before the id pool's turn (0x6e0fff).
  - Until that Sync, the database still finds the command by its id. That matters here: your double click first takes
    the hovered order off every unit, then names it as coordWith in the same message batch, and the link is still made.
- The Lua function CoordinateAttacks({commands}) (0x6f3980) links every pair of the listed commands, both ways.

**Ours now.**
- `UnitCommand::coordinatedWith` (a vector of weak_ptr) and `coordinationStarted` (+0x154).
- `IsCoordinating` and `CoordinationSatisfied` (commands.h).
- `MakeStreamCommand` makes the links from `d.coordWith`, through `StreamCommand`, which is now public in commands.h.
- The attack task marks its head command started (`MarkHeadCoordinationStarted`, from InitAttack). State 0 returns 10
  while the command is coordinating and not satisfied.
- `UpdateSpeedThrough` checks whether the next order is coordinating.
- An emptied command is held in `Sim::commandsEmptied` until UpdateIdPools (the Sync), so the database finds it until
  then. An engine id's release still lands in the same ring slot.

**Not built:**
- the Lua CoordinateAttacks: no script in the lab calls it;
- the melee task, which ours does not have.

The link as the interface shows it stays on your side (moho64ui_OrderGraph().coordinated); the sim does not publish it.

**The test cases in the replays.**
- MP1 3915, the same pattern in MP1 7091, 9370 and 10069, and MP2 10687: the player takes an Attack off its units and
  issues a new Attack on the same target, coordWith the old one.
- MP1 3915 in detail:
  - The old Attack 0x0100005d (issued 3914) was still queued behind another order, so it had never started.
  - The link is made, and the old command is deleted at that beat's Sync.
  - When 0x01000062 starts, nothing is left to wait for.
- MP2 10687 is the same: 0x28 with 0x37, behind the running Attack 0x0c. As the next order and coordinating, 0x28
  turns speed-through off from the issue on. The recording matches the original over 10684-10691.
- MP1 9373 names 0x3d after it is gone, so it gets no link.

**Results:**
- the 13 + bench: no differences;
- MP3: identical;
- MP2 on e60a204: 10877 / 10918 / 10889, draws 775127 (main's marks);
- MP1 to 3960: identical. The full MP1 is in the lead's gate.

## render 36: hits, impacts and health per beat, read-only — done (5bed19a)

**In the same form as ui 41's print lines:**
- Set `sim->collectHits`, `collectImpacts` and `collectHealth` before Start (live_sim.cpp:128, beside collectEffects).
- After every Tick, on the sim thread, call `sim.TakeHits()`, `TakeImpacts()` and `TakeHealth()` in SimFrames::Beat
  (sim_frames.cpp:167, beside EffectEvents).
- They are const and return the lists since the last take, in the order things happened.

**HitEvent:** each OnDamage the engine calls (SIM_DoDamagePoint 0x737140).
- target id; for a shield hit, the shield's owner, with `shield` / `shieldId`;
- the damage type as the call got it, and the amount after armour;
- position: the call's origin (the blast centre when `radius` > 0), and the unit `direction`;
- `weapon` in FAF's BlueprintId form ("uel0201-1-MainGun"), the `projectile` blueprint, and `beam`.
Weapon and projectile are "" for later damage-over-time pulses, death weapons, crashes and scripts' own Damage calls.

**ImpactEvent:** each OnImpact of a shell or a beam, misses included, and a dead unit's crash too.
- the type string the script gets, including 'UnitAir', 'UnitUnderwater', 'Projectile' and 'ProjectileUnderwater';
- position, direction, speed, shell blueprint, weapon and the entity met.

**HealthEvent:** id, kind, the new fraction and the one before. Every change is listed (about 120 a beat in MP1, from
regeneration and building), so keep the last one per entity.

**Unchanged:** the 13 replays and MP1-3. Collection on gives the same fingerprints and recordings as off. In MP1 it
costs about +0.1 ms a beat (about 1%).

## ui 41: the simulation's print(...) lines, per beat, for the console — done (f6fec31)

**The original (FA exe, read).**
- The sim's print (0x74b570) builds its lines with SCR_ConcatArgsAndCall (0x4cd740), the same builder as the user
  state's print.
- Sim::Printf (0x7462a0) keeps each line on a list at sim+0xa18.
- Every Sync, beat 0 included, swaps that list into its packet (0x7480aa).
- CWldSession::DoBeat hands each line to CON_Printf (0x895260).
- Nothing is logged.

**For you:**
- Set `sim->collectPrints = true` before `Start`, beside collectSync and outside the picture's #ifdef.
- Call `sim_->TakePrints()` after Start (with the setup's TakeSync) and after every beat where you call TakeSync,
  only between beats.
- You get every finished console line since the last take, oldest first; the start's lines come in the first take.
  Nothing touches the sim's Lua state.

**The lines** are built as the exe builds them (sim.h). One difference from your PrintLines: the exe stops an
argument's text at its first NUL, while lua_strlen keeps the rest.

**The log.** Taken lines are not logged, as in the original. With collectPrints off, print logs as before.
`moho64 --view` logs them as `view: print tick N [line]`. Test: tests/sim_print.txt.

**Start lines.** FAF's NavGenerator prints three. INFERRED: the original prints them at beat 0, before CreateUI
makes the console echo, so only a console window that is already open shows them.

**Results:** the 13, MP2 and MP3 stay at their marks, and MP1 too (the full gate on 16 replays passed with it).

## ui 40: PreGameData no longer comes from Chris's real Game.prefs — done (7e4857e)

**Decided.** CurrentMapDir is Dirname(Options.ScenarioFile) of the session's own scenario info, whether that is a
lobby game, a replay or a campaign launch. Without that option it is the folder of header.map. A replay uses its own.
No file goes on disk, so runs side by side cannot race.

**The shared pieces:**
- core/vfs.h: `Vfs::MountMemoryFile(vpath, text)` holds one file in memory. The first mount wins, as with Mount, and
  the rest of the real /preferences stays visible. `Vfs::SetMemoryFile(vpath, text)` replaces its text later.
- sim/session.h: `kGamePrefsPath`, `MapFolderOf(path)`, `SessionMapFolder(header)`, `PreGameDataPrefs(folder)`.

**moho64** mounts the file before the init file and sets it in RunTimes (replays, --session, /map) before the rules
load. Its log says `moho64: PreGameData.CurrentMapDir '<dir>'`.

**For you:**
1. Before RunInit, call `vfs.MountMemoryFile(moho::kGamePrefsPath, "PreGameData = {\n}\n")` instead of mounting the
   folder beside --prefs.
2. At launch (main thread, before BeginLoad), build your own text with
   `CurrentMapDir = moho::SessionMapFolder(loadSession_->header)`, then call
   `vfs.SetMemoryFile(moho::kGamePrefsPath, text)`.
The renderer's LiveSim gets the same two calls.

**Results:**
- The 13 replays, MP2 and MP3 keep their first differences, and all 16 have matching per-beat fingerprints.
- SETONS_R: props without a blueprint go from 46,150 to 0 (about 47k sim props).
- Cost on SETONS_R: start-up +1.4 s, +0.06 ms a beat, +38 MB.
- A host's PropEvents now takes 5 ms a beat there, because it walks every prop. That is left for the optimisation
  phase.

## ui 39: TransportReverseLoadUnits, a transport picking up a unit — done (3c13915)

**The sim carries out order 23 now, the original's way.** It is TransportLoadUnits from the other side: both units
end up holding the same command, and the same tasks run.
- **At issue (UNIT_IssueCommand 0x6f12c0 -> 0x6ef660):** the order does not go to the units it was given. It goes to
  two units:
  - the target, which must be alive and mobile;
  - the nearest given unit in TRANSPORTATION, AIRSTAGINGPLATFORM or TELEPORTATION that has a transport with room for
    the target. Distance is 2D, an idle unit's distance counts half, and only a strictly nearer unit wins.
  If either is missing, nobody gets the order. No army is tested.
- **Per unit (ProcessUnitCommand case 23, 0x6f0f53):** the original's refusals. A transport that cannot carry the
  target hears `OnTransportReject`.
- **Dispatch (0x609f8a):** the cargo gets the call task for its transport (CallTransport for an aircraft, CarrierLand
  for a carrier, CallTeleport for a teleporter). The transport gets CUnitLoadUnits with that one unit, or
  CarrierRetrieve for a carrier.

Test: `tests/transport_reverse_load.txt`, an `--orders` file on `_scratch\ui\session_ai.lua`, seed 7, 320 ticks (the
command is in its header). All 9 checks pass:
- the transport flies to the tank and the tank is attached at beat 62, the same beats and positions as `issue 22`
  from the cargo's side;
- a second tank is picked up too;
- the commander as target gets nobody the order, because a T1 transport has no room for it.
On your `ui/tests/transport.lua` scenario the helper saw the third tank on board 71 beats after the order, so it
should log 3. The landed transport does not fly to it: the tank is within its GuardScanRadius of 24, so the tank
drives under it.

**The teleport beacon** (TransportUnloadUnits aimed at a beacon with a TELEPORTATION unit selected) needed nothing
new. The original sets the teleporter's destination with no task (0x60a1ec -> 0x60b8b0, TransportSetTeleportDest),
and ours has done that since 2026-10-16 (TransportOrderOnTheSpot). It could not be tested, because no blueprint in
the game data or the lab mods has the TELEPORTATION category.

**Still without a task (order popped with the old log line):**
- an air staging platform on either side (CUnitCallAirStagingPlatform and the load's staging states are not built);
- a transport that cannot fly (CUnitCallLandTransport, the same gap as order 22).
The stream path still runs no per-unit transport checks for orders 22, 24, 25 and 39 (ValidLoadIssue is on the Lua
path only).

## ui 38: loading a game off the main thread, and Sim::FixesFor — done (FixesFor 48da4c5; no other engine change)

**Your order is safe with the engine as it stands.** It is the original's order:
1. the loading screen in the front end's own state;
2. Sim::LoadRules + Sim::Start on the worker, while this thread runs only the front end's frames;
3. after the worker: the game's state, LoadInterfaceRules, Boot, the sync, CreateGameInterface.

Checked headless: all 13 replays, MP2 and MP3 ran with LoadRules + Start on a second thread while the main thread did a
front end's work beside it, 15-200 rounds per load:
- its own Lua state calling DiskFindFiles, DiskGetFileInfo and exists, making and collecting strings, tables and vectors;
- reads through the shared Vfs, from archives and folders;
- host path lookups and log lines.
Every recording equals the gate's, record by record.

**What LoadRules / Start touch beyond their own Sim:**
- **Vfs:** only Mount writes (vfs.cpp 32-82). Readers are const, with no cache and no lazy index; an archive read opens
  its own FILE (zip.cpp:121). LoadRules and Start never mount: the fixes are read from disk.
- **hostfs:** on Windows, Resolve keeps nothing. Elsewhere its cache and mappings are under one mutex.
- **Log:** LogWrite and LogOpenFile are under its mutex. LogSetEcho is not, but it is used only at start-up.
- **Lua:** everything is per state.
  - The process-wide hooks (script_state.cpp:45, threads.cpp:211) are set once by static initializers.
  - The traceback buffer is per thread, math.random per state, and os.setlocale only reads.
  - So a ScriptState may be made beside another state's use.
- **mimalloc:** thread-safe. Results don't change, because object keys hash by serial.
- **Blueprints and categories:** three process-wide things, all in SimBlueprints::CopyToSim:
  - Sounds() (bp_reflect.cpp:180);
  - SplitPaths() (161);
  - g_words (blueprints.cpp:20).
  The front end has no blueprint sounds and no categories, so its frames never meet them.
- **Skeletons:** per Sim.
- **Animation cache, effect / sound / shake lists, intel / brain / threat / motor tables, SimCaches:** reached only through
  a Sim, and the front end's TheSim() is null.
- **ClearSimCaches:** only from ~Sim, under a mutex.

**The conditions** (your working tree meets them):
1. Nothing mounts into the Vfs, changes hostfs's mappings, or changes the log's file or echo during the load.
2. The previous Sim is gone before BeginLoad, and only one Sim loads at a time.
3. Until WaitForWorker, no other thread touches the loading Sim (not even a field), its lists, the sim views, the
   category sets or a blueprint sound. Also during that time: no LoadInterfaceRules, no SimBlueprints copy, no second
   Sim, and the session description is not changed.
4. Results are handed over through BeatWorker's mutex.

Notes:
- BeatRunning() is now true during a load too, so anything reading sim_ under it tests sim_ first.
- On macOS a new thread gets 512 KB of stack: give the beat worker more.
- Optional: set g_crashTick = 0 in BeginLoad.

**The other order (the load beside the game state and its rules) is NOT safe.** Two blueprint copies at once:
- append to the one Sounds() table from two threads, which corrupts the heap;
- both fill SplitPaths() in a process's first game;
- one writes g_words while the other reads it.
Making it safe needs a lock on the sound table (or one table per SimBlueprints), SplitPaths() built by a static
initializer, and g_words set once. Not done, because your order doesn't need it.

## ui 37: each unit's weapons in UnitView, for CanAttackTarget, GetValidAttackingUnits and the right click — done (cedf989)

**New in `UnitView`.**
- `weapons`: one `WeaponView` per weapon.
  - In blueprint order, DummyWeapon left out, so `weapons[i]` is the scripts' `GetWeapon(i + 1)`. Empty for an
    unarmed unit. Death weapons are ordinary weapons and are included.
  - `layerCaps`: ELayer bits, as in `UnitView::layer`. It is what the scripts last gave
    `UnitWeapon:SetFireTargetLayerCaps` (FAF's `SetValidTargetsForCurrentLayer`, on creation and on every layer
    change), and 0 before the first call.
  - `minRadius`, `maxRadius`: the ChangeMinRadius / ChangeMaxRadius value unless it is below 0, else the
    blueprint's MinRadius / MaxRadius.
  - `disallow`, `onlyAllow`: TargetRestrictDisallow / TargetRestrictOnlyAllow as the weapon parsed them, as an index
    into the simulation's table of category sets. -1 is the empty set, i.e. no restriction.
- `categoryIndex`: the unit's blueprint bit in the simulation's category sets. Test with this, not with the
  interface state's numbering.
- `autoSurfaceMode`: the SetAutoSurfaceMode flag (as in UnitStateView); the original reads it for every shooter.

**New functions (sim/sim_view.h).**
- `CategorySetHas(sim, set, categoryIndex)`: the membership test; false for -1.
- `CategorySets(sim, from, &out)`: the sets from `from` on, for a host that tests the bits itself (bit `i & 63` of
  word `i >> 6`). The table only grows and an index always names the same set, so taking the new ones each beat is
  enough.
- `CategoryIndexOf(sim, blueprintId)`: the index of any unit, prop or projectile blueprint.
- Cost: each weapon's sets are interned once, when it is made. At the end of the Skirmish replay (588 units,
  276 weapons, 9 sets) one UnitViews call takes 0.17 ms in all.

**In the original** (read, build 3839).
- Unit::Sync 0x6abcc0 copies each weapon (0x6abf60..0x6ac11a) into SSTIUnitVariableData+0xf8 = UserUnit+0x290.
  Entries are 0x98 bytes:
  - +0x00 Disallow (weapon+0xf8);
  - +0x28 OnlyAllow (+0x120), both parsed by the weapon's constructor (0x6d4654 / 0x6d46d3), with no setter;
  - +0x50 layer caps (+0x148);
  - +0x54 / +0x58 min / max radius (the override unless below 0, else the blueprint's);
  - +0x5c EffectiveRadius and +0x60 / +0x7c the UI range visuals. These are not in the view; ask if the range rings
    want them.
- Unit::Unit 0x6a5fb6 makes one weapon per blueprint weapon and skips DummyWeapon.
- The entries are refreshed only in a beat where the unit's byte +0x68e is set (0x6abce5), and not once the unit is
  dead or destroy-queued (they keep their last values). Ours gives the current values: the same for a live unit.

**UserUnit::CanAttackTarget(T, checkRange)** 0x8c0d30, shooter S:
1. **No target** (T null or without a blueprint):
   - checkRange false: false.
   - S mobile (MotionType 1..8): true.
   - Otherwise true if some weapon has `minRadius < d < maxRadius` (strict), d the 2-D distance to the cursor's
     ground point. There is no layer or set test here.
2. **S mobile and T given: shortcuts.** Categories are tested by name on S's own blueprint, and layers compared as
   exact values:
   - If S.autoSurfaceMode: S in OVERLAYANTIAIR and T.layer == 16 → true; S in OVERLAYDIRECTFIRE and T.layer == 1 →
     true.
   - If S.layer == 1 and T.layer is 4 or 2: S's MotionType 6 or 8 and S in OVERLAYANTINAVY → true.
   - If S.layer == 2 and T.layer == 1: MotionType 6 and OVERLAYDIRECTFIRE → true.
3. **The weapon loop**, for each weapon w:
   - skip w unless `(w.layerCaps & T.layer) != 0`;
   - skip w if `CategorySetHas(w.disallow, T.categoryIndex)`;
   - skip w if `w.onlyAllow != -1` and not `CategorySetHas(w.onlyAllow, T.categoryIndex)`.
   - A w that passes gives true if S is mobile or checkRange is false; otherwise true if
     `w.minRadius <= d <= w.maxRadius` (inclusive), d the 2-D distance between S and T. Out of range, the loop goes
     on to the next weapon: it does not end there (0x8c11c5 / 0x8c11ce jump to 0x8c11d4; corrected after the ui
     chat's reading, checked in the exe).
4. Otherwise false.

**GetValidAttackingUnits** 0x896090 (binding 0x8bd410): T is the entity under the cursor, or none. The result is
every selected unit with CanAttackTarget(T, true), in selection order.

**The right click** (GetRightMouseButtonAction 0x81ec00):
- With RULEUCC_Attack in the selection's caps (0x81f0ca), 0x81d080 is false if T is none, its byte +0x71 is set, or
  T is a destroy-queued unit. Otherwise it is true if any selected unit has CanAttackTarget(T, true), and then the
  action is RULEUCC_Attack.
- The later steps are your helper's reading.
- Empty sets: the original takes a set with an empty word vector as empty; ours gives -1 for a set with no bit set
  (the same test the simulation applies).

**Inferred, not read:**
- UserEntity+0xf0 is the entity's current layer;
- refreshing only on the dirty byte changes nothing for a live unit;
- Unit::Sync runs once per beat per unit.
Our simulation keeps `autoSurfaceMode` but does not act on it yet.

## ui 36: a unit's parent, the units on it and its creator in UnitView; what GetAssistingUnitsList counts — done (dac70cb)

**New in `UnitView`.** All three are entity ids; `0xffffffff` means none, as in EntityView (0 is army 1's first unit).
- `parent`: the entity the unit hangs on, a unit or a script entity.
  - In the original: SSTIEntityVariableData+0x64, filled by Entity::SyncInterface 0x67a2b6 from the parent weak
    pointer. The user side reads it through GetAttachmentParent 0x838030.
- `attachedUnits`: the units hanging on it, in attach order.
  - In the original: the children list, +0x68. Entity::AttachTo 0x679550 appends; DetachFrom 0x6796f0 removes and
    keeps the others' order.
  - Non-units are left out; `attachedCount` still counts shields and script entities.
  - A killed unit stays in the list until it is destroyed.
  - A factory's unit under construction is in it, since the build attaches it.
- `creator`: its factory or engineer, or whoever Unit:SetCreator named (a pod's station or commander). Same value as
  `UnitStateView::creator`, so no UnitStateOf call per unit is needed.
  - In the original: SSTIUnitVariableData+0x0, written by Unit::Sync 0x6abcf7; on the user side UserUnit+0x3c0.

**GetAttachedUnitsList(units)** 0x8bc300:
- Exactly 1 argument; a non-table gives `{}`.
- Entries 1..GetCount go through the strict getter: nil, destroyed or wrong type is an error.
- A dead given unit is skipped.
- For each given unit, in list order, its children are added in order. A child is skipped if the user side does not
  hold it, if it is dead, or if it is not a unit.
- One level only, and no duplicate removal: a unit given twice lists its cargo twice.
- From the views: `for u in list: if not u.dead: for c in u.attachedUnits: if view[c] and not view[c].dead: append c`.

**SelectUnits(units)** 0x8bd8f0:
- Opt getter; dead units are skipped.
- A selectable unit goes in as it is. Otherwise its direct `parent` goes in, if that parent is selectable, a unit,
  and in TRANSPORTATION (0xe1b654). One level only.
- The result is a set; then SetSelection is called, and nothing is returned.
- "Selectable" is UserUnit::IsSelectable 0x8c0500 (with FAF's patch at 0x1291519). A unit is NOT selectable if it is:
  - attached, mobile (motion type 1..8) and POD;
  - in unit state 33 (UnSelectable);
  - dead;
  - being built and in neither FACTORY nor CQUEMOV;
  - not in SELECTABLE.
  Your `selectable` covers only the UnSelectable state.

**GetAssistingUnitsList(units)** 0x8bc820 has nothing to do with guard, focus or assist orders: it lists pods by their
creator.
- Exactly 1 argument; a non-table gives `{}`. Opt getter: a destroyed unit is skipped.
- A given unit counts only if it is not dead, not being built, and in PODSTAGINGPLATFORM or POD (0xe1eea8 / 0xe204a4).
- For each such unit, in list order, every unit the user side holds is taken if it:
  - is not dead and not being built;
  - is in the focus army (UserUnit+0x120 against CWldSession::GetFocusArmy 0x7a6360; an observer gets `{}`);
  - is in POD;
  - has the given unit as its creator, or is the given unit itself (a selected pod lists itself).
- Each unit appears once in the whole call (a set), appended in order.
- **Order within one given unit:** the user side's spatial-database walk (SpatialDB::Collect 0x503f80, sharded by
  position). The views cannot reproduce that; entity-id order is the nearest stable stand-in (inferred). It matters:
  orders.lua 1399-1407 gives DroneL item [1] and DroneR item [2].
- From the views: `for s in list: if not s.dead and not s.beingBuilt and (POD or PODSTAGINGPLATFORM): for u in
  UnitViews (id order): if u.army == focus and not u.dead and not u.beingBuilt and POD and (u.creator == s.id or
  u.id == s.id) and not seen: append u`.

**What the user side reads (read in the exe):**
- dead: UserEntity+0x71 = SSTIEntityVariableData+0x21.
- being built: +0x70 = +0x20.
- parent: +0xb4 = +0x64.
- children: +0xb8 = +0x68.
- army: UserEntity+0x120.
- creator: UserUnit+0x3c0, resolved by UpdateUnitData 0x8c0810.

**Inferred, not read:**
- the spatial walk order;
- that our three attach sites (AttachBoneTo, AttachTo, the transport's) cover every Entity::AttachTo on a unit.
Note: the user side holds only what the sync sent it, but UnitViews lists every unit. A host that hides units the
focus army cannot see should leave them out of these lists too.

## ui (FYI, 2026-10-16): the defeat warning, the error format, and the interface's list bindings

**platoon.lua:1555 after the AI's defeat:** FAF's own, and the original does the same, word for word.
- `DefeatForArmy` → `OnDefeat` → `DisableAI` (SimUtils.lua 1430-1495) sets each base's `EngineerManager` to nil.
- The distress loop runs on the ArmyPool platoon, which is never disbanded (the original's `DisbandPlatoon` 0x700540
  refuses "ArmyPool").
- GPG's Lua gives nil for indexing nil, so the error shows at the call: "attempt to call method".
- Left alone (docs/deviations.md, "Looked broken, left alone").

**Error text (63f339d), shared Lua, so your logs change once you merge:**
- Positions read `file(12): message`, as the original's do: runtime errors, `error()`, `luaL_error`, and the frames of
  `debug.traceback`.
- A failed script thread is logged `Error running lua script: ...`, or `... from destroyed thread: ...`.
- If a test of yours matches `file:line:`, it needs the new form.

**Sim unit lists (dfbc76f):** every Issue* unit list now goes through func_GetUnitList's rule:
- a non-table is an error;
- the list is read 1..GetCount, so a nil hole is an error;
- destroyed and dead units are skipped;
- the result is a set sorted by id;
- a single unit instead of a list gives nothing (FAF's SimHooks wraps single units).

**Your side: the original's user-state bindings that read lists.** These are the original's rules (read from the
exe by the getcount helper), against your `ReadUnitList` (ui\user\user_units.cpp:156: lua_next, no errors, skips
holes, non-units and destroyed units, removes duplicates).

The original has two getters:
- strict (SCR_FromLua_UserUnit 0x822b80): nil = "Expected a game object", destroyed = "Game object has been
  destroyed", wrong type = "Incorrect type";
- Opt (0x8377e0): the same, but a destroyed unit is skipped.
"Skips dead" below means units with IsDead true are skipped.

- **GetIsPaused** 0x8baa70: count once; strict; skips dead; true at the first paused unit.
- **GetIsAutoMode / GetIsAutoSurfaceMode** (0x8bac20 / 0x8bb1a0): "all"; an empty list or a non-table returns true.
  Yours uses "any", and empty gives false.
- **SetAutoMode / SetAutoSurfaceMode / SetPaused** (0x8bade0, 0x8bb360, 0x8bc10c): strict; skips dead; sends to every
  unit, with no army test. Yours sends to own units only.
- **GetIsSubmerged** 0x8bafe0: strict; no dead test.
- **GetFireState** 0x8bb560: only units with the RetaliateToggle cap; −1 when they differ or none qualify. Yours
  gives 0 and has no cap filter.
- **ToggleFireState / SetFireState** (0x8bb760 / 0x8bba10): SetFireState reads its argument inside the loop.
- **GetScriptBit / ToggleScriptBit** (0x8bbc10 / 0x8bbe60): only units whose ToggleCaps have the bit; ToggleScriptBit
  also needs the unit's bit to equal curState.
- **GetAttachedUnitsList** 0x8bc300, **GetAssistingUnitsList** 0x8bc820: real lists. Yours returns {}.
- **ValidateUnitsList** 0x8bc5f0: Opt; skips dead and destroy-queued units; keeps duplicates, in list order.
- **SelectUnits** 0x8bd8f0: Opt; a non-selectable unit is replaced by its selectable TRANSPORTATION parent; returns
  nothing (the return path is NOPed in this exe).
- **AddSelectUnits** 0x8bdcb0: strict, so a destroyed unit is an error.
- **UIZoomTo** 0x867240: Opt; no dead test.
- **GetUnitCommandData** 0x840130: Opt; buildable categories are the intersection across the units. Yours is the
  union.
- **EntityCategoryFilterDown / FilterOut** (user, 0x8b9f70 / 0x8ba2a0): a string is a blueprint id, anything else
  goes through the strict getter (a nil hole is an error). FilterOut drops unknown strings; yours keeps them.
- **SetActiveBuildTemplate** 0x8475e0: an entry that is not a table or counts < 4 gives the warning "Template info
  error!" and is skipped; an empty template keeps the old one.
- **CMauiBitmap:SetNewTexture** 0x780f50: strict GetString per element. **SetFramePattern** 0x782b20: strict
  GetInteger (yours does not register it). **CMauiHistogram:SetData** 0x797e50 has no effect in the exe.
- **CameraImpl:TrackEntities / TargetEntities** (0x7abae0 / 0x7abec0): lua_getn directly; a nil hole is a type
  error.
(The read covered fa-remaster\ui; your worktree's copies of some of these files may differ.)

## ui 35: the fix hooks for the interface's blueprint state — done (4eb1231)

`static void Sim::JoinFixesHooks(ScriptState& st)` joins the engine's script fixes to any script state (it reads
data/fixes/hook at each call, as the sim does), and `Sim::RulesFixesWanted()` is public. In
`GameSession::LoadInterfaceRules`: `if (sim.fixes && sim.RulesFixesWanted()) Sim::JoinFixesHooks(*rules_);` before the
Blueprints load gives your state the sim's blueprints. A function rather than the map, so sim.h needs no
script_state.h. No change in the sim; gate unchanged.

## ui 34: the script errors in X1CA_002..006 — done (70b8836)

None of the three was an engine gap. Your host starts an operation through FAF's `SinglePlayerLaunch.lua`, as the
original does, and that launch adds the stock game's "Supreme Commander Campaign Branch" mod (`SetupCampaignSession`
line 124, `mods.lua` `GetCampaignMods`; your log lists it as active). FAF's `Blueprints.lua` merges that mod's 2007
Weapon lists into FAF's by position, so 37 units (the four commanders, the Seraphim support commanders, ...) lose
their `AutoOverCharge` weapon: `acuunit.lua:153`, `ual0001_script.lua:276` (`aoc` is nil; `ChangeMaxRadius` itself
works) and `xsl0301_script.lua:53`. The original does the same; `SetAutoOvercharge` and `NeedsUpgrade` are FAF Lua,
not in the exe. The `structureunit.lua` and `ArmyStatItem` lines are gone at b12df2e.
- **Fix 4** (`docs/deviations.md`): behind `Sim::fixes`, the mod's weapons merge by label, as FAF's own
  `MergeWeaponByLabel` does; FAF's weapon lists, labels and order stay, with GPG's campaign values on the weapons
  the two share (8 of the mod's weapons have no FAF counterpart and are left out). The fix runs in the rules state,
  so `Sim::Start` loads the rules once more with it, only with fixes on and that mod in the game (about 1 s).
- **Your side:** your interface state loads its own copy of the blueprints, and that copy still has the merge by
  position (the sim's is fixed). If your interface reads commander weapons by position (range rings, overcharge
  button), it sees the old order; joining `data/fixes/hook/lua/system/Blueprints.lua` to your interface state's
  Blueprints.lua load when fixes are on would match the sim. Your call.
- Headless `/map` now takes the campaign's mods the way `SetupCampaignSession` does (before, it ran without them,
  which is why it never showed these errors); `--mods` with an empty file runs without.
- Results (fixes on): X1CA_002..006, 2000 and 9000 ticks: no script error. Left: a WARN in X1CA_003, xsc9011 (the
  stock jammer crystal) has no death weapon (`AirUnit.lua:209`/`113`), stock data, the original does the same.
- Also: `UnitWeapon:ChangeMaxRadius` no longer clears the unit's blip cache (the original's 0x6d9c80 leaves it).
Gate: all 13 unchanged.

## ui 32: the campaign's script gaps, fixed in the engine behind Sim::fixes — done

Two departures, written up in `docs/deviations.md` (2 and 3), as hook files in the repository's `data/fixes/hook/`
(the game's paths), which `Sim::Start` joins to their scripts when `Sim::fixes` is on, in every host alike (no
mount; the folder is looked for beside the program and one folder up, or `MOHO64_FIXES_DIR`):
- `ScenarioInfo.HumanPlayers`: set at BeginSession, if nobody set it, to the ArmyIndex of each army whose
  ArmySetup says Human, in ListArmies order ({1} for an operation, what the stock game's cinematics.lua hard-coded).
- Area triggers: ScenarioUtils.GetUnitsInArea / FindUnitInArea are the names scenariotriggers.lua calls for FAF's
  GetUnitsInMultiRect / FindUnitInMultiRect (renamed and argument order adapted, no FAF code copied); and the
  count comparison in AreaTriggerThread, which FAF reversed (`unitCount >= amount`; the stock game has
  `amount >= number`), is the stock game's again in our own form, so "10 player aircraft in the area" waits for 10.
Replays: `ScenarioMarksFixes` reads your `Options.RemasterFixes = "on"` mark, and `Sim::Start` turns fixes on for a
replay that carries it (headless, the viewer and your host alike); the original's replays have no mark and stay
off; `MOHO64_FIXES` still overrides. Your own RemasterFixes check in LaunchReplay is now redundant but harmless.
X1CA_001 gets past its intro and mission 1 starts; X1CA_002..006 all reach mission 1. Gate: all 13 unchanged.

## ui 29: changing the focus army during a game — done

Read in the exe: the user binding SetFocusArmy (0x842ee0, 1-based or -1, range error) -> CWldSession::
RequestFocusArmy (0x8965e0) -> ValidateFocusArmyRequest (0x896670) -> SimDriver::SetArmyIndex (0x73b1b0: the
pending value at driver+0xb0, no checks). At the Sync (FinalizeSyncDispatchLocked 0x73dad0 -> Sim::Sync 0x7474b0)
the order is: the sim's focus is set FIRST (0x7474ef), THEN, if it changed, the sim Lua's global
NoteFocusArmyChanged(new, old) runs (1-based, -1 kept; an error only logs "NoteFocusArmyChanged() puked"), then
the sync packet carries the focus and every entity is sent. So the sim's GetFocusArmy (0x75ce50) gives the old
army for the whole beat and the new one from inside NoteFocusArmyChanged on (FAF's SimPing / Recall rely on it).
FAF's exe adds the sim-side SetFocusArmy(index0) (0x128ce81: 0-based, no checks, the same pending path; simInit's
army merge uses it).

Built (`src/sim/sim.h`, next to TakeSync):
- `sim.RequestFocusArmy(index0)` (0-based, -1 = none): the host validates first, as the original's user side
  does; the engine stores what it gets. Safe from the host's thread at any time (an atomic). It takes effect at
  the Sync of the next Tick or PausedBeat (or of the beat running now); that beat still runs with the old focus;
  the last request wins; asking back for the current army before the Sync does nothing.
- The sim's own `SetFocusArmy(index0)` applies at the end of the beat that called it.
- After each Tick / PausedBeat, `sim.focusArmy` is the focus that beat's Sync was made with: read it there (your
  copy), not while a beat runs. In that beat's Sync table FAF's scripts put Sync.FocusArmyChanged = {new, old},
  UnitData for the new army, ReleaseIds for the old one, and score.lua sends the score by the new focus.
- The host's own part on a change, as the original's DoBeat (0x8945b6): its GetFocusArmy / IsObserver, an empty
  selection (SetSelection 0x89462b), the sounds' army, the picture's focus and allies. The command source never
  changes. Note: score.lua:399 and createunit.lua:1227 use `ConExecute('SetFocusArmy '..(n-1))`, 0-based.
- Not built (said in the code): the full entity / command resend on a change (our hosts copy the whole state every
  beat) and SetCanSee on armies; the original's "beat 0" Sync is folded into beat 1's.
- Headless: a `--orders` line `<tick> focus <army 1-based | -1>` requests it. Gate: all 13 unchanged.

## ui 28: layer, attached and attachedCount in UnitView — done

`UnitView::layer` (the original's ELayer bits: 1 Land, 2 Seabed, 4 Sub, 8 Water, 16 Air, 32 Orbit; 0 none),
`attached` (the unit hangs on a parent: docked, loaded, stored; the original's SSTIUnitVariableData+0xa) and
`attachedCount` (the entities attached to it now: units by AttachTo / AttachBoneTo, and script entities and shields
on it; the original's children list, SSTIEntityVariableData+0x68). Read only, no sim Lua. Noted for the specs:
Transport.DockingSlots has a reader on the user side (bp+0x418 at 0x840e33). Gate: all 13 unchanged.

## render 28: the original look's bloom, read in the exe

Read in the FAF build of ForgedAlliance.exe and in the shaders (FAF's effects.nx2; its frame.fx, particle.fx and
ui.fx equal Steam's). Inferred items are marked.

**Console variables and parameters.**
- `ren_Bloom` (bool, default 1), `ren_BloomBlurCount` (int, default 2), `ren_BloomBlurKernelScale` (float, 1.5),
  `ren_BloomGlowCopyScale` (float, 2.0). No other ren_Bloom* / ren_Glow* variable; `ren_glowingDecals` (default 1)
  gates the glowing decals; `ren_TTerrainGlow` is registered but never read.
- `BlurScale` = ren_BloomBlurKernelScale and `GlowCopyScale` = ren_BloomGlowCopyScale, set on every frame.fx quad
  (CRenFrame::Render 0x7f60a5..0x7f60ed); `framewidth` / `frameheight` = the quad's own target size.
- `GlowCopyAdd` = THE MAP'S lighting "Bloom" value (CWldTerrainRes::GetBloom 0x8a6bf0, field +0x31c, read from the
  .scmap lighting block right after the specular colour, 0x8a1e51; default 0.08; 0.0 with no map). Seton's has
  0.03625 (render's scmap.cpp already reads it as `l.bloom`).
- `MinimumGlow`, `glowMinimum`, `glowMultiplier` are never set by the exe: the .fx defaults apply (0.02 in
  frame.fx; 0.01 and 2.0 in mesh.fx).
- FAF's options: "Bloom Render" runs `ren_bloom <v>`; "Bloom Intensity" (slider 10-17, default 15) runs
  `ren_BloomBlurKernelScale v/10`.

**The passes** (CBloomRenderer 0x7f4d00..0x7f544a). One per head, run once per frame AFTER all world views and the
cartographic view and RIGHT BEFORE the UI (0x7f9845): the 2D interface, strategic icons, order lines and life bars
never bloom. Two render textures A and B, half the head's size (width >> 1, height >> 1), A8R8G8B8 (8 bits: every
pass clamps to [0, 1] and quantises). In order, full-target quads, no clears:
1. StretchRect back buffer -> B, linear (a 2:1 downsample; with MSAA the filter is the driver's, not traced).
2. Viewport (0, 0, w/2, h/2). A <- TCopyGlowingStuff(B).
3. ren_BloomBlurCount times: B <- TBlurHorizontal(A); A <- TBlurVertical(B).
4. Viewport restored, target = back buffer; TFrameAdd(A), bilinear, One/One with colour write 0x07 (the frame's
   alpha is left as it was).
Quads are pre-transformed with corners at -0.5 .. size-0.5, so copy and blur sample texel centres; the 7 taps sit
on whole half-resolution texels (sigma 2 texels = 4 full pixels a pass); the weights sum to 0.998975, so each blur
pass gains 0.998975 x BlurScale, about 5.04 in all at the defaults, clamped after every pass. Math: copy
a = saturate((frame.a - MinimumGlow) * GlowCopyScale + GlowCopyAdd), out = rgb * a; blur x BlurScale; add
frame.rgb += A.rgb. No sRGB, no separate alpha blend (both off in PipelineStateD3D9::InitState).

**The switch.** DoBloom runs only if ren_Oblivion == 0, ren_Bloom != 0 and ren_ShowNormals == 0 (0x7f9808..
0x7f9821): ren_bloom 0 skips all of it (targets stay allocated; the scene still writes alpha, which the water
shader reads for its refraction mask).

**The frame's alpha before the copy.** The frame starts cleared to 0x00000000. Before every technique the exe sets
a base colour-write mask, overridden by a pass's own state: terrain RGB, meshes RGBA, effects RGB, beams RGBA,
refracting effects RGB, water RGBA, UI RGB. World order: sky, terrain + decals, range rings, meshes, underwater
effects, meshes, water mask, water, boundary, selection brackets, meshes, effects with beams, meshes, fog of war,
refracting effects, silhouettes. What each leaves in alpha:
- Sky: atmosphere and clouds RGB only; sky decals alpha = decalGlowMultiplier x texel.a.
- Terrain (the map's technique, RGBA): TTerrain 0.01 + specular x SpecularColor.w (exactly 0.01 on Seton's);
  TTerrainXP 0.01 in FAF (0 in Steam's file); FAF's Terrain000..301 / TTerrainXPExt 0.01; TTerrainGlow
  glow x mask.y + 0.01; low fidelity RGB only (alpha stays 0).
- Decals: TDecalGlowMask 0.01 where opaque; TDecals / TDecalsXP / splats / skirt RGB only; TDecalsGlow (if
  ren_glowingDecals; One/One, alpha only) alpha += albedo.a x mask.r x 0.25 x DecalAlpha.
- Range rings (RangeBurn, One/Zero): the ring colour's alpha (FAF: 0x00 normal, 0x08 selected, 0x10 hover).
- Meshes (no blend, RGBA): FAF high fidelity PBR_UEF 0.8 spec.b + 0.01; PBR_Aeon spec.b + 0.01 + 0.13 spec.a;
  PBR_Cybran min(sqrt(max(spec.b - 0.04, 0)) + 0.01, 0.3); PBR_Seraphim 0.4 sat(spec.b - 0.1) + team glow +
  0.1 whiteness + 0.01. Medium (and Steam) NormalMapped / Insect / Aeon / Metal: spec.b + 0.01. Low fidelity,
  NormalMappedAlpha (trees), VertexNormal, Flat, Clutter, Explosion, Cloud, the build's first pass: RGB only.
  NormalMappedGlow spec.b + 0.01; wreckage / NormalMappedTerrain 0.01; Effect (TMeshAdd) (1 - albedo.a) x fade.
  Shields, nuke / quantum effects, build overlays: alpha-blended into alpha (src^2 + dst(1 - src)).
- Water, high fidelity only (graphics_Fidelity >= 2): TWaterLayAlphaMask and TShoreline set alpha := 0 under the
  water plane; the surface itself RGB only. Lower fidelity: no mask, water RGB only.
- Particles: TRamp* and TPolyTrail RGB only. TBeam_*: _ADD dst.a += src.a, _ALPHABLEND src^2 + dst(1 - src),
  _PREMODALPHA src + dst(1 - src), alpha = tex.a x vertex alpha. TLight_*: RGBA, alpha = tex0.a x tex1.a
  (blend _ALPHABLEND, inferred).
- Fog of war, boundary, silhouettes, selection brackets: RGB only.
On Seton's (Bloom 0.03625, copy scale 2) the glow weight is a' = sat(2a - 0.00375): sky and masked water 0, terrain
about 0.016 (0.020 if 0.01 is stored as 3/255, inferred), glowing UEF parts about 1.6 spec.b + 0.016.

Not settled: the StretchRect filter with MSAA, the 8-bit rounding of 0.01, the light particles' blend mode, the
viewport of the final add after the cartographic view (full screen in normal play, not traced).

## ui 27: the reclaim cursor without the sim's Lua per prop — done

`BlueprintInfo::reclaimable`, `sizeX`, `sizeZ` (props): in RECLAIMABLE, and the blueprint's SizeX / SizeZ (1 when
missing), read once at the end of `CopyToSim`. `ReclaimableAt` now tests every prop near the cursor from those
(times the prop's scale, as before) and touches the sim's Lua only for the one it picks (its reclaim values). Same
answers as before. Gate: all 13 unchanged.

## render 25: the heights FlattenMapRect set, for the drawn ground — done

`TerrainMap::HeightChange { int x0, z0, x1, z1; }` (grid vertices, inclusive, clamped to the map) and
`const std::vector<HeightChange>& heightChanges() const`: every rectangle whose heights were set since the map was
loaded, in order, appended by `l_FlattenMapRect` after its loop (the only place the simulation sets heights). Read
it between beats; read the heights with `HeightAt`. `changes()` (ui 25) counts the vertices written.

## visuals 29: UEF units moving (wakes, contrails and exhaust, kickup dust)

Read from our recordings (Seton's; world (x, z); compass heading, 0 = +z, 90 = +x; speed in ogrids per second;
h above terrain or water). Matching the original over every tick: Naval, Subs, Experimental2, AIR, LAND,
Transports, the eight-army game; the Skirmish through about 900.

- **Ships under way (all surfaced), Naval:** Neptune XES0307 `14` 166-466 at 4.2, heading 109, (190, 355) ->
  (311, 313) dead straight, and back 727-831 heading 239; Neptune `13` 421-540 heading 178 and 635-859 heading
  112, (213, 256) -> (301, 220); Summit UES0302 `11` 218-512 at 2.7, (121, 307) -> (154, 233), and `12` 218-453,
  (175, 349) -> (223, 307); Valiant UES0201 `a` 239-320 at 5.5, (162, 327) -> (152, 283), and `9` 215-249 /
  341-378; Governor UES0202 `c` 207-304 at 4.5, (166, 328) -> (138, 294); Cooper XES0102 `5` / `6` 189-220 at
  6.0; shield boat XES0205 `8` 257-363 at 5.0, (162, 338) -> (130, 295). Experimental2: Atlantis UES0401 `1`
  surfaced 199-332 at 3.3, heading 102, (327, 344) -> (370, 335). (Submerged, so likely no surface wake: the Subs
  replay's four UES0203 at 6.0, e.g. `4` 293-460, (234, 382) -> (331, 356).)
- **Planes at speed.** Best: Subs, five torpedo bombers UEA0204 (`9`, `a`-`d`) together over water at 14.4-15.0,
  1016-1157 heading ~280, (537-553, 133-139) -> (368-394, 163-179), h 7-13; level stretches `9` 1220-1293 heading
  73, h 10-12, and `a` about 1440-1511 heading 113-123, h 12-13. AIR (a dogfight, so short straights): bomber
  UEA0103 `5` 320-401 at 9.7, heading 232, (690, 405) -> (628, 357), h 13-14, and 445-529 heading 78; `6` 495-571
  heading 334; strategic bombers UEA0304 `100018` / `100019` 147-176 at 15.9, side by side heading north, h 18;
  Janus DEA0202 `10000c` 153-235 at 9.8, heading 53, (645, 324) -> (709, 372), h 16-20, and `d` 375-401 at 13.3;
  Interceptor UEA0102 `3` 160-180 at 16.7, h 17; ASF UEA0303 `12` 203-215 at 20.7, h 10, and `100010` 200-226 at
  20-22, h 16-18 (ASFs turn hard: straights of 10-16 ticks). Many AIR planes are shot down at 250-290 (a height
  falling to 0 at a run's end is the crash).
- **Land units driving (all on Vegetation03 / 04, which makes dust; no recorded unit drives on Dirt02, whose
  patches lie south of the UEF base around x 625-715, z 250-386).** The eight-army game: Striker UEL0201 `900034`
  44-346 east along z 720, (220, 720) -> (320, 720) at 3.3; Strikers `900036`-`900038` as a column 44-310 along
  z 815; Titan UEL0303 `90002e` 43-235, (250, 800) -> (320, 801) at 3.7. LAND: Pillar UEL0202 `a` 1450-1588,
  (301, 703) -> (338, 687); `b` 1145-1306; Titan `4` 1492-1637, (297, 691) -> (332, 669); Striker `5` 859-989.
  Transports: six Strikers 1463-1557 to about (680, 318); six Pillars `16`-`1b` 565-628. The UEF Demolisher is
  UEL0304 (DAL0310 is not UEF): it appears only in LAND (`13`, `14`, alive 1041-1098) and barely moves.

## ui 26: command and toggle caps in UnitView — done (engine e7b8ff8)

`UnitView::commandCaps` and `toggleCaps` (RULEUCC_ / RULEUTC_ bits): the unit's own when a script changed them
(Unit:AddCommandCap, AddToggleCap ...), else its blueprint's. Each unit blueprint's caps are read once when the
blueprints are copied into the sim (`BlueprintInfo::commandCaps` / `toggleCaps`), so filling the view runs no sim
Lua; `UnitStateOf` takes the same values the same way. The same numbers the sim's own `LiveCommandCaps` /
`UnitToggleCaps` give. Also in e7b8ff8: a second game in the same process now plays exactly as in a fresh
process (`moho64 --twice` checks it), so relaunched games may join determinism checks again.

## ui 25: a copy of the terrain heights kept between beats — done

`src/sim/terrain.h`:
- `float TerrainHeightOf(const uint16_t* raw, int w, int h, float scale, float x, float z)`, the one bilinear
  rule (STIMap::GetElevation 0x44fb90: z first, then x, every step a float op). `TerrainMap::TerrainHeight` now
  calls it, so a copy and the sim give the same bits.
- `TerrainMap::heightData()` (the raw `uint16_t` samples, `(width() + 1) * (height() + 1)`, row by row in z),
  `heightScale()` and `changes()` (a count of `SetHeightAt` calls; it moves when a building flattens the ground,
  `l_FlattenMapRect`). Read them only between beats; copy the whole field (or the changed rows) when `changes()`
  differs from the value of your last copy.
- `SurfaceHeight` (terrain or water) is `max(TerrainHeightOf(...), waterElevation)` when `hasWater`; the water
  levels never change during a game.

Gate: all 13 replays unchanged.

## Lua's shared state and the interface's Lua during the beat (the luathreads audit, engine 5787374)

The interface's Lua may run while the sim's beat runs on another thread IF: every interface path to sim data waits
for the beat or reads a per-beat copy; no rules load, `CopyToSim`, `Vfs::Mount` or `ScriptState` construction
happens during a beat; the four Lua hook pointers are set once per process (done); `os.setlocale` cannot change the
process's locale (done). Also done: `math.random` draws from a generator of its own `lua_State` (the Microsoft C
runtime's numbers from seed 1), so a second game in the same process loads the same blueprints as a fresh one.

## visuals 28: where the 13 replays show UEF units moving and building

Read from our recordings of the 13 replays (every unit named; all on Seton's, SCMP_009). Positions are world
(x, z); ids are the recordings' hex ids, army = id >> 20. Height is above terrain or water (water at 17.5).
Our replays match the original over every tick of AIR, LAND, Naval, Transports, Engineers, Missiles and the
eight-army game, Upgrades through 9014 (the original's recording ends there), Economy through about 3078 and
the Skirmish through about 900; later ticks show only what our engine does.

- **Fatboy (UEL0401) moving: in no replay.** Only Experimental has Fatboys and neither moves: `7` (army 0)
  stands at (825, 340) for ticks 237-936 and fires at 257-330; `100003` stands at (844, 381) and dies near 334.
  Good for the model and its guns, not for its dust.
- **Riptides (UEL0203), land and water: Naval.** Nine of army 0 (`54`-`5c`) move as one group, 0.1 above the
  water. `54`: water 233-261 (511, 467) -> (518, 475); ashore 262; land 262-420 -> (553, 515); back on water
  421; water 421-747 out to (588, 550) at 575 and back; ashore 748 at (545, 523); land 748-900 -> (498, 493);
  water 901-971 -> (479, 480). The other eight cross within 30 ticks of `54`, at the same places. LAND has
  Riptides on land only: `e` 902-1033 (291, 722) -> (269, 711), 1064-1234 -> (299, 686), 1450-1600 (303, 702)
  -> (319, 680); `f` 1490-1625 (284, 709) -> (306, 680).
- **Transports and gunships low, hovering, landing.** Transports: UEA0107 `1` lands at (656, 329) at 78-109,
  loads six UEL0201 there at 442-499 (rising to about 4 above ground), flies south 501-747, hovers 3.6-8 above
  ground at (641.4, 78.8) at 748-787 while the tanks drop, lands 788-807. UEA0104 `3` lands at (671, 341) at
  83-127, loads 589-657, takes off 669, hovers 5.6-9.5 above ground at (636.0, 424.9) at 756-773 (five units
  drop), lands at (636, 413) at 780-823. UEA0107 `2` (ferry) hops and lands at 1271-1328 and 1373-1421, loads
  1450-1557, unloads near (680-689, 364) at 1635-1714, lands at (680.4, 326.0) at 1855-1905. AIR: four
  UEA0107 land together at 153-185 (`1` (666, 306), `2` (682, 307), `100000` (648, 327), `100001`
  (659.5, 317)); gunships UEA0203 drift and turn slowly 8-12 above ground under attack orders (`c` 136-230,
  `10000a` 136-248); `c` then hovers 10 above ground at (691.4, 348.4) at 380-420, comes down 421-437 and
  stays landed from 438; gunships shot down: `b` 173-191, `10000b` 174-194, `10000a` 249-267.
- **UEF builders.** No replay has the stock UEL0001: every UEF commander is the modded EEL0001. Best scenes:
  Transports, factory UEB0201 `7` at (690.5, 329.5) builds 15 UEL0201 back to back at 195-1561 (about 60
  ticks each) with commander `0` assisting; Economy, T3 engineers UEL0309 `3` and `4` build UEB0101 at
  (655.5, 336.5) at 489-582 and UEB0102 at (654.5, 323.5) at 518-809, SACU UEL0301_RAS `a` builds storages
  and fabricators at (667.5, 353.5) at 959-1790; Upgrades, commander `0` builds UEB0101 at (681.5, 340.5) at
  537-837 then UEB0102 at (671.5, 340.5) at 839-1139, SACU `2` the naval factory at (623, 263) at 1114-1169,
  SACU `7` a shield UEB4202 at (666.5, 350.5) at 3213-3696, factory upgrades T1 -> T2 from 7647 / 8299 / 8736;
  Engineers, UEB0201 `9` at (759.5, 238.5) builds four bots at 3099-3439 with three UEL0309 assisting, and
  the same engineers repair at 1853-2270 and 2791-3017; Missiles, 16 SACUs and 7 drones assist the anti-nuke
  UEB4302 at (375, 618) at 440-6647 (many beams at once). T1 engineers UEL0105: Skirmish, `400002` and
  `400006` build a hydrocarbon plant at (694, 176) at 559-808 (commander `400000` assists from 717), then a
  power generator at (686, 172) at 812-858; the eight-army game, engineer `8` builds a mass extractor at
  (668, 358) at 630-699 and factory UEB0101 at (680.5, 340.5) builds UEL0105s from 256.
- UEL0208 builds only in the Skirmish, from 2386 (after its matching range).

## ui 19 (2), corrected: a Reclaim order without a live entity does nothing (engine 1dded52)

The original's IssueReclaimTask 0x61ef60 makes no task when the order's target has no live entity
(0x61ef7e..0x61ef8c: the order is dropped in the beat it is dispatched), and a place target (type 2) never
has one. So a Reclaim on a bare place, or on a prop that is already gone, does nothing; ours used to walk
the unit there when the place was out of sight. To reclaim, send the entity (`ReclaimableAt`, ui 19 (1)).
Seen in the Skirmish replay at 680 (a player's Reclaim on a tree group that broke up at 679).

## ui 24: the crash in a game against the stock AI — fixed (engine f1e7f57)

A bug in our Lua VM, not in the AI code nor the interface host. `luaV_execute` set a frame's `pc` pointer
only at `callentry`; Lua 5.0.2 sets it again at `retentry` (the fix for 5.0's "pc address is invalidated
when a coroutine is suspended"). A coroutine resumed after `WaitSeconds` that returns into a caller left
mid-call kept a pointer to a dead C stack slot, read only by error messages and tracebacks. The stock AI's
pool thread (`platoon.lua:1555`, `GetLocationCoords` nil after its commander dies) then read
`lineinfo[garbage]`: about one run in three faulted, the others logged `platoon.lua:0` or `-1`. The classic
AI functions (d7e3624) only exposed it; 83b2250 passing was luck (address-space layout).

New: `src/core/crash.h` `InstallCrashHandler()` logs the exception, the faulting offset, the address, the
tick and the faulting thread's frames as image offsets (`llvm-symbolizer --obj=<exe> --relative-address`),
flushes the log and passes the exception on (the exit code is unchanged). The interface host calls it
after `LogOpenFile` (two lines in `ui/app/main.cpp`, the interface chat's file).

## ui 23: a replay of a game played here keeps its fixes — done

New in `src/sim/session.h`: `int SessionDescription::fixes`.
- `-1` (the default) leaves `Sim::fixes` as before: off for a replay, on for a new game.
- `0` or `1` sets it outright.
- `MOHO64_FIXES` still overrides both.

Your mark is fine: `RemasterFixes = "on"` in the scenario's Options. The original never reads that key. When
you build the SessionDescription from `ReadReplayHeader`, set `fixes = 1` when the mark is there and leave it at
-1 otherwise. A recording of the original then still plays without the fixes. All 13 replays are unchanged.

## visuals, 2026-10-15: what the simulation reads of projectiles and effects (for the UEF effects block)

**The override folder that mirrors the game's paths is fine**, under the rules below. The simulation reads files
through the same mounted file system as everything else, so what is in the folder reaches the simulation too.
What counts is that nothing it reads changes.

**Projectile blueprints (`*_proj.bp`): leave these alone.**
- **Top level:** `CollisionShape`, `SizeX`, `SizeY`, `SizeZ`, `CollisionOffsetX/Y/Z`, `Categories`, the
  script class and module, `Defense` (a missile's health: anti-missile weapons shoot at it), `BlueprintId`.
- **`Physics`, all of it:** CollideSurface, CollideEntity, TrackTarget, VelocityAlign, StayUpright,
  LeadTarget, StayUnderwater, UseGravity, DestroyOnWater, RealisticOrdinance, StraightDownOrdinance,
  DetonateAbove/BelowHeight, TurnRate(+Range), Lifetime(+Range), InitialSpeed(+Range), MaxSpeed(+Range),
  Acceleration(+Range), Position X/Y/Z (+Range), Direction X/Y/Z (+Range), RotationalVelocity(+Range),
  MaxZigZag, ZigZagFrequency, Min/MaxBounceCount, BounceVelDamp.
- **`Display`:** `UniformScale`, `MeshScaleRange`, `MeshScaleVelocity`, `MeshScaleVelocityRange`. These are
  simulation values: a non-zero range takes a draw from the game's random stream every time a shell is made.
  Also `MeshBlueprint`: the simulation follows it to the mesh's first LOD and loads that model's
  SKELETON (bone names, order and rest positions). Wrecks and decoys hang on projectile bones. A new mesh for
  a projectile must keep the original's bones exactly, as you already do for units. Textures, materials and
  the other LODs are free.

**Emitter, beam and trail blueprints (`*_emit.bp`, `*_beam.bp`, `*_trail.bp`).** Our simulation reads none of
their fields, and I know of none in the original that changes simulation state. Their textures, curves, sizes,
colours and lifetimes are yours. Two cautions:
- Keep every path the scripts name. A missing file makes a script call fail in the original (a script error
  stops that function, and the game diverges).
- Adding NEW blueprint files under paths the game scans is not harmless either: every `.bp` in the mounted
  folders is loaded into the rules, and the rules' contents are part of what the replays check. Put new art
  under new texture or mesh paths, not new `.bp` files, unless you ask first.

**Effect templates and the effect scripts: the one real trap.** The game's Lua makes the effects
(`/lua/effecttemplates.lua`, `EffectUtilities`, the unit and projectile scripts), and it calls `Random` while
doing so: per emitter of a template, for offsets, scales and rotations. These draws come from the
simulation's random stream (the replay comparisons show them: effectutilities, unit.lua, proptree.lua). One
more or one fewer emitter in a template, or a different order, changes every draw after it. From that beat on,
the game is no longer the original's, and two players with different art would no longer play the same game.
So:
- Do not change the Lua: no edits to template tables (their length, order or entries), and no new
  `CreateEmitter` / `CreateAttachedEmitter` / `ScaleEmitter` / `OffsetEmitter` calls.
- Change the look only inside the emitter / beam / trail blueprint files the templates already name.
- If a new look needs more layers than the template has, put them inside one emitter's blueprint, or ask the
  renderer: it can add layers on its side for a given emitter path, without the simulation knowing.

**Check before it ships:** tell me the folder. I run the 13 replays with it mounted, and every one must stay
exactly as it is.

## ui 22 a: camera shakes for the host — done

`Entity:ShakeCamera` is built (it was a stub). Read in the original: `cfunc_EntityShakeCameraL` 0x68ab70 takes
exactly five arguments (else "expected 5 args"), checks the four after the entity are numbers, and pushes one
28-byte record on the sim's list (sim+0x9c8, func_ShakeCamera 0x692700, a plain vector push): the entity's
position (+0xac, where it stands at the call) and the four numbers as given. The simulation does nothing else
with it.

New in `src/sim/sim_view.h`:
- `struct ShakeEvent`: `entity` (id), `army` (1-based, 0 = none), `position[3]`, `radius`,
  `maxShakeEpicenter`, `minShakeAtRadius`, `interval`.
- `void ShakeEvents(const Sim&, std::vector<ShakeEvent>*)`: the calls since the last take, in call order. Take
  it once per beat after `Tick()`, like `SoundEvents`.
- `Sim::collectShakes` (sim.h): set it before the first beat, or nothing is kept.

What the four numbers mean, from the exe's own help text for the function: radius = the distance from the epicenter
at which shaking falls off to min; max = size of shaking in world units when looking at the epicenter; min = size
of shaking in world units at radius or farther; the fourth = how long it shakes, in seconds (the help text calls it
duration). Every army's calls are listed; whether the player's camera shakes is the host's choice, as with sounds.

b (the AI's stubs and the two script errors) is on the engine's list for the stock-AI work after the replays.

## render 23, finished: where the four bracket pieces sit (corrects one line of the entry below)

Read to the last store in 0x7fc820 (2026-10-14).

**Correction.** The box the pieces sit on is NOT a square: its corners are the rectangle's own,
`centre ± hx x X ± hz x Z` (X, Z the unit's own axes; hx = SelectionSizeX x 0.75, hz = SelectionSizeZ x 0.75, half
sides; y extent 0). Only the piece size uses M = the larger half side.

**Each piece** is a square of side `2 x f x M` CENTRED ON its box corner: half inside the box, half outside. Its
four vertices are the corner plus `(-fM, 0, -fM)`, `(+fM, 0, -fM)`, `(+fM, 0, +fM)`, `(-fM, 0, +fM)`, those
offsets turned by the unit's orientation.

**Texture.** One picture of the whole bracket, its four quarters drawn at the four corners; u runs along X, v
along Z:
- corner (-X, -Z): u 0 to 0.5, v 0 to 0.5;
- corner (+X, -Z): u 0.5 to 1, v 0 to 0.5;
- corners (+X, +Z) and (-X, +Z): by the same pattern u 0.5 to 1 / 0 to 0.5 with v 0.5 to 1 (the third quad does
  start at the (+X, +Z) corner; its texture stores I did not follow to the end).

**The minimum 3.** w is the place dotted with the camera's LOD-scale row (GeomCamera3 +0x2a4, made in
GeomCamera3::Init 0x4700a0 and SetLODScale 0x470b90: a row of the camera's matrix, times the LOD scale, divided
by the camera's field +0x2bc), plus its fourth element. So `f x M / w` is the piece's half size in the units the
original's level-of-detail choice uses for "size on screen"; `ren_SelectBracketMinPixelSize` says pixels, and
that holds if +0x2bc is the viewport's size in pixels, which I have not read. Your LOD-by-screen-size measure
is the closest thing you have; f is raised to `3 x w / M` when the piece would be smaller.

## render 23: how the original sizes a selection bracket — the rule read; the four pieces' exact corners not yet

Read in the original's own drawing routine (0x7fc820) and its parameter loader (0x7fc4b0, the Lua table
`RenderSelectParams`). What is certain:

**The parameters** (the table's keys, with the values the exe starts with):
`ren_SelectionSizeFudge` 1.85, `ren_SelectionHeightFudge` 0.12, `ren_UnitSelectionScale` 0.75,
`ren_SelectBracketMinPixelSize` 3, `ren_SelectBracketSize` 0.2.

**The square.**
- Side along x: `SelectionSizeX x ren_UnitSelectionScale` (0.75) when the blueprint's SelectionSizeX is above 0;
  else the mesh's own bounds along x times `ren_SelectionSizeFudge` (1.85). The same along z with
  SelectionSizeZ. These are half sides (the box is made with them as extents about its centre).
- The pieces are laid out on **M = the larger of the two**: the bracket is a square, also for a long unit.
- Centre: the unit's drawn place, plus SelectionCenterOffsetX / Y / Z turned by the unit's orientation (only
  when one of the three is not 0), plus `ren_SelectionHeightFudge` (0.12) upward. The box lies flat in the
  unit's own frame (its y extent is 0), so it tilts with the unit.
- SelectionSizeY and SelectionYOffset are not read by this routine.

**The pieces' share f.** `f = SelectionThickness` when its absolute value is at least 0.00001, else
`ren_SelectBracketSize` (0.2). Then the smallest size: with w the camera's depth term for the unit's place
(the dot of the place with the view's fourth row, + its fourth element), if `M x f / w` is under
`ren_SelectBracketMinPixelSize` (3), f is raised to `3 x w / M`. So far from the unit the pieces stop
shrinking; I have not worked out the unit of that 3 (the row may carry the screen's size: "pixels" by its name).

**The drawing.** Four quads, one per corner, each showing a quarter of one texture (texture coordinates 0 to
0.5 on each axis: the art is a 2 x 2 sheet of the four corners). The quads' corners are built from the lengths
`f x M` and `(1 - f) x M` along the box's two axes.

**Not read to the end:** exactly which of those lengths is measured from which box corner for each of the four
quads (the routine re-uses its scratch space between the quads and I did not follow every store). The two
readings are "a piece of side f x M sitting in each corner" and "a piece of side (1 - 2f) x M inset by f x M";
your nine tenths of the shorter half side is neither, and it uses the SHORTER side where the original uses the
longer. If you want it exact, tell me and I finish the read; otherwise the first reading (side f x M in each
corner, never under the 3) is the plain one to try against a picture of the original.

**New in `BlueprintDisplay`** (at its end, nothing moved): `selectionThickness` and
`selectionCenterOffset[3]`.

## Chris, 2026-10-13 (through the renderer's chat): a tank's shell goes through the commander — found; the fix is the renderer's, with one new field from the engine

The simulation stops the shell where it should. Checked on his own game (the one of 20:52, played back from
its recorded stream: all 853 sound lines equal the live game's): 64 tank shells came at the commander, every
one of them ended in a hit on a unit, and the hit point is the commander's near face (a shell from the north
west: last step from (357.09, 19.93, 673.03), hit at 6% of it, 0.35 inside the box's corner).

What he saw is drawn between two beats. `render/sim_frames.cpp` moves every shell on along its velocity for the
part of the beat that has passed ("projectiles fly on along their velocity"). A shell that hit in this beat
stands at the hit point with the velocity it arrived with, and is only destroyed when its OnImpact runs at the
start of the next beat: for that tenth of a second it is drawn flying on, up to 2.5 units for a tank's shell,
which is through the commander and about its own height beyond.

**New: `ProjectileView::stopped`** (src/sim/sim_view.h, at the struct's end): true for a shell that hit
something this beat. `position` is the hit point; `velocity` is left as it is (scripts read it at the impact).
A host that advances shells between beats must leave a stopped one where it is. Renderer: that is one line in
your frame code; the trail then ends at the hit point too.

## ui 21: a factory's infinite build (the repeat queue) — done

The switch is acted on now, the original's way (its command dispatcher, read at 0x598f0b):

- When a factory's order ends and its count has run out, and the unit's repeat switch is on, the order gets
  its count back (what it was queued with: 5 tanks become 5 tanks again) and goes to the END of the queue. An
  order that is alone in the queue stays where it is and simply builds again. The next order starts one beat
  later.
- Only factory build orders repeat (command type 7). Nothing else in a queue is touched by the switch.
- Taking the switch off changes nothing in the queue: the orders still there are built once more each, in the
  order they stand in, and then go.
- `SetRepeatQueue` as an info pair calls the unit's script `OnStartRepeatQueue` / `OnStopRepeatQueue` when
  the state changes (FAF's units have those; the original calls them before it sets the flag).

Checked headless on the session table you gave me: a land factory with one tank and one engineer queued and
the switch on has built 2 + 1 after 40 seconds, 3 + 2 after 80, 4 + 4 after 120, queue still two entries;
without the switch 1 + 1 and an empty queue. All 13 replays pass (none uses the switch).

Not done, and not asked: a factory that ASSISTS another one copies that factory's queue and its repeat switch
in the original (the guard task reads the same flag); ours' assisting factory still takes the head item once.

For your driver: `moho64 --orders` has a line `<tick> info <unit id> <key> <value>` now (an info pair as the
interface sends it), beside `<tick> lua <one line>`.

## Chris, 2026-10-13 (through the renderer's chat): "the acu is not aiming properly and shooting the tanks" — fixed

A commander's gun missed everything that was not where it had first aimed. Rebuilt headless (the same session and
seed as his game, ten of his tanks set round the AI's commander): it killed the first tank, then fired at the
second for almost thirty seconds without one hit; each shell passed 0.7 beside the tank and struck the ground.

Cause: an aim controller measures its firing direction from where the muzzle stood in last beat's pose. A
commander's build arm and its gun both turn the torso, and the build arm's step, which runs first, had already
put the torso back to rest when the gun read that pose. So the gun aimed from where its muzzle is with the torso
unturned, about one unit beside the real one, and the torso settled some 3.5 degrees off. The first target was
hit only because of where it happened to stand.

Fixed: the muzzles are read once a beat before the build arms' step. Every unit whose builder arm shares a bone
with a weapon was affected: all four commanders and the support commanders. The same case now: nine of the ten
tanks dead in thirty seconds. All 13 replays pass unchanged (none has a commander fighting before its first
difference).

For tests of this kind `moho64 --orders` takes a new line, `<tick> lua <one line of Lua>`, run in the simulation
state before that beat (a test building its case; not an order, never part of a replay).

## render 22: a shield's ratio in the plain unit view — done

`UnitView` (src/sim/sim_view.h) has two new members at its end; nothing else in it moved:

- `bool hasShield`: a shield entity hangs on the unit right now (a generator's bubble or a personal shield).
  FAF's script makes it when the unit is finished, or when the upgrade that brings it is, and destroys it with
  the unit or when the upgrade is taken away.
- `float shieldRatio`: what the shield's script last gave `Unit:SetShieldRatio`. 1 full, health / max health
  under fire and while it recharges, the charge-up's progress while it comes back, 0 when it is down or
  switched off. It is the same number as `UnitStateView::shieldRatio`.

So `hasShield` with a ratio of 0 is a shield that is down, and no `hasShield` is a unit without one: draw the
bar on the first, nothing on the second.

One thing to know: until this commit the ratio in `UnitStateView` was always 0 (see "ui 20 d, corrected" below).
If you tested the bar against that view before, test again.

## ui 20 d, corrected: the shield ratio was always 0 — fixed

My answer under d below said the number is filled. It was not: you were right, and a generator with its bubble
up gave 0. The engine keeps a script's `Unit:SetShieldRatio(x)` under the name "ShieldRatio" (the setter's name
without "Set"), and the view asked for "SetShieldRatio", which nothing ever wrote. I had read the code and not
watched it run, and said so; watching it (every SetShieldRatio call of a replay, then the view) showed the
calls arriving and the view missing them.

Fixed: the unit keeps the number itself and both views (`UnitStateView::shieldRatio` and the new
`UnitView::shieldRatio`) read that. Your case (a ueb4202 made by CheatSpawnUnit beside a ueb1201) now gives
what lua/shield.lua sets: 1 once the bubble is up, less under fire, the charge-up's progress after it fell.

The same mistake sat on two neighbours, also fixed: `UnitStateView`'s capturable and reclaimable flags read
"SetCapturable" and "SetReclaimable", so a script's `SetCapturable(false)` or `SetReclaimable(false)` never
showed; a unit always looked capturable and reclaimable by its blueprint alone. If the interface greys out the
Capture or Reclaim cursor from those two, it will now do so on units whose script switched it off.

## render 21: a projectile's SetVizTo... flags — done

`ProjectileView` (src/sim/sim_view.h) has `vizFocus`, `vizAllies`, `vizEnemies`, `vizNeutrals` now, with the
same meaning as on `EntityView`: 0 never called, 1 'Always', 2 'Never', 3 'Intel', as the script last called
SetVizToFocusPlayer / Allies / Enemies / Neutrals on the shell. They sit at the end of the struct; nothing else
in it moved. FAF's ShieldCollider shell sets all four to 'Never' in its OnCreate, so it arrives as 2, 2, 2, 2.

Another way a shell is hidden from everybody in the original: I know of none that the simulation holds. A shell
has no recon blip of its own; whether it is drawn is the viz flags and, on the user side, whether its mesh has a
level of detail for the distance. A blueprint without a mesh (Display.Mesh empty) draws nothing either, but that
you see yourself.

## ui 20: reclaim of a building (the walk, the speed, the arm) and a shield's ratio — a to c done, d answered

The Reclaim order's task was a shorter form of the original's. It is now CUnitReclaimTask state by state
(read at 0x61f000; `TickPropReclaim` in src/sim/build.cpp carries both a prop and a unit), and the three things
Chris saw come from that.

**a. The walk.** The original's rule, now ours: the builder must be within MaxBuildDistance of the target,
measured as the distance between the two centres LESS the larger side of the builder's footprint LESS the larger
side of the target's footprint (whole cells, not half of them). Farther than that, or nearer than 1, it walks
first (to a free cell beside the target's footprint), then reclaims. So a commander (MaxBuildDistance 10,
footprint 1) reclaims a building two cells wide from up to 13 away without a step, and one eight cells wide
from up to 19. That is the original's reach, and it is generous: "it reclaims from where it stands" is right
inside it. Before today ours took HALF the target's size, a shorter reach than the original's, so if anything
it walked more. If he stood farther than the rule allows and it still did not walk, give me the two positions
and the building's id and I look at that order.

**b. The speed. This was wrong, and badly.** The original asks the BUILDER: `builder:GetReclaimCosts(target)`.
Ours asked the target. For a prop FAF's function answers the same either way; for a unit it does not: asked the
wrong way round it took the reclaimer's build cost and the TARGET's build rate. A T1 engineer on a land factory
came out at 0.1 x 260 / 20 = 1.3 seconds. Asked the right way it is 0.1 x (the target's BuildCostEnergy or
BuildCostMass, whichever is larger) x the builder's ReclaimTimeMultiplier / the builder's BuildRate: a building
that costs 750 energy takes a T1 engineer (rate 5) 15 seconds and the commander (rate 10) 7.5.
What happens in that time, as the original does it: every beat the target loses MaxHealth / (seconds x 10)
health (plus what it regenerates in a beat) and carries the state BeingReclaimed; nothing is paid out and the
builder's own shortage of resources does not slow it. When the health is used up the engine calls the target's
OnReclaimed, then its `CreateWreckageProp(0)`, destroys the unit, and the same task goes on reclaiming the WRECK
as a prop. That second part is where the mass comes from, and it has the prop's own pace (ui 19's numbers).
An unfinished unit is taken the prop's way from the start and pays back what was put into it.

**c. The arm.** The original's task aims the builder's arm at the target before it starts (SetAimTarget, which
runs the script's OnPrepareArmToBuild: that is what opens it), waits until the arm is on target unless the two
stand within 1 of each other, and lets it go when the task ends. The order's task did none of that; it does all
three now, like the build task. Nothing new is needed on your side or the renderer's: the arm is bones in the
unit's pose like a turret's.

**d. A shield's ratio.** `UnitStateView::shieldRatio` is the number FAF's shield script hands to
`Unit:SetShieldRatio`, read back from the unit each time the view is asked for. lua/shield.lua sets it to 1 when
the shield is made, to health / max health when it takes damage and while it recharges, to the charge-up's
progress while it comes up after being down, and to 0 when it goes down or is switched off. The owner is the
unit that carries the shield (a generator, or the unit with a personal shield), so both kinds are covered. I
have read this, not watched it in a running game: if a generator whose bubble is up gives you 0, tell me the
unit's id and I trace that one. One trap: before the shield's first OnCreate has run (a generator still being
built, a shield upgrade not yet finished) the script has never called it and the view says 0.

The Engineers replay, which reclaims, now matches the original 418 beats longer (first difference at beat 1373,
was 955).

## ui 19: reclaim: the prop at a place, and an order with a place and no entity — done

**(1) The lookup.** `ReclaimableAt(Sim&, const float at[3], float within, ReclaimableView* out)` in
src/sim/sim_view.h. It answers for PROPS (trees, rocks, wreckage) of the category RECLAIMABLE; a unit under the
cursor stays yours to find with `UnitAt`. Of the props whose box (the blueprint's SizeX by SizeZ times the
prop's scale, around its position, on the map plane) lies within `within` of the place it takes the nearest;
at equal distance the one whose centre is nearer. `within` 0 means the place must lie inside the box. It
returns false when there is none, and fills `ReclaimableView`: `id` (what the Reclaim order names),
`blueprint`, `position`, the box's `sizeX` / `sizeZ`, and `mass` / `energy` left in it now (the prop script's
MaxMassReclaim / MaxEnergyReclaim times its ReclaimLeft, which is what the original's rollover shows; 0 for a
prop that holds nothing). It reads the scripts' state, so call it between beats like `BuildSiteOf`.

Two things to know for the cursor. A tree's box is small (about 1 by 1), so pass a `within` that suits the
zoom, or the player has to hit the trunk. And it does not test what the army sees: a wreck in the fog is
answered too; filter with `ReconAtPoint` if the cursor should not give it away.

**(2) A Reclaim order with a place and no entity.** Read in the original (CUnitReclaimTask::Execute
0x61f02e): the task is made all the same and carries a flag "created without an entity" and the place. Every
beat it asks the army's recon data whether the place is in sight. In sight: there is nothing there to reclaim
and the order ends as failed, at once. Not in sight: the unit goes there, and the order ends when the place
comes into view. That is the original's way of handling a reclaim on something the player saw once and that
may be gone. So a click on bare ground in view does nothing at all in the original.

Ours used to hand such an order to an old fallback ("go to the target and end there"), which is where the
commander's odd standing came from. Now `TickReclaim` does the original's thing. The beam Chris saw I cannot
place from here: no reclaim had started on the simulation's side (OnStartReclaim is only called with a target
in range), so if a beam shows again on an order WITH an entity that does not get reclaimed, send me the unit
and prop ids and the beat.

All thirteen replays pass unchanged. Neither part was tried by me in your driver: please run a reclaim on a
tree and on a wreck through `ReclaimableAt`, and one on bare ground.

## ui 18: GameTick() and the beat the commander can be selected again — neither side is off by one

**(a) GameTick().** Read: the user function (0x8be420) returns `session+0x458`, which `CWldSession::DoBeat`
writes from the beat's sync data (0x8945a3, the sync's field +8), and the simulation fills that field with its
tick counter `sim+0x900` (0x747616) after the beat has run. The counter starts at 0 and goes up by one at the
start of every beat that is not paused. So after N beats the user side reads **N**: `sim->tick()`, as you have
it. The unit states in the same sync are those after beat N, so "GameTick() = N" and "what the units look like
after beat N" always belong together.

**(b) The warp-in.** Ours is not late. The original's own recording of the skirmish replay has the commander
`UnSelectable` on beats 31 to 51 and free on 52, the same beats as ours (the recorder writes the unit states
every beat, and that replay matches the original for its first 340 beats). It follows from the scripts too:
`CommanderWarpDelay` first runs in beat 1, `WaitSeconds(3)` is `WaitTicks(31)` = 30 beats later = beat 31;
`WarpInEffectThread` starts in that same beat, `WaitSeconds(2.1)` is `WaitTicks(22)` = 21 beats later = beat 52.

**Your SelectUnits is right as well.** Read: the user `SelectUnits` (0x8bd8f0) skips a unit that is dead or
being destroyed, and takes it only if `UserUnit` vt+0x30 (0x8c0500) says yes. That test answers no for a unit
in the state `UnSelectable` (unit state 0x21) and for a docked pod. For a unit it refuses, SelectUnits takes the
unit's transport instead when there is one of the category TRANSPORTATION.

**So what happens in the original?** With all three as they are, FAF's loop can only take the commander when a
try falls on GameTick() 52 or later, directly after a check that still read 50 or less: the tries have to step
over tick 51. The user side's `WaitSeconds(0.1)` (userInit.lua:52) waits whole frames until 0.1 s of real time
has passed, so one round takes 0.1 s plus up to a frame, and the tries drift against the simulation's beats and
do skip a tick now and then. Whether the skip falls on 51 is chance. In your driver the tries fall on every
tick from 41 to 51, one per beat, so it can never happen. I have not seen the original's start with my own
eyes, and I cannot tell from the code how often it wins; the bound of 50 against a warp that ends at 52 looks
like a slip in FAF's script more than a design.

What I would do, and it is yours and Chris's to decide since it is the interface's behaviour: give Chris what
he expects (the commander selected with its build menu as the warp ends), for example by letting that loop's
last try come after the commander is free, and list it as a deviation from the script as written. Nothing to
change on the simulation's side.

## ui 17: a building can be set inside another building's skirt — fixed

Chris is right, and the original tests exactly what he says: skirt against skirt. Read in the exe: the site test
(0x720d90, asked by the engineer's build task just before it creates the unit, 0x5f7aec, and by
`CAiBrain::CanBuildStructureAt` 0x57cbb0) calls 0x720770, which takes every unit that is not mobile from the
collision grid around the new building's skirt rect and refuses the site when that unit's skirt rect meets the
new one's, strictly (touching edges are allowed, which is how adjacency sits skirt to skirt). Ours tested the new
skirt against the standing buildings' FOOTPRINTS only; that was on my list as a known difference from the day the
placement code was read and never built.

Now `LocationIsFree` and `CanBuildStructureAt` (src/sim/build.cpp) do the original's test, so both your
`BuildSiteOf` answer and the build order itself refuse your case: a generator at 685.0, 347.5 (683.5 to 685.5)
against the land factory's skirt 684.5 to 692.5. (The interface track ran it again: refused; the generator
ordered one step further left is built at 681.5 to 683.5.)

**The rectangle to draw** is the one you draw when the blueprint's offsets centre it; in general it is
`RUnitBlueprint::GetSkirtRect` 0x51ec50: with `o` = the footprint's corner (round-half-even of position minus
half the footprint), x from `o.x + SkirtOffsetX` to `o.x + SkirtOffsetX + SkirtSizeX`, the same along z; where a
SkirtSize is 0 the footprint's own edge on that axis. The simulation has already clamped the offsets to 0 or
less and the size to at least the footprint.

Three things to know:
- A building under construction counts like a finished one, and so does a dead one until it is removed.
- One quirk of the original is kept: the candidates come from the collision grid's 4 by 4 cells around the new
  skirt, and a standing building is filed there by its collision box, not its skirt. A skirt that reaches into
  the new one from a building whose box lies wholly in another cell is not found. With the stock buildings'
  sizes this can only happen at the outer 1.5 of a factory's skirt and only for certain alignments; if Chris
  finds a spot where a generator still goes one step into a factory's square, that is this, and the original
  does the same there.
- Not built: the original also keeps a new skirt off the AI brain's own pending build plans (brain+0x98). That
  is for the stock AIs, which do not build yet.

The thirteen replays pass unchanged. Not tried by me in your driver: please run your seed 7 case again.

## ui 16: a factory told to Stop keeps its work progress — fixed

You read it right: the original's engine does it. The factory's build task (CFactoryBuildTask) sets the
factory's work progress to 0 in its destructor (0x5fa010: Busy and Building off, work progress 0, then
`OnStopBuild`), however the task ends: the unit finished, Stop, the queue cleared, the unit under construction
killed. Ours ended every other build task that way but left the factory's alone, an exception from the first
building code that no replay ever showed (after a finished unit the progress is 0 anyway).

Now `EndBuildTask` (src/sim/build.cpp) sets it to 0 for the factory task too, before `OnStopBuild` as the
original does. The thirteen replays pass unchanged. Not tried by me in your driver: please run the Stop at half
way again; `UnitView::workProgress` should read 0 from the beat the task ends.

## render 20: a beam's far end, and the life bar's three numbers — done

**The far end.** `EffectEvent` has `hasPosition2` and `position2[3]` (src/sim/sim_view.h): for a Create with a
second entity (`AttachBeamEntityToEntity`) the place of that entity's point, taken exactly as `position` is
taken for the first (the entity's bone in this beat's pose when the list is taken; where it was at the call if
the entity is gone by then). It is filled whether or not the far entity has a mesh, so adjacency's hubs are
covered, and `EntityViews` stays as it is (entities without a mesh are still not listed). `entity2` / `bone2`
are unchanged. A beam's later `AttachBeamToEntity` call is as before (it has one entity; its place is
`position`).

**The life bar.** `BlueprintDisplay` has `lifeBarSize`, `lifeBarHeight`, `lifeBarOffset` for units: the
blueprint's `LifeBarSize`, `LifeBarHeight`, `LifeBarOffset` as the simulation has them after the blueprint
scripts and mods, 0 where the blueprint does not set one (the engine's own defaults for a missing one are not
read; say if you meet a unit without them).

Not tried by me in a picture: please check the adjacency links' far ends. The thirteen replays pass as before
(nothing of the simulation changed).

## ui 15: the unit blueprint's fields at +0x458, +0x460, +0x468

| Offset | Field | Commander (uel0001) |
|---|---|---|
| +0x458 | `Defense.Shield.ShieldSize` | not in the file: 0 |
| +0x45c | `Defense.Shield.RegenAssistMult` | |
| +0x460 | `AI.GuardScanRadius` | 31.5 |
| +0x464 | `AI.GuardReturnRadius` | |
| +0x468 | `AI.StagingPlatformScanRadius` | not in the file: 0 |

So the "Defense" extractor (0x7ec980) reads the shield's size, and "Miscellaneous" (0x7ed430) the air staging
platform's scan radius where there is one, else the guard scan radius.

**Where the blocks begin** (the whole unit blueprint, for the next question of this kind):

| Block | Begins | Known from |
|---|---|---|
| Footprint / AltFootprint | +0x0d8 / +0x0e8 | the reflection table itself |
| Physics | +0x278 | many reads in the simulation (MotionType +0x290, MaxSpeed +0x2a0) |
| Intel | +0x330 | as you have it (RadarRadius +0x338, SonarRadius +0x33c, OmniRadius +0x340) |
| Air | +0x368 | `Air.CanFly` is read at +0x368 all over the flight code |
| Transport | +0x3f8 | `Transport.StorageSlots` is read at +0x414 (block +0x1c) |
| Defense | +0x420 | see below |
| AI | +0x460 | `AI.GuardFormationName` +0x470, `AI.NeedUnpack` +0x48c, `AI.AttackAngle` +0x4d0, each read in code |
| Economy | +0x4e8 | `BuildCostEnergy` +0x4e8, `BuildCostMass` +0x4ec; `MaxBuildDistance` is its +0x7c = **+0x564** |

**How sure.** The offsets INSIDE each block are the exe's own reflection entries (`push <offset>; push <name>`,
the entries from 0x521435 Transport, 0x521664 AI, 0x521ab5 Defense, 0x52183f the shield struct, 0x521f44 Economy). AI's
three are read straight off: GuardScanRadius +0x00, GuardReturnRadius +0x04, StagingPlatformScanRadius +0x08, and
AI's start at +0x460 is confirmed by three reads in simulation code. **+0x458 is by arithmetic, not by a read:**
Transport ends at +0x420 (its last field, RepairRate, is its +0x24), AI begins at +0x460, so Defense is the 0x40
bytes between; its entries are MaxHealth +0x00, RegenRate +0x08, AirThreatLevel +0x0c, SubThreatLevel +0x14,
EconomyThreatLevel +0x18, ArmorType +0x1c (a string, 0x1c bytes long), which leaves exactly the two floats of
the shield struct (ShieldSize +0, RegenAssistMult +4) at +0x38. I did not find the entry that names the shield
struct's place in Defense, and I did not read 0x7ec980.

**Corrected by the interface track the same day (their main 6926a52), so that nobody builds on what follows:**
the picture's scale was not 1 : 1. FAF's `/lua/system/blueprints-units.lua` sets `AI.StagingPlatformScanRadius =
Economy.MaxBuildDistance + the footprint's size + 1` for engineers and commanders, so the "Miscellaneous" ring IS
the build range (12 on the commander as the game runs it, not the file's 0), and with that ring as the scale the
thick red one is the direct fire weapons' 22. The extractor at 0x7ec980 halves +0x458 (ShieldSize is a
diameter). And +0x564 is read by the world view at 0x853f56, a plain circle of MaxBuildDistance round each
selected builder in the "buildanchored" mode. My two points below about the ring at 10 and "none of these" are
therefore wrong; the offsets and block starts above stand.

**For Chris's picture** (your reading, with the blueprint's numbers beside it; the scale is 1 : 1, the
commander's OmniRadius is 24):

- The ring at 10 is `Economy.MaxBuildDistance` (10 on uel0001), as you took it. But by the table above the
  "Defense" extractor does not read it: on a plain commander +0x458 is 0, so that filter draws nothing for it.
  Another extractor must read +0x564; look for that offset in the range renderer's code.
- Its direct fire weapons say MaxRadius 22 (RightZephyr, OverCharge, AutoOverCharge), the missiles 256 (min 20),
  GuardScanRadius 31.5, VisionRadius 26. None of these is 18.6, 12.9 or 51.7, so those three rings are not the
  commander's own blueprint values at that scale. Whether a live unit's answer (unit vtable +0x44) differs from
  the blueprint I have not read; a weapon's radius does change at run time (`Weapon:ChangeMaxRadius`, which
  enhancements and FAF's veterancy use), and the simulation's weapons hold the changed value
  (`UnitWeapon::ovMaxRadius` on our side). If you need that out of the simulation, ask for it as a view field.

The tool, if you want to look up another field: `_scratch\engine\session\bpfields_all.py <lo> <hi>` lists every
reflection entry in a code range (0x51a000 0x526000 covers the unit blueprint), `bpfield.py <FieldName>` one.

## render 19: `paused` in the plain unit view — done

`UnitView::paused` (src/sim/sim_view.h), filled by `UnitViews` from the same flag `UnitStateView::paused` reads
(`Unit:SetPaused`). Every unit has it, with or without work or orders, so the per-unit `UnitStateOf` call for the
pause mark can go. Nothing else in the view changed; the simulation is untouched (the thirteen replays pass as
before).

## render 18 (and the interface's enhancement orders): a Script order keeps its Lua table now — fixed

You read it right, and the fault was the simulation's: the command stream's reader took the order's Lua value
(`StreamIssueData::lua`, the serialized bytes) and `MakeStreamCommand` never made a table of it. Only orders a
SCRIPT gave (`IssueScript`) had their table. So every Script order of a player (type 28: an enhancement's
`{TaskName = 'EnhanceTask', Enhancement = ...}`, FAF's attack-move `{TaskName = 'AttackMove', ...}`) ran as the
bare ScriptTask, logged "Did you forget to add TaskName to your command lua param table?" and did nothing.

Now `MakeStreamCommand` (src/sim/commands.cpp) deserializes the value and, when it is a table, keeps it as the
command's data table, the same slot `IssueScript` fills. Nothing changes in what a host writes: the bytes of
`SerializeLua` in `StreamIssueData::lua`, as both of you do.

Checked on the thirteen replays: the upgrades replay holds such orders (the commander's enhancements), and its
economy now matches to beat 1550 instead of 1332 and its random draws for 2,967 instead of 2,234; 3,844
differences on the way are gone. Its marks behind its first difference (beat 648) moved, as they must when
upgrades that never happened now happen; they were reset. Not checked by me: your `--enhance` case and the
interface's enhancement button; please run them.

## ui 14: what the original's interface sends with a group's order — from about 1,250 orders of the 13 replays

Taken from the command streams of the lab's thirteen replays (all played by Chris in the original's own
interface), with `engine-ref\replay_dump.py` and a count over all of them. Not read from the exe's interface
code: where the data does not decide, I say so.

**What the records show, without exception:**

| Order | Units | formIndex | scale | how often |
|---|---|---|---|---|
| Move (right click on ground) | 2 or more | 0 | 1.0 | 126 of 126 |
| Move | 1 | 0 | 1.0 | 60 of 60 |
| Attack on a unit (right click on an enemy) | 1 | 0 | 1.0 | 20 of 20 |
| Attack | 2 or more | 0 | 1.0 | 14 of 14 |
| Patrol | 2 or more | 0 | 1.0 | 13 of 13 |
| Guard, Reclaim, Repair, Capture, Build*, Upgrade, Stop, Dive, Ferry, transport orders, Script (FAF's attack-move) | any | -1 | none | all |
| a factory's rally orders (IssueFactoryCommand: Move, Patrol) | 1 | -1 | none | 6 of 6 |

- **The type stays what it is**: Move is type 2 (Move), not FormMove; Attack 10, Patrol 16. No replay holds a
  FormMove, FormAttack, FormPatrol, AggressiveMove or FormAggressiveMove order at all, so what Ctrl does is
  not in my data.
- **formIndex is 0, the scale 1.0, also for ONE unit.** The simulation only builds a formation when the order
  holds more than one unit (`GenerateFormation`), so a single unit's 0 changes nothing; send it all the same,
  it is what the original sends.
- **The orientation** is a turn about the up axis only, written as a quaternion in the order **(w, x, y, z)**
  with x = z = 0: heading = 2 x atan2(y, w), and the heading is the direction FROM THE MEAN OF THE UNITS'
  POSITIONS TO THE TARGET, atan2(dx, dz). Checked on the naval replay's order at beat 147: 17 ships, mean
  position (185.38, 366.79), target (294.66, 271.94): direction (109.28, -94.85) = 130.96 degrees; the record
  has (w 0.415, y 0.910) = 130.96 degrees. (The middle of the units' bounding box would give 132.45: it is
  the mean.) For an Attack on a unit the target point is the unit's place.

**Three orders that do not fit**, all with one unit and formIndex -1 where the table says 0: an Attack on the
GROUND by one tank (land battle, beat 340), and two Patrol points given to one engineer (economy replay, beat
4491). All other single-unit orders with -1 are factory rally orders. My reading, not proven: these three
were given through the order BUTTON (attack-ground and patrol have no right click), and the button's path
sends no formation for one unit, while a group's patrol does carry it. If you want it exact I can read the
interface's issue function in the exe; say so.

**Records** (replay, stream beat, units, type, target, formIndex, quaternion w x y z, scale):

```
Naval    147  17 units  Move   pos (294.66, 17.50, 271.94)  form 0  (0.415, 0, 0.910, 0)   1.0
Naval    159  19 units  Move   pos (125.45, 17.50, 239.05)  form 0  (-0.098, 0, 0.995, 0)  1.0
Naval    207  2 units   Move   pos (550.76, 18.78, 478.87)  form 0  (0.920, 0, 0.392, 0)   1.0
LAND      60  0x7 0x9   Move   pos (280.18, 23.86, 696.28)  form 0  (-0.430, 0, 0.903, 0)  1.0
LAND      84  0x2 0x4   Attack ent 0x30100005 (a blip)      form 0  (-0.391, 0, 0.920, 0)  1.0
LAND     104  0x6       Move   pos (296.42, 20.44, 687.85)  form 0  (0.082, 0, 0.997, 0)   1.0
LAND     113  0x6       Attack ent 0x30100006               form 0  (0.053, 0, 0.999, 0)   1.0
LAND     232  0x1 0x2 0x3  Patrol pos (287.05, 23.83, 692.46)  form 0  (0.918, 0, -0.397, 0)  1.0
LAND     340  0x5       Attack pos (287.13, 23.80, 692.64)  form -1
Economy 4491  0x4       Patrol pos (660.50, 18.68, 329.50)  form -1
Skirmish 178  factory 0x700001  Move (rally)  pos (106.92, 25.76, 917.21)  form -1
```

**What formIndex 0 becomes on our side**: the naval order at beat 159 (19 ships and subs) is the formation
script 'AttackFormation' in the simulation. I have seen no other index in any replay.

## ui 13: a unit that is still being built carries out no order — fixed in the simulation

The fault was the simulation's, not the interface's: your order is right to go out.

The original's command dispatcher (`IAiCommandDispatchImpl::TaskTick`, FA exe 0x598f6d) asks four things
before it starts the next order of a unit's queue, and waits a beat when any is true: the unit is being built
(IUnit vtable +0x34, the flag at entity +0x98), it is dead, it has state Attached, it has state
BlockCommandQueue. Ours had the Attached test and not the first: the queue of a factory under construction
was started at once. Now `StartHead` (src/sim/commands.cpp) holds the queue while `beingBuilt`; the orders
stay in the queue, as in the original, and begin the beat after the unit is finished. It holds for every
kind of order to every unfinished unit, not only a factory's build orders.

Checked on the thirteen replays: all pass, and the skirmish replay, whose first difference had been exactly
this (an unfinished factory at beat 165 starting its first engineer), now matches to beat 340 (units), 510
(economy) and for 21,044 random draws instead of 14,177. Not checked: your case on Seton's with seed 7; please
run it and tell me what the factory's focus is at 5.6 now.

## ui 12: a unit's own build restrictions — two functions in `sim_view.h`

Both read the simulation's own data, nothing is kept twice:

- **`std::vector<std::string> UnitRestrictedBlueprints(const Sim&, uint32_t unitId)`**: the ids of the unit
  blueprints this unit may NOT build although its blueprint's `Economy.BuildableCategory` names them, in
  blueprint order: what `Unit:AddBuildRestriction` took away and `RemoveBuildRestriction` has not given back
  (a commander before its tier upgrades, a factory before its own). Empty for a unit without any, and for an
  id that is no unit. It is the counterpart of `RestrictedBlueprints(sim, army)`: a build menu is
  `BuildableCategory` less both lists.
- **`bool UnitMayBuild(const Sim&, uint32_t unitId, const std::string& blueprintId)`**: the very test a build
  order goes through (`UnitCanBuild`: the blueprint's BuildableCategory, the unit's restrictions, its
  army's). Use it where you would rather ask than subtract (a hotkey, a template, a queued order).

I chose a list of ids over the raw bit words in `UnitStateView`, as you suggested either: the words are indexed
by the simulation's blueprint order, which a host should not have to know, and the state view is fetched every
frame for the units on screen while this changes only when an upgrade finishes. Ask when the selection changes
and after an enhancement or an upgrade completes.

Checked on the eight-army replay after 200 beats (`moho64 --view`): 8 units have restrictions of their own,
the eight commanders; one commander's list (unit 0, `eel0001`) has 114 blueprints, and `UnitMayBuild` says no
to the first of them. A commander's list shrinks as the scripts call `RemoveBuildRestriction` with its tier upgrades.

## render 17: the first game's shader names — the exe MAPS them to the present ones (a table of 18)

It neither draws nothing nor falls back to one technique: the exe carries a dictionary of the old names,
and every mesh material's `ShaderName` goes through it before it is used.

**Where.** `MeshMaterial::Create` (FA exe 0x7dc1b0) calls the lookup 0x7dbdb0 with the blueprint's
`ShaderName` and stores what comes back as the material's shader name (0x7dc262). The dictionary is
built once by `ShaderDictionary::ShaderDictionary` (0x7db3a0). All of it, old name -> name used:

| Blueprint says | Technique used |
|---|---|
| `TMeshNoLighting` | `Flat` |
| `TMeshNoNormals` | `VertexNormal` |
| `TMeshAlpha` | `NormalMappedAlpha` |
| `TMeshGlow` | `NormalMappedGlow` |
| `TMeshTerrain` | `NormalMappedTerrain` |
| `Simple` | `Unit` |
| `Team` | `Unit` |
| `TMeshAlphaGlowFade` | `UnitBuild` |
| `TMeshMetalBuild` | `AeonBuild` |
| `TMeshShield` | `Shield` |
| `TMeshZFill` | `ShieldFill` |
| `TMeshAdd` | `Effect` |
| `TMeshExplosion` | `Explosion` |
| `TMeshCloud` | `Cloud` |
| `TMeshOuterCloud` | `OuterCloud` |
| `TMeshEMPNuke` | `NukeEMP` |
| `TMeshQuantumNuke` | `NukeQuantum` |
| `TMeshTemporalBubble` | `TemporalBubble` |

So the Aeon commander's death is `NukeQuantum` and `NormalMappedAlpha`, the Cybran's `OuterCloud`.
Checked in the `effects/mesh.fx` the lab runs (FAF's, effects.nx2): `NukeQuantum` and `OuterCloud`
are techniques there, `NukeQuantum` with `AlphaBlend_SrcAlpha_InvSrcAlpha` and no depth write; so a
plain lit model is the wrong look for them. Of the 18 targets, three are in that file under no name
I could match (`TemporalBubble`, `UnitBuild`, `Shield`: neither plain nor with a fidelity suffix).

**The rule of the lookup (0x7dbdb0), in its order:**
1. the name is in the table: the mapped name is used. The first time each old name is met the log gets
   `Use of 'old' shader: <name>` (a set of names already reported, 0x10be638, keeps it to once);
2. the name is not in the table and is EMPTY: `Unit`;
3. anything else: the name as it is.

**After that, the technique** (`CD3DEffect::SetTechnique` 0x42d290, with the name from above): the
effect keeps a map of the techniques that carry a fidelity definition. A name in that map gives, for
the fidelity now set (the global at 0xf58e88), the technique NAMED in its entry for that level; an
entry with nothing for that level uses the technique of the name itself and logs `technique %s in
effect %s has an invalid fidelity definition`. A name not in that map goes straight to the effect's
own `GetTechnique(name)`. So the exe does not paste `_HighFidelity` and the like onto the name: which
technique stands for which level is written in the effect file, in each technique's annotations
(`string abstractTechnique = "NormalMappedAlpha"; int fidelity = FIDELITY_HIGH;` on
`NormalMappedAlpha_HighFidelity`; 140 techniques of mesh.fx carry an `abstractTechnique`). The name a
blueprint gives is the abstract one. What is drawn when `GetTechnique` finds nothing (a name that is neither in the table nor a
technique) I have not read; none of the names you list gets that far.

## render 16: a projectile's scale is the size it is drawn at — yes, and `ProjectileView::size` has it now

Your reading is right, and the original goes one step further: there is ONE value, the entity's scale
(three floats, entity +0x84 / +0x88 / +0x8c), and it is the size the mesh is drawn at. Read in the FA exe:

- **At launch** (Projectile constructor, 0x69b952): scale = `Display.UniformScale` + a draw within
  +- `MeshScaleRange`, the same number on all three axes. So `UniformScale` is the scale's starting
  value, not a factor applied on top of it. The scale velocity starts as `MeshScaleVelocity` + a draw
  within +- `MeshScaleVelocityRange` (0x69ba1d), also the same on three axes.
- **Every beat** (Projectile::MotionTick, 0x69be40): scale += scale velocity x 0.1, per axis. The
  velocity is in the scale's own units per second: UniformScale 17 with `SetScaleVelocity(5.1)` is
  22.1 after one second and 37.4 after four, as you draw it.
- **`Projectile:SetScaleVelocity(v)` or `(vx, vy, vz)`** (0x6a2ee0) writes the velocity only.
- **`Entity:SetScale(s)` or `(sx, sy, sz)`** (0x690fa9) writes those three scale floats outright:
  it is the drawn size, not a factor. On a blueprint whose `UniformScale` is 0.1, `SetScale(1, 1, 1)`
  draws the mesh ten times as large as it was.
- **`Entity:SetDrawScale(s)`** (0x690b35) writes the SAME three floats, all to `s`. It is `SetScale`
  with one number; there is no second "draw scale" on top. Whichever of the two was called last counts.

So "draw at UniformScale x scale x drawScale", as `sim_view.h` said, was wrong in two ways, and that
line was mine. Changed in the views (engine, this commit):

- **`ProjectileView::size[3]`**: the size to draw the mesh at this beat, kept by the simulation the way
  the original keeps it (launch value, + velocity x 0.1 a beat, replaced by `SetScale` /
  `SetDrawScale`). Draw at `size` and you need neither your own sum nor `scale` / `drawScale`; between
  two beats you may add `scaleVelocity` x the time since the beat.
- `scale`, `drawScale` and `scaleVelocity` stay as they were (the raw numbers), so nothing of yours
  breaks; the comment on them now says they are not factors.

Not changed, and to know: **`EntityView` and `PropEvent` still carry `scale` and `drawScale` as two
numbers.** For those the original has the same single value, so the rule there is "the one set last";
the views cannot tell you which was last. Say if a script-made entity or a prop shows a wrong size and
I will give those one `size` too. (`Entity:GetScale()` in the simulation's Lua still returns ours'
SetScale numbers, 1 unless set: the original returns the value above. No script in the game files
reads a projectile's scale back, so it waits.)

## ui 11: `SetCommandSource` and `EndGame` in the simulation's Lua — done

Both are globals of the simulation's Lua now.

- **`EndGame()`**: what the EndGame order of the command stream does (FA exe 0x74b8b0 calls the
  simulation's own EndGame, vtable +0x5c): the next beat still runs, then the game stands, and
  `IsGameOver()` is true from the call on. It was a stub that did nothing.
- **`SetCommandSource(army, source, enabled)`**: which command sources may command an army (the list an
  army has from the session's header; the same list `OkayToMessWithArmy` and the command stream's
  source check go by). `true` adds the source, `false` takes it away. **Both numbers count from 0**,
  as FAF's scripts pass them (`SetCommandSource(data.Army - 1, commandSource, false)`).
  This one is not the original's function: FAF's exe patch adds it (the name is in the patch section
  of the exe, 0x1294028), so I could not read it the usual way and took its behaviour from the
  scripts' use. Nothing else happens in it: the brain's `AbandonedByPlayer` is not called (that
  belongs to the stream's CommandSourceTerminated).

Checked by script after 50 beats of the eight-army replay: `SetCommandSource(0, 0, false / true)` and
an army number out of range run without error, `EndGame()` runs and `IsGameOver()` is true after
it. The victory script's whole path (ToObserver, SyncGameResult, EndGameThread) I have not run:
that is your driver's. The thirteen replays keep their marks.

## render, a question by message: `SetEmitterCurveParam(name, a, b)` — it REPLACES the curve by one key

Read now (it was "not read" in the answers to render 9 and 10). FA exe 0x66def0
(`IEffect:SetEmitterCurveParam`, four arguments with the effect itself):

1. the name is looked up in the emitter's curve-parameter names (0x8d9fd0); an unknown name is a
   script error;
2. a NEW, empty curve is made on the stack and one key is put into it (0x5151b0, which inserts by x
   and then recomputes the curve's lowest and highest y): **the key is (x = 0, y = the third argument,
   z = the fourth argument)**;
3. the effect's own "set curve parameter" (vtable +0x38) is called with the parameter's index and
   that curve: the emitter's curve is replaced, its old keys are gone. Nothing is scaled.

So what you do (the named curve becomes one key, x 0, y = height, z = size) is the original's
behaviour. With one key the curve is constant over the particle's or emitter's life: y is the value
and z its random range, as in every key of that curve in the blueprint. Not seen in that function: an
x range being set on the new curve; it is whatever an empty curve starts with.

## render 15: hidden bones — done

`sim/sim_view.h`:

```cpp
struct UnitHiddenBones {
  uint32_t id;             // the unit's entity id
  std::vector<int> bones;  // the hidden bones, rising, indices as in UnitPose (.scm order)
};
void HiddenBones(const Sim& sim, std::vector<UnitHiddenBones>* out);   // once per beat, beside UnitPoses
```

- Only units with at least one hidden bone are listed, each with its whole list as it is now (not
  changes). A unit that shows everything again drops out of the list.
- Children are resolved: `Unit:HideBone(bone, true)` hides the bone and everything below it, and all
  of them are in the list; `ShowBone(bone, true)` shows them all again; with `false` only the one
  bone changes. That is the original's rule (the pose bone's visible flag, set alone or through the
  children: FA exe 0x6d14f0, 0x54be30).
- `HideBone` and `ShowBone` are methods of units only in the original, so there is nothing for props
  or script-made entities: wreckage does not hide bones by this call.
- They were stubs until now and are kept for this list only; the simulation does not go by them (the
  thirteen replays keep their marks).

Seen in the eight-army test game at beat 600: 10 of 99 units have hidden bones, 345 in all; a
commander (`eel0001`) hides 44 (bones 7 8 9 10 14 16 17 18 19 20 24 26 ...: its upgrades).
`moho64 ... --view` prints that count and one unit's list at the end of a run.

## ui 10: the simulation's `IsGameOver()` — done

`IsGameOver()` in the simulation's Lua now answers as the original's does (FA exe 0x74b980): true when
the EndGame order has been taken (`sim.endGame`, sim+0x8dc) or the game stands already (`sim.frozen`,
sim+0x8dd). It was a stub that said false.

The order of things, as you asked: the order is applied before a beat; that one beat still runs, its
script threads see `IsGameOver()` true, FAF's `GameOverListenerThread` leaves its wait and calls the
listeners, and what they put in `Sync` goes out with that beat's sync table. After the beat the game
stands.

Checked without the interface: the eight-army replay with an EndGame order given before beat 300.
The sync table of beat 301 is 25,860 bytes and carries `ScoreAccum` and `StatsToSend`; the tables
after it are the paused beats' small ones. To repeat it: `moho64 ... --view --orders <file>` with a
line `300 endgame` in the file (the `--orders` file knows `endgame` now); the log names the sync
table that carries the score.

Nothing else changed; the thirteen replays keep their marks (none of them ends by an EndGame order:
their command streams simply stop).

## ui 9: a factory's rally queue in the unit's view — done

`UnitStateView` (`sim/sim_view.h`) has the two fields under the names you wrote against:

```cpp
bool hasFactoryQueue = false;            // the unit is a factory that keeps a rally queue (also when it is empty)
std::vector<CommandView> factoryQueue;   // its entries, first to last, as `queue` has them
```

- `hasFactoryQueue` is true for every unit the simulation accepts a factory order for: its blueprint
  is in category FACTORY and it has a builder. That includes the mobile ones (a Fatboy, an aircraft
  carrier, a CZAR), which keep a rally queue too.
- The first rally is in it from the beat the factory is made: one Move (type 2) to the blueprint's
  `InitialRallyX` / `InitialRallyZ` in the factory's own frame, and the engine puts it back whenever
  the queue is empty. Seen in the eight-army test game after 1,500 beats: 17 units keep a rally
  queue, all with an entry; a T1 land factory at (680.5, 340.5) has one Move at (671.7, 340.5).
- An entry is a `CommandView` as in `queue`: `type`, `position` (with `hasPosition`), `entity` when
  the rally is on a unit (a transport beacon, a carrier). `rallypoint.lua` wants the last one's
  `type` and `position`: `factoryQueue.back()`.
- `unit:GetCommandQueue()` in the original answers with this queue for such a unit and with `queue`
  for every other, as you read at 0x8c5480; the view gives you both and you choose by
  `hasFactoryQueue`.

**Your question (where the original's interface decides "factory order"): not read.** I looked for it
and did not find it in the time I gave it: the string FACTORY is not referenced anywhere on the
interface's side of the exe, so it is not a category test by name there. What I can say from the
simulation's side: a factory order is carried out only for units with `hasFactoryQueue`, and is
silently dropped for the rest, so sending message 13 to anything else does nothing. Your rule
(buildings in FACTORY, not for build orders) is the safe one and I would keep it: `hasFactoryQueue`
alone would be wrong for the interface, because a Fatboy has a rally queue and a right click with a
Fatboy selected must still move the Fatboy. If you want it settled, ask again as its own request and
I will read the interface's order path in the exe.

`moho64 ... --view` prints the count and one factory's rally queue at the end of a run.

## render 14: props that change, wreckage, shields, what scripts make — done

All three parts, in `sim/sim_view.h`. The simulation's own behaviour is untouched (the thirteen replays keep
their marks); three script calls that did nothing before are now kept for you: `Entity:SetMesh`,
`Entity:SetDrawScale` and `Entity:SetVizToFocusPlayer` / `Allies` / `Enemies` / `Neutrals`.

### 1. Props, as events

```cpp
struct PropEvent {
  enum Kind { Create, Change, Destroy } kind;
  uint32_t id;            // the prop's entity id
  int mapIndex;           // its place in the map file's prop list (from 0); -1: made after the start
  std::string blueprint;  // Create, Change: the prop blueprint's id (its path)
  std::string mesh;       // Create, Change: the mesh Entity:SetMesh gave it, "" = its blueprint's own
  float position[3], orientation[4] /* x y z w */, scale[3], drawScale;   // Create, Change
};
void PropEvents(Sim& sim, std::vector<PropEvent>* out);   // once per beat, after Tick()
```

Set `sim.collectProps = true` before the first beat, as with `collectEffects`.

- **How the map's props are matched: by index.** The simulation makes them in the map file's order, so
  `mapIndex` is the entry's index in the file's prop list, counted from 0 over EVERY entry of the file
  (an entry whose blueprint the simulation cannot find is skipped but still counted). You keep drawing
  them from the file; nothing is listed for them at the start.
- **Create**: a prop made after the start (wreckage, `CreateProp`, `CreatePropHPR`,
  `CreatePropAtBone`), with what it is when the list is taken. Wreckage is the prop blueprint
  `/props/defaultwreckage/defaultwreckage_prop.bp` with the dead unit's wreck mesh in `mesh` (for
  example `/units/urs0201/urs0201_mesh_wreck`) and the script's scale.
- **Change**: the prop's mesh, position, orientation, scale or draw scale is no longer what you were
  last told (for a map prop: what the file has). The whole state is given again. What makes changes:
  - a burnt tree: `SetMesh` to the blueprint's `Display.MeshBlueprintWrecked`;
  - a tree knocked over: the script calls `Entity:FallDown`, and the engine then turns the prop about
    its own origin (its foot) a little each beat until it lies flat; you get one Change per beat while
    it falls, with the orientation of that beat. There is no separate "falling" state to draw;
  - **expect one Change for many map props on the first take.** FAF's prop script lifts every prop
    that sits below the ground to the ground's height when it is made (`Prop.OnCreate`: "we never
    want things hiding underground"), and map files have many such, often by a hair. On the lab's
    test map 5,025 of about 5,000 map props came that way on beat 1. That is where the simulation has
    them, so it is where they should be drawn.
- **Destroy**: the prop is gone (reclaimed, destroyed, a tree burnt out). Only `id` and `mapIndex` are
  filled.
- At most one Create or Change per prop per list (the state at the time of taking); a prop made and
  gone between two takes gives a Destroy only.
- Cost: the list compares every prop with what was last told. Measured 0.066 ms a beat with 6,200
  props (the lab's Experimental replay: 1,174 made, 20,393 changed, 185 gone over 947 beats), and
  0.46 ms a beat with 14,000 props in the eight-army game (13,725 made, 302,786 changed, 4,716 gone
  over 1,500 beats: about 200 changes a beat, each with its strings). So expect around a millisecond
  on Seton's 46,000 in a busy game. If that shows in your numbers say so and it becomes a marked
  list instead.

### 2. Script-made entities with a mesh

```cpp
struct EntityView {
  uint32_t id;
  bool shield;            // made by _c_CreateShield (a shield bubble)
  std::string mesh;       // the mesh blueprint, as Entity:SetMesh last gave it
  int army;               // 1-based, 0 = none
  float position[3], orientation[4] /* x y z w */;
  float scale[3];         // Entity:SetScale
  float drawScale;        // Entity:SetDrawScale
  uint32_t parent;        // the entity it is attached to (id), 0xffffffff = none
  int parentBone;         // -1: the entity itself
  uint8_t vizFocus, vizAllies, vizEnemies, vizNeutrals;   // 0 never called, 1 'Always', 2 'Never', 3 'Intel'
};
void EntityViews(const Sim& sim, std::vector<EntityView>* out);   // once per beat, after Tick()
```

Every entity made with `_c_CreateEntity` or `_c_CreateShield` (FAF's `Entity` and `Shield` classes) that
has a mesh set now, in creation order; one without a mesh (a vision marker, a sound carrier) is left
out. Position and orientation are the simulation's for the beat; for an attached one that is its
parent's bone with `SetParentOffset` applied, so you can draw it where it is given without following
the parent yourself. Seen in the same replay: at most 3 at one beat, among them
`/effects/entities/CzarShield/CzarShield_mesh` (a shield, draw scale 26, attached to its unit, viz
Always / Always / Intel / Intel) and `/effects/entities/Shield01/Shield01z_mesh` (an entity, draw scale
25).

- **Size.** A shield bubble's size is `drawScale` (FAF calls `SetDrawScale(size)`); `scale` is what
  `SetScale` gave and is 1 for them. Draw at `scale * drawScale`.
- **Who sees it.** The four `viz` numbers are the script's calls as given; the simulation does not
  decide for you. `'Intel'` means "as the army's intel on it allows": use `ReconOf(sim, focus, id)` for
  the entity (for one that is not a unit that is the recon at its place). Not built: the original's
  own rule for an entity nobody set anything on; treat 0 as 'Always' for allies and focus and 'Intel'
  for the rest, which is what FAF sets explicitly on shields.

### A unit's mesh

`UnitView::mesh`: the mesh blueprint the unit's script last set with `SetMesh` ("" = none set, draw the
blueprint's own). A personal shield sets the unit's shielded mesh and sets the plain one back when it
goes down. Note that FAF sets a mesh on many units at creation (in the test replay 15 units had one,
for instance a mod's commander: `/mods/blackopsfaf-acus/units/eel0001/eel0001_mesh`), so a filled
field does not by itself mean "shielded": compare with the blueprint's own mesh.

### 3. A projectile's scale

`ProjectileView` has three more fields:

- `scale[3]`: `Entity:SetScale(x, y, z)` as last set. 1, 1, 1 unless a script set it (the UEF's build
  box, sized to the unit) or the blueprint's `MeshScaleRange` drew another size at launch.
- `drawScale`: `Entity:SetDrawScale`.
- `scaleVelocity[3]`, per second: `Projectile:SetScaleVelocity`, or the blueprint's `MeshScaleVelocity`
  as drawn at launch. The simulation does not apply it (nothing in it goes by a shell's drawn size),
  so `scale` stays as set and you add `scaleVelocity` x the seconds since you first saw the shell.

All of it is on top of the blueprint's `Display.UniformScale`: draw at `UniformScale * scale *
drawScale`.

### Not in this

- The pieces of a death that are projectiles come through `ProjectileViews` already; the ones made as
  props or entities come through the two lists above.
- `HideBone` / `ShowBone` are still not handed over (the scripts hide and show parts of meshes with
  them); say so if you want them, as a per-unit list of hidden bones.
- The decal note you found is right and the table in the answer to render 8 is corrected: the type is
  in `text` when the script gave no second texture, in `text2` when it did.

Try it: `moho64 ... --view` prints the counts, the time the prop list took and one sample of each
kind at the end of a run.

## render 13: the whole map is seen for three seconds at the start — it is M28's own script

Not the engine and not the `FogOfWar` option: the AI mod does it, and it does the same in the original.

`mods/M28AI/lua/AI/M28Overseer.lua`, `RevealCivilainsToAIByGivingVision(aiBrain)` (forked once per
M28 brain at line 884): it waits until game time 4.4 s, then calls
`M28UnitInfo.GiveUnitTemporaryVision(acu, 10000, override)` on that army's commander.
`GiveUnitTemporaryVision` (`M28UnitInfo.lua` line 3576) applies a buff `CrateVisBuff<n>` with
`VisionRadius = { Add = 10000 }` and `Duration` = 3 seconds, or 0.6 seconds when the M28 army has a
human on its team (`override = 0.6`). M28's comment says why: so that the AI knows where the
civilian buildings are, as a human player would from having played the map. A radius of 10000 is the
whole of any map.

That fits what you counted to the beat: army 1 in `config_m28_fog.lua` is an M28 AIx on team 2, which
has no human, so its commander sees everything for 30 beats from 4.4 s (your samples at ticks 46 to
71), and its three M28 team mates do the same in the same beats.

What it means for you:

- **The player's own army.** In that config the human (army 8) is on team 3 with three M28 armies. Their
  commanders get the same buff for 0.6 s, and an ally's sight counts as the army's own (in the original
  too), so the human's army also sees the whole map, for about six beats around tick 44 to 50. With one
  sample every fifth beat you will catch it once or twice. In a game with no M28 ally the human's army
  never gets it.
- **"Unexplored".** The original would show the same: with an M28 ally the whole map counts as seen
  after five seconds. If you want black ground to mean something in such a game you would have to
  leave those beats out on your side (for instance: ignore a beat in which every sampled place is
  seen); that is a display choice, the simulation's answer is right as it is.
- **What an AI can act on.** Yes: every M28 army has seen every enemy's starting units (and holds
  blips of them, `IsSeenEver`) from second five. That is M28's design, on both engines.

Nothing was changed in the engine for this. (How it was found, for the next question of this kind:
a vision source of that size can only come from `InitIntel` / `SetIntelRadius`, and those are only
called from scripts; `SetIntelRadius('Vision', ...)` is what FAF's buff code calls for a
`VisionRadius` buff.)

## ui 8: what an army sees — done

`sim/sim_view.h`:

```cpp
enum ReconBits : uint32_t {
  kReconRadar = 1, kReconSonar = 2, kReconOmni = 4, kReconSeenNow = 8,
  kReconSeenEver = 0x10, kReconKnownFake = 0x20, kReconMaybeDead = 0x40, kReconHeld = 0x80,  // units only
};
uint32_t ReconAt(const Sim& sim, int army, const float position[3], int water = 0);
uint32_t ReconOf(const Sim& sim, int army, uint32_t entityId);
```

Both are the simulation's own recon, read only: nothing new is computed for the host, and the
simulation's weapons and scripts go by the same answers. `army` is 1-based. An ally's intel counts as
the army's own, as it does in the simulation.

- **`ReconOf`, an entity.** A unit of the army or of an ally: 0xF (radar, sonar, omni, seen). Another
  army's unit: the flags the army holds on it, i.e. its blip: `kReconHeld` = there is a blip at all;
  `kReconSeenNow` = in sight (vision, or water vision for a unit under water, cloak and cloak fields
  taken off); radar / sonar / omni with the stealth rules applied; `kReconSeenEver` stays once seen;
  `kReconKnownFake` and `kReconMaybeDead` as the blip scripts know them (`IsKnownFake`,
  `IsMaybeDead`). 0 = the army knows nothing of it. These flags are renewed when the army's recon
  runs: the armies take turns, one a beat (civilians included), so with N armies an answer can be up to
  N - 1 beats old. That is the original's own pace (weapons wait for it too), not a shortcut of ours.
  Anything that is not a unit (a projectile, a prop, a shield): the answer of its place.
- **`ReconAt`, a place.** The army's grids at that point now: vision cells are 2 units wide, the others
  4. `water`: 0 for something on land or in the air (vision, radar, omni), 1 on the water's surface
  (sonar counts too), 2 under water (water vision instead of vision, sonar, no radar). The army's own
  radar-stealth, sonar-stealth and cloak fields are taken off as for a unit; a unit's own stealth or
  cloak is not in a place's answer. Without fog of war `kReconSeenNow` is always set. 0 outside the map.

For the sounds: `ReconOf(sim, focusArmy, ev.entity) & kReconSeenNow` for an entity that still exists,
`ReconAt(sim, focusArmy, ev.position)` when it is gone by the time you take the list (an explosion's
unit usually is). For the mark under the mouse and the panel, `kReconSeenNow` is the bit the original's
`IsSeenNow` gives its scripts; whether the byte the exe tests at 0x7fd43e is that same bit I have not
read.

**Not read:** what the original's sound manager does with sounds of unseen units. So this gives you
sight; the rule for radar-only contacts (heard or not) is still yours to choose until that is read.
Hearing what is seen and not what is only on radar is the cautious choice: it cannot give away more
than the picture does.

`--view` logs per army how many of the others' units it knows of, sees now, and has on radar, sonar or
omni. The thirteen test replays keep their marks.

## ui 7: the world's sounds, and three facts about a unit — done

**1. The sounds of a beat.** `sim/sim_view.h`:

```cpp
struct SoundEvent {
  enum Kind { Play, Loop } kind = Play;
  uint32_t entity = 0xffffffff;       // the entity it was called on (a weapon: its unit)
  int army = 0;                       // that entity's army, 1-based, 0 = none
  std::string bank, cue;              // the sound (as GetCueBank names them)
  std::string rumbleBank, rumbleCue;  // Loop: the second sound of the call
  float position[3] = {0, 0, 0};      // where the entity stood at the call
};
void SoundEvents(const Sim& sim, std::vector<SoundEvent>* out);
```

Off unless `sim.collectSounds = true` (set it before the first beat); take the list once per beat after
`Tick()`, it is empty again afterwards. In the order of the calls:

- `Play`: `Entity:PlaySound(sound)` and `UnitWeapon:PlaySound(sound)` (the entity is then the weapon's
  unit). Those two are every one-shot the simulation's scripts have; projectiles, props and shields
  are entities and come the same way.
- `Loop`: `Entity:SetAmbientSound(sound, rumble)`: from now on this is the entity's running sound.
  Either name pair may be empty (the script passed nil for it); **both empty = the loop stops**. When
  an entity with a running loop is gone, the list holds a `Loop` with both empty for its id (the same
  rule as an attached effect's `Destroy`), so a loop never has to be timed out. A "stop" for an entity
  that had no loop is not listed.

A sound that is not a sound object (a script passing nil to `PlaySound`) is left out.

**What is left out: nothing.** Every call of every army is listed, with the army of the entity, and
the simulation does not ask who can see what. What the original sends its user side, and whether it
drops sounds of units the focus army does not see, I have **not read** (the calls go to the sim's sound
manager at sim+0x8c4; `UnitWeapon:PlaySound` is 0x6d7c50). Until that is read, the sensible rule on
your side is the one the eye already has: play what belongs to an entity the focus army sees
(`UnitView` is there for units; for projectiles and props, play by distance). Say if you want the
original's rule read; it is a contained piece of work.

Seen with `--view` on the LAND test replay, 600 ticks: 548 sounds played once, 19 loops set, 17
ended; the log shows the totals and the first of each (`view: sound events: ...`).

**2. Three facts**, in `UnitStateView` (`UnitStateOf`):

```cpp
int tacticalSiloBuildCount = 0, nukeSiloBuildCount = 0;  // asked for and not finished (the one being built among them)
int tacticalSiloMaxStorage = 0, nukeSiloMaxStorage = 0;  // MaxProjectileStorage; 0 = no such silo
int kills = 0;                                           // the unit's KILLS statistic
bool capturable = true, reclaimable = true;              // what SetCapturable / SetReclaimable left
```

- `kills` is the value the scripts keep with `SetStat('KILLS', n)` (FAF: `UpdateStat('KILLS', ...)`,
  for example `lua/sim/Projectile.lua:309`), read the way `GetStat` reads it; 0 until a script sets it.
- The build count is the silo's queue: every missile asked for by the button or by auto mode that is
  not in the store yet, the one in work included. The room is the counted weapon's
  `MaxProjectileStorage`, known before the first missile is asked for.
- `capturable` / `reclaimable` are what the scripts last passed to `SetCapturable` / `SetReclaimable`,
  true before any call. They are the unit's own switches only: whether a given unit may capture or
  reclaim it (allies, a unit being built, the caps of the one asking) is not in them.

The thirteen test replays keep their marks (the list is only kept when asked for).

## visuals (relayed by Chris, 2026-10-12): scenery under a new building — it is already removed, by FAF's script

No engine change is needed, and none would be right: the game's own script does it, and the engine runs
that script.

`lua/sim/units/StructureUnit.lua`, `OnStartBeingBuilt` (FAF's file, the one the lab runs): when a
structure starts being built **on land** and is **not an upgrade** of its builder, it takes
`GetReclaimablesInRect` around its position and calls `Destroy()` on every entry that

- is a prop (`IsProp`, set by `lua/sim/Prop.lua`, so any prop class that derives from `Prop`),
- has neither `OBSTRUCTSBUILDING` nor `INVULNERABLE` among its blueprint's categories, and
- has its **position** nearer than `radius` to the structure's position (the prop's centre counts, not
  its box), with `radius = max(Physics.MeshExtentsX or SizeX, MeshExtentsY or SizeY, MeshExtentsZ or SizeZ)`
  of the structure's blueprint. That is the largest full extent, height included, used as a radius: a
  generous circle, wider than the footprint.

It also puts a small flash on each prop it takes. The engine's part, `GetReclaimablesInRect`, returns
every unit and prop in the entity-grid cells the rectangle touches, unfiltered (no category and no
reclaim value is needed to be found), so props "with no category, nothing to reclaim" are found and
destroyed like any other.

So the split you already made is the one the game expects: `OBSTRUCTSBUILDING` + `RECLAIMABLE` props
are reclaimed by the builder before the structure can start (`build.cpp` `FindSiteProp`: the original's
0x5f6ea0); everything else under the structure is destroyed when it starts.

What a scenery prop needs for this: it must be a real prop of the simulation (in the map's prop list
or made with `CreateProp`), with a blueprint whose script class derives from `Prop`, and without the
two categories above. Give `INVULNERABLE` to a prop only if it must survive a building on top of it.
What it does not cover: structures built on water or the seabed (the script's `layer == 'Land'`
test), upgrades in place, and anything that is drawn only by the renderer and is not a prop of the
simulation: the simulation cannot remove what it does not hold.

Seen running in ours: in the Economy test replay 31 props are destroyed from `StructureUnit.lua:301`
(through `Prop.Destroy`), the first two at tick 688 (`MOHO64_DEBUG_DESTROY=1` logs every `Destroy()`
with the script lines that called it: grep `structureunit.lua:301`). If a prop of yours stays under a
new building in a game, send its blueprint id and where it stood.

## render 12: the session's seed — done

`sim/sim_view.h`:

```cpp
uint32_t SessionSeed(const Sim& sim);
```

It is the number the simulation's random stream was seeded with in `Start()`: the replay header's seed
when a replay plays, the session's seed for a new game (`--seed N`, or what the lobby's host hands
out). So every player of a game and its replay get the same value, and two games differ unless they
were started with the same seed on purpose. Valid after `Start()`; 0 before. Reading it draws nothing
from the stream, and the simulation does not use what you derive from it.

One thing to know: the seed says nothing about the clock. `Sim::tick()` (ten to the second) is the
clock every player shares; a replay that is fast-forwarded reaches a given tick at another wall time,
so weather that follows `(seed, tick / 10)` is the same for everyone, weather that follows wall time
is not.

## render 11: the display part of blueprints, handed over in the program — done

`sim/sim_view.h`:

```cpp
struct DisplayLod {
  std::string mesh, albedo, normals, specular, secondary, lookup, shader;  // virtual paths; "" = not set
  float cutoff = 0;                                                        // LODCutoff (0: not set)
};
struct BlueprintDisplay {
  std::string id;          // as UnitView::blueprint and ProjectileView::blueprint name it (a prop: its .bp path)
  int kind = 0;            // 0 unit, 1 projectile, 2 prop
  float uniformScale = 1;  // Display.UniformScale (1 when not set)
  std::vector<DisplayLod> lods;
  std::string icon; int iconSortPriority = 0; float iconFadeInZoom = 0;   // units only
  float selectionSizeX = 0, selectionSizeZ = 0;                           // units only
};
void BlueprintDisplays(const Sim& sim, std::vector<BlueprintDisplay>* out);
```

It is your struct with three additions: `kind`, since the three kinds share the list; `lookup` (the dump
has it); and "not set" is `""` or 0 where the dump wrote "-". It reads the same tables as
`tools/dump_display.lua` (`__blueprints[id].Display.MeshBlueprint` → that mesh blueprint's `LODs`), so
what the scripts and the mods changed after loading is in it, and it lists the same blueprints: with the
lab's mods 1400 of them (671 units, 165 projectiles, 564 props) with 2278 levels, the dump's own count.
The unit fields are `StrategicIconName`, `StrategicIconSortPriority`, `Display.Mesh.IconFadeInZoom`,
`SelectionSizeX` and `SelectionSizeZ` as the blueprint has them (0 or `""` when it does not set one; the
engine's own default for a missing value was not looked up).

Call it once after `Start()`; the blueprints do not change afterwards. `--view` logs the counts and one
unit's line.

## ui 6: a building's site near its builder; a new commander's fire state — both are the original's

**1. The site.** Nothing to do with the builder's distance. There is a mass deposit at 680.5, 350.5
(marker `MASS_196`), and the original's placement test refuses a structure whose **skirt** would lie over
a mass or hydrocarbon deposit unless the structure is the kind built on one (`BuildRestriction`
`RULEUBR_OnMassDeposit` / `RULEUBR_OnHydrocarbonDeposit`). A land factory's skirt is 8 x 8, so every site
centre from 676.5 to 684.5 in x and 346.5 to 354.5 in z is refused: your 681.6, 351.0 and 684.5, 352.5
are inside that square, 686.1, 353.3 and 688.5, 346.5 are outside. The trees there do not block. The order
is taken and the build task then fails at its first step, which is why the commander stays idle (the
original does the same with an order that its interface would not have let through).

The same test also refuses: ground that is not flat enough for the blueprint (`MaxGroundVariation`, over
the footprint, or around the skirt with `FlattenSkirt`), the wrong layer (water depth), a structure on the
footprint, and another structure's footprint inside this one's skirt.

And the site is snapped: the footprint's corner goes to the nearest whole cell, so a 5 x 5 footprint ends
on x.5, z.5 (your order at 686.1, 353.3 was built at 686.5, 353.5) and an even one on whole numbers.

For the build preview there is a call in `sim/sim_view.h`:

```cpp
bool BuildSiteOf(Sim& sim, const std::string& blueprint, const float pos[3], float site[3], bool* canBuild);
```

`site` is where the structure would stand for an order at `pos` (snapped, y on the ground or the water),
`canBuild` the placement test at that site. False: no such unit blueprint. It does not say whether the
builder may build that blueprint (`RestrictedBlueprints`, the live caps) or can get there. It is cheap
enough to ask every frame while the mouse moves. `--view` logs two examples.

**2. Fire state 2 is right for a commander under FAF.** The engine starts every unit at 0 (return fire).
FAF's `lua/sim/Unit.lua` (`OnCreate`, line 348) then calls `self:SetFireState(2)` for every unit in
`STRUCTURE`, `EXPERIMENTAL` or `COMMAND` ("default to ground fire"); `StructureUnit.lua` and
`ExternalFactoryUnit.lua` do the same. So a new commander, every building and every experimental shows
2, and an ordinary tank shows 0. FAF's own order button has an entry for 2 (`orders.lua`,
`retaliateStateInfo`: 0 return-fire, 1 hold-fire, 2 "stand-ground" / `HoldGround`).

## ui 5: unit states for the panels — done

`sim/sim_view.h` has two more calls:

- `UnitStateOf(sim, unitId, &state)` fills a `UnitStateView` (false: no such unit). Ask for the units a
  panel shows, not for every unit every beat.
- `RestrictedBlueprints(sim, army)` gives the ids of the unit blueprints that army may not build
  (`AddBuildRestriction`), in blueprint order; `army` is 1-based.

What `UnitStateView` holds, by your five points:

1. **`queue`**, first to last: per entry `id` (the command's id, what `CommandWriter`'s
   `RemoveCommandFromQueue`, `SetCommandTarget`, `IncreaseCommandCount` and the others take), `type`
   (the engine's command numbers: 2 Move, 7 BuildFactory, 8 BuildMobile, 10 Attack, 15 Guard, 16 Patrol,
   19 Reclaim, 20 Repair, 21 Capture, 22 / 24 transport load / unload, 27 Upgrade, ...), `hasPosition`
   and `position`, `entity` (the target's entity id, 0xffffffff = none), `blueprint` (a build's
   blueprint id) and `count`.
2. **`focus`, `guarded`, `creator`**: entity ids, 0xffffffff = none. `guarded` is the unit whose guard
   order it is carrying out.
3. **Switches:** `fireState` (0 return fire, 1 hold fire, 2 hold ground), `scriptBits` (bit n = toggle n
   on), `paused`, `repeatQueue`, `autoMode`, `autoSurfaceMode`, and `commandCaps` / `toggleCaps`: the
   RULEUCC_ and RULEUTC_ bits as they are now (the blueprint's, as `AddCommandCap`, `RemoveCommandCap`,
   `AddToggleCap` ... changed them), so a button can follow an upgrade.
4. **Meters:** `shieldRatio` (what the shield's script last gave `SetShieldRatio`; 0 = none or down),
   `fuelRatio` (-1 = the unit uses no fuel), `tacticalSiloCount` / `nukeSiloCount` (missiles in store),
   and the economy, [0] energy and [1] mass, all per second: `productionPerSecond` and
   `consumptionPerSecond` (what the unit's scripts ask for), `produced` and `consumed` (what the last
   beat really gave and took), and `buildRate` as it is now.
5. **Restrictions:** `RestrictedBlueprints` above. (What a commander cannot build before an upgrade is
   not a restriction: it is in the blueprint's build categories, and the unit's own allowed set changes
   with `AddBuildRestriction` / `RemoveBuildRestriction` on the unit; say if you want that set too.)

Two honest limits: `repeatQueue` and `autoSurfaceMode` are kept as the player set them (the info pairs
`SetRepeatQueue` / `SetAutoSurfaceMode`) but the engine does not act on them yet (a factory does not
repeat its queue; a submarine does not surface by itself); and `shieldRatio` is the script's number,
so it moves only when the shield's script updates it.

`moho64 --session <file> --ticks N --view` logs the state of the first three units that have orders and
each army's restriction count.

## render 10: beams and trails — read from the exe

Read: `CEfxBeam::OnTick` 0x655350 and `Update` 0x654f30, the beam's quad 0x491760, the beam draw
0x491e40, `RBeamBlueprint`'s fields and defaults (0x50eef0), `CEfxTrailEmitter::Tick` 0x671850 and
`OnTick` 0x671d90, the trail blueprint's fields (0x50f330).

**1. Colours.** Yes: red, green, blue, alpha, as floats ("RGBA start color of beam"). They go into the
vertices unchanged, `StartColor` on the two corners at the beam's start and `EndColor` on the two at
its end. Default for both: (1, 1, 1, 0).

**2. Thickness / Size.** The engine does no arithmetic on either. A beam's four corners each carry the
`Thickness` and the beam's direction, with the direction's sign flipped on the two sides; a trail
segment carries `Size` ("Startsize"). Whether the vertex shader pushes each side out by the whole value
(so it is half the width) is in the game's fx file, not in the exe.

**3. Length and Lifetime.**
- A beam on one entity runs from its `POSITION` parameter to `POSITION + (0, 0, LENGTH)` in the frame of
  the bone it is on (the entity's own frame for bone -1): along the bone's +Z, so a negative `Length`
  points backwards. Default 10.
- A beam between two entities ignores `Length`: its ends are the two bones (the `POSITION` and
  `ENDPOSITION` parameters are offsets in each bone's frame). If the second entity is gone the original
  does not end the beam: it falls back to the one-entity form above until the script destroys it. So
  the effect events (8) now end a beam only with its first entity.
- A free beam (`CreateBeamEmitter`, before `AttachBeamToEntity`) runs from `POSITION` to `ENDPOSITION`
  in world space.
- I found no special value in this code, 2000 included: a `Length` is used as it is. How a weapon's
  collision beam gives the emitters attached to it their real length was not read (the game's own
  scripts do not call `SetBeamParam('LENGTH', ...)` for it, so it is on the engine's side); for those
  beams `BeamViews` has the start and the end.
- `Lifetime` is in ticks and counts down by one each tick **only while the beam is not attached to an
  entity**; when it reaches 0 the beam is destroyed (a start value of 0 or less never ends). An
  attached beam ignores it and lives until its entity is gone or the script destroys it. Default 1.

**4. Texture.** **v runs along the beam** (your guess): 0 at the start, `n` at the end; u is 0 on one
side and 1 on the other. `n` is 1 when `RepeatRate` is 0 (the texture stretched once over the beam),
otherwise `length * RepeatRate`: `RepeatRate` is per world unit ("how often the texture repeats per
ogrid"), not per tick. `UShift` and `VShift` are handed to every vertex unchanged as a second pair;
what the shader multiplies them by is in the fx file. The positions are interpolated between the
entity's last and current transform for the frame.

**5. Blend mode.** A beam without `Blendmode` is **3, add** (the blueprint's default). The numbers are
those of particles, 0 to 4 (no refraction). The technique is `TBeam_OneTexture_<mode>`, or
`TBeam_TwoTexture_<mode>` when the beam has a second texture. Other defaults: `LODCutoff` 200,
`UShift` / `VShift` / `RepeatRate` 0.

**6. Trails.** **One segment per tick**, from where the emitter was at the tick's start to where it is
at its end (the entity's bone, moved by the `POSITION` parameter). A segment carries both ends, the
previous segment's direction and its own (for the join), its texture coordinates, `Size`, `SortOrder`,
the two textures, the blend mode, and the technique `TPolyTrail`.
- `TrailLength`: yes, the time a place of the trail stays, in ticks (the buffer keeps a segment for
  `TrailLength` + 2 ticks; start times are counted as for particles).
- `TextureRepeatRate` is **per world unit** ("texture repeat rate in units"): the coordinate along the
  trail is `TextureRepeatRate * distance travelled since the trail began`, at each end of a segment.
- `Lifetime`: ticks; negative = until destroyed. An attached trail ends when its entity is gone.
- A trail that could not emit (not visible) replays the missed ticks, at most `min(TrailLength, 24)`;
  with more than that it starts afresh (no join to the old part).

`SetBeamParam` names, in the engine's order: POSITION_X/Y/Z, ENDPOSITION_X/Y/Z, LENGTH, LIFETIME,
STARTCOLOR_R/G/B/A, ENDCOLOR_R/G/B/A, THICKNESS, USHIFT, VSHIFT, REPEATRATE, LODCUTOFF. A trail has
POSITION_X/Y/Z, LIFETIME, LENGTH, SCALE.

## render 9: how the original turns an EmitterBlueprint into particles — read from the exe

Read: `CEfxEmitter::OnTick` 0x65dac0, `Tick` 0x65ce00 (one call emits one tick's particles),
`ProcessLifetime` 0x65c700, `UpdateCurve` 0x65c7f0, the curve reader 0x514e50, the vertex fill 0x493210.
Your guesses were mostly right; the differences are marked **bold**.

**1. Time.** Everything is in ticks. `Lifetime` and `Repeattime` (the blueprint's own field texts say
"in ticks"), curve `x`, `LifetimeCurve`'s value, and the shader's `time`: a particle's start time is
written as `time + offset`, the offset being counted in ticks (below), and its lifetime goes into the
vertex unchanged. **Any negative `Lifetime` means "until destroyed"**: the test is
`Lifetime >= 0 and age >= Lifetime`, so -0.2 and -50 are the same. The emitter's age is its `TICKCOUNT`
parameter; it grows by `TICKINCREMENT` after each tick.

**2. Curves.** A key is `{x, y, z}`. At time `t`: before the first key, the first key; after the last,
the last; between two keys **both `y` and `z` are interpolated linearly**, and the value is
`y + (u - 0.5) * z` with `u` uniform in [0, 1): your `y + uniform(-z/2, +z/2)`. No keys: 0. The random
numbers come from a generator of the effects' own (a Mersenne twister apart from the simulation's), so
nothing here needs to match a replay. **Yes, the curve is read at age modulo `Repeattime`**
(`fmod(age, Repeattime)`, made positive). A curve with a single key and no spread (`|z|` near 0) is
read once and kept; every other curve is read, and its spread rolled, for each particle.

**3. Emission.** `EmitRateCurve` is particles per tick. Each tick: `n = rate(t) + carry`; `floor(n)`
particles are made and the fraction is carried to the next tick. `InterpolateEmission`: the `floor(n)`
particles of a tick are spread over the tick, particle `i` at the fraction `i / floor(n)`: the emitter's
position is interpolated to that fraction, the curves are read at `t + fraction`, and the particle's
start time gets `+ fraction`. Without it all are made at the tick's position and time. Catching up:
an emitter that could not emit (see `EmitIfVisible` under 9) counts the missed ticks; when it emits
again it first runs the tick once for each missed one (at most 24, and at most the longest particle
lifetime, `ceil(max(y + z/2))` over `LifetimeCurve`), each with a start time that many ticks in the
past, then the current tick. The `CatchupEmit` flag itself is not tested in any of the functions read,
so as far as this reading goes the catching up happens with or without it.

**4. Position, velocity, acceleration.**
- Start position: `(POSITION param + X/Y/ZPosCurve(t) * scale)` taken through the emitter's transform
  (the bone's or entity's rotation and position), plus the emitter's "size" (see 5).
- Velocity: `(XDirection, YDirection, ZDirection)(t) * scale`, **not normalised**, times
  `VelocityCurve(t)`; per tick. `LocalVelocity`: the direction is turned by the emitter's rotation at
  emission before the multiply.
- Acceleration: `(XAccel, YAccel, ZAccel)(t) * scale`, per tick squared; `LocalAcceleration` turns it
  by the emitter's rotation. `Gravity` then subtracts **0.02 from the y component** (per tick squared;
  that is 2 world units per second squared, not the simulation's 4.9).
- `scale` is the `SCALE` parameter (what `ScaleEmitter` sets), 1 by default: it multiplies positions,
  directions (so speed), accelerations and both sizes, not lifetimes or rates.

**5. Sizes.** `StartSizeCurve(t) * scale` goes into the vertex as the size, and
`(EndSize - StartSize) / lifetime` as its growth per tick; whether the shader treats size as half an
edge or a whole one is in `particle.fx`, the engine passes the number through. **`SizeCurve` is not a
particle size: it is the size of the emitter** ("Size of emitter over time"). Each particle starts at a
random point of a horizontal disc around the emitter's point, of diameter `SizeCurve(t) * scale`: a
random direction in the world's XZ plane (`normalize(u1 - 0.5, 0, u2 - 0.5)`) times
`(u3 - 0.5) * SizeCurve(t) * scale` (so denser near the middle). The disc is not turned by the bone.

**6. Rotation.** `InitialRotationCurve` in degrees and `RotationRateCurve` in degrees per tick; both
are multiplied by pi/180 at emission.

**7. Texture and ramp.** Into the vertex go: `FrameRateCurve(t)` as it is; `1 / TextureFramecount`;
`floor(TextureSelectionCurve(t)) / TextureStripcount` (the strip's offset; 0 when the texture is not
animated); `1 / TextureStripcount`; `RampSelectionCurve(t)` as it is (the engine does no flooring or
centring on it). How the shader turns those into texture coordinates is in `particle.fx`.

**8. `Blendmode`.** Your table is right: 0 `_ALPHABLEND`, 1 `_MODULATEINVERSE`, 2 `_MODULATE2XINVERSE`,
3 `_ADD`, 4 `_PREMODALPHA`, 5 `_REFRACT` (the suffix of the technique's name; 5 is drawn in the
refraction pass).

**9. Which technique.** The name is a base plus the blend suffix. Base: `TRampAnimate` if
`TextureFramecount > 1` or `TextureStripcount > 1`, else `TRamp`; then `Align` if `AlignRotation`; else
if `AlignToBone`: `Flat` when `Flat` is also set, otherwise `AlignToBone`; else `Flat` if `Flat`; else
nothing. So e.g. `TRampAnimateAlign_ADD`. At emission `AlignToBone` also does this: with `Flat`, the
initial rotation is `atan2(-f.x, f.z)` of the bone's forward axis `f` (the curve is not used); without
`Flat`, **the particle's velocity is replaced by the bone's forward axis** (the shader aligns to it).
`SnapToWaterline`: a particle's start height is raised to at least the water level + 0.01, unless the
emitter's `SortOrder` is below -101 (the console value `efx_ParticleWaterSurface`, "sort order at which
particles start rendering under water"), where it is kept at most at the water level - 0.01.
`OnlyEmitOnWater`: a particle whose start point has terrain above the water is dropped; the others
start at the water level + 0.01. `EmitIfVisible`: the emitter emits in a tick only if some camera sees
it; for an emitter attached to an entity that camera must also have it within `LODCutoff` (view depth;
0 = no limit). Without `EmitIfVisible` it always emits and `LODCutoff` is not looked at.
`CreateIfVisible` destroys the emitter at its first tick if no camera sees it. `SortOrder` goes into
the draw bucket's key with the textures and blend mode.

**10. Drag.** `ParticleResistance` (a bool) is the shader's `DragEnabled` for that bucket.
`ResistanceCurve(t)` ("the drag coefficient divided by the mass") is the per-particle value `r`; the
vertex carries `r`, `1 / r` and `1 / r^2` (`DragCoeff`, `invDragCoeff`, `invDragCoeffSq`).

**11. Who follows.** `CreateEmitterAtBone` and `CreateEmitterAtEntity` copy the bone's / entity's world
transform once and stay there. `CreateAttachedEmitter` and `CreateEmitterOnEntity` are tied to the
entity: each tick the emitter's transform is the entity's bone again, and it ends when the entity is
gone or queued for destruction. In both cases **a particle, once made, is on its own**: its position,
velocity and acceleration are world values fixed at emission; nothing carries it along.

The vertex, for comparing with the shader's inputs (0x5c bytes, in this order): position (3), initial
rotation, start size, size growth, velocity (3), rotation rate, acceleration (3), start time, lifetime,
frame rate, 1 / frame count, strip offset, ramp selection, 1 / strip count, r, 1 / r, 1 / r^2.

The names `SetEmitterParam` takes, in the engine's order: POSITION_X, POSITION_Y, POSITION_Z, TICKCOUNT,
LIFETIME, REPEATTIME, TICKINCREMENT, BLENDMODE, FRAMECOUNT, USE_LOCAL_VELOCITY, USE_LOCAL_ACCELERATION,
USE_GRAVITY, ALIGN_ROTATION, INTERPOLATE_EMISSION, TEXTURE_STRIPCOUNT, ALIGN_TO_BONE, SORTORDER, FLAT,
SCALE, LODCUTOFF, EMITIFVISIBLE, CATCHUPEMIT, CREATEIFVISIBLE, SNAPTOWATERLINE, ONLYEMITONWATER,
PARTICLERESISTANCE. All are floats; the flags count as set when greater than 0. The curves
`SetEmitterCurveParam` names: XDIR, YDIR, ZDIR, EMITRATE, LIFETIME, VELOCITY, X_ACCEL, Y_ACCEL, Z_ACCEL,
RESISTANCE, SIZE, X_POSITION, Y_POSITION, Z_POSITION, BEGINSIZE, ENDSIZE, ROTATION, ROTATION_RATE,
FRAMERATE, TEXTURESELECTION, RAMPSELECTION (each with the suffix `_CURVE`).

Not read: how `ResizeEmitterCurve` rewrites a curve, the "camera sees it" test itself, beams and trails.
Ask for whichever you need. (`SetEmitterCurveParam` is read since: it replaces the curve by one key, see
the entry at the top of this file.)

## render 8: the effects the scripts create — done (emitters, beams, trails, light particles, decals)

`sim/sim_view.h` has `EffectEvent` and `EffectEvents(sim, &list)`; the simulation keeps the list only
while `sim.collectEffects` is set (set it before the first beat, with `collectSync`). Call it once per
beat after `Tick()`; it hands over the events since the last call, in the order the scripts made them.

The struct is your proposal with three small changes:

- `text` and `text2`: a decal has two textures and a type, a light particle a texture and a ramp, so
  there are two "other strings", in call order.
- `entity` / `entity2` are entity ids as in `UnitView::id`, and **none is 0xffffffff**, not 0: id 0 is a
  real unit (the first unit of army 1).
- `army` is what the script passed (1-based, or -1).

What each function fills (the argument order is the original's own help text, read from the exe):

| function | entity, bone | entity2, bone2 | blueprint | numbers | text |
|---|---|---|---|---|---|
| `CreateAttachedEmitter`, `CreateEmitterAtBone`, `CreateTrail`, `CreateBeamEmitterOnEntity` | yes | | the blueprint | | |
| `CreateEmitterAtEntity`, `CreateEmitterOnEntity` | entity, bone -1 | | the blueprint | | |
| `CreateBeamEmitter(blueprint, army)` | none | | the blueprint | | |
| `CreateBeamEntityToEntity`, `AttachBeamEntityToEntity` | yes | yes | the blueprint | | |
| `CreateAttachedBeam` | yes | | the texture | length, thickness | |
| `CreateBeamToEntityBone` | yes | yes | the texture | thickness | |
| `CreateLightParticle`, `CreateLightParticleIntel` | yes | | the texture | size, lifetime | the ramp |
| `CreateDecal` | none; `position` given | | texture 1 | heading, sizeX, sizeZ, lod, duration, fidelity | texture 2 in `text` and the type in `text2`; when the script gave no second texture (nearly always) the type is in `text` and `text2` is empty: the strings fill the two fields in order and an empty one takes no place |
| `CreateSplat` | none; `position` given | | the texture | heading, sizeX, sizeZ, lod, duration, fidelity | |
| `CreateSplatOnBone(entity, offset, bone, ...)` | yes | | the texture | offset x, y, z, sizeX, sizeZ, lod, duration | |

`Call` events carry the method's name, its numbers and its strings: `ScaleEmitter`, `OffsetEmitter`,
`SetEmitterParam` (`text` = the parameter), `SetEmitterCurveParam`, `ResizeEmitterCurve`, `SetBeamParam`,
and `AttachBeamToEntity(emitter, entity, bone, army)`, which is a call on the emitter that
`CreateBeamEmitter` made (with `entity`, `bone`, `army` and `position` filled).

`Destroy` comes when the script destroys the effect (its own `Destroy`, or a trash bag's) and, at the
beat's end, when the entity it is attached to is gone. "Attached" is decided by the function:
`CreateAttachedEmitter`, `CreateEmitterOnEntity`, `CreateTrail`, every beam function and
`AttachBeamToEntity` are; `CreateEmitterAtBone` and `CreateEmitterAtEntity` are not (they are what an
explosion uses on the entity that is about to vanish), nor are light particles and decals, which end by
their own lifetime. For emitters this split is read from the exe: `CreateEmitterAtBone` and
`CreateEmitterAtEntity` copy the bone's / entity's world transform into the emitter once (0x659390,
0x659300) and never set its entity; the attached kinds go through `CEffectImpl::SetEntity` (0x6592d0),
which sets the "attached" byte, and `CEfxEmitter::ProcessLifetime` (0x65c700) ends an attached emitter
when its entity is gone or queued for destruction. Beams and trails do the same (`CEfxBeam::Update`
0x654f30, the trail's 0x6717e0), with one exception: a beam between two entities ends only with its
FIRST entity (see render 10, point 3). An effect whose blueprint has a lifetime simply runs out: no
event.

`position` of a `Create` is the attach point (the entity's bone in this beat's pose) when the list is
taken; if the entity is gone by then, where it was at the call. Effects on entities that are neither
units nor projectiles: the common one is a weapon's collision beam (bone 0 its start, 1 and 2 its end),
which you have in `BeamViews` by the same id; shields and script-made entities do move with their unit
and have no view yet. Say when you meet one that matters.

`moho64 --session <file> --ticks N --view` logs the counts by function and one sample. For scale: the
recorded skirmish's set-up (seven M28 armies) makes 8,535 effects in its first 1,500 beats, with 12,957
calls on them (10,285 of them `ScaleEmitter`) and 4,542 ends.

## render 7: the colour of a projectile's mesh — white (read from the exe)

The mesh instance's colour is an argument of `MeshRenderer::CreateMeshInstance` (0x7df530), and the
original has two callers:

- `UserUnit::CreateMeshInstance` (0x8c00e0) passes the army's colour (`army+0x148`, an ARGB value; -1 when
  the unit has no army).
- `UserEntity::CreateMeshInstance` (0x8b88d0), which every other entity uses, projectiles and props
  alike, passes the constant **-1 = 0xFFFFFFFF, opaque white** (`push -1` at 0x8b892e).

So `vertex.color` is white for projectiles and props, and the army's colour only for units. Your guess
was right; blue rockets would be wrong.

## render 6: the live frame, bone poses and projectiles — done

In `src/sim/sim_view.h`, beside `UnitViews` and `ArmyViews`, read once per beat after `Tick()`:

- **Bone poses.** `UnitPoses(sim, &list)` gives, in entity-id order, every unit that is not in its rest
  pose as `{ id, bones }`; `UnitPoseOf(sim, unitId, &bones)` gives one unit's (false: no such unit; an
  empty list: rest pose). A `BonePose` is `{ bone, position[3], orientation[4] (x, y, z, w) }`: the bone's
  transform **relative to the unit**, composed down the bone chain from the unit's origin, positions in
  world units (the mesh's `UniformScale` applied). `bone` is the index in the `.scm` bone list. A bone is
  listed when its transform in the unit's frame differs from the rest pose, so the children of a turned
  bone are listed too; a bone not listed stands at rest (rest = the same composition of the `.scm`'s
  parent-relative positions x scale and rotations). This is the pose the engine's weapons, animations and
  other manipulators wrote this beat. The unit's own position and orientation are `UnitViews`'.
- **Projectiles.** `ProjectileViews(sim, &list)`: `id`, `blueprint` (the projectile blueprint's id, a
  path such as `/projectiles/.../x_proj.bp`), `army` (1-based), `position`, `orientation`, `velocity`
  (world units per second), in creation order.
- **Beams.** `BeamViews(sim, &list)`: the collision beams that are on: `id`, `army`, `unit` (the firing
  unit's entity id), `start` and `end`. `blueprint` is empty today: the collision beam is the invisible
  thing that deals the damage; the visible beam is an effect the scripts attach to it
  (`CreateBeamEmitter` / `AttachBeamEntityToEntity`), which the engine records as calls only. Say when
  you want those effect calls handed over (it is item 5 of render 2, with the build beams).

Checked on the Experimental replay (`moho64 --sim <replay> --ticks 900 --view`): up to 15 posed units
with 385 bones at one beat, 356 projectiles, 2 beams. Entity ids of projectiles and beams are the
engine's ids (reused after a quarantine, like units').

## ui 4: what the in-game interface needs from a running simulation — done (first form)

All four parts exist; `moho64 --session <file> --view [--orders <file>]` exercises them headless.

**1. The sync table of each beat.** `sim.collectSync = true` before the beats; after every `Tick()` or
`PausedBeat()`, `std::vector<std::string> Sim::TakeSync()` gives the beats' `Sync` tables as bytes
(`SerializeLua` form, oldest first, one entry per beat: a paused beat syncs too). For each entry the host
does what the original's `CWldSession::DoBeat` does in the user state: `PreviousSync = Sync`,
`Sync = <the table read with PushSerializedLua>`, then `OnSync()`. As in the original (`Sim::Sync`,
0x747b11) the engine adds `PausedBy` and `TimeoutsRemaining` while a pause holds and `__ArmyStats`
(only its `Tick`) when there is a focus army, and after handing the table over runs the scripts'
`ResetSyncTable()`. That reset now happens every beat in every run, replays included; the gate is
unchanged by it. Not built: `Sync.Cheaters` and the statistics' `Children` (the debug window
`/lua/debug/ArmyStats.lua`).
A table that contains itself is written as nil with a warning; the game's `Sync` has none.

**2. Session facts** (`src/sim/sim_view.h`, plain structs; the host builds the Lua tables):
- `ArmyViews(sim)`: per army `index` (1-based), `name`, `nickname`, `faction` (0-based), `human`,
  `civilian`, `outOfGame`, `showScore`, `alliance` towards every army, and the colour as the original
  derives it: `armyColor` / `playerColor` are the options' indices into `/lua/GameColors.lua`
  (`ArmyColors`, `PlayerColors`; 0 = not given, a civilian then takes `CivilianArmyColor`), `rgb` is set
  when a script called `SetArmyColor` (it wins).
- `EconomyTotalsOf(sim, army)`: `income`, `lastUseRequested`, `lastUseActual`, `stored`, `maxStorage`,
  `reclaimed`, each `[0]` energy and `[1]` mass, rates per beat.
- `sim.focusArmy` (0-based, -1 none), `sim.tick()` (ten ticks a second), and `sim.sessionScenario`: the
  scenario info as launched, serialized, for `SessionGetScenarioInfo()`. (The scripts' own `ScenarioInfo`
  is not handed out: during the game it collects engine objects, units by name among them.)

**3. Units** (`UnitViews(sim, &list)`, same header): `id`, `blueprint`, `army`, `position`,
`orientation` (x, y, z, w), `health`, `maxHealth`, `fractionComplete`, `workProgress`, `dead`,
`beingBuilt`, `selectable`, `queueLength`, `headCommand`. In entity-id order; ids as in the recordings
(army 1's first unit has id 0).

**4. Orders** (`src/sim/command_writer.h`). The interface's orders are the command stream's messages, the
same bytes a replay holds, so one piece of code carries out both:

    CommandWriter writer(source);     // session.header.armies[session.focusArmy].source
    ... during the frame, whatever the player did:
    StreamIssueData d;                // sim/commands.h: type, target, target2, formation, blueprint, cells, count
    d.type = 2;                       // the engine's command numbers (2 = Move)
    d.target.type = 2; d.target.pos = {x, y, z};
    writer.IssueCommand(unitIds, d, /*clear queue (no shift)*/ true);
    writer.LuaSimCallback(name, SerializeLua bytes of the argument table, unitIds);   // SimCallback
    writer.ProcessInfoPair(unit, "SetFireState", "1");  RequestPause();  Resume();  ...
    ... before the next beat:
    ApplyCommands(sim, writer.Take());

`Take()` puts `SetCommandSource` in front, as a client does. Every message the engine carries out has its call
(`CreateUnit`, `DestroyEntity`, `SetCommandTarget`, `RemoveCommandFromQueue`, `ExecuteLuaInSim`, ...).
Keeping the taken bytes with `CommandWriter::AdvanceMessage()` between beats gives a replay's body.
Checked: a Move for the commander walks it there; a pause holds the tick while beats and sync tables go
on; a callback reaches `/lua/SimCallbacks.lua`'s `DoCallback`.

`writer.IssueFactoryCommand(factoryIds, d, clear)` is the rally order (what a factory's products are told
to do: the order a player gives with a factory selected); it is carried out since 2026-10-12. Building
from a factory is an ordinary `IssueCommand` of type 7 (BuildFactory) with the blueprint and a count.

Known gaps on the engine side (updated 2026-10-17): classic AI opponents (below; their threat map and
platoons are built since 2026-10-16, their build functions are in work), `Dock`, `CallTransport` or
`TransportReverseLoadUnits` on a transport that cannot fly (CUnitCallLandTransport 0x6007c0 is not built), and
`TransportReverseLoadUnits` with an air staging platform (carried out otherwise since 2026-10-17, ui 39).
`DetachFromTransport`, `Dock` (it had no case at all before, even on a
staging platform) and a teleport through another unit's beacon are carried out since 2026-10-16. The
others are carried out (nuke and tactical launch, overcharge,
capture, sacrifice, teleport, special action, kill / destroy self and pause since 2026-10-12; a
captured unit changes army through `ChangeUnitArmy`).

**AI opponents, where they stand (2026-10-12).** A new game against M28 runs: the recorded skirmish's
set-up (the player and seven M28 AIx armies on Seton's Clutch) started as a new game through `--session`
plays ten game-minutes headless, the AIs building up to 1,400 units. The classic AIs (`rush`, `adaptive`,
`turtle`, ...) now get their commander and their economy (the personality object and the platoons'
`OnCreate` call were missing) but do not build: their brains rest on engine functions that are still
stubs (39 of `CAiBrain`'s: threat map, `BuildPlatoon`, `DecideWhatToBuild`, attack vectors; 44 of
`CPlatoon`'s: forming platoons, their orders). That block comes after the interface can show a game.
**Update 2026-10-16:** those functions are built from the exe (src/sim/ai_threat.cpp, ai_platoon.cpp,
ai_brain.cpp, ai_build.cpp; engine d7e3624, 64a3518, e78131a). A rush AI in a 6000-tick test game now
builds factories, mass extractors (and upgrades them), power and engineers, forms 19 platoons and sends
tanks to attack. Not yet checked against the original game (an oracle run with the classic AI would need a
probe of FindPlaceToBuild / BuildStructure / DecideWhatToBuild). The interface chat's request 24 (a crash in
a game against the classic AI) was a Lua VM bug the new AI code exposed; fixed (see ui 24 above).

## render 5: standing request to merge `render` into `main` — accepted

Chris confirmed it (2026-10-12): the engine track merges `origin/render` into `main` at its convenient
stops, without a pull request, after checking that only `render/` and `docs/render-*` paths differ. The
interface track merges its own branch. First merge: 2026-10-12, `render` at 2b92b8a (commit 3934213).

Note for the renderer track: the engine's answers to render 1 to 5 are all in this file, not under the
requests in `render/REQUESTS.md`: that file is yours, and no file has two writers. Read this file on
`main` after each merge.

## render 4: `.scm` version 6 = version 5 with 32-bit indices — agreed

`src/sim/skeleton.cpp` reads the header's bone offsets and the bones, checks only the `MODL` mark and
never the version or the index data, so it takes such a file today. No version check is planned; if one
is ever added it will accept 5 and 6.

## ui 1 and ui 2: mounting as a library function; the engine-object handle outside the simulation — done

1. `src/script/mount.h`: `bool MountFromInitFile(const std::string& initFile, const std::map<std::string,
   std::string>& folders, Vfs&, std::vector<std::string>& hookDirs)`, exactly the proposed form. It runs the
   init file (with `InitFileDir` and `SHGetFolderPath`; `folders` replaces the system's answer per name),
   mounts its `path` table and appends its `hook` table. `src/app/main.cpp` calls it; the interface host can
   drop its copy.
2. `src/script/script_handle.h` / `.cpp`: `ScriptObject`, `BindObject`, `PushObject`, `GetObject`,
   `CheckAnyObject`, `CheckObject<T>`, `ToObject<T>`, the `ScriptTypeOf<T>` hook for fast type tests, and
   `RegisterObjectHandle(L)` (creates the `moho64.cobject` metatable; call it once per state instead of
   creating the metatable by name). `src/script/threads.cpp` includes only this. `src/sim/script_object.h`
   keeps the simulation's class tables and stubs and includes the handle, so existing includes keep working.

The interface build was checked against this as it is (`ui/build_ui.py`, which globs `src/script`): it
builds unchanged. It can now stop linking `src/sim/script_object.cpp` and include
`script/script_handle.h` where it included `sim/script_object.h`. The replay gate is unchanged.

## ui 3: start a simulation from the lobby's game configuration — done

A session can now be started from the table the lobby script hands to `CLobby:LaunchGame`, built the way
the original builds it (FA exe 0x7c38c0, read for this). New files: `src/sim/session.h`, `session.cpp`.

From the command line (headless, for checking a recorded configuration):

    moho64 --init <init.lua> --session <file.lua> [--seed N] [--mods uids.txt] --ticks N [--log out.log]

`<file.lua>` is what `moho64ui --session-out` writes. Without `--mods` the mods are the configuration's
`GameMods`; without `--seed` the seed comes from the clock and is written to the log.

From a host (the interface host links the simulation library, CMake target `mohocore`):

    #include "sim/session.h"
    SessionDescription session;
    LaunchParams params;              // seed, the lobby's own peer id, whether there are network peers
    params.seed = <a number the host picks once per game>;
    std::string why;
    if (!SessionFromGameConfig(L, gameConfigIndex, params, &session, &why)) -> the script's LaunchFailed(why)
    Sim sim(&vfs, hookDirs, uids);    // uids: the uid of each GameMods entry, in order
    sim.sessionMods = session.header.mods;
    sim.LoadRules();                  // the blueprints, with the session's mods
    sim.Start(session);               // map, armies, brains, props, BeginSession
    ... then one sim.Tick() per beat (ten a second at normal speed); the script's GameLaunched() on success

What `SessionFromGameConfig` does, in the original's order:

- `GameOptions.ScenarioFile` is run in a fresh environment after `/lua/dataInit.lua` (in the state `L`, so
  that state needs `doscript` and the mounted files) and its `ScenarioInfo` taken; the lobby's `GameOptions`
  become `ScenarioInfo.Options`. A missing file or a file without `ScenarioInfo` fails with `NoConfig`.
- The army names are the `armies` of the team named `FFA` in `Configurations.standard.teams`.
  `PlayerOptions[slot]` becomes army number `slot` of that list: its table gets `ArmyName` (the caller's
  table is changed, as in the original). More players than names fails with `StartSpots`. The armies are
  in slot order; empty slots are skipped, so slots 1 and 3 give simulation armies 1 and 2 named `ARMY_1` and
  `ARMY_3`. No players at all gives one army named `default`.
- Unless `GameOptions.CivilianAlliance` is `none`, every name in
  `Configurations.standard.customprops.ExtraArmies` is added after the players as
  `{ PlayerName = 'civilian', ArmyName = <name>, Civilian = true, Human = false }`.
- Every `Human` army's `OwnerID` gets a command source (one per owner, in order of appearance); the army
  whose owner is the lobby's own peer id is the focus army (`params.localOwner`; left at -1 it is the first
  human army's owner). `Observers` get a source each. Pauses are unlimited in a local game; with network
  peers (`params.network`) `GameOptions.Timeouts` is the number of pauses per player and observers get none.
- `CheatsEnabled == 'true'` switches cheats on. `GameSpeed` (`fast`, `adjustable`) is returned in the
  description for whoever drives the clock; the simulation does not use it.
- The result holds exactly what a replay's header holds (map, mods, scenario info, sources, the armies'
  options, seed), so `Sim::Start` treats both alike. A launched session reports `SessionIsReplay() == false`
  and `GetFocusArmy()` = the focus army in the simulation's Lua.

Checked with `_scratch/ui/session_out.lua`: army 1 `ARMY_1` "Tester" (human, focus) with its commander at
the start position, `ARMY_9` and `NEUTRAL_CIVILIAN` as civilians, 600 ticks without a script error.

Three things to know:

1. **The recorded file does not compile as written.** `repr` puts `-- table: <address> (<n> bytes)` after
   every opening brace; for an empty table the closing brace lands inside that comment
   (`GameMods = { -- table: ... (40 bytes)},`), so three braces stay open. `PushGameConfigFile` (what
   `--session` uses) removes exactly those comments before compiling, so the file works as it is; a host
   that passes the live table to `SessionFromGameConfig` is not affected.
2. **Classic AI armies do not play yet** (state as of 2026-10-12 under ui 4, "AI opponents"): they start
   with their commander but do not build. M28 plays.
3. **Nothing reaches the simulation after the start yet**: orders, sim callbacks and the per-beat sync data
   are ui 4. Say which you need first.

## render 3: the display part of blueprints as the simulation has them — done

No engine change was needed: the engine can run a Lua file in the simulation's Lua state, where the
merged blueprints are. The script is `tools/dump_display.lua`:

    moho64 --init <init.lua> --mods <uids.txt> --sim <replay> --ticks 1 --quiet
           --sim-lua tools/dump_display.lua --log display.log

Every line it writes to the log starts with `DISPLAY` and has tab-separated fields:

    <blueprint id> <lod index from 0> <cutoff> <shader> <mesh> <albedo> <normals> <specular> <secondary>
    <lookup> <uniform scale>

`-` stands for a name that is not set; a last line `DISPLAY-END` gives the counts. In the log each line
has the prefix `info: `. It covers every blueprint whose `Display.MeshBlueprint` names a mesh blueprint
with levels of detail: units and props alike. The names are the game's virtual paths after all active
mods' blueprint scripts ran (for example a remastered unit shows its `LabPBR` shader and the mod's
texture paths). Any replay will do: the blueprints depend on the mods, not on the game played. If a field
is missing, add it to the script: it is plain Lua in the game's dialect and belongs to `tools/`, so ask
here rather than editing it from the renderer branch.

## render 1: which blueprint each unit is — done

Recordings written by moho64 now carry a `U` record, exactly as proposed: `u8 'U', u8 length, u16 0,
u32 unit id`, then `length` bytes of the blueprint id padded with zeros to a multiple of 4. It is written
once per unit, before the first `T` record that lists it. Unit ids are reused after a unit is gone, so a
later `U` with the same id means a new unit. `tools/recdiff.py` `load()` returns them as
`R["names"]`, a list of `(unit id, blueprint id)` in order of appearance. Recordings made by the
original game do not have the record; recordings of ours made before this change do not either (make
them again).

## render 2: what a recording cannot show — noted, for the live frame

Agreed as the shopping list for the live frame. What the engine has today, per item:

1. Bone poses: every unit keeps a per-beat pose (positions and rotations per bone) that the weapons and
   animations already write. It can be handed over as "bones that differ from the rest pose".
2. Projectiles and collision beams: id, blueprint, position, orientation are there; a beam's end points
   are computed each beat while it is on.
3. Army facts: colour index and faction come from the session's army options and are available.
4. Props that change: the engine knows which props were reclaimed, knocked over or created (wreckage).
5. Construction: builder and target are known per beat; build beams are script-made effects (emitters
   and beams the scripts create), which the engine currently records as calls without visuals.

Nothing is built for this yet. When the renderer wants to start on the live frame, say so in
`render/REQUESTS.md` with the first two items it wants, and the hand-over is added for those first.
