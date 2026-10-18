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

-- The Remastered Preset (Chris, 2026-10-17: "yes that works for me so lets add it"): the new looks below set
-- together, as the game's own Fidelity Presets set its settings (optionslogic.SetValue while the dialog is
-- open; each can still be changed after; Custom leaves them as they are). Off is the original game's look
-- whole. What each level turns on is a first proposal, to be set by the renderer's measured costs (his request
-- for the tooltips). Frame Rate, Graphics API, the two upscalers and Frame Generation are choices of
-- smoothness and screen, not looks: no level touches them. Water Ripples come on with Weather Effects (the
-- renderer's advice, 2026-10-18); the Cinematic Lens Blur is a replay camera's, in no level.
local remasterPresets = {
    [0] = { remaster_look = 'original', remaster_effects = 0, remaster_effects_quality = 1, remaster_tracers = 0,
            remaster_rich_ground = 0, remaster_snow_tracks = 0, remaster_soft_shadows = 0,
            remaster_ground_detail = 0, remaster_atmosphere = 0, remaster_weather_effects = 0,
            remaster_water_ripples = 0, remaster_ray_tracing = 0 },
    [1] = { remaster_look = 'remastered', remaster_effects = 1, remaster_effects_quality = 0.5,
            remaster_tracers = 1, remaster_rich_ground = 1, remaster_snow_tracks = 1, remaster_soft_shadows = 0,
            remaster_ground_detail = 0, remaster_atmosphere = 0, remaster_weather_effects = 0,
            remaster_water_ripples = 0, remaster_ray_tracing = 0 },
    [2] = { remaster_look = 'remastered', remaster_effects = 1, remaster_effects_quality = 0.75,
            remaster_tracers = 1, remaster_rich_ground = 1, remaster_snow_tracks = 1, remaster_soft_shadows = 1,
            remaster_ground_detail = 1, remaster_atmosphere = 0, remaster_weather_effects = 0,
            remaster_water_ripples = 0, remaster_ray_tracing = 0 },
    [3] = { remaster_look = 'remastered', remaster_effects = 1, remaster_effects_quality = 1,
            remaster_tracers = 1, remaster_rich_ground = 1, remaster_snow_tracks = 1, remaster_soft_shadows = 1,
            remaster_ground_detail = 1, remaster_atmosphere = 1, remaster_weather_effects = 1,
            remaster_water_ripples = 1, remaster_ray_tracing = 0 },
    [4] = { remaster_look = 'remastered', remaster_effects = 1, remaster_effects_quality = 1,
            remaster_tracers = 1, remaster_rich_ground = 1, remaster_snow_tracks = 1, remaster_soft_shadows = 1,
            remaster_ground_detail = 1, remaster_atmosphere = 1, remaster_weather_effects = 1,
            remaster_water_ripples = 1, remaster_ray_tracing = 1 },
}
function RemasterPresetValues(level)  -- (also for the tests)
    local values = remasterPresets[level]
    if not values then return nil end
    local out = {}
    for key, value in values do out[key] = value end
    -- (ray tracing only where the card can: elsewhere its one state is Off)
    if moho64ui_RayTracingSupported() == false then out.remaster_ray_tracing = 0 end
    return out
end
local function SetRemasterPreset(control, level)
    local values = RemasterPresetValues(level)
    if not values then return end  -- (Custom)
    local logic = import("/lua/options/optionslogic.lua")
    for key, value in values do
        logic.SetValue(key, value, true)
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
        title = "Remastered Preset",
        key = 'remaster_preset',
        tip = "Sets the remastered looks below together, from Off (the original game's look) to Ultra. Each can still be changed after; Custom leaves them as they are.",
        type = 'toggle',
        default = 5,
        update = SetRemasterPreset,
        set = function(key, value, startup) end,
        custom = {
            states = {
                { text = "<LOC _Off>", key = 0 },
                { text = "<LOC _Low>", key = 1 },
                { text = "<LOC _Medium>", key = 2 },
                { text = "<LOC _High>", key = 3 },
                { text = "<LOC _Ultra>", key = 4 },
                { text = "<LOC _Custom>", key = 5 },
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
        tip = "Light in the air (sun shafts, lamp beams, glowing mist), volumetric clouds and the sky. Needs a Time of Day chosen in the lobby.",
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
        tip = "The sun's shadows sharp where they start and soft farther off, and small contact shadows at units' feet.",
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
        tip = "Bounce light and reflections, on graphics cards that can ray trace. Reflections apply from the next map loaded.",
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
        -- rain and snow lit where they fall (they sparkle round lamps), rain's splashes, wet shorelines and the
        -- shallows' caustics
        title = "Weather Effects",
        key = 'remaster_weather_effects',
        tip = "Rain and snow lit by lamps, rain's splashes, wet shorelines and caustics in shallow water.",
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
        -- the renderer's water ripples (World::waterRipples; Chris's next looks of 2026-10-17): the modern look's
        -- water, on a map with water
        title = "Water Ripples",
        key = 'remaster_water_ripples',
        tip = "Shells landing in water, ships, hovering units and units wading leave rings and wakes that spread, cross and fade.",
        type = 'toggle',
        default = 0,
        set = Look("remaster_water_ripples"),
        custom = {
            states = {
                { text = "<LOC _Off>", key = 0 },
                { text = "<LOC _On>", key = 1 },
            },
        },
    },
    {
        -- the renderer's request 33: grass near the camera, pressed flat round units and buildings; since render
        -- 23e9550 also the battle's scorches and craters kept on the ground, weathering (up to 2000)
        title = "Ground Detail",
        key = 'remaster_ground_detail',
        tip = "Grass and small plants near the camera, sunlight through leaves and grass, and the scorches and craters of battle kept on the ground.",
        type = 'toggle',
        default = 0,
        set = Look("remaster_ground_detail"),
        custom = {
            states = {
                { text = "<LOC _Off>", key = 0 },
                { text = "<LOC _On>", key = 1 },
            },
        },
    },
    {
        -- the renderer's request 34 (Chris's list of 2026-10-17, item 7): AMD's FSR 3 frame generation; like
        -- Temporal Upscaling it needs one sample a pixel, so with the Anti-Aliasing on it does nothing
        title = "Frame Generation",
        key = 'remaster_frame_generation',
        tip = "Shows a frame made between each two drawn ones: twice the frames, a little more delay. Best when the game already runs at 60 or more. Needs anti-aliasing off (or Temporal Upscaling on).",
        type = 'toggle',
        default = 0,
        set = Look("remaster_frame_generation"),
        custom = {
            states = {
                { text = "<LOC _Off>", key = 0 },
                { text = "<LOC _On>", key = 1 },
            },
        },
    },
    {
        -- the cinematic camera's lens (World::lens, render/cinecam.h; the renderer's answer of 2026-10-18): depth of
        -- field only while the replay's cinematic camera shows (the key "Cinematic camera in replays")
        title = "Cinematic Lens Blur",
        key = 'remaster_lens',
        tip = "A camera lens's depth of field: what the camera follows sharp, the near and far blurred. Only in the cinematic camera of a replay.",
        type = 'toggle',
        default = 0,
        set = Look("remaster_lens"),
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
