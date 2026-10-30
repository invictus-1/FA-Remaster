-- Our AI's per-slot options in the skirmish lobby (ui/data/hook/lua/ui/lobby/lobby.lua, playerdata.lua): our AI put
-- into a slot from its list starts at the defaults; "Remastered AI Settings..." first in its list opens the dialog;
-- personality, aggression, economy and teacher mode picked there land in the slot's options and show after the AI's
-- name; another of our AIs for the slot keeps them; a game's own AI gets no entry, and our AI picked over it starts at
-- the defaults; the launch carries them (the suite's runner reads the launch's table, --replay-out).
--   moho64ui --init <init.lua> --prefs <a copy> --stage setup --size 1920x1080 --replay-out <dir>/r
--            --lua ui/tests/lobby_aioptions.lua --game-lua ui/tests/lobby_aioptions.lua --frames 900 --no-world --log out.log
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
        local wanted = GetFrontEndData('lobby_aioptions_test') or {}
        failed = wanted.failed or 0
        if failed > 0 then LOG("TEST note: " .. failed .. " failed in the front end") end
        local t = GetArmiesTable().armiesTable
        check("Launch started the game with the player and two AIs", tostring(t[1].human) .. " " .. tostring(t[2] and not t[2].human) ..
            " " .. tostring(t[3] and not t[3].human), "true true true")
        WaitSeconds(5)
        check("the game runs", GetGameTimeSeconds() >= 5, true)
        SetFrontEndData('lobby_aioptions_test', nil)
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

    check("the player's slot carries the four at their defaults", settings(players[1]), "random 0 0 off")
    check("our AI: Veteran picked into slot 2", pick(slots[2].name, "Remastered AI: Veteran"), true)
    check("is in slot 2", players[2] and players[2].AIPersonality, "remastered_veteran")
    check("at the defaults", settings(players[2]), "random 0 0 off")
    check("its row shows the name alone", slots[2].name._text:GetText(), "Remastered AI: Veteran")
    local list = slots[2].name._list
    check("its list: the settings first, then the game's entries", list:GetItem(0) .. ", " .. list:GetItem(1),
        "Remastered AI Settings..., Kick Player")

    check("Remastered AI Settings... picked", pick(slots[2].name, "Remastered AI Settings..."), true)
    WaitFrames(3)
    local dialog = GUI.remasterAISettings
    check("opens the dialog", dialog ~= nil and not IsDestroyed(dialog), true)
    if not dialog then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    local combos = dialog.combos
    check("which shows the defaults", combos.RemasterPersonality._text:GetText() .. " / " .. combos.RemasterAggression._text:GetText() ..
        " / " .. combos.RemasterEconomy._text:GetText() .. " / " .. combos.RemasterTeacher._text:GetText(), "Random / 0  Normal / 0  Normal / Off")
    check("Rusher picked", pick(combos.RemasterPersonality, "Rusher"), true)
    check("aggression +1 picked", pick(combos.RemasterAggression, "+1"), true)
    check("economy -2 picked", pick(combos.RemasterEconomy, "-2  Army first"), true)
    check("teacher mode on", pick(combos.RemasterTeacher, "On"), true)
    check("the slot's options have them", settings(players[2]), "rusher 1 -2 on")
    check("the dialog shows them", combos.RemasterPersonality._text:GetText() .. " / " .. combos.RemasterTeacher._text:GetText(), "Rusher / On")
    clickOn(dialog.closeButton)
    WaitFrames(3)
    check("Close closes it", tostring(IsDestroyed(dialog)) .. " " .. tostring(GUI.remasterAISettings), "true nil")
    local row = slots[2].name._text:GetText()
    LOG("TEST note: slot 2's row: " .. row)
    local forms = { ["Remastered AI: Veteran (Rusher, A+1, E-2, Teacher)"] = true, ["Remastered AI: Veteran (Rusher A+1 E-2 T)"] = true,
        ["Remastered AI: Veteran (...)"] = true }
    check("the row shows them after the name, as much as fits", forms[row] and slots[2].name._list:GetStringAdvance(row) <= slots[2].name.Width() - 24, true)

    check("our AI: Commander picked for the same slot", pick(slots[2].name, "Remastered AI: Commander"), true)
    check("keeps the slot's settings", players[2].AIPersonality .. " " .. settings(players[2]), "remastered_commander rusher 1 -2 on")
    check("its list still begins with the settings", slots[2].name._list:GetItem(0), "Remastered AI Settings...")

    check("the game's AI: Normal into slot 3", pick(slots[3].name, "AI: Normal"), true)
    check("its list has no settings", slots[3].name._list:GetItem(0), "Kick Player")
    check("its row the name alone", slots[3].name._text:GetText(), "AI: Normal")
    check("our AI: Recruit picked over it", pick(slots[3].name, "Remastered AI: Recruit"), true)
    check("starts at the defaults", players[3].AIPersonality .. " " .. settings(players[3]), "remastered_recruit random 0 0 off")
    pick(slots[3].name, "Remastered AI Settings...")
    WaitFrames(3)
    dialog = GUI.remasterAISettings
    check("its own dialog shows its own settings", dialog and dialog.combos.RemasterPersonality._text:GetText(), "Random")
    if dialog then
        pick(dialog.combos.RemasterPersonality, "Turtle")
        dialog:Close()
    end
    check("a change there leaves slot 2 as it was", settings(players[3]) .. " / " .. settings(players[2]), "turtle 0 0 off / rusher 1 -2 on")
    check("and slot 3's row shows it in words", slots[3].name._text:GetText(), "Remastered AI: Recruit (Turtle)")

    -- another player's view (the lobby made to answer "not the host" for a moment, the rows drawn again): the list
    -- has only the settings, and the dialog shows them without letting them change
    local lobbyComm = upvalue(upvalue(lobby.SetSlotInfo, 'RemasterGameSetSlotInfo') or lobby.SetSlotInfo, 'lobbyComm')
    local isHost = lobbyComm and lobbyComm.IsHost
    check("the lobby's connection found", isHost ~= nil, true)
    if isHost then
        lobbyComm.IsHost = function() return false end
        lobby.SetSlotInfo(2, players[2])
        check("a joiner's list of our AI: the settings alone", slots[2].name._list:GetItemCount() .. " " .. slots[2].name._list:GetItem(0),
            "1 Remastered AI Settings...")
        pick(slots[2].name, "Remastered AI Settings...")
        WaitFrames(3)
        dialog = GUI.remasterAISettings
        check("opens the dialog for a joiner too, showing the host's settings", dialog and dialog.combos.RemasterPersonality._text:GetText(), "Rusher")
        check("which a joiner cannot change", dialog and tostring(dialog.combos.RemasterPersonality:IsDisabled()) .. " " ..
            tostring(dialog.combos.RemasterTeacher:IsDisabled()), "true true")
        if dialog then dialog:Close() end
        lobbyComm.IsHost = nil
        if lobbyComm.IsHost ~= isHost then lobbyComm.IsHost = isHost end
        lobby.SetSlotInfo(2, players[2])
        check("the host's list again", slots[2].name._list:GetItem(1), "Kick Player")
    end

    SetFrontEndData('lobby_aioptions_test', {failed = failed})
    clickOn(GUI.launchGameButton)
end)
