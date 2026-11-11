-- The lobby object in its local form, checked from the script side.
--   moho64ui --init <init.lua> --stage setup --lua ui/tests/lobby.lua --frames 2 --log out.log
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local LobbyComm = import("/lua/ui/lobby/lobbyComm.lua")

local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end

local lobby = LobbyComm.CreateLobbyComm("None", 0, "Tester", nil, nil)
check("a lobby is made", lobby ~= nil and not IsDestroyed(lobby), true)
check("it has the script class's functions", type(lobby.Hosting), "function")
check("before hosting the local id is -1, as a string", lobby:GetLocalPlayerID(), "-1")
check("not host yet", lobby:IsHost(), false)
check("the local player's name", lobby:GetLocalPlayerName(), "Tester")
check("no port without a network protocol", lobby:GetLocalPort(), nil)

local hosting = 0
lobby.Hosting = function(self) hosting = hosting + 1 end
lobby:HostGame()
check("HostGame calls Hosting once", hosting, 1)
check("the host gets the id 0", lobby:GetLocalPlayerID(), "0")
check("host now", lobby:IsHost(), true)
check("hosting twice is an error", pcall(lobby.HostGame, lobby), false)
check("nobody else is in it", table.getn(lobby:GetPeers()), 0)
check("names stay as they are", lobby:MakeValidGameName("My Game") .. "|" .. lobby:MakeValidPlayerName("0", "Tester"), "My Game|Tester")
lobby:BroadcastData({ Type = "Test" })

-- launching: the host starts a session from the game configuration; a configuration whose
-- scenario file does not exist cannot start, and the script hears why
local heard = {}
lobby.LaunchFailed = function(self, reasonKey) table.insert(heard, "failed:" .. tostring(reasonKey)) end
lobby.GameLaunched = function(self) table.insert(heard, "launched") end
lobby:LaunchGame({ GameOptions = { ScenarioFile = "/maps/x/x_scenario.lua" }, PlayerOptions = {}, Observers = {}, GameMods = {} })
check("a launch that cannot start calls LaunchFailed with the reason", table.concat(heard, " "), "failed:NoConfig")

local uidLobby = LobbyComm.CreateLobbyComm("None", 0, "Other", "42", nil)
check("a given id is kept", uidLobby:GetLocalPlayerID(), "42")
uidLobby:Destroy()
check("Destroy", IsDestroyed(uidLobby), true)
check("the wrong number of arguments is an error", pcall(InternalCreateLobby, LobbyComm.LobbyComm, "None", 0), false)
lobby:Destroy()

-- The program's own game options (ui/data/hook): in the game's list beside the game's own, each
-- with a default that is one of its values.
local options = {}
for _, option in import('/lua/ui/lobby/lobbyOptions.lua').globalOpts do
    options[option.key] = option
end
check("the game's own options are there", options.FogOfWar ~= nil and options.Victory ~= nil, true)
for _, key in {'RemasterDayStart', 'RemasterDayLength', 'RemasterWeather', 'RemasterMapEdge'} do
    local option = options[key]
    local default = option and option.values[option.default]
    check("the option " .. key .. " is in the list with a default", default ~= nil, true)
end
check("a game starts in the morning unless chosen (Chris, 2026-10-18)", options.RemasterDayStart.values[options.RemasterDayStart.default].key, 'morning')
check("and the map's own light is the first choice", options.RemasterDayStart.values[1].key, 'map')
check("a day takes twenty minutes unless chosen", options.RemasterDayLength.values[options.RemasterDayLength.default], '20')
check("the map's edge has no machinery unless chosen (Chris's choice)", options.RemasterMapEdge.values[options.RemasterMapEdge.default].key, 'beyond')
check("and the original's frame is the other choice", options.RemasterMapEdge.values[2].key, 'frame')
check("and the map's own sculpted land the third (the renderer's request 62)", options.RemasterMapEdge.values[3].key, 'surround')

-- The program's own looks: options of each player in the Options menu's Remastered tab (after Video; Chris,
-- 2026-11-08), applied at the start (optionslogic.Apply) and when changed. One map of both tabs (the game's
-- own graphics settings stay in Video; headers have the key '').
local video = {}
for _, tab in {'video', 'remaster'} do
    for _, item in import('/lua/options/options.lua').options[tab].items do
        video[item.key] = item
    end
end
-- The Remastered tab itself (docs/ui-options-layout.md): after Video, the game's Video again its own, every key a
-- preset level sets on the preset's own page, each option in one tab only, eight sections, and our tips shown
do
    local O = import('/lua/options/options.lua')
    check("the tabs: gameplay, ui, video, remaster, sound", table.concat(O.optionsOrder, " "), "gameplay ui video remaster sound")
    check("the Remastered tab's title", O.options.remaster.title, "Remastered")
    local inVideo = {}
    for _, item in O.options.video.items do
        if type(item.key) == 'string' and string.find(item.key, '^remaster_') then table.insert(inVideo, item.key) end
    end
    check("the Video tab has none of ours but the signpost to the Remastered tab", table.concat(inVideo, " "), "remaster_show_tab")
    local last, beforeLast = O.options.video.items[table.getn(O.options.video.items)], O.options.video.items[table.getn(O.options.video.items) - 1]
    check("Video ends with the header FA Remastered and its Show button", beforeLast.type .. " " .. beforeLast.title .. " | " ..
        last.type .. " " .. last.custom.text, "header FA Remastered | button Show")
    local remaster, headers, empty, more = {}, {}, 0, false
    local items = O.options.remaster.items
    check("its first row: Go to Section", items[1].key .. " " .. items[1].type, "remaster_goto_section toggle")
    for i, item in items do
        if item.type == 'header' then
            table.insert(headers, item.title)
            if item.title == "More" then more = true end
            if not items[i + 1] or items[i + 1].type == 'header' then empty = empty + 1 end
        else
            remaster[item.key] = item
        end
    end
    check("eight sections under the game's header rows", table.concat(headers, "|"),
        "Display & Performance|Preset|Sky & Light|Ground & Weather|Water|Effects & Battle|Units & Readability|Picture & Camera")
    check("none empty, and no look left over for a section More", empty .. " " .. tostring(more), "0 false")
    check("Go to Section names the eight", table.getn(items[1].custom.states) .. " " .. items[1].custom.states[5].text, "8 Water")
    check("53 rows: Go to Section, 8 headers, 44 options", table.getn(items), 53)
    check("Haze & Mist is on the tab, set by no preset level (a taste setting)", tostring(remaster.remaster_haze_mist ~= nil) .. " " ..
        tostring(O.RemasterPresetValues(4).remaster_haze_mist) .. " " .. tostring(O.RemasterPresetValues(0).remaster_haze_mist), "true nil nil")
    local missing = {}
    for key in O.RemasterPresetValues(4) do
        if not remaster[key] then table.insert(missing, key) end
    end
    if not remaster.remaster_preset then table.insert(missing, 'remaster_preset') end
    check("every key a preset level sets is on the preset's tab", table.concat(missing, " "), "")
    -- the six that come before the preset are exactly the ones no level touches
    local before, touched = {}, {}
    for i = 1, table.getn(items) do
        if items[i].key == 'remaster_preset' then break end
        if items[i].type ~= 'header' and items[i].key ~= 'remaster_goto_section' then
            table.insert(before, items[i].key)
            if O.RemasterPresetValues(4)[items[i].key] ~= nil or O.RemasterPresetValues(0)[items[i].key] ~= nil then
                table.insert(touched, items[i].key)
            end
        end
    end
    check("Display & Performance: six, none set by a preset level", table.getn(before) .. " [" .. table.concat(touched, " ") .. "]", "6 []")
    local seen, twice = {}, {}
    for _, tabKey in O.optionsOrder do
        for _, item in O.options[tabKey].items do
            if item.type ~= 'header' then
                if seen[item.key] then table.insert(twice, item.key) end
                seen[item.key] = tabKey
            end
        end
    end
    check("every option in one tab only", table.concat(twice, " "), "")
    check("ours in Interface and Gameplay stayed there", tostring(seen.remaster_color_vision) .. " " .. tostring(seen.remaster_path_preview)
        .. " " .. tostring(seen.remaster_score_extras) .. " " .. tostring(seen.remaster_import_faf_keys) .. " " .. tostring(seen.remaster_sim_order),
        "ui ui ui ui gameplay")
    local function headerAbove(tabKey, key)
        local h = nil
        for _, item in O.options[tabKey].items do
            if item.type == 'header' then h = item.title end
            if item.key == key then return h end
        end
    end
    check("each under a header FA Remastered", tostring(headerAbove('ui', 'remaster_color_vision')) .. "|" ..
        tostring(headerAbove('ui', 'remaster_import_faf_keys')) .. "|" .. tostring(headerAbove('gameplay', 'remaster_sim_order')),
        "FA Remastered|FA Remastered|FA Remastered")
    local Tips = import('/lua/ui/help/tooltips.lua').Tooltips
    local tipless, wrong = {}, {}
    for _, tabKey in O.optionsOrder do
        for _, item in O.options[tabKey].items do
            if type(item.key) == 'string' and string.find(item.key, '^remaster_') then
                local t = Tips['options_' .. item.key]
                if not item.tip then table.insert(tipless, item.key)
                elseif not t or t.description ~= item.tip or t.title ~= item.title then table.insert(wrong, item.key) end
            end
        end
    end
    check("every option of ours has a tip", table.concat(tipless, " "), "")
    check("and the dialog's tooltip shows it (Tooltips['options_' .. key])", table.concat(wrong, " "), "")
end
for _, key in {'remaster_effects', 'remaster_effects_quality', 'remaster_tracers', 'remaster_rich_ground', 'remaster_snow_tracks'} do
    check("the Remastered tab has " .. key, video[key] ~= nil and video[key].set ~= nil, true)
end
check("the remastered effects are on unless chosen", video.remaster_effects and video.remaster_effects.default, 1)
check("the options took effect at the start", moho64ui_Looks().remasteredEffects, true)
video.remaster_tracers.set('remaster_tracers', 0, false)
check("a look switched off in the menu is off", moho64ui_Looks().tracers, false)
video.remaster_tracers.set('remaster_tracers', 1, false)
video.remaster_effects_quality.set('remaster_effects_quality', 0.5, false)
check("and the effects quality follows", moho64ui_Looks().effectsQuality, 0.5)
video.remaster_effects_quality.set('remaster_effects_quality', 1, false)
check("the Remastered tab has Render Scale (FSR 1 or FSR 3 scale it up), off unless chosen", video.remaster_upscale and video.remaster_upscale.default, 1)
-- FSR 3 (the renderer's request 30): off unless chosen; on, the scene is drawn with one sample a pixel
-- (FAF 3840, 2026-11-04: new games are made by 3840's rules, and the menus name that version too: Chris)
check("the front end's GetVersion: the version new games are made with", GetVersion(), "1.5.3840")
check("the Remastered tab has Temporal Upscaling (FSR 3), off unless chosen", video.remaster_temporal_upscale and video.remaster_temporal_upscale.default, 0)
video.remaster_temporal_upscale.set('remaster_temporal_upscale', 1, false)
check("on: FSR 3, and the anti-aliasing gives way (one sample a pixel)", tostring(moho64ui_Looks().temporalUpscale) .. " " .. moho64ui_Looks().samplesDrawn, "true 1")
video.remaster_temporal_upscale.set('remaster_temporal_upscale', 0, false)
-- (Frame Generation on keeps one sample too: render request 52, 2026-11-03; the test's preferences may have it on)
check("off again: the anti-aliasing's samples (one while Frame Generation is on)", moho64ui_Looks().samplesDrawn,
      moho64ui_Looks().frameGeneration and 1 or moho64ui_Looks().sceneSamples)
-- Every remastered look ON unless chosen (Chris, 2026-10-18); Frame Generation off (a speed trade-off)
for _, key in {'remaster_atmosphere', 'remaster_soft_shadows', 'remaster_weather_effects', 'remaster_ground_detail',
               'remaster_water_ripples', 'remaster_lens', 'remaster_battle_effects', 'remaster_energy_shields',
               'remaster_hidden_outlines'} do
    check("the Remastered tab has " .. key .. ", on unless chosen", video[key] and video[key].default, 1)
end
check("Frame Generation off unless chosen", video.remaster_frame_generation.default, 0)
check("Sea Sparkle off unless chosen (Chris, 2026-11-06: the sea is better without it)", video.remaster_sea_sparkle and video.remaster_sea_sparkle.default, 0)
do  -- (Chris, 2026-11-06: it adds input delay; no preset level, Ultra included, turns it on, and its tip says so)
    local O, on = import('/lua/options/options.lua'), false
    for level = 0, 4 do on = on or (O.RemasterPresetValues(level).remaster_frame_generation or 0) ~= 0 end
    check("no preset level turns Frame Generation on", on, false)
    check("its tip says it adds input delay", string.find(video.remaster_frame_generation.tip, "ADDS INPUT DELAY", 1, true) ~= nil, true)
end
-- The renderer's looks from Chris's big test (2026-11-01): each on unless chosen, the earlier look kept as Off, in no
-- preset level; at the start the program has their defaults (the test's preferences choose none of them)
for _, key in {'remaster_ocean_glitter', 'remaster_shore_soak', 'remaster_gateway', 'remaster_warpin_colours', 'remaster_ghost_look', 'remaster_guide',
               'remaster_cloak_look', 'remaster_stealth_veil', 'remaster_unit_detail', 'remaster_cloud_layout', 'remaster_storm_clouds', 'remaster_night_sky'} do
    check("the Remastered tab has " .. key .. ", on unless chosen", video[key] and video[key].set ~= nil and video[key].default, 1)
end
check("Shield Bending: Subtle unless chosen, of Off, Subtle and Full", video.remaster_shield_bend and
    video.remaster_shield_bend.default .. " " .. table.getn(video.remaster_shield_bend.custom.states), "1 3")
check("Unit Glow at Night: a slider, 75 of 100 unless chosen", video.remaster_unit_glow and
    video.remaster_unit_glow.type .. " " .. video.remaster_unit_glow.default .. " " .. video.remaster_unit_glow.custom.max, "slider 75 100")
check("Haze & Mist: a slider, 100 of 0..200 in steps of 10 unless chosen", video.remaster_haze_mist and
    video.remaster_haze_mist.type .. " " .. video.remaster_haze_mist.default .. " " .. video.remaster_haze_mist.custom.min .. " " ..
    video.remaster_haze_mist.custom.max .. " " .. video.remaster_haze_mist.custom.inc, "slider 100 0 200 10")
check("Sharpen: a slider, 50 of 100 unless chosen", video.remaster_sharpen and
    video.remaster_sharpen.type .. " " .. video.remaster_sharpen.default .. " " .. video.remaster_sharpen.custom.max, "slider 50 100")
do
    local l = moho64ui_Looks()
    check("the program starts with them on", tostring(l.oceanGlitter) .. " " .. tostring(l.shoreSoak) .. " " .. tostring(l.remasteredGate) ..
        " " .. tostring(l.ghostLook) .. " " .. tostring(l.guideOverGrass) .. " " .. tostring(l.cloakLook) .. " " .. tostring(l.stealthLook) .. " " .. tostring(not l.seaSparkle) ..
        " " .. tostring(l.unitDetail) .. " " .. tostring(l.cloudMix) .. " " .. tostring(l.stormCells) .. " " .. tostring(l.starryNight),
        "true true true true true true true true true true true true")
    check("Shield Bending Subtle, the units' glow 0.75, sharpening 0.5", l.shieldBend .. " " .. l.unitGlow .. " " .. l.sharpen, "1 0.75 0.5")
    check("the haze and mist as made", l.hazeMist, 1)
end
video.remaster_ocean_glitter.set('remaster_ocean_glitter', 0, false)
video.remaster_ghost_look.set('remaster_ghost_look', 0, false)
video.remaster_stealth_veil.set('remaster_stealth_veil', 0, false)
check("Ocean Glitter, Ghost Look and Stealth Veil switched off in the menu are off", tostring(moho64ui_Looks().oceanGlitter) .. " " ..
    tostring(moho64ui_Looks().ghostLook) .. " " .. tostring(moho64ui_Looks().stealthLook), "false false false")
video.remaster_ocean_glitter.set('remaster_ocean_glitter', 1, false)
video.remaster_ghost_look.set('remaster_ghost_look', 1, false)
video.remaster_stealth_veil.set('remaster_stealth_veil', 1, false)
video.remaster_shield_bend.set('remaster_shield_bend', 2, false)
check("Full Shield Bending chosen in the menu reaches the program", moho64ui_Looks().shieldBend, 2)
video.remaster_shield_bend.set('remaster_shield_bend', 0, false)
check("and Off", moho64ui_Looks().shieldBend, 0)
video.remaster_shield_bend.set('remaster_shield_bend', 1, false)
video.remaster_unit_glow.set('remaster_unit_glow', 100, false)
video.remaster_sharpen.set('remaster_sharpen', 0, false)
check("the sliders at 100 and 0: the earlier glow, no sharpening", moho64ui_Looks().unitGlow .. " " .. moho64ui_Looks().sharpen, "1 0")
video.remaster_unit_glow.set('remaster_unit_glow', 75, false)
video.remaster_sharpen.set('remaster_sharpen', 50, false)
video.remaster_haze_mist.set('remaster_haze_mist', 0, false)
check("Haze & Mist at 0 in the menu: clear air", moho64ui_Looks().hazeMist, 0)
video.remaster_haze_mist.set('remaster_haze_mist', 200, false)
check("at 200: twice as thick", moho64ui_Looks().hazeMist, 2)
ConExecute("remaster_haze_mist 7")
check("the console's remaster_haze_mist stays inside 0..2", moho64ui_Looks().hazeMist, 2)
ConExecute("remaster_haze_mist 0.6")
ConExecute("remaster_haze_mist")
check("and the bare name keeps the amount", string.format("%.2f", moho64ui_Looks().hazeMist), "0.60")
video.remaster_haze_mist.set('remaster_haze_mist', 100, false)
check("back at 100: as made", moho64ui_Looks().hazeMist, 1)
ConExecute("remaster_shield_bend full")
check("the console's remaster_shield_bend takes the words", moho64ui_Looks().shieldBend, 2)
ConExecute("remaster_shield_bend subtle")
do
    local O = import('/lua/options/options.lua')
    -- in every preset level by the renderer's costs (options.lua remasterPresets, 2026-11-04): High and Ultra their
    -- defaults, Medium without Sharpen, Low without Shore Soak too, Off the earlier looks (Ghost and Cloak the original's)
    local keys = {'remaster_ocean_glitter', 'remaster_shore_soak', 'remaster_shield_bend', 'remaster_unit_glow', 'remaster_sharpen',
                  'remaster_gateway', 'remaster_ghost_look', 'remaster_guide', 'remaster_cloak_look', 'remaster_stealth_veil',
                  'remaster_sea_sparkle', 'remaster_unit_detail', 'remaster_cloud_layout', 'remaster_storm_clouds', 'remaster_night_sky'}
    local function line(level)
        local values, out = O.RemasterPresetValues(level), {}
        for _, key in keys do table.insert(out, tostring(values[key])) end
        return table.concat(out, " ")
    end
    check("Ultra has them at their defaults", line(4), "1 1 1 75 50 1 1 1 1 1 0 1 1 1 1")
    check("High the same", line(3), line(4))
    check("Medium without Sharpen", line(2), "1 1 1 75 0 1 1 1 1 1 0 1 1 1 1")
    check("Low without Shore Soak too", line(1), "1 0 1 75 0 1 1 1 1 1 0 1 1 1 1")
    check("Off: the earlier looks, Ghost and Cloak the original's", line(0), "0 0 2 100 0 0 1 0 1 0 0 0 0 0 0")
    -- Warp-in in Faction Colours (the renderer's request 67, 2026-11-10): on from Low up, the earlier warp-in in Off
    check("Warp-in in Faction Colours: off in Off, on from Low", O.RemasterPresetValues(0).remaster_warpin_colours .. " " ..
        O.RemasterPresetValues(1).remaster_warpin_colours .. " " .. O.RemasterPresetValues(4).remaster_warpin_colours, "0 1 1")
    check("the program has it on at the start", moho64ui_Looks().warpInColours, true)
    video.remaster_warpin_colours.set('remaster_warpin_colours', 0, false)
    check("Off chosen in the menu reaches the program", moho64ui_Looks().warpInColours, false)
    video.remaster_warpin_colours.set('remaster_warpin_colours', 1, false)
    check("and On again", moho64ui_Looks().warpInColours, true)
end
check("the Photographic Look: Off unless chosen, and its five choices", video.remaster_photo_look and
    video.remaster_photo_look.default .. " " .. table.getn(video.remaster_photo_look.custom.states), "0 5")
video.remaster_photo_look.set('remaster_photo_look', 4, false)
check("Cold War chosen in the menu reaches the program", moho64ui_Looks().photoLook, 4)
video.remaster_photo_look.set('remaster_photo_look', 0, false)
check("and Off again", moho64ui_Looks().photoLook, 0)
-- The Season (render c267feb; Chris: seasons as a player's option): today's Summer unless chosen, in no preset level
check("the Season: Summer unless chosen, and its two choices", video.remaster_season and
    video.remaster_season.default .. " " .. table.getn(video.remaster_season.custom.states), "0 2")
check("the program starts in Summer", moho64ui_Looks().season, 0)
video.remaster_season.set('remaster_season', 1, false)
check("Autumn chosen in the menu reaches the program", moho64ui_Looks().season, 1)
video.remaster_season.set('remaster_season', 0, false)
check("and Summer again", moho64ui_Looks().season, 0)
ConExecute("remaster_season autumn")
check("the console's remaster_season takes the season's name", moho64ui_Looks().season, 1)
ConExecute("remaster_season summer")
do
    local O = import('/lua/options/options.lua')
    local inAny = false
    for level = 0, 4 do inAny = inAny or O.RemasterPresetValues(level).remaster_season ~= nil end
    check("the Season is in no preset level", inAny, false)
end
check("Ray Tracing on unless chosen, where the card can", video.remaster_ray_tracing.default, moho64ui_RayTracingSupported() == false and 0 or 1)
-- The Remastered Preset (Chris, 2026-10-17): the new looks set together; Custom (5) by default leaves them
check("the Remastered tab has the Remastered Preset, Ultra unless chosen", video.remaster_preset and video.remaster_preset.default, 4)
do
    local O = import('/lua/options/options.lua')
    local off, high, ultra = O.RemasterPresetValues(0), O.RemasterPresetValues(3), O.RemasterPresetValues(4)
    check("Off: the original look, the remastered effects off", off.remaster_look .. " " .. off.remaster_effects, "original 0")
    check("High: Atmosphere and Weather Effects on, Ray Tracing not yet", high.remaster_atmosphere .. " " .. high.remaster_weather_effects .. " " .. high.remaster_ray_tracing, "1 1 0")
    check("Ultra: Ray Tracing where the card can", ultra.remaster_ray_tracing, moho64ui_RayTracingSupported() == false and 0 or 1)
    check("Water Ripples come on with Weather Effects: Medium off, High and Ultra on",
        O.RemasterPresetValues(2).remaster_water_ripples .. " " .. high.remaster_water_ripples .. " " .. ultra.remaster_water_ripples, "0 1 1")
    check("the Cinematic Lens Blur is in no level", off.remaster_lens == nil and high.remaster_lens == nil and ultra.remaster_lens == nil, true)
    check("Custom touches nothing", O.RemasterPresetValues(5), nil)
    local low, medium = O.RemasterPresetValues(1), O.RemasterPresetValues(2)
    check("Hidden Unit Outlines from Low, Battle Effects and Energy Shields from Medium, none at Off",
        off.remaster_hidden_outlines .. low.remaster_hidden_outlines .. low.remaster_battle_effects .. medium.remaster_battle_effects ..
        medium.remaster_energy_shields .. ultra.remaster_battle_effects, "010111")
end
check("on at first: the lighting, Water Ripples, the lens, Battle Effects, Energy Shields, Hidden Unit Outlines",
    tostring(moho64ui_Looks().softShadows and moho64ui_Looks().weatherEffects and moho64ui_Looks().atmosphere and
             moho64ui_Looks().waterRipples and moho64ui_Looks().lens and moho64ui_Looks().battleEffects and
             moho64ui_Looks().energyShields and moho64ui_Looks().hiddenOutlines), "true")
video.remaster_soft_shadows.set('remaster_soft_shadows', 0, false)
video.remaster_weather_effects.set('remaster_weather_effects', 0, false)
video.remaster_battle_effects.set('remaster_battle_effects', 0, false)
check("switched off in the menu: Soft Shadows, Weather Effects, Battle Effects, nothing else", tostring(moho64ui_Looks().softShadows) .. " " ..
    tostring(moho64ui_Looks().weatherEffects) .. " " .. tostring(moho64ui_Looks().battleEffects) .. " " .. tostring(moho64ui_Looks().atmosphere) ..
    " " .. tostring(moho64ui_Looks().energyShields), "false false false true true")
video.remaster_soft_shadows.set('remaster_soft_shadows', 1, false)
video.remaster_weather_effects.set('remaster_weather_effects', 1, false)
video.remaster_battle_effects.set('remaster_battle_effects', 1, false)
video.remaster_water_ripples.set('remaster_water_ripples', 0, false)
video.remaster_lens.set('remaster_lens', 0, false)
check("Water Ripples and the lens off in the menu", tostring(moho64ui_Looks().waterRipples) .. " " .. tostring(moho64ui_Looks().lens), "false false")
video.remaster_water_ripples.set('remaster_water_ripples', 1, false)
video.remaster_lens.set('remaster_lens', 1, false)
-- (no picture here says whether the card can ray trace: both states; where it cannot, one, greyed out)
check("Ray Tracing offers Off and On until the renderer says the card cannot",
    tostring(moho64ui_RayTracingSupported()) .. " " .. table.getn(video.remaster_ray_tracing.custom.states), "nil 2")
-- The game's own graphics settings drive the renderer and mean what they say; a player who has
-- never chosen starts at the full look.
check("Fidelity is High unless chosen", video.fidelity and video.fidelity.default, 2)
check("Shadow Fidelity is High unless chosen", video.shadow_quality and video.shadow_quality.default, 3)
check("Level Of Detail is High unless chosen", video.level_of_detail and video.level_of_detail.default, 2)
check("Anti-Aliasing is 4 unless chosen, of Off, 2 and 4", video.antialiasing and (video.antialiasing.default .. " " .. table.getn(video.antialiasing.custom.states)), "4 3")
local aa = GetAntiAliasingOptions()
check("the card's anti-aliasing choices", table.getn(aa) .. ": " .. tostring(aa[1]) .. " " .. tostring(aa[2]), "2: 2 4")
video.shadow_quality.set('shadow_quality', 0, false)
check("Shadow Fidelity Off turns the shadows off", moho64ui_Looks().shadows, 0)
video.level_of_detail.set('level_of_detail', 0, false)
check("Level Of Detail Low halves the model detail", moho64ui_Looks().meshDetail, 0.5)
video.fidelity.set('fidelity', 0, false)
check("Fidelity Low is noted", moho64ui_Looks().fidelity, 0)
check("Texture Detail is High unless chosen", video.texture_level and video.texture_level.default, 0)
video.texture_level.set('texture_level', 2, false)
check("Texture Detail Low leaves out the two largest sizes", moho64ui_Looks().textureDetail, 2)
video.texture_level.set('texture_level', 0, false)
ConExecute("SC_AntiAliasingSamples 2")
check("Anti-Aliasing 2 draws the scene with 2 samples", moho64ui_Looks().sceneSamples, 2)
ConExecute("SC_AntiAliasingSamples 16")
check("and more than 4 with 4", moho64ui_Looks().sceneSamples, 4)
video.shadow_quality.set('shadow_quality', 3, false)
video.level_of_detail.set('level_of_detail', 2, false)
video.fidelity.set('fidelity', 2, false)
-- The renderer's whole look: remastered unless chosen, the original's one click away
check("the Remastered tab has Look, Remastered unless chosen", video.remaster_look and video.remaster_look.default, 'remastered')
video.remaster_look.set('remaster_look', 'original', false)
check("Look Original gives the renderer's original look", moho64ui_Looks().originalLook, true)
video.remaster_look.set('remaster_look', 'remastered', false)
check("and back", moho64ui_Looks().originalLook, false)
-- The graphics API: Direct3D 12 unless chosen (Chris's choice), or Vulkan
check("the Remastered tab has Graphics API, Direct3D 12 unless chosen", video.remaster_graphics_api and video.remaster_graphics_api.default, 'd3d12')
check("the pictures are drawn with Direct3D 12 at the start", moho64ui_Looks().vulkan, false)
video.remaster_graphics_api.set('remaster_graphics_api', 'vulkan', false)
check("Vulkan chosen: the next picture is drawn with Vulkan", moho64ui_Looks().vulkan, true)
video.remaster_graphics_api.set('remaster_graphics_api', 'd3d12', false)
check("and back", moho64ui_Looks().vulkan, false)
-- The frame rate: vertical sync unless chosen (Chris's choice), uncapped, or a cap
check("the Remastered tab has Frame Rate, vertical sync unless chosen", video.remaster_frame_rate and video.remaster_frame_rate.default, 'vsync')
do  -- (as the profile has it: the suite's template may have chosen another)
    local chosen = (import('/lua/user/prefs.lua').GetFromCurrentProfile('options') or {}).remaster_frame_rate or 'vsync'
    check("the window keeps to the screen's rate at the start, unless the profile chose otherwise", moho64ui_Looks().verticalSync, chosen == 'vsync')
end
video.remaster_frame_rate.set('remaster_frame_rate', 60, false)
check("a cap of 60 makes frames of 1000/60 ms", math.abs(moho64ui_Looks().frameMs - 1000 / 60) < 0.01 and not moho64ui_Looks().verticalSync, true)
video.remaster_frame_rate.set('remaster_frame_rate', 'uncapped', false)
check("uncapped has no shortest frame", moho64ui_Looks().frameMs, 0)
video.remaster_frame_rate.set('remaster_frame_rate', 'vsync', false)
check("and back to vertical sync", moho64ui_Looks().verticalSync, true)
-- Low Latency (the renderer's latency limiter, render 0ed9975): on unless chosen, in no preset level
check("the Remastered tab has Low Latency, on unless chosen", video.remaster_low_latency and video.remaster_low_latency.default, 1)
check("the window's pictures are told so at the start", moho64ui_Looks().lowLatency, true)
video.remaster_low_latency.set('remaster_low_latency', 0, false)
check("Low Latency off in the menu reaches the program", moho64ui_Looks().lowLatency, false)
video.remaster_low_latency.set('remaster_low_latency', 1, false)
check("and on again", moho64ui_Looks().lowLatency, true)
do
    local O = import('/lua/options/options.lua')
    local inAny = false
    for level = 0, 4 do inAny = inAny or O.RemasterPresetValues(level).remaster_low_latency ~= nil end
    check("Low Latency is in no preset level", inAny, false)
end
-- Options the remastered picture does its own way are not in the menu; the glow is on unless chosen
for _, key in {'shadow_resolution', 'shadow_render_distance', 'render_skydome', 'vsync', 'frametime'} do
    check("the Video tab leaves out " .. key, video[key], nil)
end
check("Bloom Render is on unless chosen", video.bloom_render and video.bloom_render.default, 1)
video.bloom_render.set('bloom_render', 0, false)
check("Bloom Render Off takes the glow away", moho64ui_Looks().bloom, false)
video.bloom_render.set('bloom_render', 1, false)
check("and On gives it back", moho64ui_Looks().bloom, true)
video.bloom_intensity.set('bloom_intensity', 10, false)
check("Bloom Intensity 10 is a glow of 1.0", math.abs(moho64ui_Looks().bloomIntensity - 1.0) < 0.001, true)
video.bloom_intensity.set('bloom_intensity', 15, false)
check("the presets run without a missing option", pcall(video.fidelity_presets.update, nil, 3), true)

-- Front-end data: kept outside the Lua state (so the game's state can hand it to the front end's),
-- a copy of plain values
SetFrontEndData('moho64ui_test', {opID = 'X1CA_002', campaignID = 'uef', difficulty = 2, nested = {true}})
local fed = GetFrontEndData('moho64ui_test')
check("front-end data comes back as a copy", fed and (fed.opID .. " " .. fed.campaignID .. " " .. fed.difficulty .. " " .. tostring(fed.nested[1])), "X1CA_002 uef 2 true")
SetFrontEndData('moho64ui_test', nil)
check("and nil takes it away", GetFrontEndData('moho64ui_test'), nil)

-- The display mode: "Windowed" and each screen's own mode, the main screen's first, listed at the
-- start (no window here, so the command line has not chosen); the choice is a request the window's
-- loop takes (the screens themselves: ui/tests/screens.lua)
local adapter = video.primary_adapter and video.primary_adapter.custom and video.primary_adapter.custom.states or {}
check("the display modes are Windowed and one per screen", table.getn(adapter) == 1 + moho64ui_Looks().screens and adapter[1].key == 'windowed'
    and string.find(adapter[2].key or '', '^%d+,%d+,%d+$') ~= nil, true)
check("and a profile that never chose takes the main screen's own", video.primary_adapter.default, adapter[2] and adapter[2].key)
local second = video.secondary_adapter and video.secondary_adapter.custom and video.secondary_adapter.custom.states or {}
check("no second screen to choose (no window here: no renderer's second window to show it)", table.getn(second) == 1 and second[1].key == 'disabled', true)
ConExecute("SC_PrimaryAdapter windowed")
check("Windowed asks the window to leave a full screen", moho64ui_Looks().displayRequest, 1)
check("at the size it had as a window (1024x768 when none is kept)", moho64ui_Looks().windowedWidth .. "x" .. moho64ui_Looks().windowedHeight, "1024x768")
ConExecute("SC_PrimaryAdapter " .. (adapter[2] and adapter[2].key or "1,1,1"))
check("a mode asks for a full screen", moho64ui_Looks().displayRequest, 2)
ConExecute("SC_PrimaryAdapter overridden")
check("and Command Line Override asks nothing new", moho64ui_Looks().displayRequest, 2)

-- The camera's options and the strategic icons (other tabs of the menu): each does what it says
local all = {}
for _, section in import('/lua/options/options.lua').options do
    for _, item in section.items or {} do
        all[item.key] = item
    end
end
local function option(key, value)
    all[key].set(key, value, false)
    return moho64ui_Looks()
end
check("Zoom Wheel Sensitivity 80 zooms 0.8 a notch", math.abs(option('wheel_sensitivity', 80).zoomAmount - 0.8) < 0.001, true)
check("Pan Speed 200", option('keyboard_pan_speed', 200).panSpeed, 200)
check("Accelerated Pan Speed Multiplier 6", option('keyboard_pan_accelerate_multiplier', 6).panFast, 6)
check("Keyboard Rotation Speed 25", option('keyboard_rotate_speed', 25).turnSpeed, 25)
check("Accelerated Keyboard Rotate Speed Multiplier 3", option('keyboard_rotate_accelerate_multiplier', 3).turnFast, 3)
check("Screen Edge Pans Main View Off", option('screen_edge_pans_main_view', 0).edgePans, false)
check("Arrow Keys Pan Main View Off", option('arrow_keys_pan_main_view', 0).arrowsPan, false)
check("Lock Fullscreen Cursor To Window On", option('lock_fullscreen_cursor_to_window', 1).lockCursor, true)
check("Scale strategic icons 200%", option('strat_icon_scale', 2.0).iconScale, 2)
check("Always Show Strategic Icons On", option('strat_icons_always_on', 1).iconsAlways, true)
option('wheel_sensitivity', 40)
option('keyboard_pan_speed', 90)
option('keyboard_pan_accelerate_multiplier', 4)
option('keyboard_rotate_speed', 10)
option('keyboard_rotate_accelerate_multiplier', 2)
option('screen_edge_pans_main_view', 1)
option('arrow_keys_pan_main_view', 1)
option('lock_fullscreen_cursor_to_window', 0)
option('strat_icon_scale', 1.0)
local back = option('strat_icons_always_on', 0)
check("and all back to the game's defaults", math.abs(back.zoomAmount - 0.4) < 0.001 and back.panSpeed == 90 and back.panFast == 4
    and back.turnSpeed == 10 and back.turnFast == 2 and back.edgePans and back.arrowsPan and not back.lockCursor and back.iconScale == 1
    and not back.iconsAlways, true)

LOG("TEST done: " .. failed .. " failed")
