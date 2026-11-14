-- The re-test panel (ours; Chris's pick of 2026-11-09: ui/user/retest.h, ui/data/lua/retest.lua, its key action and
-- console command remaster_retest). One file for every case, named after "--" as /retestcase <case>; each run is given
-- a COPY of a list (ui/tests/retest_list.md, or a real list of the lab) as --retest <copy>, because the verdicts file
-- is written beside it and the reader's case changes the copy:
--   reader     (--stage setup --lua, --frames 8): the list as the host reads it (groups, the columns by their names,
--              tables of another kind skipped, marks, Where, the ids), and read again after the file changed.
--   verdicts1  (--stage setup --lua, --frames 8): verdicts and notes written at once, the list's bytes untouched.
--   verdicts2  (the same folder, after the runner took the "Score thing" row out of the list): read back at this
--              start, one changed, the row that left the list kept in the file.
--   panel      (--stage frontend --lua, --frames 900): the window on the main menu (open at a first start), its key,
--              the menu's items, Fail by a click with a note typed, the count, the drop-down, the wheel, a drag of the
--              window, the Options dialog's items.
--   game       (--session <a game> --game-lua, --frames 1500): the window in a game: the game's items, its key, a
--              click on Pass, a note, the game's keys while the note line has no focus, the game running on.
--   round      (--stage frontend --lua --lua-again --game-lua, --frames 4500): the main menu, the lobby, a game
--              launched from it, the main menu again, the game played back as a replay: the window there again each
--              time, its place and its filter kept; closed stays closed; each screen's items.
--   off        (WITHOUT --retest, --stage frontend --lua, --frames 300): nothing of it exists.
--   missing    (--retest <a file that is not there>, --stage frontend --lua, --frames 300): the panel says so.
--   real       (--retest <a copy of the lab's RETEST_LIST_2026-11-04.md or RETEST_REMAINING_2026-11-04.md>,
--              --stage frontend --lua, --frames 400): the engine's real lists as they are written.
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local kKey = 'retest_test'
local state = GetFrontEndData(kKey) or {}
local failed = state.failed or 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local case = (GetCommandLineArg("/retestcase", 1) or {})[1] or "panel"
local inGame = rawget(_G, "SessionIsActive") and SessionIsActive()

local function read(file)
    local f = io.open(file, "rb")
    if not f then return nil end
    local s = f:read("*a")
    f:close()
    return s
end
-- every place of a plain text in another replaced
local function replace(s, a, b)
    local out, at = "", 1
    while true do
        local from, to = string.find(s, a, at, true)
        if not from then break end
        out = out .. string.sub(s, at, from - 1) .. b
        at = to + 1
    end
    return out .. string.sub(s, at)
end
-- the list's item with that first cell (and its group's title)
local function item(list, what)
    for _, group in (list and list.groups) or {} do
        for _, it in group.items do
            if it.what == what then return it, group.title end
        end
    end
    return nil
end
local function titles(list)
    local out = {}
    for _, group in list.groups do table.insert(out, group.title) end
    return table.concat(out, "|")
end
local function counts(list)
    local out, all = {}, 0
    for _, group in list.groups do
        table.insert(out, tostring(table.getn(group.items)))
        all = all + table.getn(group.items)
    end
    return table.concat(out, " "), all
end
local function where(it) return it and table.concat(it.where, ",") end
local function click(x, y)
    moho64ui_MouseMove(x, y)
    WaitFrames(3)
    moho64ui_MouseButton("Left", true, x, y)
    WaitFrames(2)
    moho64ui_MouseButton("Left", false, x, y)
    WaitFrames(3)
end
local function clickOn(c)
    click(math.floor((c.Left() + c.Right()) / 2), math.floor((c.Top() + c.Bottom()) / 2))
end
-- the panel's row of an item, by its first cell
local function rowOf(Panel, what)
    for _, row in Panel.Controls.rows or {} do
        if row.item and row.item.what == what then return row end
    end
end
local function profileKept(key)
    return import("/lua/user/prefs.lua").GetFromCurrentProfile(key)
end

-- --- reader ---
if case == "reader" then
    local list = moho64ui_RetestList()
    check("with --retest there is a list", type(list), 'table')
    check("its file is the one named", string.find(list.file, "retest_list.md", 1, true) ~= nil, true)
    check("the verdicts file is beside it, .verdicts.md for .md", list.verdictsFile, string.sub(list.file, 1, -4) .. ".verdicts.md")
    check("read once, without trouble", tostring(list.serial) .. " " .. tostring(list.error), "1 nil")
    check("the groups: the headings that have items (a row before any heading: a group without a title)", titles(list),
        "|First group (two columns)|Second group (with Where)|Columns in another order")
    local each, all = counts(list)
    check("their items (the keys' sheet, the table of numbers, the table without its rule: skipped)", each .. " = " .. all, "1 4 10 1 = 16")
    local first = list.groups[2].items
    check("the first cell is what, without its marks; an escaped bar is a bar; the same cell twice is told apart",
        first[1].what .. "|" .. first[2].what .. "|" .. first[3].what .. "|" .. first[4].what,
        "Plain item|Item with bold and code marks|A bar | inside a cell|Plain item (2)")
    check("the second cell is how, without its marks", first[2].how, "press Ctrl-Alt-S, then look")
    check("an id: the hash of the heading and the first cell", first[1].id, "b8dfe2cd8c4d4ed0")
    check("the same cell again has its own", first[4].id, "f72da27bf9891225")
    check("a row before any heading too", list.groups[1].items[1].id, "67cf17e6cb65fca8")
    check("no verdict, an empty note", tostring(first[1].verdict) .. "|" .. tostring(first[1].note), "nil|")
    check("Where: one screen", where(item(list, "Menu thing")), "menu")
    check("Where: two", where(item(list, "Lobby or menu thing")), "lobby,menu")
    check("Where: any case of letters", where(item(list, "Game and replay thing")), "game,replay")
    check("Where: empty is everywhere", where(item(list, "Everywhere thing")), "")
    check("Where: a word that is no screen is left out", where(item(list, "A typo in Where")), "")
    check("no Where column: everywhere", where(first[1]), "")
    local four, group = item(list, "Four-column item")
    check("a table of four columns by its names: Item is what, How to see it is how", four and four.how, "by its column names")
    check("under the heading above it (a ### line is no group)", group, "Second group (with Where)")
    check("its other columns are not items", tostring(item(list, "ui")) .. tostring(item(list, "DONE abc1234")), "nilnil")
    check("the columns in another order", (item(list, "Reordered item") or {}).how .. "|" .. tostring(where(item(list, "Reordered item"))), "the how cell|game")
    check("the keys' sheet is skipped", item(list, "Ctrl-Alt-S"), nil)
    check("the table of numbers is skipped", item(list, "4"), nil)
    check("rows without a rule under their first are no table", item(list, "not"), nil)
    -- the file changes: a how cell, a row gone, a new group
    local text = read(list.file)
    check("the test reads the list's file", type(text) == 'string' and string.len(text) > 500, true)
    local eol = string.find(text, "\r\n", 1, true) and "\r\n" or "\n"
    local changed = replace(text, "| Game thing | in a game | game |", "| Game thing | in a game, changed | game |")
    changed = replace(changed, "| Options thing | the Options dialog | options |" .. eol, "")
    changed = changed .. eol .. "## A new group" .. eol .. "| What | How |" .. eol .. "|---|---|" .. eol .. "| New item | new how |" .. eol
    check("the file is written again by the test", moho64ui_WriteFileForTests(list.file, changed), true)
    local again = moho64ui_RetestList()
    check("the changed file is read again", again.serial, 2)
    each, all = counts(again)
    check("a row gone, a group more", each .. " = " .. all, "1 4 9 1 1 = 16")
    check("the changed cell", (item(again, "Game thing") or {}).how, "in a game, changed")
    check("its id as before", (item(again, "Game thing") or {}).id, (item(list, "Game thing") or {}).id)
    check("the row that went", item(again, "Options thing"), nil)
    check("the new group's item", (item(again, "New item") or {}).how, "new how")
    check("an untouched row's id as before", again.groups[2].items[1].id, "b8dfe2cd8c4d4ed0")
    check("no change, no new reading", moho64ui_RetestList().serial, 2)
    LOG("TEST done: " .. failed .. " failed")
    return
end

-- --- the verdicts' file ---
if case == "verdicts1" then
    local list = moho64ui_RetestList()
    local before = read(list.file)
    check("no verdicts file before the first verdict", read(list.verdictsFile), nil)
    local plain, bold, bar, score = item(list, "Plain item"), item(list, "Item with bold and code marks"), item(list, "A bar | inside a cell"), item(list, "Score thing")
    local every, lobby = item(list, "Everywhere thing"), item(list, "Lobby or menu thing")
    check("a verdict is taken", moho64ui_RetestVerdict(plain.id, "pass"), true)
    local file = read(list.verdictsFile)
    check("and written at once", type(file), 'string')
    check("under its heading, as a table", string.find(file or "", "## First group (two columns)\n| Verdict | What | Note | When | Build |\n|---|---|---|---|---|\n| Pass | Plain item |  | ", 1, true) ~= nil, true)
    check("with a note; a bar in it", moho64ui_RetestVerdict(bold.id, "fail", "too dark | at night, **really**"), true)
    check("a row whose first cell has a bar", moho64ui_RetestVerdict(bar.id, "skip", "no time"), true)
    check("a note alone is kept", moho64ui_RetestVerdict(every.id, nil, "only a note"), true)
    check("a verdict for the row the runner takes out of the list", moho64ui_RetestVerdict(score.id, "fail", "old"), true)
    check("a verdict given", moho64ui_RetestVerdict(lobby.id, "skip", "x"), true)
    check("and taken back, its note too", moho64ui_RetestVerdict(lobby.id, nil, ""), true)
    local ok, why = moho64ui_RetestVerdict("0000000000000000", "pass")
    check("an id that is no item's: refused, with a reason", tostring(ok) .. " " .. type(why), "false string")
    ok, why = moho64ui_RetestVerdict(plain.id, "maybe")
    check("a word that is no verdict: refused", tostring(ok) .. " " .. type(why), "false string")
    list = moho64ui_RetestList()
    local it = item(list, "Item with bold and code marks")
    check("the list has them: the verdict and the note", tostring(it.verdict) .. "|" .. tostring(it.note), "fail|too dark | at night, **really**")
    check("the time from the PC's clock", string.find(it.when or "", "^%d%d%d%d%-%d%d%-%d%d %d%d:%d%d$") ~= nil, true)
    check("the build", type(it.build) == 'string' and string.len(it.build) > 3, true)
    check("the note alone: no verdict", tostring(item(list, "Everywhere thing").verdict) .. "|" .. item(list, "Everywhere thing").note, "nil|only a note")
    check("the one taken back: nothing", tostring(item(list, "Lobby or menu thing").verdict) .. "|" .. item(list, "Lobby or menu thing").note, "nil|")
    file = read(list.verdictsFile) or ""
    LOG("TEST note the verdicts file:\n" .. file)
    check("the file: Fail with the note, its bar escaped", string.find(file, "| Fail | Item with bold and code marks | too dark \\| at night, **really** | " .. it.when .. " | " .. it.build .. " |", 1, true) ~= nil, true)
    check("the file: a bar in the first cell escaped", string.find(file, "| Skip | A bar \\| inside a cell | no time | ", 1, true) ~= nil, true)
    check("the file: the note alone under its own heading", string.find(file, "## Second group (with Where)\n| Verdict | What | Note | When | Build |\n|---|---|---|---|---|\n| Fail | Score thing | old | ", 1, true) ~= nil
        and string.find(file, "| - | Everywhere thing | only a note | ", 1, true) ~= nil, true)
    check("the file: no row for the one taken back, none for items without a verdict", string.find(file, "Lobby or menu thing", 1, true) == nil
        and string.find(file, "Menu thing", 1, true) == nil, true)
    local _, rows = string.gsub(file, "\n| [PFS%-]", "")
    check("the file: one row for each of the five", rows, 5)
    check("no temporary file is left", read(list.verdictsFile .. ".tmp"), nil)
    check("the list's file is untouched, byte for byte", read(list.file) == before, true)
    LOG("TEST done: " .. failed .. " failed")
    return
end
if case == "verdicts2" then
    local list = moho64ui_RetestList()
    local before, fileBefore = read(list.file), read(list.verdictsFile) or ""
    check("the runner took a row out of the list", item(list, "Score thing"), nil)
    local plain, bold, bar, every = item(list, "Plain item"), item(list, "Item with bold and code marks"), item(list, "A bar | inside a cell"), item(list, "Everywhere thing")
    check("read back at this start: a verdict", plain.verdict, "pass")
    check("read back: a verdict with its note, bar and marks as typed", tostring(bold.verdict) .. "|" .. tostring(bold.note), "fail|too dark | at night, **really**")
    check("read back: by a first cell with a bar", tostring(bar.verdict) .. "|" .. tostring(bar.note), "skip|no time")
    check("read back: the note alone", tostring(every.verdict) .. "|" .. tostring(every.note), "nil|only a note")
    check("read back: when and by which build", string.find(fileBefore, "| " .. tostring(bold.when) .. " | " .. tostring(bold.build) .. " |", 1, true) ~= nil, true)
    check("the same verdict said again changes nothing", moho64ui_RetestVerdict(bold.id, "fail", "too dark | at night, **really**"), true)
    check("the file as it was", read(list.verdictsFile) == fileBefore, true)
    check("a verdict changed", moho64ui_RetestVerdict(plain.id, "fail", "changed my mind"), true)
    local file = read(list.verdictsFile) or ""
    LOG("TEST note the verdicts file:\n" .. file)
    check("the file has the new one", string.find(file, "| Fail | Plain item | changed my mind | ", 1, true) ~= nil, true)
    check("and not the old", string.find(file, "| Pass | Plain item |", 1, true), nil)
    check("the others stay as they were read", string.find(file, "| Fail | Item with bold and code marks | too dark \\| at night, **really** | " .. bold.when .. " | " .. bold.build .. " |", 1, true) ~= nil, true)
    check("the verdict of the row that left the list is kept, under its heading", string.find(file, "| Fail | Score thing | old | ", 1, true) ~= nil
        and (string.find(file, "## Second group (with Where)", 1, true) or 1e9) < (string.find(file, "| Fail | Score thing | old | ", 1, true) or 0), true)
    local _, rows = string.gsub(file, "\n| [PFS%-]", "")
    check("five rows still", rows, 5)
    check("the list's file is untouched, byte for byte", read(list.file) == before, true)
    LOG("TEST done: " .. failed .. " failed")
    return
end

-- --- without --retest ---
if case == "off" then
    ForkThread(function()
        WaitSeconds(1)
        check("no list without --retest", moho64ui_RetestList(), nil)
        local ok, why = moho64ui_RetestVerdict("b8dfe2cd8c4d4ed0", "pass")
        check("no verdict is taken", tostring(ok) .. " " .. type(why), "false string")
        local KeyMapper = import("/lua/keymap/keymapper.lua")
        check("no key action", KeyMapper.GetKeyActions()['remaster_retest'], nil)
        check("no name for one", import("/lua/keymap/keydescriptions.lua").keyDescriptions['remaster_retest'], nil)
        check("its key does nothing", moho64ui_KeyMapAction("Ctrl-Shift-R"), nil)
        check("no key of the map runs it", KeyMapper.GetKeyLookup()['remaster_retest'], nil)
        check("the panel's script was never loaded", __modules["/moho64ui/lua/retest.lua"], nil)
        ConExecute("remaster_retest")
        WaitFrames(3)
        check("the console command is not there (the script still not loaded)", __modules["/moho64ui/lua/retest.lua"], nil)
        local Panel = import("/moho64ui/lua/retest.lua")
        Panel.Restore()
        Panel.Open()
        Panel.Command("on")
        check("asked by a script: still no panel", tostring(Panel.State.open) .. " " .. tostring(Panel.Controls.window), "false nil")
        -- a saved key map that holds the panel's key from a run with --retest: read without it
        local Prefs = import("/lua/user/prefs.lua")
        local map = KeyMapper.GetCurrentKeyMap()
        map['Ctrl-Shift-R'] = 'remaster_retest'
        Prefs.SetToCurrentProfile("UserKeyMap", map)
        check("a saved key map with the panel's key is read without it", KeyMapper.GetUserKeyMap()['Ctrl-Shift-R'], nil)
        check("and with everything else", KeyMapper.GetUserKeyMap()['Ctrl-Shift-P'], 'remaster_palette')
        Prefs.SetToCurrentProfile("UserKeyMap", nil)
        LOG("TEST done: " .. failed .. " failed")
    end)
    return
end

local Panel = import("/moho64ui/lua/retest.lua")
local Palette = import("/moho64ui/lua/palette.lua")

-- --- a list that is not there ---
if case == "missing" then
    ForkThread(function()
        WaitSeconds(1.5)
        local list = moho64ui_RetestList()
        check("the option alone gives a list, with why it is empty", type(list) == 'table' and type(list.error) == 'string' and table.getn(list.groups) == 0, true)
        check("the key action is there", moho64ui_KeyMapAction("Ctrl-Shift-R"), "remaster_retest")
        check("the panel is open and says so", Panel.State.open and Panel.State.message == list.error and Panel.Controls.message ~= nil
            and Panel.Controls.message.Right() <= Panel.Controls.list.Right(), true)
        check("the count", Panel.State.countText, "0 of 0 done")
        LOG("TEST done: " .. failed .. " failed")
    end)
    return
end

-- --- the engine's real lists ---
if case == "real" then
    ForkThread(function()
        WaitSeconds(1.5)
        local list = moho64ui_RetestList()
        local each, all = counts(list)
        LOG("TEST note " .. tostring(list.file) .. ": groups " .. titles(list) .. "; items " .. each .. " = " .. all)
        check("the real list is read", list.error, nil)
        if string.find(list.file, "RETEST_LIST_2026-11-04", 1, true) then
            check("its two tables: the four-column one of section 1, the two-column one of 1b", each, "71 5")
            local it, group = item(list, "Kennel / station drones fly out and assist")
            check("a four-column row: Item is what, How to see it is how (Chat and Status left out)", it and it.how, "build a Kennel near factories at work")
            check("under its heading", string.sub(group or "", 1, 19), "1. Fixes to check (")
            it = item(list, "Ray reflections on with the ocean")
            check("a two-column row: the other column is how (\"Look for\")", it and it.how, "the sea draws normally")
            check("no item from the Chat or Status columns", tostring(item(list, "engine")) .. tostring(item(list, "landed c009f41")), "nilnil")
        else
            check("its two tables", each, "15 10")
            local it = item(list, "T3 radar: no straight seam in the radar ring area")
            check("a row", it and it.how, "a T3 radar")
            it = item(list, "Snow holds more on steep, windward and scoured ground (renderer; --snow-hold 0 for the old look)")
            check("a row with marks in neither cell", it and it.how, "SETONS_R in snow")
        end
        check("every item has an id and a first cell", (function()
            local seen, n = {}, 0
            for _, group in list.groups do
                for _, it in group.items do
                    if string.len(it.id) ~= 16 or it.what == "" or seen[it.id] then return false end
                    seen[it.id] = true
                    n = n + 1
                end
            end
            return n
        end)(), all)
        check("the panel is open on it", Panel.State.open, true)
        check("no Where column: every item on every screen", Panel.State.count.all, all)
        check("its rows: a heading, then items", table.getn(Panel.Controls.rows) >= 3 and Panel.Controls.rows[1].entry.kind == 'group'
            and Panel.Controls.rows[2].entry.kind == 'item', true)
        local list0 = Panel.Controls.list
        local last = Panel.Controls.rows[table.getn(Panel.Controls.rows)]
        check("the long rows are wrapped inside the list", last.Bottom() <= list0.Bottom() + 1 and last.Right() <= list0.Right() + 1, true)
        -- to the end of the list by the scrollbar's own question, and a verdict there
        Panel.Controls.list:ScrollSetTop("Vert", 10000)
        WaitFrames(3)
        local rows = Panel.Controls.rows
        check("scrolled to the last item", rows[table.getn(rows)].entry == Panel.State.entries[table.getn(Panel.State.entries)], true)
        local row = rows[table.getn(rows)]
        clickOn(row.buttons.pass)
        check("a click on Pass there", row.item.verdict, "pass")
        check("the count", Panel.State.countText, "1 of " .. all .. " done")
        check("the real list's own file is a copy: the verdicts are beside the copy", string.find(read(list.verdictsFile) or "", "| Pass | " .. replace(row.item.what, "|", "\\|") .. " |", 1, true) ~= nil, true)
        Panel.Controls.list:ScrollSetTop("Vert", 0)
        LOG("TEST done: " .. failed .. " failed")
    end)
    return
end

-- --- the window in a game ---
if case == "game" and inGame then
    ForkThread(function()
        WaitSeconds(4)
        local c = Panel.Controls
        check("in a game the panel is there (a first start: open)", Panel.State.open and c.window ~= nil and not IsDestroyed(c.window), true)
        check("the screen is the game", Panel.State.screen, "game")
        check("the game's items (and those of every screen)", Panel.State.count.all, 12)
        check("the count's line", c.count:GetText(), "0 of 12 done   (whole list: 0 of 16)")
        check("the game item is among the rows' entries, the menu's is not", (function()
            local game, menu = false, false
            for _, entry in Panel.State.entries do
                if entry.kind == 'item' and entry.item.what == "Game thing" then game = true end
                if entry.kind == 'item' and entry.item.what == "Menu thing" then menu = true end
            end
            return tostring(game) .. " " .. tostring(menu)
        end)(), "true false")
        check("the panel does not pause the game", SessionIsPaused(), false)
        local walked = 0
        GetFrame(0):ApplyFunction(function(control) walked = walked + 1 end)
        LOG("TEST note the look at the screen walks the head's controls once a second while the panel is open: " .. walked .. " controls in this game")
        check("no control has the keyboard focus", c.rows[2].edit:GetCurrentFocusControl(), nil)
        -- the game's keys run while the note line has no focus
        moho64ui_Key("p", "Ctrl Shift")
        check("a key of the key map runs with the panel open (the command palette's)", Palette.State.open, true)
        moho64ui_Key("p", "Ctrl Shift")
        check("and again", Palette.State.open, false)
        -- its own key
        check("Ctrl-Shift-R is the panel's key in a game", moho64ui_KeyMapAction("Ctrl-Shift-R"), "remaster_retest")
        moho64ui_Key("r", "Ctrl Shift")
        check("the key closes it", tostring(Panel.State.open) .. " " .. tostring(Panel.Controls.window), "false nil")
        moho64ui_Key("r", "Ctrl Shift")
        c = Panel.Controls
        check("and opens it", Panel.State.open and c.window ~= nil, true)
        -- a click on Pass, a note typed
        local row = rowOf(Panel, "Plain item")
        check("the first group's first item has a row", row ~= nil, true)
        clickOn(row.buttons.pass)
        check("a click on Pass", row.item.verdict, "pass")
        check("the host has it", (item(moho64ui_RetestList(), "Plain item") or {}).verdict, "pass")
        check("the count", c.count:GetText(), "1 of 12 done   (whole list: 1 of 16)")
        clickOn(row.edit)
        check("a click on the note line gives it the focus", row.edit:GetCurrentFocusControl() == row.edit, true)
        moho64ui_Type("fine in a game")
        moho64ui_Key("p", "Ctrl Shift")
        check("while it is typed in, the key map's keys do not run", Palette.State.open, false)
        local time = GetGameTimeSeconds()
        moho64ui_Key(13)
        check("Enter keeps the note and gives the focus back", tostring(row.edit:GetCurrentFocusControl()) .. "|" .. (item(moho64ui_RetestList(), "Plain item") or {}).note, "nil|fine in a game")
        check("the verdict stays", (item(moho64ui_RetestList(), "Plain item") or {}).verdict, "pass")
        local file = read(moho64ui_RetestList().verdictsFile) or ""
        check("in the file", string.find(file, "| Pass | Plain item | fine in a game | ", 1, true) ~= nil, true)
        WaitSeconds(2)
        check("the game went on meanwhile", GetGameTimeSeconds() > time + 1, true)
        -- a long note is kept whole (the line took 200 characters until 2026-11-13: Chris's notes of that test were cut)
        local long = string.sub(string.rep("a long note of his. ", 30), 1, 599)
        clickOn(row.edit)
        row.edit:SetText("")
        moho64ui_Type(long)
        moho64ui_Key(13)
        local kept = (item(moho64ui_RetestList(), "Plain item") or {}).note or ""
        check("a note of 599 characters is kept whole", string.len(kept) .. " " .. tostring(kept == long), "599 true")
        clickOn(row.edit)
        row.edit:SetText("")
        moho64ui_Type("fine in a game")
        moho64ui_Key(13)
        check("and the short one typed again", (item(moho64ui_RetestList(), "Plain item") or {}).note, "fine in a game")
        check("the panel's face takes the mouse, not the world under it", (function()
            local control = moho64ui_ControlAt(c.list.Left() + 5, c.list.Bottom() - 2)
            while control do
                if control == c.window then return true end
                control = control:GetParent()
            end
            return false
        end)(), true)
        -- the game's end: the score screen holds the input (its AddInputCapture), and the panel still takes clicks
        import("/lua/ui/game/tabs.lua").EndGame()
        local score = import("/lua/ui/dialogs/score.lua")
        local waited = 0
        while not score.scoreScreenActive and waited < 12 do
            WaitSeconds(0.5)
            waited = waited + 0.5
        end
        check("the game is ended and its score screen is up", score.scoreScreenActive, true)
        WaitSeconds(1.5)
        c = Panel.Controls
        check("the panel is still there, and the screen is the score screen", tostring(Panel.State.open) .. " " .. Panel.State.screen, "true score")
        check("the score screen's items", Panel.State.count.all .. " " .. c.screenText:GetText(), "10 You are on: Score screen")
        check("a dialog of the game holds the input", AnyInputCapture(), true)
        c.list:ScrollSetTop("Vert", 7)
        WaitFrames(3)
        local scoreRow = rowOf(Panel, "Score thing")
        check("the score screen's own item has a row", scoreRow ~= nil, true)
        if scoreRow then
            clickOn(scoreRow.buttons.skip)
            check("a click on Skip over the score screen", scoreRow.item.verdict, "skip")
            clickOn(scoreRow.edit)
            moho64ui_Type("seen")
            moho64ui_Key(13)
            check("and a note typed there", (item(moho64ui_RetestList(), "Score thing") or {}).note, "seen")
        end
        -- (left open over the score screen, for a --screenshot)
        LOG("TEST done: " .. failed .. " failed")
    end)
    return
end

-- --- menu, lobby, game, menu ---
if case == "round" then
    local function upvalue(fn, name)
        for i = 1, 200 do
            local n, v = debug.getupvalue(fn, i)
            if not n then break end
            if n == name then return v end
        end
    end
    local REPLAY = "moho64_retest_round"
    local function profile() return import("/lua/user/prefs.lua").GetCurrentProfile().Name end
    if inGame and SessionIsReplay() then
        -- the game played back as a replay from the main menu
        ForkThread(function()
            WaitSeconds(4)
            check("the replay: the panel is there again (it was open on the menu)", Panel.State.open and Panel.Controls.window ~= nil, true)
            check("the screen is the replay", Panel.State.screen, "replay")
            Panel.SetFilter('screen')
            local shown = {}
            for _, entry in Panel.State.entries do
                if entry.kind == 'item' then shown[entry.item.what] = true end
            end
            check("the replay's items: the game-and-replay one, not the game's own", Panel.State.count.all .. " " .. tostring(shown["Game and replay thing"])
                .. " " .. tostring(shown["Game thing"]), "10 true nil")
            check("its key in a replay", moho64ui_KeyMapAction("Ctrl-Shift-R"), "remaster_retest")
            SetFrontEndData(kKey, nil)
            LOG("TEST done: " .. failed .. " failed")
        end)
        return
    end
    if inGame then
        ForkThread(function()
            WaitSeconds(4)
            check("the game from the lobby: the panel is there again", Panel.State.open and Panel.Controls.window ~= nil, true)
            check("with the filter kept", Panel.State.filter, "all")
            check("at the place it was dragged to", Panel.Controls.window and (Panel.Controls.window.Left() .. " " .. Panel.Controls.window.Top()), tostring(state.left) .. " " .. tostring(state.top))
            check("the screen is the game", Panel.State.screen, "game")
            check("the verdict of the menu is shown", (rowOf(Panel, "Plain item") or { item = {} }).item.verdict, "fail")
            moho64ui_Key("r", "Ctrl Shift")
            check("closed by its key", Panel.State.open, false)
            CopyCurrentReplay(profile(), REPLAY)
            local kept = GetSpecialFileInfo(profile(), REPLAY, "Replay")
            check("(the game is kept as a replay, for the last part)", kept and kept.SizeBytes > 100, true)
            SetFrontEndData(kKey, { failed = failed, left = state.left, top = state.top, phase = 2 })
            LOG("TEST note: the game is left for the menu (" .. failed .. " failed so far)")
            ExitGame()
        end)
        return
    end
    ForkThread(function()
        WaitSeconds(2)
        if state.phase == 2 then
            check("the main menu after the game: closed stays closed", tostring(Panel.State.open) .. " " .. tostring(Panel.Controls.window), "false nil")
            moho64ui_Key("r", "Ctrl Shift")
            check("its key opens it", Panel.State.open and Panel.Controls.window ~= nil, true)
            check("the filter still kept", Panel.State.filter, "all")
            check("the place still kept", Panel.Controls.window and (Panel.Controls.window.Left() .. " " .. Panel.Controls.window.Top()), tostring(state.left) .. " " .. tostring(state.top))
            check("the screen is the main menu", Panel.State.screen, "menu")
            -- the kept game played back: the replay's screen (the front end's part ends with its start)
            SetFrontEndData(kKey, { failed = failed, phase = 3 })
            if not LaunchReplaySession(GetSpecialFilePath(profile(), REPLAY, "Replay")) then
                check("LaunchReplaySession starts the replay", false, true)
                LOG("TEST done: " .. failed .. " failed (in the front end)")
            end
            return
        end
        check("the main menu: the panel is open (a first start)", Panel.State.open and Panel.Controls.window ~= nil, true)
        check("the screen is the main menu", Panel.State.screen, "menu")
        clickOn(rowOf(Panel, "Plain item").buttons.fail)
        Panel.SetFilter('all')
        -- the window dragged by its title bar
        local window = Panel.Controls.window
        local x, y = window.Left() + 120, window.Top() + 14
        moho64ui_MouseMove(x, y)
        WaitFrames(2)
        moho64ui_MouseButton("Left", true, x, y)
        WaitFrames(2)
        moho64ui_MouseMove(x - 150, y + 20)
        WaitFrames(2)
        moho64ui_MouseMove(x - 300, y + 40)
        WaitFrames(2)
        moho64ui_MouseButton("Left", false, x - 300, y + 40)
        WaitFrames(3)
        local left, top = window.Left(), window.Top()
        check("dragged by its title bar", left .. " " .. top, (x - 120 - 300) .. " " .. (y - 14 + 40))
        -- the skirmish lobby by the main menu's own Skirmish (its first-time question answered before), an AI,
        -- Launch Game
        import("/lua/user/prefs.lua").SetToCurrentProfile("MenuTutorialPrompt", true)
        import("/lua/ui/menus/main.lua").ButtonSkirmish()
        local lobby = import('/lua/ui/lobby/lobby.lua')
        local GUI
        for i = 1, 600 do
            GUI = upvalue(lobby.SetRuleTitleText, 'GUI')
            if type(GUI) == 'table' and GUI.uiCreated and GUI.slots then break end
            WaitFrames(1)
        end
        WaitSeconds(2.5)
        check("the lobby is up", type(GUI) == 'table' and GUI.slots ~= nil, true)
        local changelog = import('/lua/ui/lobby/changelog/changelogdialog.lua')
        if changelog.IsOpen() then changelog.CloseChangelogDialog() WaitFrames(3) end
        check("the panel is still there, over the lobby", Panel.State.open and not IsDestroyed(window) and window.Depth() > GUI.Depth(), true)
        check("the screen is the lobby", Panel.State.screen, "lobby")
        Panel.SetFilter('screen')
        check("the lobby's items", Panel.State.count.all, 10)
        Panel.SetFilter('all')
        -- (the lobby keeps the keyboard on its chat line; a click on a note line still gets it)
        local lobbyRow = rowOf(Panel, "Plain item")
        clickOn(lobbyRow.edit)
        check("in the lobby a click gives the note line the focus", lobbyRow.edit:GetCurrentFocusControl() == lobbyRow.edit, true)
        moho64ui_Type("typed in the lobby")
        moho64ui_Key(13)
        check("a note typed in the lobby is kept", (item(moho64ui_RetestList(), "Plain item") or {}).note, "typed in the lobby")
        -- (an AI into slot 2 through its drop-down, as the lobby's own tests pick it)
        local combo = GUI.slots[2].name
        clickOn(combo)
        local picked = false
        for r = 0, combo._list:GetItemCount() - 1 do
            if combo._list:GetItem(r) == "AI: Easy" then
                local h = combo._list:GetRowHeight()
                local _, _, topRow = string.find(moho64ui_Describe(combo._list), "top (%d+)")
                click(combo._list.Left() + 20, combo._list.Top() + (r - (tonumber(topRow) or 0)) * h + math.floor(h / 2))
                picked = true
                break
            end
        end
        check("an AI picked into slot 2", picked, true)
        SetFrontEndData(kKey, { failed = failed, left = left, top = top, phase = 1 })
        clickOn(GUI.launchGameButton)
        WaitSeconds(20)
        check("Launch Game starts the game", false, true)
        LOG("TEST done: " .. failed .. " failed (in the front end)")
    end)
    return
end

-- --- the window on the main menu ---
ForkThread(function()
    WaitSeconds(2)
    local c = Panel.Controls
    check("at a first start with --retest the panel is open on the main menu", Panel.State.open and c.window ~= nil and not IsDestroyed(c.window), true)
    check("kept in the profile", (profileKept('remaster_retest') or {}).open, true)
    check("it is the game's own window, under the head", c.window:GetParent() == GetFrame(0) and c.window.GetClientGroup ~= nil, true)
    check("inside the screen, 500 wide at scale 1", c.window.Right() <= GetFrame(0).Right() and c.window.Width() == import("/lua/maui/layouthelpers.lua").ScaleNumber(500), true)
    check("no message where the rows are", tostring(Panel.State.message) .. " " .. tostring(c.message), "false false")
    -- its key action
    local KeyMapper = import("/lua/keymap/keymapper.lua")
    check("Ctrl-Shift-R is its key", moho64ui_KeyMapAction("Ctrl-Shift-R"), "remaster_retest")
    check("a key action with its name in the key binding screen", KeyMapper.GetActionName('remaster_retest'), "Re-test list (open / close)")
    check("the panel names its key", c.hint:GetText(), "Ctrl + Shift + R hides it")
    moho64ui_Key("r", "Ctrl Shift")
    check("the key closes it", tostring(Panel.State.open) .. " " .. tostring(Panel.Controls.window), "false nil")
    check("closed is kept in the profile", (profileKept('remaster_retest') or {}).open, false)
    moho64ui_Key("r", "Ctrl Shift")
    c = Panel.Controls
    check("and opens it", Panel.State.open and c.window ~= nil, true)
    ConExecute("remaster_retest off")
    check("the console's remaster_retest off closes it", Panel.State.open, false)
    ConExecute("remaster_retest")
    c = Panel.Controls
    check("and without a word turns it over", Panel.State.open, true)
    -- the menu's items
    check("the screen is the main menu", Panel.State.screen, "menu")
    check("the drop-down shows this screen's", Panel.State.filter .. " " .. tostring(c.combo:GetItem()), "screen 1")
    check("the menu's items (and those of every screen)", Panel.State.count.all, 11)
    check("the count's line", c.count:GetText(), "0 of 11 done   (whole list: 0 of 16)")
    check("the screen is named", c.screenText:GetText(), "You are on: Main menu")
    local rows = c.rows
    check("the rows: a heading, then its items", rows[1].entry.kind .. " " .. rows[1].title:GetText() .. " | " .. rows[2].item.what .. " | " .. rows[3].title:GetText()
        .. " | " .. rows[4].item.what, "group (no heading) | A row before any heading | First group (two columns) | Plain item")
    local shown = {}
    for _, entry in Panel.State.entries do
        if entry.kind == 'item' then shown[entry.item.what] = true end
    end
    check("the menu's own item, the lobby-or-menu one, not the game's, not the Options'", tostring(shown["Menu thing"]) .. tostring(shown["Lobby or menu thing"])
        .. tostring(shown["Game thing"]) .. tostring(shown["Options thing"]), "truetruenilnil")
    local last = rows[table.getn(rows)]
    check("the rows end inside the list", last.Bottom() <= c.list.Bottom() + 1, true)
    check("more entries than fit: the scrollbar's range", (function()
        local a, b, top, bottom = c.list:GetScrollValues("Vert")
        return a .. " " .. b .. " " .. top .. " " .. (bottom == table.getn(rows) and "rows" or bottom)
    end)(), "0 " .. table.getn(Panel.State.entries) .. " 0 rows")
    -- Fail by a click, with a note typed first
    local row = rowOf(Panel, "Plain item")
    check("no control has the keyboard focus", row.edit:GetCurrentFocusControl(), nil)
    clickOn(row.edit)
    check("a click on the note line gives it the focus", row.edit:GetCurrentFocusControl() == row.edit, true)
    moho64ui_Type("too dark")
    moho64ui_Key("p", "Ctrl Shift")
    check("while it is typed in, the key map's keys do not run", Palette.State.open, false)
    clickOn(row.buttons.fail)
    check("a click on Fail", row.item.verdict, "fail")
    check("the click took the focus from the note line", row.edit:GetCurrentFocusControl(), nil)
    local list = moho64ui_RetestList()
    check("the host has the verdict and the note", tostring((item(list, "Plain item") or {}).verdict) .. "|" .. tostring((item(list, "Plain item") or {}).note), "fail|too dark")
    check("the count", c.count:GetText(), "1 of 11 done, 1 failed   (whole list: 1 of 16)")
    check("the row shows it", row.verdictText:GetText(), "FAIL")
    check("the file", string.find(read(list.verdictsFile) or "", "| Fail | Plain item | too dark | ", 1, true) ~= nil, true)
    moho64ui_Key("p", "Ctrl Shift")
    check("the keys are the key map's again", Palette.State.open, true)
    moho64ui_Key("p", "Ctrl Shift")
    check("(the palette closed again)", Palette.State.open, false)
    -- the note changed and kept by Enter; Escape gives the kept one back
    clickOn(row.edit)
    moho64ui_Type(", at night")
    moho64ui_Key(13)
    check("Enter keeps the longer note, the verdict stays", tostring((item(moho64ui_RetestList(), "Plain item") or {}).verdict) .. "|" .. (item(moho64ui_RetestList(), "Plain item") or {}).note, "fail|too dark, at night")
    check("and gives the focus back", row.edit:GetCurrentFocusControl(), nil)
    clickOn(row.edit)
    moho64ui_Type(" xxx")
    moho64ui_Key(27)
    check("Escape: the note as it was kept", row.edit:GetText() .. "|" .. (item(moho64ui_RetestList(), "Plain item") or {}).note, "too dark, at night|too dark, at night")
    -- changed, taken back
    clickOn(row.buttons.skip)
    check("the verdict changed by another button", tostring(row.item.verdict) .. " " .. c.count:GetText(), "skip 1 of 11 done   (whole list: 1 of 16)")
    clickOn(row.buttons.skip)
    check("the lit button again takes it back; the note stays", tostring(row.item.verdict) .. "|" .. row.item.note .. "|" .. c.count:GetText(), "nil|too dark, at night|0 of 11 done   (whole list: 0 of 16)")
    check("the file keeps the note alone", string.find(read(list.verdictsFile) or "", "| - | Plain item | too dark, at night | ", 1, true) ~= nil, true)
    clickOn(row.buttons.pass)
    clickOn(rowOf(Panel, "Item with bold and code marks").buttons.fail)
    check("two verdicts: the count", c.count:GetText(), "2 of 11 done, 1 failed   (whole list: 2 of 16)")
    -- the wheel scrolls the list
    moho64ui_MouseWheel(c.list.Left() + 40, c.list.Top() + 40, -1)
    WaitFrames(2)
    check("the wheel scrolls by an entry", Panel.State.top .. " " .. c.rows[1].item.what, "1 A row before any heading")
    moho64ui_MouseWheel(c.list.Left() + 40, c.list.Top() + 40, 1)
    WaitFrames(2)
    check("and back", Panel.State.top, 0)
    -- the drop-down, by clicks
    clickOn(c.combo)
    WaitFrames(3)
    local h = c.combo._list:GetRowHeight()
    click(c.combo._list.Left() + 20, c.combo._list.Top() + h + math.floor(h / 2))
    check("the drop-down's second row: all screens", Panel.State.filter .. " " .. Panel.State.count.all, "all 16")
    check("the count's line", c.count:GetText(), "2 of 16 done, 1 failed")
    check("kept in the profile", (profileKept('remaster_retest') or {}).filter, "all")
    Panel.SetFilter('game')
    check("another screen's items: the game's", Panel.State.count.all .. " " .. tostring(rowOf(Panel, "Plain item") ~= nil), "12 true")
    Panel.SetFilter('screen')
    -- the window dragged by its title bar; its place kept in the profile
    local window = c.window
    local x, y = window.Left() + 120, window.Top() + 14
    moho64ui_MouseMove(x, y)
    WaitFrames(2)
    moho64ui_MouseButton("Left", true, x, y)
    WaitFrames(2)
    moho64ui_MouseMove(x - 200, y + 30)
    WaitFrames(2)
    moho64ui_MouseMove(x - 400, y + 60)
    WaitFrames(2)
    moho64ui_MouseButton("Left", false, x - 400, y + 60)
    WaitFrames(3)
    check("dragged by its title bar", window.Left() .. " " .. window.Top(), (x - 120 - 400) .. " " .. (y - 14 + 60))
    check("its place is kept in the profile", (profileKept('remaster_retest_window') or {}).left, window.Left())
    local left, top = window.Left(), window.Top()
    moho64ui_Key("r", "Ctrl Shift")
    moho64ui_Key("r", "Ctrl Shift")
    c = Panel.Controls
    check("closed and opened: at the kept place, the verdicts shown", c.window.Left() .. " " .. c.window.Top() .. " " .. tostring(rowOf(Panel, "Plain item").item.verdict),
        left .. " " .. top .. " pass")
    -- sized by its lower right corner (the Window's own): the rows laid again, the size kept in the profile
    local rowsBefore, widthBefore, heightBefore = table.getn(c.rows), c.window.Width(), c.window.Height()
    local cx, cy = c.window.Right() - 3, c.window.Bottom() - 3
    moho64ui_MouseMove(cx, cy)
    WaitFrames(2)
    moho64ui_MouseButton("Left", true, cx, cy)
    WaitFrames(2)
    moho64ui_MouseMove(cx + 40, cy + 100)
    WaitFrames(2)
    moho64ui_MouseMove(cx + 80, cy + 200)
    WaitFrames(2)
    moho64ui_MouseButton("Left", false, cx + 80, cy + 200)
    WaitFrames(3)
    check("sized by its corner: its edges follow the mouse", c.window.Right() .. " " .. c.window.Bottom(), (cx + 80) .. " " .. (cy + 200))
    local widthAfter, heightAfter = c.window.Width(), c.window.Height()
    check("wider and taller", widthAfter > widthBefore + 70 and heightAfter > heightBefore + 190, true)
    check("more rows fit, all inside the list", table.getn(c.rows) > rowsBefore and c.rows[table.getn(c.rows)].Bottom() <= c.list.Bottom() + 1
        and c.rows[1].Right() <= c.list.Right() + 1, true)
    local keptSize = profileKept('remaster_retest_window') or {}
    check("the size is kept in the profile (at scale 1)", tostring(keptSize.width) .. " " .. tostring(keptSize.height),
        import("/lua/maui/layouthelpers.lua").InvScaleNumber(c.window.Width()) .. " " .. import("/lua/maui/layouthelpers.lua").InvScaleNumber(c.window.Height()))
    moho64ui_Key("r", "Ctrl Shift")
    moho64ui_Key("r", "Ctrl Shift")
    c = Panel.Controls
    check("closed and opened: of the kept size", c.window.Width() .. " " .. c.window.Height(), widthAfter .. " " .. heightAfter)
    -- the Options dialog: its items; the panel stays above it and takes clicks
    import("/lua/ui/dialogs/options.lua").CreateDialog(GetFrame(0))
    WaitSeconds(2.5)
    check("the Options dialog open: the screen is the options", Panel.State.screen, "options")
    check("its items", Panel.State.count.all .. " " .. c.screenText:GetText(), "10 You are on: Options")
    Panel.Controls.list:ScrollSetTop("Vert", 7)
    WaitFrames(3)
    local options = rowOf(Panel, "Options thing")
    check("the Options' own item has a row", options ~= nil, true)
    clickOn(options.buttons.pass)
    check("a click on it over the open dialog", options.item.verdict, "pass")
    -- the window's own close button
    clickOn(c.window._closeBtn)
    check("the window's X closes it", tostring(Panel.State.open) .. " " .. tostring(Panel.Controls.window), "false nil")
    moho64ui_Key("r", "Ctrl Shift")
    check("(open again, for a --screenshot)", Panel.State.open, true)
    LOG("TEST done: " .. failed .. " failed")
end)
