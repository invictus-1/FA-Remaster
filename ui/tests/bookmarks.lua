-- The replay bookmarks (ours: ui/data/lua/timeline.lua, the host's side ui/user/replay_timeline.cpp and
-- ui/app/game_session.cpp) and the camera spots in a replay (ui/data/hook/lua/usercamera.lua). A COPY of a replay
-- recorded in the original game (the lab's replays/mp 1v1, copied by the suite into the run's folder: the bookmarks
-- are written beside the replay) is started from the front end as the game's Replay dialog does. Cases (after "--"):
--   /bmcase first   none at first; the keys in the key map and the command palette; one added by the key where the
--                   replay is (named by its time), one by the bar's button, none twice at one beat, one by a right click
--                   on the track with its name typed at once; Escape leaves a name as it was, an empty one is its time
--                   again; a name made plain (one line, at most 64 bytes); the flags at their beats, of their own
--                   colour, under the track; hover says the name and time; a click on a flag and the next key jump
--                   forward; one removed by its panel; camera spot 3 saved (Ctrl-Alt-3) and gone to (Alt-3); then the key
--                   for the one before jumps back: the replay starts over, this file runs again in the new interface
--                   (the preferences' note) and finds the same bookmarks and the camera spot there; the list is noted
--                   in the preferences for "again".
--   /bmcase again   the same replay opened again (a new run on the same preferences): its bookmarks read from the file
--                   beside it, as "first" left them; the camera spot 3 of its map from the profile.
--   /bmcase other   a copy whose bookmarks file belongs to another replay (the suite writes one with another replay's
--                   digest): none taken (the file is left as it is).
--   moho64ui --init <init.lua> --prefs <a copy> --stage frontend --size 1920x1080 --lua ui/tests/bookmarks.lua
--            --game-lua ui/tests/bookmarks.lua --frames 60000 --log out.log -- /replay <a copy> /bmcase first
-- (from Git Bash with MSYS_NO_PATHCONV=1 and C:/ paths: else its "/..." words are made into paths). Without /replay
-- the test fails at once: it writes beside the replay, so it never takes the lab's own. /bm_end hover|popup leaves
-- "first" with the mouse over a flag (its tooltip) or a bookmark's panel open, for a picture (--screenshot, or with a
-- window moho64ui_ReplayCaptureForTests to /bm_capture <prefix>: <prefix>_flags.png, _popup.png, _hover.png).
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local function arg(name)
    local a = GetCommandLineArg(name, 1)
    return a and a[1]
end
local file = arg("/replay")
local case = arg("/bmcase") or "first"
local endWith = arg("/bm_end") or "hover"
local capture = arg("/bm_capture")
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
local phaseKey = "moho64ui_test_bookmarks_phase"
local expectKey = "moho64ui_test_bookmarks_expect"

if not rawget(_G, "SessionIsActive") or not SessionIsActive() then
    -- the front end: the replay starts as from the game's own Replay dialog
    SetPreference(phaseKey, nil)
    SavePreferences()
    ForkThread(function()
        WaitSeconds(1)
        if not file then
            LOG("TEST FAIL no /replay <a copy> given: the test writes beside the replay, so it never takes the lab's own")
            LOG("TEST done: 1 failed")
            ExitApplication()
            return
        end
        if not LaunchReplaySession(file) then
            LOG("TEST FAIL LaunchReplaySession " .. file)
            LOG("TEST done: 1 failed")
            ExitApplication()
            return
        end
        LOG("TEST ok   LaunchReplaySession starts the replay copy")
    end)
    return
end

local Timeline = import("/moho64ui/lua/timeline.lua")
local LayoutHelpers = import("/lua/maui/layouthelpers.lua")
local function middle(c) return math.floor((c.Left() + c.Right()) / 2), math.floor((c.Top() + c.Bottom()) / 2) end
local function click(x, y, button)
    moho64ui_MouseMove(x, y)
    moho64ui_MouseButton(button or "Left", true, x, y)
    WaitFrames(1)
    moho64ui_MouseButton(button or "Left", false, x, y)
    WaitFrames(1)
end
local function TimeText(tick)
    local s = math.floor(tick / 10)
    if s >= 3600 then return string.format("%d:%02d:%02d", math.floor(s / 3600), math.floor(math.mod(s, 3600) / 60), math.mod(s, 60)) end
    return string.format("%d:%02d", math.floor(s / 60), math.mod(s, 60))
end
local function waitJump(frames)
    WaitFrames(2)
    return WaitFor(function() local s = moho64ui_ReplayTimeline() return s and not s.seeking end, frames)
end
local function list() return moho64ui_ReplayBookmarks() or {} end
local function describe(l)
    local out = {}
    for _, b in l do table.insert(out, b.tick .. "=" .. b.name) end
    return table.concat(out, "|")
end
-- the replay held at the beat it is at (so that "where it is" stands still for a check)
local function hold()
    SessionRequestPause()
    WaitFrames(3)
    return GameTick()
end
local function capturePicture(what)
    if capture then
        moho64ui_ReplayCaptureForTests(capture .. "_" .. what .. ".png")
        WaitFrames(3)
    end
end
local cam = function() return GetCamera("WorldCamera") end
local function camNear(a, b)
    return a and b and near(a.Focus[1], b.Focus[1], 1) and near(a.Focus[3], b.Focus[3], 1) and near(a.Zoom, b.Zoom, 1)
end

-- the first interface of "first"
local function first()
    check("the game is a replay", SessionIsReplay(), true)
    check("the bar is made", WaitFor(function() return Timeline.State.created end, 300), true)
    local s = moho64ui_ReplayTimeline()
    LOG("TEST note: the bookmarks' file: " .. tostring(s.bookmarksFile))
    check("the bookmarks' file is beside the replay: <replay>.bookmarks.txt", s.bookmarksFile, file .. ".bookmarks.txt")
    -- (the game puts its panels away for its opening, StopLoadingDialog's InitialAnimations: the bar goes with them)
    check("the bar on the screen once the game's panels are there", WaitFor(function() return Timeline.State.visible end, 3600), true)
    if capture then  -- (the pictures: the game's moments on the bar too, the replay held while the scan finds them)
        SessionRequestPause()
        WaitFor(function() return not moho64ui_ReplayTimeline().scanning end, 3000000)
        SessionResume()
    end
    check("none at first", table.getn(list()) .. " " .. Timeline.State.bookmarks .. " " .. table.getn(Timeline.Controls.flags), "0 0 0")
    check("the bar's bookmark button is there", Timeline.Controls.bookmarkButton ~= nil and not Timeline.Controls.bookmarkButton:IsHidden(), true)
    -- the keys: in the key map at their keys (FAF's preset leaves them free), named in the key map and the palette
    check("Ctrl-Shift-B: add a bookmark", string.find(moho64ui_KeyMapAction("Ctrl-Shift-B") or "", "AddBookmarkHere", 1, true) ~= nil, true)
    check("Ctrl-Shift-Period: the next one", string.find(moho64ui_KeyMapAction("Ctrl-Shift-Period") or "", "NextBookmark", 1, true) ~= nil, true)
    check("Ctrl-Shift-Comma: the one before", string.find(moho64ui_KeyMapAction("Ctrl-Shift-Comma") or "", "PreviousBookmark", 1, true) ~= nil, true)
    local KeyMapper = import("/lua/keymap/keymapper.lua")
    check("its name in the key map", KeyMapper.GetActionName('remaster_bookmark_add'), "Replay bookmark: add one at this moment")
    local Palette = import("/moho64ui/lua/palette.lua")
    moho64ui_Key("p", "Ctrl Shift")
    check("the command palette opens", Palette.State.open, true)
    Palette.Controls.edit:SetText("replay bookmark")
    local found = {}
    for _, row in Palette.State.rows do
        if row.kind == 'action' and string.find(row.name, "^Replay bookmark") then table.insert(found, row.name .. " [" .. tostring(row.keyText) .. "]") end
    end
    table.sort(found)
    LOG("TEST note: the palette: " .. table.concat(found, ", "))
    check("the palette shows the three with their keys", table.getn(found) == 3 and string.find(found[1], "%[nil%]") == nil and
        string.find(found[2], "%[nil%]") == nil and string.find(found[3], "%[nil%]") == nil, true)
    moho64ui_Key(27)
    check("and closes", Palette.State.open, false)

    -- by the key, where the replay is: named by its time
    check("forward to 1:00", Timeline.Seek(600) and waitJump(6000), true)
    local t1 = hold()
    moho64ui_Key("b", "Ctrl Shift")
    WaitFrames(2)
    local l = list()
    check("Ctrl-Shift-B: a bookmark where the replay is, named by its time", describe(l), t1 .. "=" .. TimeText(t1))
    check("a flag for it", table.getn(Timeline.Controls.flags) .. " " .. Timeline.State.bookmarks, "1 1")
    check("the status place says so", Timeline.State.status, "Bookmarked " .. TimeText(t1))
    -- by the bar's button
    SessionResume()
    check("forward to 2:30", Timeline.Seek(1500) and waitJump(6000), true)
    local t2 = hold()
    click(middle(Timeline.Controls.bookmarkButton))
    check("the bar's button: a bookmark where the replay is", describe(list()), t1 .. "=" .. TimeText(t1) .. "|" .. t2 .. "=" .. TimeText(t2))
    moho64ui_Key("b", "Ctrl Shift")
    WaitFrames(2)
    check("none twice at one beat", table.getn(list()), 2)
    -- a right click on the track: the name box of a bookmark at the beat under the mouse first (Chris, 2026-11-07), the
    -- bookmark made by Enter; Escape makes none
    local _, ty = middle(Timeline.Controls.track)
    local x0 = math.floor(Timeline.XOfTick(1800))
    click(x0, ty, "Right")
    WaitFrames(2)
    check("a right click on the track opens a name box, no bookmark yet", tostring(Timeline.State.popup) .. " " ..
        table.getn(list()) .. " " .. tostring(Timeline.Controls.popupRemove == nil), "new 2 true")
    moho64ui_Key(27)
    WaitFrames(2)
    check("Escape there makes none", tostring(Timeline.State.popup) .. " " .. table.getn(list()), "false 2")
    local x3 = math.floor(Timeline.XOfTick(2400))
    local t3 = Timeline.TickAtX(x3)
    click(x3, ty, "Right")
    WaitFrames(2)
    check("its name line taking the keys", tostring(Timeline.State.popup) .. " " ..
        tostring(Timeline.Controls.popupEdit and Timeline.Controls.popupEdit:GetCurrentFocusControl() == Timeline.Controls.popupEdit), "new true")
    check("the panel above the bar", Timeline.Controls.popup and Timeline.Controls.popup.Bottom() < Timeline.Controls.bar.Top(), true)
    moho64ui_Type("First raid")
    capturePicture("popup")
    moho64ui_Key(13)
    WaitFrames(2)
    local i3
    for i, b in list() do if b.tick == t3 then i3 = i end end
    check("Enter makes the bookmark at the beat there", i3 ~= nil, true)
    if not i3 then return end
    check("with the name typed; the panel goes", tostring(list()[i3].name) .. " " .. tostring(Timeline.State.popup), "First raid false")
    -- the flags: at their beats, under the track, of their own colour
    local placed = true
    for i, f in Timeline.Controls.flags do
        if not near(f.Left() + LayoutHelpers.ScaleNumber(3), Timeline.XOfTick(list()[i].tick), 1) then placed = false end
        if f.Top() < Timeline.Controls.track.Bottom() then placed = false end
    end
    check("each flag at its beat, under the track", placed, true)
    local own = true
    for _, look in Timeline.MarkerLooks do if look.frame == Timeline.FlagColor then own = false end end
    check("of a colour none of the moments' markers has", own and Timeline.FlagColor ~= nil, true)
    -- hover: its name and time
    local f3 = Timeline.Controls.flags[i3]
    moho64ui_MouseMove(middle(f3))
    WaitFrames(2)
    check("hovering its flag says its name and time", Timeline.State.hover, "First raid " .. TimeText(t3))
    moho64ui_MouseMove(middle(Timeline.Controls.flags[1]))
    WaitFrames(2)
    check("one named by its time says \"Bookmark m:ss\"", Timeline.State.hover, "Bookmark " .. TimeText(t1))
    moho64ui_MouseMove(10, 10)
    WaitFrames(2)
    -- a right click on its flag: the panel with its name; Escape leaves it as it was
    click(Timeline.Controls.flags[i3].Left() + 4, Timeline.Controls.flags[i3].Top() + 3, "Right")
    WaitFrames(2)
    check("a right click on a flag opens its panel with its name", tostring(Timeline.State.popup) .. " " ..
        tostring(Timeline.Controls.popupEdit and Timeline.Controls.popupEdit:GetText()), i3 .. " First raid")
    Timeline.Controls.popupEdit:SetText("not this")
    moho64ui_Key(27)
    WaitFrames(2)
    check("Escape: the name as it was, the panel gone", tostring(list()[i3].name) .. " " .. tostring(Timeline.State.popup), "First raid false")
    -- an empty name is its time again; a name made plain
    Timeline.OpenBookmark(i3)
    Timeline.Controls.popupEdit:SetText("")
    moho64ui_Key(13)
    WaitFrames(2)
    check("an empty name: its time again", list()[i3].name, TimeText(t3))
    Timeline.RenameBookmark(i3, "First raid")
    local i4 = Timeline.AddBookmark(3000, "  tab\there\nnew   line  " .. string.rep("x", 80))
    local n4 = list()[i4].name
    check("a name made plain: one line, blanks run together", string.find(n4, "^tab here new line xx") ~= nil and string.find(n4, "[%c]") == nil, true)
    check("and at most 64 bytes", string.len(n4) <= 64 and string.len(n4) >= 60, true)
    Timeline.RenameBookmark(i4, "The push")
    check("four now, in the order of their beats", describe(list()), t1 .. "=" .. TimeText(t1) .. "|" .. t2 .. "=" .. TimeText(t2) .. "|" ..
        t3 .. "=First raid|3000=The push")
    capturePicture("flags")

    -- the next key jumps forward to the next one; a click on a flag jumps to its beat
    SessionResume()
    moho64ui_Key(".", "Ctrl Shift", "", 190)
    check("Ctrl-Shift-. jumps to the next bookmark", waitJump(6000) and near(GameTick(), t3, 3), true)
    LOG(string.format("TEST note: the next: at %d (bookmark %d)", GameTick(), t3))
    click(Timeline.Controls.flags[4].Left() + 4, Timeline.Controls.flags[4].Top() + 3)
    check("a click on a flag jumps to its beat", waitJump(6000) and near(GameTick(), 3000, 3), true)
    -- removed by its panel
    click(Timeline.Controls.flags[2].Left() + 4, Timeline.Controls.flags[2].Top() + 3, "Right")
    WaitFrames(2)
    check("the second's panel open", Timeline.State.popup, 2)
    click(middle(Timeline.Controls.popupRemove))
    WaitFrames(2)
    check("Remove takes it away; the panel goes", describe(list()) .. " " .. tostring(Timeline.State.popup),
        t1 .. "=" .. TimeText(t1) .. "|" .. t3 .. "=First raid|3000=The push false")
    check("and its flag", table.getn(Timeline.Controls.flags), 3)

    -- a camera spot in a replay: Ctrl-Alt-3 saves it, Alt-3 goes there
    local c = cam()
    local base = c:SaveSettings()
    local spot = { Focus = { base.Focus[1] + 40, base.Focus[2], base.Focus[3] - 30 }, Zoom = 90, Heading = base.Heading, Pitch = base.Pitch }
    c:RestoreSettings(spot)
    WaitFrames(2)
    spot = c:SaveSettings()
    moho64ui_Key("3", "Ctrl Alt")
    WaitFrames(1)
    c:RestoreSettings(base)
    WaitFrames(2)
    moho64ui_Key("3", "Alt")
    WaitFrames(2)
    check("a replay: Ctrl-Alt-3 saves camera spot 3, Alt-3 goes there", camNear(c:SaveSettings(), spot), true)
    local map = import("/lua/usercamera.lua").RemasterSpotsMapKey()
    local saved = import("/lua/usercamera.lua").RemasterSavedSpots(map)
    check("kept for the replay's map in the profile", saved["3"] ~= nil, true)
    c:RestoreSettings(base)
    WaitFrames(2)

    -- the key for the one before: back to "First raid", a jump back (the replay starts over)
    local note = { phase = 2, back = t3, list = describe(list()), spot = { x = spot.Focus[1], z = spot.Focus[3], zoom = spot.Zoom }, failed = failed }
    SetPreference(phaseKey, note)
    SavePreferences()
    moho64ui_Key(",", "Ctrl Shift", "", 188)
    -- (the host starts the replay over between two frames: this interface goes)
end

-- the second interface of "first": after the jump back
local function second(note)
    check("after the jump back: a new bar", WaitFor(function() return Timeline.State.created end, 300), true)
    check("the jump back lands at the bookmark before", waitJump(20000) and near(GameTick(), note.back, 3), true)
    LOG(string.format("TEST note: back to %d: at %d", note.back, GameTick()))
    check("the same bookmarks, kept by the host", describe(list()), note.list)
    check("their flags made anew", table.getn(Timeline.Controls.flags), table.getn(list()))
    local c = cam()
    c:RestoreSettings({ Focus = { note.spot.x - 60, 0, note.spot.z + 50 }, Zoom = 60, Heading = c:SaveSettings().Heading, Pitch = c:SaveSettings().Pitch })
    WaitFrames(2)
    moho64ui_Key("3", "Alt")
    WaitFrames(2)
    local s = c:SaveSettings()
    check("Alt-3 in the new interface: the camera spot saved before the jump", near(s.Focus[1], note.spot.x, 1) and near(s.Focus[3], note.spot.z, 1) and near(s.Zoom, note.spot.zoom, 1), true)
    SetPreference(expectKey, { list = describe(list()), spot = note.spot })
    SavePreferences()
    -- the end, for a picture: the mouse over a flag (its tooltip), or a bookmark's panel open
    check("the game's panels there again: the bar too", WaitFor(function() return Timeline.State.visible end, 900), true)
    if endWith == "popup" then
        Timeline.OpenBookmark(2)
        WaitFrames(30)
        check("at the end: a bookmark's panel open", Timeline.State.popup, 2)
    else
        local f = Timeline.Controls.flags[2]
        moho64ui_MouseMove(f.Left() + 4, f.Top() + 3)
        WaitFrames(30)
        check("at the end: hovering a flag", Timeline.State.hover ~= false, true)
    end
    capturePicture("hover")
end

-- "again": the same replay opened anew
local function again()
    check("the bar is made", WaitFor(function() return Timeline.State.created end, 300), true)
    local want = GetPreference(expectKey)
    check("\"first\" left its bookmarks noted", want ~= nil, true)
    if not want then return end
    check("opened again: the bookmarks from the file beside it", describe(list()), want.list)
    check("their flags", table.getn(Timeline.Controls.flags), table.getn(list()))
    WaitFrames(60)
    local c = cam()
    moho64ui_Key("3", "Alt")
    WaitFrames(2)
    local s = c:SaveSettings()
    check("Alt-3 in another run of the replay: its map's camera spot 3", near(s.Focus[1], want.spot.x, 1) and near(s.Focus[3], want.spot.z, 1) and near(s.Zoom, want.spot.zoom, 1), true)
end

-- "other": a file beside the replay that belongs to another one
local function other()
    check("the bar is made", WaitFor(function() return Timeline.State.created end, 300), true)
    check("another replay's bookmarks file: none taken", table.getn(list()) .. " " .. table.getn(Timeline.Controls.flags), "0 0")
end

ForkThread(function()
    local note = GetPreference(phaseKey)
    if case == "first" and note and note.phase == 2 then
        SetPreference(phaseKey, nil)
        SavePreferences()
        failed = note.failed or 0
        second(note)
    elseif case == "first" and note and note.phase == 1 then  -- (the replay started over without the test's jump back)
        SetPreference(phaseKey, nil)
        SavePreferences()
        failed = (note.failed or 0) + 1
        LOG("TEST FAIL the replay starts over only for the jump back")
    elseif case == "first" then
        SetPreference(phaseKey, { phase = 1 })
        SavePreferences()
        first()
        local n = GetPreference(phaseKey)
        if n and n.phase == 2 then
            -- (the jump back makes a new interface, whose run of this file takes the note away; this thread, if it
            -- lives on, then ends quietly; if no new interface comes, the test ends here)
            if WaitFor(function() return GetPreference(phaseKey) == nil end, 30000) then return end
            LOG("TEST FAIL the key for the one before made no jump back")
            failed = failed + 1
        end
    elseif case == "again" then
        again()
    else
        other()
    end
    LOG("TEST done: " .. failed .. " failed")
    ExitApplication()
end)
