# Shared testing framework

One framework and one set of tests run on Windows, Linux and macOS. The Windows adapter is named
`windows`. `tests/run.py --platform linux|windows|macos` selects the native adapter; the default
`--platform auto` detects the current operating system. Execution modes are `headless` and
`windowed`. Windowed mode opens an application window on the current operating system.

## Layout

```text
tests/
  framework/
    lua/framework.lua      assertions, input, waits and fixtures
    host/                  application input bridge and platform adapters
    adapters.py            process, executable, loader and operation compatibility
    runner.py, cli.py      isolated execution and JSON/JUnit reports
    selftests/             framework verification
  ui/                      new input and skirmish Lua tests
  fixtures/                skirmish session data
  suites/                  platform agnostic case definitions
  run.py                   command-line entry point
```

Tests import `/testing/framework.lua`. The dedicated `moho64ui_test` host mounts
`tests/framework/lua/` at `/testing`, registers the input adapters, and supplies the test clipboard.
The game and test host share the application library. The game executable uses the system
clipboard and contains no framework adapters or registration.
The simulation and viewer wrappers are `moho64_test` and `moho64view_test`. All test hosts capture
engine and Lua diagnostics through the engine's general log observer.

## Run

Runtime cases need your game's mount/init file. Python 3.9 or newer and its standard library are
sufficient for the runner. Build the test host with CMake's `BUILD_TESTING=ON` or the existing UI
build script:

```sh
cmake -S . -B build-native -DMOHO64_BUILD_UI=ON -DBUILD_TESTING=ON
cmake --build build-native --parallel 4
# Windowed host on Windows:
py ui/build_ui.py --tests --out build-ui
```

The CMake interface host uses software rendering and runs headless outside Windows.
The existing Zig UI build supports SDL windowed hosts on Linux and macOS when SDL3 is installed.
Simulation and viewer cases require their corresponding test executable in the build directory.
The simulation wrapper is built independently of the UI; the viewer wrapper requires a
native viewer target, supplied by the Linux compatibility layer.

Then run the same entry point on each platform:

```sh
python3 tests/run.py --list
python3 tests/run.py --build-dir build-native --init path/to/init.lua
python3 tests/run.py --build-dir build-native --init path/to/init.lua --mode windowed
python3 tests/run.py --build-dir build-native --init path/to/init.lua --test input-contract
python3 tests/run.py --build-dir build-native --init path/to/init.lua --suite tests/suites/skirmish.json
```

The same entry point accepts an explicit platform on each host:

```sh
python3 tests/run.py --platform linux --build-dir build-native --init path/to/init.lua
py tests/run.py --platform windows --build-dir build-ui --init path/to/init.lua
```

The selected platform must match the operating system running the tests.

`--build-dir` names the directory containing `moho64ui_test` (or `moho64ui_test.exe`), such as `build-native` or
`build-native/Debug` for a CMake build with configuration subdirectories. On Windows, `py` can replace
`python3`. Windowed mode requires a desktop; Linux CI can provide a virtual display:

```sh
xvfb-run -a python3 tests/run.py --build-dir build-native --init path/to/init.lua --mode windowed
```

The default suite runs input contracts, controls, keys, lists, bitmap animation, console, lobby,
sound and movies. The skirmish suite checks simulation, move/stop orders, camera behavior and restart.
Existing Lua cases remain in `ui/tests/`, with their map and preference fixtures. They keep their
assertions and scheduler calls. A local `check` adapter
uses shared reporting, and input calls use named operations through the platform adapters.
Platform independent inspection bindings remain available directly.

Each case runs in a separate process with its own preferences and user-files folder. Every
invocation has a unique output directory containing `host.log`, `command.json`, `results.json`
and `junit.xml`. Each case also writes `events.jsonl`, the authoritative execution record.
Use `--output <directory>` for test artifacts outside the checkout. Processes run in separate
POSIX sessions or Windows jobs, and their descendants are stopped when a case exits or times out.

The runner requires the dedicated test host. CMake's `BUILD_TESTING=OFF` excludes the test host
and all self-tests from the build.

Exit codes: **0** all selected cases passed; **1** a case failed or skipped; **2** invalid
configuration. Crashes, timeouts, script errors, failed assertions, missing/duplicate completion
and completion without checks fail the run. Skips are explicit and never count as passing.
Lua outcomes come from structured assertions and lifecycle events. Human log messages cannot
create a pass, failure or skip. Command cases explicitly declare their required output patterns
and also require a clean structured host record. Requested captures must exist.
See [the version 1 protocol](testing-protocol.md) for the execution-record contract.

## Write a test

```lua
local framework = import('/testing/framework.lua')
local test = framework.suite('my control')
local driver = framework.input()
test:run(function(t)
    t:require('input')
    -- Create and focus a control using the game's Maui APIs.
    driver:press('A', 'Primary')
    driver:click(200, 150, 'Left')
    t:check('the control exists', myControl ~= nil)
    t:wait_until('the control updated', function() return myControl:IsChecked() end, 5)
    t:cleanup(function() driver:release_all() myControl:Destroy() end)
end)
```

The runner starts scripts after a frame, so the window is ready before input. Assertions record
failures and continue. Cleanup runs in reverse registration order before completion and must not
yield. The host observes errors from callbacks, scheduler threads and protected engine calls,
then runs suite cleanup between frames. Completion waits for threads created through `test:fork`.
Cleanup also runs when a test skips or hands control to another application state. Crashes and
process timeouts can prevent Lua cleanup; the runner still stops the process group or job.
The windowed host restores the system clipboard before closing its window.

### Common API

| API | Contract |
| --- | --- |
| `framework.suite(name)` | Create a suite; the native host retains failures across application states |
| `test:check(label, ok)`, `test:equal(label, got, want)`, `test:near(...)` | Record assertions |
| `test:assert(label, ok)` | Record an assertion and stop on failure |
| `test:wait_frames(n)`, `test:wait_seconds(n)` | Preserve the game's scheduler semantics |
| `test:advance_frames(n)` | Wait for exactly n frame advances |
| `test:wait_until(label, predicate, timeout)` | Bounded condition wait |
| `test:fork(fn)`, `test:run(fn)` | Schedule tracked work; `run` finishes the suite automatically |
| `test:cleanup(fn)`, `test:finish()` | Resource cleanup and one completion marker |
| `test:checkpoint()` | Drain work and clean up before a state handoff; a later stage must complete the case |
| `test:skip(reason)` | Report a missing prerequisite explicitly |
| `framework.fixture(name)` | Read a fixture supplied by the runner |
| `framework.graphics.*` | Apply rendering settings and select logical graphics alternatives |

Graphics selections use `default` and `alternative`; backend identifiers stay in the framework.
`framework.within` and `within_xz` provide shared numeric and position comparisons.

### Input API

Create a device with `framework.input()` or `framework.input('Shift Primary')`.

| Operation | Meaning |
| --- | --- |
| `press(key, modifiers, text)` | Key down, text if unhandled, key up; text optionally specifies the committed character |
| `key_down(key, modifiers)`, `key_up(key, modifiers)` | Individual key transitions |
| `type_keys(text)` | Type printable ASCII with key transitions and Shift for uppercase or shifted punctuation |
| `type_text(utf8)` | Committed Unicode text without key presses |
| `move(x, y, modifiers)` | Move in client/UI pixel coordinates |
| `click(x, y, button, modifiers)`, `double_click(...)` | Complete click interactions |
| `button(button, pressed, x, y, modifiers)` | Individual named button transition |
| `button_down(button, x, y, clicks, modifiers)`, `button_up(...)` | Individual button transitions |
| `scroll(x, y, notches, modifiers)` | Positive away from the user, negative toward the user |
| `resize(width, height)` | Request client dimensions; wait for the resulting UI size |
| `clipboard_write(utf8)`, `clipboard_read()` | Clipboard operations through the selected adapter |
| `release_all()` | Release held keys and buttons |

Keys include A–Z, 0–9, F1–F24, Enter, Tab, Escape, Backspace, Space, Delete, Insert, Home, End,
PageUp, PageDown, arrows, Shift, Control, Alt and named punctuation. Buttons are Left, Middle and
Right. Names are case insensitive. **Primary** is Ctrl on Windows/Linux and Command on macOS.
Per-operation modifiers override device modifiers; otherwise held modifiers carry into later
operations. Dragging preserves held button state. Unknown names, malformed Unicode and invalid
coordinates/sizes raise errors.
`type_keys` validates its entire string before dispatch. Use `type_text` for Unicode and named
key operations for Enter, Tab and other control keys.

Windowed adapters dispatch through production application window handlers. Headless mode uses
production UI handlers and a memory clipboard shared with edit fields. Input calls dispatch
synchronously; use waits for work scheduled by controls. Injection exercises window translation,
independent of physical input devices.

## Run or register a case

Select a registered case by name:

```sh
python3 tests/run.py --init path/to/init.lua --test movie
python3 tests/run.py --init path/to/init.lua --suite tests/suites/skirmish.json --test camera
```

Register additional cases in a suite JSON file and pass `--suite`. The suite owns each case's
script, session, startup stage, frame budget and rendering settings:

```json
{
  "tests": [
    {"name": "my-control", "script": "tests/ui/my_control.lua"},
    {
      "name": "orders", "session": "tests/fixtures/skirmish.lua",
      "script": "tests/ui/skirmish_smoke.lua", "game_script": true,
      "frames": 1500, "args": ["--seed", "7"]
    },
    {
      "name": "graphics-pipelines", "host": "viewer", "operation": "graphics_pipelines",
      "frames": 3, "completion": false, "required": ["--gfx-check.*all right"]
    }
  ]
}
```

Paths are relative to the repository unless absolute. `width` and `height` select fixture
geometry; `window: false` requests a case without a window even during windowed suite runs.
`game_script: true` runs after game UI creation. `setup` names a front-end preparation script.
`world: true` enables world rendering. `required` contains regular expressions for command checks; request `rendering` or
`window_input` capabilities when those are required by a case.
`setup` runs as a separate preparation stage before the test, including when both execute in
the frontend. It cannot replace the test script.

Configuration is validated before launch. Boolean and numeric fields retain their JSON types;
timeouts must be finite and positive. Unknown fields, duplicate names, invalid stage order and
incompatible host operations are rejected. `args` cannot override runner-controlled paths,
scripts, geometry or limits. The test kind is inferred from the presence of a script. Command cases require `completion: false` and `required`.

Use explicit stages for a test that launches a game or replay from the frontend:

```json
{
  "tests": [{
    "name": "multiplayer-replay", "stage": "frontend", "frames": 3000,
    "stages": [
      {"name": "launch", "script": "ui/tests/mp_replay.lua"},
      {"name": "watch", "script": "ui/tests/mp_replay.lua", "context": "game"}
    ]
  }]
}
```

Stages execute in order and share the case's preferences and fixtures. The final stage is
automatically terminal. A frontend suite calls `test:checkpoint()` after launching the game;
preparation without a suite completes when its body returns. Both wait for scheduled work and cleanup. A terminal stage
can use `test:checkpoint()` before restarting; its replacement state repeats that stage and
must eventually finish. The host aggregates assertions across states.
Preparation may yield. The host delays session startup and game-state creation until the
frontend wrapper finishes, so preparation is independent of map-loading speed.

Replay, mod, campaign, profile and cheat-dependent cases retain their prerequisites. Supply
external replay data with `--fixture multiplayer_replay=path/to/replay.SCFAReplay`; absent data
is reported as skipped. Preference-import cases use their checked-in fixtures and their documented
`/prefscase` stages; multi-stage cases retain their preference and front-end state handoff.

Python callers import `Runner`, `TestCase` and `TestStage` from the `framework` package under `tests/`.
Platform process and operation translation belongs in `framework/adapters.py`; application input
translation belongs in `framework/host/`. Test cases use the same API on every host.

## Verification

These checks require no game data or desktop:

```sh
ctest --test-dir build-native --output-on-failure
python3 tests/framework/selftests/test_runner.py
```

CTest checks the common Lua API in the actual game VM, parses all Lua cases, checks C++ tests,
verifies process timeouts and reporting, and checks production UI state isolation.
The native host contracts inject callback and thread exceptions, failed or yielding cleanup,
missing stages, unfinished work, false log markers and premature process exit without game data.
The portable workflow runs these checks on Linux, Windows and macOS. Runtime input contracts
also need the configured game data and a display for windowed mode.

## Original-game session fixtures

The [Lua session regression suite](session-replay.md) compares frozen original-game
observations with isolated simulation runs through the shared `tests/run.py` entry
point. Its scripts and numeric fixtures are portable across native hosts.
