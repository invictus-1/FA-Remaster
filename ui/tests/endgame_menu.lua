-- A game ended the player's way (Chris's playtest of 2026-11-09: "End game doesn't work anymore"; score.lua's test
-- called tabs.EndGame() itself and so skipped all of this): the game's menu is opened (the tab's own click), its
-- "End Game" button clicked, the menu folds away, the "Are you sure you'd like to quit?" dialog comes up with the
-- world covered, its Yes is clicked; then the simulation's score arrives and the score screen is up, without a
-- script error on the way (the runner's strict reading; the screen's own check here).
-- Run it like score.lua: a launched game at 1920x1080, --frames 2400.
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local function upvalue(fn, name)
    for i = 1, 200 do
        local n, v = debug.getupvalue(fn, i)
        if not n then break end
        if n == name then return v end
    end
end
ForkThread(function()
    WaitSeconds(8)
    local tabs = import("/lua/ui/game/tabs.lua")
    local UIUtil = import("/lua/ui/uiutil.lua")
    local score = import("/lua/ui/dialogs/score.lua")
    local hotstats = import("/lua/ui/dialogs/hotstats.lua")
    local controls = tabs.controls or upvalue(tabs.ToggleTab, 'controls')
    check("the game's tabs are there", type(controls) == 'table' and controls.tabs ~= nil, true)
    -- first what Chris did in that game: the Options dialog opened in the game, every tab looked at, OK. A control
    -- of it destroyed while a texture of the skin still points at it makes the score screen's SetCurrentSkin fail
    -- (bitmap.lua's SetNewTexture: "Game object has been destroyed"), and the score screen never comes up.
    local function skinChange()
        local ok, why = pcall(UIUtil.SetCurrentSkin, UIUtil.GetCurrentSkinName())
        return ok and "fine" or tostring(why)
    end
    check("a skin change at the game's start", skinChange(), "fine")
    check("the world's sounds are on in the game", moho64ui_SoundInfo().worldOn, true)
    local Dialog = import('/lua/ui/dialogs/options.lua')
    Dialog.CreateDialog(GetFrame(0))
    WaitSeconds(0.5)
    local buttons = Dialog.RemasterOptionsTabButtons and Dialog.RemasterOptionsTabButtons(GetFrame(0)) or {}
    check("the Options dialog is up in the game, with its tabs", table.getn(buttons) >= 4, true)
    for _, b in buttons do
        if not b:IsHidden() then b:OnClick() end
        WaitSeconds(0.3)
    end
    for _, b in buttons do  -- (the first tab again: every tab's page was made and unmade once)
        if not b:IsHidden() then b:OnClick() break end
    end
    WaitSeconds(0.3)
    check("a skin change with the Options dialog open", skinChange(), "fine")
    local ok = false
    GetFrame(0):ApplyFunction(function(control)
        if not ok and control.label and control.label.GetText and control.label:GetText() == LOC("<LOC _Ok>") and control.OnClick then ok = control end
    end)
    check("the dialog's OK", ok ~= false, true)
    if ok then ok:OnClick() end
    WaitSeconds(1)
    check("a skin change after the Options dialog closed", skinChange(), "fine")
    -- the quit question, caught as the menu's action makes it
    local asked
    local realQuickDialog = UIUtil.QuickDialog
    UIUtil.QuickDialog = function(parent, text, a, b, c, d, e, f, g, h)
        asked = realQuickDialog(parent, text, a, b, c, d, e, f, g, h)
        asked.remasterText = text
        return asked
    end
    tabs.ToggleTab('main')
    WaitSeconds(1.5)
    local group = controls.contentGroup
    check("the menu is open", group and group.Buttons ~= nil, true)
    local endGame
    for _, button in group and group.Buttons or {} do
        if button.label and button.label:GetText() == LOC("<LOC _End_Game>") then endGame = button end
    end
    check("with an End Game button", endGame ~= nil, true)
    if endGame then endGame:OnClick() end
    local waited = 0
    while not asked and waited < 5 do
        WaitSeconds(0.1)
        waited = waited + 0.1
    end
    UIUtil.QuickDialog = realQuickDialog
    check("End Game asks whether to quit", asked and LOC(asked.remasterText), LOC("<LOC EXITDLG_0001>Are you sure you'd like to quit?"))
    check("the menu has folded away", controls.contentGroup, false)
    local yes = asked and (asked._button1 or asked.content and asked.content._button1)  -- (the popup's content has it)
    check("with a Yes button", yes ~= nil and yes ~= false, true)
    if yes then yes:OnClick() end
    check("Yes: the game is over", SessionIsGameOver(), true)
    waited = 0
    while not (hotstats.scoreData.current and hotstats.scoreData.history) and waited < 5 do
        WaitSeconds(0.5)
        waited = waited + 0.5
    end
    check("the simulation's scripts hand over the final score", hotstats.scoreData.current ~= nil and hotstats.scoreData.history ~= nil, true)
    WaitSeconds(3)
    check("the score screen is up", score.scoreScreenActive, true)
    check("with its dialog made (no script error on the way)", score.dialog ~= nil and score.dialog ~= false, true)
    -- (the score screen switches the world's sounds off, FAF's DisableWorldSounds: the battle is silent under it)
    check("and the world's sounds are off under it", moho64ui_SoundInfo().worldOn, false)
    LOG("TEST done: " .. failed .. " failed")
end)
