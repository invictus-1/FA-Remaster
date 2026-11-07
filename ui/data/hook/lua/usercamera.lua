-- FA Remastered: camera spots remembered per map (Chris, 2026-11-07: "up to 9 saved camera spots per map, jump back to
-- them, remembered between games on the same map"). FAF's own camera spots stay what they are: SaveCameraPosition(id)
-- and RestoreCameraPosition(id) above (its key actions SaveCameraPosition0..9 / RestoreCameraPosition0..9, which its
-- presets leave without keys; ours give spots 1..9 Ctrl-Alt-1..9 and Alt-1..9 where free:
-- ui/data/hook/lua/keymap/keymapper.lua), RestorePreviousCameraPosition back to where the camera was. FAF keeps them
-- for one game (StoredCameraSettings). This hook (the program reads it after the game's /lua/usercamera.lua, which
-- stays as it is) keeps each spot saved also in the player's profile, by map: moho64ui_camera_spots =
-- { [<the map's file (.scmap), lower case>] = { ["<id>"] = { Focus = { x, y, z }, Heading, Pitch, Zoom } } }; the
-- spots of the map played come back into StoredCameraSettings the first time one is saved or gone to there, in a game
-- or a replay of that map (a replay's jump back makes the interface anew: they come back again). FAF's list of camera
-- positions (add_cam_position, next_cam_position, rem_cam_position: /lua/ui/game/zoomslider.lua) is not touched.

local RemasterSpotsKey = 'moho64ui_camera_spots'
local remasterSpotsMap = false  -- (the map whose spots StoredCameraSettings holds)

-- The map played (its .scmap file, lower case: a game from the lobby, one from a session file and a replay all name
-- it; not every one names its scenario file); nil outside a game or a replay.
function RemasterSpotsMapKey()
    if not SessionIsActive or not SessionIsActive() then return nil end
    local info = SessionGetScenarioInfo and SessionGetScenarioInfo()
    local file = info and (info.map or info.file)
    return type(file) == 'string' and string.lower(file) or nil
end

-- The profile's spots of a map (a table of plain values), or of every map.
function RemasterSavedSpots(map)
    local all = import("/lua/user/prefs.lua").GetFromCurrentProfile(RemasterSpotsKey) or {}
    if map then return all[map] or {} end
    return all
end

-- A camera's settings as plain values, as the preferences keep them (SaveSettings' Focus is a vector).
local function RemasterPlain(settings)
    local out = {}
    for k, v in settings or {} do
        if type(v) == 'number' or type(v) == 'string' or type(v) == 'boolean' then
            out[k] = v
        elseif type(v) == 'table' then
            out[k] = { v[1], v[2], v[3] }
        end
    end
    return out
end

-- The map's spots into StoredCameraSettings, once a map (a spot saved there in this game stays as it is).
local function RemasterLoadSpots()
    local map = RemasterSpotsMapKey()
    if not map or map == remasterSpotsMap then return map end
    if remasterSpotsMap then StoredCameraSettings = {} end  -- (another map's, in this interface before: not this one's)
    remasterSpotsMap = map
    for id, settings in RemasterSavedSpots(map) do
        local slot = tonumber(id)
        if slot and StoredCameraSettings[slot] == nil and type(settings) == 'table' and type(settings.Focus) == 'table' then
            StoredCameraSettings[slot] = RemasterPlain(settings)
        end
    end
    return map
end

local RemasterSaveCameraPosition = SaveCameraPosition
function SaveCameraPosition(id)
    local map = RemasterLoadSpots()
    RemasterSaveCameraPosition(id)
    local settings = StoredCameraSettings[id]
    if not map or not settings then return end
    local Prefs = import("/lua/user/prefs.lua")
    local all = Prefs.GetFromCurrentProfile(RemasterSpotsKey) or {}
    all[map] = all[map] or {}
    all[map][tostring(id)] = RemasterPlain(settings)
    Prefs.SetToCurrentProfile(RemasterSpotsKey, all)
    SavePreferences()
end

local RemasterRestoreCameraPosition = RestoreCameraPosition
function RestoreCameraPosition(id)
    RemasterLoadSpots()
    RemasterRestoreCameraPosition(id)
end
