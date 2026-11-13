-- FA Remastered: the Options dialog's tab buttons narrowed so that five fit (this file is a hook: the program reads it
-- after the game's /lua/ui/dialogs/options.lua, which stays as it is). Our Remastered tab (ui/data/hook/lua/options/
-- options.lua; Chris, 2026-11-08) makes a fifth; the game's dialog lays its tabs for four: each 178 wide (the button's
-- texture, /scx_menu/tab_btn/tab_btn_up.dds), the first at 10 in the panel, each next one overlapping the one before
-- by 10, so four end at 692 in the panel's 698 (/scx_menu/options/panel_bmp.dds; both widths as the game's textures
-- have them, measured by ui/tests/options_tabs.lua), and a fifth would end 162 outside. Five get 145 each.
-- Here each button, and the lit picture of the chosen one (tab_btn_selected.dds, which the dialog centres on the hidden
-- button: its SetNewPage), gets the width that fits them all, the textures drawn narrower; the places stay the dialog's
-- own (RightOf the one before). With four tabs nothing changes. The dialog and the lit picture are locals of the module,
-- which a hook does not see: they are read through the functions that use them. If a later dialog changes those, its
-- layout is left as it is.
do
    local RemasterGameCreateDialog = CreateDialog
    local RemasterGoneSaid = {}
    local function Upvalue(fn, name)
        if type(fn) ~= 'function' then return nil end
        for i = 1, 200 do
            local n, v = debug.getupvalue(fn, i)
            if not n then
                -- (the game's script has no such local any more, a FAF update: said once, so that the feature that
                -- is left out does not go in silence; the lab's hook_contract.py says the same before a push)
                if not RemasterGoneSaid[name] then
                    RemasterGoneSaid[name] = true
                    WARN("FA Remastered: /lua/ui/dialogs/options.lua has no local '" .. tostring(name) .. "' where our hook reads it: a feature of ours is left out")
                end
                return nil
            end
            if n == name then return v end
        end
    end
    -- (for the tests: the lookup itself, through the dialog's CreateDialog; ui/tests/options_tabs.lua)
    function RemasterHookLocal(name) return Upvalue(RemasterGameCreateDialog, name) end

    -- A 'button' option's row cleans up after itself. The game's dialog makes such a row as a Bitmap and then gives it
    -- an OnDestroy of its own (controlTypeCreate.button: `bg.OnDestroy = function ... end`), which hides Bitmap's
    -- OnDestroy: the row's texture stays tied to the skin after the row is gone, and the next change of skin calls
    -- SetNewTexture on a destroyed control ("Game object has been destroyed"). The game's own options have no button
    -- row, so it never shows there; ours have two (Interface: Import from FAF..., Remastered: Go to Section's Show),
    -- and the score screen sets the skin first thing (score.lua CreateSkirmishScreen): after one look at those tabs in
    -- a game, End Game brought no score screen (Chris's playtest, 2026-11-09: "End game doesn't work anymore").
    -- Here the row's own OnDestroy runs, then Bitmap's. (The factory table is a local of the module, read through the
    -- functions that use it; if a later dialog changes that, a warning says so.)
    do
        local types = Upvalue(Upvalue(RemasterGameCreateDialog, 'CreateOption'), 'controlTypeCreate')
        if type(types) == 'table' and type(types.button) == 'function' then
            local gameButton = types.button
            local Bitmap = import('/lua/maui/bitmap.lua').Bitmap
            types.button = function(parent, optionItemData)
                local bg = gameButton(parent, optionItemData)
                local rowOnDestroy = bg and rawget(bg, 'OnDestroy')
                if rowOnDestroy then
                    bg.OnDestroy = function(self)
                        rowOnDestroy(self)
                        Bitmap.OnDestroy(self)
                    end
                end
                return bg
            end
        else
            WARN("FA Remastered: the Options dialog's button rows left as the game makes them (its locals not found): a skin change after them may fail")
        end
    end

    -- the width each of n tab buttons gets: the texture's (the game's) where they fit, else what fills the panel
    function RemasterTabWidth(n)  -- (also for the tests)
        local UIUtil = import('/lua/ui/uiutil.lua')
        local panel = GetTextureDimensions(UIUtil.UIFile('/scx_menu/options/panel_bmp.dds'))
        local tab = GetTextureDimensions(UIUtil.UIFile('/scx_menu/tab_btn/tab_btn_up.dds'))
        if not (panel and tab and panel > 0 and tab > 0) or n < 1 then return nil end
        return math.min(tab, math.floor((panel - 10 + 10 * (n - 1)) / n))
    end

    -- the dialog's tab buttons in their order (each has its tab's table and is clicked to show it)
    function RemasterOptionsTabButtons(dialog)  -- (also for the tests)
        local O = import('/lua/options/options.lua')
        local byTab, out = {}, {}
        dialog:ApplyFunction(function(control)
            if control.tabData and control.OnClick and not byTab[control.tabData] then byTab[control.tabData] = control end
        end)
        for _, key in O.optionsOrder do
            local button = O.options[key] and byTab[O.options[key]]
            if button then table.insert(out, button) end
        end
        return out
    end

    -- "Tab Defaults" (ours, Chris 2026-11-13): the options of ONE tab put back to their defaults. The game's Reset
    -- puts back every option of every tab at once (the sound's and the gameplay's with the graphics') and closes the
    -- dialog. Ours changes each row of the tab shown as a hand would (the dialog's own item.change: the control, the
    -- dialog's working set, and an option's own update, a slider's preview), so nothing is kept until OK or Apply, and
    -- Cancel leaves the options as they were. Headers and button rows have nothing to put back.
    function RemasterTabDefaults(tabData)  -- (also for the tests) -> how many rows were given their default
        local n = 0
        for _, item in (tabData and tabData.items) or {} do
            if item.type ~= 'header' and item.type ~= 'button' and item.default ~= nil and item.control and item.change then
                item.change(item.control, item.default)
                n = n + 1
            end
        end
        return n
    end
    -- its button, left of the game's Reset (found by its label: the buttons are locals of the game's CreateDialog),
    -- made as the game makes its four
    local function AddTabDefaults(dialog, currentTab)
        local UIUtil = import('/lua/ui/uiutil.lua')
        local LayoutHelpers = import('/lua/maui/layouthelpers.lua')
        local Button = import('/lua/maui/button.lua').Button
        local reset = false
        dialog:ApplyFunction(function(control)
            if not reset and control.label and control.label.GetText and control.OnClick
                and control.label:GetText() == LOC("<LOC _Reset>") then
                reset = control
            end
        end)
        if not reset then
            WARN("FA Remastered: the Options dialog has no Reset button where our hook looks for it: Tab Defaults is left out")
            return
        end
        local btn = Button(dialog,
            UIUtil.UIFile('/scx_menu/small-short-btn/small-btn_up.dds'),
            UIUtil.UIFile('/scx_menu/small-short-btn/small-btn_down.dds'),
            UIUtil.UIFile('/scx_menu/small-short-btn/small-btn_over.dds'),
            UIUtil.UIFile('/scx_menu/small-short-btn/small-btn_dis.dds'),
            "UI_Opt_Yes_No", "UI_Opt_Affirm_Over")
        btn.label = UIUtil.CreateText(btn, "Tab Defaults", 14)
        LayoutHelpers.AtCenterIn(btn.label, btn)
        LayoutHelpers.LeftOf(btn, reset, -6)
        import('/lua/ui/game/tooltip.lua').AddButtonTooltip(btn, 'options_tab_defaults')
        btn.OnRolloverEvent = function(self, event)
            if event == 'enter' then
                self.label:SetColor('ff000000')
            elseif event == 'exit' then
                self.label:SetColor(UIUtil.fontColor)
            end
        end
        btn.OnClick = function(self, modifiers)
            local tab = currentTab()
            if tab and not IsDestroyed(tab) then RemasterTabDefaults(tab.tabData) end
        end
        dialog.remasterTabDefaults = btn  -- (for the tests)
        -- The game's four buttons fill the row (each as wide as its texture, 6 over the next, 15 from the panel's right
        -- edge): a fifth would stand out of the panel on the left. The five get the width that fits them between 15 and
        -- 15, the textures drawn narrower, as the tab buttons above; the places stay the game's own (LeftOf).
        local row = { btn }
        dialog:ApplyFunction(function(control)
            if control ~= btn and control.label and control.label.GetText and control.OnClick then
                local text = control.label:GetText()
                if text == LOC("<LOC _Reset>") or text == LOC("<LOC _Ok>") or text == LOC("<LOC _Cancel>") or text == LOC("<LOC _Apply>") then
                    table.insert(row, control)
                end
            end
        end)
        local panel = GetTextureDimensions(UIUtil.UIFile('/scx_menu/options/panel_bmp.dds'))
        local own = GetTextureDimensions(UIUtil.UIFile('/scx_menu/small-short-btn/small-btn_up.dds'))
        if table.getn(row) == 5 and panel and own and panel > 0 and own > 0 then
            local width = math.min(own, math.floor((panel - 30 + 6 * 4) / 5))
            for _, control in row do LayoutHelpers.SetWidth(control, width) end
        else
            WARN("FA Remastered: the Options dialog's buttons left as wide as the game makes them (not its four found): Tab Defaults may stand out of the panel")
        end
    end

    function CreateDialog(over, exitBehavior)
        -- (the sliders that showed their effect while dragged are given the kept values when the dialog closes, by
        -- OK or by Cancel: options.lua's RemasterEndPreview, before the caller's own exit)
        RemasterGameCreateDialog(over, function()
            local ended, why = pcall(import('/lua/options/options.lua').RemasterEndPreview)
            if not ended then WARN("FA Remastered: the Options' slider preview was not ended: " .. tostring(why)) end
            if exitBehavior then exitBehavior() end
        end)
        local LayoutHelpers = import('/lua/maui/layouthelpers.lua')
        local dialog = Upvalue(RemasterGameCreateDialog, 'dialog')
        local gameSetNewPage = SetNewPage  -- (the dialog's own, made anew by each CreateDialog)
        if not dialog or IsDestroyed(dialog) or type(gameSetNewPage) ~= 'function'
            or Upvalue(gameSetNewPage, 'currentTabBitmap') == nil then
            WARN("FA Remastered: the Options dialog's tabs left as the game lays them (its locals not found)")
            return
        end
        AddTabDefaults(dialog, function() return Upvalue(gameSetNewPage, 'currentTabButton') end)
        local buttons = RemasterOptionsTabButtons(dialog)
        local width = RemasterTabWidth(table.getn(buttons))
        if not width then return end
        for _, button in buttons do
            LayoutHelpers.SetWidth(button, width)
        end
        -- the chosen tab's lit picture as wide as its button: the first tab's, made already, and each later one's
        local function FitLit()
            local lit, button = Upvalue(gameSetNewPage, 'currentTabBitmap'), Upvalue(gameSetNewPage, 'currentTabButton')
            if lit and button and not IsDestroyed(lit) and not IsDestroyed(button) then
                lit.Width:Set(function() return button.Width() end)
                dialog.remasterLitTab = lit  -- (for the tests)
            end
        end
        FitLit()
        SetNewPage = function(tabControl)
            gameSetNewPage(tabControl)
            FitLit()
        end
    end
end
