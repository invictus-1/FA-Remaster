# Deliberate departures from the original

The engine's rule is to do what the original does, mistakes included, because a mistake is often what
other things were tuned around. This file lists the few places where it does something else on purpose,
and the things that look broken and were left alone, with the reason.

Every departure sits behind `Sim::fixes`. It is **on in a new game** and **off while a replay is played
back**, so a recording made by the original still plays as the original did (the replay check uses
exactly that). `MOHO64_FIXES=0` or `=1` in the environment overrides it for a test.

## Departures (on with `Sim::fixes`)

### 1. The unit state "Sacrificing" (2026-10-12)

- **What is broken:** FAF's `lua/sim/Unit.lua` calls `self:SetUnitState("Sacrificing", ...)` as the first
  line of `OnStartSacrifice` and of `OnStopSacrifice`. The original engine has no unit state of that name
  (the 45 names are listed in `src/sim/recorder.cpp`; the word is in neither the stock nor the FAF
  executable), so both callbacks stop there with `Invalid enum value Sacrificing`. The sacrifice still
  works, because the engine itself adds the build progress and destroys the unit; what is lost is
  everything after that line: the sacrifice effects, and the AI brain's `OnUnitStartSacrifice` /
  `OnUnitStopSacrifice` events.
- **What we do:** with `fixes`, `SetUnitState` and `IsUnitState` accept the name. It is a state only
  scripts read; the engine does nothing with it.
- **Why it is safe:** the lines that now run only make effects, switch the death weapon off on a unit
  that is about to be destroyed anyway, and tell the brain. Nothing the simulation computes depends on
  them.
- **Tested:** `_scratch\engine\session\sac_start.lua` with and without `MOHO64_FIXES`.

## Looked broken, left alone

### M28: `IsM28AIPersonality` on the civilian armies (m28conditions.lua:4291)

In a game launched from the lobby the two civilian armies have no `AIPersonality` in `ArmySetup` (the
original's launch gives them four fields only), and M28 does `string.sub` on it, which is an error. The
original does the same, and M28 plays well with the error in place; giving the civilians an empty
personality would send M28 down a path it has never run. M28 is to be replaced later, so this stays.

### A mod shield without `Display.AnimationActivate` (xsb11022)

Data of a mod, the same in the original. Not ours to change.
