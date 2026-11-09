-- FA Remastered: the program's own looks as options of each player, in the Remastered tab of the
-- game's Options menu, after Video (this file is a hook: the program reads it after the game's
-- /lua/options/options.lua, which stays as it is). Each sends a console command the program
-- takes (ui/user/keymap.cpp, LookSettings); the game's picture follows at once. They are only
-- seen: nothing in the simulation depends on them. Chris, 2026-10-15: "The new looks will go
-- into options menu for each player"; 2026-11-08, the menu grown big: their own tab, in sections
-- (docs/ui-options-layout.md; the tab buttons narrowed for five: ui/data/hook/lua/ui/dialogs/options.lua).
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
-- Hidden Unit Outlines from Low on (readability), Battle Effects and Energy Shields from Medium on (each well under
-- half a millisecond, the renderer's measure). Chris, 2026-10-18: a player starts with every look on at its
-- highest, Ultra ("if they can't handle it they lower it"). The ten looks of Chris's big test (Ocean Glitter on
-- down; each about a millisecond or less at 4K, the renderer's measure of 2026-11-04, Sharpen ~0.9 and Unit Glow
-- ~1.0 the most): High and Ultra as their defaults; Medium without Sharpen; Low without Shore Soak too (~0.6);
-- Unit Glow 75 on every level (below 100 it adds hullEvenColours + hullShare, ~1 ms but within the noise: kept for
-- the look, the renderer's advice); the ones that help the player read the battle (Ghost Look, Guide over Grass,
-- Cloak Look, Stealth Veil, costing only when on screen: Cloak ~0.4 ms, Veil ~0.2 at most, the other two nothing
-- measurable, his remeasure of the same evening) from Low on, as Hidden Unit Outlines; Off has each as the
-- original game shows it, or our earlier look where the original has none.
local remasterPresets = {
    [0] = { remaster_look = 'original', remaster_effects = 0, remaster_effects_quality = 1, remaster_tracers = 0,
            remaster_rich_ground = 0, remaster_snow_tracks = 0, remaster_soft_shadows = 0,
            remaster_ground_detail = 0, remaster_atmosphere = 0, remaster_weather_effects = 0,
            remaster_water_ripples = 0, remaster_ray_tracing = 0, remaster_hidden_outlines = 0,
            remaster_battle_effects = 0, remaster_energy_shields = 0,
            remaster_ocean_glitter = 0, remaster_sea_sparkle = 0, remaster_unit_detail = 0, remaster_cloud_layout = 0, remaster_storm_clouds = 0, remaster_night_sky = 0, remaster_shore_soak = 0, remaster_shield_bend = 2, remaster_unit_glow = 100,
            remaster_sharpen = 0, remaster_gateway = 0, remaster_ghost_look = 1, remaster_guide = 0,
            remaster_cloak_look = 1, remaster_stealth_veil = 0 },
    [1] = { remaster_look = 'remastered', remaster_effects = 1, remaster_effects_quality = 0.5,
            remaster_tracers = 1, remaster_rich_ground = 1, remaster_snow_tracks = 1, remaster_soft_shadows = 0,
            remaster_ground_detail = 0, remaster_atmosphere = 0, remaster_weather_effects = 0,
            remaster_water_ripples = 0, remaster_ray_tracing = 0, remaster_hidden_outlines = 1,
            remaster_battle_effects = 0, remaster_energy_shields = 0,
            remaster_ocean_glitter = 1, remaster_sea_sparkle = 0, remaster_unit_detail = 1, remaster_cloud_layout = 1, remaster_storm_clouds = 1, remaster_night_sky = 1, remaster_shore_soak = 0, remaster_shield_bend = 1, remaster_unit_glow = 75,
            remaster_sharpen = 0, remaster_gateway = 1, remaster_ghost_look = 1, remaster_guide = 1,
            remaster_cloak_look = 1, remaster_stealth_veil = 1 },
    [2] = { remaster_look = 'remastered', remaster_effects = 1, remaster_effects_quality = 0.75,
            remaster_tracers = 1, remaster_rich_ground = 1, remaster_snow_tracks = 1, remaster_soft_shadows = 1,
            remaster_ground_detail = 1, remaster_atmosphere = 0, remaster_weather_effects = 0,
            remaster_water_ripples = 0, remaster_ray_tracing = 0, remaster_hidden_outlines = 1,
            remaster_battle_effects = 1, remaster_energy_shields = 1,
            remaster_ocean_glitter = 1, remaster_sea_sparkle = 0, remaster_unit_detail = 1, remaster_cloud_layout = 1, remaster_storm_clouds = 1, remaster_night_sky = 1, remaster_shore_soak = 1, remaster_shield_bend = 1, remaster_unit_glow = 75,
            remaster_sharpen = 0, remaster_gateway = 1, remaster_ghost_look = 1, remaster_guide = 1,
            remaster_cloak_look = 1, remaster_stealth_veil = 1 },
    [3] = { remaster_look = 'remastered', remaster_effects = 1, remaster_effects_quality = 1,
            remaster_tracers = 1, remaster_rich_ground = 1, remaster_snow_tracks = 1, remaster_soft_shadows = 1,
            remaster_ground_detail = 1, remaster_atmosphere = 1, remaster_weather_effects = 1,
            remaster_water_ripples = 1, remaster_ray_tracing = 0, remaster_hidden_outlines = 1,
            remaster_battle_effects = 1, remaster_energy_shields = 1,
            remaster_ocean_glitter = 1, remaster_sea_sparkle = 0, remaster_unit_detail = 1, remaster_cloud_layout = 1, remaster_storm_clouds = 1, remaster_night_sky = 1, remaster_shore_soak = 1, remaster_shield_bend = 1, remaster_unit_glow = 75,
            remaster_sharpen = 50, remaster_gateway = 1, remaster_ghost_look = 1, remaster_guide = 1,
            remaster_cloak_look = 1, remaster_stealth_veil = 1 },
    [4] = { remaster_look = 'remastered', remaster_effects = 1, remaster_effects_quality = 1,
            remaster_tracers = 1, remaster_rich_ground = 1, remaster_snow_tracks = 1, remaster_soft_shadows = 1,
            remaster_ground_detail = 1, remaster_atmosphere = 1, remaster_weather_effects = 1,
            remaster_water_ripples = 1, remaster_ray_tracing = 1, remaster_hidden_outlines = 1,
            remaster_battle_effects = 1, remaster_energy_shields = 1,
            remaster_ocean_glitter = 1, remaster_sea_sparkle = 0, remaster_unit_detail = 1, remaster_cloud_layout = 1, remaster_storm_clouds = 1, remaster_night_sky = 1, remaster_shore_soak = 1, remaster_shield_bend = 1, remaster_unit_glow = 75,
            remaster_sharpen = 50, remaster_gateway = 1, remaster_ghost_look = 1, remaster_guide = 1,
            remaster_cloak_look = 1, remaster_stealth_veil = 1 },
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
local presetSetting = false  -- (the preset is setting its looks: their own updates leave it as it is)
local function SetRemasterPreset(control, level)
    local values = RemasterPresetValues(level)
    if not values then return end  -- (Custom)
    local logic = import("/lua/options/optionslogic.lua")
    presetSetting = true
    for key, value in values do
        local item = logic.FindOptionItem(key)
        if item and item.type == 'slider' then
            -- (a slider's SetValue with skipUpdate does nothing, and without it only moves the thumb: FAF's slider
            -- change and maui/slider.lua SetValue; the dialog's working set takes a slider's value in its
            -- OnValueSet, as when the mouse lets go of it. Before 2026-11-08 the levels never reached Sharpen
            -- and Unit Glow at Night.)
            if item.control and not IsDestroyed(item.control) then
                item.control:SetValue(value)
                item.control:OnValueSet(item.control:GetValue())
            end
        else
            logic.SetValue(key, value, true)
        end
    end
    presetSetting = false
end

-- The first-start graphics check's next step down (ui/data/hook/lua/ui/game/gamemain.lua; Chris, 2026-10-18: "turn
-- ray tracing off first, then step the preset down"): given the options as they stand, the changes that lower the
-- remastered looks by one step and, for a step of the preset, the level it went to; nil when nothing is left to lower.
-- Ray Tracing goes first (where the card can ray trace: elsewhere it does nothing, and is left as it is); then the
-- Remastered Preset one level down, Ultra, High, Medium, Low, never to Off (the original's look), a level that would
-- change nothing being passed over (High after Ray Tracing went: it is Ultra without it). A level only ever lowers:
-- what the player has set lower than the level has stays as he set it (each value the lesser of his and the level's;
-- his Original Look stays), so from Custom the levels are tried from Ultra down. The Remastered Preset then names the
-- level the looks match, or Custom (as the Remastered tab would show them).
function RemasterLowerStep(current)
    local rayTracing = moho64ui_RayTracingSupported() ~= false
    local function Counts(key) return rayTracing or key ~= 'remaster_ray_tracing' end
    local changes, level = nil, nil
    if rayTracing and current.remaster_ray_tracing == 1 then
        changes = { remaster_ray_tracing = 0 }
    else
        local from = current.remaster_preset
        if type(from) ~= 'number' or from > 4 then from = 5 end  -- (Custom)
        for l = from - 1, 1, -1 do
            local step, any = {}, false
            for key, value in RemasterPresetValues(l) do
                local now, want = current[key], value
                if key == 'remaster_look' then
                    if now == 'original' then want = now end
                elseif type(now) == 'number' and type(value) == 'number' then
                    want = math.min(now, value)
                end
                if want ~= now and Counts(key) then
                    step[key] = want
                    any = true
                end
            end
            if any then
                changes, level = step, l
                break
            end
        end
    end
    if not changes then return nil end
    local after = {}
    for key, value in current do after[key] = value end
    for key, value in changes do after[key] = value end
    local named = 5
    for l = 4, 0, -1 do
        local same = true
        for key, value in RemasterPresetValues(l) do
            if after[key] ~= value and Counts(key) then
                same = false
                break
            end
        end
        if same then
            named = l
            break
        end
    end
    if named ~= current.remaster_preset then changes.remaster_preset = named end
    return changes, level
end

local looks = {
    {
        title = "Frame Rate",
        key = 'remaster_frame_rate',
        tip = "How often a new picture is drawn. Vertical Sync: in step with the screen's refresh, no tearing. Uncapped: as fast as the PC can. 60, 120, 144: at most that many frames a second.",
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
        -- the renderer's latency limiter (look 7 of Chris's list of 2026-10-17, render 0ed9975): one frame waiting to
        -- be shown, each begun just in time for its refresh. On by default (the renderer's ask), in no preset
        title = "Low Latency",
        key = 'remaster_low_latency',
        tip = "Fewer frames wait to be shown, so the picture answers the mouse and the keys sooner; on a busy PC a frame may now and then come a refresh late.",
        type = 'toggle',
        default = 1,
        set = Look("remaster_low_latency"),
        custom = {
            states = {
                { text = "<LOC _Off>", key = 0 },
                { text = "<LOC _On>", key = 1 },
            },
        },
    },
    {
        title = "Remastered Preset",
        key = 'remaster_preset',
        tip = "Sets the remastered looks below together, from Off (the original game's look) to Ultra. Each can still be changed after; Custom leaves them as they are. Season is not one of them: it stays as you chose it at every level, Off too.",
        type = 'toggle',
        default = 4,
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
        tip = "The remastered picture as a whole (lighting, sky, water, ground shading, the units' shading and glow, the build look), or the original game's. Effects, tracers, the day, the weather and rich ground keep their own options.",
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
        tip = "Draws with Direct3D 12 or Vulkan. Applies from the next map loaded.",
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
        tip = "Draws the world smaller and scales it up to the screen with AMD FSR 1: a higher frame rate for a softer picture. Off: drawn at the screen's own size.",
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
        tip = "Draws the world smaller and sharpens it up to the screen with AMD FSR 3, in place of anti-aliasing. Raises the frame rate only where the graphics card is what holds it back; in a big late game the processor usually is. Its anti-aliasing part changes from the next map.",
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
        tip = "Light in the air (sunlight shafts through the trees and under breaks in the cloud, lamp beams, glowing mist), volumetric clouds with their shadows drifting over the land, the sky, zooming out through the cloud layer to the whole map, and land and mountains past the map's edge. Needs a Time of Day chosen in the lobby.",
        type = 'toggle',
        default = 1,
        set = Look("remaster_atmosphere"),
        custom = {
            states = {
                { text = "<LOC _Off>", key = 0 },
                { text = "<LOC _On>", key = 1 },
            },
        },
    },
    {
        -- the renderer's World::clouds.mix (render b6e66de, its request 56; Chris 2026-11-06, new looks are options)
        title = "Cloud Layout",
        key = 'remaster_cloud_layout',
        tip = "How the 3D clouds lie in the sky. Mixed: at random places and of mixed sizes, as real weather. Even: in even rows of one size, as before. Only with Atmosphere on.",
        type = 'toggle',
        default = 1,
        set = Look("remaster_cloud_layout"),
        custom = {
            states = {
                { text = "Even", key = 0 },
                { text = "Mixed", key = 1 },
            },
        },
    },
    {
        -- the renderer's World::stormCells (render 309e718, its request 57; Chris 2026-11-06: "Yes, keep it")
        title = "Storm Clouds",
        key = 'remaster_storm_clouds',
        tip = "How a storm's clouds look. Cells: towering storm cells with darker bases and breaks of sun between them. Plain: the storm's clouds as before. Only with Atmosphere on.",
        type = 'toggle',
        default = 1,
        set = Look("remaster_storm_clouds"),
        custom = {
            states = {
                { text = "Plain", key = 0 },
                { text = "Cells", key = 1 },
            },
        },
    },
    {
        -- the renderer's World::starryNight (render acf9384, its request 58; Chris 2026-11-07: a beautiful starry night)
        title = "Night Sky",
        key = 'remaster_night_sky',
        tip = "The sky at night. Starry: deep and dark, full of stars, the band of the galaxy and a halo round the moon. Plain: the night sky as before.",
        type = 'toggle',
        default = 1,
        set = Look("remaster_night_sky"),
        custom = {
            states = {
                { text = "Plain", key = 0 },
                { text = "Starry", key = 1 },
            },
        },
    },
    {
        -- the sun's shadows sharp at the foot and soft farther off; small contact shadows at units' feet
        title = "Soft Shadows",
        key = 'remaster_soft_shadows',
        tip = "The sun's shadows sharp where they start and soft farther off, and small contact shadows at units' feet.",
        type = 'toggle',
        default = 1,
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
        tip = "Rain and snow lit by lamps, rain's splashes, wet shorelines and caustics in shallow water, units wet in rain and muddy from soft ground, snow lying on the units themselves, and weather on the horizon beyond the map's edge: fog banks, distant showers, ice floes while snow lies.",
        type = 'toggle',
        default = 1,
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
        default = 1,
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
        default = 1,
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
        -- Temporal Upscaling it needs one sample a pixel, so on it turns the Anti-Aliasing off (render request 52:
        -- before, with the Anti-Aliasing on it silently did nothing)
        title = "Frame Generation",
        key = 'remaster_frame_generation',
        tip = "Shows a frame made between each two drawn ones: twice the frames shown, but it ADDS INPUT DELAY (about 12 ms more) and costs time on every real frame (about 3 ms). The upscaler (FSR) is the smoother choice. Off unless chosen; no preset turns it on. Turns anti-aliasing off while on (from the next map).",
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
        default = 1,
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
        tip = "The remastered effects (explosions, smoke, fire, build effects), or the original game's.",
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
        tip = "How many particles the effects throw out: all of them, three quarters or half. Less is faster in big battles.",
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
        tip = "Tank shells drawn as hot tracers, or as the original game's plain line.",
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
        tip = "The richer ground of FA Remastered's own remastered maps. Off: their ground as plain as the original's.",
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
        tip = "Units tread tracks into lying snow.",
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
    {
        -- the renderer's photographic finish (World::photoLook, render ef7e1bb; Chris, 2026-10-18: "it should just be an
        -- option in game as in you can choose which one you want"): a style pick, Off unless chosen, in no preset
        title = "Photographic Look",
        key = 'remaster_photo_look',
        tip = "A photographic finish: the exposure adapts to the scene like an eye, a film-like colour grade, and a light vignette and grain. Natural keeps today's colours; Film, Warm and Cold War grade them. Remastered look only.",
        type = 'toggle',
        default = 0,
        set = Look("remaster_photo_look"),
        custom = {
            states = {
                { text = "<LOC _Off>", key = 0 },
                { text = "Natural", key = 1 },
                { text = "Film", key = 2 },
                { text = "Warm", key = 3 },
                { text = "Cold War", key = 4 },
            },
        },
    },
    {
        -- the renderer's seasons (WorldPicture::SetSeason, render c267feb; Chris, 2026-10-18: seasons as a player's
        -- option, visuals' autumn art for SETONS_R): a style pick, today's Summer unless chosen, in no preset; the
        -- picture's alone, from the next map opened
        title = "Season",
        key = 'remaster_season',
        tip = "The map's season, where the map has one: autumn ground, plants and leaves in the wind, and autumn weather (showers, morning fog). Applies from the next map.",
        type = 'toggle',
        default = 0,
        set = Look("remaster_season"),
        custom = {
            states = {
                { text = "Summer", key = 0 },
                { text = "Autumn", key = 1 },
            },
        },
    },
    {
        -- the renderer's battle looks as one setting (agreed 2026-10-18): World's blastWaves, blastDust, blastDebris, litSmoke, smokeShadows, heatShimmer, unitScars
        title = "Battle Effects",
        key = 'remaster_battle_effects',
        tip = "Blast waves that press the grass and bend the trees, dust and debris, rocks that chip and scorch when hit, smoke and fire lit properly with their shadows, heat shimmer, wrecks and craters that glow and cool, dust, spray and contrails behind moving units, engines' exhausts as glowing plumes, explosions that light their surroundings, remade build effects, and battle scars on units that survive.",
        type = 'toggle',
        default = 1,
        set = Look("remaster_battle_effects"),
        custom = {
            states = {
                { text = "<LOC _Off>", key = 0 },
                { text = "<LOC _On>", key = 1 },
            },
        },
    },
    {
        -- the renderer's World::energyShields
        title = "Energy Shields",
        key = 'remaster_energy_shields',
        tip = "Shield bubbles that look like energy: what is behind bends as through glass, each hit sends ripples across.",
        type = 'toggle',
        default = 1,
        set = Look("remaster_energy_shields"),
        custom = {
            states = {
                { text = "<LOC _Off>", key = 0 },
                { text = "<LOC _On>", key = 1 },
            },
        },
    },
    {
        -- the renderer's look 15, WorldPicture::SetHiddenOutlines: only what the player may see; none in the Original look
        title = "Hidden Unit Outlines",
        key = 'remaster_hidden_outlines',
        tip = "A faint outline in the army's colour where a unit is hidden behind trees or a hill: your own and allied units, and enemies in sight. Not in the Original look.",
        type = 'toggle',
        default = 1,
        set = Look("remaster_hidden_outlines"),
        custom = {
            states = {
                { text = "<LOC _Off>", key = 0 },
                { text = "<LOC _On>", key = 1 },
            },
        },
    },
    {
        -- the renderer's World::lampsTeamColour (its request 50; Chris, 2026-10-28): not a quality step, so in no preset
        title = "Unit Glow Colour",
        key = 'remaster_lamp_colour',
        tip = "The faint glow of the units' own lights: in their army's colour, or in their faction's own (UEF blue whatever colour you play). Headlights and the rest keep their colours.",
        type = 'toggle',
        default = 1,
        set = Look("remaster_lamp_colour"),
        custom = {
            states = {
                { text = "Faction colour", key = 0 },
                { text = "Team colour", key = 1 },
            },
        },
    },
    {
        -- the renderer's HotMetal::softScorch (render 5d77cf6; Chris: the remastered look the default, the old one kept)
        title = "Scorch Glow",
        key = 'remaster_scorch',
        tip = "A fresh scorch where a unit died, while its metal is still hot: Soft, a deep-red bed with bright cracks; Bright, the earlier orange-hot sheet. Needs Battle Effects on.",
        type = 'toggle',
        default = 1,
        set = Look("remaster_scorch"),
        custom = {
            states = {
                { text = "Bright", key = 0 },
                { text = "Soft", key = 1 },
            },
        },
    },
    -- The renderer's looks from Chris's big test (2026-11-01; render/world.h): each on by default, the earlier look kept
    -- as Off (Chris: new looks are player options); in the preset levels by their costs (remasterPresets above).
    -- (ui/user/user_state.h LookSettings has which field of the world each sets.)
    {
        -- the renderer's Ocean::glitter
        title = "Ocean Glitter",
        key = 'remaster_ocean_glitter',
        tip = "The sun's glitter on the sea as a soft path of light round the sun's reflection, the water elsewhere calm. Off: the earlier glitter, the whole sea sparkling with single bright pixels.",
        type = 'toggle',
        default = 1,
        set = Look("remaster_ocean_glitter"),
        custom = {
            states = {
                { text = "<LOC _Off>", key = 0 },
                { text = "<LOC _On>", key = 1 },
            },
        },
    },
    {
        -- the renderer's Ocean::nearSparkle (render 03fc92c; Chris's pick)
        title = "Sea Sparkle",
        key = 'remaster_sea_sparkle',
        tip = "Small glints on the steepest wave faces you look down on, where they catch the sky, also where the camera looks. Off unless chosen: the sea looks calmer without them.",
        type = 'toggle',
        default = 0,
        set = Look("remaster_sea_sparkle"),
        custom = {
            states = {
                { text = "<LOC _Off>", key = 0 },
                { text = "<LOC _On>", key = 1 },
            },
        },
    },
    {
        -- the renderer's World::unitDetail (render 5ee436a; Chris: a factory's side 'feels too low res')
        title = "Close-up Detail",
        key = 'remaster_unit_detail',
        tip = "A fine grain in remastered units' paint and relief, seen only at the nearest zoom where their textures are magnified; none at play zoom. Off: as before.",
        type = 'toggle',
        default = 1,
        set = Look("remaster_unit_detail"),
        custom = {
            states = {
                { text = "<LOC _Off>", key = 0 },
                { text = "<LOC _On>", key = 1 },
            },
        },
    },
    {
        -- the renderer's Water::shoreSoak
        title = "Shore Soak",
        key = 'remaster_shore_soak',
        tip = "The ground under the shallow water drawn soaked and dark like the wet beach above it, so the water meets the shore. Off: the earlier look, the shallows bright past the wet band.",
        type = 'toggle',
        default = 1,
        set = Look("remaster_shore_soak"),
        custom = {
            states = {
                { text = "<LOC _Off>", key = 0 },
                { text = "<LOC _On>", key = 1 },
            },
        },
    },
    {
        -- the renderer's EnergyShields::bend and steadyUnits (with Energy Shields on)
        title = "Shield Bending",
        key = 'remaster_shield_bend',
        tip = "How much an energy shield bends what is behind it, as through glass. Subtle: a little, and units and buildings under it stay sharp. Full: everything behind it bent, the earlier look. Off: nothing bent. Needs Energy Shields on.",
        type = 'toggle',
        default = 1,
        set = Look("remaster_shield_bend"),
        custom = {
            states = {
                { text = "<LOC _Off>", key = 0 },
                { text = "Subtle", key = 1 },
                { text = "Full", key = 2 },
            },
        },
    },
    {
        -- the renderer's World::hullGlow (and hullReach, hullAir, hullEvenColours, hullShare from it: LookSettings)
        title = "Unit Glow at Night",
        key = 'remaster_unit_glow',
        tip = "How strongly the units' own lights glow at night. 100: the earlier glow, at full strength and reach, each army's colour as bright as its lamps say and crowded bases adding up.",
        type = 'slider',
        default = 75,
        set = function(key, value, startup)
            ConExecute("remaster_unit_glow " .. tostring(value / 100))
        end,
        custom = {
            min = 0,
            max = 100,
            inc = 5,
        },
    },
    {
        -- the renderer's World::hazeMist (request 50; Chris, 2026-11-09: "an option we can adjust like unit glow").
        -- A taste setting: no preset level sets it (remasterPresets), and the original look has no haze of ours.
        title = "Haze & Mist",
        key = 'remaster_haze_mist',
        tip = "How much haze and mist the air holds in clear weather: the mist on low ground, the dust in the air, the haze the far land fades into. 100: as made. 0: clear air. 200: twice as thick. Fog, storms and snowfall are the weather's own and stay. The menu's flight follows it too.",
        type = 'slider',
        default = 100,
        set = function(key, value, startup)
            ConExecute("remaster_haze_mist " .. tostring(value / 100))
        end,
        custom = {
            min = 0,
            max = 200,
            inc = 10,
        },
    },
    {
        -- the renderer's Post::sharpen
        title = "Sharpen",
        key = 'remaster_sharpen',
        tip = "Sharpens the picture's fine detail, the units' textures most, without halos. 0: no sharpening, the earlier picture. Not with an upscaler, which sharpens by itself.",
        type = 'slider',
        default = 50,
        set = function(key, value, startup)
            ConExecute("remaster_sharpen " .. tostring(value / 100))
        end,
        custom = {
            min = 0,
            max = 100,
            inc = 5,
        },
    },
    {
        -- the renderer's World::remasteredGate (with the modern look and the remastered effects)
        title = "Remastered Gateway",
        key = 'remaster_gateway',
        tip = "The quantum gateway's field remade. Off: the original's texture. Needs the Remastered look and Remastered Effects.",
        type = 'toggle',
        default = 1,
        set = Look("remaster_gateway"),
        custom = {
            states = {
                { text = "<LOC _Off>", key = 0 },
                { text = "<LOC _On>", key = 1 },
            },
        },
    },
    {
        -- the renderer's World::ghostLook (ui request 41)
        title = "Ghost Look",
        key = 'remaster_ghost_look',
        tip = "A structure queued or being placed shows as a see-through ghost with a brighter outline, as in the original game. Off: the earlier look, a solid model in one colour.",
        type = 'toggle',
        default = 1,
        set = Look("remaster_ghost_look"),
        custom = {
            states = {
                { text = "<LOC _Off>", key = 0 },
                { text = "<LOC _On>", key = 1 },
            },
        },
    },
    {
        -- the renderer's World::guidePull
        title = "Guide over Grass",
        key = 'remaster_guide',
        tip = "A building's green outline and the placement guide drawn over the grass with a dark edge, easy to line up. Off: the earlier look, partly hidden in tall grass.",
        type = 'toggle',
        default = 1,
        set = Look("remaster_guide"),
        custom = {
            states = {
                { text = "<LOC _Off>", key = 0 },
                { text = "<LOC _On>", key = 1 },
            },
        },
    },
    {
        -- the renderer's World::cloakLook
        title = "Cloak Look",
        key = 'remaster_cloak_look',
        tip = "A cloaked unit drawn as in the original game: a see-through shimmer in its faction's shield colour, its lights off. Off: drawn as an ordinary unit.",
        type = 'toggle',
        default = 1,
        set = Look("remaster_cloak_look"),
        custom = {
            states = {
                { text = "<LOC _Off>", key = 0 },
                { text = "<LOC _On>", key = 1 },
            },
        },
    },
    {
        -- the renderer's World::stealthLook (ours: the original shows nothing for radar stealth)
        title = "Stealth Veil",
        key = 'remaster_stealth_veil',
        tip = "Your own and allied units hidden from radar (their stealth on, or in a stealth field) wear a faint shimmering veil, so you see which are hidden. Off: as in the original game, no sign.",
        type = 'toggle',
        default = 1,
        set = Look("remaster_stealth_veil"),
        custom = {
            states = {
                { text = "<LOC _Off>", key = 0 },
                { text = "<LOC _On>", key = 1 },
            },
        },
    },
}

-- Ray Tracing's default (Chris, 2026-10-18: every look on at its highest): On where the card can ray trace; where it
-- cannot, its one state, Off. Asked when it is read, as its states are (the card is known once a picture opened).
for _, item in looks do
    if item.key == 'remaster_ray_tracing' then
        item.default = nil
        setmetatable(item, { __index = function(t, k)
            if k == 'default' then return moho64ui_RayTracingSupported() == false and 0 or 1 end
        end })
    end
end

-- The Remastered tab (Chris, 2026-11-08: the Video tab "getting kinda big ... keep the same theme/look"; the layout:
-- docs/ui-options-layout.md, its recommendation (a)): the looks above in sections under the game's own header rows (FAF's
-- Gameplay and Interface tabs have them), from the screen down to the picture's finish. The first section's six are
-- the ones no preset level touches; every key a level sets is below the preset on this same tab, as the dialog needs
-- (optionslogic.SetValue changes only an option whose control is on the page shown). The prefs keep the options as one
-- flat table, not per tab (optionslogic.GetCurrent), so a player's values stay as they were. A look named in no
-- section goes under a last header "More" (none today: ui/tests/lobby.lua keeps it so).
local remasterSections = {
    { "Display & Performance", { 'remaster_frame_rate', 'remaster_low_latency', 'remaster_graphics_api', 'remaster_upscale',
                                 'remaster_temporal_upscale', 'remaster_frame_generation' } },
    { "Preset", { 'remaster_preset', 'remaster_look' } },
    { "Sky & Light", { 'remaster_atmosphere', 'remaster_cloud_layout', 'remaster_storm_clouds', 'remaster_night_sky',
                       'remaster_haze_mist', 'remaster_soft_shadows', 'remaster_ray_tracing' } },
    { "Ground & Weather", { 'remaster_rich_ground', 'remaster_ground_detail', 'remaster_snow_tracks',
                            'remaster_weather_effects', 'remaster_season' } },
    { "Water", { 'remaster_water_ripples', 'remaster_ocean_glitter', 'remaster_sea_sparkle', 'remaster_shore_soak' } },
    { "Effects & Battle", { 'remaster_effects', 'remaster_effects_quality', 'remaster_tracers', 'remaster_battle_effects',
                            'remaster_scorch', 'remaster_energy_shields', 'remaster_shield_bend', 'remaster_gateway' } },
    { "Units & Readability", { 'remaster_unit_detail', 'remaster_unit_glow', 'remaster_lamp_colour',
                               'remaster_hidden_outlines', 'remaster_ghost_look', 'remaster_guide', 'remaster_cloak_look',
                               'remaster_stealth_veil' } },
    { "Picture & Camera", { 'remaster_photo_look', 'remaster_sharpen', 'remaster_lens' } },
}
local function Header(title)  -- (the game's header row: its box unseen, only its title; "expected everywhere": FAF's)
    return { title = title, type = 'header', default = '', key = '' }
end
do
    local byKey, placed = {}, {}
    for _, item in looks do byKey[item.key] = item end
    local items, headerRows, states = {}, {}, {}
    -- "Go to Section" (Chris's "drop down menus that open up different remaster options"): the game's own drop-down,
    -- its choice scrolls the tab to that section's header; it hides nothing (the presets and the command palette
    -- need every row there). Nothing of it is applied (no set); each time the tab is made it shows the first section.
    local gotoItem = {
        title = "Go to Section",
        key = 'remaster_goto_section',
        tip = "Scrolls this tab to the chosen section.",
        type = 'toggle',
        default = 1,
        remasterNotSearched = true,  -- (the command palette leaves it out: ui/data/lua/palette.lua)
        custom = { states = states },
        update = function(control, value)
            local row = headerRows[value]
            -- the combo's group, the row's box, the dialog's grid (FAF's CreateOption)
            local grid = row and control:GetParent() and control:GetParent():GetParent()
            grid = grid and grid:GetParent()
            if grid and grid.ScrollSetTop then grid:ScrollSetTop('Vert', row) end
        end,
    }
    gotoItem.init = function()
        if gotoItem.change and gotoItem.control then gotoItem.change(gotoItem.control, 1, true) end
    end
    table.insert(items, gotoItem)
    local function Section(title, keys)
        table.insert(items, Header(title))
        table.insert(states, { text = title, key = table.getn(states) + 1 })
        headerRows[table.getn(states)] = table.getn(items)
        for _, item in keys do
            table.insert(items, item)
            placed[item.key] = true
        end
    end
    for _, section in remasterSections do
        local keys = {}
        for _, key in section[2] do
            if byKey[key] and not placed[key] then table.insert(keys, byKey[key]) end
        end
        Section(section[1], keys)
    end
    local more = {}
    for _, item in looks do
        if not placed[item.key] then table.insert(more, item) end
    end
    if table.getn(more) > 0 then Section("More", more) end
    options.remaster = { title = "Remastered", key = 'remaster', items = items }
    local at = table.getn(optionsOrder) + 1
    for i, key in optionsOrder do
        if key == 'video' then at = i + 1 end
    end
    table.insert(optionsOrder, at, 'remaster')
end

-- The tab the Options dialog shows chosen, as a click on its button chooses it (Video's "Show" button; the command
-- palette finds the buttons the same way). A frame later: the button that asks is a row of the page this replaces.
function RemasterShowOptionsTab(key)
    local tabData = options[key]
    ForkThread(function()
        WaitFrames(1)
        local tab = false
        GetFrame(0):ApplyFunction(function(control)
            if not tab and control.tabData == tabData and control.OnClick and not control:IsHidden() then tab = control end
        end)
        if tab then tab:OnClick() end
    end)
end

-- Colour Vision (ours, the Interface tab; Chris, 2026-10-23): the armies' colours, where the player sees them, from a
-- palette that stays apart for a kind of colour blindness (the host's GetArmiesTable colours, a unit's teamColor, the
-- strategic icons and the minimap; ui/user/session_bindings.cpp, ColorVisionColor, has the palettes and their source).
-- Off is the original's colours. Only what is seen: the simulation keeps its army colours (a marker ping still sends the
-- army's own colour: our hook of /lua/ui/game/ping.lua), so a replay is the same either way. FAF's GetArmiesTable is
-- a copy renewed every 4 seconds (/lua/ui/override/ArmiesTable.lua): what reads it (chat names, the unit panel) takes
-- the new colours within that; the strategic icons and the minimap at once. The score board laid its colour bars when
-- it was made: they are given the new colours here, at once (the host's own colours, not FAF's copy; not a defeated
-- army's black, and not while the team colour button has them).
function RemasterRecolourScoreBoard()  -- (also for the tests)
    local score = __modules and __modules['/lua/ui/game/score.lua']
    if not (score and score.controls and score.controls.armyLines and moho64ui_ColorVision and moho64ui_ArmyColors) then
        return
    end
    local _, teamColors = moho64ui_ColorVision()
    if teamColors then return end
    for _, line in score.controls.armyLines do
        local _, seen
        if line.armyID and line.armyID > 0 then _, seen = moho64ui_ArmyColors(line.armyID) end
        if line.color and not line.OOG and seen then
            line.color:SetSolidColor(seen)
        end
    end
end
-- (ours under a header of their own: after FAF's last header, "Casting tools", they looked as if they belonged to it)
table.insert(options.ui.items, Header("FA Remastered"))
table.insert(options.ui.items, {
    title = "Colour Vision",
    key = 'remaster_color_vision',
    tip = "The armies' colours from a palette that stays apart for colour blindness: on units' icons, the minimap, the score board, chat names and the unit panel. Deuteranopia and Protanopia are the red-green kinds, Tritanopia the blue-yellow one; High Contrast stays apart for all of them at once. Off: the game's own colours. Only your screen changes.",
    type = 'toggle',
    default = 0,
    set = function(key, value, startup)
        ConExecute("remaster_color_vision " .. tostring(value))
        RemasterRecolourScoreBoard()
    end,
    custom = {
        states = {
            { text = "<LOC _Off>", key = 0 },
            { text = "Deuteranopia (red-green)", key = 1 },
            { text = "Protanopia (red-green)", key = 2 },
            { text = "Tritanopia (blue-yellow)", key = 3 },
            { text = "High Contrast", key = 4 },
        },
    },
})

-- The original's path preview, which it has and never shows (Chris, 2026-10-23: "try it first ... I want to see it in
-- action first"): while Shift is held, with the order lines, a line with an arrowhead from the largest selected unit
-- (from its last order) to the place under the mouse, in the colour of the order a click there would give. The exe
-- draws it when its console variable ui_DrawPathPreview is on, which nothing sets (ui/user/user_units.cpp
-- WorkOutPathPreview). Interface tab, off unless chosen.
table.insert(options.ui.items, {
    title = "Path Preview",
    key = 'remaster_path_preview',
    tip = "While Shift is held, an arrow from your selected units' last order to the mouse, in the colour of the order a click there would give.",
    type = 'toggle',
    default = 0,
    set = function(key, value, startup)
        ConExecute("ui_DrawPathPreview " .. tostring(value))
    end,
    custom = {
        states = {
            { text = "<LOC _Off>", key = 0 },
            { text = "<LOC _On>", key = 1 },
        },
    },
})

-- Score Screen Extras (ours; Chris, 2026-11-07, the post-game report): on the score screen's graphs, the game's moments
-- (the replay timeline's markers, a commander killed, an experimental, a nuke, the game's end) and a strip of the
-- fighting; "Army Value (Mass)" in the list of graphs; the Kill Stats chart's ACU column counting kills (FAF's shows
-- the commanders built). Read when a graph or chart is made (ui/data/hook/lua/ui/dialogs/hotstats.lua). On at first, as
-- every remastered addition (Chris, 2026-10-18); Off: FAF's score screen as it is.
table.insert(options.ui.items, {
    title = "Score Screen Extras",
    key = 'remaster_score_extras',
    tip = "On the score screen after a game: the game's big moments (a commander killed, an experimental finished or lost, a nuke, an army defeated) marked on the graphs, with a strip of how much fighting there was, and an Army Value graph. Off: the score screen as in FAF.",
    type = 'toggle',
    default = 1,
    set = function(key, value, startup) end,
    custom = {
        states = {
            { text = "<LOC _Off>", key = 0 },
            { text = "<LOC _On>", key = 1 },
        },
    },
})

-- Key Bindings: Import from FAF (Chris, 2026-11-06: FAF opens the Key Bindings dialog only in game, F1 or the game's
-- menu, and he looked for the import in Options). A button (the dialog's 'button' type: no value kept) that opens the
-- Key Bindings dialog and asks its "Import from FAF" question at once (ui/data/hook/lua/ui/dialogs/keybindings.lua).
table.insert(options.ui.items, {
    title = "Key Bindings",
    key = 'remaster_import_faf_keys',
    tip = "Opens the Key Bindings window and offers to replace all key bindings with those of your current profile in FAF (Forged Alliance Forever). FAF's own file is only read. In a game the same window opens with F1.",
    type = 'button',
    default = 0,
    update = function(control, value)
        import('/lua/ui/dialogs/keybindings.lua').RemasterOpenKeyBindingsAndImport()
    end,
    set = function(key, value, startup) end,
    custom = {
        text = "Import from FAF...",
    },
})

-- What plays behind the main menu (Chris approved the flight on 2026-11-08: docs/ui-menuflight.md; the ui chat's lead
-- put both here, on the Interface tab, until the Remaster options are laid out anew): the live flight over a staged
-- battle on Seton's Clutch Remastered (the default; without that map: Dusk), the evening flight over the stock map as
-- before, or one still picture. Live: the main menu changes at once. And the battle's sound: none (the original's menu
-- played music only, the default) or heard from afar.
table.insert(options.ui.items, {
    title = "Menu Background",
    key = 'remaster_menu_background',
    tip = "What plays behind the main menu: a cinematic flight over a staged battle on Seton's Clutch Remastered, the earlier evening flight over the original map, or a still picture (the lightest).",
    type = 'toggle',
    default = 'flight',
    set = Look("remaster_menu_background"),
    custom = {
        states = {
            { text = "Remastered Flight", key = 'flight' },
            { text = "Original Dusk", key = 'dusk' },
            { text = "Still", key = 'still' },
        },
    },
})
table.insert(options.ui.items, {
    title = "Menu Battle Sound",
    key = 'remaster_menu_sound',
    tip = "The menu flight's battle heard from afar under the music, or not at all (the original's menu played music only). From the battle's next start.",
    type = 'toggle',
    default = 'off',
    set = Look("remaster_menu_sound"),
    custom = {
        states = {
            { text = "<LOC _Off>", key = 'off' },
            { text = "Distant", key = 'distant' },
        },
    },
})

-- The game's own graphics settings drive the renderer (Fidelity, Shadow Fidelity, Anti-Aliasing,
-- Level Of Detail, Texture Detail; ui/user/keymap.cpp) and mean what they say (Chris, 2026-10-15), so a player
-- who has never chosen starts at the full look: their defaults are raised to the highest. The
-- anti-aliasing choices are the renderer's (Off, 2, 4).
-- Options the remastered picture does its own way are left out of the menu (the renderer's answer
-- to ui request 24, 2026-10-16): Shadow Resolution (Shadow Fidelity sets the shadows' size),
-- Shadow render distance (the shadows cover every zoom) and Render Sky (the sky lights the scene;
-- without it the picture is wrong, not faster).
-- Vertical Sync and Frametime as well (2026-11-09, the review): they and the Remastered tab's Frame Rate set the
-- same frame pacing (ui/user/keymap.cpp, SC_VERTICALSYNC / SC_FRAMETIMECLAMP / REMASTER_FRAME_RATE). The game's two
-- send only on OK, ours also at the start, so a change on the Video tab held until the next start and then fell
-- back to Frame Rate's. Frame Rate has both (Vertical Sync; Uncapped or a limit), so it is the one place.
local leftOut = { shadow_resolution = true, shadow_render_distance = true, render_skydome = true, vsync = true, frametime = true }
for i = table.getn(options.video.items), 1, -1 do
    if leftOut[options.video.items[i].key] then
        table.remove(options.video.items, i)
    end
end
-- Video is the game's own again; where our looks were, a signpost to their tab (the game's 'button' row)
table.insert(options.video.items, Header("FA Remastered"))
table.insert(options.video.items, {
    title = "Picture options: the Remastered tab",
    key = 'remaster_show_tab',
    tip = "FA Remastered's own picture options (the Remastered Preset, sky, water, effects, units and more) are in the Remastered tab, next to this one.",
    type = 'button',
    default = 0,
    remasterNotSearched = true,
    set = function(key, value, startup) end,
    update = function(control, value) RemasterShowOptionsTab('remaster') end,
    custom = { text = "Show" },
})

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

-- The simulation's order for a new game that has no lobby (a campaign operation; the lobby's own option
-- "Simulation Order" decides for a skirmish or a network game): Chris, 2026-10-31, multicore option 2, our own order for
-- new games by default. Read at the launch (ui/app/game_session.cpp LaunchSinglePlayer). Replays play in their own.
-- (under a header of its own, as ours in the Interface tab: after FAF's "Cursor features" it looked as one of them)
table.insert(options.gameplay.items, Header("FA Remastered"))
table.insert(options.gameplay.items, {
    title = "Simulation Order for New Games",
    key = 'remaster_sim_order',
    tip = "How a new campaign operation works out each moment. Remastered: FA Remastered's own order, made to use many processor cores. Original: the original game's order. Both are exact; a skirmish takes the lobby's own choice, a replay the order it was recorded in.",
    type = 'toggle',
    default = 'parallel',
    custom = {
        states = {
            { text = "Remastered", key = 'parallel' },
            { text = "Original", key = 'original' },
        },
    },
})

-- A look of the Remastered Preset changed by hand: the preset shows Custom once that look is no longer its level's (the
-- design's optional step, docs/ui-options-layout.md 4.2; as the game's own Fidelity, Shadow Fidelity and Render Sky set
-- Fidelity Presets to Custom from their `update`: FAF's /lua/options/options.lua, with optionslogic.SetValue(key, 4, true)).
-- The dialog calls an item's `update` when the player chooses in its drop-down (combo.OnClick without skipUpdate) or lets
-- go of / drags its slider (OnValueSet, OnScrub: FAF's /lua/ui/dialogs/options.lua); the preset's own setting goes through
-- SetValue with skipUpdate, or for a slider through OnValueSet (SetRemasterPreset), which `presetSetting` lets pass. A look
-- set back to the level's value leaves the preset as it is then (Custom stays Custom, as the game's).
function RemasterPresetLookChanged(key, value)  -- (also for the tests)
    if presetSetting then return end
    local logic = import("/lua/options/optionslogic.lua")
    local preset = logic.FindOptionItem('remaster_preset')
    local combo = preset and preset.control
    if not combo or IsDestroyed(combo) or not combo.keyMap then return end
    local level = combo.keyMap[combo:GetItem()]
    local values = RemasterPresetValues(level)
    if values and values[key] ~= nil and values[key] ~= value then
        logic.SetValue('remaster_preset', 5, true)
    end
end
do
    local levelKeys = remasterPresets[4]
    for _, tabKey in optionsOrder do
        for _, item in (options[tabKey] and options[tabKey].items) or {} do
            if type(item.key) == 'string' and levelKeys[item.key] ~= nil and not item.remasterPresetWatched then
                local own, key = item.update, item.key
                item.remasterPresetWatched = true
                item.update = function(control, value)
                    if own then own(control, value) end
                    RemasterPresetLookChanged(key, value)
                end
            end
        end
    end
end

-- Our options' tips in the game's tooltip table: FAF's dialog shows only Tooltips["options_" .. key]
-- (/lua/ui/help/tooltips.lua; its CreateOption, Tooltip.CreateMouseoverDisplay) and never an item's `tip`, so until
-- 2026-11-08 hovering one of ours showed its key and "No Description". An entry already there (a later FAF's) is kept.
do
    local ok, tips = pcall(import, '/lua/ui/help/tooltips.lua')
    tips = ok and tips and tips.Tooltips
    if tips then
        for _, tabKey in optionsOrder do
            for _, item in (options[tabKey] and options[tabKey].items) or {} do
                if item.tip and type(item.key) == 'string' and string.find(item.key, '^remaster_')
                    and not tips['options_' .. item.key] then
                    tips['options_' .. item.key] = { title = item.title, description = item.tip }
                end
            end
        end
    else
        WARN("FA Remastered: the options' tips: no tooltip table")
    end
end
