-- The cinematic camera for replays (ours: the renderer's CineCamera, maui/worldview.cpp) and the photo. A game is
-- played and kept as a replay, then played back from the front end, as replay_record.lua and replay_play.lua do;
-- the same file serves the three states:
--   the game played (moho64ui --stage setup --session session_ai.lua --seed 7 --game-lua ui/tests/cinecam.lua
--     --frames 1500): the camera is refused there with one line, the photo is not; the commander walks; the
--     replay is kept as "moho64_cinecam";
--   the front end, then the replay (with the same preferences folder: --stage frontend --lua ui/tests/cinecam.lua
--     --game-lua ui/tests/cinecam.lua --frames 2400): on, it follows the selected unit; the bars and marks are left
--     out meanwhile and come back as they were; a wheel notch, the arrow keys, the middle button, a script's Reset
--     and the command itself hand the camera back, the picture easing back; the photo comes from it at twice the
--     view's size. It is left on at the end, following the commander (for a --screenshot).
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
local function flat(a, b) return math.sqrt((a[1] - b[1]) * (a[1] - b[1]) + (a[3] - b[3]) * (a[3] - b[3])) end
local function bools(...)
    local out = {}
    for i = 1, arg.n do table.insert(out, tostring(arg[i])) end
    return table.concat(out, " ")
end
local REPLAY = "moho64_cinecam"

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
local view = import("/lua/ui/game/worldview.lua").viewLeft
-- The photo: the console's line says where; the file is a PNG at twice the main view's size.
local function photo(what)
    ConExecute("remaster_photo")
    local line = take()
    local path = string.match(line, "^Photo saved: (.+) %(%d+x%d+%)$")
    check(what .. ": a console line says where the photo is", path ~= nil, true)
    if not path then
        LOG("TEST note: the console said \"" .. line .. "\"")
        return
    end
    LOG("TEST note: the photo " .. path)
    check(what .. ": in the preferences' folder's screenshots, named by the date and time",
        string.find(path, "/screenshots/photo_%d%d%d%d%-%d%d%-%d%d_%d%d%-%d%d%-%d%d") ~= nil, true)
    local info = moho64ui_PngInfo(path)
    check(what .. ": the file is a PNG", info ~= nil, true)
    local want = string.format("%dx%d", 2 * view.Width(), 2 * view.Height())
    check(what .. ": at twice the view's size", info and (info.width .. "x" .. info.height), want)
    check(what .. ": and holds the pixels (stored: 3 bytes each)", info and info.bytes > info.width * info.height * 3, true)
    check(what .. ": the host says so too", moho64ui_CineCamera().photo, path)
end

if not SessionIsReplay() then
    -- the game played: the camera stays off with one line; the photo is taken; the replay is kept
    ForkThread(function()
        WaitSeconds(3)
        check("the keys at first: Ctrl-Alt-C the cinematic camera, Ctrl-Alt-F12 the photo",
            tostring(moho64ui_KeyMapAction("Ctrl-Alt-C")) .. " " .. tostring(moho64ui_KeyMapAction("Ctrl-Alt-F12")), "remaster_cinecam remaster_photo")
        local cdr = GetArmyAvatars()[1]
        local c = cdr:GetPosition()
        IssueUnitCommand({cdr}, "Move", {c[1] + 40, c[2], c[3] - 30})  -- (so the replay has a unit walking)
        take()
        ConExecute("remaster_cinecam")
        check("in a game one plays the cinematic camera says it is for replays, one line", take(),
            "remaster_cinecam: the cinematic camera is for replays; in a game you play it stays off")
        ConExecute("remaster_cinecam on")
        check("on as well", take(), "remaster_cinecam: the cinematic camera is for replays; in a game you play it stays off")
        check("and it stays off", bools(moho64ui_CineCamera().on, moho64ui_CineCamera().camera == ""), "false true")
        photo("in a game one plays")
        WaitSeconds(16)
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
    WaitSeconds(3)
    local cdr = GetUnitById("0")
    check("the commander is there", cdr ~= nil, true)
    if not cdr then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    local cam = GetCamera("WorldCamera")
    local before = cam:SaveSettings()
    local s0 = moho64ui_CineCamera()
    check("before: off", bools(s0.on, s0.easingBack), "false false")
    check("before: the picture has the player's life bars, the selection's marks and the work bars",
        bools(s0.lifeBars, s0.selectionMarks, s0.workBars), bools(moho64ui_LifeBars(), true, true))

    -- on, following the selected unit (once the commander can be selected: not while he warps in)
    local waited = 0
    repeat
        SelectUnits({cdr})
        local selected = GetSelectedUnits()
        if selected and table.getn(selected) == 1 then break end
        WaitSeconds(0.5)
        waited = waited + 0.5
    until waited > 15
    check("the commander is selected", table.getn(GetSelectedUnits() or {}), 1)
    take()
    ConExecute("remaster_cinecam")
    local s = moho64ui_CineCamera()
    check("remaster_cinecam (no word) in a replay: on, following the selected unit, silently",
        bools(s.on, s.following, take() == ""), "true true true")
    check("it took over from the main view's camera", s.camera, "WorldCamera")
    check("while it shows: no life bars, selection marks or work bars in the picture", bools(s.lifeBars, s.selectionMarks, s.workBars), "false false false")
    WaitSeconds(2)
    s = moho64ui_CineCamera()
    check("two seconds on: still on by itself", bools(s.on, s.following), "true true")
    local p = cdr:GetPosition()
    LOG(string.format("TEST note: the unit at %.1f %.1f %.1f, the place circled %.1f %.1f %.1f", p[1], p[2], p[3], s.followed[1], s.followed[2], s.followed[3]))
    check("the place it circles is the unit's (as the frame shows it)", flat(s.followed, p) < 3, true)
    check("the game's camera stays where it was meanwhile", math.abs(cam:SaveSettings().Zoom - before.Zoom) < 0.01, true)
    ConExecute("remaster_cinecam on")
    check("on again: nothing changes", bools(moho64ui_CineCamera().on, take() == ""), "true true")

    -- the lens (the player's Cinematic Lens Blur): only while it shows
    ConExecute("remaster_lens 1")
    check("Cinematic Lens Blur on", moho64ui_Looks().lens, true)
    WaitFrames(2)
    check("the picture's lens is on while the cinematic camera shows", moho64ui_CineCamera().lens, true)
    photo("from the cinematic camera")

    -- a wheel notch over the view hands the camera back: the game's camera goes on from where it was
    local mx, my = view.Left() + view.Width() / 2, view.Top() + view.Height() / 2
    moho64ui_MouseWheel(mx, my, 1)
    s = moho64ui_CineCamera()
    check("a wheel notch hands the camera back: off, the picture easing back", bools(s.on, s.easingBack, s.camera == ""), "false true true")
    local f = cam:GetFocusPosition()
    LOG(string.format("TEST note: after the notch the focus %.1f %.1f, zoom %.1f -> %.1f (before %.1f)", f[1], f[3], cam:GetZoom(), cam:GetTargetZoom(), before.Zoom))
    check("the game's camera goes on from the cinematic one's view: near the unit, zoomed in", flat(f, cdr:GetPosition()) < 10 and cam:GetTargetZoom() < 40, true)
    check("and in the usual view: north up (Chris, 2026-10-18: it glides back to the normal view)", math.abs(cam:SaveSettings().Heading - math.pi) < 0.01, true)
    WaitSeconds(1)
    s = moho64ui_CineCamera()
    check("a second later it has eased back", s.easingBack, false)
    check("the bars and marks are back as they were", bools(s.lifeBars, s.selectionMarks, s.workBars), bools(s0.lifeBars, s0.selectionMarks, s0.workBars))
    check("and the lens is off again", s.lens, false)
    ConExecute("remaster_lens 0")
    check("Cinematic Lens Blur off", moho64ui_Looks().lens, false)

    -- the other ways back: the command, the arrow keys, the middle button, a script's move
    ConExecute("remaster_cinecam on")
    ConExecute("remaster_cinecam off")
    s = moho64ui_CineCamera()
    check("remaster_cinecam off: off, easing back", bools(s.on, s.easingBack), "false true")
    ConExecute("remaster_cinecam")
    moho64ui_Key(316, "", "down")  -- (the left arrow held: the camera's frame pans)
    WaitFrames(2)
    moho64ui_Key(316, "", "up")
    check("an arrow key hands it back", moho64ui_CineCamera().on, false)
    ConExecute("remaster_cinecam")
    moho64ui_MouseButton("Middle", true, mx, my)
    check("the middle button's press hands it back", moho64ui_CineCamera().on, false)
    moho64ui_MouseButton("Middle", false, mx, my)
    ConExecute("remaster_cinecam")
    cam:Reset()
    check("a script's camera Reset hands it back", moho64ui_CineCamera().on, false)
    ConExecute("remaster_cinecam")
    cam:MoveTo(cdr:GetPosition(), {3.14159, 0.8, 0}, 60, 0)
    check("a script's MoveTo too", moho64ui_CineCamera().on, false)
    -- with nothing selected it holds where it is
    SelectUnits({})
    ConExecute("remaster_cinecam")
    s = moho64ui_CineCamera()
    check("with nothing selected: on, holding", bools(s.on, s.following), "true false")
    ConExecute("remaster_cinecam")
    WaitSeconds(1)

    -- left on, following the commander (for a picture at the end)
    SelectUnits({cdr})
    ConExecute("remaster_cinecam")
    check("on again at the end, following", bools(moho64ui_CineCamera().on, moho64ui_CineCamera().following), "true true")
    LOG("TEST done: " .. failed .. " failed")
end)
