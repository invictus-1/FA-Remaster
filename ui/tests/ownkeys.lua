-- The program's own key actions in every key map (ui/data/hook/lua/keymap/keymapper.lua): FAF's three presets and the
-- player's saved key map get each own action with its key where that key is free; a taken key is left alone and the
-- action stays unbound there; a map the key binding screen saved notes the own actions it has seen, so one the player
-- unbound stays unbound. In the front end, on the profile of a copy of menu.prefs (no saved key map):
--   moho64ui --init <init.lua> --prefs <a copy> --stage setup --lua ui/tests/ownkeys.lua --frames 8 --log out.log
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

local KeyMapper = import("/lua/keymap/keymapper.lua")
local Prefs = import("/lua/user/prefs.lua")
local own = {
    { key = 'Ctrl-Alt-S', action = 'remaster_stats' },
    { key = 'Ctrl-Alt-C', action = 'remaster_cinecam' },
    { key = 'Ctrl-Alt-F12', action = 'remaster_photo' },
    { key = 'Alt-P', action = 'remaster_photo_mode' },
    { key = 'Ctrl-Shift-P', action = 'remaster_palette' },
    { key = 'Ctrl-T', action = 'remaster_timeline' },
}
local function copy(t)
    local out = {}
    for k, v in t or {} do out[k] = v end
    return out
end
-- the host's key map loaded again from the script's, as the key binding screen's OK does
local function reload()
    IN_ClearKeyMap()
    IN_AddKeyMapTable(KeyMapper.GetKeyMappings())
end

check("nothing saved at first", Prefs.GetFromCurrentProfile("UserKeyMap"), nil)

-- the presets: each own action at its key where the preset leaves it free; a key the preset (or its debug map) has,
-- or the Shift / Alt version of one of its hotbuild keys, stays the preset's
local hotbuildCategories = { hotbuilding = true, hotbuildingAlternative = true, hotbuildingExtra = true }
local actions = KeyMapper.GetKeyActions()
for _, preset in { "/lua/keymap/defaultKeyMap.lua", "/lua/keymap/hotbuildKeyMap.lua", "/lua/keymap/alternativeKeyMap.lua" } do
    Prefs.SetToCurrentProfile("UserKeyMapName", preset)
    local file = import(preset)
    local raw = copy(file.defaultKeyMap)
    for k, v in file.debugKeyMap or {} do raw[k] = v end
    local map = KeyMapper.GetCurrentKeyMap()
    local got, want = {}, {}
    for _, o in own do
        local taken = KeyMapper.IsKeyInMap(o.key, raw)
        local norm = KeyMapper.NormalizeKey(o.key)
        if not taken and not norm.Ctrl and (norm.Shift or norm.Alt) and not (norm.Shift and norm.Alt) then
            local base = KeyMapper.IsKeyInMap(norm.key, raw)
            taken = base and actions[base] and hotbuildCategories[actions[base].category or ''] or false
        end
        table.insert(want, o.action .. "=" .. (taken and "unbound" or o.key))
        table.insert(got, o.action .. "=" .. (KeyMapper.IsActionInMap(o.action, map) or "unbound"))
    end
    LOG("TEST note " .. preset .. ": " .. table.concat(got, " "))
    check(preset .. ": each own action at its key where free", table.concat(got, " "), table.concat(want, " "))
    local keptOwn = true
    for k, v in raw do
        if map[k] ~= v then keptOwn = false end
    end
    check(preset .. ": every key of the preset as it was", keptOwn, true)
end
Prefs.SetToCurrentProfile("UserKeyMapName", nil)
local default = KeyMapper.GetCurrentKeyMap()
check("the default preset: Ctrl-Shift-P the palette, Ctrl-K still Self-Destruct",
    tostring(default['Ctrl-Shift-P']) .. " " .. tostring(default['Ctrl-K']), "remaster_palette suicide")
check("an own action's key, as the game's key screens ask it", KeyMapper.GetCurrentKeyBinding('remaster_palette'), 'Ctrl-Shift-P')

-- a saved map without the own actions (as one saved before they existed): they come into it where free
local saved = copy(import("/lua/keymap/defaultKeyMap.lua").defaultKeyMap)
saved['Ctrl-Alt-C'] = 'patrol'  -- (the cinematic camera's key taken by the player)
Prefs.SetToCurrentProfile("UserKeyMap", saved)
local map = KeyMapper.GetCurrentKeyMap()
check("a saved map without the key gets the action", map['Ctrl-Shift-P'], 'remaster_palette')
check("and the others whose key is free", tostring(map['Ctrl-Alt-S']) .. " " .. tostring(map['Alt-P']), "remaster_stats remaster_photo_mode")
check("a saved map with the key taken keeps its own", map['Ctrl-Alt-C'], 'patrol')
check("and that action stays unbound there", KeyMapper.IsActionInMap('remaster_cinecam', map), false)
check("(as the game's key screens ask it)", KeyMapper.GetCurrentKeyBinding('remaster_cinecam'), false)
check("nothing written to the preferences by reading", Prefs.GetFromCurrentProfile("UserKeyMap")['Ctrl-Shift-P'], nil)
reload()
check("the host's key map follows: Ctrl-Shift-P", moho64ui_KeyMapAction("Ctrl-Shift-P"), "remaster_palette")
check("the host's key map follows: the taken key", string.find(moho64ui_KeyMapAction("Ctrl-Alt-C") or "", "RULEUCC_Patrol", 1, true) ~= nil, true)

-- an action the player bound elsewhere stays there
local elsewhere = false  -- (a key free in that map)
for _, k in { 'Ctrl-Shift-J', 'Ctrl-Shift-U', 'Ctrl-Alt-J', 'Ctrl-Alt-U' } do
    if not elsewhere and not KeyMapper.IsKeyInMap(k, map) then elsewhere = k end
end
saved[elsewhere] = 'remaster_palette'
Prefs.SetToCurrentProfile("UserKeyMap", saved)
map = KeyMapper.GetCurrentKeyMap()
check("an own action the player bound to another key keeps it", tostring(map[elsewhere]) .. " " .. tostring(map['Ctrl-Shift-P']), "remaster_palette nil")
saved[elsewhere] = nil

-- the key binding screen unbinds one and saves: noted, and it stays unbound
Prefs.SetToCurrentProfile("UserKeyMap", copy(saved))
KeyMapper.ClearUserKeyMapping('Ctrl-Shift-P')  -- (the screen's Unbind: it writes the saved map itself)
check("the screen's unbind writes the map without it", Prefs.GetFromCurrentProfile("UserKeyMap")['Ctrl-Shift-P'], nil)
check("and the others the screen saw with it", Prefs.GetFromCurrentProfile("UserKeyMap")['Alt-P'], 'remaster_photo_mode')
local seen = Prefs.GetFromCurrentProfile("UserKeyMapRemasterActions") or {}
check("the own actions that map has seen are noted", table.getn(seen), table.getn(own))
KeyMapper.SaveUserKeyMap()
check("an action the player unbound stays unbound", KeyMapper.IsActionInMap('remaster_palette', KeyMapper.GetCurrentKeyMap()), false)
-- an action newer than the noted ones still comes in
local fewer = {}
for _, a in seen do if a ~= 'remaster_palette' then table.insert(fewer, a) end end
Prefs.SetToCurrentProfile("UserKeyMapRemasterActions", fewer)
check("an action newer than the saved map comes in by itself", KeyMapper.GetCurrentKeyMap()['Ctrl-Shift-P'], 'remaster_palette')

-- back to a preset: the note goes with the saved map
KeyMapper.ResetUserKeyMapTo('/lua/keymap/defaultKeyMap.lua')
check("a preset chosen again: no saved map, no note", tostring(Prefs.GetFromCurrentProfile("UserKeyMap")) .. " " ..
    tostring(Prefs.GetFromCurrentProfile("UserKeyMapRemasterActions")), "nil nil")
check("and the preset's own keys again", KeyMapper.GetCurrentKeyMap()['Ctrl-Shift-P'], 'remaster_palette')
Prefs.SetToCurrentProfile("UserKeyMapName", nil)
reload()
LOG("TEST done: " .. failed .. " failed")
