-- FA Remastered: "Import from FAF" in the Key Bindings dialog (this file is a hook: the program reads it after the
-- game's /lua/ui/dialogs/keybindings.lua, which stays as it is). Chris, 2026-11-05 (third test): import my FAF
-- hotkeys. The button takes the key bindings of the current profile in FAF's own preferences file (only read:
-- moho64ui_OriginalKeyBindings, ui/user/prefs.cpp) into the player's profile here, after asking, as the preset
-- buttons replace them: the bindings he changed (UserKeyMap, UserDebugKeyMap), the preset they start from
-- (UserKeyMapName) and his own actions (UserKeyActions). The dialog is then made again with them; closing it saves
-- them and loads the keys, as the game's own preset buttons do (ConfirmNewKeyMap on close).
-- The dialog keeps itself in a local of its module, which a hook does not see: it is read through CreateUI, which
-- uses it. If a later dialog changes that, the button is left out.
do
    local RemasterGameCreateUI = CreateUI
    local function ModuleLocal(name)
        for i = 1, 200 do
            local n, v = debug.getupvalue(RemasterGameCreateUI, i)
            if not n then return nil end
            if n == name then return v end
        end
    end
    local kFields = { 'UserKeyMap', 'UserDebugKeyMap', 'UserKeyMapName', 'UserKeyActions' }

    -- the import itself (also for the tests): the original's current profile's bindings into this profile's
    function RemasterImportOriginalKeyBindings(keys)
        local Prefs = import('/lua/user/prefs.lua')
        for _, field in kFields do
            Prefs.SetToCurrentProfile(field, keys[field])
        end
        -- FAF's map has none of this program's own actions (the bookmarks, the camera spots, ...): they come into it
        -- where their key is free, and the note of the own actions the saved map has seen starts again. (Chris's test
        -- of 2026-11-08: after the import Ctrl-Shift-B and Alt-1..9 did nothing: the dialog's save had noted every own
        -- action as seen, so none was added to the imported map; keymapper.lua's GetUserKeyMap.)
        if keys.UserKeyMap then
            local map = {}
            for key, action in keys.UserKeyMap do map[key] = action end
            import('/lua/keymap/keymapper.lua').RemasterAddOwnKeys(map)
            Prefs.SetToCurrentProfile('UserKeyMap', map)
        end
        Prefs.SetToCurrentProfile('UserKeyMapRemasterActions', nil)
    end

    function CreateUI()
        RemasterGameCreateUI()
        local popup = ModuleLocal('popup')
        local content = popup and popup.content
        if not content or IsDestroyed(content) then return end
        local UIUtil = import('/lua/ui/uiutil.lua')
        local LayoutHelpers = import('/lua/maui/layouthelpers.lua')
        local Tooltip = import('/lua/ui/game/tooltip.lua')
        local button = UIUtil.CreateButtonWithDropshadow(content, "/BUTTON/medium/", "Import from FAF")
        LayoutHelpers.SetWidth(button, 200)
        LayoutHelpers.AtTopIn(button, content, 4)
        LayoutHelpers.AtRightIn(button, content, 10)
        Tooltip.AddControlTooltip(button, {
            text = "Import from FAF",
            body = "Replace all key bindings with those of your current profile in FAF (Forged Alliance Forever). FAF's own file is only read.",
        })
        popup.remasterImportButton = button
        button.OnClick = function(self, modifiers)
            RemasterAskImport(popup)
        end
    end

    -- Options > Interface > Key Bindings "Import from FAF..." (Chris, 2026-11-06: FAF opens this dialog only in game, F1
    -- or the game's menu; he looked for it in Options): the dialog opened (closed first if it was up) and the import
    -- asked at once, the same question as its own button
    function RemasterOpenKeyBindingsAndImport()
        local open = ModuleLocal('popup')
        if open then CloseUI() end
        CreateUI()
        local popup = ModuleLocal('popup')
        if popup and popup.remasterImportButton then RemasterAskImport(popup) end
        return popup and popup.remasterImportButton ~= nil
    end

    function RemasterAskImport(popup)
        local UIUtil = import('/lua/ui/uiutil.lua')
        do
            local keys, why = moho64ui_OriginalKeyBindings()
            if not keys or not keys.UserKeyMap then
                UIUtil.QuickDialog(popup, "There are no FAF key bindings to import" .. (why and (": " .. why) or "."),
                    "<LOC _Ok>", nil, nil, nil, nil, nil, true, { escapeButton = 1, enterButton = 1, worldCover = false })
                return
            end
            UIUtil.QuickDialog(popup,
                string.format("Replace all key bindings with the %d of your FAF profile \"%s\"?",
                    table.getsize(keys.UserKeyMap), tostring(keys.Name or "?")),
                "<LOC _Yes>", function()
                    RemasterImportOriginalKeyBindings(keys)
                    CloseUI()  -- (its close saves the bindings and loads the keys, as after a preset)
                    CreateUI()
                end,
                "<LOC _No>", nil, nil, nil, true, { escapeButton = 2, enterButton = 1, worldCover = false })
        end
    end
end
