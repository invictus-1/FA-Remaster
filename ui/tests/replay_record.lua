-- A game is recorded as a replay, the original's way: LastGame.SCFAReplay in the player's replay
-- folder, which the game's Replay dialog lists; "Save Replay" keeps it under a name.
-- Run it as the game test (--session with session_ai.lua, --seed 7, --game-lua this file),
-- with --frames 1800; then replay_play.lua plays the kept replay back.
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
ForkThread(function()
    WaitSeconds(8)
    -- the commander walks somewhere (a right click on the ground), so the replay has an order
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local cdr = GetArmyAvatars()[1]
    local cam = GetCamera("WorldCamera")
    local s = cam:SaveSettings()
    s.Focus = cdr:GetPosition()
    s.Zoom = 120
    cam:RestoreSettings(s)
    WaitSeconds(1)
    SelectUnits({cdr})
    local c = cdr:GetPosition()
    local at = view:Project({c[1] + 30, c[2], c[3] - 20})
    moho64ui_MouseButton("Right", true, at.x, at.y)
    moho64ui_MouseButton("Right", false, at.x, at.y)
    WaitSeconds(10)
    local p = cdr:GetPosition()
    check("the commander walks", math.abs(p[1] - c[1]) + math.abs(p[3] - c[3]) > 10, true)

    local profile = import("/lua/user/prefs.lua").GetCurrentProfile().Name
    local listed = GetSpecialFiles("Replay")
    check("the replay folder is named", type(listed.directory), "string")
    check("with the original's extension", listed.extension, "SCFAReplay")
    local mine = false
    for _, name in (listed.files[profile] or {}) do
        if name == "LastGame" then mine = true end
    end
    check("this game's replay is listed as LastGame", mine, true)
    local info = GetSpecialFileInfo(profile, "LastGame", "Replay")
    check("and has a size", info and info.SizeBytes > 100, true)
    check("and a write time", info and info.WriteTime and info.WriteTime.year >= 2026, true)
    check("and a time stamp that sorts as text", info and string.len(info.TimeStamp), 16)
    -- kept under a name, as the Save Replay dialog does
    CopyCurrentReplay(profile, "moho64_test")
    local kept = GetSpecialFileInfo(profile, "moho64_test", "Replay")
    check("Save Replay keeps it under a name", kept and kept.SizeBytes > 100, true)
    local path = GetSpecialFilePath(profile, "moho64_test", "Replay")
    check("whose path is the folder, the profile and the name", string.sub(path, -string.len(profile .. "/moho64_test.SCFAReplay")), profile .. "/moho64_test.SCFAReplay")
    check("a file that is not there has no info", GetSpecialFileInfo(profile, "no_such_replay", "Replay"), false)
    LOG("TEST done: " .. failed .. " failed")
end)
