-- The main menu's background in the window's own picture (app/backdrop.h, the game's way: the map drawn
-- straight into the window by the renderer, the interface laid over it and painted only where it changes),
-- through the state changes: the main menu, a full screen and back to a window, the skirmish lobby (the
-- background stopped), Launch Game, the loading screen, the game, and back to the main menu. A live window only
-- (without one the background is the read-back way's pixels):
--   moho64ui --init <init.lua> --prefs <a copy> --stage frontend --window --size 1600x900
--            --lua ui/tests/menu_backdrop.lua --game-lua ui/tests/menu_backdrop.lua --frames 3000 --log out.log
-- (It writes the preferences: give it a copy. The window goes to a full screen for two seconds.)
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed" (in the game; the
-- front end's own checks go along). The host's log has, beside: "the menu's background: ..., in the window's
-- picture" twice (at the start and after the game), and with MOHO64UI_DIRTY_CHECK=1 "checked N frames against a
-- full redraw, 0 unlike".
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
    -- the game launched from the lobby: a moment of it, then back to the main menu
    ForkThread(function()
        WaitSeconds(3)
        failed = (GetFrontEndData('menu_backdrop_test') or {}).failed or 0
        check("the game launched from the lobby runs", GetGameTimeSeconds() > 0, true)
        LOG("TEST done: " .. failed .. " failed")
        ExitGame()
    end)
    return
end

-- the lobby keeps its controls in locals of its module: read through a function using them
local function upvalue(fn, name)
    for i = 1, 200 do
        local n, v = debug.getupvalue(fn, i)
        if not n then break end
        if n == name then return v end
    end
end
local function clickOn(control)
    local x, y = control.Left() + control.Width() / 2, control.Top() + control.Height() / 2
    moho64ui_MouseMove(x, y)
    WaitFrames(3)
    moho64ui_MouseButton("Left", true, x, y)
    WaitFrames(2)
    moho64ui_MouseButton("Left", false, x, y)
    WaitFrames(3)
end
-- The interface's own pixel at the window's left edge, half way down (the main menu has nothing of its own
-- there): over the background only the veil, black at 0x38 (the picture's darkening, as the read-back way's
-- 200 / 256 of the world's colours).
local function edge()
    return moho64ui_PixelAt(4, math.floor(GetFrame(0).Height() / 2))
end

ForkThread(function()
    WaitSeconds(3)
    local W, H = GetFrame(0).Width(), GetFrame(0).Height()
    check("the main menu: the background's part of the interface is the veil alone", edge(), "38000000")
    -- a full screen and back (the Options' display mode, as its Apply sets it)
    ConExecute("SC_PrimaryAdapter fullscreen")
    WaitSeconds(2)
    LOG("TEST note: full screen " .. GetFrame(0).Width() .. "x" .. GetFrame(0).Height())
    check("full screen: the veil still", edge(), "38000000")
    ConExecute("SC_PrimaryAdapter windowed")
    WaitSeconds(2)
    check("a window again, of the size before", GetFrame(0).Width() .. "x" .. GetFrame(0).Height(), W .. "x" .. H)
    check("a window again: the veil still", edge(), "38000000")
    -- the main menu's Skirmish (its first-time question answered before): the menu goes, its background stops,
    -- the lobby comes
    import("/lua/user/prefs.lua").SetToCurrentProfile("MenuTutorialPrompt", true)
    import("/lua/ui/menus/main.lua").ButtonSkirmish()
    WaitSeconds(4)
    local lobby = import("/lua/ui/lobby/lobby.lua")
    local GUI = upvalue(lobby.SetRuleTitleText, 'GUI')
    check("the lobby is up", type(GUI) == 'table' and GUI.launchGameButton ~= nil, true)
    check("the lobby: no veil (the background stopped)", edge() ~= "38000000", true)
    local changelog = import("/lua/ui/lobby/changelog/changelogdialog.lua")
    if changelog.IsOpen() then changelog.CloseChangelogDialog() WaitFrames(3) end
    SetFrontEndData('menu_backdrop_test', {failed = failed})
    if GUI and GUI.launchGameButton then clickOn(GUI.launchGameButton) end
    WaitSeconds(15)  -- (the front end's script runs on while the game loads behind the loading screen: 2-8 s)
    check("Launch Game starts the game", false, true)
    LOG("TEST done: " .. failed .. " failed (in the front end)")
end)
