-- The Options' graphics-memory strip (ours; /moho64ui/lua/memory_strip.lua, ui/user/card_memory.cpp; Chris's wish of
-- 2026-11-14, ui/REQUESTS.md 57). In a game whose pictures imitate a smaller card (MOHO64UI_CARD_IMITATION=<MB>, given
-- again as "-- /stripcase <MB>" so that the test knows it): the strip shows under the Options dialog on the Video and
-- Remastered tabs and not on the others; its first line has the estimate and the card's memory; it follows a change
-- made in the dialog before Apply (Texture Detail through the dialog's own control); a part not counted yet is never
-- shown as free; at 4096 MB with the settings high the strip says the card is too small and names the settings that
-- give the most back, each one a real saving. The runner: the lab's suite/memory_strip.sh (8192 and 4096).
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local card = tonumber((GetCommandLineArg("/stripcase", 1) or {})[1] or "") or 8192
local O = import('/lua/options/options.lua')
local Dialog = import('/lua/ui/dialogs/options.lua')
local optionslogic = import('/lua/options/optionslogic.lua')
local strip = import('/moho64ui/lua/memory_strip.lua')
local function Button(buttons, key)
    for _, b in buttons do if b.tabData == O.options[key] then return b end end
end
local function Item(key) return optionslogic.FindOptionItem(key) end
local function Choose(key, value)  -- a drop-down chosen as a click chooses it
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
local function Wait(n) for i = 1, n do WaitFrames(1) end end
ForkThread(function()
    WaitSeconds(3)
    check("the host's estimate is there", moho64ui_CardMemoryEstimate ~= nil, true)
    local now = moho64ui_CardMemoryNow()
    check("the card's memory is the imitated card's", now and math.floor(now.total + 0.5), card)
    LOG("TEST note the card now: total " .. tostring(now and now.total) .. " MB, budget " .. tostring(now and now.budget) .. " MB, used " .. tostring(now and now.used) .. " MB")

    Dialog.CreateDialog(GetFrame(0))
    local buttons = Dialog.RemasterOptionsTabButtons(GetFrame(0))
    local dialog = buttons[1] and buttons[1]:GetParent()
    Button(buttons, 'video'):OnClick()
    Wait(40)
    local S = strip.State
    check("the strip is made with the dialog", S.panel ~= false and not IsDestroyed(S.panel), true)
    check("and shown on the Video tab", S.hidden, false)
    local first = S.lines[1] or ""
    LOG("TEST note the strip: " .. table.concat(S.lines, " / "))
    check("its first line: the estimate", string.find(first, "Graphics memory, estimated for a large late game: about ", 1, true) == 1, true)
    check("and the card's own memory, a figure that stands still", string.find(first, string.format(" of the card's %.1f GB", card / 1024), 1, true) ~= nil, true)
    -- what the armies may build is made ahead only while there is room: kept out of the sum, said on a line of its own
    local needed, ahead = strip.Needed(S.estimate)
    LOG(string.format("TEST note needed %.0f MB, made ahead %.0f MB, the estimate's total %.0f MB", needed, ahead, S.estimate.total))
    check("the first line's number is what the game needs (the total without what is made ahead)", string.find(first, string.format("about %.1f GB", needed / 1024), 1, true) ~= nil and ahead > 0, true)
    local said = false
    for _, l in S.lines do
        if string.find(l, "made ahead while there is room", 1, true) then said = true end
    end
    check("and what is made ahead has its own line", said, true)
    check("the strip lies under the dialog", S.panel.Top() >= dialog.Bottom(), true)
    -- centred with the dialog (Chris, 2026-11-14): the two centres within a pixel
    check("and centred with it", math.abs((S.panel.Left() + S.panel.Right()) - (dialog.Left() + dialog.Right())) <= 2, true)
    -- a part not counted yet is never shown as free: every part with 0 and measured false is named as not counted
    local unmeasured = 0
    for _, p in S.estimate.parts do
        if p.mb <= 0 and not p.measured then unmeasured = unmeasured + 1 end
    end
    local said = false
    for _, l in S.lines do
        if string.find(l, "Not counted yet (not free): ", 1, true) == 1 then said = true end
    end
    check("the parts not counted yet are named as such where there are any", said, unmeasured > 0)

    -- a change in the dialog, before Apply: Texture Detail High -> Low lowers the estimate
    Choose('texture_level', 0)
    Wait(40)
    local high = S.estimate.total
    Choose('texture_level', 2)
    Wait(40)
    local low = S.estimate.total
    LOG(string.format("TEST note Texture Detail High %.0f MB, Low %.0f MB", high, low))
    check("Texture Detail Low in the dialog lowers the estimate at once", low < high - 100, true)
    check("and nothing is applied: the profile keeps its value", optionslogic.GetCurrent().texture_level ~= 2 or high == low, true)

    -- the other tabs: no strip
    Button(buttons, 'gameplay'):OnClick()
    Wait(40)
    check("the Gameplay tab: no strip", S.hidden, true)
    Button(buttons, 'remaster'):OnClick()
    Wait(40)
    check("the Remastered tab: the strip again", S.hidden, false)

    if card <= 4096 then
        -- Medium textures on a 4 GB card: the game fits (the renderer's case of 2026-11-17: 2.9 GB needed, though the
        -- total with what is made ahead is over 5): no warning
        Button(buttons, 'video'):OnClick()
        Choose('texture_level', 1)
        Wait(40)
        local warned = false
        for _, l in S.lines do
            if string.find(l, "Over the card's memory by ", 1, true) == 1 then warned = true end
        end
        LOG(string.format("TEST note Medium textures at %d MB: needed %.0f MB, the total %.0f MB", card, S.needed, S.estimate.total))
        check("Medium textures on this card: no warning though the total with what is made ahead is over the card", warned == false and S.needed < card and S.estimate.total > card, true)
        -- the settings high: over the card, and the advice
        Choose('texture_level', 0)
        Wait(40)
        local over = false
        for _, l in S.lines do
            if string.find(l, "Over the card's memory by ", 1, true) == 1 then over = l end
        end
        LOG("TEST note over: " .. tostring(over))
        check("at " .. card .. " MB with Texture Detail High the strip says the card is too small", over ~= false, true)
        check("and names settings that give memory back", table.getn(S.advice) > 0, true)
        local real = true
        for _, a in S.advice do
            if not (a.saves > 50) then real = false end
        end
        check("each a real saving, the most first", real and (table.getn(S.advice) < 2 or S.advice[1].saves >= S.advice[2].saves), true)
        check("Texture Detail among them", S.advice[1] and (S.advice[1].key == 'texture_level' or (S.advice[2] and S.advice[2].key == 'texture_level')), true)
    end

    -- Cancel: the dialog and the strip go, no setting changed
    local cancel = false
    dialog:ApplyFunction(function(control)
        if not cancel and control.label and control.label.GetText and control.label:GetText() == LOC("<LOC _Cancel>") and control.OnClick then cancel = control end
    end)
    if cancel then cancel:OnClick() end
    Wait(10)
    check("Cancel: the strip goes with the dialog", IsDestroyed(S.panel), true)
    LOG("TEST done: " .. failed .. " failed")
end)
