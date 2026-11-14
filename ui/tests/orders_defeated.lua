-- A defeated army's units take orders in a game with cheats on (ours, Chris 2026-11-13: in his test his commander died
-- while he looked through another army; the 26 units he then spawned for his own army took no order: "that should be
-- allowed"). The original's simulation refuses every order for an army that is out of the game (Sim::OkayToMessWith
-- 0x747320), cheats or not. The engine's game option Options.RemasterOrdersAfterDefeat = "on" lifts that while cheats
-- are on (docs/engine-answers.md, batch 164); the interface stamps it into every launch whose options say
-- CheatsEnabled "true" (GameSession: StampOrdersAfterDefeat), so it is in the record and a replay plays as the game
-- went. A game without cheats is the original's in this (simorder.lua's lobby game checks that nothing is stamped).
-- One run, from the skirmish lobby into the game:
--   moho64ui --init <init.lua> --prefs <a copy> --stage setup --size 1920x1080 --no-world
--            --lua ui/tests/orders_defeated.lua --game-lua ui/tests/orders_defeated.lua --frames 6500 --log out.log
-- The lobby: cheats on, two AIs, no teams, Launch Game. The game: the option is in the scenario's options; the
-- player's commander is destroyed, the army is out of the game and the two AIs play on; three tanks are made for the
-- defeated army by the game's cheat callback; the army taken again (the console's SetFocusArmy, as a click on its name
-- in the score list does with cheats on); the tanks are sent off by a right click and go.
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

if rawget(_G, "SessionIsActive") and SessionIsActive() then
    ForkThread(function()
        WaitSeconds(8)
        failed = (GetFrontEndData('orders_defeated_lobby') or {}).failed or 0
        local options = SessionGetScenarioInfo().Options or {}
        check("the lobby's game has cheats on", options.CheatsEnabled, "true")
        check("so its launch says: a defeated army's units take orders", options.RemasterOrdersAfterDefeat, "on")
        local own = GetFocusArmy()
        local others = 0
        for index, army in GetArmiesTable().armiesTable do
            if index ~= own and not army.civilian then others = others + 1 end
        end
        check("two other armies play", others, 2)
        local cdr = GetArmyAvatars()[1]
        if not cdr then
            LOG("TEST FAIL no commander")
            LOG("TEST done: " .. (failed + 1) .. " failed")
            return
        end
        local p = cdr:GetPosition()
        SelectUnits({cdr})
        SimCallback({Func = 'ToggleSelfDestruct', Args = {owner = own, noDelay = true}}, true)
        local waited = 0
        while not GetArmiesTable().armiesTable[own].outOfGame and waited < 20 do
            WaitSeconds(0.5)
            waited = waited + 0.5
        end
        check("the player's army is out of the game", GetArmiesTable().armiesTable[own].outOfGame, true)
        WaitSeconds(4)
        check("the game goes on for the others", SessionIsGameOver(), false)

        -- three tanks for the defeated army, and the army taken again
        local spot = {p[1] + 12, p[2], p[3] + 12}
        SimCallback({Func = "CheatSpawnUnit", Args = {army = own, pos = spot, bpId = "uel0201", count = 3, yaw = 0}})
        WaitSeconds(2)
        ConExecute("SetFocusArmy " .. (own - 1))
        waited = 0
        while GetFocusArmy() ~= own and waited < 10 do
            WaitSeconds(0.5)
            waited = waited + 0.5
        end
        check("the defeated army is the focus army again (cheats on: the console's SetFocusArmy)", GetFocusArmy(), own)
        UISelectionByCategory("LAND", false, false, false, false)
        local tanks = {}
        for _, u in GetSelectedUnits() or {} do
            if u:GetBlueprint().BlueprintId == "uel0201" then table.insert(tanks, u) end
        end
        check("its three tanks are there and can be selected", table.getn(tanks), 3)
        if table.getn(tanks) ~= 3 then
            LOG("TEST done: " .. failed .. " failed")
            return
        end
        local function middle()
            local m = {0, 0, 0}
            for _, u in tanks do
                local q = u:GetPosition()
                for i = 1, 3 do m[i] = m[i] + q[i] / 3 end
            end
            return m
        end
        local from = middle()
        SelectUnits(tanks)
        local view = import("/lua/ui/game/worldview.lua").viewLeft
        GetCamera("WorldCamera"):SnapTo(spot, nil, 110)
        WaitSeconds(0.5)
        local at = view:Project({from[1] + 25, from[2], from[3]})
        moho64ui_MouseMove(at.x, at.y)
        WaitSeconds(0.2)
        moho64ui_MouseButton("Right", true, at.x, at.y)
        moho64ui_MouseButton("Right", false, at.x, at.y)
        WaitSeconds(1)
        local queue = tanks[1]:GetCommandQueue() or {}
        check("the simulation took the order: a move is in the first tank's queue", queue[1] and queue[1].type or "none", "Move")
        WaitSeconds(6)
        local to = middle()
        local went = math.sqrt((to[1] - from[1]) * (to[1] - from[1]) + (to[3] - from[3]) * (to[3] - from[3]))
        LOG(string.format("TEST note: the tanks went %.1f in seven seconds (from %.0f,%.0f)", went, from[1], from[3]))
        check("and the tanks go where they were sent", went > 5 and to[1] > from[1] + 4, true)
        SetFrontEndData('orders_defeated_lobby', nil)
        LOG("TEST done: " .. failed .. " failed")
    end)
    return
end

local function upvalue(fn, name)
    for i = 1, 200 do
        local n, v = debug.getupvalue(fn, i)
        if not n then break end
        if n == name then return v end
    end
end
local function click(x, y)
    moho64ui_MouseMove(x, y)
    WaitFrames(3)
    moho64ui_MouseButton("Left", true, x, y)
    WaitFrames(2)
    moho64ui_MouseButton("Left", false, x, y)
    WaitFrames(3)
end
local function clickOn(c)
    click(math.floor((c.Left() + c.Right()) / 2), math.floor((c.Top() + c.Bottom()) / 2))
end
local function topRow(list)
    local _, _, top = string.find(moho64ui_Describe(list), "top (%d+)")
    return tonumber(top) or 0
end
local function clickRow(list, row)
    local h = list:GetRowHeight()
    local lines = math.max(1, math.floor(list.Height() / h))
    for i = 1, list:GetItemCount() do
        local top = topRow(list)
        if row < top then moho64ui_MouseWheel(list.Left() + 20, list.Top() + 2, 1)
        elseif row >= top + lines then moho64ui_MouseWheel(list.Left() + 20, list.Top() + 2, -1)
        else break end
    end
    click(list.Left() + 20, list.Top() + (row - topRow(list)) * h + math.floor(h / 2))
end
local function pick(combo, text)
    clickOn(combo)
    local list = combo._list
    for r = 0, list:GetItemCount() - 1 do
        if list:GetItem(r) == text then
            clickRow(list, r)
            return true
        end
    end
    clickOn(combo)
    return false
end

ForkThread(function()
    WaitFrames(3)
    local lobby = import('/lua/ui/lobby/lobby.lua')
    lobby.CreateLobby('None', 0, 'Tester', nil, nil, GetFrame(0), function() end)
    lobby.HostGame("Tester's Skirmish", '/maps/SCMP_009/SCMP_009_scenario.lua', true)
    WaitFrames(30)
    local GUI = upvalue(lobby.SetRuleTitleText, 'GUI')
    local gameInfo = upvalue(lobby.GetGameSettings, 'gameInfo')
    check("the lobby is up", type(GUI) == 'table' and type(gameInfo) == 'table' and GUI.slots ~= nil, true)
    local changelog = import('/lua/ui/lobby/changelog/changelogdialog.lua')
    if changelog.IsOpen() then changelog.CloseChangelogDialog() WaitFrames(3) end
    check("the lobby itself has no row for it and sets nothing", gameInfo.GameOptions.RemasterOrdersAfterDefeat, nil)
    -- cheats on, every army for itself (so that the game goes on after the player's defeat)
    gameInfo.GameOptions.CheatsEnabled = 'true'
    gameInfo.GameOptions.AutoTeams = 'none'
    gameInfo.GameOptions.Victory = 'demoralization'
    check("an AI: Easy picked into slot 2", pick(GUI.slots[2].name, "AI: Easy"), true)
    check("and one into slot 3", pick(GUI.slots[3].name, "AI: Easy"), true)
    SetFrontEndData('orders_defeated_lobby', {failed = failed})
    clickOn(GUI.launchGameButton)
    WaitFrames(30)
    check("Launch Game starts the game", false, true)
    LOG("TEST done: " .. failed .. " failed (in the front end)")
end)
