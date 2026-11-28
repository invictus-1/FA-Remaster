-- FA Remastered: the first start's recommendation (ours, not the game's; Chris, 2026-11-27, the ideas' number 1:
-- "make sure it starts on ultra for everyone but recommends lower settings if their pc is too weak"). Every
-- remastered look starts at its highest, on every PC. Once per profile, on the main menu, the renderer is asked what
-- it suggests for this graphics card (the host's moho64ui_SuggestedPreset: the Remastered Preset, Texture Detail and
-- Shadow Fidelity, by the card's memory and ray tracing); where that is LOWER than what is set, a box says so and
-- offers it: "Use recommended" puts the three in as the Options dialog's OK would, "Keep my settings" changes nothing.
-- Nothing is ever changed without that click, and nothing is ever raised. Either way the profile's options then hold
-- remaster_first_start_advice = 1 and the box never comes again for that profile (the Options' graphics-memory strip
-- keeps showing the recommendation: /moho64ui/lua/memory_strip.lua).
-- The measured check in a player's first game stays as it is (the hook of /lua/ui/game/gamemain.lua: the card's time
-- per frame, a question, Ray Tracing first): this one is by memory, before a game is loaded; that one by speed.
-- Called by the main menu's hook (ui/data/hook/lua/ui/menus/main.lua) while the menu is up.
local UIUtil = import("/lua/ui/uiutil.lua")

-- How things stand, for the tests: state 'off' (not tried yet), 'waiting' (no graphics device to ask yet), 'none'
-- (nothing lower to recommend: no box), 'before' (the profile was asked before), 'asking', 'used', 'kept'; text (the
-- box's lines joined), want (the three values offered).
State = { state = 'off', text = "", want = false }
Controls = { box = false }

local kKey = 'remaster_first_start_advice'
local kKeys = { 'remaster_preset', 'texture_level', 'shadow_quality' }

function Asked()
    return (import("/lua/user/prefs.lua").GetFromCurrentProfile('options') or {})[kKey] ~= nil
end

local function MarkAsked()
    local Prefs = import("/lua/user/prefs.lua")
    local options = Prefs.GetFromCurrentProfile('options') or {}
    options[kKey] = 1
    Prefs.SetToCurrentProfile('options', options)
    SavePreferences()
end

-- What to offer for the renderer's suggestion `s` ({preset, texture_level, shadow_quality}) against the options as
-- they are set (`current`); nil when nothing of it is lower, when there is no suggestion, or when the Remastered
-- Preset is Custom (5: the player has set the looks one by one). Each value offered is the lower of the suggestion's
-- and the one set (Texture Detail counts down: 0 High, 2 Low), so nothing is raised.
function Offer(s, current)
    if not s or not s.preset or s.preset < 0 or not current then return nil end
    local preset, texture, shadow = current.remaster_preset or 4, current.texture_level or 0, current.shadow_quality or 3
    if preset == 5 then return nil end
    local want = {
        remaster_preset = math.min(s.preset, preset),
        texture_level = math.max(s.texture_level or 0, texture),
        shadow_quality = math.min(s.shadow_quality or 3, shadow),
    }
    if want.remaster_preset == preset and want.texture_level == texture and want.shadow_quality == shadow then return nil end
    return want
end

-- An option's value in the Options dialog's own words ("Remastered Preset High").
local function Named(key, value)
    local ok, item = pcall(function() return import('/lua/options/optionslogic.lua').FindOptionItem(key) end)
    item = ok and item or nil
    local text = tostring(value)
    for _, st in (item and item.custom and item.custom.states) or {} do
        if st.key == value then text = LOC(st.text or text) end
    end
    return LOC((item and item.title) or key) .. " " .. text
end

-- The box's lines for an offer: what the card is (its memory in GB when `cardMB` is known, ray tracing or not when
-- `rayTracing` is not nil), what is recommended (only what would change), and where to change it later.
function Lines(want, current, cardMB, rayTracing)
    local card = {}
    if cardMB and cardMB > 0 then table.insert(card, string.format("has %.0f GB of memory", cardMB / 1024)) end
    if rayTracing == false then table.insert(card, "does not do ray tracing") end
    local changes = {}
    for _, key in kKeys do
        if want[key] ~= current[key] then table.insert(changes, Named(key, want[key])) end
    end
    local lines = { "FA Remastered starts with every look at its highest." }
    if table.getn(card) > 0 then
        table.insert(lines, "Your graphics card " .. table.concat(card, " and ") .. ": a large game at these settings may need more.")
    else
        table.insert(lines, "A large game at these settings may need more than your graphics card has.")
    end
    table.insert(lines, "Recommended for this PC: " .. table.concat(changes, ", ") .. ".")
    table.insert(lines, "You can change this at any time in Options.")
    return lines
end

-- The three put in as the Options dialog's OK puts a choice in (optionslogic SetCurrent: each option's own `set`,
-- then the profile's options saved); a new Remastered Preset with the looks its level sets, as its row would.
function Use(want)
    local logic = import("/lua/options/optionslogic.lua")
    local current = logic.GetCurrent()
    if want.remaster_preset ~= current.remaster_preset then
        for key, value in import("/lua/options/options.lua").RemasterPresetValues(want.remaster_preset) or {} do current[key] = value end
        current.remaster_preset = want.remaster_preset
    end
    current.texture_level, current.shadow_quality = want.texture_level, want.shadow_quality
    logic.SetCurrent(current)
end

-- The box for an offer. Enter or "Use recommended" puts it in; Escape, "Keep my settings" or a click beside it
-- changes nothing. Of the game's own parts (its Popup, its text, its buttons), as the report's box.
function Show(want, lines)
    if Controls.box and not IsDestroyed(Controls.box) then return Controls.box end
    local Popup = import("/lua/ui/controls/popups/popup.lua").Popup
    local Group = import("/lua/maui/group.lua").Group
    local LayoutHelpers = import("/lua/maui/layouthelpers.lua")
    local WrapText = import("/lua/maui/text.lua").WrapText
    local parent = GetFrame(0)
    local content = Group(parent)
    local probe = UIUtil.CreateText(content, "", 14, UIUtil.bodyFont)
    probe:Hide()
    local width = 600
    local rows = { { text = "Recommended settings for this PC", size = 18, colour = UIUtil.fontColor }, { text = "", size = 6 } }
    for i, line in lines do
        local colour = i == 3 and 'FFFFFFFF' or 'FFA0A8B0'  -- (the recommendation itself in white)
        for _, part in WrapText(line, LayoutHelpers.ScaleNumber(width), function(t) return probe:GetStringAdvance(t) end) do
            table.insert(rows, { text = part, size = 14, colour = colour })
        end
        table.insert(rows, { text = "", size = 2 })
    end
    local height = 14
    for _, row in rows do height = height + row.size + 6 end
    LayoutHelpers.SetDimensions(content, width + 40, height + 84)
    local texts, top = {}, 14
    for _, row in rows do
        local t = UIUtil.CreateText(content, row.text, row.size, UIUtil.bodyFont)
        if row.colour then t:SetColor(row.colour) end
        LayoutHelpers.AtTopIn(t, content, top)
        LayoutHelpers.AtHorizontalCenterIn(t, content)
        top = top + row.size + 6
        table.insert(texts, t)
    end
    local use = UIUtil.CreateButtonWithDropshadow(content, '/BUTTON/large/', "Use recommended")
    local keep = UIUtil.CreateButtonWithDropshadow(content, '/BUTTON/large/', "Keep my settings")
    LayoutHelpers.AtBottomIn(use, content, 10)
    LayoutHelpers.AtBottomIn(keep, content, 10)
    LayoutHelpers.AtHorizontalCenterIn(use, content, -150)
    LayoutHelpers.AtHorizontalCenterIn(keep, content, 150)
    local box = Popup(parent, content)
    box.texts, box.content, box.use, box.keep = texts, content, use, keep
    local answered, escaped = false, false
    local function Answer(used)
        if answered then return end
        answered = true
        if used then Use(want) end
        MarkAsked()
        State.state = used and 'used' or 'kept'
        LOG("FA Remastered: the first start's recommendation: " .. (used and "used" or "kept as it was"))
        if not IsDestroyed(box) then box:Close() end
    end
    -- (the game's popup takes its escape handler off the stack twice when Escape closes it by that handler: once here)
    box.OnEscapePressed = function(self)
        escaped = true
        Answer(false)
    end
    -- (and on the main menu Escape goes past that handler to the menu's own: named here while the box is up,
    -- ui/data/hook/lua/ui/uimain.lua)
    local uimain = import("/lua/ui/uimain.lua")
    local function OnEscape() Answer(false) end
    uimain.RemasterEscapePush(OnEscape)
    box.OnClosed = function(self)
        uimain.RemasterEscapeDrop(OnEscape)
        if not escaped then import("/lua/ui/dialogs/eschandler.lua").PopEscapeHandler() end
        Controls.box = false
        Answer(false)  -- (a click beside it: kept)
    end
    use.OnClick = function() Answer(true) end
    keep.OnClick = function() Answer(false) end
    UIUtil.MakeInputModal(box, function() Answer(true) end, function() Answer(false) end)
    Controls.box = box
    return box
end

-- One try, by the main menu's hook each time it looks (every two seconds while the menu is up): nothing once the
-- profile was asked or a box is up; nothing yet while no graphics device answers (a menu without its live
-- background has none: the measured check in the first game is then the only one); else the box, or the note that
-- nothing is to recommend. Returns the state.
function Try()
    if Controls.box and not IsDestroyed(Controls.box) then return State.state end
    if State.state == 'used' or State.state == 'kept' or State.state == 'none' then return State.state end
    if Asked() then
        State.state = 'before'
        return State.state
    end
    local s = rawget(_G, 'moho64ui_SuggestedPreset') and moho64ui_SuggestedPreset()
    if not s then
        State.state = 'waiting'
        return State.state
    end
    local logic = import("/lua/options/optionslogic.lua")
    local current = logic.GetCurrent()
    local want = Offer(s, current)
    if not want then
        MarkAsked()
        State.state = 'none'
        LOG("FA Remastered: the first start's recommendation: nothing lower to recommend for this card (" .. tostring(s.reason) .. ")")
        return State.state
    end
    local now = rawget(_G, 'moho64ui_CardMemoryNow') and moho64ui_CardMemoryNow()
    local rays = rawget(_G, 'moho64ui_RayTracingSupported') and moho64ui_RayTracingSupported()
    local lines = Lines(want, current, now and now.total, rays)
    State.want, State.text, State.state = want, table.concat(lines, " "), 'asking'
    LOG("FA Remastered: the first start's recommendation: asking (" .. tostring(s.reason) .. "): " .. lines[3])
    Show(want, lines)
    return State.state
end
