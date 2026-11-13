-- FA Remastered: the score screen's additions (ours; this file is a hook: the program reads it after the game's
-- /lua/ui/dialogs/hotstats.lua, FAF's graphs and charts of the score screen, which stays as it is). Chris, 2026-11-07
-- (a post-game report with graphs: "lets do all of those"). FAF's screen already shows every army's mass and energy
-- (in, rate, spent, wasted), units alive, built, killed and lost over the game's time, from the history the
-- simulation's /lua/sim/score.lua keeps every 30 seconds; this adds what it lacks:
--   * the game's moments on its graphs (the Graph page's and the Dual page's): a commander killed, an experimental
--     finished or killed, an army defeated, a strategic missile launched, landed or stopped, the game's end; each a
--     marker at its time along the top of the graph with a line down through it, in the replay timeline's looks
--     (/moho64ui/lua/timeline.lua: its icons, its frame colours, the army's colour under it; the mouse over one says
--     what it was and when); and along the bottom a strip of how much fighting there was in every 5 seconds. They are
--     the host's (moho64ui_GameMoments, ui/user/replay_timeline.cpp): a replay's are its timeline's; a game played is
--     scanned from its own replay once it is over (the engine's ScanReplayMoments, on a thread of its own: some 7 to
--     40 times the game's speed), so in a long game they come in while the screen is up ("Finding the game's
--     moments 40%").
--   * "Army Value (Mass)" in the list of graphs: the mass of what the army built less the mass of what it lost (the
--     score's general.built.mass - general.lost.mass, never below 0). INFERRED, ours: the score keeps no value of what
--     stands, so this counts structures too, and not units it was given or captured (a unit given and then lost
--     lowers it).
--   * a fix of FAF's Kill Stats chart: its ACU column shows the commanders BUILT ({"units","cdr","built"} in the
--     chart's table), not those killed; with the additions on it shows the kills, as the Overview's kill column does.
-- All three follow the Interface option "Score Screen Extras" (ours; the options hook), on at first; Off is FAF's
-- screen as it is.
do
    local Prefs = import('/lua/user/prefs.lua')
    local RemasterGame = {
        create_graph = create_graph,
        create_graph_bar = create_graph_bar,
        page_graph = page_graph,
        return_value = return_value,
    }

    -- A local of FAF's module (a hook does not see them): through a function of it that uses it.
    local RemasterGoneSaid = {}
    local function ModuleLocal(fn, name)
        for i = 1, 200 do
            local n, v = debug.getupvalue(fn, i)
            if not n then
                -- (the game's script has no such local any more, a FAF update: said once, so that the feature that
                -- is left out does not go in silence; the lab's hook_contract.py says the same before a push)
                if not RemasterGoneSaid[name] then
                    RemasterGoneSaid[name] = true
                    WARN("FA Remastered: /lua/ui/dialogs/hotstats.lua has no local '" .. tostring(name) .. "' where our hook reads it: a feature of ours is left out")
                end
                return nil
            end
            if n == name then return v end
        end
    end

    function RemasterScoreExtrasOn()
        local options = Prefs.GetFromCurrentProfile('options')
        local v = options and options.remaster_score_extras
        return v == nil or v == 1 or v == true
    end

    -- Army Value (Mass): a graph of the list (page_graph's info_dialog), and its values (return_value, which the
    -- graph asks for every entry of the history and the final score)
    RemasterArmyValueGraph = { name = "Army Value (Mass)", path = { "general", "built", "mass", remaster = 'armyvalue' }, key = 61 }
    function return_value(periode, player, path)
        if path and path.remaster == 'armyvalue' then
            local built = RemasterGame.return_value(periode, player, { "general", "built", "mass" })
            local lost = RemasterGame.return_value(periode, player, { "general", "lost", "mass" })
            return math.max(0, built - lost)
        end
        return RemasterGame.return_value(periode, player, path)
    end
    function page_graph(parent)
        local list = ModuleLocal(RemasterGame.page_graph, 'info_dialog')
        if list then
            local at = nil
            for i, item in list do
                if item == RemasterArmyValueGraph then at = i end
            end
            if RemasterScoreExtrasOn() and not at then
                table.insert(list, RemasterArmyValueGraph)
            elseif at and not RemasterScoreExtrasOn() then
                table.remove(list, at)
            end
        end
        return RemasterGame.page_graph(parent)
    end

    -- The Kill Stats chart's ACU column (create_graph_bar draws from the module's histo table)
    function create_graph_bar(parent, name)
        local histo = ModuleLocal(RemasterGame.create_graph_bar, 'histo')
        local acu = histo and histo.kills_histo and histo.kills_histo[5]
        local data = acu and acu.data and acu.data[1]
        if data and data.path and data.path[1] == "units" and data.path[2] == "cdr" then
            data.path = { "units", "cdr", RemasterScoreExtrasOn() and "kills" or "built" }
            RemasterKillStatsAcu = data.path[3]  -- (for the tests)
        end
        return RemasterGame.create_graph_bar(parent, name)
    end

    -- The moments on a graph -------------------------------------------------------------------------------------
    local UIUtil = import('/lua/ui/uiutil.lua')
    local LayoutHelpers = import('/lua/maui/layouthelpers.lua')
    local Group = import('/lua/maui/group.lua').Group
    local Bitmap = import('/lua/maui/bitmap.lua').Bitmap
    local Tooltip = import('/lua/ui/game/tooltip.lua')
    local S = LayoutHelpers.ScaleNumber
    local K = {
        markerSize = 20,
        stripHeight = 16,     -- the fighting strip along the bottom
        columnWidth = 3,      -- (as the graph's own lines' points)
        fight = 'c8642c',     -- (the replay timeline's strip)
    }
    -- What the last graph shows, for the tests: markers (each {moment, control, line, x}), columns (the strip's),
    -- status (the line while they are looked for, "" when done), done (the look is over), hover (the text of the
    -- marker under the mouse; false).
    RemasterMoments = { markers = {}, columns = {}, status = "", done = false, hover = false }

    local function GraphX(bg, seconds, gameSeconds)
        return bg.Left() + (bg.Right() - bg.Left()) * math.min(1, math.max(0, seconds / gameSeconds))
    end

    -- A click on a moment's marker: the game's own replay opened at that moment (ours; Chris's pick of 2026-11-09,
    -- the host's moho64ui_WatchMoment: the bookmark is put beside the replay's and it runs ahead to ten seconds
    -- before the moment). The score screen is left for it, so it asks first. RemasterMoments.asking: the question,
    -- while it is up (for the tests too).
    local function AskWatch(m, text)
        if not rawget(_G, 'moho64ui_WatchMoment') then return false end
        if RemasterMoments.asking and not IsDestroyed(RemasterMoments.asking) then return true end
        RemasterMoments.asking = UIUtil.QuickDialog(GetFrame(0),
            "Watch this moment in the replay?\n" .. text .. "\nThe score screen is left for it.",
            "<LOC _Yes>", function()
                local ok, why = moho64ui_WatchMoment(m.tick, text)
                if not ok then
                    UIUtil.ShowInfoDialog(GetFrame(0), "The replay cannot be opened: " .. tostring(why) .. ".", "<LOC _Ok>")
                end
            end,
            "<LOC _No>", nil, nil, nil, true, { escapeButton = 2, enterButton = 1, worldCover = false })
        return true
    end

    local function AddMarker(overlay, bg, m, gameSeconds, timeline)
        local look = timeline.MarkerLooks[m.kind] or timeline.MarkerLooks.CommanderDied
        local mk = Bitmap(overlay)
        mk:SetSolidColor(look.frame)
        LayoutHelpers.SetDimensions(mk, K.markerSize, K.markerSize)
        mk.Depth:Set(function() return bg.Depth() + 6 end)
        local inner = Bitmap(mk)
        inner:SetSolidColor('ff0c1114')
        LayoutHelpers.SetDimensions(inner, K.markerSize - 4, K.markerSize - 4)
        LayoutHelpers.AtCenterIn(inner, mk)
        inner.Depth:Set(function() return mk.Depth() + 1 end)
        inner:DisableHitTest()
        -- (an experimental: the unit's own picture, as the timeline finds it)
        local texture = look.skin and UIUtil.UIFile(look.icon) or look.icon
        if look.unitIcon and m.unit ~= "" and DiskGetFileInfo(UIUtil.UIFile('/icons/units/' .. m.unit .. '_icon.dds', true)) then
            texture = UIUtil.UIFile('/icons/units/' .. m.unit .. '_icon.dds', true)
        end
        local icon = Bitmap(mk, texture)
        LayoutHelpers.SetDimensions(icon, K.markerSize - 4, K.markerSize - 4)
        LayoutHelpers.AtCenterIn(icon, mk)
        icon.Depth:Set(function() return inner.Depth() + 1 end)
        icon:DisableHitTest()
        local armies = GetArmiesTable().armiesTable
        local stripe = Bitmap(mk)
        stripe:SetSolidColor(armies and armies[m.army] and armies[m.army].color or 'ffffffff')
        stripe.Left:Set(mk.Left)
        stripe.Right:Set(mk.Right)
        stripe.Top:Set(mk.Bottom)
        stripe.Height:Set(S(3))
        stripe.Depth:Set(function() return mk.Depth() + 1 end)
        stripe:DisableHitTest()
        -- the line down through the graph at the moment's time (under the armies' lines)
        local line = Bitmap(overlay)
        line:SetSolidColor(look.frame)
        line:SetAlpha(0.6)
        line.Width:Set(math.max(1, S(1)))
        line.Top:Set(function() return mk.Bottom() + S(3) end)
        line.Bottom:Set(bg.Bottom)
        line.Depth:Set(function() return bg.Depth() + 1 end)
        line:DisableHitTest()
        local text = timeline.MomentText(m)
        mk.HandleEvent = function(self, event)
            if event.Type == 'MouseEnter' then
                RemasterMoments.hover = text
                -- (with the host's moho64ui_WatchMoment a click opens the replay there: the tip says so)
                Tooltip.CreateMouseoverDisplay(self, { text = rawget(_G, 'moho64ui_WatchMoment') and (text .. "  (click: watch it in the replay)") or text }, 0, false, nil, true)
                return true
            elseif event.Type == 'MouseExit' then
                RemasterMoments.hover = false
                Tooltip.DestroyMouseoverDisplay()
                return true
            elseif event.Type == 'ButtonPress' then
                return AskWatch(m, text)
            end
            return false
        end
        table.insert(RemasterMoments.markers, { moment = m, control = mk, line = line, x = GraphX(bg, m.tick / 10, gameSeconds) })
    end

    -- the markers along the top in time order, each centred at its time or, where that would overlap the one before,
    -- just right of it; those pushed past the graph's right edge (the game's end) are moved back left, in their order
    -- (each line stays at its time)
    local function LayoutMarkers(bg, gameSeconds)
        local size, gap = S(K.markerSize), S(2)
        local lefts, lastX = {}, -10000
        for i, mk in RemasterMoments.markers do
            local at = GraphX(bg, mk.moment.tick / 10, gameSeconds)
            local x = math.floor(at - size / 2)
            if x < lastX + size + gap then x = lastX + size + gap end
            lefts[i], lastX, mk.x = x, x, at
        end
        local most = math.floor(bg.Right()) - size
        for i = table.getn(lefts), 1, -1 do
            if lefts[i] > most then lefts[i] = most end
            most = lefts[i] - size - gap
        end
        for i, mk in RemasterMoments.markers do
            local dx, lx = lefts[i] - bg.Left(), math.floor(mk.x) - bg.Left()
            mk.control.Left:Set(function() return bg.Left() + dx end)
            mk.control.Top:Set(function() return bg.Top() + S(2) end)
            mk.line.Left:Set(function() return bg.Left() + lx end)
        end
    end

    -- the fighting strip: columns of the graph's points' width, each the most damage dealt in the 5 s it holds
    -- (square root, as the timeline's: a quiet stretch still shows)
    local function MakeStrip(overlay, bg, fight, gameSeconds)
        for _, c in RemasterMoments.columns do c:Destroy() end
        RemasterMoments.columns = {}
        local width = bg.Right() - bg.Left()
        local count = math.max(1, math.floor(width / S(K.columnWidth)))
        local values, most = {}, 0
        for _, f in fight do
            if f.tick / 10 < gameSeconds then
                local i = math.min(count, math.floor(f.tick / 10 / gameSeconds * count) + 1)
                values[i] = math.max(values[i] or 0, f.damage)
                most = math.max(most, f.damage)
            end
        end
        if most <= 0 then return end
        for i = 1, count do
            local v = values[i]
            if v and v > 0 then
                local h = math.max(1, math.floor(S(K.stripHeight) * math.sqrt(v / most)))
                local c = Bitmap(overlay)
                c:SetSolidColor(K.fight)
                c:SetAlpha(0.7)
                local dx = (i - 1) * S(K.columnWidth)
                c.Left:Set(function() return bg.Left() + dx end)
                c.Width:Set(S(K.columnWidth))
                c.Bottom:Set(bg.Bottom)
                c.Height:Set(h)
                c.Depth:Set(function() return bg.Depth() + 1 end)
                c:DisableHitTest()
                c.value = v
                table.insert(RemasterMoments.columns, c)
            end
        end
    end

    local function MarkGraph(graph, bg)
        if not moho64ui_GameMoments then return end
        local timeline = import('/moho64ui/lua/timeline.lua')
        local overlay = Group(graph)
        LayoutHelpers.FillParent(overlay, bg)
        overlay:DisableHitTest()
        local status = UIUtil.CreateText(overlay, "", 12, UIUtil.bodyFont)
        status:SetColor('ffc0c0c0')
        status:DisableHitTest()
        LayoutHelpers.AtRightTopIn(status, bg, 8, K.markerSize + 8)
        status.Depth:Set(function() return bg.Depth() + 6 end)
        RemasterMoments = { markers = {}, columns = {}, status = "", done = false, hover = false, overlay = overlay }
        local mine = RemasterMoments
        local gameSeconds = math.max(GetGameTimeSeconds(), 0.1)  -- (the graph's own time axis: create_graph's)
        ForkThread(function()
            local seen, stripAt = 0, -1
            while not IsDestroyed(overlay) and RemasterMoments == mine do
                local info = moho64ui_GameMoments(true)
                if info then
                    local list = info.moments or {}
                    local added = false
                    for i = seen + 1, table.getn(list) do
                        local m = list[i]
                        if m.tick / 10 <= gameSeconds + 0.5 then  -- (a replay ended early: what lies past its end is not shown)
                            AddMarker(overlay, bg, m, gameSeconds, timeline)
                            added = true
                        end
                    end
                    seen = table.getn(list)
                    if added then LayoutMarkers(bg, gameSeconds) end
                    -- (the strip again when the look has gone on a while, and once at its end)
                    local fight = info.fight or {}
                    if not info.scanning or table.getn(fight) >= stripAt + 24 then
                        MakeStrip(overlay, bg, fight, gameSeconds)
                        stripAt = table.getn(fight)
                    end
                    if info.scanning then
                        local share = info.ticks > 0 and math.min(99, math.floor(100 * info.scanned / math.max(1, info.ticks))) or 0
                        mine.status = "Finding the game's moments " .. share .. "%"
                    else
                        mine.status = ""
                        mine.done = true
                    end
                    status:SetText(mine.status)
                    if mine.done then break end
                else
                    mine.status = ""
                    status:SetText("")
                end
                WaitSeconds(0.5)
            end
        end)
    end

    function create_graph(parent, path)
        local graph = RemasterGame.create_graph(parent, path)
        if graph and bg and RemasterScoreExtrasOn() then MarkGraph(graph, bg) end
        return graph
    end
end
