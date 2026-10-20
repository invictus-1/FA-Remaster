local framework = import('/testing/framework.lua')
local driver = framework.input()
-- The skirmish lobby's slots and its map, driven as a player drives them: the game's own lobby (FAF's
-- /lua/ui/lobby/lobby.lua) opened as the main menu's Skirmish opens it, then
--   the slots: an AI put into a slot from the slot's list, its faction, colour and team from the row's
--   lists, an AI removed again ("Kick Player") and moved ("Move AI to slot") from its list, the AI fill
--   panel (Fill Slots, Clear Slots, the teams count), the buttons beside it (close the empty slots,
--   random map, auto teams, autobalance), the map's markers (a right click closes a slot, a left click
--   moves the player) and an AI of random faction;
--   the map: the map dialog (the Options button), its filters, its Random Map, a map chosen by a click
--   in its list and OK, Cancel, then a map with options of its own (ui/tests/maps/lobby_test, mounted
--   with --maps) found by its name and one of its options set;
-- and Launch Game: the launched game has those armies, factions, colours and teams, that map and its
-- options. The same file serves both states:
--   moho64ui_test --init <init.lua> --prefs <a copy> --stage setup --size 1920x1080 --maps ui/tests/maps
--            --lua ui/tests/lobby_slots.lua --game-lua ui/tests/lobby_slots.lua --frames 900 --no-world --log out.log
-- (It writes the preferences: give it a copy. Run it from the repository's top, for --maps.)
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local test = framework.suite('lobby_slots')
local check = function(what, got, want) return test:equal(what, got, want) end
local TEST_MAP = '/maps/lobby_test/lobby_test_scenario.lua'

if rawget(_G, "SessionIsActive") and SessionIsActive() then
    -- the launched game: what the lobby set up (army 1 the player, 2 the AI: Normal, 3 the AI: Easy)
    test:fork(function()
        WaitFrames(5)
        local wanted = GetFrontEndData('lobby_slots_test') or {}
        local armies = GetArmiesTable()
        local t = armies.armiesTable
        local players = 0
        for i, a in t do if not a.civilian then players = players + 1 end end
        check("Launch started the game, the player's army first", tostring(SessionIsActive()) .. " " .. tostring(armies.focusArmy), "true 1")
        check("with the three players of the lobby", players, 3)
        check("the player: human, UEF, as in the lobby", tostring(t[1].human) .. " " .. t[1].faction .. " " .. t[1].nickname, "true 0 Tester")
        check("army 2 is the AI: Normal", not t[2].human and string.find(t[2].nickname, "(AI: Normal)", 1, true) ~= nil, true)
        check("of the faction chosen in its row: Aeon", t[2].faction, 1)
        check("in the colour chosen in its row", string.lower(t[2].color), string.lower(tostring(wanted.color)))
        check("army 3 is the AI: Easy", not t[3].human and string.find(t[3].nickname, "(AI: Easy)", 1, true) ~= nil, true)
        check("its random faction became one of the four", t[3].faction >= 0 and t[3].faction <= 3, true)
        LOG("TEST note: the random faction drawn: " .. tostring(t[3].faction))
        check("the two AIs of team 2 are allies", IsAlly(2, 3), true)
        check("and the player, without a team, the enemy of both", tostring(IsEnemy(1, 2)) .. " " .. tostring(IsEnemy(1, 3)), "true true")
        local scenario = SessionGetScenarioInfo()
        check("on the map chosen in the map dialog", scenario.name .. " " .. string.lower(scenario.Options.ScenarioFile), "Lobby Test Map " .. TEST_MAP)
        check("with the map's option set in the dialog", scenario.Options.lobby_test_drops, 3)
        check("and its other option at the map's default (the lobby adds it at the launch)", scenario.Options.lobby_test_waves, '2')
        check("the lobby's own options come along", scenario.Options.UnitCap .. " " .. scenario.Options.Victory, "1000 demoralization")
        SetFrontEndData('lobby_slots_test', nil)
        test:finish()
    end)
    return
end

-- the lobby keeps its controls and settings in locals of its module: read through a function using them
local function upvalue(fn, name)
    for i = 1, 200 do
        local n, v = debug.getupvalue(fn, i)
        if not n then break end
        if n == name then return v end
    end
end
local function click(x, y, button)
    button = button or "Left"
    driver:move(x, y)
    WaitFrames(3)
    driver:button(button, true, x, y)
    WaitFrames(2)
    driver:button(button, false, x, y)
    WaitFrames(3)
end
local function centre(c)
    return math.floor((c.Left() + c.Right()) / 2), math.floor((c.Top() + c.Bottom()) / 2)
end
local function clickOn(c, button)
    local x, y = centre(c)
    click(x, y, button)
end
-- the first shown row of an item list (the engine's own: the host's moho64ui_Describe gives it)
local function topRow(list)
    local _, _, top = string.find(moho64ui_Describe(list), "top (%d+)")
    return tonumber(top) or 0
end
-- a row of an item list clicked where it is shown, scrolled into sight with the wheel first
local function clickRow(list, row)
    local h = list:GetRowHeight()
    local lines = math.max(1, math.floor(list.Height() / h))
    for i = 1, list:GetItemCount() do
        local top = topRow(list)
        if row < top then driver:scroll(list.Left() + 20, list.Top() + 2, 1)
        elseif row >= top + lines then driver:scroll(list.Left() + 20, list.Top() + 2, -1)
        else break end
    end
    click(list.Left() + 20, list.Top() + (row - topRow(list)) * h + math.floor(h / 2))
end
-- a drop-down list (FAF's Combo): a click opens it, a click on the row with this text picks it
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
-- a list of pictures (FAF's BitmapCombo): a click opens it; the shown elements, in order, with what
-- each shows; a click on the one `test` takes
local function shownElements(combo)
    local out = {}
    for _, e in combo._list do
        local x, y = centre(e)
        local c = moho64ui_ControlAt(x, y)
        table.insert(out, {x = x, y = y, shows = c and moho64ui_Describe(c) or ""})
    end
    table.sort(out, function(a, b) return a.y < b.y end)
    return out
end
local function pickPicture(combo, test)
    clickOn(combo)
    for _, e in shownElements(combo) do
        if test(e.shows) then
            click(e.x, e.y)
            return e.shows
        end
    end
    clickOn(combo)
end
local function shows(control)
    return control and moho64ui_Describe(control) or "nil"
end

test:fork(function()
    WaitFrames(3)
    local Prefs = import('/lua/user/prefs.lua')
    Prefs.SetToCurrentProfile('stored_filter_state', nil)  -- (the map dialog's filters as a new profile has them)
    Prefs.SetToCurrentProfile('LobbyOpt_AutoTeams', nil)
    local gameColors = import('/lua/gamecolors.lua').GameColors
    local lobby = import('/lua/ui/lobby/lobby.lua')
    -- as the main menu's Skirmish (/lua/ui/menus/main.lua ButtonSkirmish): a lobby hosted in single-player form
    lobby.CreateLobby('None', 0, 'Tester', nil, nil, GetFrame(0), function() end)
    lobby.HostGame("Tester's Skirmish", '/maps/SCMP_009/SCMP_009_scenario.lua', true)
    WaitFrames(30)
    local GUI = upvalue(lobby.SetRuleTitleText, 'GUI')
    local gameInfo = upvalue(lobby.GetGameSettings, 'gameInfo')
    check("the lobby is up", type(GUI) == 'table' and type(gameInfo) == 'table' and GUI.slots ~= nil, true)
    local changelog = import('/lua/ui/lobby/changelog/changelogdialog.lua')
    if changelog.IsOpen() then changelog.CloseChangelogDialog() WaitFrames(3) end
    local players = gameInfo.PlayerOptions
    local slots = GUI.slots
    -- the map picture's markers of the start spots (made anew when the lobby redraws the map)
    local function marker(i) return GUI.mapView.startPositions[i] end
    local function occupied()
        local n = 0
        for i = 1, 16 do if players[i] then n = n + 1 end end
        return n
    end
    local function rowShown(i)  -- the row's faction, colour and team lists are shown
        return not slots[i].faction:IsHidden() and not slots[i].color:IsHidden() and not slots[i].team:IsHidden()
    end
    check("slot 2 is open: its row says so and shows no faction, colour nor team", slots[2].name._text:GetText() .. " " .. tostring(rowShown(2)), "Open false")

    ---------------------------------------------------------------------------------------------------
    -- an AI into slot 2 from the slot's list
    clickOn(slots[2].name)
    local list = slots[2].name._list
    check("a click on an open slot's name opens its list", slots[2].name._dropdown:IsHidden(), false)
    check("which offers Close, Occupy and the AIs", list:GetItem(0) .. ", " .. list:GetItem(1) .. ", " .. list:GetItem(3), "Close, Occupy, AI: Normal")
    clickOn(slots[2].name)
    check("AI: Normal picked from it", pick(slots[2].name, "AI: Normal"), true)
    local ai = players[2]
    check("puts an AI: Normal into slot 2", ai and (tostring(ai.Human) .. " " .. ai.AIPersonality .. " " .. ai.PlayerName), "false medium AI: Normal")
    check("the row shows its name and its faction, colour and team", slots[2].name._text:GetText() .. " " .. tostring(rowShown(2)), "AI: Normal true")
    check("an AI starts as a random faction", tostring(ai.Faction) .. " " .. tostring(string.find(shows(slots[2].faction._bitmap), "random_ico", 1, true) ~= nil), "5 true")
    check("in a free colour, which its marker on the map shows", string.lower(shows(marker(2).marker)), "bitmap colour " .. string.lower(gameColors.PlayerColors[ai.PlayerColor]))
    check("the dropdown closed", slots[2].name._dropdown:IsHidden(), true)

    -- its faction, colour and team from the row's lists
    local got = pickPicture(slots[2].faction, function(s) return string.find(s, "aeon_ico", 1, true) end)
    check("the faction list shows the factions' icons; Aeon picked", got ~= nil, true)
    check("the AI is Aeon", players[2].Faction, 2)
    check("and its row shows the Aeon icon", string.find(shows(slots[2].faction._bitmap), "aeon_ico", 1, true) ~= nil, true)
    clickOn(slots[2].color)
    local colours = shownElements(slots[2].color)
    clickOn(slots[2].color)
    local taken = string.lower(gameColors.PlayerColors[players[1].PlayerColor])
    local offersTaken = false
    for _, e in colours do offersTaken = offersTaken or string.lower(e.shows) == "bitmap colour " .. taken end
    check("the colour list leaves out the player's colour", table.getn(colours) .. " " .. tostring(offersTaken), (table.getn(gameColors.PlayerColors) - 1) .. " false")
    local fifth = colours[5] and string.lower(colours[5].shows)
    got = pickPicture(slots[2].color, function(s) return string.lower(s) == fifth end)
    local colour = string.lower(gameColors.PlayerColors[players[2].PlayerColor])
    check("the fifth colour picked is the AI's", got and string.lower(got), "bitmap colour " .. colour)
    check("its row and its marker show it", string.lower(shows(slots[2].color._bitmap)) .. " " .. string.lower(shows(marker(2).marker)), "bitmap colour " .. colour .. " bitmap colour " .. colour)
    check("team 2 picked from the team list", pick(slots[2].team, " 2"), true)
    check("puts it into team 2 (the lobby's 3)", tostring(players[2].Team) .. " '" .. slots[2].team._text:GetText() .. "'", "3 ' 2'")

    -- another AI into slot 3, then out of it again from its list
    pick(slots[3].name, "AI: Easy")
    check("an AI: Easy into slot 3", players[3] and players[3].AIPersonality, "easy")
    check("an AI's list begins with Kick Player and offers moves to the other slots", slots[3].name._list:GetItem(0) .. ", " .. slots[3].name._list:GetItem(1), "Kick Player, Move AI to slot 1")
    check("Kick Player picked from it", pick(slots[3].name, "Kick Player"), true)
    check("takes the AI out: the slot is empty", players[3], nil)
    check("its row says Open again, without faction, colour and team", slots[3].name._text:GetText() .. " " .. tostring(rowShown(3)), "Open false")
    check("and its marker is clear", string.lower(shows(marker(3).marker)), "bitmap colour 00777777")
    -- moved to another slot from its list
    pick(slots[3].name, "AI: Easy")
    pick(slots[3].name, "Move AI to slot 4")
    check("Move AI to slot 4: the AI is in slot 4, slot 3 empty", tostring(players[4] and players[4].AIPersonality) .. " " .. tostring(players[3]), "easy nil")
    check("the rows show it", slots[3].name._text:GetText() .. " / " .. slots[4].name._text:GetText(), "Open / AI: Easy")
    pick(slots[4].name, "Kick Player")
    check("three AI removed again: the player and the AI: Normal", occupied(), 2)

    ---------------------------------------------------------------------------------------------------
    -- the AI fill panel: the AI to fill with, Fill Slots, Clear Slots
    check("the host has the AI fill panel", GUI.AIFillCombo ~= nil and not GUI.AIFillButton:IsHidden(), true)
    pick(GUI.AIFillCombo, "AI: Easy")
    local spots = lobby.GetNumAvailStartSpots()
    check("Seton's Clutch has 8 start spots", spots, 8)
    clickOn(GUI.AIFillButton)
    local filled = 0
    for i = 3, 8 do if players[i] and players[i].AIPersonality == "easy" and rowShown(i) then filled = filled + 1 end end
    check("Fill Slots puts an AI: Easy into each open slot, shown in its row", filled, 6)
    check("the slots of the player and the AI stay as they were", players[1].PlayerName .. " " .. players[2].AIPersonality, "Tester medium")
    local free = true
    for i = 1, 8 do for j = i + 1, 8 do free = free and players[i].PlayerColor ~= players[j].PlayerColor end end
    check("every AI in a colour of its own", free, true)
    clickOn(GUI.AIClearButton)
    check("Clear Slots takes out the AIs it put in", occupied(), 2)
    check("their rows say Open", slots[5].name._text:GetText() .. " " .. tostring(rowShown(5)), "Open false")
    check("the AI put in by hand stays", players[2] and players[2].AIPersonality, "medium")
    -- the teams count beside them: two teams share the players in slot order
    pickPicture(GUI.TeamCountSelector, function(s) return string.find(s, "team_2_icon", 1, true) end)
    check("two teams from the teams count: the player team 1, the AI team 2", players[1].Team .. " " .. players[2].Team, "2 3")
    check("which the rows show", slots[1].team._text:GetText() .. "|" .. slots[2].team._text:GetText(), " 1| 2")

    ---------------------------------------------------------------------------------------------------
    -- close the empty slots, and open them again
    clickOn(GUI.closeEmptySlots)
    local closed = 0
    for i = 3, 8 do if gameInfo.ClosedSlots[i] and slots[i].name._text:GetText() == "Closed" then closed = closed + 1 end end
    check("the close slots button closes the six empty slots, their rows say Closed", closed, 6)
    check("and their markers carry the red X", marker(3).textOverlay:GetText(), "X")
    check("the occupied ones stay", tostring(gameInfo.ClosedSlots[1]) .. " " .. tostring(gameInfo.ClosedSlots[2]), "nil nil")
    clickOn(GUI.closeEmptySlots)
    local opened = 0
    for i = 3, 8 do if not gameInfo.ClosedSlots[i] and slots[i].name._text:GetText() == "Open" then opened = opened + 1 end end
    check("again, it opens them", opened, 6)
    -- a marker on the map: a right click closes its slot, another opens it; a left click moves the player there
    clickOn(marker(3).markerOverlay, "Right")
    check("a right click on slot 3's marker closes the slot", tostring(gameInfo.ClosedSlots[3]) .. " " .. slots[3].name._text:GetText(), "true Closed")
    clickOn(marker(3).markerOverlay, "Right")
    check("another opens it", tostring(gameInfo.ClosedSlots[3]) .. " " .. slots[3].name._text:GetText(), "false Open")
    clickOn(marker(5).markerOverlay)
    check("a click on slot 5's marker moves the player there", tostring(players[1]) .. " " .. tostring(players[5] and players[5].PlayerName), "nil Tester")
    clickOn(marker(1).markerOverlay)
    check("and on slot 1's back", players[1] and players[1].PlayerName, "Tester")

    -- random map: a map with room for the players, which the lobby's map picture shows. (With two
    -- players here: FAF's button keeps the maps with room in a table with gaps, table.filter, and draws
    -- from 1 to table.getn of it, which counts from 1 to the first gap, as the original's luaL_getn
    -- (FA exe 0x90e090): when the first map in name order has too few start spots the count is 0, the
    -- draw gives 1 and the script stops at the map that is not there, in the original as here.)
    check("the lobby draws with the interface's Random (FAF's math.random): 1..n, and 1 for Random(1, 0) as the original's (0x8bea00)", tostring(math.random == Random) .. " " .. Random(1, 0) .. " " .. tostring(Random(3) >= 1 and Random(3) <= 3), "true 1 true")
    check("Random takes at most two numbers", pcall(Random, 1, 2, 3), false)
    local before = gameInfo.GameOptions.ScenarioFile
    local maps = import('/lua/ui/dialogs/mapselect.lua').LoadScenarios()
    local tries, changed = 0, false
    while tries < 5 and not changed do  -- (a draw may give the same map)
        clickOn(GUI.randMap)
        tries = tries + 1
        changed = string.lower(gameInfo.GameOptions.ScenarioFile) ~= string.lower(before)
    end
    check("the random map button draws another map", changed, true)
    local drawn
    for _, s in maps do if string.lower(s.file) == string.lower(gameInfo.GameOptions.ScenarioFile) then drawn = s end end
    LOG("TEST note: the random map drawn: " .. tostring(drawn and drawn.name))
    check("one with room for the two players", drawn and table.getsize(drawn.Configurations.standard.teams[1].armies) >= 2, true)
    -- (the scenario's preview texture when it has one, else the picture in its map file)
    local source = drawn and (drawn.preview ~= '' and DiskGetFileInfo(drawn.preview) and drawn.preview or drawn.map) or "?"
    check("the lobby's map picture shows it", string.find(string.lower(shows(GUI.mapView.mapPreview)), "mappreview " .. string.lower(source) .. " ", 1, true), 1)

    ---------------------------------------------------------------------------------------------------
    -- the map dialog: the host's Options button
    local ms = import('/lua/ui/dialogs/mapselect.lua')
    clickOn(GUI.gameoptionsButton)
    WaitFrames(5)
    local dialog = upvalue(ms.CreateDialog, 'dialogContent')
    check("the Options button opens the map dialog", dialog ~= nil and not IsDestroyed(dialog), true)
    local mapList = dialog.mapSelectGroup.mapList
    local title = dialog.mapSelectGroup.title
    local all = mapList:GetItemCount()
    check("its list has the maps, its title says how many", all > 20 and title:GetText() == LOCF("<LOC lobui_0581>%d Maps Available", all), true)
    local byName = {}
    for _, s in maps do byName[LOC(s.name)] = s end  -- (as the list shows them)
    check("the lobby's map is chosen in it", mapList:GetItem(mapList:GetSelection()), drawn and LOC(drawn.name))
    -- the filters
    local filters = upvalue(ms.CreateDialog, 'filters')
    check("five filters: players, size, type, AI markers, obsolete", table.getn(filters), 5)
    local function listed(test)  -- every map of the list but the chosen one (always kept) passes `test`
        local selected = mapList:GetSelection()
        for r = 0, mapList:GetItemCount() - 1 do
            local s = byName[mapList:GetItem(r)]
            if r ~= selected and not (s and test(s)) then return false end
        end
        return true
    end
    local function armies(s) return table.getsize(s.Configurations.standard.teams[1].armies) end
    pick(filters[1].combo, "2")
    local n2 = mapList:GetItemCount()
    check("Supported Players 2: fewer maps, each for 2 players", n2 < all and n2 > 1 and listed(function(s) return armies(s) == 2 end), true)
    check("the title follows", title:GetText(), LOCF("<LOC lobui_0581>%d Maps Available", n2))
    pick(filters[1].comboFilter, ">=")
    check("the comparison >=: every map for at least 2", mapList:GetItemCount(), all)
    pick(filters[1].comboFilter, "<=")
    check("the comparison <=: the maps for at most 2", mapList:GetItemCount() >= n2 and listed(function(s) return armies(s) <= 2 end), true)
    pick(filters[1].combo, "All")
    pick(filters[1].comboFilter, "=")
    check("All again", mapList:GetItemCount(), all)
    pick(filters[2].combo, "5km")
    check("Map Size 5km: the 5 km maps", mapList:GetItemCount() < all and listed(function(s) return s.size[1] == 256 end), true)
    pick(filters[2].combo, "All")
    pick(filters[3].combo, "Official")
    check("Map Type Official: the original maps", mapList:GetItemCount() < all and listed(ms.CheckMapIsOfficial), true)
    pick(filters[3].combo, "All")
    check("all maps again", mapList:GetItemCount(), all)
    -- the dialog's own Random Map button chooses another map of the list
    local was = mapList:GetSelection()
    clickOn(dialog.randomMapButton)
    check("the dialog's Random Map button chooses another map", mapList:GetSelection() ~= was and mapList:GetSelection() >= 0, true)
    check("whose name the description shows", dialog.mapInfoGroup.description:GetItem(0), byName[mapList:GetItem(mapList:GetSelection())] and byName[mapList:GetItem(mapList:GetSelection())].name)
    -- a map chosen by a click in the list: its picture and description, then OK
    local row
    for r = 0, mapList:GetItemCount() - 1 do if mapList:GetItem(r) == "Seton's Clutch" then row = r end end
    clickRow(mapList, row)
    check("a click on Seton's Clutch in the list chooses it", mapList:GetSelection(), row)
    local picture = dialog.preview.mapPreview
    check("the dialog's picture shows it", shows(picture), "mappreview /maps/SCMP_009/SCMP_009.scmap 256x256")
    check("and its description", dialog.mapInfoGroup.description:GetItem(0), "Seton's Clutch")
    check("OK is on", dialog.selectButton:IsDisabled(), false)
    clickOn(dialog.selectButton)
    WaitFrames(3)
    check("OK closes the dialog", IsDestroyed(dialog), true)
    check("and makes it the lobby's map", string.lower(gameInfo.GameOptions.ScenarioFile), "/maps/scmp_009/scmp_009_scenario.lua")
    check("which the lobby's map picture shows", shows(GUI.mapView.mapPreview), "mappreview /maps/SCMP_009/SCMP_009.scmap 256x256")
    check("the slots stay", occupied(), 2)

    ---------------------------------------------------------------------------------------------------
    -- auto teams: the button goes through its states, each sets the teams by the map's start spots
    pick(slots[3].name, "AI: Easy")
    local states = {}
    local mapMid = GUI.mapView.Top() + GUI.mapView.Height() / 2
    local leftMid = GUI.mapView.Left() + GUI.mapView.Width() / 2
    local function teamsAre(rule)
        for i = 1, 3 do if players[i].Team ~= rule(i) then return false end end
        return true
    end
    for n = 1, 5 do
        clickOn(GUI.autoTeams)
        local state = gameInfo.GameOptions.AutoTeams
        table.insert(states, state)
        if state == 'tvsb' then
            check("top against bottom: each by its marker's place on the map", teamsAre(function(i) return marker(i).Top() < mapMid and 2 or 3 end), true)
            check("and the rows' team lists are off", slots[2].team:IsDisabled(), true)
        elseif state == 'lvsr' then
            check("left against right", teamsAre(function(i) return marker(i).Left() < leftMid and 2 or 3 end), true)
        elseif state == 'pvsi' then
            check("odd slots against even", teamsAre(function(i) return math.mod(i, 2) == 1 and 2 or 3 end), true)
        end
    end
    check("auto teams: top-bottom, left-right, odd-even, manual, none", table.concat(states, " "), "tvsb lvsr pvsi manual none")
    check("with none the team lists are on again", slots[2].team:IsDisabled(), false)
    -- autobalance: two teams of equal size by the ratings; of an odd number of players the last is
    -- left out without a team
    clickOn(GUI.PenguinAutoBalance)
    local pair = players[1].Team .. players[2].Team
    check("autobalance: the player and the AI: Normal in teams 1 and 2, the AI: Easy without", tostring(pair == "23" or pair == "32") .. " " .. players[3].Team, "true 1")
    -- the teams by hand for the game: the player without a team, the two AIs in team 2
    pick(slots[1].team, " - ")
    pick(slots[2].team, " 2")
    pick(slots[3].team, " 2")
    check("teams by hand", players[1].Team .. " " .. players[2].Team .. " " .. players[3].Team, "1 3 3")
    -- the AI: Easy keeps the random faction (the lobby draws one at the launch)
    pickPicture(slots[3].faction, function(s) return string.find(s, "random_ico", 1, true) end)
    check("the AI: Easy of random faction", players[3].Faction, 5)

    -- Cancel leaves the lobby's map as it is
    clickOn(GUI.gameoptionsButton)
    WaitFrames(5)
    dialog = upvalue(ms.CreateDialog, 'dialogContent')
    clickRow(dialog.mapSelectGroup.mapList, topRow(dialog.mapSelectGroup.mapList) + 1)  -- (the chosen map is the first shown)
    check("another map clicked in the dialog", dialog.mapInfoGroup.description:GetItem(0) ~= "Seton's Clutch", true)
    clickOn(dialog.cancelButton)
    WaitFrames(3)
    check("Cancel closes it, the lobby's map stays", tostring(IsDestroyed(dialog)) .. " " .. string.lower(gameInfo.GameOptions.ScenarioFile), "true /maps/scmp_009/scmp_009_scenario.lua")

    -- a map with options of its own: found by its name, chosen, one of its options set
    clickOn(GUI.gameoptionsButton)
    WaitFrames(5)
    dialog = upvalue(ms.CreateDialog, 'dialogContent')
    mapList = dialog.mapSelectGroup.mapList
    local nameField = upvalue(ms.PopulateMapList, 'nameFilter')
    clickOn(nameField)
    driver:type_keys("Lobby Test")
    check("typing into the name filter leaves the maps of that name (and the chosen one)", mapList:GetItemCount() .. " " .. tostring(mapList:GetItem(0)), "2 Lobby Test Map")
    clickRow(mapList, 0)
    check("a click chooses the test map", dialog.mapInfoGroup.description:GetItem(0), "Lobby Test Map")
    local container = upvalue(ms.RefreshOptions, 'OptionContainer')
    local options = upvalue(ms.RefreshOptions, 'Options')
    local lines = upvalue(container.CalcVisible, 'OptionDisplay')
    local last = options[table.getn(options)]
    check("the options panel ends with the map's own options under Advanced", LOC(options[table.getn(options) - 2].text) .. ", " .. LOC(options[table.getn(options) - 1].text) .. ", " .. LOC(last.text), LOC("<LOC lobui_0164>Advanced") .. ", Test: supply drops, Test: waves")
    local cx, cy = centre(container)
    for i = 1, table.getn(options) do driver:scroll(cx, cy, -1) end
    WaitFrames(2)
    local dropsLine
    for _, line in lines do if line.text:GetText() == "Test: supply drops" then dropsLine = line end end
    check("the wheel scrolls the panel to them", dropsLine ~= nil, true)
    check("each at its default", dropsLine and dropsLine.combo._text:GetText(), LOCF("<LOC gameui_0010>%s (default)", "none"))
    check("'many' picked for supply drops", dropsLine and pick(dropsLine.combo, "many"), true)
    clickOn(dialog.selectButton)
    WaitFrames(3)
    check("OK: the test map is the lobby's map", string.lower(gameInfo.GameOptions.ScenarioFile), TEST_MAP)
    check("with the option set", gameInfo.GameOptions.lobby_test_drops, 3)
    check("the lobby's map picture shows its map", shows(GUI.mapView.mapPreview), "mappreview /maps/SCMP_009/SCMP_009.scmap 256x256")
    local shown = false
    for _, o in upvalue(lobby.RefreshOptionDisplayData, 'formattedOptions') or {} do
        shown = shown or (o.text == "Test: supply drops" and o.value == "many")
    end
    check("the lobby's options list has it", shown, true)
    local ox, oy = centre(GUI.OptionContainer)
    for i = 1, 100 do driver:scroll(ox, oy, -1) end
    shown = false
    for _, line in GUI.OptionDisplay do shown = shown or (line.text:GetText() == "Test: supply drops" and line.value:GetText() == "many") end
    check("and shows it at the end, scrolled there with the wheel", shown, true)
    check("the slots stay", occupied(), 3)

    ---------------------------------------------------------------------------------------------------
    -- Launch Game: the game part of this file checks what the game got
    -- (the launched game ends the front end and this thread with it: the failures so far go along)
    SetFrontEndData('lobby_slots_test', {color = gameColors.PlayerColors[players[2].PlayerColor]})
    Prefs.SetToCurrentProfile('stored_filter_state', nil)
    driver:click(centre(GUI.launchGameButton))
    test:checkpoint()
end)
