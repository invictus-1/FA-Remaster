-- LAN games, the browser's part (see net_lan_host.lua). FAF's own discovery service (lobbyComm.CreateDiscoveryService)
-- finds the host's game with its table and the engine's fields (Address "ip:port", Protocol, Hostname), counts it,
-- and hears RemoveGame once the host has gone quiet for 5 s; ValidateIPAddress resolves an address.
--   moho64ui --init <init.lua> --stage frontend --lua ui/tests/net_lan_browse.lua --frames 2000000000 --log lanbrowse.log
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

check("ValidateIPAddress resolves a host:port", ValidateIPAddress("localhost:16112"), "127.0.0.1:16112")
check("and gives nil without a port", ValidateIPAddress("localhost"), nil)

local discovery = import("/lua/ui/lobby/lobbyComm.lua").CreateDiscoveryService()
local found, removed = nil, nil
discovery.GameFound = function(self, index, config)
    if config.GameName == "Test LAN game" then found = { index = index, config = config } end
end
discovery.GameUpdated = function(self, index, config) end
discovery.RemoveGame = function(self, index)
    if found and index == found.index then removed = index end
end
ForkThread(function()
    check("the host's game is found", WaitFor(function() return found ~= nil end, 60), true)
    local c = found and found.config or {}
    check("with its table", c.Map, "/maps/SCMP_009/SCMP_009_scenario.lua")
    check("its game port in Address", string.find(c.Address or "", ":16112$") ~= nil, true)
    check("and its protocol", c.Protocol, "UDP")
    check("a Hostname", type(c.Hostname), "string")
    check("counted", discovery:GetGameCount(), 1)
    check("gone once the host has left", WaitFor(function() return removed ~= nil end, 40), true)
    check("not counted then", discovery:GetGameCount(), 0)
    discovery:Destroy()
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
