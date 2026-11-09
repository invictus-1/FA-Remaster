-- The main menu's staged battle (ours: docs/ui-menuflight.md): a scenario of its own on Seton's Clutch Remastered.
-- Its ground, props and markers are the remastered map's own files, read as they are (visuals' folder, mounted with
-- --maps; nothing of it is copied here); only the script is ours (menu_battle.lua), and only two of the map's armies
-- play: ARMY_1 (the UEF) on the north-east half, ARMY_2 (the Cybran) on the south-west, with the map's civilians.
version = 3
ScenarioInfo = {
    name = "Seton's Clutch Remastered: the menu's battle",
    description = "The staged battle behind the main menu.",
    type = 'skirmish',
    starts = true,
    preview = '',
    size = {1024, 1024},
    map = '/maps/SETONS_R/SETONS_R.scmap',
    save = '/maps/SETONS_R/SETONS_R_save.lua',
    script = '/moho64ui/menuflight/menu_battle.lua',
    Configurations = {
        ['standard'] = {
            teams = {
                { name = 'FFA', armies = {'ARMY_1', 'ARMY_2'} },
            },
            customprops = {
                ['ExtraArmies'] = STRING( 'ARMY_9 NEUTRAL_CIVILIAN' ),
            },
        },
    }}
