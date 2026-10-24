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
}
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

local RemasterGetDefaultKeyMap = GetDefaultKeyMap
function GetDefaultKeyMap()
    return RemasterAddOwn(RemasterGetDefaultKeyMap())
end

local RemasterGetUserKeyMap = GetUserKeyMap
function GetUserKeyMap()
    local map = RemasterGetUserKeyMap()
    if not map then return nil end
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
