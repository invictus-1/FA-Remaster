# Our own AI: design goals (Chris's direction)

Chris's direction for FA Remastered's own AI, which replaces FAF's base AI and goes beyond M28. Written down on
2026-10-28 from his words in the engine chat. The study behind it is [ai-study-m28.md](ai-study-m28.md): how M28 plays
and why it holds back. Our AI takes ideas only, never M28's code (CC BY-NC-SA, see CLAUDE.md).

The core of it, in Chris's words: proper tactics instead of "oh no a T1 tank is approaching, let's all retreat while I
have 500 Percivals and 5 Fatboys".

## 1. Understand where the battles are

The AI knows which of its forces can actually meet which of the enemy's, and judges each matchup on its own.

- An enemy navy that cannot reach land is no reason for our land forces to hold back.
- Strong navy against a weaker enemy navy: push and win the sea, then move resources into land.
- Each domain (land, sea, air) is judged against what can really reach it, not against the enemy's total.

## 2. Combined arms

- Joint attacks: land and navy together, followed by air support, so the forces together are stronger than any one
  alone.
- Supporting fire to crack a defensive line so an attack force can go in: artillery, navy and bombers giving that
  extra fire.

## 3. Cracking a fortified line

Example: land is pinned down by layers of T2/T3 point defence and T2 artillery.

- With navy available:
  - long-range ships poke at the enemy;
  - heavy ships such as battleships break the shields and destroy the line;
  - then land and air attack, or all three together, so the enemy loses the advantage of his defences.
- Without navy: build large groups of units to keep pressure on.

## 4. When the enemy is too strong to attack head-on

Example: too much point defence, anti-air and everything else.

- Build a gigantic force and turtle, while one or more of:
  - building nukes;
  - hit-and-run attacks to slowly wear the shields down;
  - a combined breach:
    - sacrificial units first (a group of Bricks and an experimental) to weaken the shields;
    - then a group of T3 bombers to take out key points and large chunks;
    - then the main force pushes in.

## 5. Never afraid of a clearly weaker force

- Pressure grows with the advantage.
- A decisive push when far ahead stays committed.
- A big army never retreats from a small one.

## 6. Fun for beginners and pros

Difficulty by:
- reaction speed;
- attention;
- micro skill;
- deliberate mistakes;
- aggression.

Resource cheats only as a separate, named option. See ai-study-m28.md section 7.4.

## Next

A design talk with Chris, using these goals and ai-study-m28.md section 7.5's open questions, before anything is built.

## Decisions (design talk with Chris, 2026-10-28)

1. **M28 stays beside ours.** Ours becomes the default and replaces FAF's base AI; M28 (our variant) stays in the
   lobby as an option and is ours' sparring partner in tests.
2. **Difficulty = skill levels with fair intel on all of them.** Levels change reaction speed, attention, micro and
   aggression. Resource cheats and map vision are separate, named options.
3. **First build: the core native, Lua as glue.** Map analysis, the unit / threat index and battle estimates in C++
   first; strategy and personalities in Lua until they settle, then their hot parts move native.
4. **Extras, all in the first version:**
   - **Personalities** (rusher, turtle, air player, naval player, ...) on top of any difficulty.
   - **Announces its plans, to allies only.** An enemy announcing its moves would spoil the fun.
   - **Teacher mode** (lobby option): eases off when far ahead, so new players are not crushed.
   - **Team play with allies is key.**
5. **Pings understood properly** (Chris's words, summarised):
   - A ping on nothing, or made by accident, is recognised as such and ignored.
   - "Assist" at the player's base or near a force with enemies nearby: the AI sees its ally is under attack and
     sends support.
   - "Attack" on an enemy base: no suicide rush; the AI looks at what is there, picks a sensible target and diverts
     a fitting share of its attention and forces to that area.
   - Wanted if possible: more detailed pings, e.g. "attack the ACU", "attack with an experimental", "assist my force".
     These need new ping types in the interface (ui chat).
6. **Detailed pings by a radial menu:** hold the ping key and drag toward an option (Attack, Assist, Defend, Attack
   ACU, Experimental here, Expand here, Retreat, ...). The AI reads the ping's type and what lies under it.
7. **Difficulty names:** Recruit, Veteran, Commander, Supreme Commander.
8. **Testing:** headless matches against M28 and FAF's AI on SETONS_R with fixed seeds, scored on wins, on how it
   fights (no fleeing from weaker forces, pushes when ahead) and on its cost per tick; then Chris plays it and judges
   the feel.
9. **Timing:** start now, with dedicated helpers, beside the 4v4 and multicore work; the ui adds the ping menu when it
   has time.
10. **Unit threat values (Chris's idea, 2026-10-28):** each unit gets a power rating; a group's ratings are summed and
    compared with the enemy's. Planned form: the rating is worked out from the blueprint per domain and target type
    (damage per second vs ground / air / naval / structures, health + shields, range, speed, cost), so every unit
    and mod unit has one; fights use a square-law estimate (and later a small native fight simulation). Chris's
    knowledge is the tuning layer: once the battle estimate exists, the computed rating of every unit is printed
    side by side, Chris corrects the ones that feel wrong (a multiplier per unit) and adds "A beats B" overrides for
    matchups he knows.
    Chris's addition: a short readable label per unit for the tuning list, tier + domain letter + value, e.g. "T1G 1.0"
    (G ground, A air, N naval, S structure, X experimental), so a list of ~400 units is quick to scan and correct.
11. **Threat and build value are two different numbers (Chris, 2026-10-28):**
    - **Threat** is raw: what the unit can do (damage, health, shields, range, splash, speed), never its cost. It
      answers "how dangerous is this force".
    - **Cost per mass** belongs to the build choice only: "what should I build against what the enemy has". Example:
      against a mass of GCs, Percivals per mass are a good answer for their single-target killing power.
    - A specialist is not marked down in threat for being a specialist: the Percival's group score suffered from
      overkill against small (500-health) targets, which said little about a T3 fight. Its threat should come out
      much higher than the first list's 14.1 (below the Harbinger and the Othuum). In fights the threat is judged
      against the enemy's real composition; for the printed list, against targets of the unit's own tier.
12. **A score per target size (Chris, 2026-10-28):** each unit carries a group score against an army of T1, of T2
    and of T3 units, and a single-target score against a T1, a T2, a T3 and an experimental target. The AI then sees
    WHY a unit's threat is low or high (a group killer, a single-target killer) and can pick what takes out the
    enemy's group. These are worked out once when the blueprints load (a small table per unit), so they cost nothing
    while the game runs.
13. **Min-max the economy, and counter cheaply (Chris, 2026-10-29, after the first duels):**
    - The economy is driven hard from the first second, at the same time as scouting, building an army and expanding,
      never one after the other.
    - Counters are picked for the least mass that beats what the enemy fields. Example: against a group of Mech
      Marines, a group of Strikers wins. To save mass, a few T1 point defences and a few T1 artillery can hold a
      point instead.
    - So defences are build candidates beside units: cheaper per threat, but fixed. They are worth it only where the
      enemy will come (a choke, an expansion under raids).
    - This is tuned step by step from the duels.
14. **Min-maxing is not fear of the yellow (Chris, 2026-10-29):**
    - Running low on mass or energy (the economy bar's yellow) for a short while to build things quickly is fine.
      What is wrong is staying negative the whole time.
    - Investments are judged by risk against reward. Example: the risk is a short mass dip for 2 minutes, and the
      reward is a doubled mass income. Upgrading extractors, more extractors, experimentals and tech are all worth a
      dip when the payoff is better.
    - Before the dip, it judges the danger from the map and the enemy units it knows. If it does not know, it scouts
      first. If it judges the moment safe, or safe enough, it invests.
    - A big enemy army is no reason by itself to hold back the economy. If no one is attacking, there is no reason to
      hold back, so it never sits there without teching up.
15. **Read the enemy's play style and adapt (Chris, 2026-10-29):**
    - It works out from what it scouts how each enemy plays: turtling or ecoing, rushing, teching, air or navy heavy.
      It judges the enemy's economy and tech against its own (extractors and their tiers, power, factories' tier, the
      units appearing).
    - **Against a greedy, eco-ing or turtling enemy:** it decides whether to attack and harass to stop them, or, if
      it judges it can out-eco them and win later, to go full throttle into economy and defence before attacking.
    - **When it is behind:** example: the first 10 minutes are T1 against T1, then T2 units appear. It scouts. If
      the enemy's economy looks strong (many T2 extractors, T2 power) while it is still on T1, it builds a defence
      force first, then goes into full economy to catch up. Or it keeps its existing force in a defensive role to buy
      time until it can match: a few point defences, a rush to T2, point defences at T2, while it ecos.
    - The read is re-checked as the game goes on, and every switch is logged with its reason.
16. **The whole picture, and few knobs (Chris, 2026-10-29):**
    - The aim: it plays like a real player, from beginner to pro to beyond a pro. It is fun and challenging, never
      scared to fight, and always knows what it is doing. Chris "always hated playing against AI" that was scared or
      lost. Ours is the answer to that.
    - It stays efficient: the hot parts native, its cost per beat measured in every duel.
    - **Player knobs, later, kept few:** a small number of sliders the player can drag, such as more defensive / more
      aggressive (and perhaps eco against army). Each one moves a weight the AI already uses (commit margins,
      investment risk, defence share). None replaces the AI's own judgement. No wall of 50 options that break the AI
      or do nothing, as M28 has. The AI does the deciding; the knobs only lean it.
    - The design keeps absorbing Chris's ideas: each new idea is merged into these decisions (built, changed or
      removed), so the AI stays one coherent player, not a pile of features.

## Changes the duels made to these goals (helper aicore, 2026-10-30)

Chris, 2026-10-30: the goals are an idea, not a cage; where the evidence shows a better way, change them and write down
why. The rules that stay: our own design (no M28 code or logic), exact and replay-safe, no cheating beyond the lobby's
options, and never fleeing a clearly weaker force unless it can be shown to cost games. Each change below names what
the duels showed. The duels are 1v1 on SETONS_R against FAF's adaptive AI and M28, with fog of war, several seeds.

17. **Tests play with fog of war (changes decision 8's setup).** The first duels had `FogOfWar = none`, where every army
    sees everything and scouting cannot matter. With fog the AI's picture of "ahead" broke: after its scouts lost sight
    of the enemy's army it saw ratios of 20-50 and sent decisive pushes into it. So what the AI has seen of the enemy
    now fades slowly instead of vanishing, and every duel since phase 6 plays with fog.
18. **The army is kept by our own economy too, not only by the enemy's army we know (refines 13 and 15).** Under fog the
    known enemy army is a fraction of the real one; at minute 10 our army was 500-1,800 mass against 2,700-6,000 while
    our economy was ahead of FAF's. The army kept now also grows with our income after the opening. With it, all six
    games of the 20-minute set were alive at minute 20 instead of three.
19. **The opening is its own plan (refines 13 and 14).** "Economy in parallel with everything" left the energy bar
    empty for half of the first five minutes and the extractors waiting behind power and factories. The opening now
    puts extractors first, then a power curve, and makes factories and upgrades wait for energy; the mass at minute 10
    doubled (56 -> 114 a second, above FAF's AI in every seed).
20. **A fight the real numbers say we are losing ends, whatever the estimate says (refines 5).** Decision 5 and the
    never-flee rule stay for forces the AI knows are weaker. But under fog the estimate only sees what the army has
    seen: against M28 an attack stayed on for 3.5 minutes at an estimate of 0.8 while losing 10-20 times what it
    killed to artillery and missiles it never saw. So every minute of an attack the mass lost is set against the mass
    killed, and a clearly bad trade ends it (a "withdraw", not a "retreat": the enemy there is not weaker, only
    unseen). In the duels it fires 0-2 times a game; faults (retreats from a clearly weaker force) stay at zero.
21. **The commander is kept safe, and does not wander (new).** Games were lost when the commander died far from its
    base (reclaiming 180-270 units out, or shelled by a frigate at its work). It now keeps to its base for field
    jobs and backs away when hurt.
22. **Ships shelling the base get an answer from the sea and the air (Chris's choice, 2026-10-30).** Against M28 one
    modded ship shelled 13,400 mass of our coastal base in 20 minutes with nothing of ours able to reach it. A first
    form of the air answer (the best anti-ship aircraft by build value, sent at ships seen off our coast) halved that
    ship's damage in one game but did not change the 20-minute set: under fog the ships are seen late, and few aircraft
    reach them. So the navy came as its own phase (phase 10, 2026-10-31), with the air answer inside it: naval
    factories by our shore while the enemy has ships, ships by build value that defend the coast, attack the nearest
    ships the estimate favours, else hold. Observed M28 games had about half of its army at sea.

23. **Reclaim is income, not a side job (refines 13 and phase 6's map control; 2026-11-01).** The army-first rule
    (only every third factory makes engineers while the army is short, up to a small floor) held us at 25-37
    engineers. An income probe showed that M28's lead of twice our income from minute 12 was not its extractors -
    even with ours at minute 10 - but reclaim: 27,000-32,000 mass by minute 15-17 with about 140 engineers, much of
    it the wrecks of the fights at our own base. So the reclaim within reach of the start now earns its engineers
    whatever the army's state, one per 250 mass, at most 40. On the judge set the M28 games last a median 28.2
    minutes instead of 22.6 and the FAF veteran games 41.2-41.5 instead of 27.8-42.4. Reclaiming our own base's
    wrecks quickly also takes that mass from the enemy.

24. **Mass that piles up goes into extractor upgrades before factories (refines 14; 2026-11-01).** Decision 14 weighs
    each upgrade by payback against danger; the duels showed our mass bar full at minutes 10-13 while 1-5 upgrades ran
    and up to 38 T1 land factories were built as the sink, against M28's 9-15 upgrades at once. Now the overflow is an
    investment first - cheapest payback, nearest the base - and only when no upgrade can start does it build factories.
    Our income against M28's rose from half to 0.83 at minute 15 and from 0.18 to 0.54 at minute 20 (judge set).

25. **Team play: an ally's attacked zone is ours to defend, and a big army is used (Chris's big test, 2026-11-03).**
    In his 4v4 the allied AI built a massive army beside an ally under attack by a small one and did nothing; armies
    of hundreds sat at home. Decision 4's "team play is key" now has its first form: an ally's zone within reach that
    enemies are in is defended like our own when the estimate lets us. The sitting was mostly a fault (stale orders,
    staging zones beside the enemy's base or inside our own), not a choice. And an army stops growing at a share of
    the unit cap (T1 first): hundreds of T1 units were spam that slowed the game, not strength.

26. **The opening keeps the commander's power before its extractors; the base is built compact (Chris's big test,
    2026-11-03).** Chris's wish: "its first job should be to build a factory, then extractors, then expand like a
    player", and "use adjacency, build at the starting point". Tried as asked - factory, then the start zone's four
    extractors, then the commander's two generators - the energy stalled: in the 4v4 of all our AI on SETONS_R (his
    game's session and seed) the eight armies made 23 mass a second together at minute 2 instead of 62, and 188 at
    minute 6 instead of 447 (with power first: 58-62 and 226-447, as before). So the order stays factory, two
    generators, then the four extractors right after; the rest of his wish holds: the base sites go nearest the start and the generators ring our first factory (adjacency), and
    only a few base structures are started at once. At minute 6 the structures stand 24-32 world units from the start
    on average (30-54 before), with the same income.

27. **An empty mass bar is not a deficit; the dip is planned (refines 14; the coordinator's round of 2026-11-04).** In
    the 4v4 of all our AI (Chris's session) the mass bar sat at 0-4% from minute 6 because every bit of income was
    spent, and decision 14's "sustained deficit" rule read that as a stall: five of the eight armies stayed on T2
    extractors from minute 15 to 35, the T3 upgrade held 200 times. Now one extractor upgrade at a time may go past the
    budget and the empty bar when it pays back within 10 minutes and costs at most 2 minutes of income ("the risk is a
    short mass dip, the reward a doubled mass income"), from minute 8. At minute 25 the eight armies made 1,317 mass a
    second instead of 726, with 48 T3 extractors instead of 11.
28. **An experimental is bought like any unit, by build value (refines 13 and Chris's big test).** It was built only
    from piling mass, which an economy that spends everything never has. Now it is bought instead of units when its
    build value against the enemy's known army is at least half the best T3 unit's and the income carries it (5
    minutes of it), the base is safe, and the T1 factories pause while it is built; with piling mass or the army at its
    cap, the best experimental goes whatever its value. Mod experimentals count. The T1 factories beside a T2 / T3 HQ
    upgrade to support factories (one army kept 18 T1 factories beside its T3 HQ for 15 minutes).
29. **What the radar shows is a place to look, not nothing (refines 17).** Under fog a contact of unknown type (a radar
    blip of a unit never seen) counted in its zone with no mass and no place, so the defence sent one unit to the
    zone's centre while M28's artillery shot our extractors from beyond their sight (the kill probe: 62% of our lost
    mass in the first 15 minutes killed by units we did not see). Such contacts now have their place (the defence and
    attacks aim at them when nothing else is known there) and count at the mean mass of the enemy's known land units.
30. **The commander stays home from the mid-game (refines 21).** Every duel against M28 ends when our commander dies;
    it died far from its base at a job or an assist, 85-165 world units out, to gunships and T2 bots. From minute 10
    its work keeps within 60 world units of the start. The duels lasted about as long: it now dies inside its base
    to gunships, or at its refuge from ships off the coast (the base zone they reach least, up to 200 out) - that
    refuge is the next thing to fix; anti-air at the base alone did not change the gunships' toll.
31. **Build orders, then expansion and adapting (Chris, 2026-11-04).** Every player and M28 play a build order, so
    the first 2-3 minutes are mostly the same for everyone; the difference is how a player adapts after that. The
    extra engineers' job is mass expansion from the start. With allies present, the AI does not build in its allies'
    bases; with no ally near, it expands and explores as much as it can while it fights, into every part of the map and
    every domain (land, navy, air). The opening becomes a book (the pros' first minutes per faction and map size, from
    high-level FAF replays; M28's and Seton's replays' first orders until then), and the expansion engineers claim the
    nearest free mass points outside allied start areas in waves along safe paths, with the army's front covering
    the expansion zone.
32. **What does not decide the duels against M28 (aicore3's rounds 2-4, 2026-11-04).** Each was probed, changed and
    judged (8 M28 recruit duels, 2 FAF veteran), and none moved the result beyond the spread, so the cause lies
    elsewhere:
    - **the commander's refuge from ships:** moving it inside the base and out of the known ships' reach only moved
      its death from 28-165 world units from the start to 0-47 - the base itself falls at minutes 19-28;
    - **seeing the artillery:** radar over the economy zones and a scout to hits by the unseen; half the artillery
      that killed our mass was already known on radar, and acting on "outranged and shelled" found almost no fight
      we could take (the hold-backs' margins were 0-0.1, the overall ratio about 0.13);
    - **the engineer count:** engineers by income (62 at minute 15) or free of the army's floor (104) did not raise
      the extractors held: 35-36 at minute 15 and 17-20 at minute 20 in every form, against M28's 62.
    The mass point probe (98 points our land units reach on SETONS_R) shows the race even until minute 10 (we led 29
    to 19 at minute 6 and 46 to 36 at minute 9; 44 to 48 at minute 10 over the judge set), then M28 takes the last
    free points and our extractors fall to its raids. A fight bench (scripted army against army at equal mass, both
    sides human, no AI) found our counter-picks winning against M28-like mixes holding their ground: our T2 missile
    launchers lost 0.0-0.6 of what they killed against T2 bots, T1/T2 artillery and mobile shields, the AI's T2 mix
    0.2-0.6, T1 tanks 0.8-1.6. So what loses the duels is how and where the army fights, not what it builds.
33. **The battle estimate is measured against real fights (refines 10-12; 2026-11-04).** Every defend, attack, hold-back,
    post and investment decision reads the battle estimate, and a fight bench (two forces of equal mass, both scripted,
    no AI) found it wrong by 0.3 in the margin on average, its sign flipping with the composition: T2 tanks against T1
    artillery 0.85 against a real 0.13, the AI's own missile-launcher mix against T2 bots 0.16 against 0.54-0.90. So
    the AI held back from fights it would win and walked into fights it would lose. The estimate now models what the
    bench showed - each stack fights from its own place (the short-range units run ahead, the launchers stay back),
    shells miss what walks and hit what stands, shells that split count their fragments, a defender stands its ground -
    and the bench's matrix (with pairs held out) is its test: the mean error fell to 0.15 in meeting fights and 0.08
    against a defender. Against M28 the duels lasted about as long and traded better (lost per killed 9.0 -> 5.7), the
    FAF veteran games 5-9 minutes longer.

34. **Engineers reclaim only in controlled space (Chris's re-test, 2026-11-06; refines 23).** Chris saw the engineers of
    every army scattered over the sea and in enemy bases, "a crap ton just going across the ocean to I guess reclaim".
    A probe of the all-AI 4v4 found up to 38 of our engineers on the water at once, most of them on reclaim trips, and
    up to 57 nearer an enemy start than their own: the reclaim map checked only that a field was reachable by land and
    had no enemy known in it now. His rule: reclaim only where we hold the ground. So a zone is a reclaim target only
    while nothing hostile was seen within 120 of it for a minute and it lies away from every enemy start; the trip must
    pay for the walk (more mass the farther it is), take at most 3 engineers to a zone, and go by land (no shortcut over
    the sea); engineers whose zone stops being safe fall back at once. Asking also for a force of our own there (3-6
    T1 tanks) cut the duels' reclaim by up to 30% and they ended earlier, so the force is not asked. Against M28 the
    duels lasted as long (26.6 minutes against 27.5) and traded better (lost per killed 7.3 against 10.1), with 20%
    fewer engineers lost (42% in the first 15 minutes) for 3-11% less reclaim; the FAF veteran games lasted 53.3 and
    40.2 minutes against 37.7 and 42.2. In the 4v4 the engineers away on the water fell from 128 sightings to 36, and
    those nearer an enemy start from a mean of 12 to under 1.

