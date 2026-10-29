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
- **Pings** (decision 5 and 6; the ui's answer in [ui-answers.md](ui-answers.md), 2026-10-28): a ping is FAF's
  sim callback `SpawnPing` (`/lua/SimPing.lua`), in the command stream and the replay like any order. It calls every
  allied brain's `DoPingCallbacks(data)`, and `DoAIPing(data)` when the pinger is not an AI. The radial menu keeps
  FAF's `Type` (for the look) and adds two fields: `Intent` = 'attack' | 'assist' | 'defend' | 'attack_acu' |
  'experimental' | 'expand' | 'retreat', and `Target` = the entity id under the cursor when the key went down.
  - **The sim side checks `Target`**: callback arguments come from a player's machine. A Target that is no entity, or
    one the pinger's army cannot see now, is dropped and the ping stays a place. A ping without `Intent` (FAF's own
    four, an old replay) means what it means today.
  - A ping on nothing, far from any unit and zone of interest, or repeated in quick succession by accident is
    ignored.
  - *Assist / Defend* at an ally's base or force with enemies near: a *hold* operation there, sized by the battle
    estimate of the threat.
  - *Attack* on an enemy area: no suicide rush. The brain looks at what is known there, picks a sensible target in
    that area and gives an operation there a share of its forces that the estimate says is enough; if nothing is
    enough, it says so to its allies (2.9) and scouts or prepares.
  - Each `Intent` maps to an operation type: attack / attack_acu -> *push* or *snipe* on the target, assist /
    defend -> *hold* at the place or the ally's force, experimental -> the experimental joins an operation there,
    expand -> *take* that zone, retreat -> the operations near the place fall back.

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

The personality is a per-slot lobby option of ours beside the AI key (the ui's answer): `RemasterPersonality` in the
slot's PlayerOptions, which reach the sim as `ScenarioInfo.ArmySetup[army]`. Its default is Random, drawn in the
sim from the AI's own stream (4.2), so it is the same on every machine and in the replay.

### 2.8 Teacher mode (lobby option)

A per-slot option like the personality: `RemasterTeacher` in the slot's PlayerOptions (`ScenarioInfo.ArmySetup`).
When far ahead of a human enemy, the brain caps its aggression level at 2 and its attention budget at half,
announces nothing, and keeps playing well. It never throws a game outright; it eases off so the player can recover
and learn. **OPEN:** whether it also eases off against a human player in a team game, or only in 1vN games with
humans on one side.

### 2.9 Announcements, to allies only

The strategic and operation layers say what they do in allied chat ("Taking the east expansion", "Pushing mid with
navy support in 30 s", "I can't break that line yet, building artillery"). Only allies receive them; an enemy AI
never announces. The text comes from the operations, so it says what the AI will really do.

The path (the ui's answer): FAF's `AIChatBrainComponent` (`/lua/aibrains/components/ChatBrainComponent.lua`),
`brain:SendChatToAllies(text, args, location)`. It writes to `Sync.ChatMessages`, and every player's interface shows
the line only to the AI's allies. `args` are formatted on the receiving side (`<LOC>` texts in each player's
language), and `location` (a position or an area) gives the line FAF's camera icon, so "attacking here" can be
clicked. Our brain class takes that component in.

## 3. Native C++ and Lua

**The rule:** C++ for what is heavy and data-shaped (whole-map work, per-unit loops, per-zone sums, estimates);
Lua for decisions while they settle (strategy, operations, personalities, special mod rules, chat). When a Lua part
has settled and shows in the profiler, its hot core moves native. The native core lives in `src/ai/` and touches the
sim only through read-only accessors, except for what it owns itself (its own state on the `Sim`).

### 3.1 Map analysis (phase 1, built: `src/ai/map_analysis.*`)

Made once per game, and only in a session with our own AI: at the end of `Sim::Start` (after `BeginSession`, still in
the loading screen, before beat 1) when an army's `AIPersonality` starts with `remastered`, or at the first
`Remastered.AI*` call. `MOHO64_AI_MAPDUMP=<file>` forces it in any session for a look (4.4). It needs the resource
deposits, which FAF's `BeginSession` makes (`CreateResources`). So it must not be asked from `OnCreateAI`, which runs
earlier, inside `OnCreateArmyBrain`, before any deposit exists. The brains read it from their think threads' first
wake. It reads only the terrain, the engine's passability rule (`Navigation::MobileCaps`, the original's
OCCUPY_MobileCheck), the named footprints, the resource deposits and the armies' start positions, and writes nothing
of the sim's. (At the end of `Sim::Start` the navigation exists already in the sessions tried; if it did not,
`MobileCaps` would make it and read the terrain types from Lua, which the dump's log line warns of.) Single-threaded,
deterministic: 250-300 ms on SETONS_R (20 km), the same zones at 1 and 32 threads. It could be split over the worker
pool later, with results written per item.

**SETONS_R (2026-10-28):** 108 land zones (67 seeded by mass) and 54 water zones, 400 links, 18 chokepoints, 11 cut
zones. All 108 mass and 8 hydro points are in a zone, and each of the 8 starts has its own mass zone. All 8 starts
are on one island. The two nearest starts are 198 world units apart by land. Some pairs are much shorter by hover
across the water than by land (army 2 to army 3: 789 by land, 603 by hover). Section 4.4 has the dump and its
picture.

- **Layers and components.** For each domain (land, naval, hover, amphibious) the cells the domain's standard 1x1
  footprint can stand on (`Vehicle1x1`, `Water1x1`, `WaterLand1x1` for hover, `Amphibious1x1` from the game's
  footprints; built-in copies of the stock values when a mod has none of those names), on a grid of 4 x 4 world units ("segments", 256 x 256 on a 20 km
  map). A segment counts for a domain when at least half of its cells pass. Connected areas per domain get an id:
  **islands** (land), **ponds** (naval), **plateaus** (hover, amphibious).
- **Zones.**
  - **Mass zones.** Land zones are seeded on groups of mass points: deposits within 30 world units of each other on
    one island form one group. Each zone grows over the island's segments by travel distance: every segment goes to
    the nearest seed within 56 world units.
  - **Plain land zones** cover what no mass zone reaches. Each is seeded at the unzoned segment farthest from every
    zone (ties: the lowest index) and grows over unzoned segments within 64. Islands under 8 segments without mass
    get none.
  - **Water zones** are made the same way over the ponds (radius 96; ponds under 16 segments get none).
  - **Slivers are merged.** A plain zone under 40 segments (water: 80) joins its same-domain neighbour with the
    longest border. The bounded growth leaves thin leftovers between zones, and without the merge they made false
    chokepoints.
  - **Segments only hover or amphibious units reach** (shallows, seabed, tiny islands) join the nearest zone through
    such segments.
  - Every zone has: its domain, island or pond, centre (its segment nearest the mean of its segments), size, mass
    and hydro points (with positions), the start positions in it (civilian armies left out), and one anchor segment
    per domain (where that domain enters it).
- **Adjacency.** Two zones are neighbours when their segments touch (4-neighbour). A land zone and a water zone are
  coast neighbours. Each adjacency keeps its **border width** (segment pairs on the border, x 4 world units in Lua)
  and the midpoint of the border.
- **Travel distances per domain.**
  - Between neighbours: the segment-grid distance from one zone's anchor to the other's, through the two zones only.
    Moves are 8-neighbour, straight steps 4 and diagonal steps 4 x 1.4142, and a diagonal step needs both straight
    steps beside it.
  - Between any two zones: the shortest path over the zone graph (Dijkstra from every zone).
  - -1 when a domain cannot get from one zone to the other. This is the "can it reach" answer that the index and the
    strategic layer use.
- **Chokepoints.** An adjacency between two zones of one domain is a chokepoint when two things hold. Its border is
  narrow: at most 6 segment pairs, about 24 world units (**OPEN**, tune on more maps). And there is no short way
  round: without it, the detour in the zones' own domain is at least 1.5 times the direct distance, or there is
  none. A land zone whose removal leaves its land neighbours in more than one piece is a **cut zone**.
- **Build spots.** Per land zone, the segments where a square of 2, 4, 8 or 16 world units, cornered at the segment's
  corner, fits on the terrain. Every cell must be one a land footprint stands on, the height range at most 1 world
  unit (the default MaxGroundVariation), and no deposit cell in it. The largest size per segment is kept, so "a
  4 x 4 spot in zone 12" is a list read. Live structures and props are not in it, because they change during the
  game: phase 3's build query checks those with the engine's own occupancy grids.
- **Caching per map (OPEN, phase 1b):** the result depends only on the map file, the footprints and the deposits, so
  it can be written next to the user's prefs keyed by a hash of those and read back. Today it is computed every
  game; only worth it if a big map takes more than a fraction of a second.

### 3.2 Unit / threat index (phase 2, built: `src/ai/unit_index.*`)

**When it runs.** The engine keeps it for **every army that plays our AI**, and only for those; a game without our AI
never builds it. It is rebuilt every 5 beats (`kIndexPeriod`) in a stage of its own, just before the script threads
(`Sim::Tick`, before `threads_->RunTick`). There it reads only last beat's committed state: the units, their blips,
their blueprints' ratings (3.3) and the zones (3.1). A query before the first build builds it at once.

**What it holds, per observing army:** per zone, per side (own, allied, enemy, as that army sees them) and per
movement class (land, naval, hover, amphibious, air, static):
- the count and the mass of the units;
- their summed ratings against each target type (ground, air, naval, structure);
- the ratings by range band (short < 30, medium < 60, long < 150, strategic), and the longest range;
- the contacts whose type is unknown.

Per zone and side it also keeps the engineers, commanders, experimentals, factories, shield health and economy
mass. Units under construction are left out, and a damaged unit counts with the square root of its health share.

**Fair intel** (`Unit::recon`, intel.cpp):
- An enemy unit counts only while the army holds a blip of it, and where the blip is.
- Its type and rating are known once the army has had it in sight (LOSNow or LOSEver) or omni sees it.
- A radar or sonar contact never seen counts as an unknown contact of its movement class. A player too sees air, land
  or naval on the radar icon.
- A jammer's fake blips count as unknown contacts until the army knows them as fakes (KnownFake). A real blip marked
  KnownFake is left out.
- Own and allied units are known in full.
- Seen in a SETONS_R game with fog of war (`explored`), at beat 2990: army 1 knows 8 of the 301 enemy units and has 2
  unknown contacts. The same game without fog knows nearly all of them.
- Lost contacts are not remembered yet (a later phase; the engine's threat map keeps them).
- The cheat option "AI vision" will read every unit, as omni would (phase 3, with the lobby option).

**"Can it reach"** (`AIZoneThreat`): the enemy strength that can come to a zone within a travel horizon, or shoot into
it.
- Mobile units reach by the map analysis' travel distance for their own movement class. A navy in a pond that
  touches no land zone cannot come to one.
- Aircraft reach by straight distance.
- Anything reaches whose longest range covers the zone.
- Only strength against the asked target type counts, so a submarine adds nothing against ground.
- The check on SETONS_R: a frigate and a sub (enemy) in the closed pond, zone 113.
  - The navy's threat against ground is 5.5 in the pond and in its two land neighbours, zones 4 and 59, which the
    frigate's range covers. It is 0 in zones 5 and 31 and in the open-sea zone 160, even with a 2000-unit horizon.
  - The Tigershark counts against ground because its deck gun can hit land.

**Reaction delay** (2.6) will be a ring of these summaries, one per build, so that a slow brain reads an older one
(phase 5, with the difficulty knobs).

**Cost:** measured with the proofs (see the aicore notes). Each build is a pass over the units with a zone lookup per
unit, plus an array clear. The zone x zone sums of `AIZoneThreat` are made per query.

### 3.3 Unit ratings and the battle estimate (ratings and square law: phase 2, built; small fight: phase 5)

- **Ratings from the blueprints** (Chris's idea, ai-design-goals.md decision 10). Every unit blueprint gets, from its
  blueprint alone:
  - **damage per second** against ground, air, naval and structure targets:
    - each weapon's damage x the shots per firing x the rate of fire;
    - the shots per firing count salvos, racks fired together, projectiles per firing, damage-over-time pulses and
      beam pulses;
    - which targets a weapon hits comes from `FireTargetLayerCapsTable`, narrowed to air by a
      `TargetRestrictOnlyAllow` of AIR;
    - left out: death, teleport and defence (anti-missile) weapons, weapons that come only with an upgrade, manual
      fire (overcharge), and nukes (the unit is marked "strategic" instead);
  - health plus shield;
  - the longest range per target type;
  - speed and mass.
- **The rating** against a target type is sqrt(dps x (health + shield)), divided by the same for the reference unit:
  the UEF T1 tank uel0201 (24 dps x 300 hp), which rates 1.0.
  - This is the per-unit term of the square law. A group's fighting strength is (sum of ratings)^2, so groups compare
    by their sums, and 20 tanks beat 10 by far more than twice.
  - Examples: T1 tank `T1G 1.0`, UEF T1 interceptor `T1A 1.4` (air only), UEF T1 frigate `T1N 3.8`, UEF ACU `CG 12.9`,
    T1 point defence `T1S 7.5`, UEF battleship `T3N 97.8`, Galactic Colossus `T4X 186.3`.
- **Chris's tuning lists:** `MOHO64_AI_RATINGS=<file>` writes two files.
  - `<file>`: every unit, mods included (about 1,360 with the lab's mods), by domain, tier and rating.
  - `<file>` with `_stock` before its extension: the game's own playable units only, about 450, the list Chris tunes
    first. It has the four factions, no mod, no civilians, campaign operation units, dummies or effects, sorted by
    faction, tier and domain.
  - Each row has:
    - the label (tier + domain letter + best rating: T1 T2 T3, T4 experimental, C commander; G ground, A air, N naval,
      S structure, X experimental);
    - id, faction, name, mass, health, shield;
    - dps and range per target type, speed, and the four ratings;
    - a `fix` column of 1.0 for his multiplier;
    - a tier note.
  - **The tier** comes from the categories TECH1 / TECH2 / TECH3 / EXPERIMENTAL / COMMAND. A mobile unit whose
    categories give none, or one at least two tiers below what its mass says, takes its mass band's tier instead,
    never lower than its categories'.
    - This catches a mod's 3,600-mass "TECH1" bot: BlackOps' UnderTaker, Tha-Yath and Redhawk now read `T4X*`.
    - The mass bands: ground under 150 / 600 / 3,000, then T4; air under 150 / 500 / 3,000; naval under 600 / 4,000 /
      15,000.
    - Such a label gets a `*`, and the tier note says what the categories gave.
  - **OPEN:** reading his corrected list back (a multiplier per unit, plus "A beats B" overrides) as a file in
    `data/ai`, in phase 3.
  - Rows that look rough, the kind the lists are for: the mods' commanders (BlackOps) rate very high from their extra
    weapons; T1 point defence comes out at 241 dps; beam units such as the Arachne.
- **The square-law estimate:** `Remastered.AISquareLaw(ours, theirs)` returns two numbers:
  - the margin ours^2 / (ours^2 + theirs^2), where 0.5 is even;
  - the share of the winner's strength left at the end.

  The inputs are summed ratings, for example `AIZoneThreat`'s two answers.
- **Not yet:** closing time from range and speed, and reinforcements on the way (phase 4, with the operations).
- **The small fight simulation (phase 5)** for the few fights that matter each second (an objective, a push, a
  breach stage):
  - both sides as lists of units with blueprint dps, health, range and speed;
  - stepped in half-second steps for up to 30 s, with simple target choice, in native code, deterministic;
  - on the worker pool when there are several, one fight per task, results committed in the fights' order.

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
- **Ours** (settled with the ui, 2026-10-28): the key carries type and skill only, as FAF's lists do, so the slot's
  drop-down stays short. There are four keys, `remastered_recruit`, `remastered_veteran`, `remastered_commander` and
  `remastered_supreme`, each with a cheating twin (`..._cheat`) for the resource-cheat option. Personality
  (`RemasterPersonality`) and teacher mode (`RemasterTeacher`) are per-slot options beside the key (2.7, 2.8). The
  engine knows our AI by the prefix: an army whose `AIPersonality` starts with `remastered`
  (`ai::SessionHasOurAi`).
- **Where our Lua lives:** `fa-remaster/data/ai/` in the repository:
  - `hook/<path>` hooks a game script: `hook/lua/aibrains/index.lua` adds our keys to `keyToBrain`;
  - every other `.lua` file is a script at its own path: `lua/remastered/ai/brain.lua` is
    `/lua/remastered/ai/brain.lua` (the brain; a skeleton until phase 3);
  - the lobby's entries are the interface's: an aitypes hook in `ui/data/hook` (the ui's part). The interface needs
    no `data/ai` at all.
  - It is not a FAF mod folder: as the base AI it is always there, never switched on in the mods manager.
- **How the sim takes it (decided 2026-10-28, the lead with the ui): only in a session that has our AI, into a layer
  the Sim owns** (`src/ai/scripts.h`).
  - When an army's `AIPersonality` starts with `remastered`, `Sim::CreateArmies` gives the sim state `data/ai`, after
    the armies' options are read and before the brains are made (`OnCreateArmyBrain` imports `index.lua`):
    - its hooks join the state's extra hooks, after the engine's script fixes (`ScriptState::AddExtraHooks`);
    - its other scripts become the state's own files (`ScriptState::SetOwnFiles`). `doscript` / `import` and
      `exists()` find them only where the game's file system has no file of that path, so they never replace a game
      file. `DiskFindFiles` does not list them, so FAF's `BeginSessionAI` folder imports never pick them up.
  - **Nothing is mounted.** moho64ui has one file system shared by its menus, the interface and the Sim, from game to
    game. A mount from the sim would stay for the next game and would be read while other threads read the file
    system. The state's own files go with the state, so no other state and no next game sees them. The same rule
    holds for headless moho64, the ui's games, replays and the replay scan, with no host involvement.
  - The folder is found as the engine's script fixes are: beside the program (`data/ai`) or one folder up, and
    `MOHO64_AI_DIR` names another.
  - A replay of a game with our AI has the keys in its armies' options and takes it again. A game without our AI
    loads exactly today's Lua: B1, b8 and the 16 are identical.
  - **The test** is `moho64 --session <table with our key> --then-session <table without>` (two games in one process,
    as the interface plays its next game; tools in the aicore notes). In the first game `exists('/lua/remastered/ai/brain.lua')`
    is true and the armies with our keys have the brain. In the second game it is false, `Remastered.AIZoneAt` is nil,
    and the log shows no `data/ai` line.
  - **Phase 1 keeps the same rule for its own part:** the `Remastered.AI*` functions are made in the same place, only
    in a session with our AI.
- **The brain class** derives from FAF's plain army brain (`/lua/aibrain.lua` AIBrain, what human armies use), not
  from `/lua/aibrains/base-ai.lua`, so none of FAF's builder managers run.

### 4.2 The Lua API

- A table of C functions in the `Remastered` global (script/fastlib.h; that table exists only in sessions with the
  engine's script fixes, which every new game has), named `Remastered.AI*`. They are added only in a session with our
  AI (4.1). Each returns a number, a short list, or one small table. Nothing returns a table per unit per tick. Zone,
  island and pond ids are 1-based small integers. Domains are the strings 'land', 'naval', 'hover', 'amphibious'.
- **Phase 1 (built, src/ai/map_analysis.cpp):**

  | Call | Returns |
  |---|---|
  | `AIMapInfo()` | `{zones, landZones, waterZones, links, islands, ponds, segment, width, height}` |
  | `AIZoneAt(x, z [, domain])` | the zone id at a world point; 0 for none, or where that domain cannot stand |
  | `AIZone(id)` | `{id, domain, area, massSeeded, x, z, segments, cut, massPoints = {{x, z}, ...}, hydroPoints, starts = {army, ...}, neighbours = {id, ...}}` |
  | `AIZoneNeighbours(id)` | `{{zone, a, b, width, x, z, coast, choke}, ...}` by neighbour id; width in world units |
  | `AIZoneDistance(a, b [, domain = 'land'])` | travel distance in world units; -1 when that domain cannot get there |
  | `AIChokepoints()` | the chokepoint links, `{{a, b, width, x, z, ...}, ...}` |
  | `AIBuildSpots(zone, size)` | `{{x, z}, ...}`: centres of terrain spots of at least `size` (2, 4, 8, 16) in the zone |

  A bad zone id or domain is a Lua argument error. No call hands out a wall-clock time.
- **Phase 2 (built, src/ai/unit_index.cpp):** `army` is the 1-based army index of an army that plays our AI. Any other
  army is an argument error: an AI never asks with another army's eyes.

  | Call | Returns |
  |---|---|
  | `AIUnitRating(blueprintId)` | `{id, label, tier, domain, mass, health, shield, speed, best, maxRange, kinds, dps = {ground, air, naval, structure}, range = {...}, rating = {...}}`, or nil |
  | `AIZoneForces(army, zone)` | `{own = S, ally = S, enemy = S}`. Each S is `{land = D, naval = D, air = D, structure = D, engineers, commanders, experimentals, factories, shieldHealth, economyMass}`, and each D is `{count, mass, unknown, maxRange, ground, air, naval, structure}` |
  | `AIZoneThreat(army, zone, target, horizon [, from])` | `enemy, friendly`: summed ratings against `target` of what can reach the zone (3.2); `from` limits it to one domain |
  | `AIIndexInfo(army)` | `{tick, own, ally, enemy, unknown, outside, period}` |
  | `AISquareLaw(ours, theirs)` | `margin, survivors` (3.3) |
- **Randomness:** the AI never calls the sim's `Random` for its own choices (that stream is shared with the game and
  the original's replays compare its draws). It gets its own MT19937 per brain, seeded from the session seed and the
  army index (`Remastered.AIRandom(brain)`, phase 3): same draws on every machine, and the game's stream is untouched.
- **When the brain thinks:** a script thread per brain (`ForkThread` from `OnCreateAI`), waking on fixed ticks
  (layer periods in 2.1-2.3) offset by army index, so the AI's cost is spread over beats.

### 4.3 Where the native work runs in the beat

- Map analysis: once, at the end of `Sim::Start` when our AI is in the session (built), else on the first call.
- Index: one fixed stage each beat, after the recon tick and before the script threads (phase 2 picks the exact
  point and documents it in `engine-ref/specs/beat_order.md`'s style), only when a brain of ours exists.
- Battle estimates and fight simulations: inside the Lua call that asks (serial), or as one batch stage for the
  queued fights of the beat (parallel, phase 5).

### 4.4 The debug dump (`MOHO64_AI_MAPDUMP=<file>`)

- **What it does:** it forces the map analysis at the end of `Sim::Start` in any session, ours or not, and writes two
  files.
- **`<file>`, text:**
  - the map, the footprints used and the time taken;
  - each domain's areas;
  - the deposits;
  - one line per zone: domain, area, mass or plain, centre, segments, mass, hydro, starts, cut, neighbours, and the
    build spots per size;
  - one line per link: width, midpoint, coast or choke, and the distance per domain;
  - the travel distances between the start zones;
  - the zone grid, one character per segment.
- **`<file>.png`, the picture:** 3 pixels per segment.
  - Land zones are warm colours and water zones blue, with darker borders.
  - Red marks a chokepoint, black a zone centre, orange a cut zone's centre, white a mass point, cyan a hydro point,
    and magenta a start zone.
- **It changes nothing:** it only reads and writes files. The b8 fingerprint with the dump is identical to b8
  without it.

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
| 1 (built 2026-10-28) | Map analysis native (`src/ai/`), its Lua calls, the `MOHO64_AI_MAPDUMP` dump; computed and its calls made only in a session with our AI | The 16 + B1 / b8 identical to main; SETONS_R's dump looked at (zones around the mass, ponds, chokepoints at the passes); 250-300 ms; an AI-key session identical at 1 / 32 threads, shuffle, reverse |
| 2 (built 2026-10-28) | Unit / threat index per observing army with fair intel and "can it reach"; blueprint ratings with Chris's labels and list (`MOHO64_AI_RATINGS`); the square-law estimate; their Lua calls | Scripted sessions (a navy in a closed pond does not threaten distant land; with fog only blips count, unseen contacts are unknown); the 16 + B1 / b8 identical; our-key sessions identical at 1 / 32 threads, shuffle, reverse |
| 3 | The brain in `data/ai/` (keys and skeleton are in since phase 1b): OnCreateAI, own random stream, think loops; economy and building (opening from the map, build-power jobs, expansion) and simple defence | Our AI vs FAF's adaptive on SETONS_R: it expands, stays out of stalls, defends; it does not crash for 60 minutes; Lua + native cost per tick |
| 4 | Strategic and operation layers: aggression with hysteresis, objectives, gathering, push, retreat by estimate, raids, navy and air domains, combined arms timing | Ours vs M28 and FAF's AI, several seeds: the fight scores of section 6; "never fled a weaker force" at zero faults |
| 5 | Cracking fortified lines and staged breaches; the small fight simulation; group micro (focus fire, kiting in even fights); difficulty knobs and the four levels | Scripted fortified-line scenarios (a session that starts with T2 PD + artillery lines); level-vs-level matches (each level beats the one below most of the time) |
| 6 | Team play, pings (with the ui's radial menu), allied announcements, personalities, teacher mode; hot Lua parts moved native | 2v2 / 4v4 with a human-ally stand-in (scripted pings); Chris's playtest |

## 8. Open points (collected)

1. ~~Personality: per-slot option or in the key~~ settled with the ui: per-slot `RemasterPersonality` (2.7).
2. ~~Ping path~~ settled with the ui: SpawnPing with `Intent` and `Target`, Target checked in the sim (2.5).
3. ~~Announcements' path~~ settled with the ui: `SendChatToAllies` (2.9).
4. Teacher mode in team games.
5. Map-analysis caching per map, if big maps need it.
6. Chokepoint width threshold, zone radii and the sliver sizes, to tune on more maps than SETONS_R.
7. How long "too strong to attack" waits before a long plan, per personality.
8. ~~`data/ai/` as a FAF mod folder; how the sim takes it~~ settled: not a mod folder; the Sim takes it only with our
   keys, into the state's own files and hooks, nothing mounted (4.1).
9. Where exactly the index stage sits in the beat (phase 2 reads the beat order and decides).
