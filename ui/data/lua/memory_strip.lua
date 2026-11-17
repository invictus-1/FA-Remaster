-- The Options' graphics-memory strip (ours; Chris's wish of 2026-11-14 through the engine: "a small vram calculator
-- spot that tells you how much vram it'll eat and if too much is on it'll recommend to lower a few settings"; his aim:
-- weak PCs as close to max as they carry). The numbers are the renderer's (ui/REQUESTS.md 57, render 106f227f:
-- WorldPicture::EstimateCardMemory, pure arithmetic from its measures of 2026-11-14, within 1.4 % of every late-game run
-- at 8 GB; only the Large game is measured) through the host's moho64ui_CardMemoryEstimate, and the card's own memory
-- and the system's budget for the program through moho64ui_CardMemoryNow (nil where no picture of the world is drawn on
-- the interface's thread). The original game has no such thing.
-- A small panel under the Options dialog, on its Video and Remastered tabs: the estimate for the settings AS CHOSEN IN
-- THE DIALOG (its working set, before Apply) against the card's own memory, a bar, the largest parts, the parts the renderer has
-- not counted yet (never shown as free), and over the budget the settings that give the most back (each asked of the
-- estimate one step lower, the others as chosen). It changes no setting. It follows the dialog every 15 frames.
-- Its place and look are for Chris's eye (handoff: build it, show him a picture, not finished before his word).
local LayoutHelpers = import('/lua/maui/layouthelpers.lua')
local Bitmap = import('/lua/maui/bitmap.lua').Bitmap
local UIUtil = import('/lua/ui/uiutil.lua')

-- For the tests: the panel, the lines shown, the last estimate and the advice.
State = { panel = false, lines = {}, estimate = false, now = false, advice = {}, hidden = true, needed = 0, card = false }

local kWidth = 664    -- (the Options dialog's panel is 698 wide at scale 1: the strip a little narrower)
local kGameSize = "large"  -- (only the Large game is measured: the strip says "a large late game")
local kRows = 10      -- (the text's rows at most: the lines wrapped to the panel's width)

-- The working set's values (the dialog's currentOptionsSet: every option's value as chosen, before Apply) as the
-- renderer's CardMemoryChoice. The window's size is the screen's now (a change of the display mode in the Video tab
-- is not followed until it is applied: INFERRED good enough for a first look).
function Choice(set)
    set = set or {}
    local function num(key, otherwise)
        local v = set[key]
        if type(v) == 'number' then return v end
        if type(v) == 'string' and tonumber(v) then return tonumber(v) end
        return otherwise
    end
    local function on(key, otherwise) return num(key, otherwise and 1 or 0) ~= 0 end
    local fsr3, fg = on('remaster_temporal_upscale', false), on('remaster_frame_generation', false)
    local aa = num('antialiasing', 0)
    local samples = 1
    if not fsr3 and not fg then samples = aa >= 4 and 4 or aa >= 2 and 2 or 1 end  -- (LookSettings::SceneSamples)
    local shadow = num('shadow_quality', 1)
    local rays = on('remaster_ray_tracing', false)
    if rawget(_G, 'moho64ui_RayTracingSupported') and moho64ui_RayTracingSupported() == false then rays = false end
    local frame = GetFrame(0)
    return {
        width = frame and frame.Width() or 1920, height = frame and frame.Height() or 1080,
        upscale = num('remaster_upscale', 1), temporalUpscale = fsr3, frameGeneration = fg, samples = samples,
        textureDetail = num('texture_level', 1), rayTracing = rays,
        shadowSize = shadow <= 0 and 0 or shadow == 1 and 1024 or shadow == 2 and 2048 or 4096,  -- (game_session.cpp's)
        softShadows = on('remaster_soft_shadows', true), atmosphere = on('remaster_atmosphere', true),
        groundDetail = on('remaster_ground_detail', true), weatherEffects = on('remaster_weather_effects', true),
        surround = true,  -- (the land round the map: on by default where a map has one; counted, the largest case)
        gameSize = kGameSize,
    }
end

-- The settings that give memory back, each one step lower than chosen (the option's next value toward less),
-- as the estimate's own differences. The steps: the option's values in its list's order, the next one that costs less.
local kLowerable = { 'texture_level', 'remaster_ray_tracing', 'shadow_quality', 'antialiasing', 'remaster_upscale',
    'remaster_frame_generation', 'remaster_atmosphere', 'remaster_weather_effects', 'remaster_ground_detail',
    'remaster_soft_shadows' }

local function Copy(t)
    local c = {}
    for k, v in t or {} do c[k] = v end
    return c
end

local function ItemOf(key)
    local ok, item = pcall(function() return import('/lua/options/optionslogic.lua').FindOptionItem(key) end)
    return ok and item or nil
end

-- {key, title, valueText, savesMB} for each option that gives back over 50 MB, the most first
-- What a game NEEDS of an estimate: its total without the part "made_ahead" (the renderer's rule, read by its Fable
-- lead 2026-11-17, render world.cpp: what the armies may build is made ahead only while the memory in use plus 1 GB is
-- under 85 % of the budget, so it fills free room and never takes a card over; counted in, a 4 GB card with Medium
-- textures read 5.3 GB and warned, while the game needs 2.9 GB there and fits). The part's id is the renderer's, kept
-- stable for this.
function Needed(e)
    local ahead = 0
    for _, p in e.parts or {} do
        if p.id == 'made_ahead' then ahead = ahead + p.mb end
    end
    return e.total - ahead, ahead
end

-- (`total`: what the settings as chosen need, Needed's)
function Advice(set, total)
    local out = {}
    for _, key in kLowerable do
        local item = ItemOf(key)
        local states = item and item.custom and item.custom.states
        if states and set[key] ~= nil then
            local best = nil
            for _, s in states do
                if s.key ~= set[key] then
                    local trial = Copy(set)
                    trial[key] = s.key
                    local e = moho64ui_CardMemoryEstimate(Choice(trial))
                    local saves = total - Needed(e)
                    if saves > 50 and (not best or saves < best.saves) then  -- (the smallest step that gives back: "one step lower")
                        best = { key = key, title = LOC(item.title or key), value = LOC(s.text or tostring(s.key)), saves = saves }
                    end
                end
            end
            if best then table.insert(out, best) end
        end
    end
    table.sort(out, function(a, b) return a.saves > b.saves end)
    return out
end

local function GB(mb) return string.format("%.1f GB", mb / 1024) end

-- The lines for an estimate and the card's memory now (`now` nil: no card known). The first line holds what the game
-- needs against THE CARD'S OWN memory, a figure that stands still (the Fable lead's check, 2026-11-17, with the
-- renderer: the first version showed the system's budget for the program, which falls and rises as other programs
-- use the card: 3.9 GB alone, 1.8 GB beside a second game, on the same card). Over the card: the warning and the
-- settings that give the most back. Under the card but over what the system gives the program right now: one line
-- that says so (other programs hold the rest), no advice to lower a setting.
function Lines(e, now, advice)
    local lines = {}
    local needed, ahead = Needed(e)
    local card = now and now.total or nil
    local budget = now and now.budget and now.budget > 0 and now.budget < now.total and now.budget or card
    local head = "Graphics memory, estimated for a large late game: about " .. GB(needed)
    if e.margin > 0 then head = head .. string.format(" (+/- %.1f)", e.margin / 1024) end
    if card then head = head .. " of the card's " .. GB(card) end
    local over = card ~= nil and needed > card
    table.insert(lines, { text = head, over = over })
    local parts, unmeasured = {}, {}
    for _, p in e.parts or {} do
        if p.mb > 0 and p.id ~= 'made_ahead' then table.insert(parts, p) end
        if p.mb <= 0 and not p.measured then table.insert(unmeasured, p.name) end
    end
    table.sort(parts, function(a, b) return a.mb > b.mb end)
    local largest = {}
    for i = 1, math.min(4, table.getn(parts)) do table.insert(largest, parts[i].name .. " " .. GB(parts[i].mb)) end
    if table.getn(largest) > 0 then table.insert(lines, { text = "Largest: " .. table.concat(largest, ", ") }) end
    if ahead > 0 then
        table.insert(lines, { text = "Plus up to " .. GB(ahead) .. " made ahead while there is room (never over the card).", dim = true })
    end
    if table.getn(unmeasured) > 0 then
        table.insert(lines, { text = "Not counted yet (not free): " .. table.concat(unmeasured, ", "), dim = true })
    end
    if over then
        local tips = {}
        for i = 1, math.min(3, table.getn(advice or {})) do
            local a = advice[i]
            table.insert(tips, a.title .. " " .. a.value .. " (" .. GB(a.saves) .. ")")
        end
        local text = "Over the card's memory by " .. GB(needed - card) .. "."
        if table.getn(tips) > 0 then text = text .. " Gives the most back: " .. table.concat(tips, ", ") end
        table.insert(lines, { text = text, over = true })
    elseif budget and needed > budget then
        table.insert(lines, { text = "It fits the card, but Windows gives the game only " .. GB(budget) .. " of it right now: other programs hold the rest. Close them before a large game.", over = true })
    end
    return lines, card, needed
end

-- The panel under `dialog`; `tabNow()` gives the tab button shown, `setNow()` the dialog's working set. Shown on the
-- Video and Remastered tabs only.
function Attach(dialog, tabNow, setNow)
    if not rawget(_G, 'moho64ui_CardMemoryEstimate') or not dialog or IsDestroyed(dialog) then return false end
    local O = import('/lua/options/options.lua')
    local panel = Bitmap(dialog)
    panel:SetSolidColor('E0101418')
    panel.Depth:Set(function() return dialog.Depth() + 50 end)
    LayoutHelpers.SetWidth(panel, kWidth)
    -- centred under the dialog (Chris, 2026-11-14: "center it with the menu so it flows with it"). Only the top is
    -- set from the dialog's bottom: the layout helpers' Below also pins the left edge, which undid the centring.
    panel.Top:Set(function() return dialog.Bottom() + LayoutHelpers.ScaleNumber(4) end)
    LayoutHelpers.AtHorizontalCenterIn(panel, dialog)
    panel:DisableHitTest()
    local texts = {}
    for i = 1, kRows do
        local t = UIUtil.CreateText(panel, "", 13, UIUtil.bodyFont)
        t:DisableHitTest()
        if i == 1 then LayoutHelpers.AtLeftTopIn(t, panel, 10, 6) else LayoutHelpers.Below(t, texts[i - 1], 2) end
        t.Right:Set(function() return panel.Right() - LayoutHelpers.ScaleNumber(10) end)
        t:SetClipToWidth(true)
        texts[i] = t
    end
    -- the bar under the first line: its trough, the estimate's share of the budget, a mark at the budget's end
    local trough = Bitmap(panel)
    trough:SetSolidColor('FF303840')
    trough:DisableHitTest()
    LayoutHelpers.SetHeight(trough, 6)
    local fill = Bitmap(trough)
    fill:DisableHitTest()
    LayoutHelpers.SetHeight(fill, 6)
    State = { panel = panel, lines = {}, estimate = false, now = false, advice = {}, hidden = false, needed = 0, card = false }
    local mine = State
    local shown = ""
    local function Lay(lines, share, over)
        -- each line wrapped to the panel's width (a long one goes on over two or three rows, never cut off)
        local rows = {}
        local width = panel.Width() - LayoutHelpers.ScaleNumber(20)
        for _, l in lines do
            local wrapped = import('/lua/maui/text.lua').WrapText(l.text, width, function(t) return texts[1]:GetStringAdvance(t) end)
            for _, w in wrapped do
                if table.getn(rows) < kRows then table.insert(rows, { text = w, over = l.over, dim = l.dim }) end
            end
        end
        local n = table.getn(rows)
        for i = 1, kRows do
            local l = rows[i]
            texts[i]:SetText(l and l.text or "")
            texts[i]:SetColor(l and (l.over and 'FFE8A048' or l.dim and 'FF8C949C' or 'FFD0D8E0') or 'FFD0D8E0')
        end
        -- (the bar sits under the first line; the other lines under the bar)
        LayoutHelpers.Below(trough, texts[1], 3)
        trough.Left:Set(function() return panel.Left() + LayoutHelpers.ScaleNumber(10) end)
        trough.Right:Set(function() return panel.Right() - LayoutHelpers.ScaleNumber(10) end)
        if texts[2] then LayoutHelpers.Below(texts[2], trough, 3) end
        fill.Left:Set(trough.Left)
        fill.Top:Set(trough.Top)
        local w = math.max(0, math.min(1, share or 0))
        fill.Width:Set(function() return (trough.Right() - trough.Left()) * w end)
        fill:SetSolidColor(over and 'FFC86428' or 'FF4C9CD8')
        local last = texts[math.max(1, n)]
        panel.Height:Set(function() return last.Bottom() - panel.Top() + LayoutHelpers.ScaleNumber(6) end)
    end
    ForkThread(function()
        while State == mine and not IsDestroyed(panel) and not IsDestroyed(dialog) do
            local tab = tabNow and tabNow()
            local onTab = tab and (tab.tabData == O.options.video or tab.tabData == O.options.remaster)
            if not onTab then
                if not panel:IsHidden() then panel:Hide() end
                mine.hidden = true
            else
                if panel:IsHidden() then panel:Show() end
                mine.hidden = false
                local set = (setNow and setNow()) or {}
                local e = moho64ui_CardMemoryEstimate(Choice(set))
                local now = moho64ui_CardMemoryNow()
                local needed = Needed(e)
                local card = now and now.total or nil
                local advice = (card and needed > card) and Advice(set, needed) or {}
                local lines = Lines(e, now, advice)
                local key = ""
                for _, l in lines do key = key .. l.text .. "|" end
                if key ~= shown then
                    shown = key
                    mine.estimate, mine.now, mine.advice, mine.lines, mine.needed, mine.card = e, now, advice, {}, needed, card
                    for _, l in lines do table.insert(mine.lines, l.text) end
                    Lay(lines, card and needed / card or 0, card ~= nil and needed > card)
                end
            end
            WaitFrames(15)  -- (frames, not seconds: in a game the interface's seconds are the game's, which stand while it is paused)
        end
    end)
    return true
end
