-- The Options' sliders show their effect while they are dragged (Chris's pick of 2026-11-09; ui/data/hook/lua/options/
-- options.lua Preview / RemasterEndPreview, the dialog's hook): Unit Glow at Night, Haze & Mist and Sharpen send
-- their look at every step of a drag; Cancel puts the kept value back, also for a slider on a tab the player has
-- left; OK keeps the new one. In the front end:
--   moho64ui --stage setup --size 1920x1080 --lua ui/tests/options_preview.lua --frames 300 --log out.log
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
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
local function Item(key) return optionslogic.FindOptionItem(key) end
local function Looks() return moho64ui_Looks() end
local function near(a, b) return type(a) == 'number' and math.abs(a - b) < 0.001 end
local function previewed()
    local n = 0
    for _ in O.RemasterPreviewed do n = n + 1 end
    return n
end
local function Press(dialog, label)
    local found = false
    dialog:ApplyFunction(function(control)
        if not found and control.label and control.label.GetText and control.label:GetText() == LOC(label) and control.OnClick then found = control end
    end)
    if found then found:OnClick() end
    return found ~= false
end

ForkThread(function()
    WaitSeconds(0.5)
    local haze0, sharp0, glow0 = Looks().hazeMist, Looks().sharpen, Looks().unitGlow
    LOG("TEST note at the start: haze " .. tostring(haze0) .. ", sharpen " .. tostring(sharp0) .. ", glow " .. tostring(glow0))
    local buttons, dialog = Open()
    Button(buttons, 'remaster'):OnClick()
    WaitSeconds(0.2)
    check("opening the dialog changes no look", near(Looks().hazeMist, haze0) and near(Looks().sharpen, sharp0) and near(Looks().unitGlow, glow0), true)

    -- a drag: each step reaches the picture, nothing is kept yet
    local haze = Item('remaster_haze_mist').control
    haze:OnScrub(150)
    check("Haze & Mist dragged to 150: the look follows at once", near(Looks().hazeMist, 1.5), true)
    haze:OnScrub(40)
    check("and on to 40", near(Looks().hazeMist, 0.4), true)
    check("the profile keeps what it had", (optionslogic.GetCurrent().remaster_haze_mist or 100) ~= 40, true)
    local glow = Item('remaster_unit_glow').control
    glow:OnScrub(20)
    check("Unit Glow at Night dragged to 20 likewise", near(Looks().unitGlow, 0.2), true)
    check("two sliders are marked", previewed(), 2)
    -- Cancel on ANOTHER tab (the game's own cancel would miss the sliders: it asks only the tab shown)
    Button(buttons, 'video'):OnClick()
    WaitSeconds(0.2)
    check("Cancel", Press(dialog, "<LOC _Cancel>"), true)
    WaitSeconds(0.2)
    check("Cancel puts both back, from another tab too", near(Looks().hazeMist, haze0) and near(Looks().unitGlow, glow0), true)
    check("and nothing is marked", previewed(), 0)

    -- a value set (the slider let go) and OK: kept
    buttons, dialog = Open()
    Button(buttons, 'remaster'):OnClick()
    WaitSeconds(0.2)
    local sharpen = Item('remaster_sharpen').control
    sharpen:OnScrub(80)
    sharpen:SetValue(80)
    sharpen:OnValueSet(80)  -- (the slider's own call when the button is let go)
    check("Sharpen let go at 80: the look has it before OK", near(Looks().sharpen, 0.8), true)
    check("OK", Press(dialog, "<LOC _Ok>"), true)
    WaitSeconds(0.2)
    check("after OK the look stays", near(Looks().sharpen, 0.8), true)
    check("and the profile has it", optionslogic.GetCurrent().remaster_sharpen, 80)

    -- the Remastered Preset moves Sharpen's slider: that shows at once too, and Cancel takes it back
    buttons, dialog = Open()
    Button(buttons, 'remaster'):OnClick()
    WaitSeconds(0.2)
    local preset = Item('remaster_preset').control
    for index, key in preset.keyMap do
        if key == 1 then
            preset:SetItem(index)
            preset:OnClick(index, nil)
        end
    end
    check("the preset Low sets Sharpen's slider to 0", Item('remaster_sharpen').control:GetValue(), 0)
    check("and the look follows before OK", near(Looks().sharpen, 0), true)
    check("Cancel again", Press(dialog, "<LOC _Cancel>"), true)
    WaitSeconds(0.2)
    check("Cancel: Sharpen is the kept 80 again", near(Looks().sharpen, 0.8), true)
    LOG("TEST done: " .. failed .. " failed")
end)
