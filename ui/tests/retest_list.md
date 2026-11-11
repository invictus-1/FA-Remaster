# A list for ui/tests/retest.lua (the re-test panel's reader; not a real list)

Prose before the first heading, with a | bar in it, is no table. A run copies this file and gives the copy to
`--retest`: the verdicts file is written beside the copy, never here.

| What | How to see it |
|---|---|
| A row before any heading | it stands in a group without a title |

## First group (two columns)
Some prose under the heading.

| What | How to see it |
|---|---|
| Plain item | look at it |
| Item with **bold** and `code` marks | press `Ctrl-Alt-S`, then **look** |
| A bar \| inside a cell | escaped as written |
| Plain item | the same first cell again: its own id |

A table of another kind, skipped (no What or Item column):

| Key | What it does |
|---|---|
| Ctrl-Alt-S | Performance overlay |

| 1 | 2 | 3 |
|---|---|---|
| 4 | 5 | 6 |

## Second group (with Where)
| What | How to see it | Where |
|---|---|---|
| Menu thing | on the main menu | menu |
| Lobby or menu thing | in the lobby | lobby, menu |
| Game thing | in a game | game |
| Game and replay thing | a game or a replay | Game, Replay |
| Options thing | the Options dialog | options |
| Score thing | after a game | score |
| Everywhere thing | anywhere | |
| A typo in Where | still shown everywhere | gmae |

### A smaller heading is no group
| Item | Chat | Status | How to see it |
|---|---|---|---|
| Four-column item | ui | DONE abc1234 | by its column names |
| Another four-column item | engine | landed | `how` text |

## Empty group
Only prose here.

## A table without its rule is no table
| What | How to see it |
| not | a table |

## Columns in another order
| Where | How | What |
|:--|:-:|--:|
| game | the how cell | Reordered item |
