-- FA Remastered: the day, its length and the weather as game options, added to the game's own list
-- (this file is a hook: the program reads it after the game's /lua/ui/lobby/lobbyOptions.lua, which
-- stays as it is). They are only seen: nothing in the simulation depends on them. The program
-- reads them from the game's options when it starts the game's picture (ui/app/game_session.cpp).
table.insert(globalOpts, {
    default = 1,
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

table.insert(globalOpts, {
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

table.insert(globalOpts, {
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
