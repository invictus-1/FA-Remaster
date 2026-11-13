-- The Options dialog with our Remastered tab (Chris, 2026-11-08; docs/ui-options-layout.md): five tab buttons narrowed
-- to fit the panel (ui/data/hook/lua/ui/dialogs/options.lua), the chosen one's lit picture as wide, each label inside
-- its button; the Remastered tab's 55 rows; the Remastered Preset setting its looks on that page (Sharpen and Shore
-- Soak too: a slider and a toggle) and showing Custom once one of them is changed by hand, "Go to Section" scrolling to
-- a header, Video's "Show" button choosing the Remastered tab, a hover on one of our titles showing its tip; OK saving
-- what the preset set. In the front end:
--   moho64ui --init <init.lua> --prefs <a copy> --stage setup --size 1920x1080 --lua ui/tests/options_tabs.lua
--            --frames 300 --log out.log [--screenshot x.png -- /optcase top|water|video]
-- With /optcase the dialog is left open for the picture: the Remastered tab's top (top), scrolled to Water (water),
-- or Video's end with the Show button (video).
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
local function Upvalue(fn, name)
    if type(fn) ~= 'function' then return nil end
    for i = 1, 200 do
        local n, v = debug.getupvalue(fn, i)
        if not n then return nil end
        if n == name then return v end
    end
end

local case = (GetCommandLineArg("/optcase", 1) or {})[1]
local O = import('/lua/options/options.lua')
local Dialog = import('/lua/ui/dialogs/options.lua')
local optionslogic = import('/lua/options/optionslogic.lua')

local function Open()
    Dialog.CreateDialog(GetFrame(0))
    local buttons = Dialog.RemasterOptionsTabButtons(GetFrame(0))
    return buttons, buttons[1] and buttons[1]:GetParent()
end
local function Button(buttons, key)
    for _, b in buttons do if b.tabData == O.options[key] then return b end end
end
-- the row (FAF's CreateOption box) of an option key on the page shown, and the page's grid
local function Row(key)
    local row = false
    GetFrame(0):ApplyFunction(function(control)
        if not row and control._label and control._label._tipText == key then row = control end
    end)
    return row, row and row:GetParent()
end
local function TopRow(grid)
    local _, _, visibleMin = grid:GetScrollValues('Vert')
    return visibleMin + 1
end
local function Index(items, pred)
    for i, item in items do if pred(item) then return i end end
end
local function WorkingSet()
    return Upvalue(Upvalue(Dialog.CreateDialog, 'RemasterGameCreateDialog'), 'currentOptionsSet')
end
local function Item(key) return optionslogic.FindOptionItem(key) end
local function Choose(key, value)  -- a toggle's drop-down chosen as a click chooses it
    local combo = Item(key).control
    for index, k in combo.keyMap do
        if k == value then
            combo:SetItem(index)
            combo:OnClick(index, nil)
            return true
        end
    end
    return false
end

ForkThread(function()
    WaitSeconds(0.5)
    local buttons, dialog = Open()
    check("the dialog is open with five tab buttons", table.getn(buttons), 5)
    check("in the order Gameplay, Interface, Video, Remastered, Sound", buttons[4] and buttons[4].tabData == O.options.remaster and
        buttons[3].tabData == O.options.video and buttons[5].tabData == O.options.sound, true)
    -- (the textures as the game has them: the panel 698 wide, a tab button 178; the game's four end 6 short of the edge)
    local UIUtil = import('/lua/ui/uiutil.lua')
    local panelW = GetTextureDimensions(UIUtil.UIFile('/scx_menu/options/panel_bmp.dds'))
    local tabW = GetTextureDimensions(UIUtil.UIFile('/scx_menu/tab_btn/tab_btn_up.dds'))
    LOG("options_tabs: panel texture " .. tostring(panelW) .. ", tab texture " .. tostring(tabW))
    local w = Dialog.RemasterTabWidth(5)
    check("five tabs as wide as fills the panel, overlapping 10 as the game's", w, math.floor((panelW - 10 + 40) / 5))
    check("four would keep the game's own width (the texture's)", Dialog.RemasterTabWidth(4), tabW)
    check("five are narrower than the game's", w < tabW, true)
    local inside, overlap, labels, same = true, true, {}, true
    for i, b in buttons do
        if b.Left() < dialog.Left() or b.Right() > dialog.Right() then inside = false end
        if buttons[i + 1] and math.abs((b.Right() - buttons[i + 1].Left()) - 10 * b.Width() / w) > 1 then
            overlap = false
        end
        if b.Width() ~= buttons[1].Width() then same = false end
        if b.label.Width() > b.Width() - 12 then table.insert(labels, b.label:GetText() .. " " .. b.label.Width() .. "/" .. b.Width()) end
    end
    LOG("options_tabs: dialog " .. dialog.Left() .. ".." .. dialog.Right() .. ", tabs " .. buttons[1].Left() .. ".." ..
        buttons[5].Right() .. ", each " .. buttons[1].Width() .. ", Remastered's label " .. buttons[4].label.Width())
    check("all five as wide, each inside the panel", tostring(same) .. " " .. tostring(inside), "true true")
    check("each overlapping the next by 10, as the game lays them", overlap, true)
    check("the last ends inside the panel, at most 6 short of its edge (the game's four: 6)",
        dialog.Right() - buttons[5].Right() >= 0 and dialog.Right() - buttons[5].Right() <= 6, true)
    check("each label fits its button (6 to spare each side)", table.concat(labels, ", "), "")
    check("the chosen tab's lit picture as wide as its button", dialog.remasterLitTab and dialog.remasterLitTab.Width(), buttons[1].Width())

    -- the Remastered tab
    Button(buttons, 'remaster'):OnClick()
    check("chosen: its button hides under the lit picture", Button(buttons, 'remaster'):IsHidden(), true)
    check("and the lit picture is as wide", dialog.remasterLitTab and dialog.remasterLitTab.Width(), buttons[1].Width())
    local row, grid = Row('remaster_goto_section')
    local lines = nil
    if grid then _, lines = grid:GetScrollValues('Vert') end
    check("55 rows on the page", lines, 55)
    check("Go to Section first, at the top", grid and TopRow(grid), 1)
    check("it shows the first section", Item('remaster_goto_section').control:GetItem(), 1)

    -- the tooltip of one of ours (the dialog's own hover: Tooltip.CreateMouseoverDisplay("options_" .. key)); its row
    -- scrolled into view first (the grid places only the rows it shows: a hover elsewhere cannot happen)
    Choose('remaster_goto_section', 3)
    local label = Row('remaster_atmosphere')._label
    label:HandleEvent({ Type = 'MouseEnter' })
    local tip, none = false, false
    GetFrame(0):ApplyFunction(function(control)
        if control.GetText then
            local t = control:GetText() or ""
            if string.find(t, "Light in the air", 1, true) then tip = true end
            if t == "No Description" then none = true end
        end
    end)
    label:HandleEvent({ Type = 'MouseExit' })
    check("hovering Atmosphere's title shows its tip, not 'No Description'", tostring(tip) .. " " .. tostring(none), "true false")

    -- the preset sets its looks on this page: Low (Sharpen 0, Shore Soak 0, Atmosphere 0)
    Choose('remaster_preset', 1)
    local set = WorkingSet() or {}
    check("the Remastered Preset Low: the working set has Sharpen 0, Shore Soak 0, Atmosphere 0, Unit Glow 75",
        tostring(set.remaster_sharpen) .. " " .. tostring(set.remaster_shore_soak) .. " " .. tostring(set.remaster_atmosphere) ..
        " " .. tostring(set.remaster_unit_glow), "0 0 0 75")
    check("and the controls show it", Item('remaster_sharpen').control:GetValue() .. " " ..
        Item('remaster_shore_soak').control.keyMap[Item('remaster_shore_soak').control:GetItem()], "0 0")
    Choose('remaster_preset', 0)
    set = WorkingSet() or {}
    check("Off: Unit Glow at Night 100, the original look", tostring(set.remaster_unit_glow) .. " " .. tostring(set.remaster_look), "100 original")

    -- a look of the preset changed by hand: the preset shows Custom, as the game's Fidelity Presets do (its own setting of
    -- the looks, sliders too, leaves it as chosen; a look chosen as the level has it changes nothing)
    local function Preset() local c = Item('remaster_preset').control return c.keyMap[c:GetItem()] end
    Choose('remaster_preset', 4)
    check("Ultra chosen: its own setting of the looks (Sharpen's and Unit Glow's sliders too) leaves it Ultra", Preset(), 4)
    Choose('remaster_atmosphere', 1)
    check("Atmosphere chosen as Ultra has it: still Ultra", Preset(), 4)
    Choose('remaster_atmosphere', 0)
    check("Atmosphere turned off by hand: the preset shows Custom", Preset(), 5)
    check("and the dialog's working set has Custom", (WorkingSet() or {}).remaster_preset, 5)
    Choose('remaster_preset', 3)
    check("High chosen: High again, Atmosphere on", Preset() .. " " .. tostring((WorkingSet() or {}).remaster_atmosphere), "3 1")
    local sharpen = Item('remaster_sharpen').control
    sharpen:SetValue(20)
    sharpen:OnValueSet(sharpen:GetValue())
    check("Sharpen's slider let go of at 20 by hand (High: 50): Custom", Preset(), 5)
    Choose('remaster_preset', 1)

    -- Go to Section
    local items = O.options.remaster.items
    local water = Index(items, function(item) return item.type == 'header' and item.title == "Water" end)
    Choose('remaster_goto_section', 5)
    check("Go to Section Water: the Water header at the top", TopRow(grid), water)
    local units = Index(items, function(item) return item.type == 'header' and item.title == "Units & Readability" end)
    Choose('remaster_goto_section', 7)
    check("Units & Readability likewise", TopRow(grid), units)
    Choose('remaster_goto_section', 8)
    local _, _, vmin, vmax = grid:GetScrollValues('Vert')
    local picture = Index(items, function(item) return item.type == 'header' and item.title == "Picture & Camera" end)
    check("the last, Picture & Camera: scrolled to the end, its header in view", vmax == 55 and picture > vmin and picture <= vmax, true)

    -- Video: the game's own rows, then our header and the Show button, which chooses the Remastered tab
    Button(buttons, 'video'):OnClick()
    local show = Item('remaster_show_tab').control
    check("the Video tab has the Show button", show ~= nil and show._button ~= nil, true)
    local _, videoLines = Row('remaster_show_tab'):GetParent():GetScrollValues('Vert')
    check("Video's rows: the game's 12 (its 17 less the five we leave out), our header and the button", videoLines, 14)
    show._button:OnClick()
    WaitSeconds(0.2)
    check("Show chooses the Remastered tab", Button(buttons, 'remaster'):IsHidden() and not Button(buttons, 'video'):IsHidden(), true)
    check("which shows its first section again", Item('remaster_goto_section').control:GetItem(), 1)

    -- OK saves what the preset set
    local ok = false
    dialog:ApplyFunction(function(control)
        if not ok and control.label and control.label.GetText and control.label:GetText() == LOC("<LOC _Ok>") and control.OnClick then ok = control end
    end)
    check("the dialog's OK", ok ~= false, true)
    if ok then ok:OnClick() end
    local saved = optionslogic.GetCurrent()
    check("after OK the profile has Low's looks: preset 1, Sharpen 0, Shore Soak 0, Atmosphere 0",
        tostring(saved.remaster_preset) .. " " .. tostring(saved.remaster_sharpen) .. " " .. tostring(saved.remaster_shore_soak) .. " " ..
        tostring(saved.remaster_atmosphere), "1 0 0 0")
    check("and the program took them", tostring(moho64ui_Looks().sharpen) .. " " .. tostring(moho64ui_Looks().shoreSoak) .. " " ..
        tostring(moho64ui_Looks().atmosphere), "0 false false")

    -- Tab Defaults (ours): the tab shown put back to its defaults in the dialog's working set, the other tabs' options
    -- left alone, nothing kept until OK, and Cancel leaves it all
    do
        local b2, d2 = Open()
        local tabDefaults = d2.remasterTabDefaults
        LOG("options_tabs: Tab Defaults " .. tostring(tabDefaults and tabDefaults.Left()) .. ".." .. tostring(tabDefaults and tabDefaults.Right()) .. ", its label " .. tostring(tabDefaults and tabDefaults.label.Width()) .. ", the dialog from " .. d2.Left())
        check("the dialog has a Tab Defaults button inside the panel, its label inside it (6 to spare each side)",
            tabDefaults ~= nil and tabDefaults.Left() >= d2.Left() and tabDefaults.label.Width() <= tabDefaults.Width() - 12, true)
        -- (the five buttons of the row narrowed to fit: each label still inside its button)
        local tight = {}
        d2:ApplyFunction(function(control)
            if control.label and control.label.GetText and control.OnClick and not control.tabData
                and control.label.Width() > control.Width() - 12 then
                table.insert(tight, control.label:GetText() .. " " .. control.label.Width() .. "/" .. control.Width())
            end
        end)
        check("and every button's label of the dialog fits its button", table.concat(tight, ", "), "")
        Button(b2, 'remaster'):OnClick()
        check("before: the Remastered tab shows Low's looks (kept above)", tostring((WorkingSet() or {}).remaster_preset) .. " " ..
            tostring((WorkingSet() or {}).remaster_sharpen), "1 0")
        local soundBefore = (WorkingSet() or {}).master_volume
        tabDefaults:OnClick()
        local set2 = WorkingSet() or {}
        local wrong = {}
        for _, item in O.options.remaster.items do
            if item.type ~= 'header' and item.type ~= 'button' and item.default ~= nil and set2[item.key] ~= item.default then
                table.insert(wrong, item.key .. "=" .. tostring(set2[item.key]) .. "/" .. tostring(item.default))
            end
        end
        check("Tab Defaults: every option of the Remastered tab is at its default in the working set", table.concat(wrong, " "), "")
        check("the preset's control shows its default, Sharpen's slider its own", Item('remaster_preset').control.keyMap[Item('remaster_preset').control:GetItem()]
            .. " " .. Item('remaster_sharpen').control:GetValue(), Item('remaster_preset').default .. " " .. Item('remaster_sharpen').default)
        check("another tab's option is left alone (the sound's master volume)", set2.master_volume, soundBefore)
        check("nothing is kept yet: the profile still has Low", optionslogic.GetCurrent().remaster_preset, 1)
        d2.cancelBtn:OnClick()
        check("Cancel leaves the profile as it was", tostring(optionslogic.GetCurrent().remaster_preset) .. " " .. tostring(optionslogic.GetCurrent().remaster_sharpen), "1 0")
        check("and the program's looks (the sliders' preview is taken back)", tostring(moho64ui_Looks().sharpen), "0")
    end

    -- left open for a picture
    if case then
        buttons, dialog = Open()
        if case == 'video' then
            Button(buttons, 'video'):OnClick()
            local _, g = Row('remaster_show_tab')
            g:ScrollSetTop('Vert', 99)
        else
            Button(buttons, 'remaster'):OnClick()
            if case == 'water' then Choose('remaster_goto_section', 5) end
        end
        LOG("options_tabs: left open for the picture: " .. case)
    end
    -- our hooks read locals of the game's scripts through the functions that use them; one that is gone (a FAF update)
    -- is said once in the log (the lab's suite looks for that line), and the lookup gives nil
    local OptionsDialog = import("/lua/ui/dialogs/options.lua")
    check("the hook finds a local of the game's dialog (CreateOption)", type(OptionsDialog.RemasterHookLocal('CreateOption')), 'function')
    check("a local that is not there: nil", OptionsDialog.RemasterHookLocal('NoSuchLocalOfTheDialog'), nil)
    check("asked again: nil again (the warning is said once)", OptionsDialog.RemasterHookLocal('NoSuchLocalOfTheDialog'), nil)
    LOG("TEST done: " .. failed .. " failed")
end)
