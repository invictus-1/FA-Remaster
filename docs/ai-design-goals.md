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
