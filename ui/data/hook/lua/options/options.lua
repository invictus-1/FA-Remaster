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
        title = "Frame Rate",
        key = 'remaster_frame_rate',
        type = 'toggle',
        default = 'vsync',
        set = Look("remaster_frame_rate"),
        custom = {
            states = {
                { text = "Vertical Sync", key = 'vsync' },
                { text = "Uncapped", key = 'uncapped' },
                { text = "60", key = 60 },
                { text = "120", key = 120 },
                { text = "144", key = 144 },
            },
        },
    },
    {
        -- (Chris's rule: every new look is a player's option, the original's way included. The
        -- renderer's whole look at once: lighting, sky, water, ground shading, unit shaders, glow,
        -- the build look; effects, tracers, the day, weather, map edge and rich ground stay apart.)
        title = "Look",
        key = 'remaster_look',
        type = 'toggle',
        default = 'remastered',
        set = Look("remaster_look"),
        custom = {
            states = {
                { text = "Remastered", key = 'remastered' },
                { text = "Original", key = 'original' },
            },
        },
    },
    {
        -- (Chris, 2026-10-15: a choice, Direct3D 12 by default; from the next map loaded)
        title = "Graphics API",
        key = 'remaster_graphics_api',
        type = 'toggle',
        default = 'd3d12',
        set = Look("remaster_graphics_api"),
        custom = {
            states = {
                { text = "Direct3D 12", key = 'd3d12' },
                { text = "Vulkan", key = 'vulkan' },
            },
        },
    },
    {
        title = "Upscaling (FSR 1)",
        key = 'remaster_upscale',
        type = 'toggle',
        default = 1,
        set = Look("remaster_upscale"),
        custom = {
            states = {
                { text = "<LOC _Off>", key = 1 },
                { text = "Ultra Quality", key = 0.77 },
                { text = "Quality", key = 0.67 },
                { text = "Balanced", key = 0.59 },
                { text = "Performance", key = 0.5 },
            },
        },
    },
    {
        -- (the renderer's request 30, 2026-10-16: AMD's FSR 3 in place of FSR 1, at Upscaling's share; at
        -- Upscaling "Off" it smooths the edges at the screen's own size. It takes Anti-Aliasing's place, from
        -- the next map loaded as that setting.)
        title = "Temporal Upscaling (FSR 3)",
        key = 'remaster_temporal_upscale',
        type = 'toggle',
        default = 0,
        set = Look("remaster_temporal_upscale"),
        custom = {
            states = {
                { text = "<LOC _Off>", key = 0 },
                { text = "<LOC _On>", key = 1 },
            },
        },
    },
    -- The lighting upgrade (the renderer's request 31; Chris, 2026-10-17: all of it in, bundled to keep the
    -- menu short), each off unless chosen. Costs at 1440p in docs/render-lighting.md, "The options".
    {
        -- light in the air (sun shafts, lamp beams, glowing mist), volumetric clouds, the sky from the
        -- atmosphere's tables; with the lobby's Time of Day
        title = "Atmosphere",
        key = 'remaster_atmosphere',
        type = 'toggle',
        default = 0,
        set = Look("remaster_atmosphere"),
        custom = {
            states = {
                { text = "<LOC _Off>", key = 0 },
                { text = "<LOC _On>", key = 1 },
            },
        },
    },
    {
        -- the sun's shadows sharp at the foot and soft farther off; small contact shadows at units' feet
        title = "Soft Shadows",
        key = 'remaster_soft_shadows',
        type = 'toggle',
        default = 0,
        set = Look("remaster_soft_shadows"),
        custom = {
            states = {
                { text = "<LOC _Off>", key = 0 },
                { text = "<LOC _On>", key = 1 },
            },
        },
    },
    {
        -- bounce light and reflections (the reflections from the next map loaded). Only where the graphics
        -- card can ray trace: elsewhere one state, greyed out (the dialog reads the states when it opens; the
        -- renderer says after its first picture whether the card can)
        title = "Ray Tracing",
        key = 'remaster_ray_tracing',
        type = 'toggle',
        default = 0,
        set = Look("remaster_ray_tracing"),
        custom = setmetatable({}, {
            __index = function(t, k)
                if k ~= 'states' then return nil end
                if moho64ui_RayTracingSupported() == false then
                    return { { text = "Not on this graphics card", key = 0 } }
                end
                return { { text = "<LOC _Off>", key = 0 }, { text = "<LOC _On>", key = 1 } }
            end,
        }),
    },
    {
        -- rain and snow lit where they fall (they sparkle round lamps), and rain's splashes
        title = "Weather Effects",
        key = 'remaster_weather_effects',
        type = 'toggle',
        default = 0,
        set = Look("remaster_weather_effects"),
        custom = {
            states = {
                { text = "<LOC _Off>", key = 0 },
                { text = "<LOC _On>", key = 1 },
            },
        },
    },
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

-- The game's own graphics settings drive the renderer (Fidelity, Shadow Fidelity, Anti-Aliasing,
-- Level Of Detail, Texture Detail; ui/user/keymap.cpp) and mean what they say (Chris, 2026-10-15), so a player
-- who has never chosen starts at the full look: their defaults are raised to the highest. The
-- anti-aliasing choices are the renderer's (Off, 2, 4).
-- Options the remastered picture does its own way are left out of the menu (the renderer's answer
-- to ui request 24, 2026-10-16): Shadow Resolution (Shadow Fidelity sets the shadows' size),
-- Shadow render distance (the shadows cover every zoom) and Render Sky (the sky lights the scene;
-- without it the picture is wrong, not faster).
local leftOut = { shadow_resolution = true, shadow_render_distance = true, render_skydome = true }
for i = table.getn(options.video.items), 1, -1 do
    if leftOut[options.video.items[i].key] then
        table.remove(options.video.items, i)
    end
end

-- The Fidelity Presets as the game's, but without Render Sky (left out above), and with Bloom
-- Render on at High and Ultra: the glow belongs to the full modern look (the game's presets turn
-- it off at every level).
local function SetPreset(control, value)
    local logic = import("/lua/options/optionslogic.lua")
    local aa = GetAntiAliasingOptions()
    local aaHigh, aaMedium = 0, 0
    if table.getn(aa) > 0 then
        aaHigh = aa[table.getn(aa)]
        aaMedium = aa[math.ceil(table.getn(aa) / 2)]
    end
    local presets = {
        [0] = { {'fidelity', 0}, {'shadow_quality', 0}, {'texture_level', 2}, {'antialiasing', 0}, {'level_of_detail', 0}, {'bloom_render', 0} },
        [1] = { {'fidelity', 1}, {'shadow_quality', 1}, {'texture_level', 1}, {'antialiasing', 0}, {'level_of_detail', 1}, {'bloom_render', 0} },
        [2] = { {'fidelity', 2}, {'shadow_quality', 2}, {'texture_level', 0}, {'antialiasing', aaMedium}, {'level_of_detail', 2}, {'bloom_render', 1} },
        [3] = { {'fidelity', 2}, {'shadow_quality', 3}, {'texture_level', 0}, {'antialiasing', aaHigh}, {'level_of_detail', 2}, {'bloom_render', 1} },
    }
    for _, pair in presets[value] or {} do
        logic.SetValue(pair[1], pair[2], true)
    end
end

for _, item in options.video.items do
    if item.key == 'fidelity_presets' then item.update = SetPreset
    elseif item.key == 'bloom_render' then item.default = 1  -- (the game's: off; the renderer's look has it on)
    elseif item.key == 'fidelity' then item.default = 2
    elseif item.key == 'shadow_quality' then item.default = 3
    elseif item.key == 'level_of_detail' then item.default = 2
    elseif item.key == 'texture_level' then item.default = 0  -- (High)
    elseif item.key == 'antialiasing' then
        item.default = 4
        item.custom.states = {
            { text = "<LOC OPTIONS_0029>Off", key = 0 },
            { text = "2", key = 2 },
            { text = "4", key = 4 },
        }
    end
end
