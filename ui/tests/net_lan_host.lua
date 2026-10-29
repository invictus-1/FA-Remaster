-- LAN games, the host's part (with net_lan_browse.lua, two programs at once: the suite's net_lobby runs them after the
-- lobby's three). It hosts a "UDP" lobby on port 16112, so its LAN listener on 15000 opens and answers the browser
-- with its GameConfigRequested table; after 12 s of the PC's clock it closes, and the browser must see it go.
--   moho64ui --init <init.lua> --stage frontend --lua ui/tests/net_lan_host.lua --frames 2000000000 --log lanhost.log
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local function WaitFor(cond, seconds)
    local deadline = moho64ui_WallSeconds() + seconds
    while not cond() and moho64ui_WallSeconds() < deadline do WaitFrames(1) end
    return cond()
end

local lobby = import("/lua/ui/lobby/lobbyComm.lua").CreateLobbyComm("UDP", 16112, "LanHost", nil, nil)
check("the lobby opens its port", lobby ~= nil, true)
local asked = 0
lobby.GameConfigRequested = function(self)
    asked = asked + 1
    return { GameName = "Test LAN game", Map = "/maps/SCMP_009/SCMP_009_scenario.lua", Players = 1 }
end
lobby:HostGame()
ForkThread(function()
    check("a browser asks for the game", WaitFor(function() return asked > 0 end, 60), true)
    WaitFor(function() return false end, 12)
    lobby:Destroy()
    LOG("TEST done: " .. failed .. " failed")
    ExitApplication()
end)
-- (a hang ends the run after two minutes of the PC's clock)
ForkThread(function()
    local stop = moho64ui_WallSeconds() + 120
    while moho64ui_WallSeconds() < stop do WaitFrames(1) end
    LOG("TEST FAIL the run took over two minutes")
    LOG("TEST done: hung")
    ExitApplication()
end)
