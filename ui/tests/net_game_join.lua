-- A 2-player network game over loopback, the joiner's part (see net_game_host.lua). It joins 127.0.0.1:16121 (again
-- with a new lobby while the host's program is still starting), says hello, and launches the game the host's 'Launch'
-- names, as FAF's lobby.lua does; in the game it gives its own orders, chats with the host, answers the host's change
-- of the game's speed with one of its own, sees the host's pause, changes its own checksum of beat 250 on purpose (the
-- desync dialog on both), runs past beat 300 and quits after beat 310. Variants (net_game_host.lua's header):
-- NET_GAME_EJECT: it stops answering after beat 150 (moho64ui_NetTestForTests("freeze")); NET_GAME_LOADING: before it
-- is ready. Both then wait a while and quit.
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
local EJECT, LOADING = rawget(_G, "NET_GAME_EJECT"), rawget(_G, "NET_GAME_LOADING")
ForkThread(function()
    local stop = moho64ui_WallSeconds() + 240
    while moho64ui_WallSeconds() < stop do WaitFrames(1) end
    LOG("TEST FAIL the run took over four minutes")
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
if LOADING then  -- (before the game's screen says READY: a player lost while the others load)
    moho64ui_NetTestForTests("freeze")
    ForkThread(function()
        LOG("TEST note: the joiner stops answering before it is ready")
        WaitFor(function() return false end, 40)
        LOG("TEST done: " .. failed .. " failed")
        ExitApplication()
    end)
    return
end
local heard, speeds, desyncs, sawPause = {}, {}, {}, false
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
if not EJECT then moho64ui_NetTestForTests("corrupt", 250) end
ForkThread(function()  -- (the host's pause, seen here)
    while not sawPause do
        if SessionIsPaused() then sawPause = true end
        WaitFrames(1)
    end
end)

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
    if EJECT then
        WaitFor(function() return GameTick() >= 150 end, 60)
        LOG("TEST note: the joiner stops answering at tick " .. GameTick())
        moho64ui_NetTestForTests("freeze")
        local frozeAt = GameTick()
        WaitFor(function() return false end, 30)
        check("frozen, its own game stands too", GameTick() <= frozeAt + 6, true)
        LOG("TEST done: " .. failed .. " failed")
        ExitApplication()
        return
    end
    -- chat: the host's two messages arrive from the host; ours to the host alone
    check("the host's message to us arrives, from the host", WaitFor(function() return Heard("Host: host to joiner") end, 60), true)
    check("and its message to everyone", WaitFor(function() return Heard("Host: host to all") end, 10), true)
    SessionSendChatMessage({ 1 }, { NetTest = "joiner to host" })
    -- the game's speed: the host's +3 reaches us, noted as client 1's; we answer with +1
    check("the host's speed reaches us", WaitFor(function() return GetGameSpeed() == 3 end, 30), true)
    check("noted as the host's (client 1 to +3)", WaitFor(function() return Noted(speeds, "1:3") end, 5), true)
    SetGameSpeed(1)
    check("ours is taken", WaitFor(function() return GetGameSpeed() == 1 end, 10), true)
    check("and noted as ours (client 2 to +1)", WaitFor(function() return Noted(speeds, "2:1") end, 5), true)
    check("beat 310 comes", WaitFor(function() return GameTick() >= 310 end, 90), true)
    check("the host's pause was seen here", sawPause, true)
    check("the desync dialog is shown", WaitFor(function() return table.getn(desyncs) > 0 end, 10), true)
    check("naming the host", desyncs[1] and desyncs[1].who, "Host")
    local cl = GetSessionClients()
    check("the host is still there", cl[1] and cl[1].connected, true)
    check("our own speed note: GetGameSpeed agrees", GetGameSpeed(), 1)
    LOG("TEST note: the joiner quits at tick " .. tostring(GameTick()))
    LOG("TEST done: " .. failed .. " failed")
    ExitApplication()
end)
