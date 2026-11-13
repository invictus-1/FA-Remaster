-- The game's window, in a REAL window (docs/ui-window.md; Chris, 2026-11-13): its smallest size, the game's icon, mouse
-- button 4 as a key, Alt+Enter both ways, and the place kept in the preferences and taken up again at the next start.
-- Three runs on one copy of the preferences (the lab's window_checks.sh, `wc_window`; the two with a window show on
-- the screen for a few seconds each):
--   moho64ui ... --stage frontend --lua ui/tests/window_live.lua --frames 600 -- /wincase prepare     (no window)
--   moho64ui ... --stage frontend --window --lua ui/tests/window_live.lua --frames 100000 -- /wincase first
--   moho64ui ... --stage frontend --window --lua ui/tests/window_live.lua --frames 100000 -- /wincase again
-- prepare: the profile's Primary Adapter set to Windowed. first: the checks, the window left at 200,160 and 1280x800.
-- again: the window opens there. The runs with a window give no --size: the command line leaves the mode to the player's
-- option, which is when the place is kept (DisplayMode::placeKept). Each ends itself (ExitApplication).
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
local case = (GetCommandLineArg("/wincase", 1) or {})[1] or "first"
local Prefs = import('/lua/user/prefs.lua')
local function primary() return (Prefs.GetFromCurrentProfile('options') or {}).primary_adapter end
local function place(p) return p and (p.x .. "," .. p.y .. " " .. p.width .. "x" .. p.height) end

ForkThread(function()
    WaitSeconds(1)
    if case == 'prepare' then
        local o = Prefs.GetFromCurrentProfile('options') or {}
        o.primary_adapter = 'windowed'
        Prefs.SetToCurrentProfile('options', o)
        SavePreferences()
        check("prepared: the profile's Primary Adapter is Windowed", primary(), 'windowed')
        check("no window in this run", moho64ui_WindowProbe(), nil)
        LOG("TEST done: " .. failed .. " failed")
        ExitApplication()
        return
    end
    local p = moho64ui_WindowProbe()
    if not p then
        LOG("TEST FAIL this run has no live window (give --window)")
        LOG("TEST done: 1 failed")
        ExitApplication()
        return
    end
    check("the command line left the mode free: the place is kept in this run", moho64ui_Looks().placeKept, 1)
    check("a framed window (the profile chose Windowed)", p.full, false)
    check("with the game's own icon", p.icon, true)
    if case == 'again' then
        check("the next start: the window where and as large as the last one stood", place(p), "200,160 1280x800")
        LOG("TEST done: " .. failed .. " failed")
        ExitApplication()
        return
    end
    check("its first size: 1024x768 inside (nothing kept yet)", p.width .. "x" .. p.height, "1024x768")
    -- the smallest size
    moho64ui_WindowPoke("size", 640, 480)
    WaitSeconds(0.3)
    p = moho64ui_WindowProbe()
    check("set to 640x480: it stays 1024x768 inside", p.width .. "x" .. p.height, "1024x768")
    -- mouse buttons 4 and 5 as keys
    rawset(_G, "extraRan", {})
    IN_AddKeyMapTable({
        ["XButton1"] = {action = "UI_Lua table.insert(extraRan, 'x1')"},
        ["XButton2"] = {action = "UI_Lua table.insert(extraRan, 'x2')"},
    })
    moho64ui_WindowPoke("xbutton", 1)
    moho64ui_WindowPoke("xbutton", 2)
    check("mouse buttons 4 and 5 pressed in the window run the actions bound to XButton1 and XButton2", table.concat(extraRan, " "), "x1 x2")
    IN_RemoveKeyMapTable({["XButton1"] = {action = ""}, ["XButton2"] = {action = ""}})
    -- a place of its own
    moho64ui_WindowPoke("size", 1280, 800)
    moho64ui_WindowPoke("move", 140, 120)
    WaitSeconds(0.3)
    check("moved and sized by hand", place(moho64ui_WindowProbe()), "140,120 1280x800")
    -- Alt+Enter: a full screen on the screen the window is on, the option following, the place kept
    moho64ui_WindowPoke("altenter")
    WaitSeconds(1.5)
    p = moho64ui_WindowProbe()
    check("Alt+Enter: a full screen", p.full, true)
    check("the profile's Primary Adapter follows (a screen's mode)", string.find(tostring(primary()), "^%d+,%d+,%d+") ~= nil, true)
    check("and the window's place is in the preferences", tostring(GetPreference('Windows.Main.x')) .. "," .. tostring(GetPreference('Windows.Main.y')) .. " " ..
        tostring(GetPreference('Windows.Main.width')) .. "x" .. tostring(GetPreference('Windows.Main.height')) .. " " ..
        tostring(GetPreference('Windows.Main.maximized')), "140,120 1280x800 false")
    moho64ui_WindowPoke("altenter")
    WaitSeconds(1.5)
    p = moho64ui_WindowProbe()
    check("Alt+Enter again: the window back where and as large as it was", tostring(p.full) .. " " .. place(p), "false 140,120 1280x800")
    check("the profile's Primary Adapter is Windowed again", primary(), 'windowed')
    -- maximized, a full screen and back: maximized again, the plain place kept beside
    moho64ui_WindowPoke("maximize")
    WaitSeconds(0.5)
    check("maximized", moho64ui_WindowProbe().maximized, true)
    moho64ui_WindowPoke("altenter")
    WaitSeconds(1.5)
    moho64ui_WindowPoke("altenter")
    WaitSeconds(1.5)
    p = moho64ui_WindowProbe()
    check("a maximized window sent to a full screen and back is maximized again", tostring(p.full) .. " " .. tostring(p.maximized), "false true")
    moho64ui_WindowPoke("restore")
    WaitSeconds(0.5)
    check("and restored it stands where it stood", place(moho64ui_WindowProbe()), "140,120 1280x800")
    -- left at another place for the next start
    moho64ui_WindowPoke("move", 200, 160)
    WaitSeconds(0.3)
    LOG("TEST done: " .. failed .. " failed")
    ExitApplication()
end)
