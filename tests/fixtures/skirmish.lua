-- Small reproducible local game. All assets and scripts come from the player's own installation.
GameConfig = {
    GameOptions = {
        ScenarioFile = '/maps/SCMP_009/SCMP_009_scenario.lua',
        Victory = 'demoralization', UnitCap = 500, FogOfWar = 'explored',
        TeamSpawn = 'fixed', GameSpeed = 'normal', CheatsEnabled = 'false',
        CivilianAlliance = 'enemy', PrebuiltUnits = 'Off', NoRushOption = 'Off',
        RestrictedCategories = {}, Share = 'ShareUntilDeath',
    },
    PlayerOptions = {
        [1] = {PlayerName = 'Test Player', Human = true, OwnerID = '0', Faction = 1,
               Team = 2, ArmyColor = 1, PlayerColor = 1, StartSpot = 1, Civilian = false},
        [2] = {PlayerName = 'Test AI', Human = false, OwnerID = '-1', Faction = 2,
               Team = 3, ArmyColor = 2, PlayerColor = 2, StartSpot = 2, Civilian = false, AIPersonality = 'easy'},
    },
    Observers = {}, GameMods = {},
}
