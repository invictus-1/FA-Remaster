-- FA Remastered: the day, its length, the weather and the map's edge as game options, added to the game's own list
-- (this file is a hook: the program reads it after the game's /lua/ui/lobby/lobbyOptions.lua, which
-- stays as it is). They are only seen: nothing in the simulation depends on them. The program
-- reads them from the game's options when it starts the game's picture (ui/app/game_session.cpp).
table.insert(globalOpts, 1, {
    default = 3,  -- (Morning: Chris, 2026-10-18, every remastered look on by default; "Map's Own Light" is the original)
    label = "Time of Day",
    help = "The hour the game starts at",
    key = 'RemasterDayStart',
    values = {
        { text = "Map's Own Light", help = "The map's fixed light, as in the original game. No day, no night, no weather", key = 'map' },
        { text = "Dawn", help = "The game starts as the sun rises", key = 'dawn' },
        { text = "Morning", help = "The game starts at nine in the morning", key = 'morning' },
        { text = "Noon", help = "The game starts at noon", key = 'noon' },
        { text = "Afternoon", help = "The game starts at three in the afternoon", key = 'afternoon' },
        { text = "Dusk", help = "The game starts as the sun sets", key = 'dusk' },
        { text = "Night", help = "The game starts at midnight", key = 'night' },
        { text = "Random", help = "The game starts at an hour drawn by chance", key = 'random' },
    },
})

table.insert(globalOpts, 2, {
    default = 4,
    label = "Length of Day",
    help = "How long a whole day and night take in game time",
    key = 'RemasterDayLength',
    value_text = "%s Minutes",
    value_help = "A whole day and night take %s minutes of game time",
    values = {
        { text = "Time Stands Still", help = "The hour never changes: with Time of Day this gives a game that is all day, all night, all dawn or all dusk", key = 'still' },
        '5', '10', '20', '30', '45', '60', '90', '120',
    },
})

table.insert(globalOpts, 3, {
    default = 1,
    label = "Weather",
    help = "The weather of the game. With the map's own light there is none",
    key = 'RemasterWeather',
    values = {
        { text = "Changing", help = "The weather changes by itself every few minutes: fair, cloudy, rain, storms, fog", key = 'changing' },
        { text = "Changing, Cold", help = "The weather changes by itself, and it snows in place of rain", key = 'cold' },
        { text = "Changing, Dry", help = "The weather changes by itself and is mostly clear", key = 'dry' },
        { text = "None", help = "Always fair", key = 'none' },
        { text = "Always Clear", help = "A clear sky for the whole game", key = 'clear' },
        { text = "Always Cloudy", help = "Clouds for the whole game", key = 'cloudy' },
        { text = "Always Overcast", help = "A grey sky for the whole game", key = 'overcast' },
        { text = "Always Rain", help = "Rain for the whole game", key = 'rain' },
        { text = "Always Storm", help = "A storm with lightning for the whole game", key = 'storm' },
        { text = "Always Fog", help = "Fog for the whole game", key = 'fog' },
        { text = "Always Snow", help = "Snow for the whole game", key = 'snow' },
    },
})

table.insert(globalOpts, 4, {
    default = 1,
    label = "Map Edge",
    help = "What lies round the map, where no unit can go",
    key = 'RemasterMapEdge',
    values = {
        -- (Chris, 2026-11-10: "On by default", the renderer's request 71 A: the land sculpted for the map past its edges
        -- (its request 62) is what No Machinery shows on a map that brings one; a map without one is laid out again
        -- mirrored, as before. The key stays 'beyond', so what a profile, a preset or a replay kept as 'beyond' shows
        -- it too; the choice 'surround' of 2026-11-09 is gone: a profile that kept it is given this default by the
        -- lobby (its key is no longer one of the values), and the program reads it as 'beyond'.)
        { text = "No Machinery", help = "The land goes on past the map's edge: the map's own sculpted land on maps that bring it (Seton's Clutch Remastered), other maps laid out again mirrored; greyed out where units cannot go", key = 'beyond' },
        { text = "Machinery", help = "The frame of machinery round the map, as in the original game", key = 'frame' },
        -- (the earlier look of No Machinery, kept as a choice: the map laid out again mirrored on every map)
        { text = "Mirrored Map", help = "The map laid out again mirrored past its edge on every map, the earlier look of No Machinery", key = 'mirror' },
    },
})

-- The simulation's order (Chris, 2026-10-31, multicore option 2): new games in our own order, made to run on many
-- cores and as exact as the original's; the original's order for its own replays (they play in the order they were
-- recorded in, whatever this says). The program adds the order's version at the launch (ui/app/game_session.cpp).
table.insert(globalOpts, {
    default = 1,
    label = "Simulation Order",
    help = "How the game works out each moment. Both give the same game on every player's machine",
    key = 'RemasterSimOrder',
    values = {
        { text = "Remastered", help = "FA Remastered's own order, made to use many processor cores (new games)", key = 'parallel' },
        { text = "Original", help = "The original game's order, one core at a time", key = 'original' },
    },
})

-- Watch after Defeat (ours; Chris's pick of 2026-11-09): when the player's army is defeated and the game goes on (an
-- allied AI still fights, or the others fight each other), the view goes free over the whole map by itself, where
-- observing is allowed (FAF's lobby allows it in every single-player game). The original changes nothing by itself:
-- the player sees his dead army's fog until he clicks an army's name in the score list; that is Off. The interface
-- does it (ui/data/hook/lua/ui/game/gameresult.lua); the simulation is the same either way.
table.insert(globalOpts, {
    default = 1,
    label = "Watch after Defeat",
    help = "What your view does when your army is defeated and the game goes on",
    key = 'RemasterWatchAfterDefeat',
    values = {
        { text = "On", help = "Your view goes free over the whole map by itself; a click on an army's name in the score list shows that army's view", key = 'on' },
        { text = "Off", help = "As in the original game: the view stays your defeated army's until you click an army's name in the score list", key = 'off' },
    },
})
