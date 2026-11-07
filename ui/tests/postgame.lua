-- The post-game report (Chris, 2026-11-07): FAF's score screen with our additions (ui/data/hook/lua/ui/dialogs/
-- hotstats.lua; the host's moho64ui_GameMoments, ui/user/replay_timeline.cpp, GameSession::GameMoments). The game runs
-- five minutes ahead (--fast-forward 3000), then the player's heavy bots (the game's own cheat callback makes the units)
-- kill an experimental of the opponent's made beside them, and then the opponent's commander: a victory. Checked: the
-- host begins looking for the game's moments when the game is over, by itself (its own replay scanned, the engine's
-- ScanReplayMoments), not before; the score screen comes up the game's way; the moments found are the game's (the
-- experimental killed, the commander killed where and when he was, his army defeated, the game's end for the winner);
-- on the Graph page each is a marker along the top at its time, its line down through the graph, the mouse over one
-- says what it was; the fighting strip along the bottom has its tallest column where the fighting was; the graphs of
-- the list (Mass Rate, Energy Rate, Units Still Alive, Total Units Built, Total Losses, Army Value) each draw a line
-- for every army with its end value; Army Value is built less lost mass; the Dual page's graph has the markers too;
-- the Kill Stats chart counts the commanders killed; with the option Score Screen Extras off the screen is FAF's (no
-- markers, no Army Value, the chart's column as FAF has it); Continue leaves the game.
-- Run it like score_tabs.lua: a launched game with cheats on (session_ai_cheats.lua), --fast-forward 3000 --frames 20000.
-- `/postgame_pictures <prefix>` (the game's command line, after "--"; in a --window run): the screens saved as
-- <prefix>_graph.png, _hover.png, _armyvalue.png, _massrate.png, _dual.png, _killstats.png for Chris; the run then ends
-- itself (ExitApplication) instead of Continue.
-- The script's part ends with "TEST done"; the host then logs "the game is left: back to the front end".
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local pictures = (GetCommandLineArg("/postgame_pictures", 1) or {})[1]
local function picture(name)
    if not pictures or not moho64ui_GameMomentsCaptureForTests then return end
    WaitSeconds(0.5)
    moho64ui_GameMomentsCaptureForTests(pictures .. "_" .. name .. ".png")
    WaitSeconds(0.5)
    LOG("TEST note picture " .. pictures .. "_" .. name .. ".png")
end

-- every text the scripts make from here on (the screen's labels and buttons are found by them)
local UIUtil = import("/lua/ui/uiutil.lua")
local texts = {}
local oldCreateText = UIUtil.CreateText
UIUtil.CreateText = function(parent, label, size, font, dropshadow)
    local t = oldCreateText(parent, label, size, font, dropshadow)
    table.insert(texts, t)
    return t
end
local function shown(c)
    if not c or IsDestroyed(c) then return false end
    while c do
        if c:IsHidden() then return false end
        c = c:GetParent()
    end
    return true
end
local function find(label, from, newest)
    local a, b, step = from or 1, table.getn(texts), 1
    if newest then a, b, step = b, from or 1, -1 end
    for i = a, b, step do
        local t = texts[i]
        if shown(t) and string.find(t:GetText(), label, 1, true) == 1 then return t end
    end
end
local function shownTexts(from)
    local out = {}
    for i = from, table.getn(texts) do
        if shown(texts[i]) then table.insert(out, texts[i]:GetText()) end
    end
    return out
end
local function has(list, label)
    for _, s in list do
        if s == label then return true end
    end
    return false
end
local function middle(c)
    return (c.Left() + c.Right()) / 2, (c.Top() + c.Bottom()) / 2
end
local function clickAt(x, y)
    moho64ui_MouseMove(x, y)
    WaitSeconds(0.2)
    moho64ui_MouseButton("Left", true, x, y)
    moho64ui_MouseButton("Left", false, x, y)
    WaitSeconds(0.5)
end
local function click(c)
    if not c then return false end
    clickAt(middle(c))
    return true
end
local function waitFor(cond, seconds)
    local waited = 0
    while not cond() and waited < seconds do
        WaitSeconds(0.5)
        waited = waited + 0.5
    end
    return cond()
end
local S = import("/lua/maui/layouthelpers.lua").ScaleNumber
local function near(a, b, within) return a ~= nil and b ~= nil and math.abs(a - b) <= within end
local function setExtras(on)
    local Prefs = import("/lua/user/prefs.lua")
    local options = Prefs.GetFromCurrentProfile('options') or {}
    options.remaster_score_extras = on and 1 or 0
    Prefs.SetToCurrentProfile('options', options)
end

ForkThread(function()
    WaitSeconds(3)
    -- (in a window the run ahead follows the screen's frames: waited for)
    check("the game has run ahead five minutes", waitFor(function() return GetGameTimeSeconds() >= 300 end, 30), true)
    check("the host's moments for the score screen are there", moho64ui_GameMoments ~= nil, true)
    check("while the game runs nothing is looked for yet", moho64ui_GameMoments and moho64ui_GameMoments() == nil, true)
    setExtras(true)
    local me = GetFocusArmy()
    local other = me == 1 and 2 or 1
    local cdr = GetArmyAvatars()[1]
    local p = cdr:GetPosition()
    local size = SessionGetScenarioInfo().size
    local e = {size[1] - p[1], p[2], size[2] - p[3]}  -- the opponent's start, opposite the player's
    local function spawn(army, pos, bp, n)
        SimCallback({Func = "CheatSpawnUnit", Args = {army = army, pos = pos, bpId = bp, count = n or 1, yaw = 0}})
        WaitSeconds(0.3)
    end
    -- an experimental of the opponent's in the middle of the map, the player's heavy bots beside it
    local mid = {(p[1] + e[1]) / 2, p[2], (p[3] + e[3]) / 2}
    local expAt = GetGameTimeSeconds()
    spawn(other, mid, "url0402")
    spawn(me, {mid[1] + 12, mid[2], mid[3] + 12}, "xel0305", 20)
    WaitSeconds(45)
    -- then the opponent's commander: a victory
    local killAt = GetGameTimeSeconds()
    spawn(me, {e[1] + 8, p[2], e[3] + 8}, "xel0305", 12)
    check("the game ends with a victory", waitFor(SessionIsGameOver, 90), true)
    local endTime = GetGameTimeSeconds()
    -- the host looks for the moments as soon as the game is over (its own replay scanned), not when asked
    check("the host begins looking for the moments when the game is over", waitFor(function() return moho64ui_GameMoments() ~= nil end, 3), true)
    local hotstats = import("/lua/ui/dialogs/hotstats.lua")
    waitFor(function() return hotstats.scoreData.history ~= nil end, 5)
    local sd = hotstats.scoreData
    local n = table.getn(sd.history or {})

    -- the score screen, the game's way; its graphs' bitmaps (a graph's lines are bitmaps of three pixels each)
    local made = {}
    local BitmapClass = import("/lua/maui/bitmap.lua").Bitmap
    local oldInit = BitmapClass.__init
    BitmapClass.__init = function(self, parent, filename, debugname)
        oldInit(self, parent, filename, debugname)
        table.insert(made, {bmp = self, parent = parent})
    end
    local graphs = {}
    local oldGraph = hotstats.create_graph
    hotstats.create_graph = function(parent, path)
        local from, texts0 = table.getn(made) + 1, table.getn(texts) + 1
        local g = oldGraph(parent, path)
        table.insert(graphs, {made = from, textsFrom = texts0, path = path, bg = hotstats.bg, time = GetGameTimeSeconds()})
        return g
    end
    check("the Score button is up", click(find(LOC("<LOC _Score>"), 1, true)), true)
    local first = table.getn(texts) + 1
    check("it asks first", click(find(LOC("<LOC _Yes>"), 1, true)), true)
    local score = import("/lua/ui/dialogs/score.lua")
    check("the score screen comes up", waitFor(function() return score.dialog ~= false and find("Standard", first) ~= nil end, 5), true)
    local tab = {}
    for _, name in {"Standard", "Chart", "Graph", "Dual", "Continue"} do
        local label = find(name, first, true)
        tab[name] = label and label:GetParent()
    end
    check("the Graph page is shown first", tab.Graph ~= nil and tab.Graph:IsChecked(), true)

    -- the moments: looked for while the screen is up, then the game's own
    check("the moments are found", waitFor(function() return hotstats.RemasterMoments.done end, 180), true)
    local info = moho64ui_GameMoments() or {moments = {}, fight = {}}
    check("the look is over", info.scanning, false)
    check("the game's length is the game's", near(info.ticks / 10, endTime, 1), true)
    local function momentsOf(kind, army)
        local out = {}
        for _, m in info.moments do
            if m.kind == kind and (not army or m.army == army) then table.insert(out, m) end
        end
        return out
    end
    local expDied = momentsOf("ExperimentalDied", other)
    check("the opponent's experimental killed, once", table.getn(expDied), 1)
    if expDied[1] then
        check("its unit", expDied[1].unit, "url0402")
        check("killed while the bots fought it", expDied[1].tick / 10 > expAt and expDied[1].tick / 10 < killAt, true)
        check("where it was made", near(expDied[1].position[1], mid[1], 30) and near(expDied[1].position[3], mid[3], 30), true)
    end
    local cdrDied = momentsOf("CommanderDied", other)
    check("the opponent's commander killed, once", table.getn(cdrDied), 1)
    if cdrDied[1] then
        check("after the bots came and before the end", cdrDied[1].tick / 10 > killAt and cdrDied[1].tick / 10 <= endTime, true)
        check("at his base", near(cdrDied[1].position[1], e[1], 40) and near(cdrDied[1].position[3], e[3], 40), true)
    end
    check("the player's commander lives", table.getn(momentsOf("CommanderDied", me)), 0)
    check("the opponent's army defeated", table.getn(momentsOf("ArmyDefeated", other)), 1)
    check("the game's end for the winner", table.getn(momentsOf("GameEnded", me)), 1)
    local fought = 0
    for _, f in info.fight do fought = fought + f.damage end
    check("the fighting is counted", fought > 0, true)

    -- the markers on the Graph page (the score)
    local RM = hotstats.RemasterMoments
    local g = graphs[table.getn(graphs)]
    local timeline = import("/moho64ui/lua/timeline.lua")
    local function checkMarkers(what, g)
        local RM = hotstats.RemasterMoments
        local want = 0
        for _, m in info.moments do
            if m.tick / 10 <= g.time + 0.5 then want = want + 1 end
        end
        check(what .. ": a marker for each moment", table.getn(RM.markers), want)
        local bg = g.bg
        local good, bad = 0, nil
        local lastRight = -10000
        for _, mk in RM.markers do
            local x = bg.Left() + (bg.Right() - bg.Left()) * mk.moment.tick / 10 / g.time
            local c = mk.control
            -- (its line at its time from under the marker to the graph's bottom; the marker along the top, inside the
            -- graph, not over the one before it, its middle at its time or moved aside by the others crowding there:
            -- at most their number of markers' widths)
            local aside = table.getn(RM.markers) * (S(20) + S(2))
            if shown(c) and near(mk.line.Left(), math.floor(x), 1) and near(c.Top(), bg.Top() + S(2), 1) and
                mk.line.Bottom() == bg.Bottom() and mk.line.Top() > c.Bottom() and c.Left() > lastRight and
                near((c.Left() + c.Right()) / 2, x, aside) and c.Left() >= bg.Left() - S(10) and c.Right() <= bg.Right() + 1 then
                good = good + 1
            elseif not bad then
                bad = mk.moment.kind .. " at " .. mk.moment.tick .. ": line " .. mk.line.Left() .. " for " .. x .. ", marker " .. c.Left() .. ", " .. c.Top()
            end
            lastRight = c.Right()
        end
        check(what .. ": each at its time along the top, its line down through the graph", bad or good, want)
        for _, mk in RM.markers do  -- (the experimental's, alone at its time: its marker right over its line)
            if mk.moment.kind == "ExperimentalDied" then
                check(what .. ": the experimental's marker over its line", near((mk.control.Left() + mk.control.Right()) / 2, mk.line.Left(), 1.5), true)
            end
        end
    end
    if g then
        checkMarkers("the score graph", g)
        -- the fighting strip: its tallest column where the fighting was
        local tallest = nil
        for _, c in RM.columns do
            if not tallest or c.Height() > tallest.Height() then tallest = c end
        end
        check("the fighting strip", table.getn(RM.columns) > 0, true)
        if tallest then
            local t = (tallest.Left() - g.bg.Left()) / (g.bg.Right() - g.bg.Left()) * g.time
            check("its tallest column in the fighting", t >= expAt - 10 and t <= endTime + 5, true)
            check("along the bottom of the graph", tallest.Bottom() == g.bg.Bottom(), true)
        end
        picture("graph")
        -- the mouse over the commander's marker: what it was and when
        local cdrMarker = nil
        for _, mk in RM.markers do
            if mk.moment.kind == "CommanderDied" then cdrMarker = mk end
        end
        if cdrMarker then
            local x, y = middle(cdrMarker.control)
            moho64ui_MouseMove(x - 3, y)
            WaitSeconds(0.2)
            moho64ui_MouseMove(x, y)
            WaitSeconds(0.5)
            check("the mouse over a marker says what it was", RM.hover, timeline.MomentText(cdrMarker.moment))
            picture("hover")
            moho64ui_MouseMove(x, y + 300)
            WaitSeconds(0.3)
        end
    end

    -- the list's graphs: a line for every army through the game, its end value
    local combo = hotstats.combo_graph
    local list = {}
    for i = 1, (combo and combo._list) and combo._list:GetItemCount() or 0 do  -- (FAF's info_dialog, Army Value last)
        list[i] = combo._list:GetItem(i - 1)
    end
    check("Army Value is the list's last graph", list[table.getn(list)], "Army Value (Mass)")
    local function index(name)
        for i, s in list do
            if s == LOC(name) or s == name then return i end
        end
    end
    local function checkLines(what, path)
        local g = graphs[table.getn(graphs)]
        check(what .. ": a graph drawn for it", g ~= nil and g.path[1] == path[1] and g.path[2] == path[2], true)
        if not g then return end
        local area = g.bg:GetParent()
        local width = g.bg.Right() - g.bg.Left()
        local points = {[me] = 0, [other] = 0}
        for i = g.made, table.getn(made) do
            local b = made[i].bmp
            if made[i].parent == area and not IsDestroyed(b) and b.Bottom() - b.Top() > 0 then
                local w = b.Right() - b.Left()
                if math.abs(w - 6) < 0.01 then points[me] = points[me] + 1
                elseif math.abs(w - 3) < 0.01 then points[other] = points[other] + 1 end
            end
        end
        check(what .. ": the player's line across the graph", points[me] > width / 3 - 10, true)
        check(what .. ": the opponent's line across the graph", points[other] > width / 3 - 10, true)
        local shownList = shownTexts(g.textsFrom or 1)
        local ends = true
        for _, army in {me, other} do
            ends = ends and has(shownList, hotstats.FormatNumber(math.floor(hotstats.return_value(n + 1, army, path))))
        end
        check(what .. ": each army's end value", ends, true)
    end
    local paths = {
        {"<LOC SCORE_0089>Mass Rate", {"resources", "massin", "rate", fac_mul = 10}, "massrate"},
        {"<LOC SCORE_0086>Energy Rate", {"resources", "energyin", "rate", fac_mul = 10}},
        {"<LOC SCORE_0080>Units Still Alive", {"general", "currentunits", false}},
        {"<LOC SCORE_0079>Total Units Built", {"general", "built", "count"}},
        {"<LOC SCORE_0085>Total Losses", {"general", "lost", "count"}},
        {"Army Value (Mass)", {"general", "built", "mass", remaster = 'armyvalue'}, "armyvalue"},
    }
    for k, entry in paths do
        local i = index(entry[1])
        check(LOC(entry[1]) .. ": in the list", i ~= nil, true)
        if i then
            if k == 3 then  -- (one by the mouse, as a player picks it: the list opened, its row clicked)
                click(combo)
                WaitSeconds(0.5)
                clickAt(combo._list.Left() + 20, combo._list.Top() + combo._list:GetRowHeight() * (i - 0.5))
            else
                combo.OnClick(combo, i, list[i])
            end
            WaitSeconds(3)
            checkLines(LOC(entry[1]), entry[2])
            if graphs[table.getn(graphs)] then checkMarkers(LOC(entry[1]), graphs[table.getn(graphs)]) end
            if entry[3] then picture(entry[3]) end
        end
    end
    local cur = sd.current
    local av = {"general", "built", "mass", remaster = 'armyvalue'}
    for _, army in {me, other} do
        check("Army Value of army " .. army .. " is its built less its lost mass", hotstats.return_value(n + 1, army, av),
            math.max(0, cur[army].general.built.mass - cur[army].general.lost.mass))
    end

    -- Dual: the score graph with its markers over the overview chart
    click(tab.Dual)
    WaitSeconds(3)
    check("Dual: a graph", graphs[table.getn(graphs)] ~= nil and graphs[table.getn(graphs)].path[2] == "score", true)
    if graphs[table.getn(graphs)] then checkMarkers("Dual's graph", graphs[table.getn(graphs)]) end
    picture("dual")

    -- Kill Stats: the commanders killed in its ACU column
    click(tab.Chart)
    WaitSeconds(1.5)
    local mark = table.getn(texts) + 1
    check("Chart: the Kill Stats button", click(find(LOC("<LOC SCORE_0121>Kill Stats"), first, true)), true)
    WaitSeconds(1.5)
    check("Kill Stats: its ACU column counts the kills", hotstats.RemasterKillStatsAcu, "kills")
    local values = shownTexts(mark)
    check("Kill Stats: the player's commander kill shown", has(values, tostring(hotstats.makeKMG(math.max(1, cur[me].units.cdr.kills)))), true)
    picture("killstats")

    -- the option off: FAF's screen as it is
    setExtras(false)
    click(tab.Graph)
    WaitSeconds(3)
    combo = hotstats.combo_graph
    local count = (combo and combo._list) and combo._list:GetItemCount() or 0
    check("Off: the list is FAF's (no Army Value)", count, table.getn(list) - 1)
    check("Off: no markers on the graph", hotstats.RemasterMoments.overlay == nil or IsDestroyed(hotstats.RemasterMoments.overlay), true)
    click(tab.Chart)
    WaitSeconds(1.5)
    click(find(LOC("<LOC SCORE_0121>Kill Stats"), first, true))
    WaitSeconds(1.5)
    check("Off: the Kill Stats ACU column as FAF has it", hotstats.RemasterKillStatsAcu, "built")
    setExtras(true)

    LOG("TEST done: " .. failed .. " failed")
    if pictures then  -- (a window's run ends itself: its frames follow the screen's rate)
        ExitApplication()
        return
    end
    click(tab.Continue)
    WaitSeconds(1)
    LOG("TEST FAIL Continue did not leave the game")
end)
