-- The long-frame log's limit (ours: ui/user/user_state.h, LongFrameLimit; the renderer's request 79, 2): at most 30
-- long-frame lines in a minute, the others counted, and one line when the minute ends with how many were not written
-- and the longest of them. Checked without a window, through moho64ui_LongFrameLimitForTests (keymap.cpp), which
-- gives made-up frames to the same limit the window's loop uses. Also the log's threshold in this run (a run without
-- a window: 0, off, unless MOHO64UI_LONG_FRAMES sets one; `-- /threshold <ms>` says which is expected, 0 at first).
--   moho64ui --own ui/data --init <init.lua> --prefs <a copy> --stage frontend --size 1920x1080 --quiet --no-world
--            --lua ui/tests/long_frames_limit.lua --frames 300 --log out.log [-- /threshold 33]
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
local function count(lines, prefix)
    local n = 0
    for _, l in lines or {} do
        if string.sub(l, 1, string.len(prefix)) == prefix then n = n + 1 end
    end
    return n
end
local function held(lines)  -- the lines about those not written
    local out = {}
    for _, l in lines or {} do
        if string.find(l, "not written", 1, true) then table.insert(out, l) end
    end
    return out
end

ForkThread(function()
    WaitSeconds(0.5)
    check("the test function is there", type(moho64ui_LongFrameLimitForTests), "cfunction")

    -- 100 long frames, one each 0.1 s (150 ms to 249 ms), then a short frame at 60.5 s: the minute (from 0) has ended
    local frames = {}
    for i = 1, 100 do table.insert(frames, {(i - 1) * 0.1, 149 + i}) end
    table.insert(frames, {60.5, 0})
    local lines, threshold = moho64ui_LongFrameLimitForTests(frames)
    check("a flood of 100 in a minute: 30 written", count(lines, "written "), 30)
    check("the first 30 of them", lines and lines[1] == "written 1" and lines[30] == "written 30", true)
    local h = held(lines)
    check("one line when the minute ends", table.getn(h), 1)
    check("it is the last, after the minute", lines and lines[table.getn(lines)] == h[1], true)
    check("it says how many, the minute and the longest",
        h[1], "moho64ui: long frames: 70 more in the minute from 0.0 s not written (at most 30 lines a minute), the longest 249.0 ms (frame 100)")

    -- after the minute a new one begins: 30 more may be written
    frames = {}
    for i = 1, 40 do table.insert(frames, {(i - 1) * 0.5, 120}) end  -- (0 to 19.5 s: one minute, 10 over)
    for i = 1, 40 do table.insert(frames, {61 + (i - 1) * 0.5, 130 + i}) end  -- (61 to 80.5 s: the next, 10 over)
    lines = moho64ui_LongFrameLimitForTests(frames)
    check("two minutes, 40 long frames each: 60 written", count(lines, "written "), 60)
    h = held(lines)
    check("the first minute's line comes when the second minute's first frame comes", table.getn(h), 1)
    check("before that frame's own line", lines and lines[31] == h[1] and lines[32] == "written 41", true)
    check("ten not written in the first, the longest 120",
        h[1], "moho64ui: long frames: 10 more in the minute from 0.0 s not written (at most 30 lines a minute), the longest 120.0 ms (frame 31)")

    -- the window's end inside a minute: the ones held so far are said (Flush)
    lines = moho64ui_LongFrameLimitForTests(frames, true)
    h = held(lines)
    check("at the window's end the minute not ended is said too", table.getn(h), 2)
    check("its ten, the longest the last", h[2],
        "moho64ui: long frames: 10 more in the minute from 61.0 s not written (at most 30 lines a minute), the longest 170.0 ms (frame 80)")

    -- no line about those not written when none were held
    frames = {}
    for i = 1, 30 do table.insert(frames, {i, 200}) end  -- (exactly 30 in the minute)
    table.insert(frames, {90, 0})
    lines = moho64ui_LongFrameLimitForTests(frames, true)
    check("30 in a minute: all written", count(lines, "written "), 30)
    check("and nothing said about any not written", table.getn(held(lines)), 0)

    -- spread out: 2 a minute for 10 minutes, all written
    frames = {}
    for i = 1, 20 do table.insert(frames, {i * 30, 110}) end
    lines = moho64ui_LongFrameLimitForTests(frames, true)
    check("a few a minute: every one written", count(lines, "written "), 20)
    check("nothing held", table.getn(held(lines)), 0)

    -- the log's threshold in this run, without a window (unset or 0 / off: off; a number: that)
    local want = tonumber((GetCommandLineArg("/threshold", 1) or {})[1]) or 0
    check("the long-frame log's threshold in a run without a window", threshold, want)
    LOG("TEST done: " .. failed .. " failed")
end)
