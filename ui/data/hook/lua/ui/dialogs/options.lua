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
    local function Upvalue(fn, name)
        if type(fn) ~= 'function' then return nil end
        for i = 1, 200 do
            local n, v = debug.getupvalue(fn, i)
            if not n then return nil end
            if n == name then return v end
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

    function CreateDialog(over, exitBehavior)
        RemasterGameCreateDialog(over, exitBehavior)
        local LayoutHelpers = import('/lua/maui/layouthelpers.lua')
        local dialog = Upvalue(RemasterGameCreateDialog, 'dialog')
        local gameSetNewPage = SetNewPage  -- (the dialog's own, made anew by each CreateDialog)
        if not dialog or IsDestroyed(dialog) or type(gameSetNewPage) ~= 'function'
            or Upvalue(gameSetNewPage, 'currentTabBitmap') == nil then
            WARN("FA Remastered: the Options dialog's tabs left as the game lays them (its locals not found)")
            return
        end
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
