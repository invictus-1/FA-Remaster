# The interface track's answers (newest first)

Answers to the other tracks' requests to the interface (CLAUDE.md: "the layer asked answers in a file of its own").
The requests themselves are in the asking track's file; each answer names it.

## engine (by message, aicore's phase 6): our AI's per-slot options RemasterAggression and RemasterEconomy, with personality and teacher mode (2026-10-30, ui 04a689a)

Built, all four together (none had a slot control before). Every slot's PlayerOptions carries them, so they reach the
simulation as `ScenarioInfo.ArmySetup[army]` and the replay keeps them:

| Key | Values | Default |
|---|---|---|
| `RemasterPersonality` | `'random'`, `'balanced'`, `'rusher'`, `'turtle'` (strings) | `'random'` |
| `RemasterAggression` | `-2`, `-1`, `0`, `1`, `2` (numbers); shown as -2 Defensive .. +2 Aggressive | `0` |
| `RemasterEconomy` | `-2`, `-1`, `0`, `1`, `2` (numbers); shown as -2 Army first .. +2 Economy first | `0` |
| `RemasterTeacher` | `'off'`, `'on'` (strings) | `'off'` |

- **Where:** a slot with our AI has "Remastered AI Settings..." first in its list; it opens a small dialog of the four.
  The host changes them; the other players see them read-only. Picking another of our AIs (another skill) for the same
  slot keeps them. The slot's row shows what differs from the defaults, as much as fits (e.g. "Remastered AI: Recruit
  (Turtle)").
- **Files:** `ui/data/hook/lua/ui/lobby/data/playerdata.lua` adds the four keys to every PlayerData (FAF's keys are
  fixed when one is made). `ui/data/hook/lua/ui/lobby/lobby.lua` holds the entry, the dialog and the row text.
- **Human slots:** they carry the four at their defaults, unused.
- **Test:** `ui/tests/lobby_aioptions.lua`. It checks the lobby, the joiner's view and the launch's table, and the AI's
  own log line from brain/personality.lua: `aiop 2 .. personality rusher teacher true` and `aiop 3 .. personality
  turtle`.
- **For aicore:** the brain reads Personality and Teacher today. Aggression and Economy are numbers for it to read
  (`tonumber(setup.RemasterAggression) or 0`).
- **Direction, confirmed by the engine (2.7):** Aggression - = defensive, + = aggressive (bolder). Economy - = army
  first, + = economy first. 0 = no change.
- **Personalities:** air and naval players come when the brain has them; tell me and I add them to the list.

## render/REQUESTS.md 50: a setting for the units' lamp colour (2026-10-28, main 57b43fc)

Built as asked: "Unit Glow Colour" in the Options dialog's Remastered tab, after Hidden Unit Outlines, in no preset
(a preference, not a quality step): Team colour (the default) or Faction colour. It runs the console command
`remaster_lamp_colour team|faction` (1 / 0 too), which sets `LookSettings::lampsTeamColour`; `WorldLooks` gives it to
`World::lampsTeamColour` every frame, so a change shows at once in a running game. Test: console (the default, faction,
team again). Not checked by eye in a window: please look once at a night game with it on Faction colour.

## engine (by message, docs/ai-design-goals.md 39f16fe): pings our own AI understands, given by a radial menu (2026-10-28, built: main 57b43fc)

How a ping travels today (FAF, read): the interface's `/lua/ui/game/ping.lua` DoPing sends
`SimCallback({Func = 'SpawnPing', Args = data})`, data being FAF's SyncPingData: `Owner` (army from 0), `Location`,
`Type` ('Alert' | 'Move' | 'Attack' | 'Marker'), `Lifetime`, `Mesh`, `Ring`, `ArrowColor`, `Sound`, and for a marker
`Marker`, `Name`, `Color`. The simulation's `/lua/SimPing.lua` SpawnPing (one ping a second per army: PingLimit 1,
PingTimeout 1 s) makes the ping's mesh, syncs it to the players (Sync.Ping), and calls every ALLIED brain:
`brain:DoPingCallbacks(data)`, and `brain:DoAIPing(data)` when the pinger is not an AI. So a ping is a sim callback:
in the command stream, the same for every player, and in the replay.

The radial pings, as I would build them (ours; nothing new on the wire but two fields):
- The same callback and table, so FAF's own code (meshes, rings, the arrows at the screen's edge, sounds, the limit,
  markers) and other mods keep working: `Type` stays one of FAF's four, picked for its look: Attack and Attack ACU
  'Attack' (red), Assist and Defend 'Move' (blue), Experimental here, Expand here and Retreat 'Alert' (yellow).
- Two fields more in `data`: `Intent` = 'attack' | 'assist' | 'defend' | 'attack_acu' | 'experimental' | 'expand' |
  'retreat' (the AI's word for it), and `Target` = the entity id of the unit under the cursor when the ping key went
  down, if there was one (an ally's unit for assist / defend, an enemy's for attack / attack_acu). The place is where
  the key went down, not where the drag ends.
- The AI reads them in its brain's DoAIPing / DoPingCallbacks (an allied human's ping: `data.Intent`, `data.Location`,
  `data.Target`). THE SIM SIDE MUST CHECK `Target`: callback arguments come from a player's machine, so a Target that
  is no entity, or one the pinger's army cannot see now, is dropped (the ping stays a place). Pings without `Intent`
  (FAF's own four keys, an old replay) mean what they mean today.
- The menu: a key action of ours, rebindable (which key by default is Chris's to say; FAF's ping keys stay too).
  Holding it opens a ring of wedges round the cursor, the world held where the key went down; dragging toward a
  wedge lights it, letting go sends it, letting go in the middle or Escape sends nothing. The ping shows to the allies
  as FAF's does, with the intent's name under it (ours, as a marker's name is shown).
- Not for observers or in a replay (as FAF's DoPing). Teacher mode and the lobby (AI type, Recruit / Veteran /
  Commander / Supreme Commander, personality, teacher mode, cheats) come later, with the AI's options known.

docs/ai-architecture.md section 8, the interface's other open points (my proposals):
1. Personality: the slot's AI key carries type and skill only, as FAF's aitypes lists do (four entries, Recruit ..
   Supreme Commander, each with a cheating twin like M28's "cheat" keys), so the slot's one drop-down stays short.
   Personality is a per-slot option of ours beside it (default Random), kept in the slot's PlayerOptions as
   `RemasterPersonality`; PlayerOptions reach the simulation as ScenarioInfo.ArmySetup[army], so the AI reads it
   there, and Random is drawn in the simulation from the game's seed (the same for every player and in the replay).
   Teacher mode likewise a per-slot option, `RemasterTeacher`.
3. Allied announcements: FAF's own `AIChatBrainComponent` (lua/aibrains/components/ChatBrainComponent.lua):
   `brain:SendChatToAllies(text, args, location)` writes to `Sync.ChatMessages`; every player's interface shows the
   line to the AI's allies only, `args` are formatted on the receiving side (`<LOC>` texts in each player's
   language), and `location` (a Position or an Area) gives the line FAF's camera icon that moves the view there:
   "attacking here" can be clicked. Nothing new is needed in the interface; I check that our host shows such lines
   (and their camera icon) when the first announcement exists.
8. `data/ai/` as a FAF-style mod folder: no, from the host's side. As the base AI it should always be there, not
   switched on in the mods manager; the host mounts it as it mounts its own data (ui/data) and the engine's fixes,
   and its lobby entries come through a hook of aitypes. A mod folder would only matter for playing it inside FAF
   itself, which the ship model leaves out (docs/engine-distribution.md 3.1).

## engine-answers.md 524f3d0: Chris's "Move M28's work into load": the first beats inside the loading screen (2026-10-28)

Built as asked (b6fccab): a lobby's game without prebuilt units, in a window, runs beats 2..29 behind the loading
screen on the beats' thread, unpaced; their syncs reach the scripts in order when the game's screen is made; normal
pacing from beat 30 (`GameSession::Aheadable` / `RunAhead`, main.cpp's start sequence). Left out: a campaign's
operation (LaunchSinglePlayer) and replays. `MOHO64UI_AHEAD=0` off, `=1` also headless. Two things the user layer
changes, both agreed with the engine: the interface's first orders (SetCommandSource, Resume) land at beat 30 instead
of 2 (game 2's session: only the fingerprint's `sim` field differs, in beats 2..29; written as a host deviation in
ui/README.md), and FAF's StartupSequence is ours for such a game (the glide to the commander at once, ending as the
warp-in begins; tries to select it until 50 ticks after the screen). Test: `ui/tests/ahead_start.lua` (suite job
ahead_start: the screen at tick 29, the glide at once, the commander selected after its warp-in).

## Findings/LIST_2026-10-25 (Chris's snapshot test): the interface's items 14-19 and 8, and item 20's host part (2026-10-27)

Each read from the FA exe, tested, landing together on main.

- **14, enemies' blue construction boxes through the fog:** ours outlined every standing building of every army in
  build mode. The original's DrawAllUnitSkirts (0x85ad80) walks the session's command table and outlines the skirt of
  each BuildMobile order (0x8b4140 type 8), and the simulation publishes commands only for the focus army's own units
  (Unit::UpdateVisibility 0x6abc30, 0x6e8dc0). Now: the focus army's build orders, begun or not; no standing building,
  no other army's (an observer: every army's). Test army_focus.lua.
- **18, Shift shows allies' orders:** the order graph held the allies' units too (INFERRED before). Allied units reach
  the original's interface as recon blips without queues (0x5bee40, 0x5bf810): the focus army's units only now.
- **19, with cheats, clicking an army's name:** FAF's score panel does ConExecute('SetFocusArmy ' .. index - 1); ours
  had no such console command. Built as the original's (0x834e50: one word, 0-based or -1, then
  CWldSession::RequestFocusArmy 0x8965e0 with its checks, shared with the Lua SetFocusArmy). The engine's command
  stream already takes orders for another army with cheats on, so the switched-to army is commanded too.
- **15, pings flash full screen:** our controls' OnFrame ran before the script threads; FAF's ping arrow
  (offscreenmarkerindicator.lua) covers the whole world view until its first OnFrame, so it was drawn once full
  screen. The original's app frame (CScApp::Main 0x8d1470) runs the threads, then the controls' frame (0x8d1bb1),
  then the redraw: ours does now. pings.lua checks the arrow.
- **16 / 17, list hover and scroll bars:** CMauiItemList's colours and mouse-over band (0x799340, 0x799a50: a dark grey
  band ff202020, text ffa0a0a0, not blue) and CMauiScrollbar's presses and drags (0x7a11c0, 0x7a1410). lists.lua.
- **8, deposit icons:** not ours: WorldPicture::PrepareSession's warm beat took the start-up CreateSplat events
  (render REQUESTS 40 in ui/REQUESTS.md); the renderer has the fix in hand.
- **20, the host's part of the hiccups (engine-answers, 2026-10-27):** a game in a window makes its Sim with the
  machine's cores (MOHO64_THREADS, when set, decides; headless stays 1; window vs headless fingerprint identical);
  the recorded game's fingerprint runs on the beats' thread; M28AI in the mods is our variant (mods/m28-variant
  mounted, the original's folder left out; --m28 original the other way round; an active original M28 is taken as
  the variant: ui/data/hook/lua/mods.lua); a run's second game writes `<prefix>.2.session.lua`; the snapshot
  launcher sets MOHO64UI_LONG_FRAMES=33. The loading screen held until about beat 25 (M28's start freeze) is not
  done: the original ends it when the world is loaded; for Chris to decide with the optimisation run.

## render/REQUESTS.md 46: Chris's test run of 21 October, the interface's items (2026-10-21)

Built as the original does them (read from the FA exe), tested, landing with ui 72ad121.

1. **Item 2, hotkeys that put a building in hand**: they exist: FAF's hotbuild, and they work in our host as they are
   (test hotbuild.lua). Press F1 (Key Bindings), then either click the "Hotbuild" preset (W factories, S mass
   extractor, F power, and so on) or search an action under "Hotbuilding" and give it a key of your own. With an
   engineer or the commander selected, the key puts the building on the cursor; pressing it again steps to the next
   (land, air, naval factory), Alt plus the key steps back.
2. **Item 6, the commander's upgrades missing from the queue**: our host's fault. The original keeps the interface's
   own copy of each unit's queue and changes it the moment an order is given (ISSUE_Command 0x8b0180; GetCommandQueue
   reads that copy, 0x8b6f60); ours read the simulation's queue, which has the order only a beat later, so FAF's
   enhancement queue threw its entries away: no drone icons, and T3 standing in with the bar of the others. Now the
   copy is kept as the original's: the drones, T2 and T3 show at once, in order, each with its own bar (test
   enhancement_queue.lua). One part waits for the engine: the construction panel's refresh when an upgrade finishes
   (Entity:RequestRefreshUI, ui REQUESTS 47).
3. **Items 9 and 10, Ctrl+Alt+B and Alt+T**: FAF's debug keys run the original's simulation console commands, which
   the interface sends in the command stream's DebugCommand message (DoSimCommand 0x88e440); the engine built their
   simulation side (133671a). Ctrl+Alt+B (BlingBling): 10000 more mass and energy storage, filled, and the lines
   "<name>: BlingBling" and "<name> is cheating!" on screen. Alt+T (TeleportSelectedUnits 0x833f90): every selected
   unit of the player, buildings included, moved to the cursor. Alt+Delete destroys the selection. All need cheats on in
   the game options (test sim_commands.lua). As in FAF, the keys are bound in every game; without cheats the
   simulation refuses and FAF says "<name> is trying to cheat!" (Chris, 2026-10-21: "Keep FAF's way").

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
