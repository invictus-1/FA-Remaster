-- "Import from FAF" in the Key Bindings dialog (ui/data/hook/lua/ui/dialogs/keybindings.lua; Chris's third test,
-- 2026-11-05): the original's current profile's bindings read from its file (here the made-up one --import-prefs
-- names: profile 3 "Fixture Pilot Two"), the dialog's button, and after the import and the dialog's close the bindings
-- saved in this profile and the key working.
--   moho64ui --init <init.lua> --prefs <a copy> --stage setup --size 1920x1080 --import-prefs ui/tests/prefs_import.prefs
--            --lua ui/tests/keybind_import.lua --frames 200 --log out.log
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
ForkThread(function()
    WaitFrames(3)
    local Prefs = import('/lua/user/prefs.lua')
    local KeyMapper = import('/lua/keymap/keymapper.lua')
    local keys, why = moho64ui_OriginalKeyBindings()
    check("the original's current profile is read", keys and keys.Name, "Fixture Pilot Two")
    if not keys then
        LOG("TEST note: " .. tostring(why))
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    check("its key map", keys.UserKeyMap and keys.UserKeyMap["Ctrl-Q"], "fixture_action")
    check("its debug map", keys.UserDebugKeyMap and keys.UserDebugKeyMap["Ctrl-Alt-P"], "debug_navpath")
    check("its preset", keys.UserKeyMapName, "/lua/keymap/hotbuildKeyMap.lua")
    check("its own action", keys.UserKeyActions and keys.UserKeyActions.fixture_action and keys.UserKeyActions.fixture_action.category, "ui")
    check("and nothing but the bindings (no options)", keys.options, nil)

    local dialog = import('/lua/ui/dialogs/keybindings.lua')
    dialog.CreateUI()
    WaitFrames(3)
    local popup
    for i = 1, 200 do
        local n, v = debug.getupvalue(dialog.CloseUI, i)
        if not n then break end
        if n == 'popup' then popup = v end
    end
    check("the dialog has the Import from FAF button", popup and popup.remasterImportButton ~= nil, true)
    -- the button's Yes: the bindings into this profile, the dialog closed (which saves them and loads the keys)
    dialog.RemasterImportOriginalKeyBindings(keys)
    dialog.CloseUI()
    WaitFrames(3)
    local saved = Prefs.GetFromCurrentProfile("UserKeyMap") or {}
    check("saved in this profile", saved["Ctrl-Q"], "fixture_action")
    check("with its preset", Prefs.GetFromCurrentProfile("UserKeyMapName"), "/lua/keymap/hotbuildKeyMap.lua")
    local mapping = KeyMapper.GetKeyMappings()["Ctrl-Q"]
    check("and the key does the action", mapping and mapping.action, "UI_Lua LOG('fixture action')")
    LOG("TEST done: " .. failed .. " failed")
end)
