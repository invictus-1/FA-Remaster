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
    reach them. So the navy comes as its own phase, with the air answer as part of it (not yet in the game).
