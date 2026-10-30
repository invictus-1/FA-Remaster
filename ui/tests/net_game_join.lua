-- A 2-player network game over loopback, the joiner's part (see net_game_host.lua). It joins 127.0.0.1:16121 (again
-- with a new lobby while the host's program is still starting), says hello, and launches the game the host's 'Launch'
-- names, as FAF's lobby.lua does; in the game it gives its own orders, runs past beat 300 and quits after beat 310.
--   moho64ui --init <init.lua> --prefs <a copy> --stage frontend --no-world --lua ui/tests/net_game_join.lua
--            --game-lua ui/tests/net_game_join.lua --replay-out <dir>/r --frames 2000000000 --log join.log
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
local function WaitFor(cond, seconds)  -- (the PC's clock: the other program runs at its own pace)
    local deadline = moho64ui_WallSeconds() + seconds
    while not cond() and moho64ui_WallSeconds() < deadline do WaitFrames(1) end
    return cond()
end
ForkThread(function()
    local stop = moho64ui_WallSeconds() + 180
    while moho64ui_WallSeconds() < stop do WaitFrames(1) end
    LOG("TEST FAIL the run took over three minutes")
    LOG("TEST done: hung")
    ExitApplication()
end)

local inGame = GetArmiesTable and GetArmiesTable()
if not inGame then
    local LobbyComm = import("/lua/ui/lobby/lobbyComm.lua")
    local lobby, welcome, failedWhy, launched = nil, nil, nil, false
    local function NewLobby()
        lobby = LobbyComm.CreateLobbyComm("TCP", 0, "Joiner", nil, nil)
        lobby.ConnectionToHostEstablished = function(self, localId, localName, hostId) welcome = localId end
        lobby.ConnectionFailed = function(self, reason) failedWhy = reason end
        lobby.DataReceived = function(self, data)
            if data.Type == 'Launch' then  -- (FAF's lobby.lua: each joiner launches with the host's table)
                launched = true
                self:LaunchGame(data.GameInfo)
            end
        end
        lobby.GameLaunched = function(self) LOG("TEST note: the game launched") self:Destroy() end
        lobby.LaunchFailed = function(self, why) check("the launch", why, nil) end
    end
    ForkThread(function()
        local deadline = moho64ui_WallSeconds() + 60
        repeat  -- (until the host listens)
            if lobby then lobby:Destroy() end
            failedWhy = nil
            NewLobby()
            lobby:JoinGame("127.0.0.1:16121", "Host", "0")
            WaitFor(function() return welcome ~= nil or failedWhy ~= nil end, 10)
            if not welcome then WaitFor(function() return false end, 0.5) end
        until welcome or moho64ui_WallSeconds() > deadline
        check("the host welcomes us", welcome, "1")
        if not welcome then
            LOG("TEST done: " .. failed .. " failed")
            ExitApplication()
            return
        end
        lobby:SendData("0", { Type = 'Hello' })
        check("the host launches", WaitFor(function() return launched end, 60), true)
    end)
    return
end

-- the game
ForkThread(function()
    check("a network game", SessionIsMultiplayer(), true)
    check("the joiner's army", GetFocusArmy(), 2)
    local clients = GetSessionClients()
    check("two clients", table.getn(clients), 2)
    check("the joiner is client 2", clients[2] and clients[2].name, "Joiner")
    check("and the one here", clients[2] and clients[2]['local'], true)
    check("the commander comes", WaitFor(function() return GameTick() >= 60 end, 60), true)
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local cdr = GetArmyAvatars()[1]
    local c = cdr:GetPosition()
    local cam = GetCamera("WorldCamera")  -- (over the commander, as replay_record.lua)
    local s = cam:SaveSettings()
    s.Focus = c
    s.Zoom = 120
    cam:RestoreSettings(s)
    WaitSeconds(1)
    SelectUnits({cdr})
    LOG("TEST note: selected " .. table.getn(GetSelectedUnits() or {}) .. " at tick " .. GameTick())
    local at = view:Project({c[1] + 20, c[2], c[3] - 25})
    moho64ui_MouseMove(at.x, at.y)
    WaitFrames(2)
    moho64ui_MouseButton("Right", true, at.x, at.y)
    moho64ui_MouseButton("Right", false, at.x, at.y)
    local moveTick = GameTick()
    WaitFor(function() return GameTick() >= moveTick + 60 end, 60)
    local p = cdr:GetPosition()
    check("the commander walks", math.abs(p[1] - c[1]) + math.abs(p[3] - c[3]) > 8, true)
    SelectUnits({cdr})
    import("/lua/ui/game/commandmode.lua").StartCommandMode("build", {name = "ueb1101"})
    local b = view:Project({p[1] - 8, p[2], p[3] - 8})
    moho64ui_MouseMove(b.x, b.y)
    WaitSeconds(0.3)
    moho64ui_MouseButton("Left", true, b.x, b.y)
    moho64ui_MouseButton("Left", false, b.x, b.y)
    check("the commander is told to build", WaitFor(function()
        for _, o in cdr:GetCommandQueue() or {} do
            if o.type == "BuildMobile" then return true end
        end
        return false
    end, 30), true)
    check("beat 310 comes", WaitFor(function() return GameTick() >= 310 end, 90), true)
    local cl = GetSessionClients()
    check("the host is still there", cl[1] and cl[1].connected, true)
    LOG("TEST note: the joiner quits at tick " .. tostring(GameTick()))
    LOG("TEST done: " .. failed .. " failed")
    ExitApplication()
end)
