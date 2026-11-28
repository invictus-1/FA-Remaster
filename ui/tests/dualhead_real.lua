-- The second head on a REAL second screen (ours; docs/ui-dualscreen.md; Chris, 2026-11-27: "Go ahead with the second
-- monitor test now"). The profile's Secondary Adapter names screen 2 ("0,0,0,1") and its Primary Adapter is a full
-- screen: window 2 opens full screen on the second screen and FAF's CreateSecondView puts a world view on it. In a
-- game: two heads and two root frames, the second view filling frame 1 at that screen's own size, a view of the
-- renderer's picture; a click on the commander there selects him and a right click orders a move to the ground under
-- it; the main window's picture goes into a report (screenshot.jpg) and, with MOHO64UI_SECOND_CAPTURE, window 2's is
-- read back. Then twelve seconds of the game for the log's frame numbers, and the program ends itself.
-- Any sizes (ui/tests/dualhead.lua's window case is written for the 960x540 test window). TWO FULL-SCREEN WINDOWS on
-- both of the PC's screens for about 40 s: the lab's window_checks.sh, check wc_dualreal, runs it only on a PC with
-- two screens, and never by itself in a suite.
--   moho64ui --own ui/data --init <init.lua> --prefs <a copy with secondary_adapter = "0,0,0,1"> --window
--            --session <session_ai_cheats.lua> --game-lua ui/tests/dualhead_real.lua --frames 2000000
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local function near(a, b, tol) return math.abs(a - b) <= tol end

ForkThread(function()
    while WorldIsLoading() or GetGameTimeSeconds() < 8 do WaitFrames(10) end
    local looks = moho64ui_Looks()
    local w2, h2 = looks.secondWidth, looks.secondHeight
    LOG("TEST note heads " .. tostring(looks.heads) .. ", the second " .. tostring(w2) .. "x" .. tostring(h2) .. ", head 0 " .. GetFrame(0).Width() .. "x" .. GetFrame(0).Height())
    check("two heads", looks.heads, 2)
    check("two root frames", GetNumRootFrames(), 2)
    local MH = import("/lua/ui/game/multihead.lua")
    local view = MH.view
    check("CreateSecondView made a world view on frame 1", view and view:GetRootFrame() == GetFrame(1), true)
    check("filling frame 1 at the second screen's size", view and (view.Left() .. "," .. view.Top() .. " " .. view.Width() .. "x" .. view.Height()), "0,0 " .. w2 .. "x" .. h2)
    WaitFrames(10)
    local number, on = moho64ui_ViewNumber(view)
    check("the picture's second window is on", on, true)
    check("head 1's view is a view of the picture", number ~= nil and number > 0, true)
    local cdr = GetArmyAvatars()[1]
    local pos = cdr:GetPosition()
    local cam2 = GetCamera('CameraHead2')
    cam2:SnapTo(pos, {3.14159, 1.0, 0}, 60)
    WaitFrames(20)
    local s = view:Project(pos)
    local sx, sy = math.floor(s.x), math.floor(s.y)
    LOG(string.format("TEST note the commander on head 1 at %d,%d", sx, sy))
    check("the commander projects into head 1's view", sx > 0 and sx < w2 and sy > 0 and sy < h2, true)
    SelectUnits(nil)
    moho64ui_MouseMove(sx, sy, 1)
    WaitFrames(2)
    moho64ui_MouseButton("Left", true, sx, sy, "", 1)
    moho64ui_MouseButton("Left", false, sx, sy, "", 1)
    local sel = GetSelectedUnits()
    check("a left click on the commander on head 1 selects him", sel and table.getn(sel) == 1 and sel[1] == cdr, true)
    local gx, gy = sx + 160, sy - 120
    local ground = UnProject(view, {gx, gy})
    moho64ui_MouseMove(gx, gy, 1)
    WaitFrames(2)
    moho64ui_MouseButton("Right", true, gx, gy, "", 1)
    moho64ui_MouseButton("Right", false, gx, gy, "", 1)
    WaitSeconds(0.5)
    local order = (cdr:GetCommandQueue() or {})[1]
    check("a right click on head 1 gives a move to the ground under it", order and order.type == "Move" and near(order.position[1], ground[1], 1.5) and near(order.position[3], ground[3], 1.5), true)
    ConExecute("remaster_stats on")
    -- the main window's picture into a report (its screenshot.jpg); window 2's by MOHO64UI_SECOND_CAPTURE
    moho64ui_ReportPicture("take")
    WaitFrames(3)
    local file = moho64ui_Report("the second monitor test: the main window", true)
    LOG("TEST note report: " .. tostring(file))
    local t0 = GetGameTimeSeconds()
    while GetGameTimeSeconds() < t0 + 12 do WaitFrames(20) end
    LOG("TEST done: " .. failed .. " failed")
    ExitApplication()
end)
