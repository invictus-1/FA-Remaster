-- Our AI's settings across many slots, as Chris set them in his second test (2026-11-04: "full eco + turtle" on three
-- armies and "full aggro, +2 aggression and eco" on another; the launch kept two of four): the Remastered AIx in four
-- slots, each set through its own dialog in turn; a slot's colour changed; another of our AIs picked over one; each
-- slot keeps its own and no other's (ui/data/hook/lua/ui/lobby/lobby.lua). The launch's table is the runner's
-- (--replay-out: replay.session.lua's ArmySetup).
--   moho64ui --init <init.lua> --prefs <a copy> --stage setup --size 1920x1080 --replay-out <dir>/r
--            --lua ui/tests/lobby_aislots.lua --game-lua ui/tests/lobby_aislots.lua --frames 1500 --no-world --log out.log
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
        WaitFrames(5)
        local wanted = GetFrontEndData('lobby_aislots_test') or {}
        failed = wanted.failed or 0
        if failed > 0 then LOG("TEST note: " .. failed .. " failed in the front end") end
        WaitSeconds(3)
        check("the game runs", GetGameTimeSeconds() >= 2, true)
        SetFrontEndData('lobby_aislots_test', nil)
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
local function settings(p)
    return p and (tostring(p.RemasterPersonality) .. " " .. tostring(p.RemasterAggression) .. " " ..
        tostring(p.RemasterEconomy) .. " " .. tostring(p.RemasterTeacher))
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
    local players, slots = gameInfo.PlayerOptions, GUI.slots

    -- first the plain AI in slots 2-5 (as his game before), then the AIx picked over each
    for s = 2, 5 do
        check("Remastered AI: Supreme Commander into slot " .. s, pick(slots[s].name, "Remastered AI: Supreme Commander"), true)
    end
    for s = 2, 5 do
        check("then the AIx over it in slot " .. s, pick(slots[s].name, "Remastered AIx: Supreme Commander"), true)
        check("slot " .. s .. " has it", players[s] and players[s].AIPersonality, "remastered_supreme_cheat")
    end
    -- each slot through its own dialog: 2, 3, 4 "full eco + turtle", 5 "full aggro, +2 aggression and eco"
    local want = { [2] = "turtle 0 2 off", [3] = "turtle 0 2 off", [4] = "turtle 0 2 off", [5] = "rusher 2 2 off" }
    for s = 2, 5 do
        check("slot " .. s .. ": Remastered AI Settings... picked", pick(slots[s].name, "Remastered AI Settings..."), true)
        WaitFrames(3)
        local dialog = GUI.remasterAISettings
        check("slot " .. s .. ": its dialog opens", dialog ~= nil and not IsDestroyed(dialog), true)
        if dialog then
            if s == 5 then
                pick(dialog.combos.RemasterPersonality, "Rusher")
                pick(dialog.combos.RemasterAggression, "+2  Aggressive")
            else
                pick(dialog.combos.RemasterPersonality, "Turtle")
            end
            pick(dialog.combos.RemasterEconomy, "+2  Economy first")
            clickOn(dialog.closeButton)
            WaitFrames(3)
        end
        for t = 2, 5 do
            check("after slot " .. s .. ": slot " .. t, settings(players[t]), t <= s and want[t] or "random 0 0 off")
        end
    end
    -- a colour changed (the host's: its selector), then another of our AIs over slot 4
    local free
    for i = 9, 16 do
        local used = false
        for _, p in players do if p.PlayerColor == i then used = true end end
        if not used then free = i break end
    end
    if free and slots[3].color then
        slots[3].color:OnClick(free)
        WaitFrames(3)
        check("slot 3's colour changed", players[3].PlayerColor, free)
    end
    check("Remastered AIx: Commander over slot 4", pick(slots[4].name, "Remastered AIx: Commander"), true)
    for t = 2, 5 do
        check("at the end: slot " .. t, settings(players[t]), want[t])
    end

    SetFrontEndData('lobby_aislots_test', {failed = failed})
    clickOn(GUI.launchGameButton)
end)
