-- Our AI's settings on a SECOND (and third) visit to the lobby (Chris's second test, 2026-11-04: of four AI slots set,
-- the launch kept two; lobby_aislots.lua, one visit, kept them all). Three lobbies and three games in one run:
--   visit 1: the skirmish lobby, our AIs in slots 2-5 (and FAF's own in slot 6), each set in its dialog; AFTER the
--            settings a colour, the teams, a faction and the map changed (his likely order of steps); launch;
--   game 1:  a few seconds, then back to the menu (ExitGame);
--   visit 2: the skirmish lobby again from the main menu (its Skirmish button's function): what the slots show; our AIs
--            again, mixed: settings changed, the same as before, left at the defaults, another of ours picked over a
--            set one, ours over FAF's, an AI moved to an empty slot, two swapped; then a colour, the teams, a faction
--            and the map; launch;
--   game 2:  as game 1;
--   visit 3: Chris's own way, as his lobby's log shows it: he moves to slot 8, "Fill Slots" with our AI, four set in
--            the dialog, our AIx over one, colours, "Fill Slots" again with the AIx, one set again; launch;
--   game 3:  "TEST done".
-- Each game logs the settings each of its armies must play by ("TEST expect game <g> army <i> ..."; the army's number
-- is its place among the occupied slots, src/sim/session.cpp); the runner matches them with the simulation's aiop lines
-- of that game (the log between the host's "the game is left: back to the front end" lines) and reads the games'
-- tables (<prefix>.session.lua, <prefix>.2.session.lua, <prefix>.3.session.lua).
--   moho64ui --init <init.lua> --prefs <a copy> --stage setup --size 1920x1080 --replay-out <dir>/r --no-world
--            --lua ui/tests/lobby_secondvisit.lua --lua-again --game-lua ui/tests/lobby_secondvisit.lua --frames 16000
--            --log out.log
-- (--lua-again: this file runs again in each front end that comes back after a game.) Every check logs "TEST ok ..."
-- or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local kKey = 'lobby_secondvisit_test'
local state = GetFrontEndData(kKey) or {}
local failed = state.failed or 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end

-- the four settings as the lobby keeps them, "personality aggression economy teacher"
local function settings(p)
    return p and (tostring(p.RemasterPersonality) .. " " .. tostring(p.RemasterAggression) .. " " ..
        tostring(p.RemasterEconomy) .. " " .. tostring(p.RemasterTeacher))
end

if rawget(_G, "SessionIsActive") and SessionIsActive() then
    -- a game: the settings each army must play by, then back to the menu (game 1) or the end (game 2)
    ForkThread(function()
        WaitFrames(5)
        local game = state.visit or 0
        check("game " .. game .. " comes from a visit to the lobby", game >= 1 and game <= 3, true)
        local armies = GetArmiesTable().armiesTable or {}
        local seen = 0
        for index, army in armies do
            local _, _, slot = string.find(tostring(army.name), "^ARMY_(%d+)$")
            local want = slot and state.expect and state.expect[tonumber(slot)]
            if want then
                seen = seen + 1
                local _, _, p, a, e, t = string.find(want, "^(%S+) (%S+) (%S+) (%S+)$")
                LOG(string.format("TEST expect game %d army %d personality %s teacher %s aggression %s economy %s (slot %s)",
                    game, index, p == 'random' and '*' or p, t == 'on' and 'true' or 'false', a, e, slot))
            end
        end
        local wanted = 0
        for _ in state.expect or {} do wanted = wanted + 1 end
        check("game " .. game .. ": an army for each of our AIs' slots", seen, wanted)
        WaitSeconds(3)
        check("game " .. game .. " runs", GetGameTimeSeconds() >= 2, true)
        if game < 3 then
            SetFrontEndData(kKey, {visit = game, failed = failed})
            LOG("TEST note: game " .. game .. " left for the menu (" .. failed .. " failed so far)")
            ExitGame()
        else
            SetFrontEndData(kKey, nil)
            LOG("TEST done: " .. failed .. " failed")
        end
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
-- a combo's entry by its text, or (a slot's name combo) by its key, through the mouse
local function pickRow(combo, row)
    clickOn(combo)
    if row then
        clickRow(combo._list, row)
        return true
    end
    clickOn(combo)
    return false
end
local function pick(combo, text)
    local list, row = combo._list, nil
    for r = 0, list:GetItemCount() - 1 do
        if list:GetItem(r) == text then row = r break end
    end
    return pickRow(combo, row)
end
local function pickKey(combo, key)
    local row
    for i, k in combo.slotKeys or {} do
        if k == key then row = i - 1 break end
    end
    return pickRow(combo, row)
end

local kPersonality = { random = "Random", balanced = "Balanced", rusher = "Rusher", turtle = "Turtle" }
local kAggression = { [-2] = "-2  Defensive", [-1] = "-1", [0] = "0  Normal", [1] = "+1", [2] = "+2  Aggressive" }
local kEconomy = { [-2] = "-2  Army first", [-1] = "-1", [0] = "0  Normal", [1] = "+1", [2] = "+2  Economy first" }

-- the lobby of this visit, its GUI and game info once up
local function lobbyUp(lobby)
    for i = 1, 600 do
        local GUI = upvalue(lobby.SetRuleTitleText, 'GUI')
        local gameInfo = upvalue(lobby.GetGameSettings, 'gameInfo')
        if type(GUI) == 'table' and GUI.uiCreated and GUI.slots and type(gameInfo) == 'table' and gameInfo.PlayerOptions then
            WaitFrames(30)
            local changelog = import('/lua/ui/lobby/changelog/changelogdialog.lua')
            if changelog.IsOpen() then changelog.CloseChangelogDialog() WaitFrames(3) end
            return GUI, gameInfo
        end
        WaitFrames(1)
    end
end

ForkThread(function()
    WaitFrames(3)
    local visit = (state.visit or 0) + 1
    local lobby = import('/lua/ui/lobby/lobby.lua')
    if visit == 1 then
        lobby.CreateLobby('None', 0, 'Tester', nil, nil, GetFrame(0), function() end)
        lobby.HostGame("Tester's Skirmish", '/maps/SCMP_009/SCMP_009_scenario.lua', true)
    else
        -- the main menu came back after game 1: its Skirmish button's function (the last map, LastScenario)
        local menu
        for i = 1, 600 do
            menu = import('/lua/ui/menus/main.lua')
            if menu.ButtonSkirmish then break end
            WaitFrames(1)
        end
        check("visit " .. visit .. ": the main menu is up", menu.ButtonSkirmish ~= nil, true)
        if not menu.ButtonSkirmish then
            LOG("TEST done: " .. failed .. " failed")
            return
        end
        WaitFrames(60)
        menu.ButtonSkirmish()
    end
    local GUI, gameInfo = lobbyUp(lobby)
    check("visit " .. visit .. ": the lobby is up", GUI ~= nil, true)
    if not GUI then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    local players, slots = gameInfo.PlayerOptions, GUI.slots

    -- each slot's AI and settings through its dialog: {AI's name or key, then the four or nil (left as they are)}
    local function setAI(s, ai)
        local ok
        if string.find(ai, ":") then ok = pick(slots[s].name, ai) else ok = pickKey(slots[s].name, ai) end
        check("visit " .. visit .. ": " .. ai .. " into slot " .. s, ok, true)
    end
    local function setSettings(s, p, a, e, t)
        check("visit " .. visit .. ", slot " .. s .. ": Remastered AI Settings... picked",
            pick(slots[s].name, "Remastered AI Settings..."), true)
        WaitFrames(3)
        local dialog = GUI.remasterAISettings
        check("visit " .. visit .. ", slot " .. s .. ": its dialog opens", dialog ~= nil and not IsDestroyed(dialog), true)
        if not dialog then return end
        pick(dialog.combos.RemasterPersonality, kPersonality[p])
        pick(dialog.combos.RemasterAggression, kAggression[a])
        pick(dialog.combos.RemasterEconomy, kEconomy[e])
        pick(dialog.combos.RemasterTeacher, t == 'on' and "On" or "Off")
        clickOn(dialog.closeButton)
        WaitFrames(3)
        check("visit " .. visit .. ", slot " .. s .. " set", settings(players[s]), p .. " " .. a .. " " .. e .. " " .. t)
    end
    local function checkAll(when, want)
        for s = 2, 8 do
            check("visit " .. visit .. ", " .. when .. ": slot " .. s, settings(players[s]) or "empty", want[s] or "empty")
        end
    end
    -- a colour not in use, for the slot's colour selector (the host's)
    local function freeColour()
        for i = 9, 16 do
            local used = false
            for s = 1, 16 do if players[s] and players[s].PlayerColor == i then used = true end end
            if not used then return i end
        end
    end
    -- after the settings: a colour, the teams (by the mouse, AutoTeams off first), a faction, then the map
    local function changesAfter(colourSlot, teams, factionSlot, faction, map)
        local free = freeColour()
        if free then
            slots[colourSlot].color:OnClick(free)
            WaitFrames(3)
            check("visit " .. visit .. ": slot " .. colourSlot .. "'s colour changed", players[colourSlot].PlayerColor, free)
        end
        if gameInfo.GameOptions.AutoTeams ~= 'none' then
            lobby.SetGameOption('AutoTeams', 'none')
            WaitFrames(3)
        end
        for s, team in teams do
            pick(slots[s].team, team)
            check("visit " .. visit .. ": slot " .. s .. "'s team changed", players[s].Team, team == ' 1' and 2 or 3)
        end
        slots[factionSlot].faction:OnClick(faction)
        WaitFrames(3)
        check("visit " .. visit .. ": slot " .. factionSlot .. "'s faction changed", players[factionSlot].Faction, faction)
        -- the map dialog's choice (lobby.lua selectBehavior: the bad-map flags cleared, then the options set)
        lobby.ClearBadMapFlags()
        lobby.SetGameOptions({ScenarioFile = map})
        WaitFrames(10)
        check("visit " .. visit .. ": the map changed", gameInfo.GameOptions.ScenarioFile, map)
    end

    local want
    if visit == 1 then
        setAI(2, "Remastered AI: Supreme Commander")
        setAI(3, "Remastered AIx: Supreme Commander")
        setAI(4, "Remastered AIx: Commander")
        setAI(5, "Remastered AI: Veteran")
        setAI(6, "easy")  -- (FAF's own "AI: Easy")
        setSettings(2, 'rusher', 2, -1, 'off')
        setSettings(3, 'turtle', -2, 2, 'on')
        setSettings(4, 'balanced', 1, 0, 'off')
        setSettings(5, 'rusher', -1, 1, 'on')
        want = { [2] = "rusher 2 -1 off", [3] = "turtle -2 2 on", [4] = "balanced 1 0 off", [5] = "rusher -1 1 on",
                 [6] = "random 0 0 off" }
        checkAll("after the settings", want)
        changesAfter(4, { [2] = ' 1', [3] = ' 2' }, 5, 2, '/maps/SCMP_011/SCMP_011_scenario.lua')
        checkAll("after colour, teams, faction and map", want)
        want[6] = nil  -- (FAF's AI plays by its own: no aiop line)
    elseif visit == 2 then
        -- what the slots show when the lobby comes again (FAF's skirmish starts empty: nothing of visit 1 in prefs)
        check("visit 2: the lobby opens on the last map (LastScenario)", gameInfo.GameOptions.ScenarioFile, '/maps/SCMP_011/SCMP_011_scenario.lua')
        check("visit 2: slot 1 is the player", players[1] ~= nil and players[1].Human, true)
        checkAll("at the start", {})
        setAI(2, "Remastered AIx: Supreme Commander")
        setAI(3, "Remastered AIx: Supreme Commander")
        setAI(4, "Remastered AIx: Commander")
        setAI(5, "Remastered AI: Veteran")
        setAI(6, "easy")
        setAI(7, "Remastered AI: Commander")
        checkAll("our AIs picked: the defaults", { [2] = "random 0 0 off", [3] = "random 0 0 off", [4] = "random 0 0 off",
            [5] = "random 0 0 off", [6] = "random 0 0 off", [7] = "random 0 0 off" })
        setSettings(2, 'turtle', 0, -2, 'on')      -- changed from visit 1 (rusher 2 -1 off)
        setSettings(3, 'turtle', -2, 2, 'on')      -- as in visit 1
        -- slot 4: left at the defaults (visit 1: balanced 1 0 off)
        setSettings(5, 'rusher', -1, 1, 'on')      -- as in visit 1, then another of ours over it keeps them
        setAI(5, "Remastered AIx: Recruit")
        check("visit 2: slot 5 has the AIx Recruit", players[5] and players[5].AIPersonality, "remastered_recruit_cheat")
        setAI(6, "Remastered AIx: Supreme Commander")  -- ours over FAF's
        setSettings(6, 'balanced', 2, 2, 'off')
        setSettings(7, 'rusher', 1, -1, 'off')
        want = { [2] = "turtle 0 -2 on", [3] = "turtle -2 2 on", [4] = "random 0 0 off", [5] = "rusher -1 1 on",
                 [6] = "balanced 2 2 off", [7] = "rusher 1 -1 off" }
        checkAll("after the settings", want)
        -- slot 7's AI moved to the empty slot 8, slots 3 and 4 swapped (the name list's "Move AI to slot")
        check("visit 2: slot 7's AI moved to slot 8", pickKey(slots[7].name, 'move_player_to_slot8'), true)
        check("visit 2: slot 3's AI swapped with slot 4's", pickKey(slots[3].name, 'move_player_to_slot4'), true)
        want = { [2] = "turtle 0 -2 on", [3] = "random 0 0 off", [4] = "turtle -2 2 on", [5] = "rusher -1 1 on",
                 [6] = "balanced 2 2 off", [8] = "rusher 1 -1 off" }
        checkAll("after the move and the swap", want)
        changesAfter(2, { [3] = ' 2', [5] = ' 1' }, 6, 3, '/maps/SCMP_028/SCMP_028_scenario.lua')
        checkAll("after colour, teams, faction and map", want)
    else
        -- Chris's own way (his lobby's log of 2026-11-05, _scratch/bigtest/game_prev.log, and his tables of 11-04): he
        -- moves to slot 8, fills slots 1-7 with "Fill Slots", sets four through the dialog, our AIx over one, colours,
        -- then all made AIx by "Fill Slots" again (FAF's: a second fill re-adds the AI in the slots the first filled,
        -- HostUtils.AddAI), one slot set again
        check("visit 3: the player moved to slot 8", pickKey(slots[8].name, 'occupy') and players[8] ~= nil and players[8].Human, true)
        local function fill(ai)
            check("visit 3: " .. ai .. " for the fill", pick(GUI.AIFillCombo, ai), true)
            clickOn(GUI.AIFillButton)
            WaitFrames(5)
        end
        fill("Remastered AI: Supreme Commander")
        for s = 1, 7 do
            check("visit 3: slot " .. s .. " filled", players[s] and players[s].AIPersonality, "remastered_supreme")
        end
        setSettings(1, 'turtle', 0, 0, 'off')
        setSettings(2, 'rusher', 2, 2, 'off')
        setSettings(3, 'turtle', 0, 2, 'off')
        setSettings(4, 'balanced', 2, 0, 'on')
        setAI(1, "Remastered AIx: Supreme Commander")
        local function colour(s)
            local free = freeColour()
            if free then
                slots[s].color:OnClick(free)
                WaitFrames(3)
                check("visit 3: slot " .. s .. "'s colour changed", players[s].PlayerColor, free)
            end
        end
        colour(1)
        colour(3)
        want = { [1] = "turtle 0 0 off", [2] = "rusher 2 2 off", [3] = "turtle 0 2 off", [4] = "balanced 2 0 on",
                 [5] = "random 0 0 off", [6] = "random 0 0 off", [7] = "random 0 0 off" }
        local function checkFrom1(when)
            for s = 1, 7 do check("visit 3, " .. when .. ": slot " .. s, settings(players[s]) or "empty", want[s]) end
        end
        checkFrom1("after the settings and colours")
        fill("Remastered AIx: Supreme Commander")
        for s = 1, 7 do
            check("visit 3: slot " .. s .. " filled again", players[s] and players[s].AIPersonality, "remastered_supreme_cheat")
        end
        checkFrom1("after the second fill")
        setSettings(1, 'turtle', 0, 2, 'off')
        want[1] = "turtle 0 2 off"
        checkFrom1("at the launch")
    end

    SetFrontEndData(kKey, {visit = visit, failed = failed, expect = want})
    clickOn(GUI.launchGameButton)
end)
