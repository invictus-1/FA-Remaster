-- The screenshot of "Report a Problem" (ours; /moho64ui/lua/report.lua, ui/user/report.cpp; Chris, 2026-11-27, the
-- ideas' number 3: the report by hand also holds a picture of that moment). In a WINDOW (the picture is the window's
-- shown frame, read back by the renderer; a run without a window takes none, which ui/tests/report_ask.lua covers):
-- the key asks, the screen is pictured BEFORE the question's box comes up, the box follows within a few frames, the
-- report written holds the picture and its box says so; a question called off forgets its picture, so that a report
-- written after it by the console has none.
-- The lab's runner (suite/report_ask.sh) reads the zips: screenshot.jpg whole, of the window's size (or halved under
-- MOHO64UI_REPORT_PICTURE_MAX), none in the console's.
--   moho64ui --own ui/data --init <init.lua> --prefs <a copy> --window --size 1920x1080 --session <session_ai_cheats.lua>
--            --game-lua ui/tests/report_picture.lua --frames 2000000 -- /exit 1
--   moho64ui ... --stage frontend --window --size 2560x1440 --lua ui/tests/report_picture.lua --frames 2000000 -- /exit 1
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
local function up(control) return control and not IsDestroyed(control) and true or false end
local function done()
    LOG("TEST done: " .. failed .. " failed")
    if GetCommandLineArg("/exit", 1) then ExitApplication() end
end
local inGame = rawget(_G, "SessionIsActive") and SessionIsActive()

ForkThread(function()
    -- (a game: the loading screen gone and the world drawn a while; the front end: the menu up)
    if inGame then
        while WorldIsLoading() or GetGameTimeSeconds() < 12 do WaitFrames(10) end
        WaitFrames(120)
    else
        WaitSeconds(6)
    end
    check("the host can picture the window", rawget(_G, 'moho64ui_ReportPicture') ~= nil, true)
    check("no picture before the asking", moho64ui_ReportPicture("state"), "none")

    -- the key: the picture first, the box after
    moho64ui_Key(F1, "Ctrl")
    check("asked: the question counts as open at once", Report.State.open, true)
    check("the picture is being taken, the box not yet up", tostring(Report.State.picturing) .. " " .. tostring(up(Report.Controls.ask)), "true false")
    check("asked again meanwhile: nothing new", Report.Ask(), nil)
    local waited = 0
    while not up(Report.Controls.ask) and waited < 30 do
        WaitFrames(1)
        waited = waited + 1
    end
    LOG("TEST note the box came up " .. waited .. " frame(s) after the key; the picture took " .. tostring(Report.State.pictureFrames) .. " frame(s)")
    check("the box is up within 5 frames", up(Report.Controls.ask) and waited <= 5, true)
    check("the picture was taken before it", moho64ui_ReportPicture("state"), "taken")
    local ask = Report.Controls.ask
    check("its line has the keyboard", ask and ask.inputBox and ask.inputBox:GetCurrentFocusControl() == ask.inputBox, true)
    moho64ui_Type("the picture's note")
    moho64ui_Key(ENTER)
    WaitFrames(5)
    check("Enter: a report was written", type(Report.State.last) == 'string' and string.sub(Report.State.last, -4) == ".zip", true)
    check("with the picture", Report.State.pictured, true)
    check("and the picture is forgotten after", moho64ui_ReportPicture("state"), "none")
    LOG("TEST note report file: " .. tostring(Report.State.last))
    local told = Report.Controls.told
    -- (the box's text is wrapped: its lines joined)
    local lines = {}
    for _, t in (up(told) and told.texts) or {} do table.insert(lines, t:GetText()) end
    check("the box says the report holds a picture of the game's screen",
        string.find((string.gsub(table.concat(lines, " "), "%s+", " ")), "a picture of the game's screen", 1, true) ~= nil, true)
    LOG("TEST note the box's text: " .. table.concat(lines, " / "))
    if up(told) then
        check("the box lies inside the screen", told.Left() >= 0 and told.Top() >= 0 and told.Right() <= GetFrame(0).Right() and told.Bottom() <= GetFrame(0).Bottom(), true)
    end
    moho64ui_Key(ENTER)
    WaitFrames(3)
    check("Enter closes the box", up(told), false)

    -- called off: the picture is forgotten, and the console's report after it has none
    moho64ui_Key(F1, "Ctrl")
    waited = 0
    while not up(Report.Controls.ask) and waited < 30 do
        WaitFrames(1)
        waited = waited + 1
    end
    check("asked again: a picture taken, the box up", moho64ui_ReportPicture("state") .. " " .. tostring(up(Report.Controls.ask)), "taken true")
    moho64ui_Key(ESC)
    WaitFrames(4)
    check("Escape: the question is gone", tostring(Report.State.open) .. " " .. tostring(up(Report.Controls.ask)), "false false")
    check("and its picture forgotten", moho64ui_ReportPicture("state"), "none")
    check("no second report by hand", Report.State.count, 1)
    local file = moho64ui_Report("the console's, after a question called off", false)
    check("the console's report is written", type(file), "string")
    LOG("TEST note console report file: " .. tostring(file))
    done()
end)
