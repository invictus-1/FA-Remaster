-- "Report a Problem", the report by hand (ours; /moho64ui/lua/report.lua, the game's menu row of
-- ui/data/hook/lua/ui/game/tabs.lua, the main menu's line of ui/data/hook/lua/ui/menus/main.lua; Chris's debugger,
-- 2026-11-20). In a game: the row stands in every kind of the game's menu before the rows that leave the game; the row
-- of the menu as it is built opens the question; a line typed and Enter write the report's zip into the Desktop (in
-- tests: MOHO64UI_REPORT_FOLDER) with the note and the game's time of the asking, and a box says where; Ctrl-F1 asks
-- too; Escape writes nothing, and the game's own Escape still works after. In the front end: the main menu's line, a
-- click on it, a report with no game.
-- The lab's runner (suite/report.sh) reads the zips this test names in its log.
--   moho64ui --own ui/data --init <init.lua> --prefs <a copy> --stage setup --session <session_ai_cheats.lua>
--            --game-lua ui/tests/report_ask.lua --frames 900 --no-world
--   moho64ui --own ui/data --init <init.lua> --prefs <a copy> --stage frontend --no-world --lua ui/tests/report_ask.lua --frames 400
-- `-- /pic menu | ask | told` leaves the game's menu, the question or the box up at the end (for a --screenshot).
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local Report = import("/moho64ui/lua/report.lua")
local ESC, ENTER, F1 = 27, 13, 342
local pic = (GetCommandLineArg("/pic", 1) or {})[1] or "none"
local function isZip(file)
    return type(file) == 'string' and string.find(file, "remaster_report_", 1, true) ~= nil and string.sub(file, -4) == ".zip"
end
local function up(control) return control and not IsDestroyed(control) and true or false end

if rawget(_G, "SessionIsActive") and SessionIsActive() then
    ForkThread(function()
        WaitSeconds(3)
        local Tabs = import("/lua/ui/game/tabs.lua")
        check("in a game: Ctrl-F1 is the report's key", string.find(moho64ui_KeyMapAction("Ctrl-F1") or "", "/moho64ui/lua/report.lua", 1, true) ~= nil, true)
        check("F1 stays the game's key bindings", string.find(moho64ui_KeyMapAction("F1") or "", "remaster", 1, true), nil)

        -- the row in every kind of the game's menu, once, with the rows that leave the game after it
        local leaving = { RestartGame = true, RestartReplay = true, EndSPGame = true, EndMPGame = true, EndGame = true,
                          ExitSPGame = true, ExitMPGame = true, ExitGame = true, Return = true }
        local kinds = 0
        for name, list in Tabs.RemasterMenus().main do
            kinds = kinds + 1
            local at, count, before = 0, 0, 0
            for i, item in list do
                if item.action == 'RemasterReport' then at = i count = count + 1 end
                if leaving[item.action] and at == 0 then before = before + 1 end
            end
            local n = table.getn(list)
            local nextLeaves = at == n or leaving[(list[at + 1] or {}).action] == true
            check("the menu of kind '" .. name .. "' (" .. n .. " rows): the row once, no leaving row before it, the next one leaves",
                count .. " " .. before .. " " .. tostring(nextLeaves), "1 0 true")
        end
        check("the game's menu has four kinds (a game one plays, a replay, a network game, FAF's)", kinds, 4)
        check("the row's action is there", type(Tabs.RemasterMenuAction('RemasterReport')), "function")

        -- the menu as the game builds it: the row is a button of it
        Tabs.ToggleTab('main')
        WaitSeconds(1)
        local group = Tabs.controls.contentGroup
        check("the game's menu is open", up(group) and type(group.Buttons) == 'table', true)
        local button, place = nil, 0
        for i, b in (group and group.Buttons) or {} do
            if b.label and b.label:GetText() == "Report a Problem" then button, place = b, i end
        end
        local count = group and table.getn(group.Buttons) or 0
        LOG("TEST note the game's menu: " .. count .. " rows, Report a Problem is row " .. place)
        check("with the row Report a Problem", button ~= nil, true)
        local after = {}
        for i = place + 1, count do table.insert(after, group.Buttons[i].label:GetText()) end
        LOG("TEST note the rows after it: " .. table.concat(after, " | "))
        check("the rows after it are the four that leave (Restart, End Game, Exit to Windows, Close Menu)", count - place, 4)
        if button then
            check("the row lies inside the screen", button.Left() >= 0 and button.Right() <= GetFrame(0).Right() and button.Bottom() <= GetFrame(0).Bottom(), true)
        end
        if pic == "menu" then
            LOG("TEST done: " .. failed .. " failed")
            return
        end

        -- a click on the row: the menu closes, then the question is up
        local t0 = GetGameTimeSeconds()
        if button then button.OnClick(button) end
        WaitSeconds(1)
        check("the row clicked: the menu is closed", up(Tabs.controls.contentGroup), false)
        check("and the question is up", Report.State.open and up(Report.Controls.ask), true)
        local ask = Report.Controls.ask
        check("its line has the keyboard", ask and ask.inputBox and ask.inputBox:GetCurrentFocusControl() == ask.inputBox, true)
        if ask then
            check("the question lies inside the screen", ask.Left() >= 0 and ask.Top() >= 0 and ask.Right() <= GetFrame(0).Right() and ask.Bottom() <= GetFrame(0).Bottom(), true)
        end
        -- asked again while it is up: the same one
        check("asked again while it is up: the same question", Report.Ask() == ask, true)
        moho64ui_Type("the test's note by hand: a tank stood still")
        check("typed into its line", ask and ask.inputBox:GetText(), "the test's note by hand: a tank stood still")
        if pic == "ask" then
            LOG("TEST done: " .. failed .. " failed")
            return
        end
        WaitSeconds(2)  -- (the player types: the game's time goes on, the report names the time of the asking)
        moho64ui_Key(ENTER)
        WaitFrames(3)
        check("Enter: the question is gone", tostring(Report.State.open) .. " " .. tostring(up(ask)), "false false")
        check("a report was written", isZip(Report.State.last), true)
        check("one so far", Report.State.count, 1)
        LOG("TEST note report file: " .. tostring(Report.State.last) .. (Report.State.why and (" (" .. tostring(Report.State.why) .. ")") or ""))
        -- (the runner names the test's Desktop "desktop": MOHO64UI_REPORT_FOLDER)
        local where = string.lower((string.gsub(Report.State.last or "", "\\", "/")))
        check("onto the Desktop (the test's), not into the reports folder",
            tostring(string.find(where, "/desktop/remaster_report_", 1, true) ~= nil) .. " " .. tostring(string.find(where, "/reports/", 1, true) ~= nil), "true false")
        -- the note: the line typed and the game's time of the asking (not of the Enter)
        local s = math.floor(t0)
        local _, _, m, sec = string.find(Report.State.note or "", "^the test's note by hand: a tank stood still %[asked at game time (%d+):(%d%d)%]$")
        check("the note holds the line and the game's time of the asking", m ~= nil, true)
        if m then
            local at = tonumber(m) * 60 + tonumber(sec)
            check("that time is when the row was clicked (within 2 s), not when Enter was pressed", at >= s and at <= s + 2, true)
        end
        -- the box that says where the file is
        local told = Report.Controls.told
        check("a box says where the file is", up(told), true)
        if up(told) and told.nameText then
            -- (its own box since 2026-11-27: the file's name on ONE line, inside the box)
            check("the file's name stands on one line of the box", told.nameText:GetText(), string.gsub(Report.State.last, "^.*[/\\]", ""))
            check("and fits inside it", told.nameText.Left() >= told.content.Left() and told.nameText.Right() <= told.content.Right(), true)
            check("the box lies inside the screen", told.Left() >= 0 and told.Top() >= 0 and told.Right() <= GetFrame(0).Right() and told.Bottom() <= GetFrame(0).Bottom(), true)
        end
        if pic == "told" then
            LOG("TEST done: " .. failed .. " failed")
            return
        end
        moho64ui_Key(ENTER)
        WaitFrames(3)
        check("Enter closes the box", up(told), false)

        -- the key; Escape writes nothing
        moho64ui_Key(F1, "Ctrl")
        WaitFrames(2)
        check("Ctrl-F1: the question is up", Report.State.open and up(Report.Controls.ask), true)
        local second = Report.Controls.ask
        moho64ui_Type("not sent")
        moho64ui_Key(ESC)
        WaitFrames(3)
        check("Escape: the question is gone", tostring(Report.State.open) .. " " .. tostring(up(second)), "false false")
        check("and nothing more was written", Report.State.count, 1)
        -- the game's own Escape after it: the selection is cleared (eschandler's HandleEsc), as before the question
        ConExecute("UI_SelectByCategory +nearest COMMAND")
        WaitFrames(5)
        local had = table.getn(GetSelectedUnits() or {})
        moho64ui_Key(ESC)
        WaitFrames(5)
        check("the game's own Escape still works after it (a commander selected, then none)", had .. " " .. table.getn(GetSelectedUnits() or {}), "1 0")

        -- a line of nothing: a report all the same, the note only the time
        moho64ui_Key(F1, "Ctrl")
        WaitFrames(2)
        moho64ui_Key(ENTER)
        WaitFrames(3)
        check("nothing typed and Enter: a second report", tostring(Report.State.count) .. " " .. tostring(isZip(Report.State.last)), "2 true")
        check("its note is the time alone", string.find(Report.State.note or "", "^%[asked at game time %d+:%d%d%]$") ~= nil, true)
        LOG("TEST note report file 2: " .. tostring(Report.State.last))
        moho64ui_Key(ESC)
        WaitFrames(3)
        check("Escape closes the box too", up(Report.Controls.told), false)
        LOG("TEST done: " .. failed .. " failed")
    end)
    return
end

-- the front end: the main menu's line
ForkThread(function()
    WaitSeconds(1)
    local main = import("/lua/ui/menus/main.lua")
    check("Ctrl-F1 is the report's key", string.find(moho64ui_KeyMapAction("Ctrl-F1") or "", "/moho64ui/lua/report.lua", 1, true) ~= nil, true)
    check("the key as the game names it", Report.KeyText(), "Ctrl + F1")
    local KeyMapper = import("/lua/keymap/keymapper.lua")
    check("the key action's name", KeyMapper.GetActionName('remaster_report_ask'), "Report a problem (writes a report onto your Desktop)")
    local link = main.RemasterMenuReport.link
    check("the main menu has the line", up(link), true)
    check("its text, with the key", main.RemasterMenuReport.text, "Report a Problem  (Ctrl + F1)")
    if link then
        local frame = GetFrame(0)
        check("in the bottom left corner, inside the screen", link.Left() >= 0 and link.Left() < frame.Right() / 4 and link.Top() > frame.Bottom() * 0.9 and link.Bottom() <= frame.Bottom(), true)
        LOG("TEST note the line: " .. link.Left() .. "," .. link.Top() .. " to " .. link.Right() .. "," .. link.Bottom() .. " of " .. frame.Right() .. "x" .. frame.Bottom())
        if pic == "link" then
            LOG("TEST done: " .. failed .. " failed")
            return
        end
        link:HandleEvent({ Type = 'MouseEnter' })
        link:HandleEvent({ Type = 'ButtonPress' })
    end
    WaitFrames(2)
    check("a click on it: the question is up", Report.State.open and up(Report.Controls.ask), true)
    if pic == "ask" then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    moho64ui_Type("the menu's note")
    moho64ui_Key(ENTER)
    WaitFrames(3)
    check("Enter: a report with no game", isZip(Report.State.last), true)
    check("its note is the line alone (no game's time)", Report.State.note, "the menu's note")
    LOG("TEST note report file: " .. tostring(Report.State.last) .. (Report.State.why and (" (" .. tostring(Report.State.why) .. ")") or ""))
    check("a box says where the file is", up(Report.Controls.told), true)
    if pic == "told" then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    moho64ui_Key(ENTER)
    WaitFrames(3)
    check("Enter closes the box", up(Report.Controls.told), false)
    -- the key in the front end; Escape writes nothing
    moho64ui_Key(F1, "Ctrl")
    WaitFrames(2)
    check("Ctrl-F1 in the front end: the question is up", Report.State.open, true)
    moho64ui_Key(ESC)
    WaitFrames(3)
    check("Escape: gone, nothing more written", tostring(Report.State.open) .. " " .. Report.State.count, "false 1")
    LOG("TEST done: " .. failed .. " failed")
end)
