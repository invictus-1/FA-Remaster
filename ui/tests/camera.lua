-- The camera's scripted moves, as the original's CameraImpl (the campaign's cinematics): a timed
-- MoveTo is an event WaitFor waits on; it keeps the script's heading and pitch until RevertRotation;
-- the curve is soft at both ends (straight after DisableEaseInOut); Spin turns by turns a second;
-- with UseGameClock a move stands while the game is paused.
-- Run it like game.lua (a launched game) with --frames 2400.
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local function near(a, b, d) return math.abs(a - b) <= d end
-- A move given as a player gives it: the unit selected, a right click at the place. (IssueUnitCommand's third
-- argument is the command's Lua table, not a place: a "Move" given by it goes nowhere, and until 2026-11-09 this
-- test gave its move so and the unit stood.) Answers whether the unit then has an order queued.
local function walkTo(unit, place)
    local before = GetSelectedUnits()
    local at = import("/lua/ui/game/worldview.lua").viewLeft:Project(place)
    SelectUnits({unit})
    local waited = 0  -- (a commander still arriving cannot be selected yet)
    while table.getn(GetSelectedUnits() or {}) == 0 and waited < 10 do
        WaitSeconds(0.5)
        waited = waited + 0.5
        SelectUnits({unit})
    end
    moho64ui_MouseMove(at.x, at.y)
    WaitSeconds(0.2)
    moho64ui_MouseButton("Right", true, at.x, at.y)
    moho64ui_MouseButton("Right", false, at.x, at.y)
    WaitSeconds(0.4)
    local queued = (unit:GetCommandQueue() or {})[1] ~= nil
    if not queued then LOG(string.format("TEST note: walkTo: no order after the click at %.0f, %.0f with %d selected", at.x, at.y, table.getn(GetSelectedUnits() or {}))) end
    SelectUnits(before)
    return queued
end
ForkThread(function()
    WaitSeconds(4)
    local cam = GetCamera('WorldCamera')
    local focus = cam:GetFocusPosition()
    local to = {focus[1] + 40, focus[2], focus[3] + 20}
    -- a timed move: WaitFor lets go at its end
    local start = GetSystemTimeSeconds()
    cam:MoveTo(to, {3.14159, 0.6, 0}, 60, 1.5)
    WaitFor(cam)
    local took = GetSystemTimeSeconds() - start
    check("WaitFor(camera) lets go when a 1.5 s move ends", took > 1.4 and took < 1.7, true)
    LOG("TEST note: the move took " .. string.format("%.2f", took) .. " s")
    local s = cam:SaveSettings()
    -- (the place given, moved along the view onto the ground as the original's: the focus lies on
    -- the line through it in the view's direction)
    local dx, dy, dz = math.sin(s.Heading) * math.cos(s.Pitch), -math.sin(s.Pitch), math.cos(s.Heading) * math.cos(s.Pitch)
    local vx, vy, vz = s.Focus[1] - to[1], s.Focus[2] - to[2], s.Focus[3] - to[3]
    local along = vx * dx + vy * dy + vz * dz
    local ox, oy, oz = vx - along * dx, vy - along * dy, vz - along * dz  -- (no ^: the game's Lua has it as a bitwise operator)
    local off = math.sqrt(ox * ox + oy * oy + oz * oz)
    LOG(string.format("TEST note: to %.2f %.2f %.2f, focus %.2f %.2f %.2f, heading %.3f pitch %.3f", to[1], to[2], to[3], s.Focus[1], s.Focus[2], s.Focus[3], s.Heading, s.Pitch))
    LOG("TEST note: the place given moved " .. string.format("%.1f", along) .. " along the view onto the ground, off the line by " .. string.format("%.2f", off))
    check("the move ends on its place, along the view onto the ground, at its zoom", off < 0.5 and near(s.Zoom, 60, 0.5), true)
    check("and keeps the script's pitch (the camera holds its turn)", near(s.Pitch, 0.6, 0.01), true)
    WaitSeconds(0.5)
    check("still held a while after", near(cam:SaveSettings().Pitch, 0.6, 0.01), true)
    cam:RevertRotation()
    WaitSeconds(1)
    check("RevertRotation: the pitch follows the zoom again", cam:SaveSettings().Pitch > 0.65, true)
    -- the curve: soft at both ends, straight after DisableEaseInOut (a quarter of the way in time)
    local from = cam:GetFocusPosition()
    local far = {from[1] + 100, from[2], from[3]}
    cam:MoveTo(far, {3.14159, 0.8, 0}, 60, 4)
    WaitSeconds(1)
    local soft = (cam:GetFocusPosition()[1] - from[1]) / 100
    WaitFor(cam)
    cam:DisableEaseInOut()
    from = cam:GetFocusPosition()
    far = {from[1] - 100, from[2], from[3]}
    cam:MoveTo(far, {3.14159, 0.8, 0}, 60, 4)
    WaitSeconds(1)
    local straight = (from[1] - cam:GetFocusPosition()[1]) / 100
    WaitFor(cam)
    cam:EnableEaseInOut()
    LOG("TEST note: a quarter of the way in time: soft " .. string.format("%.3f", soft) .. ", straight " .. string.format("%.3f", straight))
    check("soft at both ends: a quarter in time is 0.156 of the way", near(soft, 0.156, 0.03), true)
    check("straight after DisableEaseInOut: a quarter is 0.25", near(straight, 0.25, 0.03), true)
    -- Spin: a quarter turn a second
    local h0 = cam:SaveSettings().Heading
    cam:Spin(0.25)
    WaitSeconds(1)
    cam:Spin(0, 0)
    local turned = cam:SaveSettings().Heading - h0
    check("Spin(0.25): a quarter turn in a second", near(math.abs(turned), 3.14159 / 2, 0.1), true)
    cam:RevertRotation()
    WaitSeconds(1.5)
    -- the game's clock: a move stands while the game is paused
    cam:UseGameClock()
    SessionRequestPause()
    WaitSeconds(0.3)
    from = cam:GetFocusPosition()
    cam:MoveTo({from[1] + 30, from[2], from[3]}, {3.14159, 0.9, 0}, 60, 1)
    WaitSeconds(1.5)
    check("UseGameClock: a move stands while the game is paused", near(cam:GetFocusPosition()[1], from[1], 0.5), true)
    SessionResume()
    WaitFor(cam)
    check("and ends once it runs again", near(cam:GetFocusPosition()[1], from[1] + 30, 0.5), true)
    cam:UseSystemClock()
    cam:RevertRotation()
    -- following a unit: TrackEntities keeps the focus on the commander as it walks
    local cdr = GetArmyAvatars()[1]
    local id = cdr:GetEntityId()
    cam:TrackEntities({id}, 40, 1)
    WaitFor(cam)
    local cp = cdr:GetPosition()
    local f = cam:GetFocusPosition()
    check("TrackEntities: the focus on the unit", near(f[1], cp[1], 1.5) and near(f[3], cp[3], 1.5), true)
    local stood = {cp[1], cp[2], cp[3]}
    walkTo(cdr, {cp[1] + 8, cp[2], cp[3]})
    WaitSeconds(2.5)
    cp = cdr:GetPosition()
    check("the unit walks", math.abs(cp[1] - stood[1]) + math.abs(cp[3] - stood[3]) > 1, true)
    f = cam:GetFocusPosition()
    LOG("TEST note: following, the unit at " .. string.format("%.1f, %.1f", cp[1], cp[3]) .. ", the focus at " .. string.format("%.1f, %.1f", f[1], f[3]))
    check("and follows it as it walks", near(f[1], cp[1], 3) and near(f[3], cp[3], 3), true)
    -- the nose: behind the unit, its pitch plus the adjustment, held
    cam:NoseCam(id, 0.3, 30, 1, 0)
    WaitFor(cam)
    local nose = cam:SaveSettings()
    cp = cdr:GetPosition()
    check("NoseCam: on the unit, raised by its adjustment", near(nose.Focus[1], cp[1], 3) and near(nose.Pitch, 0.3, 0.15), true)
    LOG("TEST note: nose heading " .. string.format("%.2f", nose.Heading) .. ", pitch " .. string.format("%.2f", nose.Pitch))
    cam:RevertRotation()
    WaitSeconds(1)
    check("RevertRotation ends the nose camera", near(cam:SaveSettings().Pitch, 0.3, 0.05), false)
    -- [review N, 2026-11-10] A spin that zooms in for longer than there is zoom (a cinematic's Spin(rate, zoomRate)
    -- left running): the zoom stays a number and stops at the nearest zoom. (Before: its target went below 0, its
    -- logarithm was not a number, and neither was the zoom until the turn ended.)
    WaitSeconds(1.5)
    cam:Spin(0, -1000)
    WaitSeconds(0.5)
    local z = cam:SaveSettings().Zoom
    LOG("TEST note: the zoom after Spin(0, -1000) for half a second: " .. tostring(z))
    check("Spin(0, -1000): the zoom is still a number", z == z and z > 0, true)
    check("and no nearer than the nearest zoom", z == z and z >= cam:GetMinZoom() - 0.001, true)
    -- The swing back in frames of a 240th of a second (moho64ui_CameraStep: the camera's own frame, beside the run's
    -- frames of a 60th): no frame turns the heading or the lean by more than the step of its time, the last one
    -- included. (Before: it ended with a jump once both were within a tenth of a radian, four steps of a 240th.)
    cam:Spin(0.2, 0)
    WaitSeconds(1)
    cam:Spin(0, 0)
    cam:RevertRotation()
    local step = 0.1 * 60 / 240
    local worst, worstAt = 0, 0
    local s0 = cam:SaveSettings()
    local h, p = s0.Heading, s0.Pitch
    LOG(string.format("TEST note: the swing back begins at heading %.3f, pitch %.3f", h, p))
    for frame = 1, 400 do
        moho64ui_CameraStep('WorldCamera', 1 / 240)
        local s = cam:SaveSettings()
        local dh = math.abs(s.Heading - h)
        if dh > 3.14159 then dh = math.abs(dh - 2 * 3.14159265) end  -- (north is pi and -pi alike)
        local d = math.max(dh, math.abs(s.Pitch - p))
        if d > worst then worst, worstAt = d, frame end
        h, p = s.Heading, s.Pitch
    end
    LOG(string.format("TEST note: the biggest turn of one frame %.4f (frame %d), the step of a 240th %.4f; ends at heading %.4f, pitch %.4f", worst, worstAt, step, h, p))
    check("the swing back at 240 frames a second: no frame turns by more than its step", worst <= step + 0.0005, true)
    check("and it ends north up", near(math.abs(h), 3.14159, 0.001), true)
    LOG("TEST done: " .. failed .. " failed")
end)
