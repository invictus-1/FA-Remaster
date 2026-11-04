-- The main menu's moving glows (FAF's /lua/ui/menus/main.lua), frame by frame, against what the original's engine
-- makes of the same scripts: every frame the controls that asked for it get OnFrame(the frame's time) (FA exe
-- CMauiFrame::Frame 0x7965a0, hidden or not); the layout follows at once (LazyVars); SetAlpha sets the control and,
-- asked, everything below it (0x789790); Hide hides the control and everything below it (0x787170).
--   1. the opening: the bracket (menuBracketMiddle.Animate) slides down 1000 px a second, its two "energy" glows
--      (bracket-left/right-energy_bmp) going with it, in their places on the brackets in every frame;
--   2. the buttons' glow (large_btn_glow, EffectHelpers.FadeIn .25 s / FadeOut .4 s) on the mouse's way in and out;
--   3. the menu hidden (MenuHide: the slide back, then the group hidden) and shown again (MenuShow);
--   4. the window resized: the glows stay on their brackets;
--   5. the alpha the fades set: SetAlpha's limits, and the alpha byte the picture is drawn with.
-- A headless run steps 1/60 s a frame, so the times are exact.
--   moho64ui --init <init.lua> --prefs <a copy> --stage frontend --size 1920x1080 --lua ui/tests/menu_glows.lua
--            --frames 900 --log out.log [-- /glowpics <folder>]
-- With /glowpics, pictures of the interface (moho64ui_InterfacePng, over black) at fixed moments go into the folder.
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed". "TEST trace" lines are
-- the recorded values, one per frame.
local LayoutHelpers = import("/lua/maui/layouthelpers.lua")
local main = import("/lua/ui/menus/main.lua")

local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local function near(a, b) return math.abs(a - b) < 0.001 end

local pics = (GetCommandLineArg("/glowpics", 1) or {})[1]
local function picture(name)
    if pics then
        local ok = moho64ui_InterfacePng(pics .. "/" .. name .. ".png")
        LOG("TEST note picture " .. name .. (ok and "" or " NOT written"))
    end
end

-- the menu keeps its controls in locals of CreateUI: read through the functions using them
local function upvalue(fn, name)
    for i = 1, 200 do
        local n, v = debug.getupvalue(fn, i)
        if not n then break end
        if n == name then return v end
    end
end

ForkThread(function()
    WaitFrames(1)
    local mainMenuGroup = upvalue(main.MenuBuild, 'mainMenuGroup')
    local bracketLeft = upvalue(main.MenuBuild, 'menuBracketLeft')
    local bracketRight = upvalue(main.MenuBuild, 'menuBracketRight')
    local middle = upvalue(main.MenuAnimation, 'menuBracketMiddle')
    local topBorder = middle and upvalue(middle.Animate, 'topBorder')
    local glowLeft, glowRight
    if mainMenuGroup then
        mainMenuGroup:ApplyFunction(function(c)
            local t = c._filename and c._filename._texture and c._filename._texture()
            if type(t) == 'string' and string.find(t, 'bracket%-left%-energy') then glowLeft = c end
            if type(t) == 'string' and string.find(t, 'bracket%-right%-energy') then glowRight = c end
        end)
    end
    check("the menu's controls found", (mainMenuGroup and bracketLeft and bracketRight and middle and topBorder and glowLeft
        and glowRight) ~= nil, true)
    if not (glowLeft and glowRight and topBorder) then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    -- the glows' places on the brackets, as main.lua lays them out (AtRightTopIn(-10, 20) and AtBottomIn(10) on the left
    -- bracket, AtLeftTopIn(-10, 20) and AtBottomIn(10) on the right one): each edge floor(the bracket's edge + offset)
    local psf = LayoutHelpers.GetPixelScaleFactor()
    local F = math.floor
    local function onBrackets()
        return glowLeft.Right() == F(bracketLeft.Right() + 10 * psf) and glowLeft.Top() == F(bracketLeft.Top() + 20 * psf)
            and glowLeft.Bottom() == F(bracketLeft.Bottom() - 10 * psf) and glowRight.Left() == F(bracketRight.Left() - 10 * psf)
            and glowRight.Top() == F(bracketRight.Top() + 20 * psf) and glowRight.Bottom() == F(bracketRight.Bottom() - 10 * psf)
    end
    local function trace(what)
        LOG(string.format("TEST trace %s t=%.4f middle.Top=%.3f glowL=%.1f,%.1f,%.1f,%.1f glowR=%.1f,%.1f a=%.3f/%.3f hidden=%s/%s",
            what, CurrentTime(), middle.Top(), glowLeft.Left(), glowLeft.Top(), glowLeft.Right(), glowLeft.Bottom(),
            glowRight.Left(), glowRight.Top(), glowLeft:GetAlpha(), glowRight:GetAlpha(), tostring(glowLeft:IsHidden()),
            tostring(glowRight:IsHidden())))
    end

    -- 1. the opening slide: from above (its bottom on the top border's) down to the top border, 1000 px a second
    local height = bracketLeft.Height()
    local from, to = topBorder.Bottom() - height, topBorder.Bottom()
    local steps, frames, last, wrong, placed, moving = {}, 0, middle.Top(), 0, true, false
    local before, startTop = 0, nil
    while frames < 600 do
        trace("open")
        if not onBrackets() then placed = false end
        if glowLeft:IsHidden() or glowRight:IsHidden() or glowLeft:GetAlpha() ~= 1 or glowRight:GetAlpha() ~= 1 then wrong = wrong + 1 end
        if moving and not middle:NeedsFrameUpdate() then break end
        if middle:NeedsFrameUpdate() then moving = true end
        if not moving then before = before + 1 end
        WaitFrames(1)
        frames = frames + 1
        if middle.Top() ~= last then
            if not startTop then startTop = last end
            table.insert(steps, middle.Top() - last)
        end
        last = middle.Top()
        if moving and table.getn(steps) == 20 then picture("1_opening") end
    end
    trace("open end")
    check("the slide starts where the bracket hangs above the top border", startTop ~= nil and near(startTop, from), true)
    local n = table.getn(steps)
    local even = n >= 2
    for i = 1, n - 1 do if not near(steps[i], 1000 / 60) then even = false end end
    check("it moves 1000 px a second: 1000/60 px in every frame of 1/60 s", even, true)
    check("and stops on the top border", near(middle.Top(), to), true)
    check("in ceil(height / (1000/60)) frames", n, math.ceil((to - from) / (1000 / 60) - 1e-9))
    check("the energy glows stay on the brackets in every frame", placed, true)
    check("the energy glows shown and at full alpha all along", wrong, 0)
    LOG("TEST note the slide began " .. before .. " frames after the test's start, took " .. n .. " frames, " .. from .. " -> " .. to)

    -- the buttons come in (their brackets slide 200 px a second), then answer the mouse
    local first = upvalue(main.MenuBuild, 'mainMenu')[1].btn
    local waited = 0
    while first.OnRolloverEvent ~= first.rofunc and waited < 600 do
        WaitFrames(1)
        waited = waited + 1
    end
    check("the first button is live after its brackets came in", first.OnRolloverEvent == first.rofunc, true)
    picture("2_open")
    check("a button's own picture is one texture: Play makes no animation (Button: looping ping-pong, Loop, Play)",
        first:GetNumFrames() == 1 and not first:NeedsFrameUpdate(), true)

    -- 2. the button's glow: in over .25 s, out over .4 s, then hidden
    local glow = first.glow
    check("the button's glow starts at alpha 0 (main.lua: SetAlpha(0) at its creation)", glow:GetAlpha(), 0)
    local cx, cy = (first.Left() + first.Right()) / 2, (first.Top() + first.Bottom()) / 2
    moho64ui_MouseMove(cx, cy)
    local ins, okIn = {}, true
    for k = 1, 20 do
        WaitFrames(1)
        table.insert(ins, string.format("%.4f", glow:GetAlpha()))
        if not near(glow:GetAlpha(), math.min(k / 15, 1)) then okIn = false end
        if k == 7 then picture("3_glow_half_in") end
    end
    LOG("TEST trace glow in: " .. table.concat(ins, " "))
    check("the glow comes in linearly over .25 s: alpha k/15 after k frames, 1 from the 15th", okIn, true)
    check("and is shown", glow:IsHidden(), false)
    check("its frame updates end with the fade", glow:NeedsFrameUpdate(), false)
    picture("4_glow_full")
    moho64ui_MouseMove(cx, first.Bottom() + 200)
    local outs, okOut = {}, true
    for k = 1, 30 do
        WaitFrames(1)
        table.insert(outs, string.format("%.4f", glow:GetAlpha()))
        if not near(glow:GetAlpha(), math.max(1 - k / 24, 0)) then okOut = false end
        if k == 12 then picture("5_glow_half_out") end
    end
    LOG("TEST trace glow out: " .. table.concat(outs, " "))
    check("the glow goes out linearly over .4 s: alpha 1 - k/24 after k frames", okOut, true)
    check("and is hidden at the end (FadeOut)", glow:IsHidden(), true)

    -- 3. the menu hidden: the buttons' brackets go back, the bracket slides up, then the group is hidden
    local hiddenAt
    main.MenuHide(function() hiddenAt = CurrentTime() end)
    local up, lastTop, k2 = {}, middle.Top(), 0
    while not hiddenAt and k2 < 600 do
        WaitFrames(1)
        k2 = k2 + 1
        if middle.Top() ~= lastTop then table.insert(up, middle.Top() - lastTop) end
        lastTop = middle.Top()
        if not onBrackets() then placed = false end
    end
    trace("hidden")
    local evenUp = table.getn(up) >= 2
    for i = 1, table.getn(up) - 1 do if not near(up[i], -1000 / 60) then evenUp = false end end
    check("MenuHide: the bracket slides back up, 1000/60 px a frame", evenUp, true)
    check("and ends with its bottom on the top border", near(middle.Top(), to - height), true)
    check("the glows went with the brackets", placed, true)
    check("the menu's group hidden, the energy glows with it", mainMenuGroup:IsHidden() and glowLeft:IsHidden() and glowRight:IsHidden(), true)
    picture("6_hidden")
    WaitFrames(30)

    -- shown again: the slide down once more
    main.MenuShow()
    local frames2, steps2 = 0, {}
    lastTop = middle.Top()
    while frames2 < 600 do
        WaitFrames(1)
        frames2 = frames2 + 1
        if middle.Top() ~= lastTop then table.insert(steps2, middle.Top() - lastTop) end
        lastTop = middle.Top()
        if not onBrackets() then placed = false end
        if table.getn(steps2) > 0 and not middle:NeedsFrameUpdate() then break end
    end
    trace("shown")
    check("MenuShow: the group and its glows shown again", mainMenuGroup:IsHidden() or glowLeft:IsHidden() or glowRight:IsHidden(), false)
    check("the slide down again, in as many frames", table.getn(steps2), n)
    check("the glows on the brackets all along", placed, true)
    local gx, gy = math.floor((glowLeft.Left() + glowLeft.Right()) / 2), math.floor((glowLeft.Top() + glowLeft.Bottom()) / 2)
    LOG("TEST note the left glow's middle pixel, shown: " .. tostring(moho64ui_PixelAt(gx, gy)))

    -- 4. a resize: the brackets are centred on the window (a function of its width); their Top is the number the slide
    -- left, so the glows move sideways with them and keep their places
    local W, H = GetFrame(0).Width(), GetFrame(0).Height()
    local leftBefore = glowLeft.Left()
    moho64ui_Resize(1600, 900)
    WaitFrames(2)
    trace("resized")
    check("resized: the glows still on their brackets", onBrackets(), true)
    check("and moved with the centred brackets (left glow by (1600 - " .. W .. ") / 2)", near(glowLeft.Left() - leftBefore, math.floor((1600 - W) / 2)) or near(glowLeft.Left() - leftBefore, (1600 - W) / 2), true)
    picture("7_resized")
    moho64ui_Resize(W, H)
    WaitFrames(2)
    check("back to the first size, back in place", near(glowLeft.Left(), leftBefore) and onBrackets(), true)

    -- 5. the alpha the fades set (FA exe SetAlpha 0x789790, FAF's 0x1291575): a negative one becomes 0 (with a warning),
    -- one above 1 stays (with a warning); the picture is drawn with alpha * 255 cut toward zero (0.5: 127, not 128)
    local Bitmap = import("/lua/maui/bitmap.lua").Bitmap
    -- (a place where the interface draws nothing, so the pixel is the probe's alone: once MenuShow's veil, `darker`,
    -- has faded out, 1 s)
    WaitSeconds(1.5)
    local px, py
    for _, p in {{W / 4, H / 2}, {W * 3 / 4, H / 2}, {W / 4, H * 3 / 4}, {10, 10}} do
        if not px and moho64ui_PixelAt(math.floor(p[1]), math.floor(p[2])) == "00000000" then px, py = math.floor(p[1]), math.floor(p[2]) end
    end
    check("an empty place for the probe", px ~= nil, true)
    px, py = px or 10, py or 10
    local probe = Bitmap(GetFrame(0))
    probe:SetSolidColor('ffffffff')
    probe.Left:Set(px - 2)
    probe.Top:Set(py - 2)
    probe.Width:Set(4)
    probe.Height:Set(4)
    probe.Depth:Set(10000)
    check("SetAlpha gives the control back", probe:SetAlpha(0.5) == probe, true)
    WaitFrames(1)
    LOG("TEST note the probe's pixel at alpha .5: " .. tostring(moho64ui_PixelAt(px, py)))
    check("alpha .5 draws with the byte 127 (cut, not rounded)", string.sub(moho64ui_PixelAt(px, py) or "", 1, 2), "7f")
    probe:SetAlpha(-0.25)
    check("a negative alpha becomes 0", probe:GetAlpha(), 0)
    probe:SetAlpha(1.5)
    check("one above 1 is kept", probe:GetAlpha(), 1.5)
    check("and drawn as 1", string.sub(moho64ui_PixelAt(px, py) or "", 1, 2), "ff")
    check("SetAlpha with one argument is an error", pcall(probe.SetAlpha, probe), false)
    probe:Destroy()
    LOG("TEST done: " .. failed .. " failed")
end)
