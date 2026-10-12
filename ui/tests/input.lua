-- Mouse input, checked from the script side: which control an event goes to, how it passes up
-- to parents, enter and exit, input capture. The host's debugging functions stand in for the mouse.
--   moho64ui --init <init.lua> --stage setup --size 1920x1080 --lua ui/tests/input.lua --frames 2 --log out.log
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local Group = import("/lua/maui/group.lua").Group
local Bitmap = import("/lua/maui/bitmap.lua").Bitmap

local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end

local root = GetFrame(0)
local seen = {}
local function listen(control, name, handles)
    control.HandleEvent = function(self, event)
        table.insert(seen, name .. ":" .. event.Type)
        return handles
    end
end
local function events()
    local s = table.concat(seen, " ")
    seen = {}
    return s
end

-- a panel with two overlapping children; the right one is deeper in front
local panel = Group(root, "panel")
panel.Left:Set(100) panel.Top:Set(100) panel.Width:Set(400) panel.Height:Set(200)
local a = Bitmap(panel) a:SetSolidColor("ff800000")
a.Left:Set(100) a.Top:Set(100) a.Width:Set(250) a.Height:Set(200)
local b = Bitmap(panel) b:SetSolidColor("ff008000")
b.Left:Set(250) b.Top:Set(100) b.Width:Set(250) b.Height:Set(200)
b.Depth:Set(function() return a.Depth() + 5 end)
listen(panel, "panel", true)
listen(a, "a", false)
listen(b, "b", false)

check("the control under the cursor", moho64ui_ControlAt(150, 150) == a, true)
check("where two overlap, the one with the higher Depth", moho64ui_ControlAt(300, 150) == b, true)
check("outside every control it is the root frame", moho64ui_ControlAt(900, 900) == root, true)
b:DisableHitTest()
check("a control with hit testing off is passed over", moho64ui_ControlAt(300, 150) == a, true)
b:EnableHitTest()
b:Hide()
check("a hidden control is passed over", moho64ui_ControlAt(300, 150) == a, true)
b:Show()

moho64ui_MouseMove(150, 150)
check("moving onto a control: MouseEnter, then MouseMotion; unhandled events pass to the parent",
    events(), "a:MouseEnter panel:MouseEnter a:MouseMotion panel:MouseMotion")
moho64ui_MouseMove(160, 150)
check("moving inside it: MouseMotion only", events(), "a:MouseMotion panel:MouseMotion")
moho64ui_MouseMove(300, 150)
check("moving onto the next control: MouseExit for the old one, MouseEnter for the new",
    events(), "a:MouseExit panel:MouseExit b:MouseEnter panel:MouseEnter b:MouseMotion panel:MouseMotion")

listen(b, "b", true)
moho64ui_MouseButton("Left", true, 300, 150)
moho64ui_MouseButton("Left", false, 300, 150)
check("a handled event stops at the control", events(), "b:ButtonPress b:ButtonRelease")

local details
b.HandleEvent = function(self, event)
    details = event.Type .. " " .. event.MouseX .. "," .. event.MouseY .. " left " .. tostring(event.Modifiers.Left) ..
        " right " .. tostring(event.Modifiers.Right) .. " control " .. tostring(event.Control == b)
    return true
end
moho64ui_MouseButton("Right", true, 310, 160)
check("the event table", details, "ButtonPress 310,160 left false right true control true")
listen(b, "b", true)
events()

-- input capture: the capturing control gets the events wherever the cursor is
AddInputCapture(a)
check("capture is reported", tostring(AnyInputCapture()) .. tostring(GetInputCapture() == a), "truetrue")
moho64ui_MouseButton("Left", true, 900, 900)
check("a captured press goes to the capturing control", events(), "b:MouseExit a:MouseEnter panel:MouseEnter a:ButtonPress panel:ButtonPress")
RemoveInputCapture(a)
check("capture released", AnyInputCapture(), false)

-- a control that is destroyed while the cursor is over it is simply forgotten
moho64ui_MouseMove(300, 150)
events()
b:Destroy()
moho64ui_MouseMove(300, 150)
check("after its destruction the control below gets the cursor", events(), "a:MouseEnter panel:MouseEnter a:MouseMotion panel:MouseMotion")

-- a dragger takes the mouse from the controls until its button is released
local Dragger = import("/lua/maui/dragger.lua").Dragger
local drag = {}
a.HandleEvent = function(self, event)
    table.insert(seen, "a:" .. event.Type)
    if event.Type == "ButtonPress" then
        local dragger = Dragger()
        dragger.OnMove = function(d, x, y) table.insert(drag, "move " .. x .. "," .. y) end
        dragger.OnRelease = function(d, x, y) table.insert(drag, "release " .. x .. "," .. y) d:Destroy() end
        dragger.OnCancel = function(d) table.insert(drag, "cancel") d:Destroy() end
        PostDragger(self:GetRootFrame(), event.KeyCode, dragger)
    end
    return true
end
events()
moho64ui_MouseButton("Left", true, 150, 150)
moho64ui_MouseMove(700, 700)
moho64ui_MouseButton("Right", false, 150, 150)
moho64ui_MouseButton("Left", false, 710, 720)
check("the dragger gets the moves and the release of its own button", table.concat(drag, " | "), "move 700,700 | release 710,720")
check("meanwhile the controls only hear of the other button", events(), "a:ButtonPress a:ButtonRelease")
moho64ui_MouseMove(150, 150)
check("after the release the controls get the mouse again", events(), "a:MouseMotion")
drag = {}
moho64ui_MouseButton("Left", true, 150, 150)
PostDragger(root, 0, nil)
check("posting nil cancels the dragger", table.concat(drag, " | "), "cancel")
moho64ui_MouseButton("Left", false, 150, 150)
events()

panel:Destroy()
LOG("TEST done: " .. failed .. " failed")
