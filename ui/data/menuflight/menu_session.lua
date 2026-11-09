-- The main menu's staged battle (ours: docs/ui-menuflight.md, app/menu_flight.h): the game configuration the menu's
-- flight starts its simulation from, in the form a lobby launch hands the engine (sim/session.h SessionFromGameConfig;
-- the same as the recorded tables --session takes). Two human armies and no AI of any kind: nothing commands them but
-- the scenario's own script (menu_battle.lua), so with the same seed and files the battle is the same on every run.
-- No mods, no cheats, no fog, no victory condition (sandbox: an army without a commander is not defeated), the day and
-- the weather fixed; the armies in their factions' own colours (FAF's GameColors: 12 UEF blue, 1 Cybran red). The
-- host sets the focus army to none and records nothing (MenuFlight::Start).
GameConfig = {
  ["GameMods"] = {
  },
  ["GameOptions"] = {
    ["AIReplacement"] = "Off",
    ["AllowObservers"] = false,
    ["AutoTeams"] = "none",
    ["CheatsEnabled"] = "false",
    ["CivilianAlliance"] = "neutral",
    ["CommonArmy"] = "Off",
    ["FogOfWar"] = "none",
    ["GameSpeed"] = "normal",
    ["NoRushOption"] = "Off",
    ["PrebuiltUnits"] = "Off",
    ["RandomMap"] = "Off",
    ["RemasterDayStart"] = "dusk",
    ["RemasterDayLength"] = "still",
    ["RemasterWeather"] = "none",
    ["RemasterMapEdge"] = "beyond",
    ["RemasterSimOrder"] = "parallel",
    ["RevealCivilians"] = "No",
    ["ScenarioFile"] = "/moho64ui/menuflight/setons_r_menu_scenario.lua",
    ["Score"] = "no",
    ["Share"] = "ShareUntilDeath",
    ["ShareUnitCap"] = "none",
    ["TeamLock"] = "locked",
    ["TeamSpawn"] = "fixed",
    ["Timeouts"] = "-1",
    ["UnitCap"] = "1000",
    ["Unranked"] = "Yes",
    ["Victory"] = "sandbox",
  },
  ["Observers"] = {
  },
  ["PlayerOptions"] = {
    [1] = {
      ["AIPersonality"] = "",
      ["ArmyColor"] = 12,
      ["Civilian"] = false,
      ["Faction"] = 1,
      ["Human"] = true,
      ["OwnerID"] = "1",
      ["PlayerColor"] = 12,
      ["PlayerName"] = "UEF",
      ["StartSpot"] = 1,
      ["Team"] = 2,
    },
    [2] = {
      ["AIPersonality"] = "",
      ["ArmyColor"] = 1,
      ["Civilian"] = false,
      ["Faction"] = 3,
      ["Human"] = true,
      ["OwnerID"] = "2",
      ["PlayerColor"] = 1,
      ["PlayerName"] = "Cybran",
      ["StartSpot"] = 2,
      ["Team"] = 3,
    },
  },
}
