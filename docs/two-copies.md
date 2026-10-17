# Two copies: the lockstep check

**For:** the engine track; the interface track when its multiplayer stage comes (ui-roadmap stage 9).
**Status:** first version, 2026-10-16. `tools/twocopies.py`, `src/sim/fingerprint.*`, `MOHO64_LAYOUT_SHIFT` in
`src/app/main.cpp`. Nothing in it changes what the simulation computes. Since 2026-10-17 Lua hashes keys that are
objects by a serial number, not by their address ("Keys that are objects" below).

## Why

Multiplayer in this game is lockstep. Every player's machine runs the whole simulation from the same orders,
and only the orders cross the network. So two copies of one game, fed the same orders, must stay identical on
every beat, to the last bit. Anything that depends on the copy itself breaks that: a memory address, the hash
order of keys that are objects, memory nobody wrote, a thread's timing, the clock, the order a folder lists its
files in. The original game has the same rule and calls the failure a desync.

The 13 test replays cannot see such a bug. They compare our run with the original's recording, and one run of
ours is always consistent with itself. The two-copies check runs two copies of moho64 side by side on the same
input and compares them beat by beat. It will also guard the move to several cores: a race between threads
shows the same way, as two copies that drift apart.

## The three pieces

1. **The fingerprint** (`MOHO64_FINGERPRINT=<file>`, `src/sim/fingerprint.h`). After every beat (and once
   after start-up, as beat 0) one line: the beat, the game tick, a 64-bit hash of the state, and one hash per
   part, so that a difference has a name:

   | Part | What is hashed |
   |---|---|
   | `rng` | the session seed and the number of draws from the sim's random stream (that gives its whole state) |
   | `sim` | the path searches waiting their turn, the next command id, the command source, the pause, the focus army, the playable rect |
   | `a<N>` | army N: its economy, statistics, unit cap and count, alliances, platoons and squads |
   | `u<N>` | army N's units: id, blueprint, position, orientation, health, build fraction, layer, states, flags, velocity, both order queues, the weapons' targets and clocks, economy figures, recon flags |
   | `proj` | projectiles: id, blueprint, army, position, orientation, velocity, target, damage, expiry, bounces |
   | `props` | props: id, blueprint, position, orientation, health, fraction |
   | `ents` | every other entity (shields, beams, blips, the scripts' own): id, kind, position, orientation, health |
   | `lua` | not a hash, and not in the total: the Lua heap and the collector's threshold, in KB (see below) |

   Floats are hashed by their bits. Every list is hashed in entity-id order, never in a container's order.
   It only reads, so a run with it on plays exactly as a run without it: the 13 replays' recordings are
   byte for byte the same with and without (checked 2026-10-16). Off, it costs one test of a pointer per
   beat. On, it costs 0.5 to 1.2 ms a beat on 11 of the 13 replays, 2 ms on the bench replay and 5 ms on the
   Skirmish replay, an eight-army game on a map with 34,000 props (the props are most of it).
2. **The live check between two copies** (`MOHO64_FINGERPRINT_PEER=<the other copy's file>`). After writing its
   own line, a copy waits for the other's line of the same beat and compares them. At the first beat that
   differs, both copies write their whole state of that beat beside their file (`<file>.detail`: a line per
   army, unit, projectile, prop and entity, floats with 9 digits, which is a float's exact value) and stop
   (exit code 3). `MOHO64_FINGERPRINT_DETAIL=<beat>[,<beat>]` writes the same listing at given beats.
3. **Copies that differ on purpose** (`MOHO64_LAYOUT_SHIFT=<n>`). Before anything else, the copy takes and keeps
   n rounds of odd-sized blocks from both heaps the engine uses: the C runtime's, and the Lua heap's (mimalloc,
   through a Lua state of its own, so the game's own heap count is untouched). Everything after lands at other
   addresses. Windows and mimalloc already give every process its own addresses (mimalloc picks a random
   starting point, `third_party/mimalloc/src/os.c`), so this only makes sure of it.

`tools/twocopies.py` starts the two copies (copy B with the shift), lets them check each other, and prints the
first differing beat, the parts that differ, and the detail lines that differ, copy A above copy B.

## How to run it

From the lab folder (where `offline\bin\init_lab.lua` finds its paths), with any build of moho64:

```
py -I -X utf8 fa-remaster\tools\twocopies.py --moho64 fa-remaster\build-zig\moho64.exe <input> --out <folder>
```

| Input | Arguments |
|---|---|
| a replay, as the gate runs it | `--marks engine-ref\regress_marks_win_AIR.json` |
| any replay | `--sim <replay> --mods engine-bin\mods_bench.txt` |
| a new game (AI armies) | `--session <game configuration> --seed N --ticks 6000 [--mods <uids>]` |
| a game played in the interface | `--session <its table> --seed N --stream <its .stream> [--maps visuals\maps]` |
| two fingerprints made before | `--compare a.fp b.fp` |

Also: `--ticks N`, `--orders <file>` (moho64's own test orders, to both copies), `--env K=V`, `--shift 0` (no
deliberate shift), `--no-peer` (both run to the end, compared afterwards), `-- <more moho64 arguments>`.
The last line says `twocopies: identical <n> beats` or `twocopies: DIFFERS at beat <b> (tick <t>): <parts>`;
the exit code is 0, 1 (differs) or 2 (a copy failed).

The test that it works, `tests/two_copies_tostring.txt`: before beat 101 a table gets 16 keys made by tostring() of
fresh tables (their addresses), and a signature of the order pairs() gives them in decides how much health army 1's
first unit loses. It must split the copies:

```
py -I -X utf8 fa-remaster\tools\twocopies.py --session _scratch\ui\session_ai.lua --seed 7 --ticks 300 --orders fa-remaster\tests\two_copies_tostring.txt
twocopies: DIFFERS at beat 101 (tick 101): u1
  detail: 1 lines differ (unit 1)
  A: unit 0 uel0001 a1 p 672.5 18.6796875 346.5 q 0 0.375587851 0 -0.92678684 hp 11941/12000 ...
  B: unit 0 uel0001 a1 p 672.5 18.6796875 346.5 q 0 0.375587851 0 -0.92678684 hp 11602/12000 ...
```

Its twin, `tests/two_copies_pairs.txt`, does the same with keys that are objects (tables, functions, threads, and
every unit with its userdata, before beats 101, 111, 121 and 131). Since our Lua hashes such keys by a serial
number (below, "Keys that are objects"), the copies must agree: `twocopies: identical 301 beats` (2026-10-17: 5 of 5
runs, 2 of them with `--shift 0`; the tostring test split them in 4 of 4). Before that change the pairs test split
them in every run, the shift or not: with only its first key deciding, three runs without the shift took keys 9 and
12, 10 and 7, 14 and 4, so the system's own address randomisation is enough. The same test with units as keys
(every unit of a four-army game at beat 1500) split the copies at beat 1501 twice in three runs; once both happened
to take the same unit first. A single identical run therefore proves little about such a loop; many runs do.

## What it found (2026-10-16, main e57a0d0 with this change)

| Input | Beats | Result |
|---|---|---|
| The 13 gate replays (bench LAND AIR Naval Experimental Economy Transports Missiles Subs Engineers Upgrades Experimental2 Skirmish) | each to its end (701 to 13311) | identical, all 13 |
| Eight M28 AIs on SCMP_009, the Skirmish replay's options and mods without the lab's personal mods, seed 424242 | 6000 | identical |
| The same with seeds 1, 2 and 3 | 9000 each | identical |
| Four FAF "AI: Normal" on SCMP_009, seed 7 | 6000 | identical |
| The same with seeds 11 and 12 | 9000 each | identical |
| session_ai.lua (an idle player and an AI: Normal), seed 7 | 6000 | identical |
| The interface's recorded games: replay_testgame, replay_newcommander, _prev, _snow | 490, 7533, 7753, 7473 | identical |

No difference in the state in any of them. What did differ:

1. **The Lua heap's size** (the `lua` part), by 1 or 2 KB, in 9 of the 13 replays and in most of the AI
   games, never in the state. Seen with a counting build (not part of this change): in the AIR replay copy B had made three
   table rehashes fewer than copy A at beat 178, and its heap was 160 bytes larger from then on. The cause is
   Lua 5.0's table: a new key takes a free node, a dead key's node is reused only when it is the new key's
   main position, and the table grows when the last free node is gone (`third_party/lua/ltable.c` newkey).
   With objects as keys the main position comes from the address, so when a table grows differs between copies.
   The collector then starts from a slightly different heap: in the eight-army M28 game it ran at the same 12
   beats in both copies, with thresholds 1 to 2 KB apart from beat 3366 on. That is harmless as long as no
   script can see the moment the collector runs (a weak table it walks, a finaliser): none of the runs showed
   it, but it is the same root as item 2 and goes away with option (a) below. Gone since 2026-10-17, with
   option (a): "Keys that are objects" below.
2. **pairs() over tables whose keys are objects** (tables, units, userdata, functions): the order follows the
   keys' addresses. The test above shows it; FINDINGS.md ("PARKED FOR THE TWO-COPIES / MULTIPLAYER PHASE")
   has a mod's script whose order came out three ways with one seed. No script in the runs above did
   it in a way that reached the state. See the options below. Gone since 2026-10-17 (option (a)).

Found by reading the source (the work for the next rounds, with item 2):

3. **A C++ list in pointer order.** `UnitCommand::units` is a `std::set<Unit*>`, ordered by address. Most loops
   over it only test or count; the transports' load, retrieve and launch lists are sorted by id afterwards. One
   is not: the unload (`transport.cpp`, `TickUnload`'s `which`, filled from `c.units` with the command's units
   that ride this transport), which lets the units go in that order. Then seen in a test: an air transport
   with four tanks, unloaded by `IssueTransportUnloadSpecific`, let them go as 2 3 5 4 in one copy and 2 4 5 3
   in the other (and as 3 2 4 5, 2 3 5 4 and 2 3 4 5 in three runs of main's build). The state stayed the same
   there, as each tank leaves on its own; a transport script whose OnTransportDetach did something in turn
   would split the copies. Sorting `which` by id, as the other three lists are, gives 2 3 4 5 every time and
   leaves the 13 replays identical (gated 2026-10-16; a separate change). The original's command keeps its
   units in an EntitySet, ascending id (inferred from the other three lists, not read).
4. **The clock in the sim's Lua.** `GetSystemTimeSecondsOnlyForProfileUse` answers the real time. FAF's
   navigation mesh generator, sim callbacks, spawn helper and AI grids, M28's profiler and navigation
   generator, and RNGAI call it; every use looked at only logs or measures. A script that let it decide anything
   would split the copies. Read in full on 2026-10-17: see "The sim's Lua and the outside world" below.
5. **tostring() of a table, function or userdata** gives its address (`table: 0000057C9CDAD600`). Harmless in
   a log line; a script that used it as a key, or sorted by it, would split the copies. On Linux the
   C library prints `%p` without leading zeros, so there even the string's length, and with it the heap size,
   would differ.

Not a problem: the file listings the scripts see (`DiskFindFiles`) come sorted from the engine's file
system (`core/vfs.cpp`, `core/hostfs.cpp`), and the C++ caches keyed by blueprint or table address are only
looked up, never walked.

## pairs() over keys that are objects: the options ((a) built 2026-10-17)

Lua 5.0 puts a key that is a table, userdata, function or thread at node `address mod (nodes - 1 | 1)`
(`ltable.c` hashpointer), and next() walks the nodes in order. The original very likely does the same (its
Lua is 5.0 too; not read), on a 32-bit heap, which is why scripts that walk such tables are a known source
of desyncs in FAF.

- **(a) A serial number per object, used as its hash.** global_State gets a counter; every new table,
  userdata, closure and thread takes the next number (`luaC_link`, `luaS_newudata`, `luaE_newthread`);
  hashpointer hashes that number for collectable keys (light userdata keep their pointer). On 64 bits it costs
  no memory: the common header is a pointer and two bytes, so Table and Closure have four spare bytes at
  offset 12, Udata and lua_State six at offset 10. The sim's scripts make their objects in the same order in
  every copy, so every copy gets the same orders, and the tables grow at the same moments (item 1 above goes
  too). tostring could print the number instead of the address, as a later choice. The 13 replays should not
  move: our addresses already differ in every run, and the gate's result has not, so nothing in them depends
  on that order (inferred; the gate decides). It changes our order relative to the original's, but no
  replay of the original can depend on an order we could reproduce anyway. Cost: one load in place of a
  pointer cast in the hash. The change is in `third_party/lua` (lobject.h, lstate.h, lgc.c, lstring.c,
  lstate.c, ltable.c), small. This is the one to build. Built: the next section.
- **(b) Only the walking order made fixed.** Keep the address hash, but let next() go in insertion order
  (a second index per node, as Python's dict does). More memory per node, more work in the VM's busiest
  structure, and the tables would still grow at address-dependent moments (item 1 stays). Not worth it.
- **(c) The same addresses in every copy.** Give the sim's Lua state its own mimalloc heap in an arena at a
  fixed address, with the random start switched off. Then the same allocations give the same addresses, with
  no change to Lua. Fragile: any allocation that differs between machines (a path's length, the interface's
  own Lua in the same heap, another system's address space) moves everything after it, and a fixed address
  may be taken on another system. Good for an experiment, not as the answer.
- **(d) Leave Lua alone and fix the scripts** (sort the keys first) behind `Sim::fixes`, as the check finds
  them. Every new mod is a new risk; it does not scale.

## Keys that are objects (option (a), built 2026-10-17)

Table, Udata, Closure and lua_State have a `serial` (`third_party/lua/lobject.h`, lstate.h), taken from
`global_State::nextserial` when the object is made (luaH_new, luaS_newudata, luaF_newCclosure / newLclosure,
luaE_newthread, and the main thread in f_luaopen). Every state counts from 0. luaH_mainposition hashes a key of
those four types by its serial (the same odd modulus as before); light userdata keep their pointer; strings,
numbers and booleans are as before. On 64 bits the field sits in the padding after the common header, at offset 12:
the sizes stay 64, 32, 48 / 56 and 160 bytes (checked with zig for x86_64-windows, x86_64-linux and aarch64-macos;
a 32-bit build would grow them by 4 to 8 bytes, and no target is 32-bit).

Why that is enough. next() walks the array part, then the nodes in index order; the same keys inserted in the same
order at the same main positions fill the same nodes. A Lua 5.0 table grows only when every node holds a key, live
or dead (`firstfree` stops only at a node whose key is nil), so a table that only gains keys grows at the same count
whatever its hash; the hash moves the moment only in a table that also loses keys, as a dead key's node is reused
only as a main position. The engine's tables keyed by light userdata (the registry's fixed keys, `moho64.values`
keyed by the C++ object, the blueprint copier's `seen`) only gain keys and are never walked, so they stay alike.

What it gave (2026-10-17, main 322392e with the change):

| Check | Main | With the change |
|---|---|---|
| `tests/dialect_test.lua` run in two states at once (`tests/dialect_test.cpp`) | the orders differ | same order; the new table checks pass |
| `tests/two_copies_pairs.txt` | differs at beat 101 (or 121), 9 of 9 runs (3 with `--shift 0`) | identical, 11 of 11 (4 with `--shift 0`) |
| `tests/two_copies_tostring.txt` | differs | differs, 4 of 4 |
| the 13 gate replays and MP2, MP3: the state | identical | identical, all 15 |
| M28 x8 (seeds 424242, 1), AI: Normal x4 (seeds 7, 11), session_ai, replay_newcommander, units as keys x2: the state | identical | identical, all 8 |
| the Lua heap | differs in 6 of 10 inputs run once: AIR, Economy, LAND, AI: Normal x4 by 1 KB, MP2 and MP3 by 80 KB (M28 x8, Skirmish, Naval, Transports agreed that time) | agrees in all 23 but Missiles (below) |

The Missiles replay's heap still differs by 1 KB now and then between beats 480 and 1100, in about half the runs,
and not by an address. FAF's `SimCallbacks.DoCallback` (lua/SimCallbacks.lua:59-83) times every callback with the
clock (item 4) and SPEWs a line only when one took longer than 5 ms; the two copies' logs show copy A writing one
such line (CheatSpawnUnit, 0.007 s) that copy B did not. The string is garbage that stays until a collection, and
the Missiles replay never collects (its threshold is 440 MB), so A's heap is that line larger. Harmless unless a
collection's moment becomes visible to a script.

`tostring()` still prints the address (item 5). FAF's and M28's Lua use no tostring() of an object as a key; three
sim-side uses put an address into data: `factory-manager.lua:386` names a platoon "FactoryManager - table: ...",
`ChatBrainComponent.lua:118` stamps a chat message's Id, `utilities.lua` SerializableDeepCopy marks a repeated table
(the last two only travel to the interface).

## The sim's Lua and the outside world (read 2026-10-17)

A read of FAF 3839's sim scripts, M28AI, RNGAI, AI-Uveso and the unit packs (items 4 and 5 above). Nothing was
changed; the remedies below are for before multiplayer ships.

What the sim state can read of the machine:
- `GetSystemTimeSecondsOnlyForProfileUse` (`src/sim/sim.cpp`), the only one the scripts use.
- The whole `os` library: `luaopen_io` opens it (`script_state.cpp`, `liolib.c`): clock, time, date, getenv,
  tmpname, execute, exit. Only test tools that never load in a game call it.
- `gcinfo` / `collectgarbage`. Only FAF's benchmark runner calls `collectgarbage()`, and it needs cheats.
- `DiskGetFileInfo().LastWriteTime`. Nothing reads it.

The clock's sites, about 60, are SAFE, GATED or dead:
- SAFE: they only log or measure.
- GATED, off by default: the cheat-only benchmark runner (`lua/sim/Profiler.lua`); NavDebug and the AI grids'
  `Debug`, which need AIs in the game or cheats; M28's profilers, whose `M28Config` constants are all false;
  M28Map's sites, all inside `bDebugMessages`.
- Dead: a commented-out branch in Uveso's `aiattackutilities.lua`.
No AI lets the clock decide how much work it does in a tick or when it yields.

With object keys hashed by serial, "only logs" is weaker than it looks. A clock branch that makes even one table,
closure, thread or userdata in one copy shifts every later serial there. The one such branch found (low risk):
- When a callback takes longer than 5 ms, `SimCallbacks.DoCallback` calls `GetCurrentCommandSourceArmy()`.
- That builds `CommandSourceToArmyMap` on first use (`SimUtils.lua` GetArmyOfCommandSource). Before any chat,
  painting or AbortNavigation has built it, only the slow machine makes that table.
- Every multiplayer game with humans can meet it; AI-only games cannot.
- It splits two machines only together with a walk over object keys that reaches the state, and none was found.

Focus-army allocations break the serial assumption in real multiplayer:
- `ChatUtils.lua` makes `Sync.ChatMessages` only on the machines an allies-only chat goes to, so the serials
  then differ between the teams' machines. There are about 120 sim-side `GetFocusArmy` calls.
- The focus runs above stayed alike because no replay in them had such a chat.
- The check: twocopies `--focus-a` / `--focus-b` on a stream with an allies-only chat.
- Remedies: entity script tables take a serial derived from the entity id (in a range of their own), or
  option (c) above.
- Commander Survival Kit's `UI/campaignconfig_sim.lua` calls `AddBuildRestriction(GetFocusArmy(), ...)` in the
  sim, so each machine restricts only its own player's army: a mod that cannot work in multiplayer as written.

Remedies proposed, not built:
1. In multiplayer sessions, behind `Sim::fixes`:
   - `GetSystemTimeSecondsOnlyForProfileUse` and `os.clock` answer a clock made from the beat: tick x 0.1 + the
     calls in this beat x 1e-6. Budget loops still end, and the slow branch is never taken.
   - `os.time` / `os.date` answer the session's start.
   - `os.getenv`, `execute`, `exit` and `tmpname` are not in the sim state at all.
2. The smallest script fix for the callback case: build the map at `BeginSession`.

tostring() of objects: no script uses one as a key that is walked, sorts or compares by one, or parses an
address back out. Every non-log use is SAFE (the three above, plus `table.hash`, which only gets strings).
`Projectile.lua:982` tests `tostring(number)` for `'#'`; inf / nan are spelled differently by different C
runtimes, which matters only between platforms.

## Orders from other players (designed, not built)

In real lockstep each machine's orders travel to every other machine and are carried out on all of them at
the same later beat, the sender's own included. The original does it in its client manager
(`engine-ref\specs\command_stream.md` 1.1): before beat b, for each client in a fixed order, it reads that
client's messages up to the client's b-th Advance, puts a SetCommandSource in front of them and applies them;
a client that has not yet sent beat b holds the beat up. A client's messages are exactly our command stream.

What we have already covers the encoding. `CommandWriter` (`src/sim/command_writer.h`) writes an order as the
stream's bytes, with the source in the command id; `ApplyCommands` and `CommandStream` read them. The
interface host records every game that way (`--replay-out`) and moho64 plays it back headless
(`--session --seed --stream`); the runs above show two copies read those streams alike, and the interface's
own log agreed with the playback earlier (FINDINGS 91: 65 of 65 sound lines). `src/sim/replay.*` only reads
headers; it adds nothing here.

What is not covered: several clients in one beat (the order they are applied in), the input delay (orders
landing some beats later everywhere), command ids of a second source, and the permission checks with two
human armies. To build it into the check: moho64 would get `--client-out <file>` (this copy's orders as a
client's stream: first as many Advances as the delay, then each beat's batch and an Advance) and
`--client-in <file>` (another copy's, read as it grows; before beat b, wait until it holds b Advances),
applying the clients in source order, with the CommandWriter on this copy's source. Only the beat loop of
`src/app/main.cpp` and a reader that follows a growing file would change; the sim would not. twocopies.py
would start copy A as owner 0 and copy B as owner 1 of a session with two human armies (OwnerID 0 and 1 give
sources 0 and 1, `src/sim/session.cpp`), each with its own orders file. The interface's multiplayer host will
need the same merge rule, so it is best written once, there, and the check pointed at it. The fingerprint's
total is also what a multiplayer game would exchange to catch a desync (the original sends an MD5 of each
beat, the VerifyChecksum message, which our stream reads and skips).

## Limits

- One machine, one build, one system. A Linux or Mac build can be compared later with `--compare` on two
  fingerprint files, as the lines do not depend on the machine.
- A difference that stays inside Lua (a table walked in another order, nothing done with it yet) shows only
  when it reaches the state; the `rng` part (every Lua Random is a draw) is usually the first to show it.
- Address-dependent behaviour shows only in the runs where the two copies' addresses happen to give
  different orders: more runs, more confidence.
- Not hashed: the terrain's heights (flattening), the intel grids, the threat maps, the contents of Lua
  tables. A difference there shows when it reaches a unit.
- When the simulation runs on several threads, the two copies should also differ in their number of threads
  and in timing (a setting per copy, like the shift), so that a race shows up as a difference.
