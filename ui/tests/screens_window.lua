-- The Options menu's Primary Adapter in a LIVE window (ui/tests/screens.lua checks the lists without one): a full
-- screen on each screen of the PC in turn, the last one first, then a window again. The window's place is read
-- back (moho64ui_Looks().windowScreen: the screen its middle is on). With one screen it goes to a full screen there
-- and back; with more it moves between them (the window covers each screen for two seconds). Then the player's
-- choice is kept as the last screen's mode, and a second run starts there (case "start"), on the same preferences:
--   moho64ui --init <init.lua> --prefs <a copy> --stage frontend --window --size 1280x720
--            --lua ui/tests/screens_window.lua --frames 200000 --log move.log
--   moho64ui --init <init.lua> --prefs <the same> --stage frontend --window
--            --lua ui/tests/screens_window.lua --frames 200000 --log start.log -- /screenscase start
-- (The frame budget is only a guard: the test ends itself. The test preferences are "uncapped", about 790 frames a
-- second in the front end, and the test waits about ten seconds: 6000 frames cut it off. From Git Bash the second
-- run needs MSYS_NO_PATHCONV=1, or "/screenscase" arrives as a path and the default case runs.)
-- (In the first, --size chose the mode, so the menu offers "Command Line Override" alone; the console's
-- SC_PrimaryAdapter still asks, as the menu's Apply would: a key "w,h,rate,N" names screen N, its size being the
-- screen's own anyway. The second has no --size: the profile's Primary Adapter chooses.)
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed"; the run then ends.
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end

local case = (GetCommandLineArg("/screenscase", 1) or {})[1]

ForkThread(function()
    WaitSeconds(2)
    local looks = moho64ui_Looks()
    local n = looks.screens
    if case == "start" then
        LOG("TEST note: " .. n .. " screen(s); the window opened on screen " .. (looks.windowScreen + 1) .. ", " .. GetFrame(0).Width() .. "x" .. GetFrame(0).Height())
        check("the profile's Primary Adapter is the last screen's mode", (import('/lua/user/prefs.lua').GetFromCurrentProfile('options') or {}).primary_adapter, "0,0,0," .. (n - 1))
        check("the window opened in full screen on that screen", looks.windowScreen, n - 1)
        check("of that screen's size", GetFrame(0).Width() > 1280 or GetFrame(0).Height() > 720, true)
        LOG("TEST done: " .. failed .. " failed")
        ExitApplication()
        return
    end
    local start = looks.windowScreen
    LOG("TEST note: " .. n .. " screen(s); the window opened on screen " .. (start + 1) .. ", " .. GetFrame(0).Width() .. "x" .. GetFrame(0).Height())
    check("the window is on a screen", start >= 0, true)
    check("of the size asked", GetFrame(0).Width() .. "x" .. GetFrame(0).Height(), "1280x720")
    for i = n - 1, 0, -1 do
        ConExecute("SC_PrimaryAdapter 0,0,0," .. i)
        WaitSeconds(2)
        LOG("TEST note: full screen on screen " .. (i + 1) .. ": " .. GetFrame(0).Width() .. "x" .. GetFrame(0).Height())
        check("full screen on screen " .. (i + 1) .. ": the window is there", moho64ui_Looks().windowScreen, i)
        check("full screen on screen " .. (i + 1) .. ": larger than the window was", GetFrame(0).Width() > 1280 or GetFrame(0).Height() > 720, true)
    end
    ConExecute("SC_PrimaryAdapter windowed")
    WaitSeconds(2)
    check("a window again, of the size before", GetFrame(0).Width() .. "x" .. GetFrame(0).Height(), "1280x720")
    check("back where it was", moho64ui_Looks().windowScreen, start)
    -- kept for the next start (the menu's way): the last screen
    local Prefs = import('/lua/user/prefs.lua')
    local options = Prefs.GetFromCurrentProfile('options') or {}
    options.primary_adapter = "0,0,0," .. (n - 1)
    Prefs.SetToCurrentProfile('options', options)
    SavePreferences()
    LOG("TEST done: " .. failed .. " failed")
    ExitApplication()
end)
