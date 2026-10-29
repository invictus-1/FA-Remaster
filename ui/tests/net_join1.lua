-- The network lobby over loopback, the first joiner's part (see net_host.lua). It joins 127.0.0.1:16111 (again with a
-- new lobby while the host's program is still starting), says hello to the host and hears "Ack" from it alone, waits
-- for Joiner2 to be connected to it directly (the host's NewPeer: every pair of players connects itself) and
-- broadcasts "Ping", which Joiner2 must hear from it; once Joiner2 has been ejected (PeerDisconnected), it leaves.
--   moho64ui --init <init.lua> --stage frontend --lua ui/tests/net_join1.lua --frames 20000 --log join1.log
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
local lobby, welcome, ack, gone, failedWhy = nil, nil, nil, {}, nil
local function NewLobby()
    lobby = LobbyComm.CreateLobbyComm("TCP", 0, "Joiner1", nil, nil)
    lobby.ConnectionToHostEstablished = function(self, localId, localName, hostId) welcome = { localId, localName, hostId } end
    lobby.ConnectionFailed = function(self, reason) failedWhy = reason end
    lobby.DataReceived = function(self, data)
        if data.Type == 'Ack' then ack = data end
    end
    lobby.PeerDisconnected = function(self, name, uid) table.insert(gone, name) end
end

ForkThread(function()
    local deadline = moho64ui_WallSeconds() + 60
    repeat  -- (until the host listens)
        if lobby then lobby:Destroy() end
        failedWhy = nil
        NewLobby()
        lobby:JoinGame("127.0.0.1:16111", "Host", "0")
        WaitFor(function() return welcome ~= nil or failedWhy ~= nil end, 10)
        if not welcome then WaitFor(function() return false end, 0.5) end
    until welcome or moho64ui_WallSeconds() > deadline
    check("the host welcomes us", welcome ~= nil, true)
    check("as uid 1", welcome and welcome[1], "1")
    check("by our name", welcome and welcome[2], "Joiner1")
    check("the host is uid 0", welcome and welcome[3], "0")
    check("our id now", lobby:GetLocalPlayerID(), "1")
    lobby:SendData("0", { Type = 'Hello' })
    check("the host answers us alone", WaitFor(function() return ack ~= nil end, 60) and ack.To, "Joiner1")
    check("from the host's uid", ack and ack.SenderID, "0")
    check("and name", ack and ack.SenderName, "Host")
    check("Joiner2 is connected to us directly", WaitFor(function()
        local p = lobby:GetPeer("2")
        return p ~= nil and p.status == 'Established'
    end, 60), true)
    WaitFor(function() return false end, 0.5)
    lobby:BroadcastData({ Type = 'Ping', From = 'Joiner1', N = 7 })
    check("we hear Joiner2 ejected", WaitFor(function()
        for _, n in gone do if n == 'Joiner2' then return true end end
        return false
    end, 60), true)
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
