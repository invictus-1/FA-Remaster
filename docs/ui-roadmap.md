# Interface track: roadmap

What is done, what is left, and in which order, for everything the interface track owns: the front end,
the in-game interface, input, sound, preferences and the path into a new game. Written 2026-10-12;
`ui/README.md` has the current state in detail, `ui/REQUESTS.md` what is asked of the other tracks.

The measure used below: the original gives its interface scripts 531 engine functions. When this was
written 250 were built; `ui/README.md` has the count as it is now (401 after the first sound work).

## Where we are

Done and checked:

- The game's own interface scripts run in our engine from the player's game files, mods' hooks included.
- The control toolkit: frames, groups, bitmaps, text, borders, item lists, scrollbars, edit fields,
  histograms, the map preview; layout, depth order, hiding, destruction as the original does them.
- Text is measured and drawn the original's way (on Windows). Compared with a screenshot of the original
  main menu at 3840x2160 and 150% scale, every part sits on the same pixel.
- Drawing (a software backend), a window, the mouse, draggers, modal dialogs.
- Main menu → Skirmish → the lobby screen → Launch Game → the simulation starts and runs.

Since this was written (see `ui/README.md` for each): the keyboard and the key maps; the in-game
interface on a running game, in a state of its own; the world's picture from the renderer inside the
world view; selecting, orders, building, factory queues, unit states; the camera, the right click and
the arrow keys by the original's rules; and sound from the game's own sound files. So stages 1 to 5 are
there in a first form, and stage 6 has begun with sound.

## The shortest way to a playable skirmish

Stages 1 to 5 below, and of those only the marked items (**P**), make "start a skirmish against an AI,
see it, give orders". Everything else can follow. Two things on that way belong to other tracks: the
world's picture (renderer) and the stock AI opponents (engine; the mod AI M28 already plays).

| Stage | Result | Depends on |
|---|---|---|
| 1 | The front end can be used by hand | – |
| 2 | A skirmish can be set up completely | 1 |
| 3 | The in-game interface comes up on a running game | engine (done: sync, facts) |
| 4 | The world is on screen inside the interface | renderer |
| 5 | Units can be selected and ordered | 3, 4, engine (done: orders) |
| 6 | Sound and movies | – |
| 7 | Everything around a match: score, restart, replays, saves, campaign | 3, engine |
| 8 | The modern pass | 4 |
| 9 | Multiplayer and FAF | last, by request |

## Stage 1: the front end can be used by hand

- **P** Keyboard: key presses to the control with the focus, typing in edit fields with caret and
  selection, Enter and Escape, the escape handler. (The original's handlers are located: key down, key
  up, character.)
- **P** Key maps: the game's key bindings (`IN_AddKeyMapTable` and its two companions), key code
  conversion, `IsKeyDown`.
- **P** Drop-down lists (combo boxes) and list clicks: built from item lists and need row clicks,
  mouse-over rows and the mouse wheel.
- The options dialog for real: `GetOptions`, the video choices, applying and saving options.
- The profile dialog: creating, choosing and deleting profiles (needs typing).
- Exit (`ExitApplication`), opening links (`OpenURL`).
- Tooltips that go away, bitmap animations (the moving glows of the menu), hit testing by a bitmap's
  alpha, render passes, the game's cursor pictures, textures in formats other than .dds.
- Preferences: where our file lives by default, and taking over the player's existing profile once.

## Stage 2: a skirmish can be set up completely

- **P** Open slots: adding and removing AI opponents, their faction, colour and team.
- **P** The mods manager, so that AI mods and other mods can be switched on for the game.
- The map selection dialog (list, filters, preview, map options), game options, presets. It opens and
  looks like the original; choosing in it needs the list clicks and drop-down lists of stage 1.
- The lobby's chat line and its smaller dialogs (rules, CPU benchmark, "Load").
- The changelog dialog only once per version (it follows the preferences).

## Stage 3: the in-game interface comes up on a running game

The original runs a session's interface in its own script state, separate from the front end's.

- **P** How the original boots that state (session init script, `StartGameUI`, the world interface
  provider) and the same order here; the loading screen.
- **P** Each beat's sync table into that state (`Sync`, `PreviousSync`, `OnSync`).
- **P** The session's facts as functions: scenario info, armies table, focus army, economy totals, game
  time and tick, command sources, clients, pause state (about 25 functions).
- **P** Blueprints and unit categories in the interface's state (`GetBlueprint`, `ParseEntityCategory`
  and the category functions): the interface reads the same rules as the simulation.
- Result: economy bar, score, clock, objectives and the frame of the minimap show a live game.

## Stage 4: the world is on screen

- **P** With the renderer track: one window and one device; the interface draws on the renderer's device
  instead of the software backend (same small drawing interface, a second backend).
- **P** The world view control (17 functions): the rectangle the world is drawn in, projecting between
  screen and world, the mouse's world position.
- **P** The camera (25 functions): zoom, pan, rotate, track, save and restore, as scripts and keys drive it.
- The minimap as a second world view; world meshes and decals the interface places (build previews,
  range rings); the economy, intel and military overlays; strategic icons.

## Stage 5: selecting and ordering

- **P** Units as the interface sees them (`UserUnit`, 36 functions) from the simulation's unit list.
- **P** Selection: click, box, double click, by category; `GetSelectedUnits`, `SelectUnits`, idle
  engineers and factories, rollover info.
- **P** Orders through the engine's command writer: move, attack, patrol, build, assist, reclaim and the
  rest (`IssueCommand`, `IssueUnitCommand`, `IssueBlueprintCommand`), command mode, the orders panel.
- **P** The construction panel and factory queues (queue display, build counts, templates).
- Sim callbacks (`SimCallback`), script bits and toggles, fire states, pausing units.
- Command feedback (the blips where an order lands), pings and markers, chat to allies.

## Stage 6: sound and movies

- Sound: the game's sound banks, playing, stopping and pausing cues, voices, volumes, music, world
  sounds at their place (about 20 functions). Needs a choice of audio backend.
- Movies: the game's .sfd files (MPEG video with its sound track) for briefings and the menu background.
- **Done:** the new front-end background asked for: the game's own world behind the main menu, a
  map in the evening light with the camera gliding over it (see `ui/README.md`).

## Stage 7: everything around a match

- Pause, game speed, ending a game; the game-over state and the score screen with its graphs.
- Leaving a game: back to the menu, restart (`ExitGame`, `RestartSession`).
- Replays: writing one while playing (the command writer already gives the bytes), the replay list,
  playing one with the interface (`LaunchReplaySession`).
- Saved games (`InternalSaveGame`, `LoadSavedGame`): needs the engine to save and load a simulation.
- The campaign: operation select, briefings, `LaunchSinglePlayerSession`, objectives, cinematics.
- The console and its commands (`ConExecute`), as far as players and mods use them.

## Stage 8: the modern pass

The wish: the original's artwork and sounds, but snappy and modern.

- Sharp at every resolution and scale; resizing the window; several screens.
- Timing: no stutter when screens open, input answered in the same frame, loading off the main thread.
- The front-end background.
- Text measured and drawn without Windows' font functions, for other systems (the game's font files
  read directly), with the same results.

## Stage 9: multiplayer and FAF

Lowest priority by request: the network lobby (joining, peers, discovery of LAN games), the FAF client's
connection (GPGNet), observers, disconnect and desync dialogs.

## Running through all stages

- **Proof against the original.** Wherever possible a screen is compared with a screenshot of the
  original at the same settings. Done so far: the main menu, the options dialog, the lobby and the
  lobby's Game Options dialog, all on the same pixels. Wanted next: an in-game screen.
- **Inferred behaviour to confirm**, marked in the source: the order of mouse exit and enter; dragger cancel rules; mouse button key codes;
  re-parenting; keyboard focus callbacks.
- **Tests.** Each built piece gets checks in `ui/tests` (111 today), run on every change.
- **Housekeeping with the engine's answers:** use the shared mounting function and the engine-object
  handle that moved into `src/script`.

## From the other tracks

- Engine: classic AI opponents that build (the mod AI M28 plays today); saving and loading a simulation;
  the few orders that still end at once; later, simulation speed control and pause rules for the clock.
- Renderer: the shared window and device, the world's picture inside a rectangle with a camera given by
  the interface, picking (what is under the mouse in the world), and drawing the interface's quads.

## Decisions

Taken 2026-10-12:

1. Preferences: the player's existing game profile (name, options, key bindings) is taken over once, on
   first start. The original's file is only read, never written.
2. The front-end background: the new background is made first. Decoding the original movie can follow
   later.
3. After the playable skirmish, sound comes before replays and the score screen.
