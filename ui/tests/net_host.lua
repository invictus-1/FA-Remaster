-- The network lobby over loopback, the host's part (with net_join1.lua and net_join2.lua, three programs at once: the
-- suite's net_lobby). It hosts on TCP port 16111 through FAF's own lobbyComm; each joiner's "Hello" is answered with
-- "Ack" to that joiner alone; once both have joined and see each other (EstablishedPeers), the second is ejected
-- ("TestKick"); then the first leaves, and the host hears it.
--   moho64ui --init <init.lua> --stage frontend --lua ui/tests/net_host.lua --frames 6000 --log host.log
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

local lobby = import("/lua/ui/lobby/lobbyComm.lua").CreateLobbyComm("TCP", 16111, "Host", nil, nil)
check("the lobby opens its port", lobby ~= nil, true)
if not lobby then
    LOG("TEST done: " .. failed .. " failed")
    return
end
check("on the port asked for", lobby:GetLocalPort(), 16111)

local hellos, gone, established = {}, {}, {}
lobby.Hosting = function(self) LOG("TEST note: hosting") end
lobby.DataReceived = function(self, data)
    if data.Type == 'Hello' then
        table.insert(hellos, data.SenderName)
        self:SendData(data.SenderID, { Type = 'Ack', To = data.SenderName })
    end
end
lobby.PeerDisconnected = function(self, name, uid) table.insert(gone, name) end
lobby.EstablishedPeers = function(self, uid, peers) established[uid] = table.getn(peers) end
lobby:HostGame()
check("the host is uid 0", lobby:GetLocalPlayerID(), "0")
check("and the host", lobby:IsHost(), true)

ForkThread(function()
    local function WaitFor(cond, seconds)  -- (the PC's clock: the other programs run at their own pace)
        local deadline = moho64ui_WallSeconds() + seconds
        while not cond() and moho64ui_WallSeconds() < deadline do WaitFrames(1) end
        return cond()
    end
    check("both joiners said hello", WaitFor(function() return table.getn(hellos) >= 2 end, 60), true)
    local peers = lobby:GetPeers()
    check("two peers", table.getn(peers), 2)
    local names = {}
    for _, p in peers do names[p.name] = p end
    check("Joiner1 is uid 1", names.Joiner1 and names.Joiner1.id, "1")
    check("Joiner2 is uid 2", names.Joiner2 and names.Joiner2.id, "2")
    check("Joiner1 is established", names.Joiner1 and names.Joiner1.status, "Established")
    check("GetPeer gives it by uid", lobby:GetPeer("1") and lobby:GetPeer("1").name, "Joiner1")
    -- the joiners see each other (each tells everyone whom it is established with: 2 = the host and the other)
    check("Joiner1 tells it is established with two", WaitFor(function() return established["1"] == 2 end, 60), true)
    check("Joiner2 too", WaitFor(function() return established["2"] == 2 end, 60), true)
    WaitFor(function() return false end, 1)
    lobby:EjectPeer("2", "TestKick")
    check("the ejected one is gone from the peers", lobby:GetPeer("2"), nil)
    check("the host hears Joiner1 leave", WaitFor(function()
        for _, n in gone do if n == 'Joiner1' then return true end end
        return false
    end, 60), true)
    check("no peers then", table.getn(lobby:GetPeers()), 0)
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
