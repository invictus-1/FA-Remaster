local framework = import('/testing/framework.lua')
local driver = framework.input()
-- The lobby's smaller dialogs in single-player form, driven as a player drives them: the game's own
-- lobby (FAF's /lua/ui/lobby/lobby.lua) opened as the main menu's Skirmish opens it, then the Rules
-- text, the chat line, the lobby presets (save and load back), the Load button's dialog, and the CPU
-- benchmark with its bar in the player's row.
--   moho64ui_test --init <init.lua> --prefs <a copy> --stage setup --size 1920x1080 --lua ui/tests/lobby_small.lua
--            --frames 1100 --log out.log
-- (It writes the preferences: give it a copy. The profile's LobbyPresets, SaveGame and CPUBenchmark
-- change; a savegames/ folder is made beside the preferences. Run it from the repository's top:
-- it hands LoadSavedGame one of its own files.)
-- A headless run's clock goes in fixed frame steps and does not move while a frame's scripts run,
-- so the benchmark, which times its loops with GetSystemTimeSeconds inside one frame, measures 0
-- here: the checks take whatever it measured and look that the lobby shows and keeps it.
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local test = framework.suite('lobby_small')
local check = function(what, got, want) return test:equal(what, got, want) end
-- the lobby keeps its controls and settings in locals of its module: read through a function using them
local function upvalue(fn, name)
    for i = 1, 200 do
        local n, v = debug.getupvalue(fn, i)
        if not n then break end
        if n == name then return v end
    end
end
local function click(x, y)
    driver:move(x, y)
    WaitFrames(3)
    driver:button("Left", true, x, y)
    WaitFrames(2)
    driver:button("Left", false, x, y)
    WaitFrames(3)
end
local function centre(c)
    return math.floor((c.Left() + c.Right()) / 2), math.floor((c.Top() + c.Bottom()) / 2)
end
-- a control on screen (under the mouse, or one of its parents) for which `test` holds, in a region
local function findControl(test, x0, y0, x1, y1)
    for y = y0, y1, 8 do
        for x = x0, x1, 8 do
            local c = moho64ui_ControlAt(x, y)
            while c do
                if test(c) then return c end
                c = c:GetParent()
            end
        end
    end
end
local function findButton(text)
    return findControl(function(c) return c.label and c.label.GetText and c.label:GetText() == LOC(text) end, 400, 150, 1520, 950)
end
local function findList(firstItem)
    return findControl(function(c) return c.GetItemCount and c:GetItemCount() > 0 and c:GetItem(0) == firstItem end, 400, 150, 1520, 950)
end
-- a text shown (its first line, when a dialog wraps it: the lines keep a blank at their end)
local function findText(text)
    local want = LOC(text)
    return findControl(function(c)
        local got = c.GetText and not c.GetItemCount and c:GetText()
        if type(got) ~= 'string' then return false end
        got = string.gsub(got, "%s+$", "")
        return string.len(got) > 5 and string.sub(want, 1, string.len(got)) == got
    end, 400, 150, 1520, 950)
end

test:fork(function()
    WaitFrames(3)
    local Prefs = import('/lua/user/prefs.lua')
    local lobby = import('/lua/ui/lobby/lobby.lua')
    SetPreference('CPUBenchmark', nil)  -- (a machine without a score: the lobby measures one)
    Prefs.SetToCurrentProfile('LobbyPresets', nil)  -- (and a profile without presets)
    Prefs.SetToCurrentProfile('SaveGame', nil)  -- (nor a saved game loaded last: the Load dialog's field starts empty)
    check("no CPU score and no presets to begin with", tostring(GetPreference('CPUBenchmark')) .. " " .. tostring(Prefs.GetFromCurrentProfile('LobbyPresets')), "nil nil")

    -- as the main menu's Skirmish (/lua/ui/menus/main.lua ButtonSkirmish): a lobby hosted in single-player form
    local left = 0
    lobby.CreateLobby('None', 0, 'Tester', nil, nil, GetFrame(0), function() left = left + 1 end)
    lobby.HostGame("Tester's Skirmish", '/maps/SCMP_009/SCMP_009_scenario.lua', true)
    WaitFrames(30)
    local GUI = upvalue(lobby.SetRuleTitleText, 'GUI')
    local gameInfo = upvalue(lobby.GetGameSettings, 'gameInfo')
    check("the lobby is up", type(GUI) == 'table' and type(gameInfo) == 'table' and GUI.chatEdit ~= nil, true)
    -- FAF's changelog opens over the lobby for a profile that has not seen the newest one: closed first
    local changelog = import('/lua/ui/lobby/changelog/changelogdialog.lua')
    if changelog.IsOpen() then changelog.CloseChangelogDialog() WaitFrames(3) end

    -- the CPU column while the benchmark waits: the button counts down its ten seconds, disabled,
    -- and the player's row has a bar from 0 to 450, hidden until there is a score
    local bench = GUI.rerunBenchmark
    local bar = GUI.slots[1].CPUSpeedBar
    check("the benchmark button counts down, disabled", string.find(bench.label:GetText(), "^%d+s$") ~= nil and bench:IsDisabled(), true)
    check("the player's row has a CPU bar from 0 to 450", bar ~= nil and bar._rangeMin == 0 and bar._rangeMax == 450, true)
    check("hidden while there is no score", bar:IsHidden(), true)

    -- Rules: the host's label invites a click; a click opens "Game Rules", typing goes into it,
    -- Enter sets the game option and the label
    local rules = GUI.RuleLabel
    check("the host's rules label", rules:GetText(), LOC("<LOC lobui_0420>No Rules: Click to add rules"))
    check("the chat line has the keyboard focus", GUI.chatEdit:GetCurrentFocusControl() == GUI.chatEdit, true)
    click(rules.Left() + 20, rules.Top() + 5)  -- (on the text's row: the list's rows take the click)
    local box = GUI.chatEdit:GetCurrentFocusControl()
    check("a click on it opens the rules dialog, whose field takes the focus", box ~= nil and box ~= GUI.chatEdit and box.GetText ~= nil, true)
    driver:type_keys("No rush for 10 minutes")
    driver:press('Enter')
    WaitFrames(3)
    check("Enter sets the game's rules", gameInfo.GameOptions.GameRules, "No rush for 10 minutes")
    check("and the label shows them", rules:GetText(), "No rush for 10 minutes")
    check("the focus is back on the chat line, which got nothing", GUI.chatEdit:GetCurrentFocusControl() == GUI.chatEdit and GUI.chatEdit:GetText() == "", true)
    click(rules.Left() + 20, rules.Top() + 5)
    box = GUI.chatEdit:GetCurrentFocusControl()
    check("opened again, the dialog has the rules in its field", box ~= GUI.chatEdit and box:GetText(), "No rush for 10 minutes")
    local cancel = findButton("<LOC _Cancel>Cancel")
    check("it has a Cancel button", cancel ~= nil, true)
    if cancel then click(centre(cancel)) end
    check("Cancel closes it and keeps the rules", tostring(GUI.chatEdit:GetCurrentFocusControl() == GUI.chatEdit) .. " " .. tostring(gameInfo.GameOptions.GameRules), "true No rush for 10 minutes")

    -- the chat line: typing, Enter shows the line with the player's name, a command it does not
    -- know is answered, Up and Down bring back what was sent (the named Up and Down keys)
    driver:type_keys("hello there")
    check("typing goes into the chat line", GUI.chatEdit:GetText(), "hello there")
    driver:press('Enter')
    WaitFrames(2)
    check("Enter empties it", GUI.chatEdit:GetText(), "")
    local lines = GUI.chatDisplay.linesGroup.Lines
    check("the chat shows the line with the player's name", lines[1].author:GetText() .. "|" .. string.gsub(lines[1].message:GetText(), "%s+$", ""), "[Tester] |hello there")
    check("the line is kept in the chat's history", GUI.chatDisplay.ChatHistory[1] and GUI.chatDisplay.ChatHistory[1].messageText, "hello there")
    driver:type_keys("/nosuch")
    driver:press('Enter')
    WaitFrames(2)
    check("a command the lobby does not know is answered in the chat", string.gsub(lines[2].message:GetText(), "%s+$", ""), LOCF("<LOC lobui_0396>Command Not Known: %s", "nosuch"))
    driver:press('Up')  -- Up
    check("Up brings back the last line sent", GUI.chatEdit:GetText(), "/nosuch")
    driver:press('Up')
    check("Up again the one before", GUI.chatEdit:GetText(), "hello there")
    driver:press('Down')  -- Down
    check("Down goes forward again", GUI.chatEdit:GetText(), "/nosuch")
    driver:press('Down')
    check("and past the newest empties the line", GUI.chatEdit:GetText(), "")
    local cues = moho64ui_SoundInfo().played
    driver:type_keys(string.rep("x", 205))
    check("the chat line takes 200 characters", string.len(GUI.chatEdit:GetText()), 200)
    check("each one more plays the error sound", moho64ui_SoundInfo().played - cues, 5)
    GUI.chatEdit:SetText("")

    -- presets: in single-player form the Presets button is hidden and Load (saved games) is in its
    -- place (FAF's own: lobby.lua hides it when singlePlayer); the dialog itself, as the host of a
    -- multiplayer game opens it, saves the settings and loads them back
    check("single-player: no Presets button", GUI.restrictedUnitsOrPresetsBtn:IsHidden(), true)
    check("single-player: the Load button", GUI.loadButton:IsHidden(), false)
    import('/lua/ui/lobby/presets.lua').CreateUI(GUI)
    WaitFrames(3)
    local ok = findButton("Ok")
    check("without presets the dialog opens with its help", ok ~= nil, true)
    if ok then click(centre(ok)) end
    local create = findButton("<LOC _Create>Create")
    check("the presets dialog has Create", create ~= nil, true)
    if create then click(centre(create)) end
    driver:type_keys("Test preset")
    driver:press('Enter')
    WaitFrames(3)
    local saved = Prefs.GetFromCurrentProfile('LobbyPresets') or {}
    local mine = saved[1] or {}
    check("Create with a name saves a preset in the profile", table.getn(saved) .. " " .. tostring(mine.Name), "1 Test preset")
    check("with the map and the options", tostring(mine.MapName) .. " " .. tostring(mine.MapPath) .. " " .. tostring(mine.GameOptions and mine.GameOptions.GameRules), "Seton's Clutch " .. gameInfo.GameOptions.ScenarioFile .. " No rush for 10 minutes")
    local info = findList("Preset Name: Test preset")
    check("its details are shown, the map found", info and info:GetItem(1), "Map: Seton's Clutch")
    local load = findButton("<LOC _Load>Load")
    check("Load is enabled once a preset is chosen", load ~= nil and not load:IsDisabled(), true)
    local cap = gameInfo.GameOptions.UnitCap
    lobby.SetGameOption('UnitCap', '500')
    lobby.SetGameOption('GameRules', 'other rules')
    check("the settings changed", gameInfo.GameOptions.UnitCap .. " " .. gameInfo.GameOptions.GameRules, "500 other rules")
    if load then click(centre(load)) end
    WaitFrames(3)
    check("Load brings the preset's settings back", tostring(gameInfo.GameOptions.UnitCap) .. " " .. tostring(gameInfo.GameOptions.GameRules), tostring(cap) .. " No rush for 10 minutes")
    check("and the dialog goes", findButton("<LOC _Create>Create"), nil)

    -- the Load button: the saved games' dialog (/lua/ui/dialogs/saveload.lua), the profile's saves
    -- listed (FA exe 0x8ca380: <name>.SCFASaveGame in savegames/<profile>/; none here), its filename
    -- field with the focus. Delete with nothing chosen asks for a file (the click on Delete takes
    -- the focus from the field: CMauiEdit 0x7915a0); the field clicked, a name typed and OK: the
    -- file is not there, LoadSavedGame (0x880cc0) cannot open it and the dialog says so; Cancel
    -- closes it
    local files = GetSpecialFiles('SaveGame')
    check("saved games are .SCFASaveGame files in savegames/", files.extension .. " " .. string.sub(files.directory, -10), "SCFASaveGame savegames/")
    check("a campaign's saves too, .SCFACampaignSave", GetSpecialFiles('CampaignSave').extension .. " " .. string.sub(GetSpecialFiles('CampaignSave').directory, -10), "SCFACampaignSave savegames/")
    local worked, why = LoadSavedGame("ui/tests/lobby_small.lua")
    check("LoadSavedGame: a file that is not a saved game", tostring(worked) .. " " .. tostring(why), "false InvalidFormat")
    worked, why = LoadSavedGame("ui/tests/no such file.SCFASaveGame")
    check("LoadSavedGame: a file that is not there", tostring(worked) .. " " .. tostring(why), "false CantOpen")
    check("LoadSavedGame takes one name", pcall(LoadSavedGame), false)
    click(centre(GUI.loadButton))
    WaitFrames(3)
    cancel = findButton("<LOC _Cancel>")
    check("Load opens the saved games' dialog", cancel ~= nil, true)
    local field = GUI.chatEdit:GetCurrentFocusControl()
    check("its filename field has the focus", field ~= nil and field ~= GUI.chatEdit and field.GetText ~= nil, true)
    local delete = findButton("<LOC PROFILE_0006>Delete")
    if delete then click(centre(delete)) end
    WaitFrames(2)
    check("Delete with no file chosen asks for one", findText("<LOC file_0002>You must select a file to delete first.") ~= nil, true)
    local okInfo = findButton("<LOC _Ok>")
    if okInfo then click(centre(okInfo)) end
    check("its OK closes the question", findText("<LOC file_0002>You must select a file to delete first."), nil)
    check("the field lost the focus to the click on Delete", GUI.chatEdit:GetCurrentFocusControl(), nil)
    if field then click(centre(field)) end
    driver:type_keys("No such game")
    check("typing goes into the filename field", field and field:GetText(), "No such game")
    local okLoad = findButton("<LOC _Ok>")
    if okLoad then click(centre(okLoad)) end
    WaitFrames(3)
    local said = LOCF('<LOC uisaveload_0004>Couldn\'t open savegame "%s"', "No such game")
    check("OK: the dialog says the saved game cannot be opened", findText(said) ~= nil, true)
    okInfo = findButton("<LOC _Ok>")
    if okInfo then click(centre(okInfo)) end
    check("its OK closes the message, the Load dialog stays", tostring(findText(said) ~= nil) .. " " .. tostring(findButton("<LOC _Cancel>") ~= nil), "false true")
    cancel = findButton("<LOC _Cancel>")
    if cancel then click(centre(cancel)) end
    check("its Cancel closes it", findButton("<LOC _Cancel>"), nil)

    -- the benchmark: ten seconds after the lobby opened it runs (three times, the best kept), stores
    -- the score for the machine and shows it in the player's row
    for i = 1, 900 do
        if GetPreference('CPUBenchmark') ~= nil then break end
        WaitFrames(1)
    end
    WaitFrames(2)
    local score = GetPreference('CPUBenchmark')
    LOG("TEST note: the benchmark measured " .. tostring(score) .. " (headless: the clock does not move within a frame)")
    check("the benchmark stores a score from 0 to 450", type(score) == 'number' and score >= 0 and score <= 450, true)
    check("the button is enabled again, its label empty", tostring(bench:IsDisabled()) .. " '" .. bench.label:GetText() .. "'", "false ''")
    check("the player's row shows the score", tostring(bar:IsHidden()) .. " " .. tostring(bar._value()), "false " .. tostring(score))
    -- a stored score: what the lobby shows when it opens on a machine that has one (UpdateBenchmark(false))
    SetPreference('CPUBenchmark', 120)
    lobby.UpdateBenchmark(false)
    WaitFrames(1)
    local width = bar.Right() - bar.Left()
    check("a stored score of 120 is shown at once", tostring(bar:IsHidden()) .. " " .. bar._value(), "false 120")
    check("as a purple bar of 120/450 of the column", bar._bar.Right() - bar._bar.Left(), math.floor(120 / 450 * width))
    check("drawn with the purple bar's texture", string.find(moho64ui_Describe(bar._bar), "bar_purple_bmp", 1, true) ~= nil, true)
    -- the button runs it again at once
    click(centre(bench))
    WaitFrames(5)
    local again = GetPreference('CPUBenchmark')
    LOG("TEST note: run again from the button: " .. tostring(again))
    check("the button measures again and stores the new score", again ~= 120 and type(again) == 'number' and again >= 0 and again <= 450, true)
    check("which the row shows", tostring(bar._value()), tostring(again))
    check("the lobby is still open", left, 0)
    test:finish()
end)
