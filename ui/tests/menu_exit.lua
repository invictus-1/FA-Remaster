-- The main menu's Exit, as a player leaves the game (FAF's /lua/ui/menus/main.lua: the button "Exit" and the Escape
-- key both call ButtonExit, which asks "Are you sure you'd like to exit?"; Yes calls ExitApplication). No test walked
-- this path (the review of 2026-11-09, section 5: "the Exit button's path").
--   1. a click on Exit: the question is up; No: it goes, the menu stays;
--   2. Exit again, and Escape answers No (the dialog's escape button); then Escape on the bare menu: the question
--      comes and stays, as in the original, and Escape again answers No;
--   3. Exit and Yes: the program ends by itself. The test's last line is logged BEFORE that click; the runner then
--      checks that the program ended in order: exit code 0, and well before its frames ran out.
--   moho64ui --init <init.lua> --prefs <a copy> --stage frontend --size 1920x1080 --no-world
--            --lua ui/tests/menu_exit.lua --frames 20000 --log out.log
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

-- the newest shown text control with that text (a button's label, a dialog's line)
local function shownText(text)
    local found = nil
    GetFrame(0):ApplyFunction(function(control)
        if control.GetText and not IsDestroyed(control) and not control:IsHidden() and control:GetText() == text then found = control end
    end)
    return found
end
local function click(c)
    if not c or IsDestroyed(c) then return false end
    local x, y = (c.Left() + c.Right()) / 2, (c.Top() + c.Bottom()) / 2
    moho64ui_MouseMove(x, y)
    WaitSeconds(0.2)
    moho64ui_MouseButton("Left", true, x, y)
    moho64ui_MouseButton("Left", false, x, y)
    WaitSeconds(0.5)
    return true
end
local function waitFor(seconds, done)
    local waited = 0
    while waited < seconds do
        if done() then return true end
        WaitSeconds(0.25)
        waited = waited + 0.25
    end
    return done()
end
local function escape()
    moho64ui_Key(27, "", "down")
    moho64ui_Key(27, "", "up")
    WaitSeconds(0.5)
end

local kExit = LOC("<LOC _Exit>")
local kYes, kNo = LOC("<LOC _Yes>"), LOC("<LOC _No>")
-- (the question's own line is a wrapped text made of several controls: the dialog is known by its two buttons, which
-- the menu itself does not have)
local function asked() return shownText(kYes) ~= nil and shownText(kNo) ~= nil or nil end

ForkThread(function()
    check("the main menu is up with its Exit button", waitFor(20, function() return shownText(kExit) ~= nil end), true)
    WaitSeconds(1.5)  -- (the menu's opening slide: its buttons take the mouse once it is done)

    -- 1. the button, then No
    check("a click on Exit", click(shownText(kExit)), true)
    check("asks whether to exit: Yes and No are up", waitFor(3, function() return asked() ~= nil end), true)
    check("No is clicked", click(shownText(kNo)), true)
    check("the question is gone", waitFor(3, function() return asked() == nil end), true)
    check("and the menu is still there", shownText(kExit) ~= nil, true)

    -- 2. Escape on the question is No (the dialog's escape button)
    check("Exit once more", click(shownText(kExit)), true)
    check("the question is up", waitFor(3, function() return asked() ~= nil end), true)
    escape()
    check("Escape on the question answers No", waitFor(3, function() return asked() == nil end), true)
    check("the menu is still there", shownText(kExit) ~= nil, true)
    -- 2b. Escape on the bare menu asks too, and the question STAYS. The key map (loaded by the program at its start:
    -- CUIKeyHandler::LoadKeyMappings, keymapper.GetKeyMappings) has Escape as the action "escape", which calls
    -- uimain.EscapeHandler, and the menu gave that ButtonExit. In the original the question comes and stays (Chris's
    -- picture of 2026-11-11). Until that day ours handed the key press that opened the question to the question
    -- itself, whose Escape is No: it was gone within the frame and Escape seemed to do nothing (ui/maui/input.cpp,
    -- KeyEvent).
    escape()
    check("Escape on the bare menu asks whether to exit", waitFor(3, function() return asked() ~= nil end), true)
    WaitSeconds(1)
    check("and the question stays: the key that opened it does not answer it", asked() ~= nil, true)
    escape()
    check("Escape again answers No", waitFor(3, function() return asked() == nil end), true)
    check("the menu is still there after it", shownText(kExit) ~= nil, true)

    -- 3. Exit and Yes: the program ends (nothing can be logged after it; the runner reads how it ended)
    check("Exit again", click(shownText(kExit)), true)
    check("the question", waitFor(3, function() return asked() ~= nil end), true)
    local yes = shownText(kYes)
    check("Yes is there to click", yes ~= nil, true)
    LOG("TEST note: menu_exit: Yes is clicked now; the program must end by itself")
    LOG("TEST done: " .. failed .. " failed")
    click(yes)
    WaitSeconds(5)
    LOG("TEST FAIL the program still runs five seconds after Yes on the exit question")
end)
