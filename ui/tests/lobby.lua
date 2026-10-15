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

LOG("TEST done: " .. failed .. " failed")
