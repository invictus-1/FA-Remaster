-- The original's second head (docs/ui-dualscreen.md; Chris's approval of 2026-11-01): a root frame per head, each at
-- its own size from 0,0 (FA exe 0x84cc50), the mouse by head (UiEvent::head, maui/input.cpp), FAF's multihead.lua on
-- frame 1 (ShowLogoInHead1 in the menus, CreateSecondView in a game: a world view of the camera CameraHead2), the
-- world cover on every frame (uiutil.CreateWorldCover). Headless: the second head is made by MOHO64UI_HEADS=2 and
-- MOHO64UI_HEAD1_SIZE=1280x720 (no window, so no renderer's second window: its world view has no picture and its input
-- goes by the plain view from above). In a window (case "window") the renderer's second window shows it: its world
-- view is a view of the picture (window 1) and is picked by perspective. The case after "--" (/dualcase):
--   front: the front end, two heads
--     MOHO64UI_HEADS=2 MOHO64UI_HEAD1_SIZE=1280x720 moho64ui --init <init.lua> --prefs <a copy> --stage frontend
--              --size 1920x1080 --quiet --lua ui/tests/dualhead.lua --frames 400 --log out.log -- /dualcase front
--   game:  a game, two heads (the same switches)
--     ... --stage setup --size 1920x1080 --quiet --session <session_ai_cheats.lua> --seed 7
--              --game-lua ui/tests/dualhead.lua --frames 2400 --log out.log -- /dualcase game
--   one:   a game without the switches: one head, as before
--     moho64ui ... (as game, no MOHO64UI_HEADS) -- /dualcase one
--   window: a game in a live window with the test window for head 1 (the renderer's second window; pinned, ~1 min)
--     MOHO64UI_SECOND_WINDOW=960x540 moho64ui ... --window --size 1280x720 --stage setup --session <session_ai_cheats.lua>
--              --seed 7 --game-lua ui/tests/dualhead.lua --frames 3000 --log out.log -- /dualcase window
--     (MOHO64UI_SECOND_CAPTURE=<png> MOHO64UI_CAPTURE_FRAME=<n>: window 2's picture read back by the renderer;
--     MOHO64UI_OVERLAY_LOG=1: the overlay's frame lines, with "window 2: N a second", in the log)
-- (the world's picture is needed for the game cases: not with --no-world)
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
local function near(a, b, within) return a ~= nil and b ~= nil and math.abs(a - b) <= within end
local function WaitFor(cond, frames)
    for i = 1, frames do
        if cond() then return true end
        WaitFrames(1)
    end
    return cond() and true or false
end
local function describe(c)
    return c and moho64ui_Describe(c) or "nil"
end

local case = (GetCommandLineArg("/dualcase", 1) or {})[1]
local inGame = GetCurrentUIState() == 'game'

-- A group on a frame, at the top left, over everything there, that writes down the mouse events it hears
local function Listener(frame, name, heard)
    local Group = import('/lua/maui/group.lua').Group
    local g = Group(frame, name)
    g.Left:Set(0)
    g.Top:Set(0)
    g.Width:Set(60)
    g.Height:Set(60)
    g.Depth:Set(frame:GetTopmostDepth() + 100)
    g.HandleEvent = function(self, event)
        if event.Type ~= 'MouseMotion' then table.insert(heard, name .. ":" .. event.Type) end
        return true
    end
    return g
end

local function FrontEnd()
    check("two root frames", GetNumRootFrames(), 2)
    check("the display says two heads, the second 1280x720", moho64ui_Looks().heads .. " " .. moho64ui_Looks().secondWidth .. "x" .. moho64ui_Looks().secondHeight, "2 1280x720")
    local f0, f1 = GetFrame(0), GetFrame(1)
    check("frame 0 is head 0's size from 0,0", f0.Left() .. "," .. f0.Top() .. " " .. f0.Width() .. "x" .. f0.Height(), "0,0 1920x1080")
    check("frame 1 is head 1's own size from 0,0", f1.Left() .. "," .. f1.Top() .. " " .. f1.Width() .. "x" .. f1.Height(), "0,0 1280x720")
    check("two frames, two objects", f0 ~= f1, true)
    check("each frame's target head is 0 (INFERRED: dialogs open on head 0)", f0:GetTargetHead() .. " " .. f1:GetTargetHead(), "0 0")
    -- FAF's ShowLogoInHead1 (uimain, after the main menu): the splash on frame 1
    local logoUp = WaitFor(function() return string.find(describe(moho64ui_ControlAt(640, 360, 1)), "splash", 1, true) ~= nil end, 120)
    check("FAF's logo is on frame 1 (ShowLogoInHead1)", logoUp, true)
    LOG("TEST note: frame 1's middle: " .. describe(moho64ui_ControlAt(640, 360, 1)))
    check("and drawn there", (moho64ui_PixelAt(640, 360, 1) or "00000000") ~= "00000000", true)
    check("frame 0's middle is not frame 1's logo", string.find(describe(moho64ui_ControlAt(640, 360)), "splash", 1, true) == nil, true)
    check("head 1 has no pixels past its size", moho64ui_PixelAt(1500, 360, 1), nil)

    -- the mouse by head: each head's own control under it; the hover crosses from head 0 to head 1
    local heard = {}
    local a = Listener(f0, "A", heard)
    local b = Listener(f1, "B", heard)
    check("the control at 30,30 of head 0 is head 0's", moho64ui_ControlAt(30, 30, 0) == a, true)
    check("and at 30,30 of head 1 head 1's", moho64ui_ControlAt(30, 30, 1) == b, true)
    moho64ui_MouseMove(30, 30, 0)
    moho64ui_MouseMove(30, 30, 1)
    check("over from head 0 to head 1: A's MouseExit, then B's MouseEnter", table.concat(heard, " "), "A:MouseEnter A:MouseExit B:MouseEnter")
    heard = {}
    a.HandleEvent = function(self, event) if event.Type ~= 'MouseMotion' then table.insert(heard, "A:" .. event.Type) end return true end
    b.HandleEvent = function(self, event) if event.Type ~= 'MouseMotion' then table.insert(heard, "B:" .. event.Type) end return true end
    moho64ui_MouseButton("Left", true, 30, 30, "", 1)
    moho64ui_MouseButton("Left", false, 30, 30, "", 1)
    check("a click on head 1 goes to head 1's control", table.concat(heard, " "), "B:ButtonPress B:ButtonRelease")
    heard = {}
    moho64ui_MouseWheel(30, 30, 1, 1)
    check("so does the wheel", table.concat(heard, " "), "B:WheelRotation")
    check("the mouse's place is head 1's", GetMouseScreenPos()[1] .. "," .. GetMouseScreenPos()[2], "30,30")
    -- the input capture is one for every head (FA exe 0x7a4a77): head 0's capturing control hears head 1's click
    heard = {}
    AddInputCapture(a)
    moho64ui_MouseButton("Left", true, 600, 600, "", 1)
    moho64ui_MouseButton("Left", false, 600, 600, "", 1)
    RemoveInputCapture(a)
    check("with head 0's control capturing, head 1's click is its (the hover with it)", table.concat(heard, " "), "B:MouseExit A:MouseEnter A:ButtonPress A:ButtonRelease")
    moho64ui_MouseMove(900, 900, 0)
    a:Destroy()
    b:Destroy()

    -- a head's resize: its frame alone, and uimain.OnApplicationResize(head, w, h) (IUIManager::OnResize 0x84cfc0)
    local UIMain = import('/lua/ui/uimain.lua')
    local had = UIMain.OnApplicationResize
    local resized = {}
    UIMain.OnApplicationResize = function(head, w, h) table.insert(resized, head .. ":" .. w .. "x" .. h) end
    moho64ui_Resize(1024, 600, 1)
    check("head 1 resized: its frame", f1.Width() .. "x" .. f1.Height(), "1024x600")
    check("and not head 0's", f0.Width() .. "x" .. f0.Height(), "1920x1080")
    check("OnApplicationResize heard it with its head", table.concat(resized, " "), "1:1024x600")
    moho64ui_Resize(1280, 720, 1)
    UIMain.OnApplicationResize = had

    -- ClearFrame(1): frame 1's controls go, frame 0's stay
    local before = moho64ui_ControlAt(960, 540, 0)
    ClearFrame(1)
    WaitFrames(1)
    check("ClearFrame(1) empties frame 1", moho64ui_ControlAt(640, 360, 1) == f1, true)
    check("and leaves frame 0 as it was", moho64ui_ControlAt(960, 540, 0) == before and before ~= f0, true)
end

local function Game()
    WaitSeconds(5)
    local MH = import("/lua/ui/game/multihead.lua")
    local WV = import("/lua/ui/game/worldview.lua")
    local UIUtil = import("/lua/ui/uiutil.lua")
    local cdr = GetArmyAvatars()[1]
    local pos = cdr:GetPosition()
    check("two root frames in the game too", GetNumRootFrames(), 2)
    local view = MH.view
    check("CreateSecondView made a world view on frame 1", view and view:GetRootFrame() == GetFrame(1), true)
    check("of the camera CameraHead2", view and view._cameraName, "CameraHead2")
    check("filling frame 1", view and (view.Left() .. "," .. view.Top() .. " " .. view.Width() .. "x" .. view.Height()), "0,0 1280x720")
    check("registered in FAF's world view manager", WV.MapControls['CameraHead2'] == view, true)
    check("the main view stays on frame 0", WV.viewLeft and WV.viewLeft:GetRootFrame() == GetFrame(0), true)
    local views = moho64ui_Views()
    local main, second
    for _, v in views do
        if v.window == 0 and v.kind == "main" then main = v end
        if v.window == 1 then second = v end
    end
    check("the views: the main one and head 1's (window 1)", table.getn(views) .. " " .. tostring(main ~= nil) .. " " .. tostring(second ~= nil), "2 true true")
    check("head 1's view at its place in head 1's pixels", second and (second.x .. "," .. second.y .. " " .. second.width .. "x" .. second.height .. " " .. second.kind), "0,0 1280x720 split")
    check("the picture's view at the main window's middle is still 0", moho64ui_ViewAt(960, 540), 0)
    check("head 1's view is not given to the picture (no window: no renderer's second window)", second and second.given, false)
    local number, on = moho64ui_ViewNumber(view)
    check("and has no picture", tostring(number) .. " " .. tostring(on), "-1 false")

    local cam, cam2 = GetCamera('WorldCamera'), GetCamera('CameraHead2')
    check("CameraHead2 is a camera", cam2 ~= nil, true)
    ConExecute("cam_SetLOD CameraHead2 0.5")
    check("cam_SetLOD CameraHead2 reaches it", moho64ui_CameraLodScale('CameraHead2'), 0.5)
    check("and not WorldCamera", moho64ui_CameraLodScale('WorldCamera') ~= 0.5, true)
    ConExecute("cam_SetLOD CameraHead2 1")

    -- UI_TrackUnit CameraHead2 (FAF's track_unit_second_mon): head 1's camera follows the selection, the main one stays
    cam:SnapTo({pos[1] + 150, pos[2], pos[3] + 150}, {3.14159, 1.1, 0}, 70)
    cam2:SnapTo({pos[1] - 120, pos[2], pos[3] - 120}, {3.14159, 1.1, 0}, 70)
    WaitFrames(3)
    local mainAt = cam:GetFocusPosition()
    SelectUnits({cdr})
    ConExecute("UI_TrackUnit CameraHead2")
    WaitFrames(5)
    local f2 = cam2:GetFocusPosition()
    check("UI_TrackUnit CameraHead2: head 1's camera on the selected commander", near(f2[1], pos[1], 3) and near(f2[3], pos[3], 3), true)
    check("and the main camera where it was", near(cam:GetFocusPosition()[1], mainAt[1], 0.5) and near(cam:GetFocusPosition()[3], mainAt[3], 0.5), true)
    ConExecute("UI_TrackUnit CameraHead2")  -- (the same unit: it lets go)
    SelectUnits(nil)
    ConExecute("UI_TrackUnit WorldCamera")  -- (nothing selected: nothing followed)
    check("UI_TrackUnit with nothing selected leaves the main camera", near(cam:GetFocusPosition()[1], mainAt[1], 0.5), true)

    -- input on head 1: CameraHead2 on the commander, the main camera elsewhere
    cam2:SnapTo(pos, {3.14159, 1.1, 0}, 70)
    WaitFrames(3)
    local s = view:Project(pos)
    local sx, sy = math.floor(s.x), math.floor(s.y)
    LOG(string.format("TEST note: the commander on head 1 at %d,%d", sx, sy))
    check("the commander projects into head 1's view", sx > 0 and sx < 1280 and sy > 0 and sy < 720, true)
    check("head 1's control there is its world view", moho64ui_ControlAt(sx, sy, 1) == view, true)
    check("head 0's control at the same numbers is not it", moho64ui_ControlAt(sx, sy, 0) ~= view, true)
    SelectUnits(nil)
    moho64ui_MouseMove(sx, sy, 1)
    moho64ui_MouseButton("Left", true, sx, sy, "", 1)
    moho64ui_MouseButton("Left", false, sx, sy, "", 1)
    local sel = GetSelectedUnits()
    check("a left click on the commander on head 1 selects him", sel and table.getn(sel) == 1 and sel[1] == cdr, true)
    local w = GetMouseWorldPos()
    check("the place under the mouse on head 1 is his", near(w[1], pos[1], 2) and near(w[3], pos[3], 2), true)
    local gx, gy = sx + 60, sy
    local ground = UnProject(view, {gx, gy})
    moho64ui_MouseMove(gx, gy, 1)
    moho64ui_MouseButton("Right", true, gx, gy, "", 1)
    moho64ui_MouseButton("Right", false, gx, gy, "", 1)
    WaitSeconds(0.5)
    local queue = cdr:GetCommandQueue() or {}
    local order = queue[1]
    check("a right click on head 1 gives a move", order and order.type, "Move")
    check("to the ground under the click on head 1", order and order.position and near(order.position[1], ground[1], 1) and near(order.position[3], ground[3], 1), true)
    check("which is not under the same point of head 0 (its camera looks elsewhere)", order and order.position and not near(order.position[1], mainAt[1], 40), true)
    IssueUnitCommand({cdr}, "Stop")
    SelectUnits(nil)
    s = view:Project(cdr:GetPosition())
    sx, sy = math.floor(s.x), math.floor(s.y)
    moho64ui_MouseMove(sx - 40, sy - 40, 1)
    moho64ui_MouseButton("Left", true, sx - 40, sy - 40, "", 1)
    moho64ui_MouseMove(sx + 40, sy + 40, 1)
    moho64ui_MouseButton("Left", false, sx + 40, sy + 40, "", 1)
    sel = GetSelectedUnits()
    check("a box round him on head 1 selects him", sel and sel[1] == cdr, true)
    -- a key while the mouse is on head 1: the key map's (keys are global)
    moho64ui_MouseMove(640, 360, 1)
    moho64ui_Key(315)  -- Home: FAF's split screen
    WaitFrames(2)
    check("a key with the mouse on head 1 runs its action (Home splits head 0)", WV.viewRight ~= false and WV.viewRight ~= nil, true)
    check("the split views stay head 0's, head 1's view stays", WV.viewRight and WV.viewRight:GetRootFrame() == GetFrame(0) and MH.view == view and not IsDestroyed(view), true)
    moho64ui_Key(314)  -- End
    WaitFrames(2)
    check("End unsplits", WV.viewRight, false)
    cam = GetCamera('WorldCamera')
    -- the screen's edge pans the main view only while the mouse is on head 0
    cam:SnapTo(pos, {3.14159, 1.1, 0}, 90)
    WaitFrames(2)
    moho64ui_EdgeScroll(true)
    local m0 = cam:GetFocusPosition()
    local h0 = cam2:GetFocusPosition()
    moho64ui_MouseMove(0, 360, 1)
    WaitFrames(20)
    local m1 = cam:GetFocusPosition()
    check("the mouse at head 1's left edge does not pan the main view", near(m1[1], m0[1], 0.01) and near(m1[3], m0[3], 0.01), true)
    check("nor head 1's", near(cam2:GetFocusPosition()[1], h0[1], 0.01) and near(cam2:GetFocusPosition()[3], h0[3], 0.01), true)
    moho64ui_MouseMove(0, 540, 0)
    WaitFrames(20)
    local m2 = cam:GetFocusPosition()
    check("at head 0's left edge it does (the switch works)", not (near(m2[1], m1[1], 0.5) and near(m2[3], m1[3], 0.5)), true)
    moho64ui_MouseMove(960, 540, 0)
    moho64ui_EdgeScroll(false)

    -- the world cover (FAF's CreateWorldCover, under every dialog): one on each frame
    local Group = import('/lua/maui/group.lua').Group
    local dialog = Group(GetFrame(0), "dualhead dialog")
    dialog.Left:Set(800)
    dialog.Top:Set(400)
    dialog.Width:Set(300)
    dialog.Height:Set(200)
    dialog.Depth:Set(GetFrame(0):GetTopmostDepth() + 10)
    local covers = UIUtil.CreateWorldCover(dialog)
    check("the world cover covers both frames", covers[0] ~= nil and covers[1] ~= nil and covers[1]:GetRootFrame() == GetFrame(1), true)
    check("over head 1's world view", moho64ui_ControlAt(640, 360, 1) == covers[1], true)
    check("the dialog itself is head 0's", dialog:GetRootFrame() == GetFrame(0), true)
    dialog:Destroy()
    WaitFrames(1)
    check("the dialog gone: its covers too", moho64ui_ControlAt(640, 360, 1) == view, true)
end

-- In a live window, the renderer's second window on (MOHO64UI_SECOND_WINDOW=960x540, --size 1280x720)
local function WindowGame()
    WaitSeconds(5)
    local MH = import("/lua/ui/game/multihead.lua")
    local cdr = GetArmyAvatars()[1]
    local pos = cdr:GetPosition()
    ConExecute("remaster_stats on")  -- (the overlay's frame line: how often window 2 is drawn; MOHO64UI_OVERLAY_LOG logs it)
    check("two heads, the second the test window's 960x540", moho64ui_Looks().heads .. " " .. moho64ui_Looks().secondWidth .. "x" .. moho64ui_Looks().secondHeight, "2 960x540")
    local view = MH.view
    check("CreateSecondView made a world view on frame 1", view and view:GetRootFrame() == GetFrame(1), true)
    check("filling frame 1", view and (view.Left() .. "," .. view.Top() .. " " .. view.Width() .. "x" .. view.Height()), "0,0 960x540")
    WaitFrames(5)
    local number, on = moho64ui_ViewNumber(view)
    check("the picture's second window is on", on, true)
    check("head 1's view is a view of the picture (it has a picture, a number after the main view's)", number ~= nil and number > 0, true)
    LOG("TEST note: head 1's view is the picture's view " .. tostring(number))
    local second
    for _, v in moho64ui_Views() do
        if v.window == 1 then second = v end
    end
    check("given to the renderer with window 1, a full view at its place in head 1's pixels",
        second and (tostring(second.given) .. " " .. second.x .. "," .. second.y .. " " .. second.width .. "x" .. second.height .. " " .. second.kind), "true 0,0 960x540 split")
    check("the main window's middle is still the main view", moho64ui_ViewAt(640, 360), 0)
    check("the main view is view 0", moho64ui_ViewNumber(import("/lua/ui/game/worldview.lua").viewLeft), 0)

    -- picking on head 1 by its perspective picture: CameraHead2 on the commander, the main camera elsewhere
    local cam, cam2 = GetCamera('WorldCamera'), GetCamera('CameraHead2')
    cam:SnapTo({pos[1] + 150, pos[2], pos[3] + 150}, {3.14159, 1.1, 0}, 70)
    cam2:SnapTo(pos, {3.14159, 1.0, 0}, 60)
    WaitFrames(10)
    -- (a picture in perspective: the upper half of the view covers more ground than the lower; from above it is even)
    local top, mid, bottom = UnProject(view, {480, 60}), UnProject(view, {480, 270}), UnProject(view, {480, 480})
    local far, near_ = math.abs(top[3] - mid[3]), math.abs(mid[3] - bottom[3])
    LOG(string.format("TEST note: head 1's ground: upper half %.1f, lower half %.1f map units deep", far, near_))
    check("head 1's ground is picked in perspective (its upper half deeper than its lower)", near_ > 0 and far / near_ > 1.15, true)
    local s = view:Project(pos)
    local sx, sy = math.floor(s.x), math.floor(s.y)
    LOG(string.format("TEST note: the commander on head 1 at %d,%d", sx, sy))
    check("the commander projects into head 1's view", sx > 0 and sx < 960 and sy > 0 and sy < 540, true)
    SelectUnits(nil)
    moho64ui_MouseMove(sx, sy, 1)
    WaitFrames(2)
    moho64ui_MouseButton("Left", true, sx, sy, "", 1)
    moho64ui_MouseButton("Left", false, sx, sy, "", 1)
    local sel = GetSelectedUnits()
    check("a left click on the commander on head 1 selects him (the picture's pick)", sel and table.getn(sel) == 1 and sel[1] == cdr, true)
    local w = GetMouseWorldPos()
    check("the place under the mouse on head 1 is his", near(w[1], pos[1], 3) and near(w[3], pos[3], 3), true)
    local gx, gy = sx + 80, sy - 60
    local ground = UnProject(view, {gx, gy})
    moho64ui_MouseMove(gx, gy, 1)
    WaitFrames(2)
    moho64ui_MouseButton("Right", true, gx, gy, "", 1)
    moho64ui_MouseButton("Right", false, gx, gy, "", 1)
    WaitSeconds(0.5)
    local order = (cdr:GetCommandQueue() or {})[1]
    check("a right click on head 1 gives a move", order and order.type, "Move")
    check("to the ground under the click by head 1's picture", order and order.position and near(order.position[1], ground[1], 1.5) and near(order.position[3], ground[3], 1.5), true)
    IssueUnitCommand({cdr}, "Stop")
    SelectUnits(nil)
    WaitFrames(5)
    s = view:Project(cdr:GetPosition())
    sx, sy = math.floor(s.x), math.floor(s.y)
    moho64ui_MouseMove(sx - 40, sy - 40, 1)
    moho64ui_MouseButton("Left", true, sx - 40, sy - 40, "", 1)
    moho64ui_MouseMove(sx + 40, sy + 40, 1)
    moho64ui_MouseButton("Left", false, sx + 40, sy + 40, "", 1)
    sel = GetSelectedUnits()
    check("a box round him on head 1 selects him (the picture's units in a rectangle)", sel and sel[1] == cdr, true)
    SelectUnits(nil)
    moho64ui_MouseMove(640, 360, 0)
    -- a resize of window 2's head: its view follows, still the picture's
    moho64ui_Resize(800, 450, 1)
    WaitFrames(10)
    second = nil
    for _, v in moho64ui_Views() do
        if v.window == 1 then second = v end
    end
    check("head 1 resized: its view given at the new size", second and (second.width .. "x" .. second.height .. " " .. tostring(second.given)), "800x450 true")
    moho64ui_Resize(960, 540, 1)
    WaitFrames(10)
end

local function OneHead()
    WaitSeconds(5)
    local MH = import("/lua/ui/game/multihead.lua")
    check("one head: one root frame", GetNumRootFrames(), 1)
    check("the display says one head", moho64ui_Looks().heads, 1)
    check("CreateSecondView did nothing", MH.view, false)
    check("one world view: no views given (as before)", table.getn(moho64ui_Views()), 0)
    check("GetFrame(1) is nothing", GetFrame(1), nil)
end

-- (window, windowone: in a live window; after the checks 15 s of the game for the log's frame numbers, both heads'
-- cameras still, then the program ends itself: windowone, one head, is the numbers' comparison)
local windowed = case == "window" or case == "windowone"
check("a case is named (-- /dualcase front|game|one|window|windowone)", case == "front" or case == "game" or case == "one" or windowed, true)
check("the case's state (front end or game)", inGame, case ~= "front")
ForkThread(function()
    if case == "front" then FrontEnd()
    elseif case == "game" then Game()
    elseif case == "one" or case == "windowone" then OneHead()
    elseif case == "window" then WindowGame() end
    if windowed then
        ConExecute("remaster_stats on")
        WaitSeconds(15)
    end
    LOG("TEST done: " .. failed .. " failed")
    if windowed then ExitApplication() end
end)
