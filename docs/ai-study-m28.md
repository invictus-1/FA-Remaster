# AI study: how M28AI plays, and ideas for our own AI

Engine docs. Written 2026-10-28 by the engine chat's helper "aistudy", from reading only (no game run, no sim
run). Sources: M28AI v326 as our variant holds it (`mods/m28-variant/lua/AI/M28*.lua`, same decisions as the
original), the installed M28AI, FAF's own AI in `gamedata/lua.nx2` (`/lua/AI/`, `/lua/aibrains/`), and the
engine's measurements in `benchmarks.md`, `performance-lessons.md` and `engine-answers.md`.

## 0. Licence, read first

- M28AI v326 (maudlin27) is **CC BY-NC-SA 4.0**. Our own AI must not be a derivative of it. See `CLAUDE.md`,
  "M28AI's license".
- So this document describes **what M28 does and why it works**, in our own words: behaviours and ideas, never its
  code, never pseudo-code that follows its functions, never its tuning tables. A file or function name is given
  only to say where something lives. A few single numbers (for example "about 1.4 times the enemy's threat") are
  given as orders of magnitude, so that we know what it does; they are not values to copy.
- The general techniques named here (zones, influence or threat maps, build orders, kiting, focus fire, Lanchester
  estimates) are common RTS-AI knowledge and belong to nobody.
- A side note for Chris: the old M28AI v92 in `Documents\My Games\...\Mods\M28AI` says "MIT License" in its
  `LICENSE` and `mod_info.lua`. The version we run (v326) is CC BY-NC-SA 4.0. Nothing changes for us: we follow
  `CLAUDE.md` and treat M28 as CC BY-NC-SA.
- Our own AI will be written from scratch from the ideas in sections 6 and 7, not from M28's files.

## 1. In one paragraph

M28 plays like a player for one main reason: it does not think in platoons that are sent somewhere and forgotten. It
cuts the map into **zones** around groups of mass points. Every second it looks again at every zone: what is ours
there, what the enemy has there and next to it, how far each side can shoot, and how much the zone is worth. Each
zone then makes small, local decisions: attack, kite, hold, call for help, retreat, or send spare units somewhere
else. Engineers, factories, air, navy and the commander read the same zone data. On top of that sits a lot of
hand-made special knowledge: build orders taken from the community, ACU upgrade paths, overcharge, bomb dodging,
bomber snipes, nuke and artillery targeting, game-ender templates. That knowledge is what gives it a player's
"habits". The price: about 13 MB of Lua, roughly half of an AI game's tick, and an army that often does not want to
fight even when it is far ahead (section 4).

## 2. How M28 is organised

### 2.1 The big systems

| System | Lives in | What it decides | From what | How often |
|---|---|---|---|---|
| Map analysis | `M28Map.lua` (`SetupMap`) | Zones, adjacency, travel distances, paths, rally points, build spots | FAF's navigation mesh, mass and hydro points, terrain, water | Once at the start (heavy), then small background refreshes |
| Team layer | `M28Team.lua` (`TeamOverseer`) | Team economy, upgrades, enemy ACU status, snipe targets, sharing | Shared tables of all allied M28s | Every second; land subteam every 10 s |
| Threat | `M28Land`, `M28Navy`, `M28Air`, `M28UnitInfo` | How dangerous each zone is, by kind of danger | Units it knows, rated by mass cost | Every zone cycle (~1 s), event-driven assignment |
| Land zones | `M28Land.lua` (`ManageSpecificLandZone`) | Per zone: fight, kite, retreat, support, scouts, shields, AA escorts | Zone threat, ranges, zone value | One pass over all zones every 11 ticks (~1 s) |
| Engineers | `M28Engineer.lua` | Per zone, a ranked list of jobs and how much build power each wants | Zone needs, economy, enemy threats | Every zone cycle |
| Factories | `M28Factory.lua` | What each factory builds next | Zone support wanted, enemy army mix, air control | One factory per tick |
| Economy | `M28Economy.lua` | Income tracking, stall pausing, overflow, own-power reclaim | Unit income, storage | 1 s; full recount 30 s |
| Air | `M28Air.lua` | Fighters, bombers, gunships, torpedo bombers, scouts, transports, Novax | Air threat, AA along paths, air control | Every second per air subteam, jobs a tick apart |
| Navy | `M28Navy.lua` | Water zones: fight, kite, retreat, bombard, raid, submarines | Water-zone threat, ranges | ~1 s per pond |
| ACU | `M28ACU.lua` | Build order, fight or run, overcharge, upgrades, teleport snipes | Zone threat, own health, economy | Per zone cycle |
| Strategic weapons | `M28Building.lua` | TML, TMD cover, SMD, nukes, T3 artillery, game enders, shield cycling | Target values, defences in the way | Event-driven plus loops |
| Micro | `M28Micro.lua` | Dodging shots and bombs, circling, reversing while firing | Weapon-fired events, ballistics | Event-driven |
| Orders | `M28Orders.lua` | Do not re-send an order the unit already has | Each unit's own order list | Every order |
| Chat | `M28Chat.lua` | Messages, personality | Game events | Event-driven |

### 2.2 Map analysis: zones are the backbone

- **Segments.** A square grid over the map, from about one map unit per cell on a 5 km map to a few on a 20 km
  map. Everything in a cell is treated like its centre.
- **Plateaus** are areas that hover or amphibious units can reach, taken from FAF's navigation mesh labels.
  **Islands** are land-reachable parts of a plateau. **Ponds** are naval-reachable bodies of water.
- **Land zones** are built around groups of nearby mass points on the same land area, then grown outward over
  segments, with a limit on travel distance. Areas far from any mass get their own zones without mass. **Water
  zones** are seeded across the water and grown the same way.
- **Precomputed per zone:** midpoint and size, free mass and hydro points, neighbouring zones, paths to other zones,
  the travel distance between every pair of zones (spread over many ticks in the background), a scout patrol route,
  mass-storage spots, a spare game-ender spot, the closest allied and enemy base, and for ponds which mass points
  can be shot from the water.
- **Why it matters:** everything else is cheap to phrase once the map is in zones. "Is this zone safe?", "which
  zone needs help?", "where do I retreat?" are questions about a few hundred zones, not about the whole map.
- **Cost:** FAF builds its navigation mesh for it (about 1.0-1.5 s at tick 10). `SetupMap` itself then takes about
  0.3-1 s at ticks 16, 17 and 21. It flood-fills about 10^5 cells with path checks, calls the path finder many
  times, works out travel distances for every pair of zones, and makes about 93,000 "can I build here?" checks.
  M28's own chat message warns that the set-up takes a while. (`benchmarks.md`, `engine-answers.md`)

### 2.3 Team coordination

- All allied M28s run in the same Lua state and **share the same team tables**, so there is no messaging. They see
  the same zone threats, the same enemy units and the same snipe targets.
- A **team** is all the allies. An **air subteam** groups M28s whose bases are close, or that face the enemy from
  a similar direction. A **land subteam** is part of an air subteam on the same start plateau or island.
- **Per team:** zone data, enemy unit lists, threat totals, the big-threat list, economy totals, stall pausing,
  upgrade choices, enemy tech level, snipe targets, the scouting list.
- **Per brain:** its own economy, start point, cheat multipliers, and a few strategy rolls (first bomber, combat
  scouts, and so on).
- **Sharing:** resources are evened out between teammates. A dying player gives everything to its allies.
  Storages are gifted to whoever owns the mass point next to them. Support factories go to the teammate with the
  better HQ. Power is gifted for adjacency.

### 2.4 Threat assessment

- **A unit's threat is its mass cost**, adjusted by role and by health (plus shield). Damaged enemies are
  discounted less than damaged allies, so it plays a little safe. Veterancy adds a little. Units under
  construction count for very little. ACUs get their own rating, which grows with upgrades. T3 and experimental
  artillery count as zero combat threat (they are handled as "big threats" instead).
- **Stored per zone and per team, by kind:**
  - enemy and allied mobile direct fire, bucketed by weapon range;
  - mobile indirect fire;
  - structure direct and indirect fire;
  - ground AA and air-to-ground threat;
  - shields;
  - structure mass;
  - long-range threats that sit outside the zone but can reach it.
- **Big threats** (experimentals, upgraded ACUs, T3 artillery, game enders, nukes, SMD, battleships, Novax) are
  kept in team lists and trigger reactions: shields, SMD, retargeting, snipes.
- **Firebases:** a cluster of T2 artillery and point defence that missile launchers probably cannot break is marked
  as a firebase. Every zone in its range gets a flag.
- **Intel:** a unit goes into a zone when the AI detects it, through events. Each cycle the AI moves every known
  enemy to the zone of its last known position.
  - From the reading, M28 seems to treat a radar blip as fully known and to read the real unit's type and exact
    position. It also corrects a lost unit's position when its own units are near where the unit was last seen.
    This was read but not fully traced: **likely, not certain**.

### 2.5 Economy, engineers, factories

- **Engineers work from per-zone job lists, not a central queue.** Each zone keeps a long, ranked list of jobs.
  Each job says what it needs: build power, a minimum tech level, sometimes adjacency. Engineers fill the most
  important unmet jobs first, taking engineers off lower jobs if needed. If a building of that type is already
  started, they assist it instead of starting a new one. Demand that no engineer can meet is published, so
  factories make engineers for it and neighbouring zones send theirs.
  - **The number of engineers is measured in build power, not heads.** This is a good idea in itself.
  - **There are about 80 kinds of job:** mass, hydro, power, factories, storage, radar and sonar, shields, TMD, AA,
    point defence, artillery, TML, SMD, assisting, reclaim, capture, repair, walls, experimentals, game enders.
  - **The order of the list is fixed;** the conditions change. Emergencies come first: T3 AA against enemy T3 air,
    TMD after losing a building to a missile, point defence against teleports, power when power is low. Then mass,
    factories, SMD once the enemy has a nuke, the first experimental, and so on down to reclaim.
- **Expansion:** zones that want mass build power ask their neighbours. An engineer goes only if the path there is
  safe, judged from a precomputed list of dangerous zones. Outer zones get no build power while there is notable
  enemy combat threat in them.
- **Build spots:** each zone keeps the largest building size that fits on each grid square. When anything is
  built, the squares around it are checked again, for every size. That re-check was the variant's big cost: about
  15,000 engine checks per new building, 15-17 ms per upgrade. The variant now caches it.
- **Economy:** income is counted per unit as units are built and die. Stalls pause units tier by tier, by
  estimated usage, until the deficit is covered, with many exceptions (keep one upgrade going, keep the last power
  builder, finish work that is nearly done). Mass overflow cancels reclaim. After enough T2 or T3 power, old T1
  power is reclaimed.
- **Upgrades:** the AI aims to spend a share of its income on mass extractor upgrades. HQ factory upgrades get
  priority when the enemy is ahead in tech or has an upgraded ACU nearby. A unit is only upgraded when it is
  "safe": no enemies in this zone or next to it (or only a little threat in a core base), and no recent cancelled
  upgrade. Each new mass extractor waits a while before upgrading, depending on game time and income.
- **Factories** go down a long list of conditions; the first that matches decides. The inputs:
  - what nearby zones are asking for (direct fire, indirect fire, mobile AA, shields, stealth);
  - build power wanted;
  - enemy ranges and structures;
  - air control;
  - enemy experimentals and firebases.
  - **Adapting to the enemy's army is done here.** It keeps a ratio of mobile AA to land army, tightened without
    air control and loosened when early enemy T3 air is seen. It makes sniper bots against experimentals and ACUs,
    and artillery against point defence and firebases. The air factory makes fighters in proportion to the
    enemy's gunships and bombers, and torpedo bombers against navy.

### 2.6 Land, air, navy, ACU, scouting, strategic weapons, transports, reclaim

- **Land:** described in sections 3 and 4.
- **Air:** air control is our fighter threat compared with the enemy's, with margins.
  - **Fighters:** gather at a support point pushed out from the base as far as the zones are safe, and go after
    targets in priority order. They are added to each target until they beat its threat. They lead the target
    and skip faster targets. They refuel at air staging.
  - **Bombers:** search zones in order of distance for targets above a minimum value. A target is rejected if
    there is too much AA along the flight path. Bombers are assigned until the expected bomb damage kills the
    target.
  - **Special bombers:** dedicated T1 bombers hunt engineers. An early raid goes for the enemy's engineers and
    power. ACU snipes wait at the rally point until there are enough bombers to kill in one pass, then go
    together.
  - **Gunships** defend near the base first, then go to zones whose AA is under a budget.
  - **Strategic bombers:** enemy ones are rammed by fighters.
  - **Novax** and the experimental bombers have their own target pickers.
- **Navy:** the same three-way logic as land (kite if we outrange, attack if stronger or at home, otherwise
  retreat). A short hold stops it flipping between attacking and retreating.
  - Ships with nothing to fight bombard mass extractors and structures along the shore.
  - Every few ships become raiders that follow a water path to enemy mass and bombard it.
  - Submarines surface or dive depending on air threat.
  - It only goes naval when a pond is worth it: enemy mass reachable from the water, or no land path to the enemy.
- **ACU:**
  - **Build order:** follows a scripted opening tuned per map (factory, then power and mass, second factory land
    or air by map size and mass count). Low-mass maps get their own opening.
  - **Running away:** it runs when a long list of danger factors outweighs its own strength. The factors include
    its health and upgrades, enemy T3, point defence, air threat against our AA, and enemy ACUs it outranges. It
    goes home in critical danger (experimentals, air snipe risk, upgraded enemy ACUs).
  - **Overcharge:** picks the target that removes the most threat, and always takes a low-health enemy ACU.
  - **Upgrades:** follow a faction path (gun first). They are timed by income and safety, and dropped if the ACU
    would die before finishing.
  - **Help:** when it is in trouble its zone's value goes up, and that pulls combat units, shields and AA towards
    it.
  - **Teleport:** snipes teleport at high-value targets.
- **Scouting:** air scouts visit zones on timers: enemy start zones often, zones with mass less often, the rest
  rarely. A failed visit stretches the timer. Each land zone wants one land scout. Units that need sight (such as
  skirmishers) can ask for a priority scout.
- **TML / TMD / SMD / nukes / T3 artillery:**
  - **TML:** it tracks which TMD covers which target from each TML, by the missile's flight line. It fires at
    uncovered stationary ACUs or high-value targets, and groups TMLs for salvos.
  - **Nukes:** target the highest mass value. Targets covered by SMD are skipped unless enough nukes can
    overwhelm it.
  - **T3 artillery:** scores zones by value, adjusted by past misses, the turret turn needed, shields and allied
    fire. It then picks a point among the most valuable units.
  - **Game enders** get reserved spots, shield slots and dedicated engineer crews. Shields are cycled so a weak
    one can recharge.
- **Transports:** a shortlist made at the start: far zones on our own island, islands worth an engineer drop, and
  ponds land units cannot reach. A small engineer group is flown out and the drop zone is marked as an
  expansion. Combat drops exist but are narrow (T1 artillery onto exposed enemy mass on plateaus).
- **Reclaim:** a coarse reclaim grid is kept up to date, and zone reclaim value counts towards the zone's value.
  Engineers and the ACU reclaim nearby fields. A mass stall sends spare engineers to reclaim. Big fields relax the
  limits on sending engineers to dangerous zones.

## 3. What makes it play like a player

| Behaviour | How M28 gets it (conceptually) |
|---|---|
| Expanding | Mass points are the zones; each zone publishes "I want build power for mass"; neighbours send engineers when the path is safe; transports drop engineers on islands. |
| Raiding | Fast units become raiders and go after the zone with exposed enemy mass at the best angle away from the main fight. Engineer-hunting T1 bombers, early bomber raids, mass-extractor-hunting bombers, naval raiders. |
| Retreating when outnumbered | Each zone compares its threat with the enemy's (zone + neighbours + long-range threats reaching in); if outranged and outmatched it falls back to a rally point a couple of zones at a time, by a path judged safe. Units that already have enemies in range keep firing. |
| Kiting | If our best range beats the enemy's, longer-ranged units attack, then back off when enemies get close, reversing while facing the enemy when they can so they keep firing. Experimentals like the Megalith reverse while shooting. |
| Focus fire | Limited. The default is attack-move towards the nearest enemy. Experimentals focus the weakest enemy in range; units with short attack-move reach pick the most valuable target; T1 artillery spreads fire by "value per shot to kill"; bombers stack until the target dies in one pass. No general team-wide focus fire. |
| Defending mass | Zones with mass are valuable; valuable zones claim units first and get lower attack thresholds; a zone under attack flags "wants support" and spare units nearby answer. |
| Timing upgrades | Upgrade only when "safe" (no enemies in or next to the zone), with enough income, after a delay per extractor; ACU upgrades gated by income and danger, dropped if it would die first. |
| Adapting to the enemy's army | Factories read zone requests and enemy composition: AA ratio follows enemy air, sniper bots against experimentals, artillery against firebases, fighters in proportion to enemy bombers and gunships, torpedo bombers against navy, SMD once a nuke is seen, TMD after a missile hit. |
| Using the ACU | Scripted opening, then fights when it is stronger, overcharges the best target, runs when in danger, gets upgrades by a faction path, calls help when hurt, teleports to snipe, works as an engineer later. |
| Teaming with allies | One shared set of team tables: same threat picture, shared snipe targets, resources evened out, storages and factories gifted for adjacency, fighters cover allies' units. |
| Not repeating orders | Every order is remembered; the same order is not sent again, so units do not stutter. Micro locks stop the zone manager from overwriting a dodge. |
| Dodging | On "weapon fired" events it estimates time to impact and damage, and steps aside if it does not cost firepower. Bombs too. |
| Personality | Some random strategy rolls at the start (first bomber, strategic bombers, T3 air rush, navy priority) and a chat personality. |

**Cheating or not:**

- **AIx** is the lobby's cheating AI: it gets build-rate and income buffs that the lobby sets (and can change over
  time), and an "omni" option that marks every zone as fully seen.
- **Plain M28** still gets one trick. At 4.4 s it gives its commander map-wide sight for 3 seconds (0.6 s if a
  human is on its team), so that it knows where the civilian buildings are. After that it has seen every enemy's starting
  units. (`engine-answers.md`, render 13)
- As said in 2.4, it likely reads more from radar blips than a player could. Otherwise it plays from what it
  detects.

## 4. Why M28 does not want to fight (Chris: "5 experimentals and 500 T3 units afraid of T1 tanks")

From the reading, these are the reasons, roughly in the order I think they matter. They are behaviours seen in the
code, not measured in a game. A test with a log of which branch each zone takes would confirm which ones bite most.

1. **Every decision is local to one zone.** A zone compares the units in that zone with the enemy threat in and
   around it. M28's total army never enters the question.
   - There is no layer that says "we have 10 times the enemy's army, go". 500 units spread over 20 zones are 20
     small armies, and each one judges alone.
   - Spare units are sent to the nearest zone with any enemy in it. They are sent with a plain move, not an
     attack-move, so they arrive a few at a time. The receiving zone then judges with whatever has arrived.
2. **A safety margin on top of a cautious count.** To attack, a zone wants clearly more threat than the enemy.
   - The default is about 1.4 times the enemy's threat. A bit less is enough in a valuable zone or when our ACU is
     there. In a core base, a bit below parity is enough.
   - The enemy's side is counted generously:
     - the larger of several measures of enemy threat;
     - neighbouring zones;
     - long-range units and structures that can reach the zone;
     - a bonus for shields;
     - an upgraded or healthy enemy ACU nearby needs about twice the enemy's threat, so that it does not give the ACU free kills.
   - Our side counts only the land combat units in the zone. Whether our mobile indirect fire counts on our side
     there: **not sure**.
3. **Outranging means kiting, even when we are far stronger.** When our range beats the enemy's best range, the
   zone takes the kiting branch first: units fire, then back off as soon as the enemy comes close.
   - Against a stream of T1 tanks, longer-ranged T3 units keep stepping back towards their rally point, which is
     near the base. To a player that looks exactly like running away.
   - As far as I can tell, the size of the advantage does not switch this off. There are exceptions (engineers to
     raid, experimentals near enemy experimentals, an ACU in trouble), but "we outnumber them 20 to 1, just walk
     over them" is not one of them.
4. **Fear of air.** Without air control, a land force in a zone retreats when the enemy's air-to-ground threat
   nearby, after subtracting our AA there, is more than about a tenth of the force's own threat. Big forces with
   experimentals have a second rule: far from home, with little mobile AA, while far behind on air, they run when
   the enemy's total air-to-ground is above about a third of the force.
   - So a few enemy gunships can turn a whole T3 army around, even when enemy land units are weak.
5. **Retreat goes home, then the cycle starts again.** Retreating units go to a rally point (a core base or an
   expansion), a couple of zones at a time. Once there, they are counted in that zone again.
   - A valuable home zone claims units before less valuable zones can. Units claimed by a valuable zone can only
     be taken by an equally valuable one.
   - So armies tend to settle at home and come out only to answer "wants support" calls.
6. **"Attack with everything" is rare.** The all-in branch triggers in special cases:
   - enemies in our core base;
   - an enemy ACU or Fatboy to suicide into;
   - our snipe target nearby;
   - no buildings left;
   - enemy game enders or several T3 artillery;
   - very large numbers *inside one zone*: over a hundred direct-fire units, clearly more than the enemy units
     there, and few enemy direct-fire units near.
   - "We are at the unit cap" or "we are far ahead overall" are not on the list.
7. **Firebases and long-range structures scare whole regions.** Every zone in range of a firebase or long-range
   structure adds that threat. Units retreat unless they can clearly overwhelm it (once one zone commits, others
   may join).
8. **Experimentals mostly follow the same zone rules.** Experimentals have extra rules for engaging ACUs and enemy
   experimentals. Big amphibious forces are sent at the enemy base only in narrow cases: the enemy has artillery
   or game enders, and we have at least as many experimentals as the enemy.
9. **It slows itself down in big games.** Above about 1,500 units it stretches its decision cycles (land a tick
   per few hundred units, navy up to about 4 s). So at the size where Chris sees the problem, it also reacts
   slower and dithers longer between "attack" and "retreat".

In short: M28 asks "can the units here safely win the fight here?", many times a second, and the answer is often
no. It never asks "are we winning the game, and how do we turn that into a win?"

## 5. Where M28 is weak or costly

### 5.1 Blind spots (from reading)

- **No strategic push or army-level plan.** See section 4. It has many reactions and few intentions.
- **Mass-cost threat is linear.** 20 tanks are worth twice 10 tanks, but in a real fight between ranged units the
  bigger side wins by much more (Lanchester's square law). It also ignores formation, terrain, and who is already
  in range. So it underrates big concentrated armies, including its own.
- **Focus fire is mostly missing**, apart from experimentals, bombers and some special cases.
- **Many hand-tuned special cases.** The land combat function alone is about 7,000 lines, with branches per unit
  type, per mod (LOUD, QUIET), per faction. Hard to change without side effects.
- **Possible bugs seen in passing** (not checked in a game, listed only so that we do not copy the ideas):
  - an adjacent-zone sum that adds the zone's own value each time;
  - an AA threshold that always comes out the same;
  - an island AA sum that reads the wrong zone;
  - a scouting-interval change that never reaches the table;
  - an experimental's target health that adds the attacker's own shield.
- **Intel shortcuts:** the start-of-game map reveal, and likely full knowledge from radar blips (2.4).

### 5.2 What makes it heavy (ties in with the engine's measurements)

AI games are about half Lua, and 80-86% of that Lua is M28 (`benchmarks.md`). Its biggest costs, and why:

| Measured cost | Why, from the reading |
|---|---|
| Category checks (`EntityCategoryContains`, `EntityCategoryFilterDown`) | Every zone pass sorts its units by category with long if/else chains, and filters the same lists again and again, once per job in the engineer list, once per threat kind. Hundreds of zones, every second. |
| Zone and label lookups | "Which zone is this position in?" asked millions of times an hour. Each answer was a walk down FAF's navigation-mesh tree in Lua. The variant now caches the label per grid square. Every cycle re-zones every known unit. |
| `ManageSpecificLandZone` | One new thread per zone per team every ~1 s. Per unit: category branches, distance checks to nearby enemies, loops over neighbouring zones inside loops over units, and support searches over all zones of the plateau. |
| `ManageBombers` | Every second per air subteam: gather all bombers, walk every zone in distance order filtering enemies one by one, check AA along each candidate path (some per AA unit with geometry), engine area searches, then targets times bombers to assign. Engineer-hunting bombers each do their own walk. |
| Buildable-location checks | After every new structure, every grid square around it is re-checked for every building size: about 15,000 engine calls each time (cached in the variant now). The build-spot choice adds more checks per candidate. |
| `SetupMap` at the start | Navigation mesh, flood fills over ~10^5 cells with path checks, many path searches, travel distances for every pair of zones, ~93,000 build checks. About 3 s of frozen picture in total. |

General pattern: the same questions are recomputed from scratch in Lua many times a second, by many independent
threads, with no shared index of units by zone and kind.

## 6. How FAF's base AI differs (why it feels worse)

FAF's own AI (`/lua/AI/AIBuilders/`, `PlatoonTemplates/`, `/lua/aibrains/`; the lobby's easy, medium, adaptive,
rush, tech and turtle, plus their cheating versions) is built the other way round:

- **Builders with priorities and conditions.** Each factory or engineer task is a "builder": a template, a
  priority, and a list of conditions (game time, unit counts, economy efficiency). Managers pick the highest
  builder whose conditions hold. This is a good, simple framework, but the conditions are mostly counts and
  timers, not "what is happening on the map".
- **Platoons.** Units are gathered into platoons by templates and sent off with a behaviour. The newer adaptive
  platoons are state machines (searching, navigating, attacking, raiding, retreating). Once sent, a platoon mostly
  follows its own plan. There is no per-zone re-thinking every second, and no shared "this zone needs help" view.
- **Threat** comes from the engine's built-in influence map (threat around a position), not from its own zone
  picture with ranges.
- **Weaker skills:** little kiting, little range awareness, few build orders tuned per map, simpler expansion,
  weaker air and naval use, no dodging, no bomber snipes. Its difficulty mostly comes from resource cheats (AIx).
- **Why it feels worse:** it is predictable and slow to react. Players learn to beat its patterns.
- **Its one strength:** it is cheap. Our MP1 game with real players is only 14% Lua.
- **Other AI mods** in the lab's mod folder: RNGAI and AI-Uveso. They are installed but **not studied here**.

## 7. Ideas for our own AI: PROPOSALS FOR A TALK WITH CHRIS, NOT DECISIONS

Everything below is a suggestion to discuss. It is written from general RTS-AI knowledge and from what M28 shows
works. Our own code would be designed fresh, not ported.

### 7.1 Ideas worth keeping (as ideas, in our own form)

- **The map as zones around resources**, with adjacency and travel distances: the right unit of thought for this
  game.
- **Re-deciding often from a shared picture**, instead of fire-and-forget platoons.
- **Threat split by kind and by range**, so that "can we outrange them?" is a cheap question.
- **Jobs measured in build power**, with unmet demand published so factories and other zones react.
- **Upgrades only when safe and affordable.**
- **Remembered orders**, so that units are not re-ordered every cycle.
- **Retreat by a safe path to a sensible rally point.**
- **Map-specific openings** and **reacting to what was scouted.**
- **Team-shared knowledge** for allied AIs.

### 7.2 What to do better

1. **A strategic layer above the zones (the fix for section 4).** Once every few seconds, look at the whole
   front:
   - our army against the enemy's army;
   - our production against theirs;
   - our unit-cap use;
   - eco lead, tech lead, map control.
   It sets an **aggression level** that the zones obey, and picks **one or two objectives** (an expansion, a
   firebase, the enemy base). It gathers armies for them instead of answering every alarm.
2. **Pressure that grows with the advantage.**
   - Slightly ahead: hold ground and raid.
   - Clearly ahead: take the enemy's expansions.
   - Far ahead, or at the unit cap: a **decisive push** with everything and reinforcements streaming in, until the
     advantage is gone.
   - Commit with **hysteresis**: once committed, stay committed until the expected trade turns bad, not at the
     first threat jump.
3. **Never flee from a clearly weaker force.** If the local battle estimate says we win easily, we fight, even if
   the enemy outranges us, and the size of the advantage turns kiting off. Kiting is for even fights.
4. **A better battle estimate than summed mass.**
   - Use a square-law style strength (numbers count more than linearly for ranged units), plus damage per second,
     health, range and time to close the distance, and our reinforcements on the way.
   - In native code we can afford a tiny **fight simulation** ("if both sides fight here for 20 s, who is left?")
     for the few fights that matter each second.
5. **Team-wide focus fire** where it pays: shoot the targets that die fastest per unit of value, spread over
   targets to avoid overkill, and keep shooting the same target as the group.
6. **Air fear in proportion.** Bring AA with the army, or answer enemy air with our own air, instead of turning
   the whole army home.
7. **Fair intel.** Use only what a player could know: radar blips as blips, no map-wide reveal (or make it an
   explicit cheat option).
8. **Fewer special cases, more data.** Unit roles and numbers come from blueprints, not from per-unit branches. This
   makes mods and our remastered units work without code changes.

### 7.3 What native code can make cheap

- **Whole-map analysis at load time** (zones, adjacency, travel distances for every pair of zones, chokepoints, build
  spots) in C++, during the loading screen, a few hundred ms instead of 3 s of frozen picture. It could even be
  cached per map file.
- **Threat and influence maps** kept up to date by the engine as units move. Every query is then a table read.
  This means an index of units by zone, army and kind, updated by the engine as units move, so that "enemy direct
  fire in zone 12, by range band" costs nothing.
- **Path questions:** "is this path safe?", "the cheapest path for this threat level", "the travel time from A to
  B", answered in native code on the engine's own path grids.
- **Build-spot queries** ("the best free spot for a 4x4 next to power") from an engine-side occupancy map.
- **Battle estimates and small fight simulations** run in parallel on the multicore work.
- **Lua stays as glue:** personalities, difficulty settings, special mod rules, chat. The heavy thinking runs in
  C++ and hands Lua small, ready answers.
- **Determinism:** whatever runs natively must give the same answer on every machine, as the sim does, since the AI
  is part of the sim.

### 7.4 Difficulty for beginners and pros (fair play first)

The idea: one brain, and difficulty changes **how well and how fast it plays**, not how much free stuff it gets.

| Knob | Beginner | Pro |
|---|---|---|
| Reaction delay (time from seeing something to acting on it) | Seconds | A fraction of a second |
| Attention (how many fronts or zones it can manage per second, like a player's actions per minute) | One or two at a time | Many |
| Micro (kiting, dodging, focus fire, bomber snipes) | Off or rare | Full |
| Scouting and intel use | Scouts late, forgets what it saw | Scouts on timers, remembers, predicts |
| Build order and economy | Loose, with idle time and some floating resources | Tight openings per map, no stalls |
| Aggression | Attacks late and telegraphs it; retreats early | Pressures from minute one, punishes mistakes, decisive pushes |
| Mistakes on purpose | Sometimes over-extends, sends units into point defence, forgets AA | Rare |
| Adaptation | Fixed style | Counters the player's army mix and changes plan |

- **Mistakes must look human.** A beginner AI should lose because it plays like a beginner (late, unfocused,
  greedy), not because it stands still or makes nonsense moves.
- **Resource cheats only as an extra, clearly named option** (like today's AIx), never as the main way to make it
  hard.
- **Optional "teacher" style for beginners:** it can ease off when far ahead, so a new player is not crushed in ten
  minutes. This could be a lobby option.
- **Personalities** (rusher, turtle, air player, naval player) on top of any difficulty, so games feel different.
- **"Cooler to watch":** the strategic layer can announce its intentions in chat ("taking the east expansion"), make
  visible pushes, and react to the player's big moves.

### 7.5 Open questions for the talk

- Do we keep M28 (our variant) as an option beside our own AI, or replace it fully once ours is good?
- Which difficulty levels do we name, and is "fair intel" the default for all of them?
- How much of the first version is native? A proposal: map analysis, the unit index and threat maps first, the
  rest in Lua until it settles.
- What do we test against? A proposal: our AI against M28 and against FAF's AI on SETONS_R, with the same seeds,
  scored on win rate and on Lua and native cost per tick.
