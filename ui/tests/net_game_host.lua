-- A 2-player network game over loopback, the host's part (with net_game_join.lua: two programs at once, the suite's
-- net_game in net_lobby_run.sh). In the front end it hosts on TCP port 16121 through FAF's own lobbyComm; once the
-- joiner is there it launches a game of Seton's Clutch with the host's army (1) and the joiner's (2) as FAF's lobby.lua
-- does: {Type = 'Launch', GameInfo = ...} to everyone, then its own LaunchGame. In the game both give orders (the
-- commander walks and builds a power generator); then chat both ways (SessionSendChatMessage), the game's speed
-- changed by each (both must end on the same), a pause by the host that both see, and a checksum the joiner changes on
-- purpose at beat 250 (both must show the desync dialog); they run to beat 300; the joiner then quits: its connection
-- ends, and the host ejects it at once (the original's 202 / 203) and runs on without it. Both record the game
-- (--replay-out): their fingerprints (<prefix>.fp) must be equal at every beat both ran (the runner compares them).
-- Variants (a global set in front of this file by the runner, both programs alike):
--   NET_GAME_AI: an AI army on the host's side too.
--   NET_GAME_EJECT: after the orders the joiner stops answering (moho64ui_NetTestForTests("freeze")); the host sees it
--     go quiet, ejects it (EjectSessionClient, as the disconnect dialog's button) and its game runs on without it; the
--     runner then plays the host's replay (net_game_replay.lua) and compares its fingerprints with the host's.
--   NET_GAME_LOADING: the joiner stops answering before it is ready (a player lost while loading): the host's beats
--     wait (the waiting dialog), it ejects the joiner and its game starts without it.
--   NET_GAME_RUN_TO = <beat>: after the orders both run on to that beat and end there (no chat, speed, pause or desync):
--     a long plain game, for the beats' timing over a link with latency (MOHO64UI_REAL_TIME, MOHO64UI_NET_BEAT_LOG,
--     MOHO64_NET_DELAY_MS: docs/ui-net-game.md, "A link with latency").
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
local EJECT, LOADING = rawget(_G, "NET_GAME_EJECT"), rawget(_G, "NET_GAME_LOADING")
local RUN_TO = rawget(_G, "NET_GAME_RUN_TO")
-- (a hang ends the run after four minutes of the PC's clock)
ForkThread(function()
    local stop = moho64ui_WallSeconds() + 240 + (RUN_TO or 0) / 5
    while moho64ui_WallSeconds() < stop do WaitFrames(1) end
    LOG("TEST FAIL the run took over four minutes")
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
                        FogOfWar = "explored", GameSpeed = "adjustable", UnitCap = "1000", Share = "ShareUntilDeath",
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

-- the game: what the engine hands the scripts is noted (the chat, the speed's changes, the desync dialog)
local heard, speeds, desyncs, console = {}, {}, {}, {}
AddConsoleOutputReciever(function(text) table.insert(console, text) end)
local gamemain = import("/lua/ui/game/gamemain.lua")
local receiveChat = gamemain.ReceiveChat
gamemain.ReceiveChat = function(sender, data)
    if type(data) == "table" and data.NetTest then table.insert(heard, sender .. ": " .. data.NetTest) end
    return receiveChat(sender, data)
end
local uimain = import("/lua/ui/uimain.lua")
local noteSpeed, showDesync = uimain.NoteGameSpeedChanged, uimain.ShowDesyncDialog
uimain.NoteGameSpeedChanged = function(client, rate)
    table.insert(speeds, client .. ":" .. rate)
    return noteSpeed(client, rate)
end
uimain.ShowDesyncDialog = function(beat, strings)
    table.insert(desyncs, { beat = beat, who = strings[1], n = table.getn(strings) })
    return showDesync(beat, strings)
end
local function Heard(line)
    for _, l in heard do if l == line then return true end end
    return false
end
local function Noted(list, entry)
    for _, l in list do if l == entry then return true end end
    return false
end

ForkThread(function()
    check("a network game", SessionIsMultiplayer(), true)
    check("the host's army", GetFocusArmy(), 1)
    check("beat 1 waits for the others' READY (after the game's screen)", GameTick(), 0)
    local clients = GetSessionClients()
    check("two clients", table.getn(clients), 2)
    check("the host is client 1 (the first army's)", clients[1] and clients[1].name, "Host")
    check("and the one here", clients[1] and clients[1]['local'], true)
    check("the joiner client 2", clients[2] and clients[2].name, "Joiner")
    check("speaking for source 2", clients[2] and clients[2].authorizedCommandSources[1], 2)
    check("nobody ejected", clients[2] and table.getn(clients[2].ejectedBy), 0)
    check("FAF's maxSP field", clients[2] and clients[2].maxSP, 50)
    if LOADING then
        -- the joiner never says READY: no beat runs; it goes quiet; ejected, it counts as ready and the game starts
        WaitFor(function() return false end, 3)
        check("the beats wait for the joiner's READY", GameTick(), 0)
        check("the joiner goes quiet (the disconnect dialog's cue)", WaitFor(function()
            local cl = GetSessionClients()
            return cl[2] ~= nil and cl[2].connected and cl[2].quiet > 5000
        end, 60), true)
        EjectSessionClient(2)
        check("ejected: no longer connected here", WaitFor(function() return GetSessionClients()[2].connected == false end, 5), true)
        check("the game starts without it", WaitFor(function() return GameTick() >= 40 end, 60), true)
        LOG("TEST done: " .. failed .. " failed")
        ExitApplication()
        return
    end
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
    if RUN_TO then
        -- the game runs on to that beat (both), then ends here
        check("beat " .. RUN_TO .. " comes", WaitFor(function() return GameTick() >= RUN_TO end, RUN_TO / 10 * 3 + 60), true)
        check("the joiner is still there", GetSessionClients()[2].connected, true)
        LOG("TEST note: the host's game ends at tick " .. GameTick() .. "; ping to the other " .. tostring(GetSessionClients()[2].ping) .. " ms")
        LOG("TEST done: " .. failed .. " failed")
        WaitFor(function() return false end, 1)
        ExitApplication()
        return
    end
    if EJECT then
        -- the joiner stops answering after beat 150: our game waits for it, it goes quiet; we eject it and go on
        check("the joiner goes quiet (the disconnect dialog's cue)", WaitFor(function()
            local cl = GetSessionClients()
            return cl[2] ~= nil and cl[2].connected and cl[2].quiet > 5000
        end, 60), true)
        local stood = GameTick()
        WaitFor(function() return false end, 1)
        check("meanwhile the game waits (the tick stands)", GameTick(), stood)
        EjectSessionClient(2)
        -- (GetSessionClients is FAF's copy, /lua/ui/override/SessionClients.lua, refreshed every 0.1 s)
        check("ejected: no longer connected here", WaitFor(function() return GetSessionClients()[2].connected == false end, 5), true)
        local cl = GetSessionClients()
        check("ejected by the host (client 1)", cl[2] and cl[2].ejectedBy[1], 1)
        check("ejecting ourselves is refused", pcall(EjectSessionClient, 1), false)
        check("the game runs on without the joiner", WaitFor(function() return GameTick() >= stood + 60 end, 60), true)
        LOG("TEST note: the joiner was ejected at tick " .. stood .. "; the host's game ends at tick " .. GameTick())
        LOG("TEST done: " .. failed .. " failed")
        ExitApplication()
        return
    end
    -- chat: to the joiner alone (no echo here), to everyone (the echo here too); the joiner's to us
    WaitFor(function() return GameTick() >= 150 end, 60)
    SessionSendChatMessage(2, { NetTest = "host to joiner" })
    SessionSendChatMessage({ NetTest = "host to all" })
    check("our message to everyone comes back to us, from us", WaitFor(function() return Heard("Host: host to all") end, 10), true)
    check("the joiner's message to us arrives, from the joiner", WaitFor(function() return Heard("Joiner: joiner to host") end, 30), true)
    check("our message to the joiner alone does not come back", Heard("Host: host to joiner"), false)
    check("a client that is not there is refused", pcall(SessionSendChatMessage, 3, { NetTest = "x" }), false)
    -- the game's speed: ours, then the joiner's answer; both end on the same
    SetGameSpeed(3)
    check("the speed we asked for is ours at once", GetGameSpeed(), 3)
    check("and the change is noted: client 1 to +3", WaitFor(function() return Noted(speeds, "1:3") end, 5), true)
    check("the joiner then asks for +1: we end on it", WaitFor(function() return GetGameSpeed() == 1 end, 30), true)
    check("and the change is noted: client 2 to +1", WaitFor(function() return Noted(speeds, "2:1") end, 5), true)
    -- a pause both see (the stream's own message), then on again
    SessionRequestPause()
    check("the pause comes", WaitFor(function() return SessionIsPaused() end, 20), true)
    local paused = GameTick()
    WaitFor(function() return false end, 1)
    check("while paused the tick stands", GameTick(), paused)
    SessionResume()
    check("and goes on after the resume", WaitFor(function() return GameTick() > paused + 5 end, 30), true)
    -- the joiner changed its checksum of beat 250: the desync dialog shows here, naming the joiner
    check("beat 300 comes", WaitFor(function() return GameTick() >= 300 end, 90), true)
    check("the desync dialog is shown", WaitFor(function() return table.getn(desyncs) > 0 end, 10), true)
    check("naming the joiner", desyncs[1] and desyncs[1].who, "Joiner")
    check("at the beat compared or after it", desyncs[1] ~= nil and desyncs[1].beat >= 250, true)
    LOG("TEST note: desync dialog at beat " .. tostring(desyncs[1] and desyncs[1].beat))
    -- the joiner quits after beat 310: its connection ends, and it is ejected here at once (no dialog), the game runs on
    check("the joiner, gone, is ejected at once", WaitFor(function()
        local cl = GetSessionClients()
        return cl[2] ~= nil and cl[2].connected == false
    end, 60), true)
    local left = GameTick()
    local cl = GetSessionClients()
    check("ejected by the host (client 1)", cl[2] and cl[2].ejectedBy[1], 1)
    check("near where the joiner left", left >= 300 and left <= 330, true)
    check("the console says so", WaitFor(function()
        for _, l in console do if string.find(l, "Joiner") and string.find(l, "disconnected") then return true end end
        return false
    end, 5), true)
    check("and the game runs on without it", WaitFor(function() return GameTick() >= left + 40 end, 60), true)
    LOG("TEST note: the joiner left at tick " .. tostring(left) .. "; the host's game ends at tick " .. GameTick())
    LOG("TEST done: " .. failed .. " failed")
    ExitApplication()
end)
