-- The two-PC network check (Chris approved, 2026-10-30): one program on each PC, in a window, plays a 2-player game
-- for two minutes over the original's UDP and says PASS or FAIL. One PC hosts, the other joins it by its address:
--   moho64ui ... --stage frontend --window --lua ui/tests/net_check.lua --game-lua ui/tests/net_check.lua
--            --log netcheck.log -- /netcheck host
--   moho64ui ... (the same) -- /netcheck join <the host PC's address>
-- (_scratch\ui\netcheck\Run-NetCheck.bat asks which and runs it.) In the game both commanders walk now and then and
-- build, both send a chat line, and the copies check each other as every network game does (a checksum every 50 beats:
-- a difference shows the desync dialog). At game minute 2 each writes
--   NETCHECK RESULT: PASS (...)   or   NETCHECK RESULT: FAIL: <why> (...)
-- to its log and shows it in a dialog; OK ends the program. PASS: both reached minute 2, no desync, the other PC still
-- connected, its chat line heard. Port 16121 (UDP): the host's firewall must let the program in.
local args = GetCommandLineArg("/netcheck", 2) or {}
local role, address = args[1] or "host", args[2]
local PORT = 16121
local END_TICK = 1200  -- (two minutes of game at normal speed)

local function WaitFor(cond, seconds)  -- (the PC's clock: the other PC runs at its own pace)
    local deadline = moho64ui_WallSeconds() + seconds
    while not cond() and moho64ui_WallSeconds() < deadline do WaitFrames(1) end
    return cond()
end
local label
local function Say(text)  -- (a line in the middle of the screen, the front end's or the game's)
    local UIUtil = import("/lua/ui/uiutil.lua")
    local LayoutHelpers = import("/lua/maui/layouthelpers.lua")
    if not label or IsDestroyed(label) then
        label = UIUtil.CreateText(GetFrame(0), "", 22, UIUtil.bodyFont)
        label:SetDropShadow(true)
        LayoutHelpers.AtHorizontalCenterIn(label, GetFrame(0))
        LayoutHelpers.AtTopIn(label, GetFrame(0), 120)
        label.Depth:Set(10000)
    end
    label:SetText(text)
    LOG("NETCHECK " .. text)
end
local function Finish(pass, why, detail)
    local line = "NETCHECK RESULT: " .. (pass and "PASS" or ("FAIL: " .. why)) .. " (" .. detail .. ")"
    LOG(line)
    local UIUtil = import("/lua/ui/uiutil.lua")
    UIUtil.QuickDialog(GetFrame(0), (pass and "Network check: PASS" or ("Network check: FAIL\n" .. why)) .. "\n\n" .. detail,
        "OK", function() ExitApplication() end)
    ForkThread(function()  -- (no one at this PC: the program ends by itself after a minute)
        WaitFor(function() return false end, 60)
        ExitApplication()
    end)
end

local inGame = GetArmiesTable and GetArmiesTable()
if not inGame then
    local LobbyComm = import("/lua/ui/lobby/lobbyComm.lua")
    if role == "host" then
        local function Player(name, owner, spot)
            return { PlayerName = name, OwnerID = owner, Human = true, Civilian = false, Faction = 1, Team = spot,
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
        local lobby = LobbyComm.CreateLobbyComm("UDP", PORT, "Host PC", nil, nil)
        if not lobby then
            Say("Cannot open UDP port " .. PORT .. ": is another program using it?")
            Finish(false, "the port", "port " .. PORT)
            return
        end
        local joiner
        lobby.DataReceived = function(self, data)
            if data.Type == 'Hello' then joiner = data.SenderID end
        end
        lobby.GameLaunched = function(self) self:Destroy() end
        lobby.LaunchFailed = function(self, why) Say("The launch failed: " .. tostring(why)) end
        lobby:HostGame()
        ForkThread(function()
            Say("Hosting on UDP port " .. PORT .. ". Waiting for the other PC to join (up to 10 minutes)")
            if not WaitFor(function() return joiner ~= nil end, 600) then
                Finish(false, "nobody joined in 10 minutes", "check the address typed on the other PC, and this PC's firewall")
                return
            end
            Say("The other PC joined: starting the game")
            config.PlayerOptions[1] = Player("Host PC", lobby:GetLocalPlayerID(), 1)
            config.PlayerOptions[2] = Player("Joining PC", joiner, 2)
            WaitFor(function() return false end, 1)
            lobby:BroadcastData({ Type = 'Launch', GameInfo = config })  -- (FAF's lobby.lua: the host tells, then launches)
            lobby:LaunchGame(config)
        end)
    else
        local lobby, welcome, launched = nil, nil, false
        local function NewLobby()
            lobby = LobbyComm.CreateLobbyComm("UDP", 0, "Joining PC", nil, nil)
            lobby.ConnectionToHostEstablished = function(self, localId, localName, hostId) welcome = localId end
            lobby.ConnectionFailed = function(self, reason) end
            lobby.DataReceived = function(self, data)
                if data.Type == 'Launch' then
                    launched = true
                    self:LaunchGame(data.GameInfo)
                end
            end
            lobby.GameLaunched = function(self) self:Destroy() end
            lobby.LaunchFailed = function(self, why) Say("The launch failed: " .. tostring(why)) end
        end
        ForkThread(function()
            if not address or address == "" then
                Finish(false, "no address given", "run it again and type the host PC's address")
                return
            end
            Say("Joining " .. address .. " on UDP port " .. PORT .. " (trying for up to 10 minutes)")
            local deadline = moho64ui_WallSeconds() + 600
            repeat  -- (until the host listens)
                if lobby then lobby:Destroy() end
                NewLobby()
                lobby:JoinGame(address .. ":" .. PORT, "Host PC", "0")
                WaitFor(function() return welcome ~= nil end, 15)
            until welcome or moho64ui_WallSeconds() > deadline
            if not welcome then
                Finish(false, "the host did not answer", "address " .. address .. ": is the other PC hosting? its firewall?")
                return
            end
            lobby:SendData("0", { Type = 'Hello' })
            Say("Joined: waiting for the host to start the game")
            if not WaitFor(function() return launched end, 120) then
                Finish(false, "the host did not start the game", "address " .. address)
            end
        end)
    end
    return
end

-- the game: what the engine hands the scripts is noted (the other PC's chat line, the desync dialog)
local me = GetFocusArmy()
local other = me == 1 and 2 or 1
local heard, desyncs = false, 0
local gamemain = import("/lua/ui/game/gamemain.lua")
local receiveChat = gamemain.ReceiveChat
gamemain.ReceiveChat = function(sender, data)
    if type(data) == "table" and data.NetCheck then heard = true end
    return receiveChat(sender, data)
end
local uimain = import("/lua/ui/uimain.lua")
local showDesync = uimain.ShowDesyncDialog
uimain.ShowDesyncDialog = function(beat, strings)
    desyncs = desyncs + 1
    LOG("NETCHECK desync at beat " .. tostring(beat))
    return showDesync(beat, strings)
end

ForkThread(function()
    Say("The game runs: two minutes of game, then the result")
    WaitFor(function() return GameTick() >= 60 end, 120)
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local cdr = GetArmyAvatars()[me]
    local function Click(button, pos)
        local at = view:Project(pos)
        moho64ui_MouseMove(at.x, at.y)
        WaitFrames(2)
        moho64ui_MouseButton(button, true, at.x, at.y)
        moho64ui_MouseButton(button, false, at.x, at.y)
    end
    if cdr then
        local cam = GetCamera("WorldCamera")
        local s = cam:SaveSettings()
        s.Focus = cdr:GetPosition()
        s.Zoom = 120
        cam:RestoreSettings(s)
        WaitSeconds(1)
        -- a power generator beside the commander, then a walk every 20 seconds (orders through the network)
        local c = cdr:GetPosition()
        SelectUnits({cdr})
        import("/lua/ui/game/commandmode.lua").StartCommandMode("build", {name = "ueb1101"})
        WaitSeconds(0.3)
        Click("Left", {c[1] + 8, c[2], c[3] + 8})
    end
    SessionSendChatMessage(other, { NetCheck = true, text = "hello from the " .. (role == "host" and "host" or "joining") .. " PC" })
    local step = 0
    while GameTick() < END_TICK do
        local next = math.min(GameTick() + 200, END_TICK)  -- (every 20 s of game, and exactly at minute 2)
        WaitFor(function() return GameTick() >= next end, 120)
        if cdr and not cdr:IsDead() and GameTick() < END_TICK then
            step = step + 1
            local c = cdr:GetPosition()
            local d = (math.mod(step, 2) == 1) and 15 or -15
            SelectUnits({cdr})
            Click("Right", {c[1] + d, c[2], c[3] + d})
            SelectUnits(nil)
        end
        local clients = GetSessionClients()
        if clients[other] and not clients[other].connected then break end
    end
    local clients = GetSessionClients()
    local peer = clients[other]
    local connected = peer ~= nil and peer.connected
    local detail = "game tick " .. GameTick() .. ", ping " .. tostring(peer and math.floor(peer.ping or 0)) .. " ms, " ..
        desyncs .. " desyncs, chat " .. (heard and "heard" or "not heard")
    if not connected then
        Finish(false, "the other PC left or was ejected", detail)
    elseif desyncs > 0 then
        Finish(false, "the two games differed (a desync)", detail)
    elseif GameTick() < END_TICK then
        Finish(false, "the game did not reach minute 2", detail)
    elseif not heard then
        Finish(false, "the other PC's chat line never came", detail)
    else
        Finish(true, nil, detail)
    end
end)
