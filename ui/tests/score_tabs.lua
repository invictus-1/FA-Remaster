-- The score screen of a real game, every page of it, the game's own scripts (FAF's
-- /lua/ui/dialogs/score.lua and hotstats.lua). The game runs five minutes ahead (--fast-forward 3000: the
-- AI builds its base), then both armies fight (units made by the game's own cheat callback: the player's
-- tanks and fighters at the opponent's base, the opponent's tanks at the player's, a factory of the
-- player's that builds), the opponent's commander is destroyed, and the score screen comes up the game's
-- way (the Score button, its question). Checked: the history the interface is handed is the simulation's
-- (one entry printed in the simulation, ui/tests/score_sim.lua, compared number for number); the Graph
-- page (the first shown) draws a line per army through every history entry, its axes and end values, its
-- buttons, its list of graphs and the value under the mouse; the Chart page's bars and their pages (by a
-- button and by a click on a bar); the Dual page; the Standard page's three tabs, each army's row with the
-- numbers of the score data in every kind the page offers, the sorting by a column both ways; and
-- Continue leaves the game.
-- Run it like victory.lua (a launched game with cheats on; the simulation's part goes through
-- ExecLuaInSim('dofile', ...), read from the program's working folder, the worktree) with
-- --fast-forward 3000 --frames 8000. The script's part ends with "TEST done"; the host then logs
-- "the game is left: back to the front end".
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end

-- every text the scripts make from here on: the screen's labels, numbers and buttons are found by them
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
-- the first (with newest: the last) shown text made since `from` whose label begins with `label`
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
local function join(path)
    local s = ""
    for _, key in path do
        if key then s = s .. (s == "" and "" or ".") .. key end
    end
    return s
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

ForkThread(function()
    WaitSeconds(3)
    check("the game has run ahead five minutes", GetGameTimeSeconds() >= 300, true)
    local me = GetFocusArmy()
    local other = me == 1 and 2 or 1
    local armies = GetArmiesTable().armiesTable
    local cdr = GetArmyAvatars()[1]
    local p = cdr:GetPosition()
    local size = SessionGetScenarioInfo().size
    local e = {size[1] - p[1], p[2], size[2] - p[3]}  -- the opponent's start, opposite the player's
    local function spawn(army, pos, bp, n)
        SimCallback({Func = "CheatSpawnUnit", Args = {army = army, pos = pos, bpId = bp, count = n or 1, yaw = 0}})
        WaitSeconds(0.3)
    end
    spawn(me, {p[1] - 30, p[2], p[3] - 4}, "ueb0101")
    WaitSeconds(1)
    UISelectionByCategory("FACTORY", false, false, false, false)
    IssueBlueprintCommand("UNITCOMMAND_BuildFactory", "uel0106", 3)
    spawn(other, {p[1] + 25, p[2], p[3] + 25}, "url0107", 4)
    spawn(me, {e[1] + 30, p[2], e[3] + 30}, "uel0201", 8)
    spawn(me, {e[1] + 30, p[2], e[3] - 30}, "uea0102", 2)
    WaitSeconds(25)

    -- one history entry as the simulation holds it (printed there, heard in the console)
    local heard = {}
    local handle = AddConsoleOutputReciever(function(text)
        if string.find(text, "SCORESIM", 1, true) == 1 then table.insert(heard, text) end
    end)
    ExecLuaInSim("dofile", "ui/tests/score_sim.lua")
    waitFor(function()
        local last = heard[table.getn(heard)]
        return last and string.find(last, "SCORESIM done", 1, true) == 1
    end, 5)
    RemoveConsoleOutputReciever(handle)
    local simValues, simEntry = {}, nil
    for _, line in heard do
        local _, _, h, army, path, value = string.find(line, "^SCORESIM (%d+) (%d+) (%S+) (%S+)$")
        if h then
            simEntry = tonumber(h)
            table.insert(simValues, {army = tonumber(army), path = path, value = value})
        end
    end
    check("the simulation printed a history entry", simEntry ~= nil and table.getn(simValues) >= 20, true)

    -- the opponent's commander destroyed: a victory
    spawn(me, {e[1] + 8, p[2], e[3] + 8}, "xel0305", 12)
    check("the game ends with a victory", waitFor(SessionIsGameOver, 90), true)
    local hotstats = import("/lua/ui/dialogs/hotstats.lua")
    waitFor(function() return hotstats.scoreData.history ~= nil end, 5)
    local sd = hotstats.scoreData
    local endTime = GetGameTimeSeconds()
    local n = table.getn(sd.history or {})
    check("the history every 30 seconds", sd.interval, 30)
    check("an entry for each 30 seconds of the game", n == math.floor(endTime / 30) + 1 or n == math.floor(endTime / 30), true)
    local same, differ = 0, nil
    for _, s in simValues do
        local v = sd.history and sd.history[simEntry] and sd.history[simEntry][s.army]
        for key in string.gfind(s.path, "[^%.]+") do v = type(v) == "table" and v[key] or nil end
        if tostring(v) == s.value then
            same = same + 1
        elseif not differ then
            differ = "army " .. s.army .. " " .. s.path .. ": " .. tostring(v) .. " here, " .. s.value .. " in the simulation"
        end
    end
    check("the history entry is the simulation's, number for number", differ or same, table.getn(simValues))
    local cur = sd.current
    check("each army's name in the final score", tostring(cur[me] and cur[me].name) .. " / " .. tostring(cur[other] and cur[other].name),
        armies[me].nickname .. " / " .. armies[other].nickname)
    check("both armies built and fought", cur[me].general.built.count > 3 and cur[other].general.built.count > 3 and
        cur[me].general.kills.count > 0 and cur[other].general.kills.count > 0 and cur[other].general.lost.count > 0, true)
    check("the opponent's commander counts as killed by the player", cur[me].units.cdr.kills, 1)

    -- the score screen, the game's way: the Score button, its question
    -- (every bitmap made from here on, with its parent: a graph's lines are bitmaps of three pixels each)
    local made = {}
    local BitmapClass = import("/lua/maui/bitmap.lua").Bitmap
    local oldInit = BitmapClass.__init
    BitmapClass.__init = function(self, parent, filename, debugname)
        oldInit(self, parent, filename, debugname)
        table.insert(made, {bmp = self, parent = parent})
    end
    local graphs, bars = {}, {}
    local oldGraph, oldBar = hotstats.create_graph, hotstats.create_graph_bar
    hotstats.create_graph = function(parent, path)
        local from = table.getn(made) + 1
        local g = oldGraph(parent, path)
        table.insert(graphs, {made = from, path = path, bg = hotstats.bg, time = GetGameTimeSeconds()})
        return g
    end
    hotstats.create_graph_bar = function(parent, name)
        local from = table.getn(texts) + 1
        local c = oldBar(parent, name)
        table.insert(bars, {name = name, from = from})
        return c
    end
    check("the Score button is up", click(find(LOC("<LOC _Score>"), 1, true)), true)
    local first = table.getn(texts) + 1
    check("it asks first", click(find(LOC("<LOC _Yes>"), 1, true)), true)
    local score = import("/lua/ui/dialogs/score.lua")
    check("the score screen comes up", waitFor(function() return score.dialog ~= false and find("Standard", first) ~= nil end, 5), true)
    WaitSeconds(3)  -- (the graph's lines are drawn by a thread, a part each frame)
    local tab = {}
    for _, name in {"Standard", "Chart", "Graph", "Dual", "Continue"} do
        local label = find(name, first, true)
        tab[name] = label and label:GetParent()
    end
    check("five tabs at the bottom, Graph the one shown first", tab.Graph ~= nil and tab.Continue ~= nil and tab.Graph:IsChecked() and
        not tab.Chart:IsChecked() and not tab.Standard:IsChecked() and not tab.Dual:IsChecked(), true)

    -- a graph: a line per army through every history entry (at the start of each 30 seconds the army's
    -- point stands at the entry's value, on the square-root scale the script draws), the axes, the end values
    local function maxOf(path)
        local maxv = 0
        for k = 1, n + 1 do
            for _, army in {me, other} do maxv = math.max(maxv, hotstats.return_value(k, army, path)) end
        end
        return math.ceil(math.sqrt(math.ceil(maxv * 1.05)))  -- (the scale's top, as create_graph takes it)
    end
    local function checkGraph(what, path, from)
        local g = graphs[table.getn(graphs)]
        check(what .. ": a graph drawn for it", g ~= nil and join(g.path) == join(path), true)
        if not g then return end
        local bg = g.bg
        local scaledMax = maxOf(path)
        local width, height = bg.Right() - bg.Left(), bg.Bottom() - bg.Top()  -- (the edges: the script sets those, not its Width)
        local yScale = height / scaledMax
        local segment = width * sd.interval / g.time
        -- the graph's own bitmaps of three pixels' width: a point of a line spans upwards (the focus
        -- army's, the player's, three pixels wider, from three to the left: hotstats' extraSize); the
        -- shares' bands behind them are laid out from a Top down to a Bottom above it (and where no
        -- army has any yet, the script's share is 0 / 0: no number at all)
        local lines, bands = {[me] = {}, [other] = {}}, 0
        local area = bg:GetParent()
        for i = g.made, table.getn(made) do
            local b = made[i].bmp
            if made[i].parent == area and not IsDestroyed(b) then
                local w, h = b.Right() - b.Left(), b.Bottom() - b.Top()
                if h > 0 and math.abs(w - 6) < 0.01 then
                    lines[me][b.Left() - bg.Left() + 3] = b
                elseif h > 0 and math.abs(w - 3) < 0.01 then
                    lines[other][b.Left() - bg.Left()] = b
                elseif math.abs(w - 3) < 0.01 then
                    bands = bands + 1
                end
            end
        end
        check(what .. ": the shares' bands behind the lines", bands > 2 * width / 3 - 20, true)
        local good, points, bad = 0, 0, nil
        for _, army in {me, other} do
            local line = lines[army]
            local xs = {}
            for x, _ in line do table.insert(xs, x) end
            table.sort(xs)
            points = points + table.getn(xs)
            for k = 1, n do
                local start = (k - 1) * segment
                local x0 = nil
                for _, x in xs do
                    if x >= start - 3 then
                        x0 = x
                        break
                    end
                end
                if x0 then
                    local b = line[x0]
                    local y = bg.Bottom() - math.sqrt(hotstats.return_value(k, army, path)) * yScale
                    if b.Top() <= y + 0.5 and y <= b.Bottom() + 0.5 then
                        good = good + 1
                    elseif not bad then
                        bad = "army " .. army .. " entry " .. k .. ": its point spans " .. b.Top() .. " to " .. b.Bottom() .. ", its value's place is " .. y
                    end
                end
            end
        end
        check(what .. ": both lines drawn across the graph", points > 2 * width / 3 - 20, true)
        check(what .. ": through every history entry", bad or good, 2 * n)
        local list = shownTexts(from)
        local ends = true
        for _, army in {me, other} do
            ends = ends and has(list, hotstats.FormatNumber(math.floor(hotstats.return_value(n + 1, army, path))))
        end
        check(what .. ": each army's end value", ends, true)
        check(what .. ": the value axis up to the scale's top", has(list, hotstats.FormatNumber(math.floor(scaledMax * scaledMax))), true)
        check(what .. ": the time axis up to the game's end", has(list, hotstats.tps_format(g.time)), true)
        return g
    end
    -- the page's title: hotstats' Title_score, the one text made on the dialog itself
    local function title()
        for i = first, table.getn(texts) do
            local t = texts[i]
            if not IsDestroyed(t) and t:GetParent() == score.dialog then return t:GetText() end
        end
    end
    check("the title is the graph's", title(), LOC("<LOC SCORE_0083>Score"))
    local g = checkGraph("the score", {"general", "score", false}, first)

    -- the value under the mouse: the time and the value at that point of the graph
    if g then
        local x, y = g.bg.Left() + (g.bg.Right() - g.bg.Left()) * 0.45, g.bg.Top() + (g.bg.Bottom() - g.bg.Top()) * 0.5
        moho64ui_MouseMove(x - 5, y)
        WaitSeconds(0.2)
        moho64ui_MouseMove(x, y)
        WaitSeconds(0.3)
        local scaledMax = maxOf({"general", "score", false})
        local want = hotstats.tps_format(0.45 * g.time) .. " / " .. hotstats.FormatNumber(math.floor(hotstats.ReverseScaling(0.5 * scaledMax)))
        local got = nil
        for i = first, table.getn(texts) do
            if shown(texts[i]) and string.find(texts[i]:GetText(), " / ", 1, true) then got = texts[i]:GetText() end
        end
        check("the time and value under the mouse", got, want)
    end

    -- the buttons under the graph and the list over it
    local mark = table.getn(texts) + 1
    click(find(LOC("<LOC SCORE_0111>Mass in"), first, true))
    WaitSeconds(3)
    check("the button 'Mass in': its title", title(), LOC("<LOC SCORE_0082>Total Mass Produced"))
    checkGraph("the mass produced", {"resources", "massin", "total"}, mark)
    local combo = hotstats.combo_graph
    check("the list of graphs", combo ~= nil and combo._list ~= nil, true)
    if combo and combo._list then
        mark = table.getn(texts) + 1
        click(combo)
        WaitSeconds(0.5)
        local list = combo._list
        check("the list opens", shown(list), true)
        clickAt(list.Left() + 20, list.Top() + list:GetRowHeight() * 5.5)  -- (the sixth: Total Kills)
        WaitSeconds(3)
        check("the list's 'Total Kills': its title", title(), LOC("<LOC SCORE_0084>Total Kills"))
        checkGraph("the kills", {"general", "kills", "count"}, mark)
    end

    -- the bar chart: each army's bars with the value over them; its pages by the buttons and by a click on a bar
    local function checkBars(what, name, columns)
        local b = bars[table.getn(bars)]
        check(what .. ": the chart drawn", b ~= nil and b.name == name, true)
        if not b then return end
        local list = shownTexts(b.from)
        local good, missing = 0, nil
        for c, column in columns do
            for _, army in {me, other} do
                local sum = 0
                for _, path in column do sum = sum + hotstats.return_value(0, army, path) end
                if has(list, tostring(hotstats.makeKMG(sum))) then
                    good = good + 1
                elseif not missing then
                    missing = "column " .. c .. ", army " .. army .. ": " .. tostring(hotstats.makeKMG(sum))
                end
            end
        end
        check(what .. ": the value over each army's bars", missing or good, 2 * table.getn(columns))
    end
    local overview = {
        {{"resources", "massin", "total"}, {"resources", "massout", "excess"}},
        {{"resources", "energyin", "total"}, {"resources", "energyout", "excess"}},
        {{"units", "air", "built"}, {"units", "land", "built"}, {"units", "naval", "built"}, {"units", "experimental", "built"},
         {"units", "cdr", "built"}, {"units", "structures", "built"}},
        {{"units", "air", "kills"}, {"units", "land", "kills"}, {"units", "naval", "kills"}, {"units", "experimental", "kills"},
         {"units", "cdr", "kills"}, {"units", "structures", "kills"}},
    }
    mark = table.getn(texts) + 1
    click(tab.Chart)
    WaitSeconds(1.5)
    check("Chart: shown", tab.Chart:IsChecked() and not tab.Graph:IsChecked(), true)
    check("Chart: its title", title(), LOC("<LOC SCORE_0115>Overview"))
    checkBars("Chart's overview", "main_histo", overview)
    local chartMark = mark
    mark = table.getn(texts) + 1
    click(find(LOC("<LOC SCORE_0005>Mass"), chartMark, true))
    WaitSeconds(1.5)
    check("Chart's button 'Mass': its title", title(), LOC("<LOC SCORE_0116>Mass details"))
    checkBars("the mass details", "mass_histo", {{{"resources", "massin", "total"}}, {{"resources", "massout", "total"}}, {{"resources", "massout", "excess"}}})
    -- a click on a bar goes to the page it links to (here back to the overview): the tallest bar, under its value
    local most = hotstats.return_value(0, me, {"resources", "massin", "total"}) >= hotstats.return_value(0, other, {"resources", "massin", "total"}) and me or other
    local value = bars[table.getn(bars)] and find(tostring(hotstats.makeKMG(hotstats.return_value(0, most, {"resources", "massin", "total"}))), bars[table.getn(bars)].from)
    mark = table.getn(texts) + 1
    if value then
        local x, y = middle(value)
        clickAt(x, y + 40)
        WaitSeconds(1)
    end
    check("a click on a bar: its page", bars[table.getn(bars)].name, "main_histo")
    check("with its title", title(), LOC("<LOC SCORE_0115>Overview"))

    -- Dual: the score graph over the overview chart
    local graphsBefore, barsBefore = table.getn(graphs), table.getn(bars)
    mark = table.getn(texts) + 1
    click(tab.Dual)
    WaitSeconds(3)
    check("Dual: a graph and a chart", (table.getn(graphs) - graphsBefore) .. " " .. (table.getn(bars) - barsBefore), "1 1")
    checkGraph("Dual's graph", {"general", "score", false}, mark)
    checkBars("Dual's chart", "main_histo", overview)

    -- the standard page: three tabs, a row for each army
    click(tab.Standard)
    WaitSeconds(1)
    check("Standard: shown", tab.Standard:IsChecked() and not tab.Dual:IsChecked(), true)
    check("Standard: the title says the game was won", find(LOC("<LOC SCORE_0046>Victory"), first) ~= nil, true)
    check("Standard: the game's time", find(GetGameTime(), first) ~= nil, true)
    local pages = {
        {name = "<LOC SCORE_0000>General", key = "general", columns = {"score", "kills", "built", "lost", "mass", "energy"},
         types = {"count", "mass", "energy"}, typeLabels = {"<LOC SCORE_0007>Count", "<LOC SCORE_0008>Mass Value", "<LOC SCORE_0009>Energy Value"}},
        {name = "<LOC SCORE_0010>Units", key = "units", columns = {"cdr", "land", "naval", "air", "structures", "experimental"},
         types = {"kills", "built", "lost"}, typeLabels = {"<LOC SCORE_0017>Kills", "<LOC SCORE_0003>Built", "<LOC SCORE_0019>Losses"}},
        {name = "<LOC SCORE_0020>Resources", key = "resources", columns = {"massin", "massout", "massover", "energyin", "energyout", "energyover"},
         types = {"total", "rate"}, typeLabels = {"<LOC SCORE_0027>Total", "<LOC SCORE_0028>Rate"}},
    }
    -- a cell as the script fills it: the column's value of the kind shown, or the column's own value
    local function cellValue(army, page, column, kind)
        local v = cur[army][page.key][column]
        if type(v) == "table" then v = v[kind] end
        return v or 0
    end
    -- what a page made, by the control each text belongs to, in order: the page's tab label, a sorting
    -- button per column (its label first), then the rows (seven texts on a row's background: the name,
    -- six numbers)
    local function pageOf(from)
        local groups, byParent = {}, {}
        for i = from, table.getn(texts) do
            local t = texts[i]
            if shown(t) then
                local parent = t:GetParent()
                local group = byParent[parent]
                if not group then
                    group = {}
                    byParent[parent] = group
                    table.insert(groups, group)
                end
                table.insert(group, t)
            end
        end
        local rows, columns = {}, {}
        for i = 2, table.getn(groups) do
            if table.getn(groups[i]) == 7 then
                table.insert(rows, groups[i])
            elseif table.getn(rows) == 0 then
                table.insert(columns, groups[i][1])
            end
        end
        return rows, columns
    end
    local function armyNamed(name)
        for _, army in {me, other} do
            if cur[army].name == name then return army end
        end
    end
    local function checkRows(what, rows, page, kind, sortColumn, descending)
        check(what .. ": a row for each army", table.getn(rows), 2)
        if table.getn(rows) ~= 2 then return end
        local good, bad, order = 0, nil, {}
        for r, row in rows do
            local army = armyNamed(row[1]:GetText())
            order[r] = army
            for c, column in page.columns do
                local want = army and tostring(math.floor(cellValue(army, page, column, kind))) or ("(no army named " .. row[1]:GetText() .. ")")
                if row[c + 1]:GetText() == want then
                    good = good + 1
                elseif not bad then
                    bad = "row " .. r .. " (" .. row[1]:GetText() .. ") " .. column .. ": " .. row[c + 1]:GetText() .. ", want " .. want
                end
            end
        end
        check(what .. ": the numbers are the score data's", bad or good, 12)
        if order[1] and order[2] then
            local a, b = cellValue(order[1], page, sortColumn, kind), cellValue(order[2], page, sortColumn, kind)
            check(what .. ": sorted by " .. sortColumn .. (descending and ", the most first" or ", the least first"),
                a == b or (descending and a > b) or (not descending and a < b), true)
        end
    end
    -- (as score.lua keeps them: a new page sorts by its first column and keeps the direction; a click on
    -- the column sorted by turns the direction, on another column sorts by that with the most first)
    local descending = true
    for p, page in pages do
        local what = LOC(page.name)
        local from = nil
        if p == 1 then
            local seen = 0
            for i = first, table.getn(texts) do
                if not IsDestroyed(texts[i]) and texts[i]:GetText() == what then
                    seen = seen + 1
                    if seen == 2 then  -- (the first is its tab button's label, the second the page's own)
                        from = i
                        break
                    end
                end
            end
        else
            local button = nil
            for i = first, table.getn(texts) do
                if shown(texts[i]) and texts[i]:GetText() == what then
                    button = texts[i]
                    break
                end
            end
            from = table.getn(texts) + 1
            check(what .. ": its tab", click(button), true)
            WaitSeconds(0.5)
        end
        local rows, columns = pageOf(from or first)
        check(what .. ": a sorting button over each column", table.getn(columns), 6)
        local sortColumn = page.columns[1]
        checkRows(what, rows, page, page.types[1], sortColumn, descending)
        for t = 2, table.getn(page.types) do
            click(find(LOC(page.typeLabels[t]), from, true))
            checkRows(what .. ", " .. LOC(page.typeLabels[t]), rows, page, page.types[t], sortColumn, descending)
        end
        local kind = page.types[table.getn(page.types)]
        for k = 1, 2 do
            click(columns[2])
            if sortColumn == page.columns[2] then
                descending = not descending
            else
                sortColumn, descending = page.columns[2], true
            end
            checkRows(what .. ", the second column's button" .. (k == 2 and " again" or ""), rows, page, kind, sortColumn, descending)
        end
    end

    LOG("TEST done: " .. failed .. " failed")
    click(tab.Continue)
    WaitSeconds(1)
    LOG("TEST FAIL Continue did not leave the game")
end)
