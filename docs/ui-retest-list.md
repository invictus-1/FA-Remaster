# The re-test panel and its list (ui, 2026-11-11)

Chris's pick of 2026-11-09 (the review's idea 12, "What's new: try it"): a small panel of our own that reads the
re-test list, shows the items for the screen the player is on, and takes Pass / Fail / Skip and a short note into a
file. This page is for whoever writes a list or starts a test session. The code: `ui/user/retest.h`,
`ui/data/lua/retest.lua`; the tests: `ui/tests/retest.lua` with `ui/tests/retest_list.md`.

## Starting a session
- Add `--retest <list.md>` to the launcher's line. Without the option nothing of the panel exists: no key action,
  no console command, `moho64ui_RetestList()` is nil. A shipped build never gets the option.
- The panel is open at the first start. Ctrl-Shift-R closes and opens it (the key action "Re-test list" in the
  game's Key Bindings, so it can be moved; the console's `remaster_retest [on|off]` does the same). Its title bar
  drags it. Open or closed, its place and its drop-down's choice are kept in the profile, and it comes back over
  every screen that is made (the main menu, a game, the main menu again).
- The verdicts are in `<list>.verdicts.md` beside the list, written at every click. The list itself is only read.
  A second start reads the verdicts back, so a session can go on over several runs.

## Writing a list
A list is a Markdown file. The panel reads:

| In the file | In the panel |
|---|---|
| `## heading` | a group (a heading with no item under it shows nothing; `#` and `###` lines are not groups) |
| a row of a table under it | an item |
| the column named `What` or `Item` | what to try (a table without such a column is skipped: a keys' sheet, numbers) |
| the column named `How to see it` or `How` | how to see it; in a table of two columns the other column, whatever its name |
| the column named `Where` (optional) | the screens the item is shown on: `menu`, `lobby`, `game`, `replay`, `options`, `score`, comma separated; empty or no such column: every screen |
| other columns (`Chat`, `Status` ...) | left out |
| `` `backticks` `` and `**bold**` | plain text, the marks taken away |
| `\|` inside a cell | a bar |

- A table needs its rule line (`|---|---|`) under its first row, as Markdown wants it.
- An item is known by its heading and its first cell: change either and its verdict no longer finds it (the verdict
  stays in the verdicts file, under the old heading, and is never dropped). The same first cell twice under one
  heading is shown as "... (2)".
- The file is read again while the program runs whenever it is saved, so a list can be corrected during a session.
- A `Where` column is worth adding: without it every item is shown on every screen, and "the items for the screen
  you are on" is the whole list.

## The verdicts file
```
# Verdicts: RETEST_LIST.md

## 1. Fixes to check
| Verdict | What | Note | When | Build |
|---|---|---|---|---|
| Pass | Kennel / station drones fly out and assist |  | 2026-11-11 05:40 | 5fa10ae1a524, moho64ui |
| Fail | Fog lower and see-through | still grey at dusk | 2026-11-11 05:44 | 5fa10ae1a524, moho64ui |
| - | Load time (~1 min) | 40 s here, did not time it properly | 2026-11-11 05:51 | 5fa10ae1a524, moho64ui |
```
One row for every item with a verdict or a note; `-` is a note without a verdict (kept, not counted as done). When
is the PC's clock at the click, Build the program that was running (`moho::BuildStamp()`).

## What the panel does to the game
- It takes no key: only its note line, once clicked, has the keyboard (Enter keeps the note, Escape gives the kept
  one back, a click elsewhere keeps it too). It never pauses the game.
- While a dialog of the game holds the input (the Options dialog, the score screen), the panel's own controls still
  get the mouse (`maui/control.h` `overCapture`, set only while the panel is open): this is ours, the original gives
  such a dialog every click.
- It comes above the screen when it opens, when the screen under it changes and when it is clicked; a drop-down or
  a dialog opened after that lies above it, as over any window.
- Once a second while it is open it walks the head's controls to see which screen is up (the Options dialog's tabs,
  the lobby's group); closed, it costs nothing.
