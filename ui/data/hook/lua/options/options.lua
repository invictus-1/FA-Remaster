-- FA Remastered: the program's own looks as options of each player, in the Video tab of the
-- game's Options menu (this file is a hook: the program reads it after the game's
-- /lua/options/options.lua, which stays as it is). Each sends a console command the program
-- takes (ui/user/keymap.cpp, LookSettings); the game's picture follows at once. They are only
-- seen: nothing in the simulation depends on them. Chris, 2026-10-15: "The new looks will go
-- into options menu for each player".
local function Look(command)
    return function(key, value, startup)
        ConExecute(command .. " " .. tostring(value))
    end
end

local looks = {
    {
        title = "Remastered Effects",
        key = 'remaster_effects',
        type = 'toggle',
        default = 1,
        set = Look("remaster_effects"),
        custom = {
            states = {
                { text = "<LOC _On>", key = 1 },
                { text = "Original", key = 0 },
            },
        },
    },
    {
        title = "Effects Quality",
        key = 'remaster_effects_quality',
        type = 'toggle',
        default = 1,
        set = Look("remaster_effects_quality"),
        custom = {
            states = {
                { text = "Full", key = 1 },
                { text = "Three Quarters", key = 0.75 },
                { text = "Half", key = 0.5 },
            },
        },
    },
    {
        title = "Shell Tracers",
        key = 'remaster_tracers',
        type = 'toggle',
        default = 1,
        set = Look("remaster_tracers"),
        custom = {
            states = {
                { text = "<LOC _On>", key = 1 },
                { text = "Original", key = 0 },
            },
        },
    },
    {
        title = "Rich Ground on Remastered Maps",
        key = 'remaster_rich_ground',
        type = 'toggle',
        default = 1,
        set = Look("remaster_rich_ground"),
        custom = {
            states = {
                { text = "<LOC _On>", key = 1 },
                { text = "<LOC _Off>", key = 0 },
            },
        },
    },
    {
        title = "Tracks in Snow",
        key = 'remaster_snow_tracks',
        type = 'toggle',
        default = 1,
        set = Look("remaster_snow_tracks"),
        custom = {
            states = {
                { text = "<LOC _On>", key = 1 },
                { text = "<LOC _Off>", key = 0 },
            },
        },
    },
}

for _, item in looks do
    table.insert(options.video.items, item)
end
