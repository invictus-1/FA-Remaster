-- FA Remastered: the replay timeline (Chris's approval of 2026-10-23: "this is severely lacking in the original and
-- could help people who cast gameplay"). This program's own, not a hook: the game has none. While a replay plays, a
-- bar of the game's own look (the in-game panels' frame, /game/panel/; the observer panel's slider thumb and its
-- cyan time; the game's tooltips) shows the replay's length, where it is, the game's moments as markers (hover: what
-- it was and when) and a thin strip of how much fighting there was in every 5 s. A click on the bar or a drag of its
-- thumb jumps there; a click on a marker jumps to 5 s before it and selects its unit if it is one there then. The
-- bar's X button hides it (so only the game shows), a small handle where it was (shown when the mouse comes near) or
-- the key "Replay timeline" (Ctrl-T at first: remaster_timeline) brings it back; dragged by its frame it docks at the bottom or the top of the screen or
-- stays where it is dropped. Docked at the bottom it sits above the game's bottom panels (the construction panel, the
-- unit view), never over them, and moves up and down as they come, grow and go with a selection (Chris, 2026-10-23).
-- Shown or not and its place are kept in the preferences (moho64ui_replay_timeline).
-- It also goes away while the game's panels are put away (the game's own HideGameUI: the photo mode, the player's
-- key) and comes back with them.
--
-- The host keeps the replay (ui/app/game_session.cpp; its functions ui/user/replay_timeline.cpp): the length (the
-- replay stream's beats, then the tick it ends at), the moments (the engine's ScanReplayMoments, sim/moments.h, on a
-- thread of the host's: it plays the replay a second time, some 7 to 40 times the game's speed, so the markers come
-- in over a minute or minutes, the scanned part of the track lighter) and the jumps (forward: the beats run ahead;
-- back: the replay starts over, this interface is made anew, and runs ahead). A jump back keeps the camera and the army watched through the restart
-- (moho64ui_ReplaySeek's `keep`, moho64ui_ReplayTakeKept). gamemain's OnFirstUpdate (ui/data/hook) calls Create.

local UIUtil = import("/lua/ui/uiutil.lua")
local LayoutHelpers = import("/lua/maui/layouthelpers.lua")
local Group = import("/lua/maui/group.lua").Group
local Bitmap = import("/lua/maui/bitmap.lua").Bitmap
local Button = import("/lua/maui/button.lua").Button
local Dragger = import("/lua/maui/dragger.lua").Dragger
local NinePatch = import("/lua/ui/controls/ninepatch.lua").NinePatch
local Tooltip = import("/lua/ui/game/tooltip.lua")

-- How things stand, for the tests: made, shown (the player's choice), visible (on the screen now), place ('bottom',
-- 'top' or 'free'), hover (the text of the marker under the mouse; false), status (the bar's line: "Scanning 45%",
-- "Seeking 12:40 ..."), markers and columns (how many), restored (a jump back's camera was put back), panelsTop (the
-- top of the game's bottom panels the bar docks above).
State = { created = false, shown = true, visible = false, place = 'bottom', hover = false, status = "", markers = 0,
    restored = false }
-- The controls by name (bar, track, played, scanned, thumb, time, length, status, close, handle, markers = {...}), for
-- the tests.
Controls = {}

local prefKey = 'moho64ui_replay_timeline'
local S = LayoutHelpers.ScaleNumber
-- The bar's measures (pixels at the interface's scale 1), and what it keeps while it is there. Its rows: the markers
-- at the top, the track under them, the fighting strip at the bottom.
local K = {
    barHeight = 56,     -- (inside the frame)
    leftWidth = 118,    -- the times and the status line
    rightWidth = 34,    -- the X button
    markerSize = 20,
    trackTop = 27, trackHeight = 6,
    stripTop = 37, stripHeight = 17,
    frameOut = 21,      -- (how far the frame's pieces reach beyond the bar, to their outer line)
    leadTicks = 50,     -- a click on a marker: 5 s before it
    cyan = 'ff00dbff',  -- (the score panel's time)
}
local my = {
    bar = false, ticker = false, handle = false,
    info = { ticks = 0, tick = 0 },
    moments = {}, fight = {}, columns = {}, columnValue = {}, columnMax = 0,
    scrubTick = false,     -- (the thumb dragged: the beat it would jump to)
    kept = false,          -- (a jump back's camera and army, put back when the jump is over)
    wasSeeking = false,
    placeX = 0.5, placeY = 0.8,  -- (a free place: the bar's middle, as shares of the screen)
    laidOutWidth = false, laidOutTicks = false,
    selectAfter = false,  -- (a marker's unit, selected when its jump forward is over)
    floor = false,  -- (the top of the game's bottom panels, PanelsTop)
}
local kinds = {
    CommanderDied = { icon = '/textures/ui/icons_strategic/commander_generic.dds', frame = 'ffd23c2c' },
    ExperimentalFinished = { icon = '/textures/ui/icons_strategic/experimental_generic.dds', frame = 'ff4fb04a', unitIcon = true },
    ExperimentalDied = { icon = '/textures/ui/icons_strategic/experimental_generic.dds', frame = 'ffd23c2c', unitIcon = true },
    ArmyDefeated = { icon = '/game/unit-over/icon-skull_bmp.dds', frame = 'ff8c8c8c', skin = true },
    NukeLaunched = { icon = '/game/orders/launch-nuke_btn_up.dds', frame = 'ffe0a020', skin = true },
    NukeLanded = { icon = '/game/orders/nuke_btn_up.dds', frame = 'ffd23c2c', skin = true },
    NukeIntercepted = { icon = '/textures/ui/icons_strategic/structure_antimissile.dds', frame = 'ff3c8cd2' },
    -- (FAF puts the armies still in out of the game too when it ends: the observer's icon, as the score panel's)
    GameEnded = { icon = '/widgets/faction-icons-alpha_bmp/observer_ico.dds', frame = 'ffd8c060', skin = true },
}

MarkerLooks = kinds  -- (for the tests)

local function TimeText(tick)
    local seconds = math.floor((tick or 0) / 10)
    local h, m, s = math.floor(seconds / 3600), math.floor(math.mod(seconds, 3600) / 60), math.mod(seconds, 60)
    if h > 0 then return string.format("%d:%02d:%02d", h, m, s) end
    return string.format("%d:%02d", m, s)
end

local function ArmyName(army)
    local armies = GetArmiesTable().armiesTable
    local a = army and army >= 1 and armies and armies[army]
    if not a then return army and army >= 1 and ("army " .. army) or "no army" end
    return a.nickname or a.name
end
local function ArmyColor(army)
    local armies = GetArmiesTable().armiesTable
    local a = armies and armies[army]
    return a and a.color or 'ffffffff'
end
local function UnitName(id)
    local bp = id and id ~= "" and __blueprints[id]
    if not bp then return id or "a unit" end
    local name = bp.General and bp.General.UnitName
    if name and name ~= "" then return LOC(name) end
    return LOC(bp.Description or id)
end

-- What a moment was ("Commander of <army> destroyed 14:32").
function MomentText(m)
    local who = ArmyName(m.army)
    local what
    if m.kind == 'CommanderDied' then what = "Commander of " .. who .. " destroyed"
    elseif m.kind == 'ExperimentalFinished' then what = UnitName(m.unit) .. " finished by " .. who
    elseif m.kind == 'ExperimentalDied' then what = UnitName(m.unit) .. " of " .. who .. " destroyed"
    elseif m.kind == 'ArmyDefeated' then what = who .. " defeated"
    elseif m.kind == 'NukeLaunched' then what = "Nuke launched by " .. who
    elseif m.kind == 'NukeLanded' then what = "Nuke of " .. who .. " landed"
    elseif m.kind == 'NukeIntercepted' then what = "Nuke of " .. who .. " intercepted"
    elseif m.kind == 'GameEnded' then what = "Game over, " .. who .. " still in"
    else what = tostring(m.kind) .. " (" .. who .. ")" end
    return what .. " " .. TimeText(m.tick)
end

local function SavePlace()
    SetPreference(prefKey, { shown = State.shown, place = State.place, x = my.placeX, y = my.placeY })
end

-- The track: where a beat lies along it, and the beat at a place of the screen.
local function TrackLeft() return my.bar.Left() + S(K.leftWidth) end
local function TrackWidth() return math.max(1, my.bar.Width() - S(K.leftWidth) - S(K.rightWidth)) end
function XOfTick(tick)
    if not my.bar or (my.info.ticks or 0) <= 0 then return 0 end
    return TrackLeft() + TrackWidth() * math.min(1, math.max(0, tick / my.info.ticks))
end
function TickAtX(x)
    if not my.bar or (my.info.ticks or 0) <= 0 then return 0 end
    return math.floor(math.min(1, math.max(0, (x - TrackLeft()) / TrackWidth())) * my.info.ticks + 0.5)
end

-- The top of the game's bottom panels where the bar would lie (the construction panel, the unit view; they come and
-- grow with a selection, also in a replay): the highest visible one (not hidden, nor faded out to nothing, itself or a
-- parent) lying in the lower part of the screen that reaches into the bar's width. The screen's bottom without one.
local panelTables = { "/lua/ui/game/construction.lua", "/lua/ui/game/unitview.lua", "/lua/ui/game/orders.lua" }
local function Shows(c)
    local root = GetFrame(0)
    while c and c ~= root do
        if c:IsHidden() or (c.GetAlpha and c:GetAlpha() <= 0) then return false end
        c = c:GetParent()
    end
    return true
end
local function PanelsTop()
    local root = GetFrame(0)
    local top = root.Bottom()
    local width = math.min(S(1500), root.Width() - 2 * S(K.frameOut + 40))
    local left = root.Left() + (root.Width() - width) / 2 - S(K.frameOut)
    local right = left + width + 2 * S(K.frameOut)
    local lower = root.Top() + root.Height() * 0.4  -- (a panel lies wholly below it; the screen-wide groups do not)
    for _, file in panelTables do
        local ok, module = pcall(import, file)
        local controls = ok and module and module.controls
        if type(controls) == 'table' then
            for _, c in controls do
                if type(c) == 'table' and c.IsHidden and c.GetParent and c ~= root and c.Top and type(c.Top) ~= 'number' then
                    local fine, t, b, l, r = pcall(function() return c.Top(), c.Bottom(), c.Left(), c.Right() end)
                    if fine and t > lower and t < top and r > left and l < right and b > t and Shows(c) then top = t end
                end
            end
        end
    end
    return math.floor(top)
end
-- The bottom of the bar docked at the bottom: above those panels, its frame clear of them.
local function DockedBottom()
    local root = GetFrame(0)
    return math.min(root.Bottom() - S(K.frameOut + 8), (my.floor or root.Bottom()) - S(K.frameOut + 4))
end

-- Where the bar stands: docked at the bottom (above the game's bottom panels) or the top of the screen (in the
-- middle), or where it was dropped.
local function Place()
    local root = GetFrame(0)
    local width = math.min(S(1500), root.Width() - 2 * S(K.frameOut + 40))
    my.bar.Width:Set(width)
    my.bar.Height:Set(S(K.barHeight))
    if State.place == 'top' then
        my.bar.Left:Set(function() return math.floor(root.Left() + (root.Width() - my.bar.Width()) / 2) end)
        my.bar.Top:Set(function() return root.Top() + S(K.frameOut + 64) end)  -- (below the game's tabs at the top)
    elseif State.place == 'free' then
        my.bar.Left:Set(function()
            local x = root.Left() + root.Width() * my.placeX - my.bar.Width() / 2
            return math.floor(math.max(root.Left() + S(K.frameOut), math.min(root.Right() - S(K.frameOut) - my.bar.Width(), x)))
        end)
        my.bar.Top:Set(function()
            local y = root.Top() + root.Height() * my.placeY - my.bar.Height() / 2
            return math.floor(math.max(root.Top() + S(K.frameOut), math.min(root.Bottom() - S(K.frameOut) - my.bar.Height(), y)))
        end)
    else
        my.bar.Left:Set(function() return math.floor(root.Left() + (root.Width() - my.bar.Width()) / 2) end)
        my.bar.Top:Set(function() return DockedBottom() - my.bar.Height() end)
    end
    my.bar.Right:Set(function() return my.bar.Left() + my.bar.Width() end)
    my.bar.Bottom:Set(function() return my.bar.Top() + my.bar.Height() end)
end

-- Seek: jump to a beat (a jump back keeps the camera and the army watched). `select` (an entity id, a marker's unit):
-- selected when the jump is over, if it is a unit there then.
function Seek(tick, select)
    local now = moho64ui_ReplayTimeline()
    if not now then return false, "no replay plays" end
    tick = math.max(1, math.min(math.floor(tick), now.ticks))
    local keep = nil
    if tick < now.tick then
        local cam = GetCamera('WorldCamera')
        keep = { camera = cam and cam:SaveSettings() or nil, focus = GetFocusArmy(), select = select }
        my.selectAfter = false
    else
        my.selectAfter = select or false
    end
    State.lastSeek = tick
    local ok, why = moho64ui_ReplaySeek(tick, keep)
    if ok then my.wasSeeking = true end  -- (a short jump may be over before the next frame looks)
    return ok, why
end

-- A marker's unit selected after its jump (a missile, or a unit not there, is not).
local function SelectAfter(entity)
    if not entity then return end
    local unit = GetUnitById(tostring(entity))
    if unit and not unit:IsDead() then
        SelectUnits({ unit })
        State.selected = entity
    end
end

local function PutBack(k)
    if not k then return end
    local cam = GetCamera('WorldCamera')
    if k.camera and cam then cam:RestoreSettings(k.camera) end
    if k.focus and k.focus ~= GetFocusArmy() then SetFocusArmy(k.focus) end
    State.restored = true
end

local function ShowTip(control, m)
    State.hover = MomentText(m)
    Tooltip.CreateMouseoverDisplay(control, { text = State.hover, body = "Click to watch it from 5 seconds before." }, 0, true, nil, true)
end
local function HideTip()
    State.hover = false
    Tooltip.DestroyMouseoverDisplay()
end

-- The markers sit above the track at their moment's place; one too near the one before it moves to its right, and a
-- thin line from it points at its moment's place.
local function LayoutMarkers()
    local lastX = -10000
    for _, mk in Controls.markers do
        local x = math.floor(XOfTick(mk.moment.tick) - S(K.markerSize) / 2)
        if x < lastX + S(K.markerSize) + S(2) then x = lastX + S(K.markerSize) + S(2) end
        lastX = x
        -- (along the bar: they move with it)
        local dx, at = x - my.bar.Left(), math.floor(XOfTick(mk.moment.tick)) - my.bar.Left()
        mk.Left:Set(function() return my.bar.Left() + dx end)
        mk.Top:Set(function() return my.bar.Top() + S(1) end)
        mk.pointer.Left:Set(function() return my.bar.Left() + at end)
        mk.pointer.Top:Set(function() return my.bar.Top() + S(K.markerSize + 4) end)
    end
end

local function AddMarker(m)
    local look = kinds[m.kind] or kinds.CommanderDied
    local mk = Bitmap(my.bar)
    mk:SetSolidColor(look.frame)
    LayoutHelpers.SetDimensions(mk, K.markerSize, K.markerSize)
    mk.Depth:Set(function() return my.bar.Depth() + 6 end)
    local inner = Bitmap(mk)
    inner:SetSolidColor('ff0c1114')
    LayoutHelpers.SetDimensions(inner, K.markerSize - 4, K.markerSize - 4)
    LayoutHelpers.AtCenterIn(inner, mk)
    inner:DisableHitTest()
    -- (an experimental: the unit's own picture, as the construction panel finds it; its kind's icon without one)
    local texture = look.skin and UIUtil.UIFile(look.icon) or look.icon
    if look.unitIcon and m.unit ~= "" and DiskGetFileInfo(UIUtil.UIFile('/icons/units/' .. m.unit .. '_icon.dds', true)) then
        texture = UIUtil.UIFile('/icons/units/' .. m.unit .. '_icon.dds', true)
    end
    local icon = Bitmap(mk, texture)
    LayoutHelpers.SetDimensions(icon, K.markerSize - 4, K.markerSize - 4)
    LayoutHelpers.AtCenterIn(icon, mk)
    icon.Depth:Set(function() return inner.Depth() + 1 end)
    icon:DisableHitTest()
    -- the army's colour under it, and the line down to its place on the track
    local stripe = Bitmap(mk)
    stripe:SetSolidColor(ArmyColor(m.army))
    stripe.Left:Set(mk.Left)
    stripe.Right:Set(mk.Right)
    stripe.Top:Set(mk.Bottom)
    stripe.Height:Set(S(3))
    stripe:DisableHitTest()
    mk.pointer = Bitmap(my.bar)
    mk.pointer:SetSolidColor(look.frame)
    mk.pointer.Width:Set(1)
    mk.pointer.Height:Set(S(K.trackTop - K.markerSize - 4))
    mk.pointer.Depth:Set(function() return my.bar.Depth() + 5 end)
    mk.pointer:DisableHitTest()
    mk.moment = m
    mk.HandleEvent = function(self, event)
        if event.Type == 'MouseEnter' then
            ShowTip(self, m)
            return true
        elseif event.Type == 'MouseExit' then
            HideTip()
            return true
        elseif event.Type == 'ButtonPress' and event.Modifiers.Left then
            HideTip()
            Seek(m.tick - K.leadTicks, m.entity)
            return true
        end
        return false
    end
    table.insert(Controls.markers, mk)
end

-- The fighting strip: columns of at least 2 pixels, each the most fighting in the 5 s it holds (square root: a quiet
-- stretch still shows).
local function MakeColumns()
    for _, c in my.columns do c:Destroy() end
    my.columns, my.columnValue, my.columnMax = {}, {}, 0
    local buckets = math.max(1, math.ceil((my.info.ticks or 0) / 50))
    local count = math.max(1, math.min(buckets, math.floor(TrackWidth() / S(2))))
    for i = 1, count do
        local c = Bitmap(my.bar)
        c:SetSolidColor('d0c8642c')
        c.Depth:Set(function() return my.bar.Depth() + 3 end)
        c:DisableHitTest()
        my.columns[i] = c
        my.columnValue[i] = 0
    end
    Controls.columnsMade = count
end
local function LayoutColumns()
    local count = table.getn(my.columns)
    if count == 0 then return end
    local buckets = math.max(1, math.ceil((my.info.ticks or 0) / 50))
    -- (the scale: the most fighting, but at most three times what the busiest tenth of the stretches holds, so one
    -- huge blast, a commander's or a nuke's, leaves the rest of the strip readable; higher ones are cut at the top)
    local values = {}
    for i = 1, count do
        if my.columnValue[i] > 0 then table.insert(values, my.columnValue[i]) end
    end
    table.sort(values)
    local scale = my.columnMax
    if table.getn(values) >= 10 then scale = math.min(scale, 3 * values[math.ceil(table.getn(values) * 0.9)]) end
    for i, c in my.columns do
        local x0 = XOfTick((i - 1) * buckets * 50 / count)
        local x1 = XOfTick(i * buckets * 50 / count)
        local dx = math.floor(x0) - my.bar.Left()
        c.Left:Set(function() return my.bar.Left() + dx end)
        c.Width:Set(math.max(1, math.floor(x1) - math.floor(x0) - 1))
        local h = scale > 0 and math.sqrt(math.min(1, my.columnValue[i] / scale)) * S(K.stripHeight) or 0
        h = math.floor(h + 0.5)
        if my.columnValue[i] > 0 and h < 1 then h = 1 end
        c.Height:Set(h)
        c.Top:Set(function() return my.bar.Top() + S(K.stripTop + K.stripHeight) - h end)
        if h > 0 then c:Show() else c:Hide() end
    end
end
local function AddFight(list)
    local count = table.getn(my.columns)
    local buckets = math.max(1, math.ceil((my.info.ticks or 0) / 50))
    for _, b in list do
        table.insert(my.fight, b)
        local index = math.floor(b.tick / 50)
        local i = math.min(count, math.floor(index * count / buckets) + 1)
        if b.damage > my.columnValue[i] then my.columnValue[i] = b.damage end
        if b.damage > my.columnMax then my.columnMax = b.damage end
    end
end

local function Frame()
    if not my.bar then return end
    local now = moho64ui_ReplayTimeline()
    if not now then return end
    my.info = now
    -- docked at the bottom: above the game's bottom panels as they are now
    local floor = PanelsTop()
    if floor ~= my.floor then
        my.floor = floor
        State.panelsTop = floor
        if State.place == 'bottom' then Place() end
    end
    -- the bar on the screen: the player's choice, and the game's panels there (not put away, no photo mode)
    local GameMain = import("/lua/ui/game/gamemain.lua")
    local panelsAway = GameMain.gameUIHidden
    local visible = State.shown and not panelsAway
    if visible ~= State.visible then
        State.visible = visible
        if visible then my.bar:Show() else
            my.bar:Hide()
            HideTip()
        end
    end
    -- the handle: the bar hidden by the player, and the mouse near where it was
    if my.handle then
        local near = false
        if not State.shown and not panelsAway then
            local mouse = GetMouseScreenPos()
            near = mouse and mouse[1] >= my.bar.Left() - S(80) and mouse[1] <= my.bar.Right() + S(80)
                and mouse[2] >= my.bar.Top() - S(80) and mouse[2] <= my.bar.Bottom() + S(80)
        end
        if near or State.forceHandle then my.handle:Show() else my.handle:Hide() end
    end
    -- new moments and fighting
    if now.moments > table.getn(my.moments) then
        for _, m in moho64ui_ReplayMoments(table.getn(my.moments) + 1) or {} do
            table.insert(my.moments, m)
            AddMarker(m)
        end
        State.markers = table.getn(Controls.markers)
        LayoutMarkers()
        if not State.visible then my.bar:Hide() end
    end
    local width = my.bar.Width()
    if width ~= my.laidOutWidth or now.ticks ~= my.laidOutTicks then  -- (the screen's size changed, or the replay's
        -- length: the scan found it, the stream's beats less its paused ones; everything along the track again)
        my.laidOutWidth = width
        my.laidOutTicks = now.ticks
        MakeColumns()
        AddFight({})
        local all = my.fight
        my.fight = {}
        AddFight(all)
        LayoutMarkers()
        LayoutColumns()
        if not State.visible then my.bar:Hide() end
    end
    if now.buckets > table.getn(my.fight) then
        AddFight(moho64ui_ReplayFight(table.getn(my.fight) + 1) or {})
        LayoutColumns()
        if not State.visible then my.bar:Hide() end
    end
    State.buckets = table.getn(my.fight)
    -- the track: scanned, played, the thumb (where it is, or where it is dragged), the jump's goal
    local c = Controls
    local left = TrackLeft()
    c.scanned.Width:Set(math.max(0, math.floor(XOfTick(now.scanning and now.scanned or now.ticks) - left)))
    c.played.Width:Set(math.max(0, math.floor(XOfTick(now.tick) - left)))
    local shownTick = my.scrubTick or now.tick
    c.thumb.Left:Set(math.floor(XOfTick(shownTick) - c.thumb.Width() / 2))
    if now.seeking then
        c.goal:Show()
        c.goal.Left:Set(math.floor(XOfTick(now.seekTo)))
    else
        c.goal:Hide()
    end
    if not State.visible then c.goal:Hide() end
    c.time:SetText(TimeText(shownTick))
    c.time:SetColor(my.scrubTick and 'ffffd200' or K.cyan)
    c.length:SetText("/ " .. TimeText(now.ticks))
    local status = ""
    if now.seeking then
        status = "Seeking " .. TimeText(now.seekTo) .. " ..."
    elseif now.scanning then
        status = string.format("Scanning %d%%", math.floor(100 * now.scanned / math.max(1, now.ticks)))
    end
    if status ~= State.status then
        State.status = status
        c.status:SetText(status)
    end
    -- a jump back's camera: put back as the new interface comes, and again when the jump is over
    if my.wasSeeking and not now.seeking then
        if my.kept then
            PutBack(my.kept)
            SelectAfter(my.kept.select)
            my.kept = false
        end
        SelectAfter(my.selectAfter)
        my.selectAfter = false
    end
    my.wasSeeking = now.seeking
    State.seeking = now.seeking
end

-- The track's mouse: a press jumps (on release) to where it is let go; the thumb follows meanwhile.
local function TrackEvent(self, event)
    if event.Type == 'ButtonPress' and event.Modifiers.Left then
        my.scrubTick = TickAtX(event.MouseX)
        local dragger = Dragger()
        dragger.OnMove = function(d, x, y) my.scrubTick = TickAtX(x) end
        dragger.OnRelease = function(d, x, y)
            local tick = TickAtX(x)
            my.scrubTick = false
            Seek(tick)
            d:Destroy()
        end
        dragger.OnCancel = function(d)
            my.scrubTick = false
            d:Destroy()
        end
        PostDragger(self:GetRootFrame(), event.KeyCode, dragger)
        return true
    end
    return false
end

-- The frame's mouse: the bar is dragged; let go near the top or the bottom it docks there, elsewhere it stays.
local function MoveEvent(self, event)
    if event.Type == 'ButtonPress' and event.Modifiers.Left then
        local root = GetFrame(0)
        local dx, dy = event.MouseX - my.bar.Left(), event.MouseY - my.bar.Top()
        local dragger = Dragger()
        dragger.OnMove = function(d, x, y)
            State.place = 'free'
            my.placeX = (x - dx + my.bar.Width() / 2 - root.Left()) / root.Width()
            my.placeY = (y - dy + my.bar.Height() / 2 - root.Top()) / root.Height()
            Place()
        end
        dragger.OnRelease = function(d, x, y)
            if State.place == 'free' then
                if my.bar.Top() < root.Top() + S(K.frameOut + 90) then State.place = 'top'
                elseif my.bar.Bottom() > DockedBottom() - S(60) then State.place = 'bottom' end
                Place()
            end
            SavePlace()
            d:Destroy()
        end
        dragger.OnCancel = function(d) d:Destroy() end
        PostDragger(self:GetRootFrame(), event.KeyCode, dragger)
        return true
    end
    return false
end

-- The bar, made while a replay plays (gamemain's OnFirstUpdate); again after a jump back (a new interface).
function Create()
    if my.bar or not SessionIsReplay() or not moho64ui_ReplayTimeline or not moho64ui_ReplayTimeline() then return end
    local saved = GetPreference(prefKey) or {}
    State.shown = saved.shown ~= false
    State.place = (saved.place == 'top' or saved.place == 'free') and saved.place or 'bottom'
    my.placeX, my.placeY = tonumber(saved.x) or 0.5, tonumber(saved.y) or 0.8
    my.info = moho64ui_ReplayTimeline()
    my.moments, my.fight, my.columns, my.columnValue, my.columnMax = {}, {}, {}, {}, 0
    my.laidOutWidth = false
    my.floor = PanelsTop()
    State.panelsTop = my.floor
    local root = GetFrame(0)
    Controls = { markers = {} }
    local c = Controls
    my.bar = Group(root, "replayTimeline")
    c.bar = my.bar
    my.bar.Depth:Set(root:GetTopmostDepth() + 1)
    Place()
    -- the in-game panels' frame round it, and a surface to drag it by
    local frame = NinePatch(my.bar,
        UIUtil.SkinnableFile('/game/panel/panel_brd_m.dds'),
        UIUtil.SkinnableFile('/game/panel/panel_brd_ul.dds'),
        UIUtil.SkinnableFile('/game/panel/panel_brd_ur.dds'),
        UIUtil.SkinnableFile('/game/panel/panel_brd_ll.dds'),
        UIUtil.SkinnableFile('/game/panel/panel_brd_lr.dds'),
        UIUtil.SkinnableFile('/game/panel/panel_brd_vert_l.dds'),
        UIUtil.SkinnableFile('/game/panel/panel_brd_vert_r.dds'),
        UIUtil.SkinnableFile('/game/panel/panel_brd_horz_um.dds'),
        UIUtil.SkinnableFile('/game/panel/panel_brd_lm.dds'))
    frame:Surround(my.bar, 0, 0)
    frame.Depth:Set(function() return my.bar.Depth() end)
    -- (the panel's own middle lets the world through; a darker ground under it keeps the markers and the strip legible
    -- over a bright map, the world's rings and icons)
    local ground = Bitmap(my.bar)
    ground:SetSolidColor('b4080c0e')
    ground.Left:Set(function() return my.bar.Left() - S(12) end)
    ground.Right:Set(function() return my.bar.Right() + S(12) end)
    ground.Top:Set(function() return my.bar.Top() - S(12) end)
    ground.Bottom:Set(function() return my.bar.Bottom() + S(12) end)
    ground.Depth:Set(function() return my.bar.Depth() end)
    ground:DisableHitTest()
    local grab = Bitmap(my.bar)
    grab:SetSolidColor('00000000')
    grab.Left:Set(function() return my.bar.Left() - S(K.frameOut) end)
    grab.Right:Set(function() return my.bar.Right() + S(K.frameOut) end)
    grab.Top:Set(function() return my.bar.Top() - S(K.frameOut) end)
    grab.Bottom:Set(function() return my.bar.Bottom() + S(K.frameOut) end)
    grab.Depth:Set(function() return my.bar.Depth() + 1 end)
    grab.HandleEvent = MoveEvent
    c.grab = grab

    -- the times (where it is, the length) and the status line
    local clock = Bitmap(my.bar, UIUtil.UIFile('/game/unit_view_icons/time.dds'))
    LayoutHelpers.SetDimensions(clock, 16, 16)
    LayoutHelpers.AtLeftTopIn(clock, my.bar, 0, 3)
    clock.Depth:Set(function() return my.bar.Depth() + 2 end)
    clock:DisableHitTest()
    c.time = UIUtil.CreateText(my.bar, "0:00", 16, UIUtil.bodyFont)
    c.time:SetColor(K.cyan)
    LayoutHelpers.AtLeftTopIn(c.time, my.bar, 20, 1)
    c.length = UIUtil.CreateText(my.bar, "/ 0:00", 12, UIUtil.bodyFont)
    c.length:SetColor('ffa0a0a0')
    LayoutHelpers.AtLeftTopIn(c.length, my.bar, 20, 21)
    c.status = UIUtil.CreateText(my.bar, "", 12, UIUtil.bodyFont)
    c.status:SetColor('ffe0c060')
    LayoutHelpers.AtLeftTopIn(c.status, my.bar, 0, 38)
    for _, t in { c.time, c.length, c.status } do
        t.Depth:Set(function() return my.bar.Depth() + 2 end)
        t:DisableHitTest()
    end

    -- the track: the whole replay, the part scanned, the part played; its mouse surface over the strip below it
    local function Strip(color, depth)
        local b = Bitmap(my.bar)
        b:SetSolidColor(color)
        b.Left:Set(function() return TrackLeft() end)
        b.Top:Set(function() return my.bar.Top() + S(K.trackTop) end)
        b.Height:Set(S(K.trackHeight))
        b.Depth:Set(function() return my.bar.Depth() + depth end)
        b:DisableHitTest()
        return b
    end
    c.track = Strip('ff0b1216', 2)
    c.track.Width:Set(function() return TrackWidth() end)
    c.scanned = Strip('ff2b3a40', 3)
    c.played = Strip('ff1a90b4', 4)
    local stripBack = Bitmap(my.bar)  -- (the fighting strip's ground)
    stripBack:SetSolidColor('80060a0c')
    stripBack.Left:Set(function() return TrackLeft() end)
    stripBack.Width:Set(function() return TrackWidth() end)
    stripBack.Top:Set(function() return my.bar.Top() + S(K.stripTop) end)
    stripBack.Height:Set(S(K.stripHeight))
    stripBack.Depth:Set(function() return my.bar.Depth() + 2 end)
    stripBack:DisableHitTest()
    c.goal = Bitmap(my.bar)  -- (a jump's goal)
    c.goal:SetSolidColor('ffffd200')
    c.goal.Width:Set(S(2))
    c.goal.Top:Set(function() return my.bar.Top() + S(K.trackTop - 3) end)
    c.goal.Height:Set(S(K.trackHeight + 6))
    c.goal.Depth:Set(function() return my.bar.Depth() + 5 end)
    c.goal:DisableHitTest()
    c.goal:Hide()
    local surface = Bitmap(my.bar)
    surface:SetSolidColor('00000000')
    surface.Left:Set(function() return TrackLeft() end)
    surface.Width:Set(function() return TrackWidth() end)
    surface.Top:Set(function() return my.bar.Top() + S(K.trackTop - 3) end)
    surface.Bottom:Set(function() return my.bar.Bottom() end)
    surface.Depth:Set(function() return my.bar.Depth() + 4 end)
    surface.HandleEvent = TrackEvent
    c.surface = surface
    -- the thumb (the observer panel's game speed slider's)
    c.thumb = Bitmap(my.bar, UIUtil.SkinnableFile('/slider02/slider_btn_up.dds'))
    LayoutHelpers.SetDimensions(c.thumb, 26, 20)
    c.thumb.Top:Set(function() return my.bar.Top() + S(K.trackTop + K.trackHeight / 2) - math.floor(c.thumb.Height() / 2) end)
    c.thumb.Depth:Set(function() return my.bar.Depth() + 8 end)
    c.thumb.HandleEvent = TrackEvent

    -- the X: hides the bar
    c.close = Button(my.bar,
        UIUtil.SkinnableFile('/game/menu-btns/close_btn_up.dds'),
        UIUtil.SkinnableFile('/game/menu-btns/close_btn_down.dds'),
        UIUtil.SkinnableFile('/game/menu-btns/close_btn_over.dds'),
        UIUtil.SkinnableFile('/game/menu-btns/close_btn_dis.dds'),
        "UI_Tab_Click_01", "UI_Tab_Rollover_01")
    LayoutHelpers.AtRightTopIn(c.close, my.bar, -4, -4)
    c.close.Depth:Set(function() return my.bar.Depth() + 8 end)
    c.close.OnClick = function(self) Hide() end
    Tooltip.AddControlTooltipManual(c.close, "Hide the timeline", "Ctrl-T or the handle where it was brings it back.", 0)

    -- the handle that brings it back (where the bar was; shown when the mouse comes near)
    my.handle = Group(root, "replayTimelineHandle")
    LayoutHelpers.SetDimensions(my.handle, 36, 22)
    my.handle.Left:Set(function() return math.floor(my.bar.Left() + (my.bar.Width() - my.handle.Width()) / 2) end)
    my.handle.Top:Set(function()
        if State.place == 'top' then return my.bar.Top() - S(K.frameOut) + S(4) end
        return my.bar.Bottom() + S(K.frameOut) - my.handle.Height() - S(4)
    end)
    my.handle.Depth:Set(function() return my.bar.Depth() end)
    local handleFrame = NinePatch(my.handle,
        UIUtil.SkinnableFile('/game/panel/panel_brd_m.dds'),
        UIUtil.SkinnableFile('/game/panel/panel_brd_ul.dds'),
        UIUtil.SkinnableFile('/game/panel/panel_brd_ur.dds'),
        UIUtil.SkinnableFile('/game/panel/panel_brd_ll.dds'),
        UIUtil.SkinnableFile('/game/panel/panel_brd_lr.dds'),
        UIUtil.SkinnableFile('/game/panel/panel_brd_vert_l.dds'),
        UIUtil.SkinnableFile('/game/panel/panel_brd_vert_r.dds'),
        UIUtil.SkinnableFile('/game/panel/panel_brd_horz_um.dds'),
        UIUtil.SkinnableFile('/game/panel/panel_brd_lm.dds'))
    LayoutHelpers.FillParentFixedBorder(handleFrame, my.handle, 0)
    local handleIcon = Bitmap(my.handle, UIUtil.UIFile('/game/unit_view_icons/time.dds'))
    LayoutHelpers.SetDimensions(handleIcon, 16, 16)
    LayoutHelpers.AtCenterIn(handleIcon, my.handle)
    handleIcon.Depth:Set(function() return my.handle.Depth() + 2 end)
    local handleHit = Bitmap(my.handle)
    handleHit:SetSolidColor('00000000')
    LayoutHelpers.FillParent(handleHit, my.handle)
    handleHit.Depth:Set(function() return my.handle.Depth() + 3 end)
    handleHit.HandleEvent = function(self, event)
        if event.Type == 'ButtonPress' and event.Modifiers.Left then
            Show()
            return true
        end
        return false
    end
    c.handle = handleHit
    my.handle:Hide()

    -- each frame (on a control that is never hidden: the bar's own frame updates would stop with it)
    my.ticker = Group(root, "replayTimelineTicker")
    LayoutHelpers.SetDimensions(my.ticker, 1, 1)
    LayoutHelpers.AtLeftTopIn(my.ticker, root, 0, 0)
    my.ticker:DisableHitTest()
    my.ticker:SetNeedsFrameUpdate(true)
    my.ticker.OnFrame = function(self, delta) Frame() end

    State.created = true
    State.visible = true
    State.restored = false
    -- a jump back: the camera and the army watched before it, put back now and when the jump is over
    my.kept = moho64ui_ReplayTakeKept() or false
    if my.kept then PutBack(my.kept) end
    my.wasSeeking = false
    Frame()
end

function Show()
    State.shown = true
    SavePlace()
    if my.bar then Frame() end
end
function Hide()
    State.shown = false
    SavePlace()
    if my.bar then Frame() end
end
function Toggle()
    if State.shown then Hide() else Show() end
end
