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
check("a game has the map's own light unless chosen", options.RemasterDayStart.values[options.RemasterDayStart.default].key, 'map')
check("a day takes twenty minutes unless chosen", options.RemasterDayLength.values[options.RemasterDayLength.default], '20')
check("the map's edge has no machinery unless chosen (Chris's choice)", options.RemasterMapEdge.values[options.RemasterMapEdge.default].key, 'beyond')
check("and the original's frame is the other choice", options.RemasterMapEdge.values[2].key, 'frame')

-- The program's own looks: options of each player in the Options menu's Video tab, applied at
-- the start (optionslogic.Apply) and when changed.
local video = {}
for _, item in import('/lua/options/options.lua').options.video.items do
    video[item.key] = item
end
for _, key in {'remaster_effects', 'remaster_effects_quality', 'remaster_tracers', 'remaster_rich_ground', 'remaster_snow_tracks'} do
    check("the Video tab has " .. key, video[key] ~= nil and video[key].set ~= nil, true)
end
check("the remastered effects are on unless chosen", video.remaster_effects and video.remaster_effects.default, 1)
check("the options took effect at the start", moho64ui_Looks().remasteredEffects, true)
video.remaster_tracers.set('remaster_tracers', 0, false)
check("a look switched off in the menu is off", moho64ui_Looks().tracers, false)
video.remaster_tracers.set('remaster_tracers', 1, false)
video.remaster_effects_quality.set('remaster_effects_quality', 0.5, false)
check("and the effects quality follows", moho64ui_Looks().effectsQuality, 0.5)
video.remaster_effects_quality.set('remaster_effects_quality', 1, false)
check("the Video tab has Upscaling (FSR 1), off unless chosen", video.remaster_upscale and video.remaster_upscale.default, 1)
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
ConExecute("SC_AntiAliasingSamples 2")
check("Anti-Aliasing 2 draws the scene with 2 samples", moho64ui_Looks().sceneSamples, 2)
ConExecute("SC_AntiAliasingSamples 16")
check("and more than 4 with 4", moho64ui_Looks().sceneSamples, 4)
video.shadow_quality.set('shadow_quality', 3, false)
video.level_of_detail.set('level_of_detail', 2, false)
video.fidelity.set('fidelity', 2, false)
-- The frame rate: vertical sync unless chosen (Chris's choice), uncapped, or a cap
check("the Video tab has Frame Rate, vertical sync unless chosen", video.remaster_frame_rate and video.remaster_frame_rate.default, 'vsync')
check("the window keeps to the screen's rate at the start", moho64ui_Looks().verticalSync, true)
video.remaster_frame_rate.set('remaster_frame_rate', 60, false)
check("a cap of 60 makes frames of 1000/60 ms", math.abs(moho64ui_Looks().frameMs - 1000 / 60) < 0.01 and not moho64ui_Looks().verticalSync, true)
video.remaster_frame_rate.set('remaster_frame_rate', 'uncapped', false)
check("uncapped has no shortest frame", moho64ui_Looks().frameMs, 0)
video.remaster_frame_rate.set('remaster_frame_rate', 'vsync', false)
check("and back to vertical sync", moho64ui_Looks().verticalSync, true)

LOG("TEST done: " .. failed .. " failed")
