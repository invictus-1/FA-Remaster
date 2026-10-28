# Our own AI: architecture and build plan

Engine docs. Written 2026-10-28 by the engine chat's helper "aicore", from Chris's goals and the design talk's
decisions ([ai-design-goals.md](ai-design-goals.md)) and the study of how M28 plays ([ai-study-m28.md](ai-study-m28.md),
section 7). This page says how FA Remastered's own AI is built: its layers, which parts are native C++ and which
are Lua, how it plugs into the simulation, how it stays deterministic, how it is tested, and the order of the work.
Points not settled yet are marked **OPEN**.

## 0. Licence and origin

- Our AI is our own design and our own code, MIT like the engine. Nothing of M28's code goes into it, and no
  function of ours follows one of M28's line by line. M28 (CC BY-NC-SA 4.0, maudlin27) was read for ideas only:
  what works and why, never how its code does it. See the lab's `CLAUDE.md`, "M28AI's license".
- The general techniques used here (zones, threat maps, Lanchester-style estimates, build orders, kiting, focus
  fire, flow fields) are common RTS-AI knowledge.
- M28 (our variant) stays in the lobby as an option and as the sparring partner in tests. It is never bundled into
  the AI's folder or linked from its code.

## 1. The picture in one paragraph

The AI is one brain per army, written in Lua, that asks the engine small questions and gets small, ready answers.
The engine keeps, natively, three things the Lua cannot afford to recompute: the **map** cut into zones (made once
at load), an **index** of what each army knows of every unit, by zone, domain and kind (kept up to date as the
game runs), and a **battle estimate** that says who wins a fight and how clearly. On top of those the brain runs
its layers: a **strategic layer** that looks at the whole front every few seconds and sets aggression and
objectives, an **operation layer** that turns objectives into operations on zones (hold, take, raid, crack a
fortified line), **group control** that moves and fights the units of each operation, and **economy and building**
that pays for it all. Team play, pings, difficulty, personalities, the teacher mode and the announcements are
settings and filters on those layers, not separate AIs.

```
 lobby key "remastered_<level>" + personality
        |
 Lua brain (data/ai/...)   strategic layer  -> aggression, objectives        every 3-5 s
                           operation layer  -> operations on zones          every 1 s (staggered by army)
                           group control    -> orders, micro                 every 2-10 ticks, by attention budget
                           economy/building -> build jobs in build power     every 1 s
                           team / pings / announcements (allies only)
        |  Remastered.AI* calls (small answers, no big tables per tick)
 native core (src/ai/)     map analysis (once) | unit / threat index (per beat) | battle estimate (on demand)
        |  reads only
 simulation                terrain, path caps, deposits, units, blips, intel grids, blueprints
```

## 2. The layers

### 2.1 Strategic layer (the fix for "500 Percivals afraid of a T1 tank")

Runs every 3-5 seconds of game time per brain (team brains share one result, 2.5).

- **Whole-front assessment, per domain.** For land, navy and air separately: our strength against the strength of
  the enemy forces that can actually reach that domain (the index's "can reach" answers, 3.2). An enemy navy in a
  pond with no shore near our land zones adds nothing to the land picture. Inputs: army strength (battle-estimate
  strength, not summed mass, 3.3), production rate (mass and build power spent on units per minute), eco lead,
  tech lead, map control (share of mass zones held), unit-cap use.
- **Aggression level**, a number from 0 (turtle) to 4 (decisive push), with hysteresis: it rises when the
  advantage has held for a while and falls only when the expected trade turns bad, not at the first threat jump.
  - 0 hold and defend; 1 hold ground and raid; 2 take the enemy's expansions; 3 attack a main target; 4 decisive
    push with everything and reinforcements streaming in, until the advantage is gone.
  - Far ahead or at the unit cap pushes the level up even if production is equal.
  - Personality and difficulty move the thresholds (2.6, 2.7); the teacher mode caps it (2.8).
- **Objectives**: one or two at a time, each a zone or a group of zones with a reason: an expansion to take, a
  firebase to crack, the enemy's main base, a naval control zone, an enemy experimental to kill. Picked by value
  (mass, enemy production, strategic weight such as a chokepoint) over cost (the battle estimate there, the travel
  distance).
- **Domain shifts**: winning the sea moves production into land and air (goal 1); losing the air sky asks for AA
  and fighters, it does not turn armies home (study 7.2.6).
- **"Too strong to attack head-on"** (goal 4): when every objective's estimate is bad, the level drops to 0-1 and the
  layer chooses a long plan: turtle and build up, plus one or more of nukes, hit-and-run on shields, a staged breach
  (2.2), or game-ender structures. **OPEN:** how long it waits before such a plan, per personality.

### 2.2 Operation layer (zones and operations)

Runs every second per brain, staggered so that brains do not all think on the same beat.

- **The unit of thought is the zone** (section 3.1). Each zone has a state for us: ours, contested, enemy's,
  unknown; a value; and the operations that touch it.
- **An operation** is a goal on one or more zones with the forces given to it: *hold* (defend a zone or base),
  *take* (an expansion), *raid* (fast units at exposed mass away from the main fight), *escort* (engineers to an
  expansion), *push* (a main attack along a path of zones), *crack* (a fortified line, below), *naval control* (a
  pond), *air superiority* (a sky region), *snipe* (ACU or a key building).
- **Gathering before going.** An operation gathers its forces at a staging zone on its path and leaves when the
  battle estimate for its target says "we win clearly" (or when the strategic layer orders a push). Units are not
  sent one by one into a fight (study section 4, reason 1).
- **Combined arms** (goal 2): an operation can hold forces of several domains with a timing: land and navy arrive
  together, air follows; artillery, ships and bombers are given as *support fire* to a push and aim at what blocks
  it (shields first, then point defence).
- **Cracking a fortified line** (goal 3), a staged operation:
  1. scout the line (what defences, what range, which shields);
  2. with navy: long-range ships poke from outside the defences' range; heavy ships break shields and point
     defence; without navy: artillery and a large mass of units keep pressure on;
  3. a breach: sacrificial units (Bricks, an experimental) take the shields' fire, T3 bombers take key points, then
     the main force pushes in;
  4. each stage starts when the previous one's estimate says it has done enough (shields down, defences under a
     share of their strength), not on a timer.
- **Retreat with sense.** An operation retreats only when the battle estimate of the fight it is in says it loses,
  and by a safe path of zones to the nearest held zone, not to the home base. A force that clearly outclasses what
  it meets never retreats from it (goal 5); kiting is for even fights only.

### 2.3 Group control (micro)

- Each operation's forces are one or more groups. A group gets orders when its situation changes, and every few
  ticks the brain looks at the groups its **attention budget** allows (2.6).
- Orders are remembered: the same order is not sent again (no stutter), and a micro order (a dodge, a kite step)
  locks the group's normal order for its length.
- Skills, each on or off by difficulty: attack-move versus formation move, focus fire (targets that die fastest per
  value, spread to avoid overkill, kept while the group holds), kiting in even fights, bomber stacking for one-pass
  kills, pulling back damaged units, overcharge use, shield and AA escorts kept with the army.
- **Native help later (phase 6):** focus-fire assignment and kite steps for large groups, which are per-unit loops.

### 2.4 Economy and building

- **Jobs measured in build power**, not in engineer heads: each zone and each operation publishes jobs (mass, power,
  factories, defences, shields, experimentals, reclaim) with the build power they want and a priority. Engineers
  take the highest unmet jobs; unmet demand asks factories for engineers. (The idea is common; our job list and
  its order are our own.)
- **Openings per map size and mass layout**, made from the map analysis (mass zones near the start, first
  expansion zones by travel distance), not hand-written per map.
- **Upgrades** (extractors, factories, ACU) when the zone is safe and the income allows; "safe" reads the index.
- **Production follows the operations:** what the strategic and operation layers ask for (AA share against seen
  air, artillery against firebases, navy when a pond is worth it), read from blueprint data (roles, ranges, DPS),
  not from per-unit special cases, so mod units and our remastered units work without code changes.
- **Build spots** from the map analysis (3.1) plus the live structure occupancy (the engine's own grids).

### 2.5 Team play and pings

- **One team picture.** Allied brains of ours share the index's view of the team (the army's own plus its allies'
  intel, as a player sees through allied vision) and one strategic result per team. Each brain keeps its own economy
  and operations; the team layer splits the front (who holds which zones, who supports whom) and evens out
  resources by the game's sharing options.
- **Human allies count.** A human ally's units are in the team picture; the AI defends an ally under attack as it
  defends its own zones.
- **Pings** (decision 5 and 6): the brain reads each allied ping (type, place) through FAF's ping callback in the
  sim.
  - A ping on nothing, far from any unit and zone of interest, or repeated in quick succession by accident is
    ignored.
  - *Assist / Defend* at an ally's base or force with enemies near: a *hold* operation there, sized by the battle
    estimate of the threat.
  - *Attack* on an enemy area: no suicide rush. The brain looks at what is known there, picks a sensible target in
    that area and gives an operation there a share of its forces that the estimate says is enough; if nothing is
    enough, it says so to its allies (2.9) and scouts or prepares.
  - Detailed pings from the radial menu (Attack, Assist, Defend, Attack ACU, Experimental here, Expand here,
    Retreat, ...) map to operation types. **OPEN (ui chat):** the ping types and how they reach the sim (the menu is
    the interface's; the sim needs only the type name and the place in the ping table).

### 2.6 Difficulty knobs (Recruit, Veteran, Commander, Supreme Commander)

All levels have fair intel. Resource cheats and map vision are separate, named lobby options.

| Knob | What it changes | Recruit .. Supreme Commander (first guesses, tuned in tests) |
|---|---|---|
| Reaction delay | Ticks between the index showing a change and the brain acting on it (the brain reads the index as it was that many ticks ago: a ring of snapshots, 4.3) | 40 .. 3 |
| Attention budget | Groups and zones the brain may give new orders to per second (like a player's actions per minute) | 2 .. 30 |
| Micro | Which group-control skills are on (2.3) | none .. all |
| Mistakes | A chance per decision of a human-looking error: over-extending, forgetting AA, sending a raid into point defence, a late expansion. Drawn from the AI's own deterministic stream (4.2) | often .. rare |
| Aggression | Offsets on the strategic layer's thresholds (2.1) | late and telegraphed .. pressure from minute one |
| Economy tightness | Idle engineers allowed, floating allowed, opening precision | loose .. tight |

### 2.7 Personalities

On top of any difficulty: *rusher* (early aggression, land T1/T2), *turtle* (defences first, late game, game
enders), *air player*, *naval player*, *balanced*, *random* (one of them, drawn at start from the AI's stream). A
personality is a table of weights: production shares per domain, aggression offsets, preferred operation types,
favourite openings. No personality has its own code path.

### 2.8 Teacher mode (lobby option)

When far ahead of a human enemy, the brain caps its aggression level at 2 and its attention budget at half,
announces nothing, and keeps playing well. It never throws a game outright; it eases off so the player can recover
and learn. **OPEN:** whether it also eases off against a human player in a team game, or only in 1vN games with
humans on one side.

### 2.9 Announcements, to allies only

The strategic and operation layers say what they do in allied chat ("Taking the east expansion", "Pushing mid with
navy support in 30 s", "I can't break that line yet, building artillery"). Only allies receive them; an enemy AI
never announces. The text comes from the operations, so it says what the AI will really do. **OPEN:** chat in the
sim goes through FAF's Sync (the sim cannot send chat directly): which existing path the ui chat prefers.

## 3. Native C++ and Lua

**The rule:** C++ for what is heavy and data-shaped (whole-map work, per-unit loops, per-zone sums, estimates);
Lua for decisions while they settle (strategy, operations, personalities, special mod rules, chat). When a Lua part
has settled and shows in the profiler, its hot core moves native. The native core lives in `src/ai/` and touches the
sim only through read-only accessors, except for what it owns itself (its own state on the `Sim`).

### 3.1 Map analysis (phase 1, built)

Made once per game, the first time our AI asks, or by the debug dump at the end of `Sim::Start`. It needs the
resource deposits, which FAF's `BeginSession` makes (`CreateResources`), so it must not be asked from `OnCreateAI`:
that runs earlier, inside `OnCreateArmyBrain`, before any deposit exists. From phase 3 the engine makes it at the end
of `Sim::Start` (after `BeginSession`, still in the loading screen, before beat 1) when an army of the session has
one of our keys, and the brains' think threads read it from their first wake. It reads only the terrain, the engine's
passability rule (`Navigation::MobileCaps`, the original's OCCUPY_MobileCheck), the named footprints and the
resource deposits; it writes nothing of the sim's. Deterministic, single-threaded today (about 0.1-0.3 s on a 20 km
map; split over the worker pool later if it grows, with results written per item).

- **Layers and components.** For each domain (land, naval, hover, amphibious) the cells the domain's standard 1x1
  footprint can stand on (`Vehicle1x1`, `Water1x1`, `Hover1x1`, `Amphibious1x1` from the game's footprints; built-in
  values when a mod has none of those names), on a grid of 4 x 4 world units ("segments", 256 x 256 on a 20 km
  map). A segment counts for a domain when at least half of its cells pass. Connected areas per domain get an id:
  **islands** (land), **ponds** (naval), **plateaus** (hover, amphibious).
- **Zones.** Land zones are seeded on groups of mass points (deposits within about 30 world units on one island form
  one group) and grown over the island's segments by travel distance, every segment to the nearest seed within a
  radius; what no mass zone reaches gets plain zones seeded far from the others. Water zones are seeded on a spacing
  over each pond and grown the same way. Every zone has: its domain, island or pond, centre (the segment nearest
  its mean), size, mass points and hydro points (with positions), and the start positions in it.
- **Adjacency.** Two zones are neighbours when their segments touch (8-neighbour) inside one domain area; a land
  zone and a water zone are coast neighbours when they touch at the shore (hover and amphibious paths use those).
  Each adjacency keeps its **border width** (how many segments touch) and the midpoint of the border.
- **Travel distances per domain.** Between neighbours: the segment-grid distance from one centre to the other
  through the two zones (8-neighbour, straight steps 4, diagonal 4 x 1.4142). Between any two zones: the shortest
  path over the zone graph (Dijkstra from every zone; a few hundred zones make this a few milliseconds). -1 when a
  domain cannot get from one to the other: this is the "can it reach" answer the index and the strategic layer use.
- **Chokepoints.** An adjacency is a chokepoint when its border is narrow (at most 6 segments, about 24 world units:
  **OPEN**, tune on more maps) and it links two zones that have no other short way round (the detour without it is
  at least 1.5 times the direct distance, or there is none). A zone whose removal splits its island's graph is
  marked as a **cut zone**.
- **Build spots.** Per zone, the segments where a structure of 2, 4, 8 or 16 world units fits on the terrain: every
  cell a land footprint can stand on, height range at most 1 world unit (the default MaxGroundVariation), not on a
  deposit. The largest size per segment is kept, so "a 4 x 4 spot near power in zone 12" is a list read. Live
  structures and props are not in it (they change during the game): the build query of phase 3 checks those with
  the engine's own occupancy grids.
- **Caching per map (OPEN, phase 1b):** the result depends only on the map file, the footprints and the deposits, so
  it can be written next to the user's prefs keyed by a hash of those and read back. Today it is computed every
  game; only worth it if a big map takes more than a fraction of a second.

### 3.2 Unit / threat index (phase 2)

Kept up to date by the engine at a fixed point of the beat (after recon, before the script threads run: the same
point every beat, in one order), for **every army that has one of our brains** (the cost is zero in a game
without our AI).

- **What it holds, per observing army (or per team):** for each zone, sums by owner side (ours, allied, enemy),
  domain (land, naval, air, sub), kind (direct fire, indirect fire, anti-air, anti-sub, shield, structure, economy,
  engineer, commander, experimental) and range band (short < 30, medium 30-60, long 60-150, strategic > 150 world
  units); the strongest single unit; counts. Plus per zone "what can hit this zone from outside it" (long-range units
  and structures whose range covers it).
- **Per domain "can it reach".** An enemy unit adds to a zone's threat for a domain only if it can reach that zone or
  shoot into it: a ship counts against land zones only when the land zone is a coast neighbour of its pond within
  the ship's range; a land unit counts against a water zone only when it is on the shore with range over it; air
  reaches everything. This is what keeps a stranded navy from scaring the land army.
- **Fair intel.** The index for army A is built only from what A may know: its own and allied units; enemy units
  through A's blips (`Unit::recon` flags, intel.cpp). A unit seen in line of sight (now or ever, LOSEver) counts with
  its blueprint; a radar or sonar blip that was never seen counts as "unknown" with the domain the blip's layer
  tells and an estimate of its strength from what A has seen of that enemy so far; a blip known to be fake is left
  out. Positions are the blip's (jammed blips stay where the jammer puts them). Lost contacts fade as the engine's
  threat map does (study 2.4 by contrast: no exact positions of unseen units). The cheat option "AI vision" makes
  the index read every unit, as omni would.
- **Answers to Lua:** `Remastered.AIZoneThreat(brain, zone, domain, kind, band)`, `AIZoneSummary(brain, zone)`
  (one small table), `AIZonesWhere(brain, query)` (zone lists for "contested land zones", "enemy mass zones we can
  reach by land" ...), `AIReachable(zoneA, zoneB, domain)`.
- **Reaction delay** (2.6) is a ring of per-zone summaries, one every N ticks: a slow brain reads an older one. Only
  the summaries are kept, not the units, so the memory is small.
- **Cost target:** under 0.3 ms per beat for 8 armies and 3,000 units (per-unit work is a zone lookup in an array and
  a few adds; recomputed in full every 10 beats, or kept incrementally if the profiler says so).

### 3.3 Battle estimate (phase 2 for the formula, phase 5 for the small fight)

- **Strength.** Each unit's combat value from its blueprint: damage per second against each domain (from its
  weapons' damage, rate of fire and number of shots), effective health (health plus shield, times armour), range,
  speed. A group's strength is square-law style: for ranged units the fighting value of N units grows with about N
  squared (Lanchester), so 20 tanks beat 10 by far more than twice. Shields and point defence count for their side;
  units that cannot hit the other side's domain count zero for that fight.
- **Who is in range.** Range and speed give how long the shorter-ranged side takes to close in, and how much damage
  it takes on the way; that is folded into the estimate. Reinforcements on the way count with their arrival time.
- **Answer:** a ratio (our winning margin, 0.5 = even), the expected losses of each side, and the expected time.
  `Remastered.AIBattleEstimate(brain, ourGroup or zone, theirZone or units)`.
- **The small fight simulation (phase 5)** for the few fights that matter each second (an objective, a push, a
  breach stage): both sides as lists of units with blueprint DPS, health, range and speed, stepped in half-second
  steps for up to 30 s with simple target choice, in native code, deterministic. Runs on the worker pool when there
  are several, one fight per task, results committed in the fights' order.

### 3.4 What stays Lua (at first)

The brain class, the strategic and operation layers, group orders, the economy's job list, personalities, the
difficulty table, team coordination, pings, announcements, and anything specific to a mod. All of it calls the
native core for numbers and spends its own time only on decisions.

## 4. How it plugs into the simulation

### 4.1 The brain type in the lobby

- FAF picks a brain by the army's `AIPersonality` key: `OnCreateArmyBrain` in `/lua/simInit.lua` looks the key up in
  `keyToBrain` (`/lua/aibrains/index.lua`) and sets the brain's metatable to that class; the engine then calls
  `brain:OnCreateAI(planName)` for a non-human army. The lobby's list comes from `/lua/ui/lobby/aitypes.lua` plus every
  `lua/AI/CustomAIs_v2/*.lua` file of the active mods (each returns a table with `AIList` and `CheatAIList` of
  `{key, name}`).
- **Ours:** the keys `remastered_recruit`, `remastered_veteran`, `remastered_commander`, `remastered_supreme` (and
  `..._cheat` variants for the resource-cheat option), with the personality as a separate lobby option per AI slot
  (**OPEN, ui chat:** a per-slot option, or the personality folded into the key as FAF's AIs do).
- **Where our Lua lives:** `fa-remaster/data/ai/` in the repository, laid out like a mod: `hook/lua/aibrains/index.lua`
  (adds our keys to `keyToBrain`), `lua/AI/CustomAIs_v2/remastered.lua` (the lobby list), `lua/remastered/ai/*.lua`
  (the brain and its layers). The engine mounts it **only when an army of the session has one of our keys** (Sim::Start
  reads the armies' options before the sim state is made). So a game without our AI loads exactly the Lua it loads
  today: no gate replay or session can change. A replay of a game with our AI has the keys in its armies' options
  and mounts it again. **OPEN:** whether a release also ships it as a real mod folder for FAF's own client (only if
  FAF compatibility ever matters; the ship model says it does not).
- **The brain class** derives from FAF's plain army brain (`/lua/aibrain.lua` AIBrain, what human armies use), not
  from `/lua/aibrains/base-ai.lua`, so none of FAF's builder managers run.

### 4.2 The Lua API

- A table of C functions in the `Remastered` global (script/fastlib.h; made only in sessions with the engine's script
  fixes, which every new game has), named `Remastered.AI*`. Each returns a number, a short list, or one small table.
  Nothing returns a table per unit per tick. Zone and island ids are 1-based small integers.
- **Phase 1 (built):** `AIMapInfo()`, `AIZoneAt(x, z, domain)`, `AIZone(i)`, `AIZoneNeighbours(i)`,
  `AIZoneDistance(a, b, domain)`, `AIChokepoints()`, `AIBuildSpots(zone, size)` (exact form in
  [src/ai/map_analysis.h](../src/ai/map_analysis.h)).
- **Randomness:** the AI never calls the sim's `Random` for its own choices (that stream is shared with the game and
  the original's replays compare its draws). It gets its own MT19937 per brain, seeded from the session seed and the
  army index (`Remastered.AIRandom(brain)`, phase 3): same draws on every machine, and the game's stream is untouched.
- **When the brain thinks:** a script thread per brain (`ForkThread` from `OnCreateAI`), waking on fixed ticks
  (layer periods in 2.1-2.3) offset by army index, so the AI's cost is spread over beats.

### 4.3 Where the native work runs in the beat

- Map analysis: once, at the end of `Sim::Start` when our AI is in the session (phase 3), else on the first call.
- Index: one fixed stage each beat, after the recon tick and before the script threads (phase 2 picks the exact
  point and documents it in `engine-ref/specs/beat_order.md`'s style), only when a brain of ours exists.
- Battle estimates and fight simulations: inside the Lua call that asks (serial), or as one batch stage for the
  queued fights of the beat (parallel, phase 5).

## 5. Determinism (the AI is part of the simulation)

Every machine in a game runs the AI and must get the same orders, at 1 thread and at all cores (the gate's
both-modes check, multicore.md 1).

1. **No wall clock** in any decision: no `os.clock`, no `GetSystemTimeSeconds`, no timing-based budgets. The
   attention budget is counted in decisions per game tick, never in milliseconds.
2. **No unordered iteration feeding a decision.** In C++: no `unordered_map` walked; everything in vectors by zone id,
   army index, entity id or creation order; sorts with a full tie-break (id last). In Lua: no `pairs()` over tables
   keyed by objects or by unit ids for anything that decides (two-copies.md, "pairs() over keys that are objects");
   arrays and sorted id lists.
3. **Floating point**: the engine's flags (`-ffp-contract=off -fno-fast-math`), the sim's FP state (core/fpenv.h) on
   every worker; sums in a fixed order (per zone in id order, never per thread and then merged in finish order).
4. **Parallel work** follows multicore.md: tasks read only what nothing writes during the stage, write their own
   slot, results committed in a fixed order, no Lua in a task. `MOHO64_PAR_CHECK=shuffle / reverse` must give the
   same fingerprints.
5. **Own random stream** (4.2), never the sim's.
6. **No environment switch changes a decision.** Debug switches (`MOHO64_AI_MAPDUMP`, later ones) only write files.
7. **Fair intel is also a determinism rule:** the index reads only sim state that is the same on every machine
   (blips, recon flags), never what the local player's interface shows.
8. **Proof at every phase:** the 16 gate replays and the B1 / b8 sessions identical to main (they do not use our AI),
   plus our own AI sessions identical at 1 thread, 32 threads, shuffle and reverse, and a two-copies run.

## 6. Testing

- **Headless matches on SETONS_R** (`moho64 --session <table> --seed <n> --maps visuals/maps --ticks <n>`), session
  tables made like the B1 / b8 ones (a lobby's `GameConfig` table with `GameOptions` and `PlayerOptions`; the b8 table
  is the renderer's 8-army M28 config). Ours against M28 (the variant), ours against FAF's adaptive AI, ours against
  ours (mirror), 1v1 and 2v2 / 4v4, fixed seeds, several seeds per pairing.
- **Scores**, written by a small sim-side logger of our brain (`MOHO64_AI_LOG=<file>`, a debug switch that only writes):
  - wins / losses / time to win, mass killed against mass lost;
  - **"never fled a weaker force"**: every retreat logged with the battle estimate at that moment; a retreat with
    our margin above 0.7 counts as a fault;
  - **"pushes when ahead"**: time spent at aggression 3-4 while the army-strength lead is above 1.5x; time the lead
    was above 2x without an attack;
  - use of combined arms (operations with more than one domain), breaches started and finished;
  - **cost per tick**: the native part and the Lua part (`MOHO64_PROFILE` phases), against M28's in the same game.
- **Determinism runs** at each phase (5.8).
- **Then Chris plays it** and judges the feel (decision 8), in the one big check after the UEF polish unless he asks
  sooner.

## 7. Phased plan

| Phase | Delivers | Tested by |
|---|---|---|
| 1 | Map analysis native (`src/ai/`), its Lua calls, the `MOHO64_AI_MAPDUMP` dump; computed only on request | The 16 + B1 / b8 identical to main; SETONS_R's dump looked at (zones around the mass, ponds, chokepoints at the passes); time to compute |
| 2 | Unit / threat index per observing army with fair intel and "can it reach"; the battle-estimate formula; their Lua calls | Unit tests from scripted sessions (a navy in a closed pond does not count for land; a radar blip counts as unknown); index cost per beat on b8-sized games; identical runs at 1 / 32 threads |
| 3 | The brain skeleton in `data/ai/`: lobby keys, mount-on-demand, OnCreateAI, own random stream, think loops; economy and building (opening from the map, build-power jobs, expansion) and simple defence | Our AI vs FAF's adaptive on SETONS_R: it expands, stays out of stalls, defends; it does not crash for 60 minutes; Lua + native cost per tick |
| 4 | Strategic and operation layers: aggression with hysteresis, objectives, gathering, push, retreat by estimate, raids, navy and air domains, combined arms timing | Ours vs M28 and FAF's AI, several seeds: the fight scores of section 6; "never fled a weaker force" at zero faults |
| 5 | Cracking fortified lines and staged breaches; the small fight simulation; group micro (focus fire, kiting in even fights); difficulty knobs and the four levels | Scripted fortified-line scenarios (a session that starts with T2 PD + artillery lines); level-vs-level matches (each level beats the one below most of the time) |
| 6 | Team play, pings (with the ui's radial menu), allied announcements, personalities, teacher mode; hot Lua parts moved native | 2v2 / 4v4 with a human-ally stand-in (scripted pings); Chris's playtest |

## 8. Open points (collected)

1. Personality as a per-slot lobby option or folded into the lobby key (ui chat).
2. Ping types and their path from the radial menu into the sim's ping table (ui chat).
3. Allied announcements: which chat path from the sim (ui chat).
4. Teacher mode in team games.
5. Map-analysis caching per map, if big maps need it.
6. Chokepoint width threshold and the plain-zone spacing, to tune on more maps than SETONS_R.
7. How long "too strong to attack" waits before a long plan, per personality.
8. Whether `data/ai/` also ships as a FAF-style mod folder.
9. Where exactly the index stage sits in the beat (phase 2 reads the beat order and decides).
