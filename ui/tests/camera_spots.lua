-- The camera spots remembered per map (ui/data/hook/lua/usercamera.lua over FAF's /lua/usercamera.lua; their keys
-- ui/data/hook/lua/keymap/keymapper.lua). A game (session_ai.lua, seed 7) run twice on the same preferences, the case
-- after "--":
--   /spotscase first   the keys (Ctrl-Alt-1..9 save, Alt-1..9 go there; FAF's control group keys as they were) and
--                      their names; spots 1, 2 and 9 saved by their keys and gone to, in any order; FAF's "back to
--                      where the camera was"; a spot saved again replaces it; another map's spot (put in the profile
--                      first) is not this map's; each spot kept in the profile under this map's .scmap file; FAF's
--                      own list of camera positions (zoomslider's add / next) as it was. The spots are noted in the
--                      preferences for "again".
--   /spotscase again   a second game on the same map: before anything is saved, Alt-1, Alt-2 and Alt-9 go to the spots of
--                      the first game; Alt-3 (never saved) leaves the camera where it is; the other map's spot is still
--                      in the profile.
--   moho64ui --init <init.lua> --prefs <a copy> --stage setup --size 1920x1080 --session session_ai.lua --seed 7
--            --game-lua ui/tests/camera_spots.lua --frames 1500 --log out.log -- /spotscase first
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local function arg(name)
    local a = GetCommandLineArg(name, 1)
    return a and a[1]
end
local case = arg("/spotscase") or "first"
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
local function at(s, want) return s and want and near(s.Focus[1], want.x, 1) and near(s.Focus[3], want.z, 1) and near(s.Zoom, want.zoom, 1) end
local function plain(s) return { x = s.Focus[1], z = s.Focus[3], zoom = s.Zoom } end
local Prefs = import("/lua/user/prefs.lua")
local UserCamera = import("/lua/usercamera.lua")
local spotsKey = 'moho64ui_camera_spots'
local expectKey = 'moho64ui_test_camera_spots'
local otherMap = '/maps/somewhere_else/somewhere_else.scmap'

ForkThread(function()
    WaitSeconds(5)  -- (the game's opening: the camera's glide to the commander, StartupSequence)
    local cam = GetCamera('WorldCamera')
    local base = cam:SaveSettings()
    local function goTo(dx, dz, zoom)
        cam:RestoreSettings({ Focus = { base.Focus[1] + dx, base.Focus[2], base.Focus[3] + dz }, Zoom = zoom, Heading = base.Heading, Pitch = base.Pitch })
        WaitFrames(2)
        return cam:SaveSettings()
    end
    local function key(k, mods)
        moho64ui_Key(k, mods)
        WaitFrames(2)
        return cam:SaveSettings()
    end
    local map = UserCamera.RemasterSpotsMapKey()
    LOG("TEST note: the map: " .. tostring(map))
    check("the map's key is its .scmap file, lower case", map ~= nil and map == string.lower(SessionGetScenarioInfo().map or ""), true)

    if case == "first" then
        -- the keys, and FAF's own keys of 1..9 as they were
        local KeyMapper = import("/lua/keymap/keymapper.lua")
        local keysOk = true
        for i = 1, 9 do
            if string.find(moho64ui_KeyMapAction("Ctrl-Alt-" .. i) or "", "SaveCameraPosition(" .. i .. ")", 1, true) == nil then keysOk = false end
            if string.find(moho64ui_KeyMapAction("Alt-" .. i) or "", "RestoreCameraPosition(" .. i .. ")", 1, true) == nil then keysOk = false end
        end
        check("Ctrl-Alt-1..9 save camera spots 1..9, Alt-1..9 go there", keysOk, true)
        local map0 = KeyMapper.GetCurrentKeyMap()
        check("1, Ctrl-1, Shift-1, Ctrl-Shift-1 stay FAF's control groups",
            tostring(map0['1']) .. " " .. tostring(map0['Ctrl-1']) .. " " .. tostring(map0['Shift-1']) .. " " .. tostring(map0['Ctrl-Shift-1']),
            "group1 set_group1 append_group1 fac_group1")
        check("their names in the key map", KeyMapper.GetActionName('SaveCameraPosition1') .. " / " .. KeyMapper.GetActionName('RestoreCameraPosition1'),
            "Save camera spot 1 (remembered for this map) / Go to camera spot 1")
        -- the profile: nothing of this map; another map's spot 5
        local start = {}
        start[otherMap] = { ["5"] = { Focus = { 10, 20, 30 }, Heading = 0, Pitch = 1, Zoom = 50 } }
        Prefs.SetToCurrentProfile(spotsKey, start)

        local a = goTo(30, -20, 70)
        key("1", "Ctrl Alt")
        local b = goTo(-40, 10, 110)
        key("2", "Ctrl Alt")
        local c9 = goTo(15, 45, 55)
        key("9", "Ctrl Alt")
        goTo(-10, -50, 90)
        check("Alt-1 goes to spot 1", at(key("1", "Alt"), plain(a)), true)
        check("Alt-9 to spot 9", at(key("9", "Alt"), plain(c9)), true)
        check("Alt-2 to spot 2", at(key("2", "Alt"), plain(b)), true)
        UserCamera.RestorePreviousCameraPosition()
        WaitFrames(2)
        check("FAF's back to where the camera was: before Alt-2", at(cam:SaveSettings(), plain(c9)), true)
        local here = goTo(5, 5, 80)
        check("Alt-5: another map's spot is not this map's (the camera stays)", at(key("5", "Alt"), plain(here)), true)
        local saved = (Prefs.GetFromCurrentProfile(spotsKey) or {})[map] or {}
        check("spots 1, 2 and 9 kept in the profile under this map", saved["1"] ~= nil and saved["2"] ~= nil and saved["9"] ~= nil and saved["5"] == nil, true)
        check("as plain values", saved["1"] and type(saved["1"].Focus) == 'table' and getmetatable(saved["1"].Focus) == nil and
            near(saved["1"].Focus[1], a.Focus[1], 0.01) and near(saved["1"].Zoom, a.Zoom, 0.01), true)
        check("the other map's spot left as it was", ((Prefs.GetFromCurrentProfile(spotsKey) or {})[otherMap] or {})["5"] ~= nil, true)
        -- saved again: it replaces the spot
        local a2 = goTo(-25, -25, 65)
        key("1", "Ctrl Alt")
        goTo(0, 0, 100)
        check("spot 1 saved again: Alt-1 goes to the new place", at(key("1", "Alt"), plain(a2)), true)
        check("and the profile has the new one", near(((Prefs.GetFromCurrentProfile(spotsKey) or {})[map] or {})["1"].Focus[1], a2.Focus[1], 0.01), true)
        -- FAF's own list of camera positions (add_cam_position, next_cam_position) as it was
        local Zoom = import("/lua/ui/game/zoomslider.lua")
        local e = goTo(35, 35, 75)
        Zoom.SaveCameraPos()
        goTo(-35, -35, 95)
        Zoom.RecallCameraPos()
        WaitFrames(2)
        check("FAF's camera position list: add, then next goes there", at(cam:SaveSettings(), plain(e)), true)
        SetPreference(expectKey, { a = plain(a2), b = plain(b), c = plain(c9) })
        SavePreferences()
    else
        local want = GetPreference(expectKey)
        check("\"first\" noted its spots", want ~= nil, true)
        if want then
            goTo(-10, -50, 90)
            check("a second game on the map: Alt-1 goes to spot 1 of the first game", at(key("1", "Alt"), want.a), true)
            check("Alt-2 to its spot 2", at(key("2", "Alt"), want.b), true)
            check("Alt-9 to its spot 9", at(key("9", "Alt"), want.c), true)
            local here = goTo(20, -20, 85)
            check("Alt-3 (never saved) leaves the camera where it is", at(key("3", "Alt"), plain(here)), true)
            check("the other map's spot still in the profile", ((Prefs.GetFromCurrentProfile(spotsKey) or {})[otherMap] or {})["5"] ~= nil, true)
        end
    end
    LOG("TEST done: " .. failed .. " failed")
    ExitApplication()
end)
