-- Two mouse buttons at once over the world (the review of 2026-11-09, toolkit H3; ui/maui/worldview.cpp): another
-- button pressed while a drag of the view is open does not end that drag behind its back.
--   (a) a camera drag by the middle button, a right click during it: the middle release ends the drag (the pointer is
--       no longer held; before, it stayed hidden and held and the screen's edges no longer scrolled);
--   (b) a selection box by the left button, a right click during it: the left release takes the box's units (before,
--       the box stood until the next left click and selected nothing);
--   (c) a camera drag, then a left press and release: the camera drag is still open until the middle button goes up.
-- Run it like game.lua: a launched game with cheats (session_ai_cheats.lua) at 1920x1080, --frames 1500.
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
ForkThread(function()
    WaitSeconds(8)
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local cdr = GetArmyAvatars()[1]
    local c = cdr:GetPosition()
    SelectUnits(nil)
    WaitSeconds(0.2)
    check("nothing dragged at first", moho64ui_DragState(), "")

    -- (a) the camera's drag, a right click in it
    moho64ui_MouseButton("Middle", true, 900, 500)
    moho64ui_MouseMove(880, 500)
    check("the middle button drags the camera: the pointer is held", moho64ui_DragState(), "held dragging")
    moho64ui_MouseButton("Right", true, 880, 500)
    moho64ui_MouseButton("Right", false, 880, 500)
    check("a right click during it leaves the drag open", moho64ui_DragState(), "held dragging")
    moho64ui_MouseMove(860, 500)
    moho64ui_MouseButton("Middle", false, 860, 500)
    check("the middle release ends it: the pointer is free", moho64ui_DragState(), "")

    -- (c) the camera's drag, a left click in it
    moho64ui_MouseButton("Middle", true, 900, 500)
    moho64ui_MouseMove(890, 500)
    moho64ui_MouseButton("Left", true, 890, 500)
    moho64ui_MouseButton("Left", false, 890, 500)
    check("a left click during a camera drag leaves it open", moho64ui_DragState(), "held dragging")
    moho64ui_MouseButton("Middle", false, 890, 500)
    check("and the middle release ends it", moho64ui_DragState(), "")

    -- (b) a selection box round the commander, a right click in it
    WaitSeconds(0.3)
    local p = view:Project(c)
    moho64ui_MouseMove(p.x - 60, p.y - 60)
    moho64ui_MouseButton("Left", true, p.x - 60, p.y - 60)
    moho64ui_MouseMove(p.x + 20, p.y + 20)
    check("the left button drags a box", moho64ui_DragState(), "dragging")
    moho64ui_MouseButton("Right", true, p.x + 20, p.y + 20)
    moho64ui_MouseButton("Right", false, p.x + 20, p.y + 20)
    check("a right click during it leaves the box open", moho64ui_DragState(), "dragging")
    moho64ui_MouseMove(p.x + 60, p.y + 60)
    moho64ui_MouseButton("Left", false, p.x + 60, p.y + 60)
    WaitSeconds(0.3)
    check("the left release closes the box", moho64ui_DragState(), "")
    local selected = GetSelectedUnits() or {}
    check("and its units are selected (the commander)", selected[1] and selected[1]:GetEntityId(), cdr:GetEntityId())
    SelectUnits(nil)
    LOG("TEST done: " .. failed .. " failed")
end)
