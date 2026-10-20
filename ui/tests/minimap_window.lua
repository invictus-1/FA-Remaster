-- The minimap's window by hand, the game's own (FAF's /lua/ui/game/minimap.lua on /lua/maui/window.lua, run
-- unchanged), driven as a player does it with the mouse on its real controls: its title bar dragged moves it (kept
-- inside the screen), each of its four corners and four edges dragged resizes it (the edge goes to the pointer, no
-- smaller than the script's 150 x 150), the drag handle at a corner lights up while the pointer is on it, the pointer
-- takes the script's cursor for each; the minimap world view inside follows (the client group's insets), the minimap's
-- picture follows the new size (the map in its own proportion in the middle of the view: where the view's Project puts
-- the map's corners once the picture is drawn), and a left press on it still sends the main camera to the place under
-- the pointer, after a move and after a resize (drawn at that size or not yet), the camera looking at that place itself
-- (the original's TargetLocation, FA exe 0x7a82f0: the place with its height); the place and size are saved in the
-- profile (mini_ui_minimap) at every release. Its buttons: the pin locks place and size (the profile's minimapPin),
-- the reset button puts it back where the script first had it, the close button hides it (the profile's stratview);
-- the screen made smaller pulls it back inside (window.lua's OnDirty on the frame's size). The resource icons: the
-- world view's switch (EnableResourceRendering) is on for a new view (FA exe 0x86e544: +0x136 = 1), the minimap's off
-- as FAF's minimap.lua sets it from the profile's MiniMap_resource_icons (none: off); the map options dialog of the
-- multifunction panel (its "Resources" item for the minimap) turns it on and keeps it in the profile.
-- Two runs on the same preferences file ("the next game"): the first ends with the window moved and resized, shown,
-- with the minimap's resources on (for --screenshot); the second (-- /mmcase again) finds it there.
-- Run it like orderlines.lua (a launched game, session_ai_cheats.lua, seed 7, with the world's picture):
--   --frames 2700 [--screenshot <file>]        then, on the same --prefs file:   --frames 600 -- /mmcase again
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local case = (GetCommandLineArg("/mmcase", 1) or {})[1] or "first"
local Prefs = import("/lua/user/prefs.lua")
local UIUtil = import("/lua/ui/uiutil.lua")

local function rect(c) return string.format("%d %d %d %d", c.Left(), c.Top(), c.Right(), c.Bottom()) end
local function centre(c) return (c.Left() + c.Right()) / 2, (c.Top() + c.Bottom()) / 2 end
local function saved()
    local p = Prefs.GetFromCurrentProfile('mini_ui_minimap')
    return p and string.format("%d %d %d %d (%d x %d)", p.left, p.top, p.right, p.bottom, p.width, p.height) or "nil"
end
local function size(c) return string.format(" (%d x %d)", c.Right() - c.Left(), c.Bottom() - c.Top()) end
-- the mouse as a player moves it: a press, moves in steps, a release, a few frames between each
local function press(x, y)
    moho64ui_MouseMove(x, y)
    WaitSeconds(0.05)
    moho64ui_MouseButton("Left", true, x, y)
    WaitSeconds(0.05)
end
local function moveTo(fromX, fromY, x, y)
    for i = 1, 4 do
        moho64ui_MouseMove(fromX + (x - fromX) * i / 4, fromY + (y - fromY) * i / 4)
        WaitSeconds(0.03)
    end
end
local function release(x, y)
    moho64ui_MouseButton("Left", false, x, y)
    WaitSeconds(0.1)
end
local function click(c)
    local x, y = centre(c)
    press(x, y)
    release(x, y)
end
local function drag(fromX, fromY, x, y)
    press(fromX, fromY)
    moveTo(fromX, fromY, x, y)
    release(x, y)
end
local function describe(c) return c and moho64ui_Describe(c) or "nothing" end
local function texture(c) return string.gsub(describe(c), "^bitmap ", "") end
local function cursor() return (moho64ui_Cursor()) end

ForkThread(function()
    WaitSeconds(6)
    local mm = import("/lua/ui/game/minimap.lua")
    local win = mm.controls.displayGroup
    local mini = mm.controls.miniMap
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local frame = GetFrame(0)

    if case == "again" then
        -- the next game on the same preferences: where the last one left the window, shown at once, unlocked, the
        -- minimap's resources on
        check("the next game: the minimap shows from the start (the profile's stratview)", win:IsHidden(), false)
        check("the next game: the window where the last game left it", rect(win), "1180 160 1700 560")
        check("the next game: the minimap view inside it", rect(mini), "1193 194 1687 546")
        check("the next game: not pinned", win:IsPinned(), false)
        check("the next game: the minimap's resource icons on (the profile's MiniMap_resource_icons)", mini:IsResourceRenderingEnabled(), true)
        check("the next game: the main view's on", view:IsResourceRenderingEnabled(), true)
        LOG("TEST done: " .. failed .. " failed")
        return
    end

    -- the resource icons' switches before anything is touched
    check("a new world view draws the resource icons (the original's constructor, 0x86e544)", view:IsResourceRenderingEnabled(), true)
    check("FAF's minimap.lua turns the minimap's off (no MiniMap_resource_icons in the profile)", mini:IsResourceRenderingEnabled(), false)

    check("the minimap is hidden at first (the profile's stratview false)", win:IsHidden(), true)
    mm.ToggleMinimap()
    WaitSeconds(0.3)
    check("shown by its switch", win:IsHidden(), false)
    check("the profile says so (stratview)", Prefs.GetFromCurrentProfile('stratview'), true)
    -- (the preferences' copy has the window at 10 98, 1087 x 802)
    check("the window where the profile has it", rect(win), "10 98 1097 900")
    -- the minimap view: the client group (5 in from the sides, 30 below the top for the title, 8 up from the bottom:
    -- window.lua puts its bottom edge's top at the bottom corners' top) less the layout's insets, 8 left and right, 4
    -- above, 6 below (minimap_mini.lua)
    local function insideOk() return mini.Left() == win.Left() + 13 and mini.Top() == win.Top() + 34 and mini.Right() == win.Right() - 13 and mini.Bottom() == win.Bottom() - 14 end
    check("the minimap view inside it", insideOk(), true)

    -- 1. the title bar dragged: the window follows the pointer, its size kept
    local tx, ty = win.Left() + 543, win.Top() + 15
    check("the title bar is under the pointer there", moho64ui_ControlAt(tx, ty) == win.TitleGroup, true)
    press(tx, ty)
    moveTo(tx, ty, tx + 300, ty + 100)
    check("while dragged the pointer is the move cursor", cursor(), (UIUtil.GetCursor('MOVE_WINDOW')))
    release(tx + 300, ty + 100)
    check("the title bar dragged moves the window by as much", rect(win) .. size(win), "310 198 1397 1000 (1087 x 802)")
    check("the minimap view moves with it", insideOk(), true)
    check("and the place is saved in the profile", saved(), "310 198 1397 1000 (1087 x 802)")
    check("the pointer is back to the default cursor", cursor(), (UIUtil.GetCursor('DEFAULT')))
    -- beyond the screen's edge: it stops at the edge
    tx, ty = win.Left() + 543, win.Top() + 15
    drag(tx, ty, tx + 1500, ty + 900)
    check("dragged past the bottom right it stays inside the screen", rect(win), "833 278 1920 1080")

    -- 2. its corners and edges dragged: the edge goes to the pointer
    local function handle(name, x, y, toX, toY, want, what)
        local c = win[name]
        check(what .. ": the pointer is on its " .. name .. " handle", moho64ui_ControlAt(x, y) == c, true)
        press(x, y)
        moveTo(x, y, toX, toY)
        release(toX, toY)
        check(what, rect(win), want)
        check(what .. ": the minimap view follows", insideOk(), true)
    end
    -- (the bottom right corner first: over it the pointer takes the script's NW_SE cursor and the minimap's drag
    -- handle there lights up, pressed it shows pressed, let go back to up: minimap.lua's RolloverHandler)
    local bx, by = win.Right() - 3, win.Bottom() - 3
    moho64ui_MouseMove(bx - 40, by - 40)
    WaitSeconds(0.05)
    moho64ui_MouseMove(bx, by)
    WaitSeconds(0.05)
    check("over the bottom right corner the pointer is the resize cursor", cursor(), (UIUtil.GetCursor('NW_SE')))
    check("and the minimap's drag handle there lights up", texture(mini.DragBR), mini.DragBR.textures.over)
    moho64ui_MouseButton("Left", true, bx, by)
    WaitSeconds(0.05)
    check("pressed it shows pressed", texture(mini.DragBR), mini.DragBR.textures.down)
    moveTo(bx, by, 1500, 800)
    release(1500, 800)
    check("the bottom right corner dragged", rect(win), "833 278 1500 800")
    check("the bottom right corner dragged: the minimap view follows", insideOk(), true)
    check("let go, the drag handle is back to up", texture(mini.DragBR), mini.DragBR.textures.up)
    check("and the size is saved in the profile", saved(), "833 278 1500 800 (667 x 522)")
    handle("tl", win.Left() + 2, win.Top() + 2, 700, 200, "700 200 1500 800", "the top left corner dragged")
    handle("tr", win.Right() - 2, win.Top() + 2, 1600, 150, "700 150 1600 800", "the top right corner dragged")
    handle("bl", win.Left() + 2, win.Bottom() - 2, 650, 850, "650 150 1600 850", "the bottom left corner dragged")
    local mx, my = centre(win)
    handle("tm", mx, win.Top() + 2, mx + 50, 180, "650 180 1600 850", "the top edge dragged (up and down only)")
    -- (the bottom edge's handle is 5 high from the bottom corners' top, 8 up: the last 3 rows are the window's frame)
    handle("bm", mx, win.Bottom() - 6, mx - 50, 820, "650 180 1600 820", "the bottom edge dragged")
    handle("ml", win.Left() + 2, my, 680, my + 40, "680 180 1600 820", "the left edge dragged (sideways only)")
    handle("mr", win.Right() - 2, my, 1580, my - 40, "680 180 1580 820", "the right edge dragged")
    check("the last size saved in the profile", saved(), "680 180 1580 820 (900 x 640)")
    -- no smaller than the script's minimum (SetMinimumResize(150, 150))
    drag(win.Right() - 3, win.Bottom() - 3, win.Left() + 20, win.Top() + 20)
    check("dragged small it stops at 150 x 150", size(win), " (150 x 150)")
    drag(win.Right() - 3, win.Bottom() - 3, 1480, 760)
    check("and grows again", rect(win), "680 180 1480 760")
    WaitSeconds(0.2)

    -- 3. the minimap's picture at the new size: the map in its own proportion in the middle of the view (FAF's
    -- CameraReset after the window's first frame), where the view's Project puts the map's corners once drawn
    local map = SessionGetScenarioInfo().size
    local function mapRect()  -- the part of the view the map takes, from the view's top left corner (whole pixels)
        local w, h = mini.Right() - mini.Left(), mini.Bottom() - mini.Top()
        local pw = math.floor(math.min(w, h * map[1] / map[2]))
        local ph = math.floor(pw * map[2] / map[1])
        local x0, y0 = math.floor(mini.Left() + (w - pw) / 2) - mini.Left(), math.floor(mini.Top() + (h - ph) / 2) - mini.Top()
        return x0, y0, x0 + pw, y0 + ph
    end
    local function corners()
        local a = mini:Project(Vector(0, 20, 0))
        local b = mini:Project(Vector(map[1], 20, map[2]))
        return a.x, a.y, b.x, b.y
    end
    local function cornersOk()
        local x0, y0, x1, y1 = mapRect()
        local a, b, c, d = corners()
        LOG(string.format("TEST note: the minimap %s: the map's corners at %.1f %.1f, %.1f %.1f; its part of the view %d %d %d %d",
            rect(mini), a, b, c, d, x0, y0, x1, y1))
        return math.abs(a - x0) <= 1 and math.abs(b - y0) <= 1 and math.abs(c - x1) <= 1 and math.abs(d - y1) <= 1
    end
    local function drawn()  -- (a headless run draws nothing until asked: the screen drawn once, as a frame of a window)
        moho64ui_PixelAt(0, 0)
        WaitSeconds(0.05)
    end
    drawn()
    check("the minimap's picture at the new size: the map's corners where the view shows them", cornersOk(), true)
    do
        local x0, y0, x1, y1 = mapRect()
        local inside = moho64ui_PixelAt(mini.Left() + x0 + 3, mini.Top() + y0 + 3)
        check("the picture is there inside the map's part (opaque)", string.sub(inside or "", 1, 2), "ff")
    end

    -- 4. a left press on the minimap sends the main camera to the place under the pointer
    local cam = GetCamera("WorldCamera")
    local function lookAt(fx, fy, what)
        local x0, y0, x1, y1 = mapRect()
        local px, py = mini.Left() + x0 + (x1 - x0) * fx, mini.Top() + y0 + (y1 - y0) * fy
        local wx, wz = map[1] * fx, map[2] * fy
        press(px, py)
        local m = GetMouseWorldPos()  -- (the view under the pointer is the minimap: its place)
        release(px, py)
        WaitSeconds(0.1)
        local f = cam:GetFocusPosition()
        LOG(string.format("TEST note: %s: pressed at %.1f %.1f for %.1f %.1f: the place under it %.1f %.1f, the camera looks at %.1f %.1f",
            what, px, py, wx, wz, m[1], m[3], f[1], f[3]))
        check(what .. ": the place under the pointer", math.abs(m[1] - wx) <= 2 and math.abs(m[3] - wz) <= 2, true)
        -- (the camera at that place, its height too, as the original's TargetLocation: not moved along the view after)
        check(what, math.abs(f[1] - wx) <= 2 and math.abs(f[3] - wz) <= 2, true)
    end
    lookAt(0.3, 0.7, "after a move and resize, a press on the minimap sends the camera to the place under it")
    -- a move alone: the picture is the same, only its place on the screen changed
    tx, ty = win.Left() + 300, win.Top() + 15
    drag(tx, ty, tx - 400, ty + 200)
    check("moved again", rect(win), "280 380 1080 960")
    lookAt(0.65, 0.25, "after a move, not drawn since, the press still lands under the pointer")
    -- a resize not drawn yet at its new size
    drag(win.Right() - 3, win.Bottom() - 3, 880, 700)
    check("resized again", rect(win), "280 380 880 700")
    lookAt(0.2, 0.4, "after a resize, not drawn at its new size yet, the press still lands under the pointer")
    drawn()
    check("drawn at that size: the map's corners where the view shows them", cornersOk(), true)
    lookAt(0.8, 0.6, "and drawn, a press lands under the pointer")

    -- 5. the window's buttons
    -- the pin: place and size locked (window.lua's SetPositionLock / SetSizeLock), kept in the profile
    click(win._pinBtn)
    check("the pin locks the window", win:IsPinned(), true)
    check("the profile keeps the pin (minimapPin)", Prefs.GetFromCurrentProfile('minimapPin'), true)
    local before = rect(win)
    tx, ty = win.Left() + 200, win.Top() + 15
    drag(tx, ty, tx + 200, ty - 100)
    check("pinned, the title bar does not move it", rect(win), before)
    drag(win.Right() - 3, win.Bottom() - 3, win.Right() + 150, win.Bottom() + 100)
    check("pinned, a corner does not resize it", rect(win), before)
    click(win._pinBtn)
    check("the pin again unlocks it", win:IsPinned(), false)
    check("the profile says so", Prefs.GetFromCurrentProfile('minimapPin'), false)
    -- the reset button: where the script first had it (minimap.lua's defPosition 10 157 237 367, at the scale 1)
    click(win.resetBtn)
    check("the reset button puts it back where the script first had it", rect(win), "10 157 237 367")
    check("and saves that", saved(), "10 157 237 367 (227 x 210)")
    -- the close button: hidden, and the profile says so
    click(win._closeBtn)
    check("the close button hides it", win:IsHidden(), true)
    check("the profile says so (stratview false)", Prefs.GetFromCurrentProfile('stratview'), false)
    mm.ToggleMinimap()
    WaitSeconds(0.2)
    check("shown again, where it was", (not win:IsHidden()) and rect(win), "10 157 237 367")

    -- 6. the screen made smaller pulls the window inside it (window.lua: OnDirty of the frame's Width and Height)
    tx, ty = win.Left() + 100, win.Top() + 15
    drag(tx, ty, 1500, 700)
    check("dragged to the bottom right", rect(win), "1400 685 1627 895")
    moho64ui_Resize(1280, 720)
    WaitSeconds(0.3)
    check("on a smaller screen it is pulled inside", rect(win), "1053 510 1280 720")
    check("and that is saved", saved(), "1053 510 1280 720 (227 x 210)")
    moho64ui_Resize(1920, 1080)
    WaitSeconds(0.3)
    check("the screen back to its size: the window stays", rect(win), "1053 510 1280 720")

    -- 7. the minimap's resource icons from the map options dialog (the multifunction panel's button; the dialog's
    -- "Resources" item for the minimap)
    local MF = import("/lua/ui/game/multifunction.lua")
    local options = MF.GetButton('mapoptions')
    click(options)
    WaitSeconds(0.2)
    local dialog = options.Dropout
    check("the map options button opens its dialog", dialog ~= nil and dialog ~= false, true)
    local miniItems, mainItems
    for _, g in (dialog and dialog.mapControls) or {} do
        if g.title:GetText() == LOC(mini._displayName) then miniItems = g.toggles end
        if g.title:GetText() == LOC(view._displayName) then mainItems = g.toggles end
    end
    check("the dialog has the main view's items, its Resources ticked", mainItems and mainItems[2].checked, true)
    check("and the minimap's, its Resources not", miniItems and miniItems[2].checked, false)
    if miniItems then click(miniItems[2]) end
    WaitSeconds(0.1)
    check("ticked, the minimap draws the resource icons", mini:IsResourceRenderingEnabled(), true)
    check("and the profile keeps it (MiniMap_resource_icons)", Prefs.GetFromCurrentProfile('MiniMap_resource_icons'), true)
    check("the main view's stays on", view:IsResourceRenderingEnabled(), true)
    click(options)
    WaitSeconds(0.2)
    check("the button again closes the dialog", options.Dropout, false)

    -- 8. the window left moved and resized for the next game (and the screenshot)
    tx, ty = win.Left() + 100, win.Top() + 15
    drag(tx, ty, tx + 127, ty - 350)
    drag(win.Right() - 3, win.Bottom() - 3, 1700, 560)
    check("left moved and resized", rect(win), "1180 160 1700 560")
    check("saved so", saved(), "1180 160 1700 560 (520 x 400)")
    check("the minimap view inside it", rect(mini), "1193 194 1687 546")
    moho64ui_MouseMove(900, 700)
    WaitSeconds(0.2)
    LOG(string.format("TEST note: done at %.1f s of the run's clock (%d frames)", CurrentTime(), CurrentTime() * 60))
    LOG("TEST done: " .. failed .. " failed")
end)
