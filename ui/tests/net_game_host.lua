-- A 2-player network game over loopback, the host's part (with net_game_join.lua: two programs at once, the suite's
-- net_game in net_lobby_run.sh). In the front end it hosts on TCP port 16121 through FAF's own lobbyComm; once the
-- joiner is there it launches a game of Seton's Clutch with the host's army (1) and the joiner's (2) as FAF's lobby.lua
-- does: {Type = 'Launch', GameInfo = ...} to everyone, then its own LaunchGame. In the game both give orders (the
-- commander walks and builds a power generator) and run to beat 300; the joiner then quits, and the host's game must
-- wait (its tick stands) with the joiner reported lost (GetSessionClients). Both record the game (--replay-out): their
-- fingerprints (<prefix>.fp) must be equal at every beat both ran (the runner compares them).
--   moho64ui --init <init.lua> --prefs <a copy> --stage frontend --no-world --lua ui/tests/net_game_host.lua
--            --game-lua ui/tests/net_game_host.lua --replay-out <dir>/r --frames 2000000000 --log host.log
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
-- (a hang ends the run after three minutes of the PC's clock)
ForkThread(function()
    local stop = moho64ui_WallSeconds() + 180
    while moho64ui_WallSeconds() < stop do WaitFrames(1) end
    LOG("TEST FAIL the run took over three minutes")
    LOG("TEST done: hung")
    ExitApplication()
end)

local inGame = GetArmiesTable and GetArmiesTable()
if not inGame then
    local function Player(name, owner, spot)
        return { PlayerName = name, OwnerID = owner, Human = true, Civilian = false, Faction = 1, Team = 1,
                 StartSpot = spot, PlayerColor = spot, ArmyColor = spot, AIPersonality = "", Ready = true }
    end
    local config = {
        GameMods = {},
        GameOptions = { ScenarioFile = "/maps/scmp_009/scmp_009_scenario.lua", Victory = "sandbox", CheatsEnabled = "false",
                        FogOfWar = "explored", GameSpeed = "normal", UnitCap = "1000", Share = "ShareUntilDeath",
                        CivilianAlliance = "enemy", PrebuiltUnits = "Off", NoRushOption = "Off", Timeouts = "3",
                        TeamSpawn = "fixed", TeamLock = "locked", DisconnectionDelay02 = "90", AIReplacement = "Off" },
        Observers = {},
        PlayerOptions = {},
    }
    local lobby = import("/lua/ui/lobby/lobbyComm.lua").CreateLobbyComm("TCP", 16121, "Host", nil, nil)
    check("the lobby opens its port", lobby ~= nil, true)
    if not lobby then
        LOG("TEST done: " .. failed .. " failed")
        ExitApplication()
        return
    end
    local joiner
    lobby.DataReceived = function(self, data)
        if data.Type == 'Hello' then joiner = data.SenderID end
    end
    lobby.GameLaunched = function(self) LOG("TEST note: the game launched") self:Destroy() end
    lobby.LaunchFailed = function(self, why) check("the launch", why, nil) end
    lobby:HostGame()
    ForkThread(function()
        check("the joiner says hello", WaitFor(function() return joiner ~= nil end, 60), true)
        if not joiner then
            LOG("TEST done: " .. failed .. " failed")
            ExitApplication()
            return
        end
        config.PlayerOptions[1] = Player("Host", lobby:GetLocalPlayerID(), 1)
        config.PlayerOptions[2] = Player("Joiner", joiner, 2)
        if rawget(_G, "NET_GAME_AI") then  -- (an AI on the host's side too: the runner's variant)
            config.PlayerOptions[3] = { PlayerName = "AI", OwnerID = lobby:GetLocalPlayerID(), Human = false, Civilian = false,
                                        Faction = 1, Team = 1, StartSpot = 3, PlayerColor = 3, ArmyColor = 3,
                                        AIPersonality = "medium", Ready = true }
        end
        WaitFor(function() return false end, 0.5)
        lobby:BroadcastData({ Type = 'Launch', GameInfo = config })  -- (FAF's lobby.lua: the host tells, then launches)
        lobby:LaunchGame(config)
    end)
    return
end

-- the game
ForkThread(function()
    check("a network game", SessionIsMultiplayer(), true)
    check("the host's army", GetFocusArmy(), 1)
    local clients = GetSessionClients()
    check("two clients", table.getn(clients), 2)
    check("the host is client 1 (the first army's)", clients[1] and clients[1].name, "Host")
    check("and the one here", clients[1] and clients[1]['local'], true)
    check("the joiner client 2", clients[2] and clients[2].name, "Joiner")
    check("speaking for source 2", clients[2] and clients[2].authorizedCommandSources[1], 2)
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
    local at = view:Project({c[1] - 25, c[2], c[3] + 20})
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
    local b = view:Project({p[1] + 8, p[2], p[3] + 8})
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
    check("beat 300 comes", WaitFor(function() return GameTick() >= 300 end, 90), true)
    -- the joiner quits after beat 310: our game must then wait for it, with it lost
    check("the joiner is reported lost", WaitFor(function()
        local cl = GetSessionClients()
        return cl[2] ~= nil and cl[2].connected == false
    end, 60), true)
    local stood = GameTick()
    WaitFor(function() return false end, 4)
    check("the game waits (the tick stands)", GameTick(), stood)
    check("near where the joiner left", stood >= 300 and stood <= 330, true)
    LOG("TEST note: the host's game stands at tick " .. tostring(stood))
    LOG("TEST done: " .. failed .. " failed")
    ExitApplication()
end)
