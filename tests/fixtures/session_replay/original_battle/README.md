# Original battle observations

These fixtures contain numeric simulation observations from the purchased stock
Forged Alliance engine using the existing `FA_ORACLE` Lua hooks. Two independent
original-game runs matched every captured value across 6,000 ticks.

| Fixture | Captured ticks | Purpose |
| --- | --- | --- |
| `startup.jsonl.gz` | 1–8 | Startup and initial scripted unit creation |
| `prefix.jsonl.gz` | 1–128 | Short movement, combat, construction and economy regression |
| `full.jsonl.gz` | 1–6,000 | Ten-minute session with repeated scripted orders |

The shorter captures are prefixes of the full original recording. They retain
the original values and have completion records for their declared ranges.
`provenance.json` records source identities, repeatability and fixture hashes.

`session.lua` is the captured launch configuration with descriptive map text
removed. `tests/replay/fixtures/battle.lua` supplies the shared scripted orders.
The setup's end tick remains 6,000 in every case so the order schedule is identical
to the original; a prefix case stops its engine run after its last captured tick.

Run from the repository root:

```sh
python3 tests/run.py --suite tests/suites/session_replay.json --list
python3 tests/run.py --suite tests/suites/session_replay.json --init tests/fixtures/session_replay/stock_init.lua
```

Set `FA_GAME_ROOT` to the stock game directory from your own installation, or
pass an existing stock init file. It must include the stock archives and SCMP_001.
The baseline
is tied to the original executable identified in the provenance file. The usual
FAF overlay requires a separate matching baseline.

No game archives, executables, copied game scripts, account data or machine paths
are included. These cases compare Lua-visible identity, position, health and
stored resources. Rendering, audio and hidden native state require other tests.

The initial new-engine run matches the startup fixture. The prefix and full
fixtures report the first position difference at tick 9. Their expected data
remain the original recording, and these cases keep returning failure until
the engine matches it.
