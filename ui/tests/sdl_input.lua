-- The SDL window (Linux and Mac; a Windows test build, build_ui.py --sdl): events put into SDL's own
-- queue come through the window as the system's would, and the interface hears what the Win32
-- window hands it: the game's key codes, the characters after a key down nobody handled, the
-- mouse's buttons, double clicks and wheel steps.
--   moho64ui --init <init.lua> --stage setup --window --no-world --size 1280x720 --lua ui/tests/sdl_input.lua --frames 240 --log out.log
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
if not rawget(_G, "moho64ui_SdlKey") then
    LOG("TEST done: 0 failed (not an SDL build: nothing to try)")
    return
end
local Bitmap = import("/lua/maui/bitmap.lua").Bitmap
local root = GetFrame(0)
-- SDL's scancodes
local A, B, ONE, ENTER, ESCAPE, BACKSPACE, F1, LEFT = 4, 5, 30, 40, 41, 42, 58, 80

local pad = Bitmap(root)
pad:SetSolidColor("ff203040")
pad.Left:Set(100) pad.Top:Set(100) pad.Width:Set(400) pad.Height:Set(300)
local seen, handle = {}, false
pad.HandleEvent = function(self, event)
    local t = event.Type
    if t == "KeyDown" or t == "KeyUp" or t == "Char" then
        table.insert(seen, t .. event.KeyCode .. (event.Modifiers.Shift and "s" or "") .. (event.Modifiers.Ctrl and "c" or ""))
        return handle and t == "KeyDown"
    elseif t == "WheelRotation" then
        table.insert(seen, t .. event.WheelRotation)
    elseif t == "ButtonPress" or t == "ButtonDClick" or t == "ButtonRelease" then
        table.insert(seen, t .. (event.Modifiers.Left and "L" or "") .. (event.Modifiers.Right and "R" or "") .. "@" .. math.floor(event.MouseX) .. "," .. math.floor(event.MouseY))
    end
    return false
end
pad:AcquireKeyboardFocus(true)

local function after(f)  -- (the events come through the window's Pump at the next frames)
    seen = {}
    f()
    WaitSeconds(0.15)
    return table.concat(seen, " ")
end
local function key(sc, text)
    moho64ui_SdlKey(sc, "down")
    if text then moho64ui_SdlText(text) end
    moho64ui_SdlKey(sc, "up")
end

ForkThread(function()
    WaitSeconds(0.3)
    check("a letter: key down (the game's code), the system's character, key up", after(function() key(A, "a") end), "KeyDown65 Char97 KeyUp65")
    handle = true
    check("a handled key down swallows its character", after(function() key(B, "b") end), "KeyDown66 KeyUp66")
    handle = false
    check("a digit", after(function() key(ONE, "1") end), "KeyDown49 Char49 KeyUp49")
    check("Enter: its character made as Windows makes it", after(function() key(ENTER) end), "KeyDown13 Char13 KeyUp13")
    check("Escape", after(function() key(ESCAPE) end), "KeyDown27 Char27 KeyUp27")
    check("Backspace", after(function() key(BACKSPACE) end), "KeyDown8 Char8 KeyUp8")
    check("an arrow key: the game's code and its own character", after(function() key(LEFT) end), "KeyDown316 Char316 KeyUp316")
    check("F1", after(function() key(F1) end), "KeyDown342 Char342 KeyUp342")
    moho64ui_SdlMods("Shift")
    check("Shift held", after(function() key(A, "A") end), "KeyDown65s Char65s KeyUp65s")
    moho64ui_SdlMods("Ctrl")
    check("Ctrl+A: the control character Windows makes", after(function() key(A) end), "KeyDown65c Char1c KeyUp65c")
    moho64ui_SdlMods("")
    check("text beyond ASCII: its character's code", after(function() moho64ui_SdlText("\195\169") end), "Char233")
    -- the mouse
    check("a left click on the control", after(function()
        moho64ui_SdlMouse("move", 200, 150)
        moho64ui_SdlMouse("down", 200, 150, 1)
        moho64ui_SdlMouse("up", 200, 150, 1)
    end), "ButtonPressL@200,150 ButtonReleaseL@200,150")
    check("a double click's second press", after(function()
        moho64ui_SdlMouse("down", 210, 160, 1, 2)
        moho64ui_SdlMouse("up", 210, 160, 1, 2)
    end), "ButtonDClickL@210,160 ButtonReleaseL@210,160")
    check("the right button", after(function()
        moho64ui_SdlMouse("down", 220, 170, 3)
        moho64ui_SdlMouse("up", 220, 170, 3)
    end), "ButtonPressR@220,170 ButtonReleaseR@220,170")
    check("the wheel: Windows' steps of 120, away from the user positive", after(function() moho64ui_SdlMouse("wheel", 200, 150, 1) end), "WheelRotation120")
    check("and towards", after(function() moho64ui_SdlMouse("wheel", 200, 150, -2) end), "WheelRotation-240")
    LOG("TEST done: " .. failed .. " failed")
end)
