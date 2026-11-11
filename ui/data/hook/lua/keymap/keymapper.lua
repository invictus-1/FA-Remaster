-- FA Remastered: the program's own key actions (ui/data/hook/lua/keymap/keyactions.lua) in every key map the game
-- loads (this file is a hook: the program reads it after the game's /lua/keymap/keymapper.lua, which stays as it is).
-- Chris, 2026-10-23: each comes with its key into the player's saved key map and into each of FAF's presets (default,
-- hotbuild, alternative) wherever that key is free there; a key already taken is left alone and the action stays
-- unbound in that map (the key binding screen binds it). Done where the map is read (GetDefaultKeyMap, GetUserKeyMap,
-- so GetCurrentKeyMap and GetKeyMappings), nothing written to the preferences by this alone: a saved map with them
-- added is written only when the key binding screen writes it anyway (its SetUserKeyMapping, ClearUserKeyMapping,
-- SaveUserKeyMap); then the own actions that map has seen are noted beside it (UserKeyMapRemasterActions), so one the
-- player unbound there stays unbound, and only an action newer than the saved map comes into it by itself.
local RemasterPrefs = import("/lua/user/prefs.lua")

-- The own actions and their keys, in order (the first gets its key first).
RemasterOwnKeys = {
    { key = 'Ctrl-Alt-S', action = 'remaster_stats' },
    { key = 'Ctrl-Alt-C', action = 'remaster_cinecam' },
    { key = 'Ctrl-Alt-F12', action = 'remaster_photo' },
    { key = 'Alt-P', action = 'remaster_photo_mode' },
    { key = 'Ctrl-Shift-P', action = 'remaster_palette' },  -- (the command palette; Chris: Ctrl-K stays Self-Destruct)
    { key = 'Ctrl-T', action = 'remaster_timeline' },  -- (the replay timeline, shown or hidden; only while a replay plays)
    { key = 'Ctrl-Alt-V', action = 'remaster_follow_cam' },  -- (the follow camera; the hotbuild and alternative maps' debug key there)
    { key = 'Y', action = 'remaster_radial_ping' },  -- (the radial ping menu; Chris may pick another)
    -- (the replay bookmarks, Chris 2026-11-07: B for bookmark; Ctrl-B is FAF's "select engineers"; < and > the one
    -- before and the next; all three free in FAF's three presets)
    { key = 'Ctrl-Shift-B', action = 'remaster_bookmark_add' },
    { key = 'Ctrl-Shift-Period', action = 'remaster_bookmark_next' },
    { key = 'Ctrl-Shift-Comma', action = 'remaster_bookmark_previous' },
}
-- FAF's own camera spots 1..9 (/lua/usercamera.lua SaveCameraPosition / RestoreCameraPosition, which FAF's presets leave
-- without keys), remembered per map by this program (ui/data/hook/lua/usercamera.lua; Chris, 2026-11-07): Ctrl-Alt-1..9
-- saves, Alt-1..9 goes there. 1..9 alone are FAF's control groups, Ctrl-1..9 set them, Shift-1..9 add to them,
-- Ctrl-Shift-1..9 its factory groups; Alt and Ctrl-Alt with a digit are free in its three presets.
for remasterSpot = 1, 9 do
    table.insert(RemasterOwnKeys, { key = 'Ctrl-Alt-' .. remasterSpot, action = 'SaveCameraPosition' .. remasterSpot })
end
for remasterSpot = 1, 9 do
    table.insert(RemasterOwnKeys, { key = 'Alt-' .. remasterSpot, action = 'RestoreCameraPosition' .. remasterSpot })
end
-- The re-test panel (Chris's pick of 2026-11-09; /moho64ui/lua/retest.lua), only in a program started with --retest:
-- Ctrl-Shift-R (R for re-test). Free in FAF's three presets and its debug keys, 3839 and 3840 (Ctrl-R is its
-- "toggle economy", Shift-R and Alt-R its repair / spread attack / reclaim labels, Ctrl-Alt-R the hotbuild preset's
-- reclaim labels). Without --retest the action does not exist, and a saved key map that holds it from a run with
-- --retest is read without it (GetUserKeyMap below), so the game's key mapper never meets an action it does not know.
local remasterRetest = rawget(_G, 'moho64ui_RetestList') and moho64ui_RetestList() and true or false
if remasterRetest then
    table.insert(RemasterOwnKeys, { key = 'Ctrl-Shift-R', action = 'remaster_retest' })
end
local remasterSeenKey = 'UserKeyMapRemasterActions'
local remasterHotbuild = { hotbuilding = true, hotbuildingAlternative = true, hotbuildingExtra = true }

-- Whether a key is taken in a map: bound there (its modifiers in any order), or the Shift or Alt version of a key a
-- hotbuild action is bound to, which the hotbuild modifiers take in a game (GenerateHotbuildModifiers).
local function RemasterKeyTaken(key, map)
    if IsKeyInMap(key, map) then return true end
    local norm = NormalizeKey(key)
    if norm.Ctrl or (norm.Shift and norm.Alt) or not (norm.Shift or norm.Alt) then return false end
    local action = IsKeyInMap(norm.key, map)
    local data = action and GetKeyActions()[action]
    return data ~= nil and data ~= false and remasterHotbuild[data.category or ''] == true
end

-- The own actions added to a map where their key is free and they are not bound already; `seen`: those to leave.
local function RemasterAddOwn(map, seen)
    if not map then return map end
    for _, own in RemasterOwnKeys do
        if not (seen and seen[own.action]) and not IsActionInMap(own.action, map) and not RemasterKeyTaken(own.key, map) then
            map[own.key] = own.action
        end
    end
    return map
end

-- (for the FAF import, ui/data/hook/lua/ui/dialogs/keybindings.lua: a whole map from elsewhere, which never had the own
-- actions, gets them where their key is free, as a preset does)
function RemasterAddOwnKeys(map)
    return RemasterAddOwn(map)
end

local RemasterGetDefaultKeyMap = GetDefaultKeyMap
function GetDefaultKeyMap()
    return RemasterAddOwn(RemasterGetDefaultKeyMap())
end

local RemasterGetUserKeyMap = GetUserKeyMap
function GetUserKeyMap()
    local map = RemasterGetUserKeyMap()
    if not map then return nil end
    if not remasterRetest then  -- (the re-test panel's key from a run with --retest: see RemasterOwnKeys above)
        local left = {}
        for key, action in map do
            if action ~= 'remaster_retest' then left[key] = action end
        end
        map = left
    end
    local seen = {}
    for _, action in RemasterPrefs.GetFromCurrentProfile(remasterSeenKey) or {} do seen[action] = true end
    return RemasterAddOwn(map, seen)
end

-- An own action's key: where the map read now has it (the game's own looks in the saved map, then in the preset's
-- file, where the own actions are not).
local RemasterGetCurrentKeyBinding = GetCurrentKeyBinding
function GetCurrentKeyBinding(action)
    for _, own in RemasterOwnKeys do
        if own.action == action then return IsActionInMap(action, GetCurrentKeyMap()) end
    end
    return RemasterGetCurrentKeyBinding(action)
end

-- The key binding screen wrote the saved map (or took it away): note which own actions it has seen.
local function RemasterNoteSeen()
    if RemasterPrefs.GetFromCurrentProfile("UserKeyMap") then
        local seen = {}
        for _, own in RemasterOwnKeys do table.insert(seen, own.action) end
        RemasterPrefs.SetToCurrentProfile(remasterSeenKey, seen)
    else
        RemasterPrefs.SetToCurrentProfile(remasterSeenKey, nil)
    end
end
local RemasterSetUserKeyMapping = SetUserKeyMapping
function SetUserKeyMapping(key, oldKey, action)
    RemasterSetUserKeyMapping(key, oldKey, action)
    RemasterNoteSeen()
end
local RemasterClearUserKeyMapping = ClearUserKeyMapping
function ClearUserKeyMapping(key)
    RemasterClearUserKeyMapping(key)
    RemasterNoteSeen()
end
local RemasterSaveUserKeyMap = SaveUserKeyMap
function SaveUserKeyMap()
    RemasterSaveUserKeyMap()
    RemasterNoteSeen()
end
local RemasterClearUserKeyMap = ClearUserKeyMap
function ClearUserKeyMap()
    RemasterClearUserKeyMap()
    RemasterNoteSeen()
end
local RemasterResetUserKeyMapTo = ResetUserKeyMapTo
function ResetUserKeyMapTo(newPreset)
    RemasterResetUserKeyMapTo(newPreset)
    RemasterNoteSeen()
end
