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
