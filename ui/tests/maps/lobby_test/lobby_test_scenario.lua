-- A map of the lobby test's own (ui/tests/lobby_slots.lua, mounted with --maps ui/tests/maps): a skirmish
-- scenario with options of its own (lobby_test_options.lua beside it), played on the stock Seton's Clutch's
-- map, markers and script, which it only names. Nothing of the game is copied here.
version = 3
ScenarioInfo = {
    name = 'Lobby Test Map',
    description = "The lobby test's map with its own options, on Seton's Clutch.",
    type = 'skirmish',
    starts = true,
    preview = '',
    size = {1024, 1024},
    map = '/maps/SCMP_009/SCMP_009.scmap',
    save = '/maps/SCMP_009/SCMP_009_save.lua',
    script = '/maps/SCMP_009/SCMP_009_script.lua',
    norushradius = 70,
    Configurations = {
        ['standard'] = {
            teams = {
                { name = 'FFA', armies = {'ARMY_1', 'ARMY_2', 'ARMY_3', 'ARMY_4', 'ARMY_5', 'ARMY_6', 'ARMY_7', 'ARMY_8'} },
            },
            customprops = {
                ['ExtraArmies'] = STRING('ARMY_9 NEUTRAL_CIVILIAN'),
            },
        },
    },
}
