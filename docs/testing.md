# Testing

Tests use a shared Lua framework and dedicated test hosts. Platform adapters keep
operating-system details out of test cases; production executables do not contain
the test adapters.

## Run tests

Run commands from the repository root. Build with CMake's `BUILD_TESTING=ON`;
see the [build instructions](../README.md) and [Linux setup](linux.md) for platform
requirements. Runtime tests need game data and a mount/init file.

```sh
python3 tests/run.py --help
python3 tests/run.py --list
python3 tests/run.py --build-dir build-native --init path/to/init.lua
python3 tests/run.py --build-dir build-native --init path/to/init.lua --mode windowed
```

`--build-dir` must point to the directory containing the test executables.
The runner selects the native platform automatically. Windowed tests need a
desktop or virtual display; headless tests cannot establish windowed behavior.

Use `--test NAME` to select a case and `--suite PATH` to select a suite.
Use `--list` with that suite to discover its current cases. Use `--output PATH`
to put reports in a temporary directory outside the checkout. The runner prints
the report location.

Each case runs in its own process with isolated preferences and user files.
Inspect the results and diagnostics before claiming a pass. Failed assertions,
crashes, timeouts and missing completion fail the run; skips do not count as
passes. Structured execution records determine outcomes; human log messages are
diagnostics. See the [execution protocol](testing-protocol.md) for that contract.

## Write tests

Use the game's [Lua dialect](lua-dialect.md). Import `/testing/framework.lua`,
create a suite with `framework.suite(name)`, and run its body with `test:run(fn)`.
See an [input test](../tests/ui/input_contract.lua) for a working example and the
[framework source](../tests/framework/lua/framework.lua) for the current API.

- Record assertions through the suite. Every successful test must perform checks
  and complete its lifecycle.
- Declare required capabilities with `test:require(...)` and supply prerequisites
  through fixtures. Missing prerequisites must be explicit.
- Use `framework.input()` and named keys, buttons and modifiers. `Primary` means
  Ctrl on Windows/Linux and Command on macOS. Keep platform translation in the
  adapters.
- Wait for scheduled changes with frame waits or bounded predicate waits.
- Register cleanup as resources are acquired. Release held input and destroy
  controls. Cleanup runs in reverse registration order and must not yield.
- For application state changes, use the framework's tracked work and checkpoint
  lifecycle so assertions and cleanup survive the handoff. See existing staged
  cases before adding one.

Register cases in a JSON file under [tests/suites/](../tests/suites/).
Suite definitions own scripts, fixtures, startup stages and execution settings.
Use existing suites as examples; the [case definitions](../tests/framework/cases.py)
and [CLI](../tests/framework/cli.py) define the accepted configuration.

## Verify changes

Framework self-tests need no game data or desktop:

```sh
ctest --test-dir build-native --output-on-failure
```

Run the affected runtime cases with configured game data. For input or window
changes, exercise both headless and windowed modes. Report the platform and
display environment actually exercised.

Simulation changes also require the original-engine replay comparison described
in [README.md](../README.md#how-it-is-checked); runtime smoke tests alone do not establish
replay or multiplayer determinism.

The [Lua session regression suite](session-replay.md) replays captured original-game
sessions in isolated headless processes and compares state tick by tick. Its
original baselines can be reused across engine changes.

## Original-game session fixtures

The [Lua session regression suite](session-replay.md) compares frozen original-game
observations with isolated simulation runs through the shared `tests/run.py` entry
point. Its scripts and numeric fixtures are portable across native hosts.
