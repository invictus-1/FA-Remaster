# Lua session regression suite

The [shared runner](../tests/run.py) plays a saved game in a fresh headless
simulation process and compares its Lua state against a frozen capture from the
original game. Capture the original once; subsequent regression runs need only
the new engine. The Python runner uses the standard library and reads one tick at
a time, so memory use does not grow with game duration. Compressed `.jsonl.gz`
captures are supported.

The same [collector](../tests/replay/capture.lua) runs in both engines after
`BeginSession`. It observes state through the game's simulation Lua API and
samples at the Lua thread phase, with no random draws or issued orders. It records:

- Session map, options and active mod UIDs.
- Every army's defeat state and mass/energy stored, storage ratio, income,
  requested resources and usage.
- Every unit returned by each brain's `GetListOfUnits(ALLUNITS, false)`, identified
  by entity ID: blueprint, army, position, velocity, orientation, layer, health,
  construction/work progress, fuel, shield ratio, unit states and command queue.

Fuel is `-1` for units without a motion type, whose fuel getter is invalid.

The regression result requires matching state, every scheduled tick, a complete
capture, a successful engine exit and structured host records with no engine or
Lua errors. Missing assets, stale collectors, truncated/duplicate captures,
crashes and timeouts fail. The comparator reports the first differing tick and
field, including missing or unexpected units. There are no accepted differences
or automatic baseline updates.

The initial live verification used the installed stock game in the existing
Steam/Wine lab and captured all 6,000 ticks of a ten-minute controlled session.
The original capture was 402 KB compressed, contained six blueprint types,
and observed 1,500 health decreases and 60 unit removals. A second original run
matched every captured value on all 6,000 ticks. The candidate also ran
all 6,000 ticks. The comparison reported an engineer position difference at
tick 9 (`482` versus `482.00390625` on X); it remains a failing regression case.
That result verifies the capture/comparison path, not complete engine parity.

## Capture a baseline in the original game

### Use the existing Steam/Wine hooks

With an approved original-game Steam/Wine lab running, the automated adapter
reuses `tools/fa-runner/hook.lua` and the `FA_ORACLE` exporter from that lab:

```sh
python3 tests/replay/original.py --lab /data/repos/faf --ticks 6000 --through-steam
```

The default case covers ten simulated minutes, with a construction and combat
round every simulated minute. Every tick is captured. The adapter saves the
original `LaunchSinglePlayerSession` table, source hashes, compressed original
observations, a matching candidate init file and `suite.json` in a new private
lab directory. It uses bounded log chunks for long records. The game process is
owned by its unique init path; completion and failure release that process.

The original must finish every requested tick and every configured round before
the adapter creates a runnable suite. Steam launch errors, original-game crashes
and missing completion leave an incomplete capture. The adapter preserves the
existing Steam session and does not submit login information.

Run the generated suite using the directory printed by the adapter:

```sh
python3 tests/run.py --suite /path/to/capture/suite.json --fixture-root /path/to/capture --init /path/to/capture/candidate-init.lua --output /path/to/reports
```

`oracle_format: "fa-oracle"` compares all fields exported by the existing hooks:
unit ID, blueprint, army, position, health, maximum health and stored mass/energy.
The candidate's extra fields are outside this original capture's coverage.
`session_info` selects the original launch table via the headless host's
`--session-info` option. Its Lua file returns that table. `sim_setup` names the
shared order script and `setup_input` supplies its JSON inputs. The setup runs
inside the observer coroutine, so its child threads follow the observer in the
same order as the original hooks. This mode requires the existing lab
tools only for generating original captures; candidate runs use this repository's
standard library runner.

The automated lab adapter selects its installed stock `SupremeCommander.exe`
and stock archives. The headless host supports the stock simulation's prefetch
object and runs the captured launch table with those same archives. Any engine
or Lua error fails the test. A completed run can still report a state difference;
the stock script path and the usual FAF path need their own baselines.

### Use the shared expanded collector

Use your own original-game installation, game scripts, maps, mods and replay.
Keep these at the same versions for the candidate run. Choose an end tick that
includes the game you want to cover and is reached before the simulation freezes
at game over. The default interval of one observes every game tick (ten per
simulation second); a larger interval samples less of the game.

```sh
python3 tests/replay/run.py prepare --name full-game --end-tick 18000 --output /tmp/fa-original-capture
```

In the original game's **test init configuration**, add the generated directory
as the first VFS mount and append its hook folder after the existing hooks:

```lua
table.insert(path, 1, {dir = '/tmp/fa-original-capture', mountpoint = '/'})
hook = hook or {}
table.insert(hook, '/session-replay-hook')
```

Use a native directory path on Windows, such as `C:/fa-tests/original-capture`.
Run the saved `.SCFAReplay` with that configuration and save the original game's
log. Wait for the `SESSION_REPLAY` record with `"event":"complete"`. The overlay
is test instrumentation; remove its mount and hook when finished capturing.

Extract the marked JSON records:

```sh
python3 tests/replay/run.py collect --log /path/to/original-game.log --output /path/to/fixtures/game.jsonl
```

Using `game.jsonl.gz` as the output compresses the capture. Collection validates
tick coverage and completion and refuses to overwrite an existing file. The
capture role identifies the intended host; it is not independent proof that the
log came from the original game. Keep original baselines and replays together.

## Run the regression suite

Build the test host with `BUILD_TESTING=ON`, using the existing native build:

```sh
cmake --build build-native --target moho64_test
python3 tests/run.py --suite tests/suites/session_replay.json --list
python3 tests/run.py --suite tests/suites/session_replay.json --build-dir build-native --init tests/fixtures/session_replay/stock_init.lua
```

The supplied [suite](../tests/suites/session_replay.json) includes committed,
repeatability-checked original observations and a sanitized launch configuration.
It registers an eight-tick startup case, a 128-tick prefix and the full
6,000-tick controlled session. Set `FA_GAME_ROOT` to your stock game directory
when using the supplied init file. On Windows PowerShell use
`$env:FA_GAME_ROOT = 'C:/path/to/game'`; on POSIX shells use
`export FA_GAME_ROOT='/path/to/game'`. Game assets are supplied by the player.
The simulation cases use the shared platform process adapter and run headless
on every native host, including when other cases in a mixed suite use a window.

Add a case per recorded game to cover different maps,
factions, units, combat, transports and AI/mod combinations:

```json
{
  "tests": [
    {
      "name": "naval-battle",
      "replay": "naval.SCFAReplay",
      "oracle": "naval.jsonl.gz",
      "mods": "naval-mods.txt",
      "maps": ["extra-maps"],
      "timeout": 600
    }
  ]
}
```

Capture with the same `--name` as its suite case. Input paths are relative to
`--fixture-root` (the repository by default) in the shared runner. The optional
standalone replay CLI accepts the same path root as `--fixtures`. Absolute
paths are accepted. `mods` is an ordered UID file, as accepted by `moho64 --mods`;
the configured init file must mount those mods. `maps` lists additional folders
mounted at `/maps`. Select cases with repeatable `--test NAME` and use
`--suite PATH` for a different suite.

Lua lobby configurations are also supported:

```json
{"tests": [{"name": "ai-game", "session": "game-config.lua", "seed": 12345,
            "stream": "orders.bin", "oracle": "ai-game.jsonl", "timeout": 600}]}
```

`session` names a `GameConfig` file accepted by `moho64 --session`; `seed` is
required. `stream` is optional and contains the command stream for that session.
The original capture must use that same configuration, seed and order timeline.
Using an original replay is usually easier, because its header already contains
the configuration and seed and its body contains the orders.

Every case has a unique working directory, its own init overlay and user-folder
overrides, and an in-memory `Game.prefs`. Inherited `MOHO64_*` diagnostics and
behavior overrides are removed. Original-engine behavior is selected with
`MOHO64_FIXES=0`. The shared portable platform adapter enforces timeouts and
releases the process tree. Outputs are temporary and removed by default; use
`--output /path/to/reports` to keep host logs, captures, commands, first-difference
JSON, `results.json` and `junit.xml` for inspection or CI.

Comparisons are exact by default. A case can explicitly set `abs_tol` and
`rel_tol` for numeric state values. IDs, army indices, command types, queue lengths,
strings, booleans and unit membership stay exact. Metadata always stays exact.
Changing the collector requires recapturing original baselines; its SHA-256
digest is stored in the capture header.

```sh
python3 tests/replay/run.py compare --oracle game.jsonl --actual /path/to/report/actual.jsonl
ctest --test-dir build-native -R 'lua_session_capture|session_replay_runner' --output-on-failure
```

## Coverage limits

This suite checks simulation behavior visible through Lua for the whole captured
timeline. It does not check rendering, UI input, audio, unexposed projectile/prop
internals, random draw ordering, or floating-point bit patterns such as signed
zero. Retain the binary oracle checks in `tools/recdiff.py` and `tools/regress.py`
for those simulation details, and the existing UI/renderer suites for their
workflows. A candidate compared with another candidate establishes repeatability;
original-engine parity requires a real original capture.
