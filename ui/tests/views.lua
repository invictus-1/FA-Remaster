-- Several views of the world in one picture (the renderer's WorldPicture::SetViews, render request 35; the world view's
-- side maui/worldview.cpp GatherViews, the follow camera's frame ui/data/lua/followcam.lua):
-- 1. the original's split screen as FAF's scripts make it (the game's key action split_screen_enable on Home:
--    borders.SplitMapGroup(true) makes a second world view, viewRight, of the camera "WorldCamera2"): two views given
--    at the two world views' rectangles, the right one's camera moved by the middle button's drag and zoomed by the
--    wheel over it (the left one's camera stays), a unit picked by a left click in the right half (the picture's view
--    there is 1), the place under the mouse and a right click's move order in the right view, then End unsplits (one
--    view again: the picture as it always was);
-- 2. the follow camera (ours): its key (Ctrl-Alt-V) with nothing selected says so and opens nothing; on the selected
--    commander it opens an inset view of 480 x 270 at the bottom right, behind the commander; a left click on it jumps
--    the main camera to him; a right click closes it; the key twice opens and closes; on a tank that is destroyed it
--    stays a while and closes by itself; with the split screen on, three views;
-- 3. the key action's names: the key map's, the console's (remaster_follow_cam), the command palette's.
--   moho64ui --init <init.lua> --prefs <a copy> --stage setup --size 1920x1080 --session <session_ai_cheats.lua> --seed 7
--            --game-lua ui/tests/views.lua --frames 4000 --log out.log
-- (the world's picture is needed: not with --no-world)
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
local function kinds(views)
    local out = {}
    for _, v in views do table.insert(out, v.kind) end
    return table.concat(out, " ")
end
local function WaitFor(cond, frames)
    for i = 1, frames do
        if cond() then return true end
        WaitFrames(1)
    end
    return cond() and true or false
end

ForkThread(function()
    WaitSeconds(5)
    local WV = import("/lua/ui/game/worldview.lua")
    local FC = import("/moho64ui/lua/followcam.lua")
    local KeyMapper = import("/lua/keymap/keymapper.lua")
    local cdr = GetArmyAvatars()[1]
    local pos = cdr:GetPosition()
    local cam = GetCamera('WorldCamera')
    check("one world view: no views given (the picture as it always was)", table.getn(moho64ui_Views()), 0)

    -- 1. the split screen
    check("Home is the game's split_screen_enable", string.find(moho64ui_KeyMapAction("Home") or "", "SplitMapGroup(true)", 1, true) ~= nil, true)
    local before = cam:GetFocusPosition()
    moho64ui_Key(315)  -- Home
    WaitFrames(2)
    check("split: the old main view's camera went with it (a new view, a new camera)", IsDestroyed(cam), true)
    cam = GetCamera('WorldCamera')
    check("and the new one has the old one's place (CreateMainWorldView's RestoreSettings)", near(cam:GetFocusPosition()[1], before[1], 0.5) and near(cam:GetFocusPosition()[3], before[3], 0.5), true)
    check("split: FAF's viewRight is there, of the camera WorldCamera2", WV.viewRight and WV.viewRight._cameraName, "WorldCamera2")
    local views = moho64ui_Views()
    check("split: two views, the main one and a split one", kinds(views), "main split")
    local L, R = WV.viewLeft, WV.viewRight
    check("the left view's place is the left world view's", views[1] and views[1].x == math.floor(L.Left()) and views[1].width == math.floor(L.Right() - L.Left()), true)
    check("the right view's place is the right world view's", views[2] and views[2].x == math.floor(R.Left()) and views[2].width == math.floor(R.Right() - R.Left()), true)
    check("the two halves, the right one from the screen's middle", views[2] and views[2].x >= 960 and views[1].width < 960, true)
    local rx, ry = math.floor((R.Left() + R.Right()) / 2), math.floor((R.Top() + R.Bottom()) / 2)
    check("the picture's view at the right half's middle is 1", moho64ui_ViewAt(rx, ry), 1)
    check("and at the left half's middle 0", moho64ui_ViewAt(math.floor((L.Left() + L.Right()) / 2), ry), 0)
    -- the right view's camera moves as the left's: the middle button's drag, the wheel
    local cam2 = GetCamera('WorldCamera2')
    local f0, m0 = cam2:GetFocusPosition(), cam:GetFocusPosition()
    moho64ui_MouseMove(rx, ry)
    moho64ui_MouseButton("Middle", true, rx, ry)
    moho64ui_MouseMove(rx - 100, ry)
    moho64ui_MouseButton("Middle", false, rx - 100, ry)
    local f1 = cam2:GetFocusPosition()
    check("a middle-button drag in the right view moves its camera", math.abs(f1[1] - f0[1]) > 1, true)
    check("and not the left view's", near(cam:GetFocusPosition()[1], m0[1], 0.01) and near(cam:GetFocusPosition()[3], m0[3], 0.01), true)
    local z2, z1 = cam2:GetTargetZoom(), cam:GetTargetZoom()
    moho64ui_MouseWheel(rx, ry, 1)
    check("the wheel over the right view zooms its camera", cam2:GetTargetZoom() < z2, true)
    check("and not the left view's", cam:GetTargetZoom(), z1)
    -- the right camera on the commander, the left one elsewhere
    cam2:SnapTo(pos, {3.14159, 1.1, 0}, 70)
    cam:SnapTo({pos[1] + 150, pos[2], pos[3] + 150}, {3.14159, 1.1, 0}, 70)
    WaitFrames(3)
    views = moho64ui_Views()
    check("the views follow their cameras: the right one's looks at the commander",
        views[2] and near(views[2].camera.target[1], pos[1], 3) and near(views[2].camera.target[3], pos[3], 3), true)
    local s = R:Project(pos)
    local sx, sy = R.Left() + s.x, R.Top() + s.y
    check("the commander projects into the right view", s.x > 0 and s.x < R.Width() and s.y > 0 and s.y < R.Height(), true)
    check("and not into the left one (its camera looks elsewhere)", (function()
        local t = L:Project(pos)
        return t.x < 0 or t.x > L.Width() or t.y < 0 or t.y > L.Height()
    end)(), true)
    check("the picture's view there is 1", moho64ui_ViewAt(sx, sy), 1)
    SelectUnits(nil)
    moho64ui_MouseMove(sx, sy)
    moho64ui_MouseButton("Left", true, sx, sy)
    moho64ui_MouseButton("Left", false, sx, sy)
    local sel = GetSelectedUnits()
    check("a left click on the commander in the right view selects him", sel and table.getn(sel) == 1 and sel[1] == cdr, true)
    local w = GetMouseWorldPos()
    check("the place under the mouse in the right view is his", near(w[1], pos[1], 2) and near(w[3], pos[3], 2), true)
    -- a right click on the ground in the right view: a move there
    local gx, gy = sx + 60, sy
    local ground = UnProject(R, {gx, gy})  -- (a point of the screen, as FAF's reclaim.lua gives it)
    moho64ui_MouseMove(gx, gy)
    moho64ui_MouseButton("Right", true, gx, gy)
    moho64ui_MouseButton("Right", false, gx, gy)
    WaitSeconds(0.5)
    local queue = cdr:GetCommandQueue() or {}
    local order = queue[1]
    check("a right click in the right view gives a move", order and order.type, "Move")
    check("to the place under the click there", order and order.position and near(order.position[1], ground[1], 1) and near(order.position[3], ground[3], 1), true)
    LOG(string.format("TEST note: the move goes to %.1f %.1f, the ground under the click %.1f %.1f",
        order and order.position and order.position[1] or -1, order and order.position and order.position[3] or -1, ground[1], ground[3]))
    IssueUnitCommand({cdr}, "Stop")
    -- a box in the right view selects what is in it
    SelectUnits(nil)
    s = R:Project(cdr:GetPosition())
    sx, sy = R.Left() + s.x, R.Top() + s.y
    moho64ui_MouseMove(sx - 40, sy - 40)
    moho64ui_MouseButton("Left", true, sx - 40, sy - 40)
    moho64ui_MouseMove(sx + 40, sy + 40)
    moho64ui_MouseButton("Left", false, sx + 40, sy + 40)
    sel = GetSelectedUnits()
    check("a box round him in the right view selects him", sel and sel[1] == cdr, true)
    -- End unsplits: one view again
    moho64ui_Key(314)  -- End
    WaitFrames(2)
    check("End: viewRight is gone", WV.viewRight, false)
    check("unsplit: one view again (no views given)", table.getn(moho64ui_Views()), 0)
    -- split again: the right view's camera is a new one (the original's view makes its own and deletes it), the
    -- left keeps its place (CreateMainWorldView's SaveSettings / RestoreSettings)
    check("End made the main view anew too: its old camera is gone", IsDestroyed(cam), true)
    cam = GetCamera('WorldCamera')
    local mainFocus = cam:GetFocusPosition()
    moho64ui_Key(315)  -- Home
    WaitFrames(2)
    cam2 = GetCamera('WorldCamera2')
    local f2 = cam2:GetFocusPosition()
    check("split again: the right camera starts afresh, not on the commander", not (near(f2[1], pos[1], 3) and near(f2[3], pos[3], 3)), true)
    check("and the left camera keeps its place", near(GetCamera('WorldCamera'):GetFocusPosition()[1], mainFocus[1], 0.5) and
        near(GetCamera('WorldCamera'):GetFocusPosition()[3], mainFocus[3], 0.5), true)
    moho64ui_Key(314)  -- End
    WaitFrames(2)
    cam = GetCamera('WorldCamera')
    cam:SnapTo(pos, {3.14159, 1.2, 0}, 90)
    WaitFrames(2)

    -- 2. the follow camera
    SelectUnits(nil)
    moho64ui_Key("v", "Ctrl Alt")
    check("the key with nothing selected opens nothing", FC.State.open, false)
    check("and says so", FC.State.said, "Select a unit to follow first")
    SelectUnits({cdr})
    moho64ui_Key("v", "Ctrl Alt")
    check("the key with the commander selected opens it", FC.State.open, true)
    WaitFrames(3)
    local st = moho64ui_FollowCamState()
    check("it follows the commander", tostring(st.on) .. " " .. tostring(st.unit), "true " .. cdr:GetEntityId())
    views = moho64ui_Views()
    check("two views: the main one and an inset", kinds(views), "main inset")
    check("the inset is the picture's view 1", st.view, 1)
    local v = views[2] or {}
    check("480 x 270 at the interface's scale 1", tostring(v.width) .. "x" .. tostring(v.height), "480x270")
    check("at the bottom right of the screen", v.x and v.x + v.width <= 1920 and v.x + v.width > 1920 - 60 and v.y + v.height <= 1080 and v.y + v.height > 700, true)
    check("its place is the frame's picture", FC.State.rect and FC.State.rect[1] == v.x and FC.State.rect[2] == v.y, true)
    check("the name in its title is the commander's", FC.State.name ~= "" and FC.State.name ~= nil, true)
    LOG("TEST note: the follow camera's title: " .. tostring(FC.State.name) .. string.format("; its picture at %d %d", v.x or -1, v.y or -1))
    local c = v.camera or { target = {0, 0, 0} }
    local p = cdr:GetPosition()
    check("its camera looks at the commander", near(c.target[1], p[1], 1) and near(c.target[3], p[3], 1), true)
    check("from close by and low (not from the strategic view)", c.distance < 60 and c.pitch < 0.8, true)
    local ix, iy = v.x + math.floor(v.width / 2), v.y + math.floor(v.height / 2)
    check("the picture's view at its middle is the inset (insets first)", moho64ui_ViewAt(ix, iy), 1)
    check("the frame's surface is the control there", moho64ui_ControlAt(ix, iy) == FC.Controls.hit, true)
    -- a left click on it: the main camera jumps to the commander
    cam:SnapTo({p[1] + 200, p[2], p[3] + 200}, {3.14159, 1.2, 0}, 90)
    WaitFrames(2)
    moho64ui_MouseMove(ix, iy)
    moho64ui_MouseButton("Left", true, ix, iy)
    moho64ui_MouseButton("Left", false, ix, iy)
    local focus = cam:GetFocusPosition()
    check("a left click on it jumps the main camera to the unit", near(focus[1], p[1], 2) and near(focus[3], p[3], 2), true)
    check("and keeps it open", FC.State.open, true)
    -- the split screen with it: three views
    moho64ui_Key(315)
    WaitFrames(3)
    check("split with the follow camera: three views", kinds(moho64ui_Views()), "main split inset")
    check("the inset is view 2 then", moho64ui_FollowCamState().view, 2)
    moho64ui_Key(314)
    WaitFrames(2)
    check("unsplit: the main view and the inset again", kinds(moho64ui_Views()), "main inset")
    -- a right click on it closes it
    moho64ui_MouseButton("Right", true, ix, iy)
    moho64ui_MouseButton("Right", false, ix, iy)
    WaitFrames(2)
    check("a right click on it closes it", tostring(FC.State.open) .. " " .. tostring(moho64ui_FollowCamState().on), "false false")
    check("one view again", table.getn(moho64ui_Views()), 0)
    -- the key twice, and the console (the split screen's SplitMapGroup cleared the selection: FAF's)
    SelectUnits({cdr})
    moho64ui_Key("v", "Ctrl Alt")
    check("the key opens it", FC.State.open, true)
    moho64ui_Key("v", "Ctrl Alt")
    check("the key again closes it", FC.State.open, false)
    ConExecute("remaster_follow_cam on")
    check("the console's remaster_follow_cam on", FC.State.open, true)
    ConExecute("remaster_follow_cam off")
    check("and off", FC.State.open, false)
    -- a tank destroyed while followed: it stays a while on the wreck, then closes
    SimCallback({Func = "CheatSpawnUnit", Args = {army = GetFocusArmy(), pos = {p[1] + 15, p[2], p[3] + 15}, bpId = "uel0201", count = 1, yaw = 0}})
    WaitSeconds(1.5)
    UISelectionByCategory("LAND MOBILE TECH1", false, false, false, false)
    local tank = nil
    for _, u in GetSelectedUnits() or {} do
        if u:GetBlueprint().BlueprintId == "uel0201" then tank = u end
    end
    check("a tank made and selected", tank ~= nil, true)
    if tank then
        SelectUnits({tank})
        FC.Open()
        WaitFrames(3)
        check("it follows the tank", moho64ui_FollowCamState().unit .. "", tank:GetEntityId())
        SimCallback({Func = 'ToggleSelfDestruct', Args = {owner = GetFocusArmy(), noDelay = true}}, true)
        local lost = WaitFor(function() return moho64ui_FollowCamState().lost ~= nil or not FC.State.open end, 900)
        check("the tank is destroyed: it is lost to the follow camera", lost and FC.State.open, true)
        WaitFrames(2)
        check("its title says so", string.find(FC.State.name or "", "(destroyed)", 1, true) ~= nil, true)
        WaitSeconds(1.5)
        check("it is still open 1.5 s after (on the wreck)", FC.State.open, true)
        local closed = WaitFor(function() return not FC.State.open end, 150)
        check("and closes by itself about 3 s after", closed, true)
        check("one view again after it", table.getn(moho64ui_Views()), 0)
    end

    -- 3. the key action's names
    local action = KeyMapper.GetKeyActions()['remaster_follow_cam']
    check("the key action remaster_follow_cam", action and action.action, "remaster_follow_cam")
    check("its name in the key binding screen", KeyMapper.GetActionName('remaster_follow_cam'), "Follow camera on the selected unit (open / close)")
    check("its key in the default map", moho64ui_KeyMapAction("Ctrl-Alt-V"), "remaster_follow_cam")
    check("remaster_follow_cam is among the console's names", table.getn(ConTextMatches("remaster_follow")), 1)
    local Palette = import("/moho64ui/lua/palette.lua")
    Palette.Open()
    Palette.Controls.edit:SetText("follow camera")
    local found = false
    for _, row in Palette.State.rows do
        if row.id == 'action:remaster_follow_cam' then found = true end
    end
    check("the command palette finds it", found, true)
    Palette.Close()

    LOG("TEST done: " .. failed .. " failed")
end)
