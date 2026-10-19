-- The photo mode (ours: maui/worldview.cpp, its panel ui/data/lua/photomode.lua; the renderer's free camera, moment,
-- lens and photos). A game is played and kept as a replay, then played back from the front end, as cinecam.lua does;
-- the same file serves the three states:
--   the game played (moho64ui --stage setup --session session_ai.lua --seed 7 --game-lua ui/tests/photomode.lua
--     --frames 3000, with the world's picture): opened by its command it pauses the game (one player) and puts the
--     game's panels away; the keys and the mouse fly, roll and turn the free camera and reach no key map action; the
--     panel sets the moment, the weather and the lens; a click focuses; photos at 1x and 2x are saved; H hides the
--     panel; Esc leaves: the moment cleared, the game going on, the panels and the game's camera back. The replay is
--     kept as "moho64_photomode";
--   the front end, then the replay (with the same preferences folder: --stage frontend --lua ui/tests/photomode.lua
--     --game-lua ui/tests/photomode.lua --frames 1500): the photo mode pauses the replay too and takes over from the
--     cinematic camera; it is left open at the end with its panel (for a --screenshot).
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
local function bools(...)
    local out = {}
    for i = 1, arg.n do table.insert(out, tostring(arg[i])) end
    return table.concat(out, " ")
end
local function distance(a, b)
    return math.sqrt((a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]) + (a[3] - b[3]) * (a[3] - b[3]))
end
local function eye() local e = moho64ui_PhotoMode().camera.eye return { e[1], e[2], e[3] } end
local REPLAY = "moho64_photomode"

if not rawget(_G, "SessionIsActive") or not SessionIsActive() then
    -- the front end: the replay the game kept starts
    local profile = import("/lua/user/prefs.lua").GetCurrentProfile().Name
    local file = GetSpecialFilePath(profile, REPLAY, "Replay")
    LOG("TEST note the replay: " .. file)
    local started = LaunchReplaySession(file)
    LOG((started and "TEST ok   " or "TEST FAIL ") .. "LaunchReplaySession starts the kept replay")
    if not started then LOG("TEST done: 1 failed") end
    return
end

local heard = {}
AddConsoleOutputReciever(function(text) table.insert(heard, text) end)
local function take()
    local t = heard
    heard = {}
    return table.concat(t, "|")
end
local GameMain = import("/lua/ui/game/gamemain.lua")
local Panel = import("/moho64ui/lua/photomode.lua")
local view = import("/lua/ui/game/worldview.lua").viewLeft
-- a key held for a while (the camera's frame reads what is held)
local function hold(key, seconds, mods)
    moho64ui_Key(key, mods or "", "down")
    WaitSeconds(seconds)
    moho64ui_Key(key, mods or "", "up")
    WaitSeconds(0.4)  -- (the move eases out)
end
-- a photo from the panel's Take Photo at `k` times the view's size: saved, a PNG of that size, said on the console
local function photo(k)
    Panel.Controls.size:SetItem(k)
    Panel.Controls.size:OnClick(k)
    check(k .. "x: the size taken", moho64ui_PhotoMode().size, k)
    take()
    Panel.Controls.take:OnClick()
    check(k .. "x: the photo is being made", moho64ui_PhotoMode().taking, true)
    local frames, sawProgress, sawBar = 0, false, false
    while (moho64ui_PhotoMode().taking or moho64ui_PhotoMode().saving) and frames < 3600 do
        WaitFrames(1)
        frames = frames + 1
        if moho64ui_PhotoMode().progress > 0 then sawProgress = true end
        if not Panel.Controls.progress:IsHidden() and Panel.Controls.progress.Width() > 0 then sawBar = true end
    end
    local waited = frames / 60
    check(k .. "x: its progress goes up while it is made", sawProgress, true)
    check(k .. "x: and the panel's bar shows it", sawBar, true)
    local s = moho64ui_PhotoMode()
    LOG(string.format("TEST note: the %dx photo took %.1f s of frames: %s (%s)", k, waited, s.photo, s.note))
    local line = take()
    local path = string.match(line, "Photo saved: (.+) %(%d+x%d+%)$")
    check(k .. "x: a console line says where the photo is", path ~= nil and path == s.photo, true)
    if not path then
        LOG("TEST note: the console said \"" .. line .. "\"")
        return
    end
    check(k .. "x: in the preferences' folder's screenshots, named by the date and time",
        string.find(path, "/screenshots/photo_%d%d%d%d%-%d%d%-%d%d_%d%d%-%d%d%-%d%d") ~= nil, true)
    local info = moho64ui_PngInfo(path)
    check(k .. "x: the file is a PNG", info ~= nil, true)
    check(k .. "x: at " .. k .. " times the view's size", info and (info.width .. "x" .. info.height), string.format("%dx%d", k * view.Width(), k * view.Height()))
    check(k .. "x: and holds the pixels (stored: 3 bytes each)", info and info.bytes > info.width * info.height * 3, true)
    if info then LOG(string.format("TEST note: the %dx photo: %dx%d, %d bytes", k, info.width, info.height, info.bytes)) end
    local name = string.gsub(path, "^.*/", "")
    check(k .. "x: the panel's line names the file", s.note, "Saved " .. name)
end

local function common()
    -- the photo mode's key, and the command
    check("the key at first: Alt-P the photo mode", tostring(moho64ui_KeyMapAction("Alt-P")), "remaster_photo_mode")
    local cam = GetCamera("WorldCamera")
    local before = cam:SaveSettings()
    local wasPaused = SessionIsPaused()
    check("before: closed, the game's panels there", bools(moho64ui_PhotoMode().open, GameMain.gameUIHidden), "false false")
    take()
    ConExecute("remaster_photo_mode")
    local s = moho64ui_PhotoMode()
    check("remaster_photo_mode (no word): open, silently", bools(s.open, take() == ""), "true true")
    check("the game's panels are put away the game's own way (HideGameUI)", bools(GameMain.gameUIHidden, GameMain.GetControlCluster():IsHidden()), "true true")
    check("the photo mode's panel is there", bools(Panel.State.open, Panel.State.shown, Panel.Controls.take ~= nil), "true true true")
    check("no life bars, selection marks or work bars in the picture meanwhile", bools(s.lifeBars, s.selectionMarks, s.workBars), "false false false")
    check("it pauses the game", s.paused, true)
    WaitSeconds(0.5)
    check("and the game is paused (with a beat)", SessionIsPaused(), true)
    check("the camera is flown freely", moho64ui_PhotoMode().free, true)
    ConExecute("remaster_photo_mode on")
    check("on again: nothing changes", bools(moho64ui_PhotoMode().open, take() == ""), "true true")

    -- the keys fly it: W forward (along the ground), R up, Q E roll, Shift three times as fast
    local e0 = eye()
    hold("w", 0.5)
    local e1 = eye()
    LOG(string.format("TEST note: W held half a second: the eye %.1f %.1f %.1f -> %.1f %.1f %.1f", e0[1], e0[2], e0[3], e1[1], e1[2], e1[3]))
    local plain = distance(e0, e1)
    check("W held: the camera moves", plain > 1, true)
    check("forward along the ground: its height stays", math.abs(e1[2] - e0[2]) < 0.5, true)
    local e2 = eye()
    hold(316, 0.5)  -- (the left arrow: as A)
    check("the left arrow moves it too", distance(e2, eye()) > 1, true)
    e2 = eye()
    moho64ui_Key(306, "", "down")  -- (Shift held)
    hold("s", 0.5, "Shift")
    moho64ui_Key(306, "", "up")
    local fast = distance(e2, eye())
    LOG(string.format("TEST note: half a second: %.1f units, with Shift %.1f", plain, fast))
    check("with Shift: about three times as fast", fast > plain * 2 and fast < plain * 4, true)
    e2 = eye()
    hold("r", 0.5)
    check("R: up", eye()[2] - e2[2] > 1, true)
    e2 = eye()
    hold("f", 0.3)
    check("F: down", eye()[2] < e2[2], true)
    hold("e", 0.5)
    local roll = moho64ui_PhotoMode().camera.roll
    LOG(string.format("TEST note: E held half a second: the roll %.3f", roll))
    check("E: it rolls (the right side down), and stays", roll > 0.1, true)
    hold("q", 0.5)
    check("Q: back", moho64ui_PhotoMode().camera.roll < roll - 0.1, true)
    -- the wheel: a notch up
    local mx, my = view.Left() + view.Width() * 0.3, view.Top() + view.Height() * 0.5
    e2 = eye()
    moho64ui_MouseWheel(mx, my, 1)
    WaitSeconds(0.6)
    check("a wheel notch up: up", eye()[2] > e2[2], true)
    -- the right button's drag turns
    local yaw0 = moho64ui_PhotoMode().camera.yaw
    local game0 = cam:SaveSettings()
    moho64ui_MouseButton("Right", true, mx, my)
    moho64ui_MouseMove(mx + 200, my)
    WaitFrames(2)
    moho64ui_MouseMove(mx + 300, my + 60)
    WaitFrames(2)
    moho64ui_MouseButton("Right", false, mx + 300, my + 60)
    WaitSeconds(0.3)
    local yaw1 = moho64ui_PhotoMode().camera.yaw
    LOG(string.format("TEST note: the right drag of 300 pixels: the yaw %.3f -> %.3f", yaw0, yaw1))
    check("a right drag turns it (to the right)", math.abs(yaw1 - yaw0) > 0.05, true)
    check("and gives no order and moves no camera of the game's", math.abs(cam:SaveSettings().Zoom - game0.Zoom) < 0.01 and math.abs(cam:SaveSettings().Heading - game0.Heading) < 0.001, true)

    -- the keys reach no key map action while it is open (each bound here to a counter)
    PhotoTestKeys = 0
    local counter = { action = 'UI_Lua PhotoTestKeys = PhotoTestKeys + 1' }
    IN_AddKeyMapTable({ ['W'] = counter, ['A'] = counter, ['S'] = counter, ['D'] = counter, ['Q'] = counter, ['E'] = counter,
        ['R'] = counter, ['F'] = counter, ['Shift-W'] = counter, ['UpArrow'] = counter, ['Ctrl-W'] = counter, ['H'] = counter,
        ['Esc'] = counter })
    for _, key in { "w", "a", "s", "d", "q", "e", "r", "f", 317 } do moho64ui_Key(key) end
    moho64ui_Key("w", "Shift")
    moho64ui_Key("w", "Ctrl")
    check("W A S D Q E R F, the arrows, Shift-W, Ctrl-W: none reaches the key map", PhotoTestKeys, 0)
    -- and no other action of the key map either (Chris, 2026-10-19: "that way things don't get messed up")
    local others = { ['1'] = counter, ['G'] = counter, ['P'] = counter, ['Ctrl-Alt-C'] = counter }
    IN_AddKeyMapTable(others)
    for _, key in { "1", "g", "p" } do moho64ui_Key(key) end
    moho64ui_Key("c", "Ctrl-Alt")
    check("1, G, P, Ctrl-Alt-C: no other key map action runs while it is open", PhotoTestKeys, 0)
    check("and the photo mode stays open", moho64ui_PhotoMode().open, true)

    -- the moment: by the panel's slider and combo (the picture's own; read back)
    local c = Panel.Controls
    local own = moho64ui_PhotoMode().moment
    LOG(string.format("TEST note: the game's own moment: hour %.2f, weather %s, strength %.2f", own.hour, own.weatherName, own.strength))
    check("at first the game's own", own.set, false)
    check("the weather's kinds", table.concat(moho64ui_PhotoMode().weatherKinds, " "), "clear fair cloudy overcast rain storm fog snow showers")
    c.timeSlider:SetValue(15.5)
    c.timeSlider:OnValueSet(15.5)
    local m = moho64ui_PhotoMode().moment
    check("Time of Day 15:30: the moment set, read back", bools(m.set, math.abs(m.hour - 15.5) < 0.01), "true true")
    check("its text", c.timeText:GetText(), "15:30")
    c.weather:SetItem(5)
    c.weather:OnClick(5)
    c.strengthSlider:SetValue(0.6)
    c.strengthSlider:OnValueSet(0.6)
    m = moho64ui_PhotoMode().moment
    check("Weather: rain at 60%", bools(m.weatherName, math.abs(m.strength - 0.6) < 0.01, math.abs(m.hour - 15.5) < 0.01), "rain true true")
    -- the lens: on by the panel, its blur depth
    c.lens:SetItem(2)
    c.lens:OnClick(2)
    c.depthSlider:SetValue(0.5)
    c.depthSlider:OnValueSet(0.5)
    local lens = moho64ui_PhotoMode().lens
    check("Lens on, Blur Depth half way: f/5.5", bools(lens.on, math.abs(lens.fNumber - 5.55) < 0.05), "true true")
    WaitFrames(2)
    check("the picture's lens is on", moho64ui_PhotoMode().lensShown, true)
    -- focus by a left click in the world
    check("focus at first: auto", moho64ui_PhotoMode().lens.focus, 0)
    moho64ui_MouseButton("Left", true, view.Left() + view.Width() * 0.4, view.Top() + view.Height() * 0.6)
    moho64ui_MouseButton("Left", false, view.Left() + view.Width() * 0.4, view.Top() + view.Height() * 0.6)
    local focus = moho64ui_PhotoMode().lens.focus
    LOG(string.format("TEST note: the click's focus: %.1f", focus))
    check("a left click focuses on what is there", focus > 1, true)
    WaitFrames(1)
    check("the panel says so", c.focusText:GetText(), string.format("%.0f away (clicked)", focus))
    c.autoFocus:OnClick()
    check("Auto Focus: auto again", moho64ui_PhotoMode().lens.focus, 0)
    c.lens:SetItem(1)
    c.lens:OnClick(1)

    -- photos: 1x and 2x; the first right after the Time of Day and Weather were set above
    photo(1)
    local first = moho64ui_PhotoMode().photoMean
    photo(2)
    -- the light of a photo taken at once after a change of the moment is the moment's, not the window's old light (the
    -- renderer's 72b06b4: a "fresh" photo builds its light from its own frames): a 1x photo 3 s later is as bright
    WaitSeconds(3)
    photo(1)
    local later = moho64ui_PhotoMode().photoMean
    local function light(m) return (m[1] + m[2] + m[3]) / 3 end
    LOG(string.format("TEST note: the 1x photos' mean colour: at once %.1f %.1f %.1f, 3 s later %.1f %.1f %.1f",
        first[1], first[2], first[3], later[1], later[2], later[3]))
    check("a photo at once after the moment's change has the light of one taken 3 s later (within 5%)",
        first[1] >= 0 and math.abs(light(first) - light(later)) <= math.max(2, 0.05 * light(later)), true)

    -- H hides and shows the panel
    moho64ui_Key("h")
    check("H hides the panel", bools(Panel.State.shown, Panel.Controls.take:IsHidden()), "false true")
    moho64ui_Key("h")
    check("H again shows it", Panel.State.shown, true)
    check("H reached no key map action either (FAF: the nearest factory)", PhotoTestKeys, 0)

    -- Esc leaves
    moho64ui_Key(27)
    s = moho64ui_PhotoMode()
    check("Esc leaves", bools(s.open, Panel.State.open), "false false")
    check("the moment is the game's own again", s.moment.set, false)
    check("the game's panels are back", bools(GameMain.gameUIHidden, GameMain.GetControlCluster():IsHidden()), "false false")
    check("the picture eases back to the game's camera", moho64ui_CineCamera().easingBack, true)
    local after = cam:SaveSettings()
    check("the game's camera is where it was", math.abs(after.Zoom - before.Zoom) < 0.01 and math.abs(after.Heading - before.Heading) < 0.001 and
        math.abs(after.Focus[1] - before.Focus[1]) < 0.01 and math.abs(after.Focus[3] - before.Focus[3]) < 0.01, true)
    check("it lets the game go on", s.paused, false)
    WaitSeconds(1)
    check("and the game goes on (with a beat)", SessionIsPaused(), wasPaused)
    s = moho64ui_PhotoMode()
    check("a second later the picture has eased back", moho64ui_CineCamera().easingBack, false)
    check("the bars and marks are back", bools(s.selectionMarks, s.workBars), "true true")
    check("Esc reached no key map action", PhotoTestKeys, 0)
    moho64ui_Key("w")
    check("closed: the keys reach the key map again", PhotoTestKeys, 1)
    IN_RemoveKeyMapTable({ ['W'] = counter, ['A'] = counter, ['S'] = counter, ['D'] = counter, ['Q'] = counter, ['E'] = counter,
        ['R'] = counter, ['F'] = counter, ['Shift-W'] = counter, ['UpArrow'] = counter, ['Ctrl-W'] = counter, ['H'] = counter,
        ['Esc'] = counter })
    moho64ui_Key("g")
    check("the other keys' actions run again once it is closed", PhotoTestKeys, 2)
    IN_RemoveKeyMapTable(others)

    -- a key of the player's own (Chris: "make sure people can bind a different one"): the key bindings' way, the
    -- action on another key opens and closes it
    IN_AddKeyMapTable({ ['Ctrl-Shift-K'] = { action = 'remaster_photo_mode' } })
    check("Ctrl-Shift-K bound to the photo mode", tostring(moho64ui_KeyMapAction("Ctrl-Shift-K")), "remaster_photo_mode")
    moho64ui_Key("k", "Ctrl-Shift")
    check("the player's own key opens it", moho64ui_PhotoMode().open, true)
    WaitSeconds(0.3)
    moho64ui_Key("k", "Ctrl-Shift")
    check("and closes it again", moho64ui_PhotoMode().open, false)
    WaitSeconds(0.3)
    moho64ui_Key("p", "Alt")
    check("Alt-P still opens it", moho64ui_PhotoMode().open, true)
    WaitSeconds(0.3)
    moho64ui_Key("p", "Alt")
    check("and closes it", moho64ui_PhotoMode().open, false)
    IN_RemoveKeyMapTable({ ['Ctrl-Shift-K'] = { action = 'remaster_photo_mode' } })
    WaitSeconds(1)
end

if not SessionIsReplay() then
    ForkThread(function()
        -- (the game shows its panels once its opening is over, some four seconds in: StopLoadingDialog's
        -- InitialAnimations)
        local waited = 0
        while GameMain.gameUIHidden and waited < 15 do
            WaitSeconds(0.5)
            waited = waited + 0.5
        end
        LOG(string.format("TEST note: the game's panels came at %.1f s", waited))
        WaitSeconds(1)
        common()
        -- the commander walks for the replay
        local cdr = GetArmyAvatars()[1]
        local p = cdr:GetPosition()
        IssueUnitCommand({ cdr }, "Move", { p[1] + 40, p[2], p[3] - 30 })
        WaitSeconds(14)
        local profile = import("/lua/user/prefs.lua").GetCurrentProfile().Name
        CopyCurrentReplay(profile, REPLAY)
        local kept = GetSpecialFileInfo(profile, REPLAY, "Replay")
        check("the game is kept as a replay", kept and kept.SizeBytes > 100, true)
        LOG("TEST done: " .. failed .. " failed")
    end)
    return
end

-- the replay
ForkThread(function()
    check("the game is a replay", SessionIsReplay(), true)
    WaitSeconds(1.5)
    check("the replay runs", SessionIsPaused(), false)
    -- opened during the game's opening, while the game itself still has its panels away: they stay away when the game
    -- brings them (some four seconds in), and come back when it closes
    check("the game's panels are still away (its opening)", GameMain.gameUIHidden, true)
    ConExecute("remaster_photo_mode")
    check("open in a replay", moho64ui_PhotoMode().open, true)
    check("it pauses the replay", moho64ui_PhotoMode().paused, true)
    check("the panels being away already, it leaves them so", Panel.State.hidGameUI, false)
    WaitSeconds(0.5)
    check("and the replay is paused", SessionIsPaused(), true)
    local e0 = eye()
    hold("d", 0.5)
    check("D: it moves", distance(e0, eye()) > 1, true)
    WaitSeconds(4)
    check("past the game's opening its panels stay away", bools(GameMain.gameUIHidden, GameMain.GetControlCluster():IsHidden(), Panel.State.hidGameUI), "true true true")
    moho64ui_Key(27)
    check("Esc leaves", moho64ui_PhotoMode().open, false)
    check("and the game's panels come back", GameMain.gameUIHidden, false)
    WaitSeconds(1)
    check("and the replay goes on", SessionIsPaused(), false)

    -- from the replay's cinematic camera: it takes over (the cinematic camera hands the camera back first)
    ConExecute("remaster_cinecam")
    check("the cinematic camera on", moho64ui_CineCamera().on, true)
    take()
    ConExecute("remaster_photo_mode")
    check("the photo mode takes over from it", bools(moho64ui_PhotoMode().open, moho64ui_CineCamera().camera == ""), "true true")
    ConExecute("remaster_cinecam")
    check("meanwhile the cinematic camera stays off, one line", take(), "remaster_cinecam: the photo mode has the camera; leave it first (Esc)")
    WaitSeconds(0.5)
    -- left open with its panel, a moment of its own, lower and looking out over the land (for a picture at the end)
    Panel.Controls.timeSlider:SetValue(17.5)
    Panel.Controls.timeSlider:OnValueSet(17.5)
    local pitch0 = moho64ui_PhotoMode().camera.pitch
    local x, y = view.Left() + view.Width() * 0.3, view.Top() + view.Height() * 0.9
    for i = 1, 3 do
        moho64ui_MouseButton("Right", true, x, y)
        moho64ui_MouseMove(x, y - view.Height() * 0.4)
        WaitFrames(1)
        moho64ui_MouseMove(x, y - view.Height() * 0.8)
        WaitFrames(1)
        moho64ui_MouseButton("Right", false, x, y - view.Height() * 0.8)
        WaitFrames(2)
    end
    WaitSeconds(0.5)
    local pitch1 = moho64ui_PhotoMode().camera.pitch
    LOG(string.format("TEST note: three drags up: the pitch %.3f -> %.3f", pitch0, pitch1))
    check("a drag up raises the view", pitch1 < pitch0 - 0.2, true)
    hold("f", 1.2)
    hold("w", 0.4)
    check("left open at the end, the panel shown", bools(moho64ui_PhotoMode().open, Panel.State.shown), "true true")
    LOG("TEST done: " .. failed .. " failed")
end)
