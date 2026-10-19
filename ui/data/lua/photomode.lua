-- FA Remastered: the photo mode's panel. This program's own (Chris's approval of 2026-10-18, the visuals session's
-- look 7), not a hook: the game has no photo mode. The host opens and closes the photo mode (the key "Photo mode",
-- Alt-P at first, and the console's remaster_photo_mode: ui/maui/worldview.cpp) and keeps what is not the panel's (the
-- free camera, the pause, the keys); it calls Open and Close here, and TogglePanel for the key H. Open: the game's own
-- panels go away the game's own way (gamemain.HideGameUI, what the console's UI_ToggleGamePanels calls: FA exe
-- 0x8349d0), and come back on Close only if Open put them away; the panel is made of the game's own controls (uiutil's
-- texts and buttons, maui's Slider, the Combo of the Options dialog, the dialogs' background). What it sets goes to the
-- host (moho64ui_PhotoModeSet, moho64ui_PhotoModeTake); moho64ui_PhotoMode() says how things stand.

local UIUtil = import("/lua/ui/uiutil.lua")
local LayoutHelpers = import("/lua/maui/layouthelpers.lua")
local Group = import("/lua/maui/group.lua").Group
local Bitmap = import("/lua/maui/bitmap.lua").Bitmap
local Slider = import("/lua/maui/slider.lua").Slider
local Combo = import("/lua/ui/controls/combo.lua").Combo

-- How things stand, for the tests: open, the panel shown, and whether Open put the game's panels away.
State = { open = false, shown = false, hidGameUI = false }
-- The panel's controls by name (timeSlider, gameOwn, weather, strengthSlider, lens, lensStrengthSlider, depthSlider,
-- autoFocus, focusText, size, take, progress, note), for the tests.
Controls = {}

local panel = false
local watcher = false  -- (the thread that keeps the game's panels away while it is open)
local moment = { hour = 12, weather = 1, strength = 1 }  -- (what the panel last gave, or read)
local labelWidth, sliderWidth, rowHeight, panelWidth = 132, 200, 34, 440
-- The lens's aperture by the slider's place 0..1: f/1.4 .. f/22, an even step for each f-stop.
local fLow, fHigh = 1.4, 22

local function FNumberAt(v) return fLow * math.pow(fHigh / fLow, v) end
local function PlaceOfFNumber(f) return math.log(f / fLow) / math.log(fHigh / fLow) end
local function Capitalised(name) return string.upper(string.sub(name, 1, 1)) .. string.sub(name, 2) end
local function HourText(hour)
    local minutes = math.floor(hour * 60 + 0.5)
    local h = math.floor(minutes / 60)
    return string.format("%02d:%02d", h - 24 * math.floor(h / 24), minutes - 60 * h)
end
local function Percent(v) return string.format("%d%%", math.floor(v * 100 + 0.5)) end

-- A label at the left of a row.
local function Label(parent, text, top)
    local label = UIUtil.CreateText(parent, text, 14, UIUtil.bodyFont)
    LayoutHelpers.AtLeftTopIn(label, parent, 0, top + 7)
    return label
end
-- A slider of the game's own look (the Options dialog's) in a row, with its value's text to the right.
local function RowSlider(parent, top, from, to)
    local slider = Slider(parent, false, from, to,
        UIUtil.SkinnableFile('/slider02/slider_btn_up.dds'), UIUtil.SkinnableFile('/slider02/slider_btn_over.dds'),
        UIUtil.SkinnableFile('/slider02/slider_btn_down.dds'), UIUtil.SkinnableFile('/slider02/slider-back_bmp.dds'))
    LayoutHelpers.AtLeftTopIn(slider, parent, labelWidth, top + 5)
    local value = UIUtil.CreateText(parent, "", 14, UIUtil.bodyFont)
    LayoutHelpers.AtLeftTopIn(value, parent, labelWidth + sliderWidth + 12, top + 7)
    return slider, value
end
-- A combo of the game's own look (the Options dialog's) in a row: its items, the one shown first (1 ..).
local function RowCombo(parent, top, items, shown, width)
    local combo = Combo(parent, 14, 10, nil, nil, "UI_Tab_Click_01", "UI_Tab_Rollover_01")
    LayoutHelpers.SetWidth(combo, width or sliderWidth)
    LayoutHelpers.AtLeftTopIn(combo, parent, labelWidth, top + 5)
    combo:AddItems(items, shown)
    return combo
end
local function SmallButton(parent, text)
    return UIUtil.CreateButtonStd(parent, '/dialogs/standard-small_btn/standard-small', text, 12, 2, 0,
        "UI_Opt_Mini_Button_Click", "UI_Opt_Mini_Button_Over")
end

-- The moment as the panel has it, to the host (the picture alone: the game's own clock and weather go on).
local function GiveMoment()
    moho64ui_PhotoModeSet({ hour = moment.hour, weather = moment.weather, strength = moment.strength })
end
-- The time of day's slider and text from `moment`; `own`: the game's own hour (-1 the map's own fixed light).
local function ShowMoment(own)
    local c = Controls
    if not c.timeSlider then return end
    c.timeSlider:SetValue(moment.hour)
    c.timeText:SetText(own and own < 0 and "Map's light" or HourText(moment.hour))
    c.weather:SetItem(moment.weather + 1)
    c.strengthSlider:SetValue(moment.strength)
    c.strengthText:SetText(Percent(moment.strength))
end
-- The game's own hour and weather as they are now (with the map's own fixed light the slider starts at noon).
local function ReadMoment()
    local now = moho64ui_PhotoMode().moment
    moment.hour = now.hour >= 0 and now.hour or 12
    moment.weather = now.weather
    moment.strength = now.strength
    return now.hour
end
local function FocusText(focus)
    if focus and focus > 0 then return string.format("%.0f away (clicked)", focus) end
    return "Auto (click the world to focus)"
end

local function MakePanel()
    local state = moho64ui_PhotoMode()
    local root = GetFrame(0)
    panel = Group(root, "photoModePanel")
    Controls = {}
    local c = Controls
    local rows = 13
    LayoutHelpers.SetDimensions(panel, panelWidth, rows * rowHeight + 70)
    panel.Depth:Set(root:GetTopmostDepth() + 10)
    LayoutHelpers.AtRightTopIn(panel, root, 60, 90)
    -- the game's dialogs' background (FAF's Popup: the lobby's dialog nine-patch round the content)
    local background = UIUtil.CreateNinePatchStd(panel, '/scx_menu/lan-game-lobby/dialog/background/')
    LayoutHelpers.FillParentFixedBorder(background, panel, 64)
    LayoutHelpers.DepthUnderParent(background, panel)

    local title = UIUtil.CreateText(panel, "Photo Mode", 20, UIUtil.titleFont)
    LayoutHelpers.AtLeftTopIn(title, panel, 0, 0)
    local top = 36

    -- Time of Day: 0 .. 24 h (the picture's own; Game's Own gives the game's hour and weather back)
    local own = ReadMoment()
    Label(panel, "Time of Day", top)
    c.timeSlider, c.timeText = RowSlider(panel, top, 0, 24)
    c.timeSlider.OnValueChanged = function(self, value) c.timeText:SetText(HourText(value)) end
    local function hourSet(self, value)
        moment.hour = value
        GiveMoment()
    end
    c.timeSlider.OnScrub = hourSet
    c.timeSlider.OnValueSet = hourSet
    top = top + rowHeight

    -- Weather: the renderer's kinds, and how hard it is
    Label(panel, "Weather", top)
    local kinds = {}
    for i, name in state.weatherKinds do kinds[i] = Capitalised(name) end
    c.weather = RowCombo(panel, top, kinds, moment.weather + 1)
    c.weather.OnClick = function(self, index, text)
        moment.weather = index - 1
        GiveMoment()
    end
    c.gameOwn = SmallButton(panel, "Game's Own")
    LayoutHelpers.AtLeftTopIn(c.gameOwn, panel, labelWidth + sliderWidth + 12, top + 2)
    c.gameOwn.OnClick = function(self)
        moho64ui_PhotoModeSet({ gameOwn = true })
        ShowMoment(ReadMoment())
    end
    top = top + rowHeight
    Label(panel, "Weather Strength", top)
    c.strengthSlider, c.strengthText = RowSlider(panel, top, 0, 1)
    c.strengthSlider.OnValueChanged = function(self, value) c.strengthText:SetText(Percent(value)) end
    local function strengthSet(self, value)
        moment.strength = value
        GiveMoment()
    end
    c.strengthSlider.OnScrub = strengthSet
    c.strengthSlider.OnValueSet = strengthSet
    top = top + rowHeight
    ShowMoment(own)

    -- Lens: depth of field, its strength, its aperture ("Blur Depth": f/1.4 shallow .. f/22 nearly all sharp), the focus
    Label(panel, "Lens", top)
    c.lens = RowCombo(panel, top, { "Off", "On" }, state.lens.on and 2 or 1)
    c.lens.OnClick = function(self, index) moho64ui_PhotoModeSet({ lens = index == 2 }) end
    top = top + rowHeight
    Label(panel, "Lens Strength", top)
    c.lensStrengthSlider, c.lensStrengthText = RowSlider(panel, top, 0, 1)
    c.lensStrengthSlider.OnValueChanged = function(self, value) c.lensStrengthText:SetText(Percent(value)) end
    local function lensStrengthSet(self, value) moho64ui_PhotoModeSet({ lensStrength = value }) end
    c.lensStrengthSlider.OnScrub = lensStrengthSet
    c.lensStrengthSlider.OnValueSet = lensStrengthSet
    c.lensStrengthSlider:SetValue(state.lens.strength)
    top = top + rowHeight
    Label(panel, "Blur Depth", top)
    c.depthSlider, c.depthText = RowSlider(panel, top, 0, 1)
    c.depthSlider.OnValueChanged = function(self, value) c.depthText:SetText(string.format("f/%.1f", FNumberAt(value))) end
    local function depthSet(self, value) moho64ui_PhotoModeSet({ fNumber = FNumberAt(value) }) end
    c.depthSlider.OnScrub = depthSet
    c.depthSlider.OnValueSet = depthSet
    c.depthSlider:SetValue(PlaceOfFNumber(state.lens.fNumber))
    top = top + rowHeight
    Label(panel, "Focus", top)
    c.focusText = UIUtil.CreateText(panel, FocusText(state.lens.focus), 14, UIUtil.bodyFont)
    LayoutHelpers.AtLeftTopIn(c.focusText, panel, labelWidth, top + 7)
    c.autoFocus = SmallButton(panel, "Auto Focus")
    LayoutHelpers.AtLeftTopIn(c.autoFocus, panel, labelWidth + sliderWidth + 12, top + 2)
    c.autoFocus.OnClick = function(self) moho64ui_PhotoModeSet({ focus = "auto" }) end
    top = top + rowHeight

    -- Photo Size: 1x .. 4x the screen's
    Label(panel, "Photo Size", top)
    local sizes = {}
    for k = 1, 4 do sizes[k] = string.format("%dx  (%d x %d)", k, k * state.width, k * state.height) end
    c.size = RowCombo(panel, top, sizes, state.size)
    c.size.OnClick = function(self, index) moho64ui_PhotoModeSet({ size = index }) end
    top = top + rowHeight + 6

    -- Take Photo, its progress while it is made, and a line about the last one
    c.take = UIUtil.CreateButtonWithDropshadow(panel, '/BUTTON/medium/', "Take Photo")
    LayoutHelpers.AtLeftTopIn(c.take, panel, 0, top)
    c.take.OnClick = function(self)
        local ok, why = moho64ui_PhotoModeTake()
        if not ok then c.note:SetText("No photo: " .. tostring(why)) end
    end
    local barBack = Bitmap(panel)
    barBack:SetSolidColor('ff202830')
    LayoutHelpers.SetDimensions(barBack, panelWidth - 150, 10)
    LayoutHelpers.AtLeftTopIn(barBack, panel, 150, top + 17)
    c.progress = Bitmap(panel)
    c.progress:SetSolidColor('ff80c0ff')
    LayoutHelpers.AtLeftTopIn(c.progress, barBack, 0, 0)
    c.progress.Height:Set(barBack.Height)
    c.progress.Width:Set(0)
    c.progress.Depth:Set(function() return barBack.Depth() + 1 end)
    c.progress:Hide()
    barBack:Hide()
    c.progressBack = barBack
    top = top + 48
    c.note = UIUtil.CreateText(panel, state.note or "", 14, UIUtil.bodyFont)
    LayoutHelpers.AtLeftTopIn(c.note, panel, 0, top)
    top = top + rowHeight - 6

    -- the keys
    local help = {
        "W A S D or the arrow keys: move    R F or the wheel: up, down",
        "Q E: roll    Shift: faster    Right drag: turn    Left click: focus",
        "H: hide this panel    Esc: leave the photo mode",
    }
    for i, line in help do
        local text = UIUtil.CreateText(panel, line, 12, UIUtil.bodyFont)
        LayoutHelpers.AtLeftTopIn(text, panel, 0, top)
        top = top + 18
    end
    LayoutHelpers.SetHeight(panel, top)

    -- each frame: the photo's progress, the line about it, the focus a click gave
    panel:SetNeedsFrameUpdate(true)
    panel.OnFrame = function(self, delta)
        local now = moho64ui_PhotoMode()
        local busy = now.taking or now.saving
        if busy then
            c.progressBack:Show()
            c.progress:Show()
            c.progress.Width:Set(math.floor(c.progressBack.Width() * now.progress))
            c.take:Disable()
        else
            c.progressBack:Hide()
            c.progress:Hide()
            c.take:Enable()
        end
        if now.note ~= c.noteShown then
            c.noteShown = now.note
            c.note:SetText(now.note)
        end
        c.focusText:SetText(FocusText(now.lens.focus))
    end
    State.shown = true
end

function Open()
    State.open = true
    State.hidGameUI = false
    local GameMain = import("/lua/ui/game/gamemain.lua")
    if not GameMain.gameUIHidden then  -- (put away already, by the player or the game: left as it is)
        GameMain.HideGameUI('on')
        State.hidGameUI = true
    end
    -- While it is open the game's panels stay away: the game brings them back by itself after its opening (its
    -- loading screen's end, StopLoadingDialog's InitialAnimations: HideGameUI('off') some four seconds in) and around
    -- a cinematic (NISMode); then they go again, and Close brings them back.
    if watcher then KillThread(watcher) end
    watcher = ForkThread(function()
        while State.open do
            if not GameMain.gameUIHidden then
                GameMain.HideGameUI('on')
                State.hidGameUI = true
            end
            WaitFrames(1)
        end
    end)
    if panel then panel:Destroy() end
    MakePanel()
end

function Close()
    State.open = false
    State.shown = false
    if watcher then KillThread(watcher) end
    watcher = false
    if panel then panel:Destroy() end
    panel = false
    Controls = {}
    if State.hidGameUI then
        local GameMain = import("/lua/ui/game/gamemain.lua")
        if GameMain.gameUIHidden then GameMain.HideGameUI('off') end
        State.hidGameUI = false
    end
end

function TogglePanel()
    if not panel then return end
    if panel:IsHidden() then panel:Show() else panel:Hide() end
    State.shown = not panel:IsHidden()
    -- (the progress bar shows only while a photo is made)
    if State.shown and not (moho64ui_PhotoMode().taking or moho64ui_PhotoMode().saving) then
        Controls.progressBack:Hide()
        Controls.progress:Hide()
    end
end
