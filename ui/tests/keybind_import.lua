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
    -- (Chris's state of 2026-11-08: a note saying every own action was seen already, from an earlier save)
    local allOwn = {}
    for _, own in KeyMapper.RemasterOwnKeys do table.insert(allOwn, own.action) end
    Prefs.SetToCurrentProfile("UserKeyMapRemasterActions", allOwn)
    -- the button's Yes: the bindings into this profile, the dialog closed (which saves them and loads the keys)
    dialog.RemasterImportOriginalKeyBindings(keys)
    dialog.CloseUI()
    WaitFrames(3)
    local saved = Prefs.GetFromCurrentProfile("UserKeyMap") or {}
    check("saved in this profile", saved["Ctrl-Q"], "fixture_action")
    check("with its preset", Prefs.GetFromCurrentProfile("UserKeyMapName"), "/lua/keymap/hotbuildKeyMap.lua")
    local mapping = KeyMapper.GetKeyMappings()["Ctrl-Q"]
    check("and the key does the action", mapping and mapping.action, "UI_Lua LOG('fixture action')")
    -- this program's own keys came into the imported map where free (Chris's test 2026-11-08: they did not)
    check("the bookmark key is in the imported map", saved["Ctrl-Shift-B"], "remaster_bookmark_add")
    check("camera spot 1 saved by Ctrl-Alt-1", saved["Ctrl-Alt-1"], "SaveCameraPosition1")
    check("and recalled by Alt-9", saved["Alt-9"], "RestoreCameraPosition9")
    local spot = KeyMapper.GetKeyMappings()["Alt-1"]
    check("and Alt-1 is loaded as a key", spot ~= nil, true)

    -- Options > Interface > Key Bindings "Import from FAF..." (2026-11-06): a button of the Options dialog that opens
    -- the Key Bindings dialog and asks the import's question at once
    local entry
    for _, item in import('/lua/options/options.lua').options.ui.items do
        if item.key == 'remaster_import_faf_keys' then entry = item end
    end
    check("the Interface tab has Key Bindings: Import from FAF...", entry and (entry.type .. " " .. entry.custom.text), "button Import from FAF...")
    local UIUtil, asked = import('/lua/ui/uiutil.lua'), nil
    local realQuickDialog = UIUtil.QuickDialog
    UIUtil.QuickDialog = function(parent, text, a, b, c, d, e, f, g, h)
        asked = text
        return realQuickDialog(parent, text, a, b, c, d, e, f, g, h)
    end
    if entry then entry.update(nil, 0) end
    WaitFrames(3)
    UIUtil.QuickDialog = realQuickDialog
    popup = nil
    for i = 1, 200 do
        local n, v = debug.getupvalue(dialog.CloseUI, i)
        if not n then break end
        if n == 'popup' then popup = v end
    end
    check("which opens the Key Bindings dialog", popup and popup.remasterImportButton ~= nil, true)
    check("and asks the import's question", asked and string.find(asked, "Replace all key bindings with the", 1, true) ~= nil, true)
    if popup then dialog.CloseUI() end
    LOG("TEST done: " .. failed .. " failed")
end)
