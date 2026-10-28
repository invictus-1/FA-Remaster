-- The replay timeline (ours: ui/data/lua/timeline.lua, the host's side ui/app/game_session.cpp and
-- ui/user/replay_timeline.cpp). A multiplayer replay recorded in the original game (the lab's replays/mp folder, as
-- mp_replay.lua) is started from the front end as the game's Replay dialog does (LaunchReplaySession) and watched:
-- the bar is there with the replay's length; the engine's scan (ScanReplayMoments, the whole 1v1: some 30 s, the
-- watched replay held meanwhile) shows its progress, finds the replay's real length and its moments (by kind and army
-- number: a commander's death, his army's defeat, the game's end) and the fighting; every kind of the eight has its
-- marker look and text; a jump forward (by the script, by a click on the track) lands at its beat; a click on the
-- commander's marker jumps to 5 s before his death and selects him; a jump back starts the replay over and lands at
-- its beat with the camera and the army watched kept; docked at the bottom it sits above the game's bottom panels;
-- the X hides the bar, the handle shows near where it was, the key (Ctrl-T) and the console bring it back, it is
-- dragged to another place and docks, and it goes away with the game's panels.
--   moho64ui --init <init.lua> --prefs <a copy> --stage frontend [--no-world] --lua ui/tests/timeline.lua
--            --game-lua ui/tests/timeline.lua --frames 3000000 --log out.log [-- /timeline_end hover|hidden]
-- (from Git Bash with MSYS_NO_PATHCONV=1 and C:/ paths: else its "/..." words are made into paths)
-- The test ends the run itself (ExitApplication) after its last line. `/timeline_end` (the game's own command line,
-- after "--") leaves the bar as it ends for a --screenshot: "hover" (the default: the mouse over a marker, its
-- tooltip shown) or "hidden" (hidden by its X). The jump back restarts the replay: this file runs again in the new
-- interface and goes on from the preferences' note (moho64ui_test_timeline_phase). The replay: MP_REPLAY_FILE if a
-- wrapper sets it, or the game's command line's "/replay <file>", else the lab's 1v1 (beside a worktree in the lab);
-- where it is not there the test is skipped. Its players' names stay in the lab: none of them is written here (the
-- checks go by army number and by what the scripts return).
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local file = rawget(_G, "MP_REPLAY_FILE") or (GetCommandLineArg("/replay", 1) or {})[1] or "../replays/mp/mp2_1v1_abhor_20261016.SCFAReplay"
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
-- wait (frames) until the condition holds; true if it did
local function WaitFor(cond, frames)
    for i = 1, frames do
        if cond() then return true end
        WaitFrames(1)
    end
    return cond() and true or false
end
local phaseKey = "moho64ui_test_timeline_phase"

if not rawget(_G, "SessionIsActive") or not SessionIsActive() then
    -- the front end: the replay starts as from the game's own Replay dialog
    SetPreference(phaseKey, nil)
    ForkThread(function()
        WaitSeconds(1)
        local started = LaunchReplaySession(file)
        if not started then  -- (a machine without the lab's replays: nothing to watch)
            LOG("TEST note: no replay at " .. file .. " (the lab's replays/mp): skipped")
            LOG("TEST done: 0 failed")
            ExitApplication()
            return
        end
        LOG("TEST ok   LaunchReplaySession starts the replay")
    end)
    return
end

local Timeline = import("/moho64ui/lua/timeline.lua")
local GameMain = import("/lua/ui/game/gamemain.lua")
local endWith = GetCommandLineArg("/timeline_end", 1)
endWith = endWith and endWith[1] or "hover"

local function bar() return Timeline.Controls.bar end
local function middle(c) return math.floor((c.Left() + c.Right()) / 2), math.floor((c.Top() + c.Bottom()) / 2) end
local function click(x, y, mods)
    moho64ui_MouseMove(x, y)
    moho64ui_MouseButton("Left", true, x, y, mods)
    WaitFrames(1)
    moho64ui_MouseButton("Left", false, x, y, mods)
    WaitFrames(1)
end
local function markerOf(kind)
    for _, mk in Timeline.Controls.markers or {} do
        if mk.moment.kind == kind then return mk end
    end
end
-- the game's bottom panels on the screen (not hidden or faded out, nor a parent of theirs) that reach into the bar's
-- frame: how many, and how many show at all in the lower part of the screen
local function shows(c)
    local root = GetFrame(0)
    while c and c ~= root do
        if c:IsHidden() or (c.GetAlpha and c:GetAlpha() <= 0) then return false end
        c = c:GetParent()
    end
    return true
end
local function panelsUnderBar()
    local root, b = GetFrame(0), Timeline.Controls.bar
    local l, t, r, btm = b.Left() - 21, b.Top() - 21, b.Right() + 21, b.Bottom() + 21
    local under, shown = 0, 0
    for _, file in { "/lua/ui/game/construction.lua", "/lua/ui/game/unitview.lua" } do
        for _, c in import(file).controls or {} do
            if type(c) == 'table' and c.IsHidden and c.GetParent and c.Top and type(c.Top) ~= 'number' and c ~= root then
                if c.Top() > root.Height() * 0.4 and c.Bottom() > c.Top() and c.Right() > c.Left() and shows(c) then
                    shown = shown + 1
                    if c.Left() < r and c.Right() > l and c.Top() < btm and c.Bottom() > t then under = under + 1 end
                end
            end
        end
    end
    return under, shown
end
local function selectUnits()
    local units = {}
    for i = 0, 4000 do
        local u = GetUnitById(tostring(i))
        if u then table.insert(units, u) end
    end
    SelectUnits(units)
    return table.getn(units)
end
local function waitJump(frames)
    WaitFrames(2)
    return WaitFor(function() local s = moho64ui_ReplayTimeline() return s and not s.seeking end, frames)
end

-- the first interface: the bar, the moments, the jumps forward, then a jump back
local function first()
    check("the game is a replay", SessionIsReplay(), true)
    check("the bar is made", WaitFor(function() return Timeline.State.created end, 300), true)
    -- (the replay held while the scan plays its copy: the watched game stays near its start, for the jumps below)
    SessionRequestPause()
    -- (from now on, each frame: whether the scan's progress was shown while it ran, and the scanned part grew)
    local sawScanning, grew, lastScanned, lastWidth = false, 0, -1, -1
    ForkThread(function()
        while true do
            local now = moho64ui_ReplayTimeline()
            if not now or not now.scanning then return end
            if string.find(Timeline.State.status, "^Scanning %d+%%$") then sawScanning = true end
            local width = Timeline.Controls.scanned and Timeline.Controls.scanned.Width() or 0
            if now.scanned > lastScanned and width > lastWidth and lastScanned >= 0 then grew = grew + 1 end
            lastScanned, lastWidth = now.scanned, width
            WaitFrames(1)
        end
    end)
    local s = moho64ui_ReplayTimeline()
    check("the host has the replay's timeline", s ~= nil, true)
    if not s then return end
    LOG(string.format("TEST note: the replay is %d beats long", s.ticks))
    check("its length is the stream's beats (more than a minute)", s.ticks > 600, true)
    -- (the game puts its panels away for its opening, StopLoadingDialog's InitialAnimations, some four seconds: the bar
    -- goes with them)
    check("shown at first, at the bottom, once the game's panels are there", WaitFor(function() return Timeline.State.visible end, 600) and
        (tostring(Timeline.State.shown) .. " " .. Timeline.State.place), "true bottom")
    local root = GetFrame(0)
    check("docked at the bottom, in the middle", near(bar().Bottom(), root.Bottom(), 60) and near((bar().Left() + bar().Right()) / 2, (root.Left() + root.Right()) / 2, 2), true)
    check("the key at first: Ctrl-T the timeline", tostring(moho64ui_KeyMapAction("Ctrl-T")), "remaster_timeline")
    check("its length on the bar", Timeline.Controls.length:GetText(), "/ " .. string.format("%d:%02d", math.floor(s.ticks / 600), math.mod(math.floor(s.ticks / 10), 60)))

    -- the moments come in while the engine's scan plays the replay a second time (ScanReplayMoments), the scanned part
    -- of the track growing and the progress shown meanwhile
    local beganAt = GameTick()
    local done = WaitFor(function() return not moho64ui_ReplayTimeline().scanning end, 3000000)
    check("the scan finishes", done, true)
    check("its progress shown while it ran (\"Scanning n%\")", sawScanning, true)
    check("and the scanned part of the track grew", grew > 2, true)
    check("the watched replay held meanwhile", near(GameTick(), beganAt, 1), true)
    SessionResume()
    WaitFrames(2)
    s = moho64ui_ReplayTimeline()
    LOG(string.format("TEST note: scanned to game tick %d; the replay's length now %d (its stream's beats less the paused ones)", s.scanned, s.ticks))
    check("played to its end: the length is the tick it ended at", s.ticks, s.scanned)
    check("and the bar says so", Timeline.Controls.length:GetText(), "/ " .. string.format("%d:%02d", math.floor(s.ticks / 600), math.mod(math.floor(s.ticks / 10), 60)))
    local list = moho64ui_ReplayMoments(1)
    check("the moments", table.getn(list), s.moments)
    check("a marker for each", Timeline.State.markers, s.moments)
    local known = { CommanderDied = true, ExperimentalFinished = true, ExperimentalDied = true, ArmyDefeated = true,
        NukeLaunched = true, NukeLanded = true, NukeIntercepted = true, GameEnded = true }
    local seen, unknown, notes = {}, 0, {}
    for _, m in list do
        seen[m.kind] = m
        if not known[m.kind] then unknown = unknown + 1 end
        table.insert(notes, string.format("%s %d army %d", m.kind, m.tick, m.army))
    end
    LOG("TEST note: the moments: " .. table.concat(notes, ", "))
    check("every moment of a kind the bar knows", unknown, 0)
    -- the 1v1: one commander dies, his army is defeated, the game ends with the other army in
    local cdr, out, ended = seen.CommanderDied, seen.ArmyDefeated, seen.GameEnded
    check("a commander died, his army was defeated, the game ended", (cdr and out and ended) and true or false, true)
    if cdr and out and ended then
        check("the defeated army is the commander's, after his death", out.army == cdr.army and out.tick >= cdr.tick, true)
        check("the game ended last, the other army still in", ended.army ~= cdr.army and ended.tick >= out.tick, true)
        check("the commander's moment names his unit and its entity", cdr.unit ~= "" and cdr.entity ~= nil, true)
    end
    check("the status line is empty after the scan", Timeline.State.status, "")
    check("the fighting: a 5 s bucket for every 50 ticks played", s.buckets, math.ceil(s.ticks / 50))
    local damage = 0
    for _, b in moho64ui_ReplayFight(1) do damage = damage + b.damage end
    check("and there was fighting", damage > 0, true)
    -- every kind has its marker look and its text (made-up moments: army 1, a minute in)
    for kind in known do
        local text = Timeline.MomentText({ kind = kind, army = 1, tick = 600, unit = "url0402" })
        check("the kind " .. kind .. ": a marker look and a text with its time", Timeline.MarkerLooks[kind] ~= nil and
            string.find(text, " 1:00$") ~= nil, true)
    end
    check("an intercepted nuke says so", string.find(Timeline.MomentText({ kind = "NukeIntercepted", army = 1, tick = 600 }), " intercepted ") ~= nil, true)
    check("the game's end says so", string.find(Timeline.MomentText({ kind = "GameEnded", army = 1, tick = 600 }), "^Game over, ") ~= nil, true)
    check("a moment of no army says so", string.find(Timeline.MomentText({ kind = "NukeLanded", army = 0, tick = 600 }), "no army") ~= nil, true)
    local shown = 0
    for _, c in Timeline.Controls.markers do if not c:IsHidden() then shown = shown + 1 end end
    check("the markers are on the screen", shown, s.moments)
    -- each marker over its moment's place (or just right of the one before it), above the track
    local inOrder = true
    local last = -1
    for _, mk in Timeline.Controls.markers do
        local x = math.floor(Timeline.XOfTick(mk.moment.tick))
        local nudged = last >= 0 and mk.Left() - last <= mk.Width() + 3  -- (moved right of the one before it)
        if (mk.Left() > x and not nudged) or mk.Left() <= last then inOrder = false end
        last = mk.Left()
        if mk.Bottom() > Timeline.Controls.track.Top() then inOrder = false end
    end
    check("the markers sit above the track, in order of their beats", inOrder, true)

    -- hover: what it was and when
    local cdr = markerOf("CommanderDied")
    local x, y = middle(cdr)
    moho64ui_MouseMove(x, y)
    WaitFrames(2)
    local m = cdr.moment
    local want = "Commander of " .. GetArmiesTable().armiesTable[m.army].nickname .. " destroyed " ..
        string.format("%d:%02d", math.floor(m.tick / 600), math.mod(math.floor(m.tick / 10), 60))
    check("hovering the commander's marker says what and when (\"Commander of <army> destroyed m:ss\")", Timeline.State.hover, want)
    moho64ui_MouseMove(10, 10)
    WaitFrames(2)
    check("away from the markers: no hover text", Timeline.State.hover, false)

    -- a jump forward by the script: the beats run ahead to it, the camera stays
    local cam = GetCamera("WorldCamera")
    local before = cam:SaveSettings()
    local target = math.min(GameTick() + math.floor(s.ticks / 8), s.ticks - 200)  -- (ahead of where it is now)
    local ok, why = Timeline.Seek(target)
    check("a jump forward is taken", ok, true)
    if not ok then LOG("TEST note: " .. tostring(why)) end
    WaitFrames(1)
    check("seeking: the status says so", Timeline.State.status, "Seeking " .. string.format("%d:%02d", math.floor(target / 600), math.mod(math.floor(target / 10), 60)) .. " ...")
    check("the jump forward ends", waitJump(3000), true)
    LOG(string.format("TEST note: forward to beat %d: at %d", target, GameTick()))
    check("it lands at its beat", near(GameTick(), target, 3), true)
    local after = cam:SaveSettings()
    check("the camera stays where it was", near(after.Focus[1], before.Focus[1], 0.5) and near(after.Focus[3], before.Focus[3], 0.5) and near(after.Zoom, before.Zoom, 0.5), true)

    -- a jump forward by a click on the track
    target = math.min(GameTick() + math.floor(s.ticks / 8), s.ticks - 100)
    local tx = math.floor(Timeline.XOfTick(target))
    local _, ty = middle(Timeline.Controls.track)
    click(tx, ty)
    check("a click on the track jumps there", waitJump(3000), true)
    LOG(string.format("TEST note: a click at the place of beat %d: at %d (one pixel is %.1f beats)", target, GameTick(), s.ticks / Timeline.Controls.track.Width()))
    check("it lands at the clicked beat", near(GameTick(), target, s.ticks / Timeline.Controls.track.Width() + 3), true)
    -- (the thumb follows in the bar's OnFrame, which comes after the script threads, as in the original's app frame)
    WaitFrames(1)
    check("the thumb stands there", near(middle(Timeline.Controls.thumb), Timeline.XOfTick(GameTick()), 2), true)

    -- a click on the commander's marker: the replay jumps to 5 s before his death, and he is selected
    local cmk = markerOf("CommanderDied")
    if cmk then
        local cm = cmk.moment
        click(middle(cmk))
        check("a click on a marker jumps", waitJump(6000), true)
        WaitFrames(2)
        LOG(string.format("TEST note: the commander's marker (game tick %d): at %d", cm.tick, GameTick()))
        check("to 5 s before its moment", near(GameTick(), cm.tick - 50, 3), true)
        local sel = GetSelectedUnits() or {}
        check("and selects its unit", table.getn(sel) == 1 and sel[1]:GetEntityId() == tostring(cm.entity), true)
        SelectUnits({})
        WaitFrames(2)
    end

    -- a jump back: the replay starts over; the camera and the army watched are kept
    SetFocusArmy(1)
    WaitFor(function() return GetFocusArmy() == 1 end, 60)
    cam:RestoreSettings({ Focus = { 300, before.Focus[2], 280 }, Zoom = 120, Heading = before.Heading, Pitch = before.Pitch })
    WaitFrames(2)
    local keep = cam:SaveSettings()
    local back = math.max(10, GameTick() - math.floor(s.ticks / 8))
    SetPreference(phaseKey, { phase = 2, tick = back, x = keep.Focus[1], z = keep.Focus[3], zoom = keep.Zoom,
        markers = Timeline.State.markers, buckets = s.buckets })
    SavePreferences()
    ok, why = Timeline.Seek(back)
    check("a jump back is taken", ok, true)
    if not ok then LOG("TEST note: " .. tostring(why)) end
    -- (the host starts the replay over between two frames: this interface goes)
end

-- the second interface: after the jump back
local function second(note)
    check("after the jump back: a new bar", WaitFor(function() return Timeline.State.created end, 300), true)
    local s = moho64ui_ReplayTimeline()
    check("the replay starts over and runs ahead", waitJump(6000), true)
    LOG(string.format("TEST note: back to beat %d: at %d", note.tick, GameTick()))
    check("it lands at its beat", near(GameTick(), note.tick, 3), true)
    check("the game's panels there again: the bar too", WaitFor(function() return Timeline.State.visible end, 600), true)
    WaitFrames(2)
    local cam = GetCamera("WorldCamera"):SaveSettings()
    check("the camera is where it was before the jump", near(cam.Focus[1], note.x, 1) and near(cam.Focus[3], note.z, 1) and near(cam.Zoom, note.zoom, 1), true)
    check("and so is the army watched", GetFocusArmy(), 1)
    check("the moments are kept, not looked for again", tostring(moho64ui_ReplayTimeline().scanning) .. " " .. Timeline.State.markers, "false " .. note.markers)
    check("and the fighting", moho64ui_ReplayTimeline().buckets, note.buckets)

    -- the X hides it; the handle shows where it was when the mouse comes near; the key brings it back
    local root = GetFrame(0)
    local bx, by = middle(Timeline.Controls.bar)
    local cx, cy = middle(Timeline.Controls.close)
    click(cx, cy)
    WaitFrames(2)
    check("the X hides the bar", tostring(Timeline.State.shown) .. " " .. tostring(Timeline.State.visible) .. " " .. tostring(Timeline.Controls.bar:IsHidden()), "false false true")
    check("which the preferences keep", GetPreference("moho64ui_replay_timeline").shown, false)
    moho64ui_MouseMove(math.floor(root.Width() / 2), 20)
    WaitFrames(2)
    check("far from it: no handle either (only the game shows)", Timeline.Controls.handle:IsHidden(), true)
    moho64ui_MouseMove(bx, by)
    WaitFrames(2)
    check("the mouse where it was: the handle shows", Timeline.Controls.handle:IsHidden(), false)
    local hx, hy = middle(Timeline.Controls.handle)
    click(hx, hy)
    WaitFrames(2)
    check("a click on the handle brings it back", tostring(Timeline.State.shown) .. " " .. tostring(Timeline.Controls.bar:IsHidden()), "true false")
    moho64ui_Key("t", "Ctrl")
    WaitFrames(2)
    check("Ctrl-T hides it", tostring(Timeline.State.shown) .. " " .. tostring(Timeline.Controls.bar:IsHidden()), "false true")
    moho64ui_Key("t", "Ctrl")
    WaitFrames(2)
    check("and Ctrl-T brings it back", tostring(Timeline.State.shown) .. " " .. tostring(Timeline.Controls.bar:IsHidden()), "true false")
    ConExecute("remaster_timeline off")
    WaitFrames(2)
    check("remaster_timeline off", Timeline.State.visible, false)
    ConExecute("remaster_timeline on")
    WaitFrames(2)
    check("remaster_timeline on", Timeline.State.visible, true)

    -- it goes away with the game's panels and comes back with them
    GameMain.HideGameUI('on')
    WaitFrames(2)
    check("the game's panels put away: the bar too", Timeline.Controls.bar:IsHidden(), true)
    GameMain.HideGameUI('off')
    WaitFrames(2)
    check("and back with them", Timeline.Controls.bar:IsHidden(), false)

    -- docked at the bottom it sits above the game's bottom panels: they come with a selection, and it moves up; they go,
    -- and it comes down again
    local low = Timeline.Controls.bar.Bottom()
    local under, shown = panelsUnderBar()
    check("no selection: none of the game's bottom panels under the bar", under, 0)
    LOG("TEST note: units selected: " .. selectUnits())
    WaitFrames(3)
    under, shown = panelsUnderBar()
    check("a selection: the construction panel shows", import("/lua/ui/game/construction.lua").controls.constructionGroup:IsHidden(), false)
    check("and the bar is above it and every other bottom panel, its frame too", under .. " (of " .. shown .. ")", "0 (of " .. shown .. ")")
    check("it moved up for them", Timeline.Controls.bar.Bottom() < low, true)
    check("still in the middle of the screen's width", near((Timeline.Controls.bar.Left() + Timeline.Controls.bar.Right()) / 2, (root.Left() + root.Right()) / 2, 2), true)
    local mk = Timeline.Controls.markers[1]
    check("its markers came with it", near(mk.Top(), Timeline.Controls.bar.Top() + 1, 1), true)
    LOG(string.format("TEST note: the bar's bottom %d without the panels, %d with them (their top %d)", low, Timeline.Controls.bar.Bottom(), Timeline.State.panelsTop))
    SelectUnits({})
    WaitFrames(3)
    check("the selection gone: down again", Timeline.Controls.bar.Bottom(), low)

    -- dragged by its frame: dropped in the middle it stays there, near the top it docks there, then at the bottom
    local function drag(toX, toY)
        local fx, fy = Timeline.Controls.bar.Left() + 6, Timeline.Controls.bar.Bottom() - 4
        moho64ui_MouseMove(fx, fy)
        moho64ui_MouseButton("Left", true, fx, fy)
        WaitFrames(1)
        moho64ui_MouseMove(math.floor((fx + toX) / 2), math.floor((fy + toY) / 2))
        WaitFrames(1)
        moho64ui_MouseMove(toX, toY)
        WaitFrames(1)
        moho64ui_MouseButton("Left", false, toX, toY)
        WaitFrames(2)
    end
    local b = Timeline.Controls.bar
    drag(b.Left() + 6, math.floor(root.Height() / 2))
    check("dropped in the middle of the screen: it stays there", Timeline.State.place, "free")
    check("with its markers and the fighting strip", near(Timeline.Controls.markers[1].Top(), b.Top() + 1, 1) and
        near(Timeline.Controls.thumb.Top() + Timeline.Controls.thumb.Height() / 2, b.Top() + 30, 2), true)
    check("where it was dropped", near(b.Bottom() - 4, root.Height() / 2, 2), true)
    check("which the preferences keep", GetPreference("moho64ui_replay_timeline").place, "free")
    drag(b.Left() + 6, 100)
    check("near the top: docked at the top", Timeline.State.place, "top")
    check("in the middle of the screen's width", near((b.Left() + b.Right()) / 2, (root.Left() + root.Right()) / 2, 2), true)
    drag(b.Left() + 6, root.Bottom() - 10)
    check("near the bottom: docked at the bottom again", Timeline.State.place, "bottom")
    check("which the preferences keep", GetPreference("moho64ui_replay_timeline").place, "bottom")

    -- the end, for a picture: the mouse over the commander's marker (its tooltip), or the bar hidden
    if endWith == "hidden" then
        Timeline.Hide()
        moho64ui_MouseMove(math.floor(root.Width() / 2), math.floor(root.Height() / 3))
        WaitFrames(30)
        check("at the end: hidden", Timeline.Controls.bar:IsHidden(), true)
    else
        selectUnits()  -- (the picture: the bar above the construction panel)
        WaitFrames(3)
        local under = panelsUnderBar()
        check("at the end: above the construction panel", under, 0)
        local mk = markerOf("NukeLanded") or markerOf("CommanderDied")
        local x, y = middle(mk)
        moho64ui_MouseMove(x, y)
        WaitFrames(30)
        check("at the end: hovering a marker", Timeline.State.hover ~= false, true)
    end
end

ForkThread(function()
    local note = GetPreference(phaseKey)
    if note and note.phase == 2 then
        SetPreference(phaseKey, nil)
        SavePreferences()
        failed = note.failed or 0
        second(note)
        LOG("TEST done: " .. failed .. " failed")
        ExitApplication()
        return
    end
    if note and note.phase == 1 then  -- (the replay started over without the test's jump back: stop here)
        SetPreference(phaseKey, nil)
        SavePreferences()
        failed = note.failed or 0
        check("the replay starts over only for the jump back", false, true)
        LOG("TEST done: " .. failed .. " failed")
        ExitApplication()
        return
    end
    SetPreference(phaseKey, { phase = 1 })
    SavePreferences()
    first()
    if failed > 0 and GetPreference(phaseKey).phase ~= 2 then  -- (no jump back asked: nothing comes after)
        LOG("TEST done: " .. failed .. " failed")
        ExitApplication()
        return
    end
    -- the failures so far go with the jump back into the second interface's count
    local n = GetPreference(phaseKey)
    if n then
        n.failed = failed
        SetPreference(phaseKey, n)
        SavePreferences()
    end
end)
