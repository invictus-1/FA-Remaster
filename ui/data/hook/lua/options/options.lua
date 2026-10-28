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
-- Hidden Unit Outlines from Low on (readability), Battle Effects and Energy Shields from Medium on (each well under
-- half a millisecond, the renderer's measure). Chris, 2026-10-18: a player starts with every look on at its
-- highest, Ultra ("if they can't handle it they lower it").
local remasterPresets = {
    [0] = { remaster_look = 'original', remaster_effects = 0, remaster_effects_quality = 1, remaster_tracers = 0,
            remaster_rich_ground = 0, remaster_snow_tracks = 0, remaster_soft_shadows = 0,
            remaster_ground_detail = 0, remaster_atmosphere = 0, remaster_weather_effects = 0,
            remaster_water_ripples = 0, remaster_ray_tracing = 0, remaster_hidden_outlines = 0,
            remaster_battle_effects = 0, remaster_energy_shields = 0 },
    [1] = { remaster_look = 'remastered', remaster_effects = 1, remaster_effects_quality = 0.5,
            remaster_tracers = 1, remaster_rich_ground = 1, remaster_snow_tracks = 1, remaster_soft_shadows = 0,
            remaster_ground_detail = 0, remaster_atmosphere = 0, remaster_weather_effects = 0,
            remaster_water_ripples = 0, remaster_ray_tracing = 0, remaster_hidden_outlines = 1,
            remaster_battle_effects = 0, remaster_energy_shields = 0 },
    [2] = { remaster_look = 'remastered', remaster_effects = 1, remaster_effects_quality = 0.75,
            remaster_tracers = 1, remaster_rich_ground = 1, remaster_snow_tracks = 1, remaster_soft_shadows = 1,
            remaster_ground_detail = 1, remaster_atmosphere = 0, remaster_weather_effects = 0,
            remaster_water_ripples = 0, remaster_ray_tracing = 0, remaster_hidden_outlines = 1,
            remaster_battle_effects = 1, remaster_energy_shields = 1 },
    [3] = { remaster_look = 'remastered', remaster_effects = 1, remaster_effects_quality = 1,
            remaster_tracers = 1, remaster_rich_ground = 1, remaster_snow_tracks = 1, remaster_soft_shadows = 1,
            remaster_ground_detail = 1, remaster_atmosphere = 1, remaster_weather_effects = 1,
            remaster_water_ripples = 1, remaster_ray_tracing = 0, remaster_hidden_outlines = 1,
            remaster_battle_effects = 1, remaster_energy_shields = 1 },
    [4] = { remaster_look = 'remastered', remaster_effects = 1, remaster_effects_quality = 1,
            remaster_tracers = 1, remaster_rich_ground = 1, remaster_snow_tracks = 1, remaster_soft_shadows = 1,
            remaster_ground_detail = 1, remaster_atmosphere = 1, remaster_weather_effects = 1,
            remaster_water_ripples = 1, remaster_ray_tracing = 1, remaster_hidden_outlines = 1,
            remaster_battle_effects = 1, remaster_energy_shields = 1 },
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

-- The first-start graphics check's next step down (ui/data/hook/lua/ui/game/gamemain.lua; Chris, 2026-10-18: "turn
-- ray tracing off first, then step the preset down"): given the options as they stand, the changes that lower the
-- remastered looks by one step and, for a step of the preset, the level it went to; nil when nothing is left to lower.
-- Ray Tracing goes first (where the card can ray trace: elsewhere it does nothing, and is left as it is); then the
-- Remastered Preset one level down, Ultra, High, Medium, Low, never to Off (the original's look), a level that would
-- change nothing being passed over (High after Ray Tracing went: it is Ultra without it). A level only ever lowers:
-- what the player has set lower than the level has stays as he set it (each value the lesser of his and the level's;
-- his Original Look stays), so from Custom the levels are tried from Ultra down. The Remastered Preset then names the
-- level the looks match, or Custom (as the Video tab would show them).
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
        tip = "Sets the remastered looks below together, from Off (the original game's look) to Ultra. Each can still be changed after; Custom leaves them as they are.",
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
        tip = "Blast waves that press the grass and bend the trees, dust and debris, smoke and fire lit properly with their shadows, heat shimmer, wrecks and craters that glow and cool, dust, spray and contrails behind moving units, engines' exhausts as glowing plumes, explosions that light their surroundings, remade build effects, and battle scars on units that survive.",
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
for _, item in looks do
    table.insert(options.video.items, item)
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
