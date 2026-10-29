-- The network lobby over loopback, the second joiner's part (see net_host.lua). It joins once Joiner1 has (it waits
-- a few seconds, then joins, again with a new lobby while the host's program is still starting), gets uid 2, hears
-- Joiner1's broadcast straight from it (the full mesh: SenderID "1"), and is then ejected by the host ("TestKick").
--   moho64ui --init <init.lua> --stage frontend --lua ui/tests/net_join2.lua --frames 20000 --log join2.log
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
local function WaitFor(cond, seconds)  -- (the PC's clock: the other programs run at their own pace)
    local deadline = moho64ui_WallSeconds() + seconds
    while not cond() and moho64ui_WallSeconds() < deadline do WaitFrames(1) end
    return cond()
end

local LobbyComm = import("/lua/ui/lobby/lobbyComm.lua")
local lobby, welcome, ping, ejected, failedWhy = nil, nil, nil, nil, nil
local function NewLobby()
    lobby = LobbyComm.CreateLobbyComm("TCP", 0, "Joiner2", nil, nil)
    lobby.ConnectionToHostEstablished = function(self, localId, localName, hostId) welcome = { localId, localName, hostId } end
    lobby.ConnectionFailed = function(self, reason) failedWhy = reason end
    lobby.DataReceived = function(self, data)
        if data.Type == 'Ping' then ping = data end
    end
    lobby.Ejected = function(self, reason) ejected = reason end
end

ForkThread(function()
    WaitFor(function() return false end, 6)  -- (after Joiner1)
    local deadline = moho64ui_WallSeconds() + 60
    repeat
        if lobby then lobby:Destroy() end
        failedWhy = nil
        NewLobby()
        lobby:JoinGame("127.0.0.1:16111", "Host", "0")
        WaitFor(function() return welcome ~= nil or failedWhy ~= nil end, 10)
        if not welcome then WaitFor(function() return false end, 0.5) end
    until welcome or moho64ui_WallSeconds() > deadline
    check("the host welcomes us", welcome ~= nil, true)
    check("as uid 2", welcome and welcome[1], "2")
    lobby:SendData("0", { Type = 'Hello' })
    check("Joiner1's broadcast reaches us", WaitFor(function() return ping ~= nil end, 60), true)
    check("straight from Joiner1", ping and ping.SenderID, "1")
    check("with its fields", ping and ping.N, 7)
    check("then the host ejects us", WaitFor(function() return ejected ~= nil end, 60) and ejected, "TestKick")
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
