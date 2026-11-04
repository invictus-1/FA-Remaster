# Ultra Shields fix (FA Remastered)

A fix for FreyaGroup's **Ultra Shields** mod, version 3.5 (uid `E08710C8-B785-11E4-9CA4-35FF1C5D46B0`). It contains
none of Ultra Shields' files and needs it to be installed and enabled. Following the lab's rule for mod fixes, the
original mod is never changed in place; this fix sits beside it and changes two blueprints after they load.

## What it changes

Ultra Shields has two UEF experimental shields, both called "ESD Pulse":

| | feb4401 | feb4402 |
|---|---|---|
| Shield | 150,000 | 150,000 |
| Hull | 5,000 | 7,000 |
| Mass | 19,800 | 19,800 |
| Build time | 18,875 | 13,215 |
| Built by | T3 engineers, commanders | upgrade from the T3 heavy shield only |

Chris (2026-11-03): keep the stronger feb4402 and take feb4401 out of the game; T3 engineers and commanders build
feb4402 directly as well. So:

- feb4401 loses its BUILTBYTIER3ENGINEER / BUILTBYTIER3COMMANDER categories: nothing builds it. Its blueprint stays
  loaded, so anything that names it (a replay, a unit already standing) still finds it. (Ultra Shields' Sorian AI list,
  `mods/shields/lua/customunits/ShieldGenerators.lua`, names it too, but sits below the folder Sorian AI reads,
  `<mod>/lua/CustomUnits`, so no AI ever read it.)
- feb4402 gains those two categories; the T3 heavy shield still upgrades to it, as the mod sets.

## Games and replays

The fix changes the game, so it is a mod of its own, recorded in each game's mod list like any other. Replays of games
played without it play without it. The interface adds it to new games where Ultra Shields is enabled.
