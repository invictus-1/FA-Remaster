# The interface track's answers (newest first)

Answers to the other tracks' requests to the interface (CLAUDE.md: "the layer asked answers in a file of its own").
The requests themselves are in the asking track's file; each answer names it.

## renderer, 2026-11-21: [surshadow] (render ffc7877c), "if you want it as a player option"

The row "Shadows of Trees Outside the Map" is in Options > Remastered > Sky & Light, after Soft Shadows:
`remaster_outside_shadows`, On by default (the trees outside the map cast real shadows, your `World::surroundTreeShadows`),
Off the picture as before; set in `WorldLooks`, in a running game too. In no preset; one switch, no levels (your
0.12 ms). The detail and the tests: docs/ui-options-layout.md, "Shadows of Trees Outside the Map". Nothing asked of you.

## Chris, 2026-11-27: the ideas' numbers 3 and 7 ("1-4 yes that works for me ... 6-9 all of those work")

**3. "Report a Problem" holds a picture of that moment.** In a window the question's script asks the host for the
window's next shown frame BEFORE its box comes up (`moho64ui_ReportPicture("take" | "state" | "drop")`,
`UserState::Config::captureWindow`, `GameSession::CaptureWindowInto`: the renderer's read-back of the shown frame, the
one `--window-capture` uses); the box follows one or two frames later, so it is not in the picture. The report
written holds it as `screenshot.jpg` and its README and the box that names the file say so; a question called off
forgets its picture; the console's `remaster_report` and the automatic reports take none.
- A JPEG of quality 90 without colour subsampling, written in `ui/user/report.cpp` (`JpegOf`: the tree holds no JPEG
  writer and none was brought in). Measured on a game's picture at 1920x1080: 1.0 MB in 67 ms, against 4.2 MB packed
  without loss (PNG, 160 ms), which would be about 16 MB at 3840x2160. Whole at the window's size while that is at
  most 6 MB, else halved (`MOHO64UI_REPORT_PICTURE_MAX` for tests). The game's own picture only: nothing else of the
  screen can be in it.
- Tests: `ui/tests/report_picture.lua` in a live window (the lab's `suite/window_checks.sh`, check `wc_report`: a game
  and the main menu, 19 checks each, then the zips read with an independent decoder, 22 checks); a run without a
  window takes no picture and is `report_ask`'s as before.

**7. The loading screen names the step of the load.** Under the game's tip: "Loading (2 of 5): setting up the
battlefield". The steps are the loading thread's own (`GameSession::LoadStep`, `moho64ui_LoadStep()`): 1 the rules
read, 2 the simulation started, 3 the interface's state made, 4 the game's first beats, 5 the map's picture made
ready. Each step goes into the log with the load's seconds ("moho64ui: the loading screen says: Loading (3 of 5):
preparing the interface (2.1 s into the load)"), so a report shows which step a long load stood in. The line for
graphics being prepared (ui REQUESTS 58) is one row lower now. Hook `lua/ui/game/gamemain.lua`; tests
`ui/tests/load_line.lua` (18 checks) and `wc_report`'s read of the log.

**1. The first start recommends, and changes nothing by itself** ("make sure it starts on ultra for everyone but
recommends lower settings if their pc is too weak"). Every look still starts at its highest on every PC. Once per
profile, on the main menu, `data/lua/firstadvice.lua` asks the renderer's `SuggestedPreset` (by the card's memory and
ray tracing: `moho64ui_SuggestedPreset`); where that is LOWER than what is set, a box names the card and what is
recommended (only what would change) with two buttons: "Use recommended" puts the Remastered Preset, Texture Detail
and Shadow Fidelity in as the Options' OK would, "Keep my settings" (or Escape, or a click beside it) changes nothing.
Nothing is ever raised; a Custom preset is left alone; the profile's `remaster_first_start_advice = 1` then keeps the
box from coming again. A menu without its live background has no device to ask: then the measured check in the
first game (the hook of gamemain.lua, 2026-10-18: by speed, a question, Ray Tracing first) is the only one, as before;
both stay. The Options' graphics-memory strip keeps showing the recommendation with its own button. Test
`ui/tests/firstadvice.lua`.

**4. The main menu names the build and what is new.** Bottom right, level with "Report a Problem": "FA Remastered,
build of 27 November 2026  -  What's new", with "(new)" and in white while the list holds an entry this profile has
not opened. The date is the commit's the program was built from (`ui/build_ui.py` stamps `kBuildDate` beside
`kBuildCommit`; `moho64ui_Build()`). A click opens a box with the three newest entries of `data/lua/whatsnew.lua`: a
list kept by hand, an entry a day that changed what a player sees or feels, in a player's words. THE LIST IS THE
INTERFACE TRACK'S TO KEEP at each landing that changes something a player notices (the other tracks' changes too, from
their messages). Test `ui/tests/whatsnew.lua`.

**2. A stutter's details are in the log by default.** A live window has logged its long frames since October, but
by default only those over 100 ms and without the names of what ran long (that needed `MOHO64UI_LONG_FRAMES`, which
Chris's launchers set to 33; a player who starts the program without a launcher had the bare lines). Now a live window
with the variable NOT set logs every frame over 33 ms with its parts, the card's time, the engine functions the
scripts called that ran long and the control whose Draw took longest (`LongFrameDetail`, `SetLiveWindowRun`), still at
most 30 lines a minute with the rest counted in one line. So a report from any player holds what a stutter was made
of. The variable still sets another threshold (and lifts the limit), `=0` or `=off` turns it off.
- MEASURED FIRST (the timing wraps every engine function the scripts call, so it was held back until now): the
  suite's job `scale` (5,000 units; build cd1a7e69; CPUs 0-15; two runs each way). A paused frame with nothing
  selected: 0.57 and 0.67 ms without, 0.81 and 0.63 ms with (`MOHO64UI_BINDING_PROFILE=1`), 0.66 ms with the whole
  detail (`MOHO64UI_LONG_FRAMES=50`); all selected, over idle: 0.59 / 0.61 against 0.49 / 0.80; the interface's share
  of a beat 3.6 / 3.9 against 3.6 / 3.5 ms. No difference outside the runs' own spread. The one cost seen: selecting
  all 5,000 units at once, 42.3 / 42.5 ms without, 49.2 / 52.0 / 49.2 ms with: about 70 ns for each engine function
  called, in the one moment that calls a hundred thousand of them. (A fourth run with the detail fell into the start
  of the engine's gate on the other cores and is left out: its draw time doubled with nothing selected.)
- Seen in a live window without the variable (`wc_report` on the side build): "frames longer than 33.0 ms are logged
  with what ran long in them (a live window's default ...)", and a 52 ms frame of a game's third second with its
  names (a model made ahead 4.3 ms, CMauiText:SetText 1.9 ms, ...).

**9. The settings to and from a file.** Options > Interface, at the tab's end under our header: "Save Settings: To
Desktop..." and "Load Settings: From Desktop..." (`data/lua/settingsfile.lua`; the rows in
`data/hook/lua/options/options.lua`, buttons as "Import from FAF..."). Save writes the options AS THE DIALOG SHOWS
THEM NOW as plain text onto the Desktop (`FA_Remastered_settings_<date>_<time>.txt`: a `name = value` line an option,
150 of them today, under the tab's name as a note, CR LF so that Notepad reads it). Load lists the settings files on
the Desktop, newest first; the one clicked is read and its values go INTO THE DIALOG (its working set and the rows
shown: `RemasterWorkingSet` in the dialog's hook), so nothing is kept until the dialog's own Apply or OK, and its
Cancel leaves everything.
- A file is data, never run: each line is read by a pattern; a key that is no option of this build is left out, a
  value its option cannot take is left out (not one of its states, outside a slider's range), and the box counts
  both. Not written and not read: the screen's resolution (`primary_adapter`, `secondary_adapter`: a PC's, and a
  wrong one could leave a black screen), this program's notes of what a profile was shown, and the key bindings
  (they are not options; FAF's import stays the way to carry those).
- The host keeps the place (`moho64ui_SettingsFile` in `ui/user/report.cpp`: "write", "list", "read"): only files
  named `FA_Remastered_settings*.txt` in the Desktop folder (in tests `MOHO64UI_REPORT_FOLDER`), 256 KB at most; a
  script cannot name another path.
- Tests: `ui/tests/settingsfile.lua` with the lab's `suite/settingsfile.sh` (a Desktop of the test's and a planted
  file: 31 checks, and 7 on the file written).
- Also: `MOHO64UI_BUILD_DATE` (a test switch) says another build date to `moho64ui_Build()`, so that the stored
  pictures of the main menu, whose line names the date, hold from day to day (`suite/pictures.sh` sets it).

**8. The mods manager says whose version of a shared unit is used.** Where two mods define the same unit, Chris's
fix mod keeps the stronger or newer one (his rule of 2026-11-06); the engine put the picks into the fix mod as data
(`duplicates.lua`, his answer to ui request 63). `data/lua/modduplicates.lua` reads every such file among the mods
(in an empty environment, as data), and the hook `data/hook/lua/ui/lobby/ModsManager.lua` adds a note under the
description of each of the two mods when the manager makes its row: "FA Remastered: 18 units of this mod are also in
<the other>. With both mods on, one version of each is used, the stronger or newer one: - 8 from this mod: ... - 10
from <the other>: ...", a renamed unit and a unit that is always left out said as such. In full in the row's
tooltip; in the row's own text as far as the manager shows a description. The mod's own table is as it was once the
row is made. Without a fix mod that carries the file nothing is said. Nothing of the mods or the fix is in the
repository. Test `ui/tests/modduplicates.lua` with the lab's `suite/modduplicates.sh` (a PLANTED fix mod with made-up
picks, a broken file, a file without a list: 11 checks).

**5, its first part: THE INSTALLED COPY** (Chris, 2026-11-27: "Go ahead with the installer, small installer plus art
packs"). A program started with NO `--init` that finds `install.cfg` beside itself is a player's start
(`ui/app/main.cpp`, `installed`):
- the init file is the PLAYER'S OWN `<faf>/bin/init_faf.lua` (`faf` from install.cfg, else C:/ProgramData/FAForever),
  as it is: it mounts FAF's game data, the game's own archives (FAF's `fa_path.lua` names the game's folder) and the
  player's maps and mods from where they lie. Nothing of them is copied into our folder or shipped. (The lab's own
  init file is that same file with five lab changes: read and compared 2026-11-27.)
- FAF's init file empties the ORIGINAL GAME'S shader cache under LOCAL_APPDATA at every start (its rule for its own
  program). For the init file alone LOCAL_APPDATA is answered with a folder of ours
  (`<LOCAL_APPDATA>/FA Remastered/faf-init`), so a start of ours deletes nothing of the original's: checked from
  outside, the cache's file list is the same after a start as before.
- a window as `--window` (not with `--frames`: a test's run), no console echo; our art from beside the program where
  the art packs will put it (`maps/`, `units/`, `units-faf/`, `units-mods/`, each only when there); the settings,
  logs and replays where a live window keeps them anyway (`<LOCAL_APPDATA>/FA Remastered/`).
- FAF missing: a message box says what to do (install FAF, start it once), exit code 3 (`MOHO64UI_NO_BOX=1`: the log
  only, for tests).
- FAF IS NEEDED: the scripts our hooks sit on are FAF's. Plain Forged Alliance without FAF is not supported.
- Tests: `ui/tests/installed.lua` with the lab's `suite/installed.sh` (a staged folder outside the worktree, the PC's
  own FAF: 7 checks inside, 7 from outside: the log, the folder of ours, the cache untouched, a wrong FAF folder, no
  install.cfg). Seen in a live window: the main menu with the flight over stock Seton's (no art pack in the folder).
- NOT BUILT YET: the installer program itself, the packaging step, the art packs' format, a program without a
  console window (the link's subsystem), shortcuts, the uninstall.

## Chris, 2026-11-20: the debugger's next steps ("I'll let you decide what is the best as in most convenient and gets us the best info")

Two things, both built on the report of 15 to 18 November (`ui/user/report.cpp`: the zip with the logs, the game's
record, the preferences, a crash's dump masked).

**1. "Report a Problem", a report by hand** (for what is no crash and no freeze: something looked or played wrong).
- Three ways in: the key Ctrl-F1 (key action `remaster_report_ask`, free in FAF's three presets; it can be bound
  elsewhere), the row "Report a Problem" in the game's menu (before the rows that leave the game, in all four kinds of
  the menu: a game one plays, a replay, a network game, FAF's), and a line bottom left on the main menu that names the
  key.
- It asks one line, "what happened?" (300 characters, may stay empty; Escape, Cancel or a click beside it writes
  nothing), then writes the report's zip ONTO THE DESKTOP (the reports folder where the system names no Desktop) and a
  box says where. Nothing is sent anywhere.
- The note in the zip's README is the line typed and, in a game, `[asked at game time m:ss]`: the moment the question
  came up, not the moment of Enter, so that whoever replays the game's record knows where to look.
- Made of FAF's own controls (its input dialog and QuickDialog); `ui/data/lua/report.lua` (Ask, Write, KeyText),
  hooks `lua/ui/game/tabs.lua` (new) and `lua/ui/menus/main.lua`, `moho64ui_Report(note, onDesktop)`.
- Two things read on the way: FAF's popup takes its escape handler off the stack twice when Escape closes it (once is
  taken here), and Escape typed into a line goes to the line, not to the escape handlers (it cancels here).
- Tests: `ui/tests/report_ask.lua` (a game: the row in the menu as built, the click, the line typed, the zip, the time
  of the asking, the key, Escape, the game's own Escape after it, an empty line; the front end: the line, a click, a
  report with no game) and the lab's `suite/report_ask.sh`, which reads the zips (33 + 13 + 17 checks).

**2. A dump of the program when it FREEZES.** The freeze watch (18 November) said in the log THAT the main loop stood
and in which part; now it also says WHERE.
- When the main loop has stood for ten seconds (twice the watch's time), the log keeper's thread starts this same
  program once more in a small mode, `moho64ui --dump-of <process> <main thread>`, which writes from OUTSIDE a small
  memory dump of the standing run (the engine's crash dump's kind, 0x1000: every thread's stack and registers, no heap)
  beside the log (`<log>.freeze.dmp`), and every thread's frames as places in the image. The keeper writes those
  frames into the log: `moho64ui: freeze: thread 1234 (the main loop) frame 3 +0x72df4e` (what
  `llvm-symbolizer --obj=moho64ui.exe --relative-address` takes, as a crash's frames).
- From outside, because a program that stands may stand with a lock held that a dump written by itself would wait for.
  No second program runs while the game runs: it is started at the freeze and ends when the dump is written (0.5 to
  0.6 s measured, 60 to 65 KB for a main menu). Once a time without frames, at most three a run. The small mode dumps
  only a run of this same program file.
- The next start's automatic report takes the dump in as `dump_freeze_before.dmp`, a report made later in the same
  run (the freeze passed) as `dump_freeze_now.dmp`, both masked as a crash's dump.
- `MOHO64UI_FREEZE_DUMP=0` is the way before (the watch's lines only). Windows only; elsewhere the lines stay.
- `ui/user/freeze_dump.h/.cpp`, the keeper's part in `ui/user/events.cpp`, the mode's entry at the top of `main`.
  Nothing of the engine's `core/crash.cpp` is changed or copied.
- Tests: the lab's `suite/freeze_dump.sh` (a menu that sleeps for good and is ended by force: the dump, its text, the
  frames in the log, and the main loop's own frame looked up with the build's .pdb is `moho::ui::NoteFrame`, the
  test's sleep; the start after it: the zip holds the dump, the same size, the user's folder nowhere; a sleep of 13 s
  that passes and a report by hand; the switch off): 27 + 6 + 1 checks.

**2b. The beats' part of the watch (asked by the engine the same day: "a beat that does not come back is the freeze I
most want a dump of", an endless loop in a task or a script).** In a window the beats run on a thread of their own,
so the frames go on while a beat hangs and the frames' watch sees nothing.
- Whoever runs a beat says so for the length of the call (`ui::BeatScope` round every `Tick` in `game_session.cpp`
  and `menu_flight.cpp`: three atomic stores a beat). When one beat has run for ten seconds the keeper writes
  `warning: moho64ui: freeze: the simulation's beat 1234 has not finished for 10 s (frames go on)` and has the same
  dump written, the beat's thread named and first in the frames (`thread N (the simulation's beat) frame 0 ...`, 40 of
  its frames); when the beat comes back after all: `the simulation's beat 1234 finished after all, 12.3 s after it
  began`. Where both watches see the same standing (the beats on the main loop's thread, or the main loop waiting for
  the beat) ONE dump is written. A report's README shows these lines at a glance.
- The line names the stage of the beat that began last (the engine's `moho::g_beatStage`, `core/trace.h`, his batch
  187): `... has not finished for 10 s (frames go on; the stage that began last: weapons)`; while a game loads, his
  "load: ..." names.
- Tests (`suite/freeze_dump.sh`, cases "beat" and "ahead", `MOHO64UI_BEAT_FREEZE_TEST=<beat>:<seconds>`): a game
  whose beat 30 sleeps 14 s, and the same with the first beats on the loading thread (the beat's thread is another
  one, named and first; the main loop's after it); the beat's own frame looked up is `ui::BeatScope`, the test's
  sleep; 46 checks with the freeze's. NOT PROVEN: the wording "frames go on", which needs a window (without one the
  main loop waits for the loading thread, so both cases read "no frames either").
- First real finding of the dump, in the suite's own runs: the device-lost jobs stand over 20 s on the main loop with
  the frames in `dxcompiler.dll` (the shaders compiled again after the device is lost, on the main thread).

**2c. A first start at work is not a freeze (ui/REQUESTS.md 58; the renderer's [firststart], render 250bd4fd).** The
dump's first finding was the first start with no compiled shaders stored: a minute and more without a frame while
DXC compiles inside the first frames (the renderer measured 192 s to a moving picture against 23 s with a filled
cache, and 18.5 s with the compiled shaders shipped).
- `--shipped-shaders <folder>`, else the folder `shaders_shipped` beside the program when there is one, is handed to
  `WorldPicture::SetShippedShaderCache` once at the start (read where the player's own cache has none, never
  written); the log says which, or "none". The folder's content is the renderer's (his "shader tour"); an installer
  lays it down.
- The freeze watch asks `WorldPicture::ShaderWork` (through `ui::SetShaderWorkProbe`, so that the program without the
  world's picture needs nothing of it): while shaders are being compiled its lines end "; shaders are being compiled:
  14 since the frame began, 1 at work" and NO dump is written; a dump comes at a later look that finds nothing compiled
  since the look before. A run ended by the player inside such a time is reported at the next start as "The game was
  closed while it was compiling shaders the last time it ran (a first start after an install or a new build can stand
  for minutes, once)", not as a freeze; the report is written all the same.
- The main menu shows "Preparing graphics (once after an install or a new build): N shaders so far" above "Report a
  Problem" while the count moves (`moho64ui_ShaderWork()`), and takes it away four seconds after the last one.
  NOT BUILT: the same line on a game's loading screen (the picture stands inside the long frames, so a line there
  would only show between them; the renderer's next step moves that work behind the loading screen).
- Tests: `suite/freeze_dump.sh`, case "shaders" (`MOHO64UI_SHADER_WORK_TEST=1`: the watch's lines, no dump, the next
  start's sentence, the menu's line): 48 file checks + 8. With real shaders it is the renderer's proof and Chris's
  next start after a new build.

**Found on the way, fixed 38b07a69:** the report's masking read a dump's stacks only; a copy of the environment block
made with the wide registers leaves the block's last pieces in a thread's SAVED REGISTERS, which a dump holds too
(the report's own test caught "WINDIR=C:" there at a landing). The masking reads the registers now.

## Chris, 2026-11-20: "Yes add the option for contrails" (the renderer's [contrailvapour], render 74a8c0c7)

The row "Vapour Contrails" is in Options > Remastered > Effects & Battle, after Shell Tracers: `remaster_contrail_vapour`,
On by default (the look since that render: a contrail as pale vapour lit by the sun or the moon), Off the earlier
bright trail; it sets `World::ribbons.contrailVapour` in `WorldLooks`, in a running game too. In no preset. The
detail and the tests: docs/ui-options-layout.md, "Vapour Contrails". Nothing is asked of the renderer.

## renderer, request 79 (render/REQUESTS.md): the renderer's lines into the game's log; the interface's part (1) (2026-11-14)

- (1) BUILT: `main.cpp` installs `WorldPicture::SetLogSink` right after the log is opened, before the shader compiler's
  check and before any picture: each line goes to `Logf` as `render: <line>` at the level given (0 debug, 1 info, 2
  warning, 3 error); `Logf` takes the log's lock, so the sink is safe from your threads. `render::LogFlush()` at the
  program's end. `log_print.h` is included with `RENDER_LOG_PRINT_NO_MACROS` (no header of yours that the interface
  includes pulls in the macros: checked). Before a crash report: the engine's `CrashSetBeforeReport` (his batch
  168) gets `render::LogFlush` when it is in main.
- Check: the lab's `suite/render_log.sh` (suite job `render_log`): a short game; the log has "info: render: card:",
  at least three renderer lines, the program's stdout and stderr hold no raw renderer print. On the build without
  the sink it is red (no card line, 34 raw prints on stdout); with it green. A game on stock Seton's (60 + 240
  frames) wrote 34 renderer lines, all at info. The full suite's strict reading names any warning left.
- (2) and (3) change what a player gets by default (the long-frame lines on by default; SuggestedPreset as the first
  start's starting point): PARKED for the Fable lead's check after the reset unless Chris says yes in his own words.
  (4) the suite job with `SetCardImitation`: not built yet.
- Seen in the card's report: the adapter line reads "render: card: card: NVIDIA ..." (your report's own "card:" after
  the print's "card:"): cosmetic, yours if you want it.

## renderer, request 78 (render/REQUESTS.md): Head Lamp Colour as a player option; and his answer to ui 54 used (2026-11-14)

- The row "Head Lamp Colour" is in Options > Remastered > Units & Readability, after Unit Glow Colour:
  `remaster_head_lamp_colour`, White / Faction colour / Team colour = `World::headLampColour` 0 / 1 / 2, Team by
  default (Chris: "follow the color of your team color is best"); console `remaster_head_lamp_colour
  white|faction|team`. In no preset. docs/ui-options-layout.md 7.
- The row "Allies' Pause Marks" (`remaster_allies_pause_marks`, on by default = the original's way) sets your
  `WorldPicture::SetAlliesPauseMarks`, at a picture's start and on every change.

## Chris's test of 2026-11-13 (the engine's split, `_scratch\engine\TEST_RESULTS_2026-11-13.md`): the interface's part (2026-11-14)

His build was cb9caa01; his words are in the verdicts file, the causes below were read from his logs
(`_scratch\bigtest\game_prev.log`, `game.log`) and from the code.

| His finding | Cause | What was done |
|---|---|---|
| FAIL "The menu has a background at once": "its frozen ... only runs our new make briefly before switching to old background" | The switch: the flight's cost check (`FlightCost`) had a limit of 12 ms of the card a frame, a guess; his card takes 17.6 ms at 4K (16.2 in a window), and since the fix of 2026-11-11 the check always comes. The "frozen": the kept picture shown until the flight is live (33.7 s at the first start after a new build, 6.6 s at the second) | The limit is 33.3 ms (under 30 frames a second): `ui/app/flight_cost.h`, `docs/ui-menuflight.md` section 12. The first start's wait and a 4.3 s frame as the flight comes in: reported to the renderer with the numbers (ui/REQUESTS.md 55) |
| FAIL the photo key: "Game freezes for a second" | The renderer shows the world as it was while a photo is made; `remaster_photo` asked for twice the view's size and left the supersampling to the renderer: ten frames of 75 to 146 ms, 0.93 s (his log, frames 546548-546557). The change of 2026-11-13 had only moved the second from one frame to ten | The key's photo is the view's own size, drawn once: 9 frames in 0.15 to 0.45 s at 4K against 0.42 to 1.33 s, a third in each series (measured, docs/ui-benchmarks.md "The photo key"). Asking only for no supersampling changed nothing (at 7680x4320 the renderer draws once anyway): found by the measure. `remaster_photo fine` gives the photo of twice the size as before; each photo's cost is in the log. Since the renderer's `settleFrames` (his answer to ui 56) the key's photo takes 3 frames, 0.12 to 0.17 s in the same measure |
| "it says cancelled brush stroke" (a camera drag with a second button) | FAF's own text: its map painting (`lua/ui/game/painting/PaintingBrush.lua` OnCancel, called when a dragger is cancelled); his right-button drag was a brush stroke | nothing |
| "now there is a perma attack marker" (after the rally point row) | His log: a right press with Shift and Alt on a selected factory (`RULEUCC_Move +Shift +Alt`): an attack-move point in the factory's rally route, which stays as rally points do. The order's feedback mark went (the log shows it) | nothing |
| "yellow icon that shows where the units are docking doesn't appear" (air staging) | Ours, probed 2026-11-14: the Dock order (the order panel's button, `IssueDockCommand`) draws FAF's yellow line (ddffff00) and `repair_btn_up` mark to the platform while Shift is held; a right click on the platform is `TransportLoadUnits` (the original's 0x820e01): line dd654bc2, mark `load_btn_up`. His click was a right click, on an ally's platform | He answered (2026-11-14): the icon that flashes briefly at every platform the aircraft are sent to when the Dock button is pressed, without Shift. That is the script's order feedback (FAF's commandmode.OnCommandIssued: its default blips for a Dock, which has no mesh of its own): `IssueDockCommand` did not tell the script of its orders. It does now (`ui/user/user_units.cpp`; `ui/tests/dock.lua` checks the flash at the platform) |
| picture "I don't think I should be able to see what allies pause" | The original's: its icon pass marks a paused unit when the focus army IsAlly of the unit's army (0x85bd99, re-read). It draws the mark centred on the unit's icon place (RenderUnitIcon 0x85d9a0: one centre for the icon's quad and the pause picture's); ours floated above the unit | Chris: "make it an option and have it stick closer to the unit": ui/REQUESTS.md 54 to the renderer (the place; a switch for allies' units); the option's row is the interface's |
| his notes were cut at about 200 characters | the note line's limit (`retest.lua`) | 2000; `ui/tests/retest.lua` types a note of 599 characters |
| spawned units of his defeated army took no orders ("that should be allowed") | the original's simulation refuses every order for an army out of the game (Sim::OkayToMessWith 0x747320) | the next entry |

Also found in his logs: the re-test list's Where cells put some three hundred warning lines at the head of every log
(each word that is no screen's name); now one line (`ui/user/retest.cpp`).

## engine, by message 2026-11-13 20:3x and 2026-11-14 05:48: the game option RemasterOrdersAfterDefeat (his batch 164) (2026-11-14)

The engine: `Options.RemasterOrdersAfterDefeat = "on"` in the launch's scenario info (as RemasterSimOrder travels)
lets the simulation take orders for the units of an army that is out of the game, while cheats are on; anything else,
or missing, is the original's refusal. `MOHO64_ORDERS_AFTER_DEFEAT=1` forces it for a test (0 forces it off).

The interface's half: `StampOrdersAfterDefeat` (`ui/app/game_session.cpp`) writes the option into the options of
every launch (the lobby's, and a single player's launch info) that says `CheatsEnabled = "true"` and has not set it
already. No row in the lobby: cheats on is the player's choice of a sandbox. The option is in the record, so a replay
plays as the game went. No notice "this army is defeated" was added: without cheats a player cannot take a defeated
army again, with cheats its units obey (agreed with the engine).

Test: `ui/tests/orders_defeated.lua` (the suite's job `orders_defeated`): the skirmish lobby with cheats on and two
AIs, each for itself; in the game the option is in the scenario's options; the player's commander is destroyed, the
army is out of the game; three tanks made for it by the cheat callback; the army taken again by the console's
SetFocusArmy; a right click sends the tanks off and they go. `ui/tests/simorder.lua`'s lobby game checks that the
option is there exactly where cheats are on.

## engine, by message 2026-11-11 17:1x: the suite with MOHO64_FREE_DEAD=3 and MOHO64_DEAD_TRAP=1 (batch 149's levels 2 and 3): NO FAULT

The whole suite on main 219749db (the engine's 82ca72c5 and the renderer's a059f6de in it), run 18:40 to 19:56 on
2026-11-11 with `MOHO64_FREE_DEAD=3` and `MOHO64_DEAD_TRAP=1` in the environment, three programs at a time, a side
build of that commit (`%TEMP%\ui-b1110\build3`), output `%TEMP%\ui-land-out\free3` (the log `free3.log` beside it):

- **131 jobs, 189 test runs, 0 not green.** No run ended without its result line (a read of a deleted object would
  have been an access violation: a run with no result). The stricter second reading (`strict.sh`): 176 logs, no
  FAIL line, no failed result, no script error beyond the allowed ones; its one red was a floor missing for a job
  added the same day (bindings_count), no fault of the run.
- **The switches reached the runs:** 173 of the 176 logs carry the program's line "switches set in the environment:
  ... MOHO64_DEAD_TRAP=1 ... MOHO64_FREE_DEAD=3" (the three without are options_wrong's refusals, which end before
  anything is made); the 120 logs of runs with a simulation carry yours, "dead shells and ended tasks, props, motors
  and every other entity that is no unit are deleted during the game (level 3 ...); MOHO64_DEAD_TRAP: a deleted
  object's pages are taken away".
- **What it covers on the interface's side:** every game test of the suite (the game test at 1080p and 4K, orders
  of every kind incl. reclaim of a wreck and a tree, transports, the three device-loss game cases, replays played
  and jumped in, a defeat, a draw, a victory, the menu's flight and its hand-over to a game, the campaign's first
  operation), with the interface reading props, units, shields and blips every frame from its copies.
- **Not in it:** the network runs (out of a plain full run since the same day: SUITE_NET=1), live windows.

## renderer, request 75 (render/REQUESTS.md): Cloud Layout's third value, Varied (2026-11-11)

Built. Options > Remastered > Sky & Light > Cloud Layout: Even (0), Mixed (1, the default, untouched), **Varied (2)**:
`World::clouds.mix = 2`. The program's switch is `LookSettings::cloudLayout` (0 / 1 / 2; was the on/off `cloudMix`),
set in `ApplyLooks` (`ui/app/game_session.cpp`); the console takes `remaster_cloud_layout 0|1|2` or
`even|mixed|varied` (and `on` as before: Mixed). No preset level sets Varied: with it chosen the Remastered Preset
reads Custom. `moho64ui_Looks().cloudLayout` for the tests (`cloudMix` stays: any but Even);
`ui/tests/lobby.lua` checks the three choices, the default and that each reaches the program.

## renderer, request 71 B (render/REQUESTS.md): a lost graphics device is recovered by the program (2026-11-11)

Built: `GameSession::ServeDevice` (`ui/app/game_session.h` has the whole account, the code is after `PresentToWindow`
in `game_session.cpp`), `Backdrop::LetGoOfLostDevice` / `RecoverDevice` for the menu background's own picture.

1. **Asked once a frame, between two frames, on the thread that draws.** In `PresentToWindow` after the frame is
   shown (all three `DrawToWindow`), and in the host's loop for the pictures read back (no window, `--gdi`, a window
   whose picture would not open). While nothing is lost: one `DeviceLost()` call and one flag read. The reason
   (`DeviceLostReason`) is logged once.
2. **Every picture lets go first, then each is made anew.** The program's pictures: the window's (the frames' thread:
   menus, the flight, every game, the second window and the extra views are all this one picture), a game's own (the
   frames' thread; only where the window's picture is not), the menu background's own (its own thread; the read-back
   way only). As the program is today at most one of them lives at a time, but all three are handled together:
   `LetGoOfLostDevice` on each on its own thread (the background's is asked from the frames' thread and waited for),
   then `RecoverDevice` on each.
3. **Afterwards, as after `SwapMap`:** on a game's world the sky and the map's edge (`SkyOnWorld`) and the player's
   looks with the Photographic Look's grade (given otherwise only when it changes); on a menu's world the evening
   (`Backdrop::Dress`) and under the flight the edge and its thinner air; the interface's layer and the second
   window's layer whole with the next frame; the card's picture over the window (`SetBackdrop`) given again; a photo
   that was being made is told to the photo mode as one that could not be made (your `PhotoProgress` goes back to 0
   with nothing handed over, on which it would have waited for ever). NOT again: `GameStarted`, `Beat`, the picture's
   own settings, selection, ranges, order lines. A map opening behind: left to you (point 5); begun here only if
   `LastRecoverNumbers().loadBegunAgain` says you did not.
4. **Meanwhile and on failure.** The window's title reads "FA Remastered: the graphics device was lost, restoring the
   picture" for the time of the calls (a full screen has no title bar: the taskbar's text only). `DeviceLost()` still
   true: five rounds, two seconds apart (our numbers, INFERRED), then the program closes in order with one sentence
   (the log; a message box in a window; exit code 1); the same at once when the device is there but the map could not
   be opened again. The log has each picture's `RecoverNumbers`.
5. **Checks.** `moho64ui_RecoverDevice("check" | "lose" | "fail")` runs exactly this at the next frame boundary (the
   device not lost; really lost by your `LoseDeviceForTest`, only in a run with the new `--shader-cache <folder>`,
   which is `SetShaderCache` on every picture; a recovery that finds no device, played through to the close);
   `moho64ui_DeviceState()` says what happened and what the world's switches are; `ui/tests/device_lost.lua` (its
   window branches, for a live window, are written and not run).

Tested without a window only: a game's own picture and the menu background's own picture on its thread, each with a
check, a second right after, and a real loss by `RemoveDevice` (noticed at Present / "allocating a frame upload
page"; each picture back after 4 to 7 s at 1080p on stock Seton's and SETONS_R on a quiet PC, up to 19 s beside a
build); the photo dropped and the next one made; the close.
IN A LIVE WINDOW (2026-11-11 06:53-07:06, 1920x1080, Chris's screen, the run's own shader folder): the window's
picture came back each time with the interface over it and the game going on. A game: four recoveries of a device
not lost (9.4, 5.9, 5.1 and 6.9 s from the frame that asked: the device 0.35 s, the map 4.5 to 5.9 s, 5470 props'
states carried over), then a REAL loss (`LoseDeviceForTest`: 0x887a0005, noticed at Present) and its recovery:
`device_lost.lua -- /devicecase lose`, 49 checks, 0 failed; tick 81 before the first recovery, 122 two seconds after
it, so `Beat` on the beats' thread while `RecoverDevice` ran on the frames' thread held. The menu with the flight:
two recoveries (11.9 and 10.5 s, the map 10.8 and 9.4 s), the evening and the flight's air on the new world, the
battle going on: 16 checks, 0 failed.
CHANGED 2026-11-11 on Chris's word: when the MENU's map cannot be opened again after a recovery (the window's
picture showing the menus' map, or the menu background's own picture) the program no longer closes: the menu goes
on without its background and its notice line says so; the flight counts as failed and the still map is tried, as
when a menu's map would not open at the start. A GAME's map gone still closes the program in order. Five rounds two
seconds apart stay (his yes). His yes too to a line for the player in full screen: ui request 52 to you.
STILL not tested: the second window after a recovery, a map opening behind at the moment of the loss, the read-back
host step (`--gdi`), Vulkan, a real driver reset; and two pictures at once (the program never has two).


## renderer, request 71 (render/REQUESTS.md): the land round the map by default, dxcompiler.dll, a lost device (2026-11-10)

**A, built (ui 58fa4aa0), your "simpler" way with the earlier look kept.** Map Edge's first choice, No Machinery
(key 'beyond', the default as before), now sets `World::mapSurround` on: the map's own land where the map brings
one, the mirrored map elsewhere. Why not a new default value: FAF's lobby keeps every option per profile
(`LobbyOpt_RemasterMapEdge`, and again in each lobby preset), so everybody who ever opened a lobby, Chris included,
has 'beyond' saved and a changed default would have reached nobody. The mirrored map on every map stays a choice,
the third: "Mirrored Map" (key 'mirror', `mapSurround` off). 'surround' as a key is gone; the program reads it as
'beyond' (`--map-edge surround`, an old replay's option). `--map-edge frame|beyond|mirror`. The menu's flight asks
for 'beyond', so it shows the land too. Its 0.3 s at the first frame on SETONS_R: not measured by me yet; I will say
if the menu's start or a game's first frame shows it.

**C, built (the same commit).** At the start, before any picture or window: `ShaderCompilerReady`; false: "FA
Remastered cannot start: " + why on stderr and in the log, in a message box when the run has a window, exit 3.
Checked with a copy of the program without the dll (the lab's suite/map_edge.sh).

**B: built, see the entry above** (2026-11-11; headless tests green, a live window's check is owed).

## renderer, request 69 (render/REQUESTS.md): Effects Mirrored in Water as a player option (2026-11-10)

Built. Options > Remastered, section Water, after Shore Soak: "Effects Mirrored in Water" (`remaster_water_mirror`,
toggle, On unless chosen; Off = the water mirrors none of them, as before). It sets `World::waterFxMirror` with the
other looks each frame, so it takes effect at once. Console `remaster_water_mirror 0|1`;
`moho64ui_Looks().waterFxMirror` for tests. Preset levels by your cost: off in Off and Low, on from Medium up.
No slider for the strength: `world.water.fxMirrorStrength` stays yours (8, Chris's choice); say so if he wants one.

## renderer, request 67 (render/REQUESTS.md): Warp-in in Faction Colours as a player option (2026-11-10)

Built. Options > Remastered, section Effects & Battle, after Remastered Gateway: "Warp-in in Faction Colours"
(`remaster_warpin_colours`, toggle, On unless chosen; Off = the earlier warp-in). It sets `World::warpInColours` with
the other looks each frame, so it takes effect at once. Console `remaster_warpin_colours 0|1`;
`moho64ui_Looks().warpInColours` for tests. Preset levels: off in Off, on from Low up (no measurable cost, your word).

## renderer, request 65 (render/REQUESTS.md): FSR 3 switched off inside a game; the two labels (2026-11-10)

Built, both parts.

1. **FSR 3 / Frame Generation switched off in a running game stay on until the next picture is made.** In the
   session's per-frame looks (`ui/app/game_session.cpp`, at "The player's own looks"): while `render::gSceneSamples`
   is 1 and the player's looks now ask for more than one sample (`Looks::SceneSamples() > 1`, so both FSR 3 and Frame
   Generation are off in the Options and Anti-Aliasing is above 1), the two switches keep the values last given to the
   picture (FSR 3 on when nothing was given yet). The log says once: "FSR 3 / Frame Generation switched off in this
   picture: kept on until the next map". The next picture is made with the player's Anti-Aliasing and both off, as
   before. Checked: a game begun with FSR 3 on and Anti-Aliasing 4, the option set off in the game: the option reads
   off, the log line is there. I did not ask for a live switch: the next map is how Anti-Aliasing already behaves.
   Tips: Temporal Upscaling (FSR 3) and Frame Generation both say "switched off in a running game it stays on until
   the next map".
2. **"Upscaling (FSR 1)" is now "Render Scale"** (the option's key `remaster_upscale` and its values are unchanged).
   Its tip: how large the world is drawn before it is scaled up, by FSR 1, or by FSR 3 when Temporal Upscaling
   (FSR 3) is on. The first-start note's word follows ("Render Scale in Options > Remastered").

## renderer, request 62 (render/REQUESTS.md): the map's own land past its edge as a third Map Edge choice (2026-11-09)

Built. The lobby's "Map Edge" option (`RemasterMapEdge`) has a third value:

| Text | Key | What the session sets on the World |
|---|---|---|
| No Machinery (default, unchanged) | `'beyond'` | `worldBeyond = true`, `frame = false`, `mapSurround = false` |
| Machinery | `'frame'` | `worldBeyond = false`, `frame = true`, `mapSurround = false` |
| Sculpted Land (new) | `'surround'` | `worldBeyond = true`, `frame = false`, `mapSurround = true` |

- `beyondHaze` stays false in all three, as before.
- The default stays `'beyond'`, as you asked (the surround is a block-out until the visuals track's detail pass; the
  default is Chris's word). The main menu's flight keeps `'beyond'` too (`ui/data/menuflight/menu_session.lua`).
- The option's help text says it applies on maps that bring such land (Seton's Clutch Remastered) and that other maps
  are drawn as No Machinery; that fallback is yours (`mapSurround` with no usable surround file).
- `--map-edge frame|beyond|surround` is the same for a game given with `--session`. The session logs which one:
  `session: the map's edge: no machinery, the map's own sculpted land beyond (where it has one)`.
- Where: `ui/data/hook/lua/ui/lobby/lobbyOptions.lua`, `ui/app/game_session.cpp` (`SkyOnWorld`, the option's read).
- Tested: `ui/tests/lobby.lua` (the third value's key, the default still `'beyond'`), a SETONS_R game and a game on a
  map without a surround, both with `--map-edge surround` (219 checks each, 0 failed). Not looked at in a window by
  me: the picture is your check.
- If the default should change later, or the menu's flight should show the land, say so: one line each.

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

1. **Item 5, "health regen on [a private mod's air experimental] shows +0 while shields show +600"**: our host's fault, not the blueprint's (the
   staged MIA0401 has Defense.RegenRate 300). FAF's unit view shows `GetStat("HitpointsRegeneration")`, and our
   UserUnit:GetStat always answered the default, so every unit showed "+0/s". It now reads the unit's statistics from
   the engine's view (UnitStateView::stats, engine 13a6e16): the regen numbers, veterancy (VetLevel, VetExperience)
   and the reclaim totals are real. That unit should now show "+300/s".
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
   load none. The tours of the private mod's air experimental load the Wyvern Battle Pack because his private mod requires it; Chris chose
   (2026-10-20) a mod-free tour beside them: `_scratch\ui\Open-Game-Tour.bat` (an Aeon CZAR on the rebuilt map, no
   mods at all). The mod units' remastered art moved out of `visuals\work` (visuals, 2026-10-20) into
   `visuals\work_mods\<mod>`, mounted only by the launchers of games with that mod (`--remasters` may be given more
   than once now).
