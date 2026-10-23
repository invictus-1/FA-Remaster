-- The command palette (ours: ui/data/lua/palette.lua; its key action and console command remaster_palette,
-- ui/user/keymap.cpp). One file for both places:
--   the front end (moho64ui --init <init.lua> --prefs <a copy> --stage setup --size 1920x1080 --lua
--     ui/tests/palette.lua --frames 400): opened by its key (Ctrl-Shift-P, not while a text box has the focus), typing filters
--     the console's names, the key actions and the options, best first, at most 12 rows; Up and Down choose; a console
--     variable runs (its line stays in the palette), a line typed with words, a command that prints nothing (the
--     palette closes), a key action (as its key runs it), an option (the Options dialog at its tab); Tab completes;
--     Escape, its own key, a click outside and the console's remaster_palette off close it.
--   a game (--session session_ai_cheats.lua --seed 7 --game-lua ui/tests/palette.lua --frames 1500): opened by its
--     key, a key action found and run from it (Pause: the game pauses, and again: it goes on).
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end

local Palette = import("/moho64ui/lua/palette.lua")
local ESC, ENTER, UP, DOWN, TAB = 27, 13, 317, 319, 9

local function names(rows, n)
    local out = {}
    for i, e in rows do
        if i > (n or 12) then break end
        table.insert(out, e.name)
    end
    return table.concat(out, "|")
end
-- the palette's search line set to a text, as typed (the edit field's own OnTextChanged runs)
local function query(text)
    Palette.Controls.edit:SetText(text)
end
local function first() return Palette.State.rows[1] or {} end

if rawget(_G, "SessionIsActive") and SessionIsActive() then
    ForkThread(function()
        WaitSeconds(3)
        check("in a game: Ctrl-Shift-P is the palette's key", moho64ui_KeyMapAction("Ctrl-Shift-P"), "remaster_palette")
        moho64ui_Key("p", "Ctrl Shift")
        check("Ctrl-Shift-P opens it in a game", Palette.State.open, true)
        query("toggles game pause")
        local e = first()
        check("a key action found by its name, with its key", tostring(e.kind) .. " " .. tostring(e.name) .. " " .. tostring(e.keyText),
            "action Toggles Game Pause Pause")
        check("the game runs", SessionIsPaused(), false)
        moho64ui_Key(ENTER)
        check("Enter on a key action closes the palette", Palette.State.open, false)
        WaitSeconds(1)
        check("the key action ran as its key runs it: the game is paused", SessionIsPaused(), true)
        moho64ui_Key("p", "Ctrl Shift")
        query("pause")
        check("Pause again among the first rows", string.find(names(Palette.State.rows, 3), "Toggles Game Pause", 1, true) ~= nil, true)
        for i, row in Palette.State.rows do
            if row.name == "Toggles Game Pause" then Palette.State.selected = i end
        end
        moho64ui_Key(ENTER)
        WaitSeconds(1)
        check("run again: the game goes on", SessionIsPaused(), false)
        -- left open with an action's key shown (for a --screenshot)
        moho64ui_Key("p", "Ctrl Shift")
        query("life bars")
        check("left open: Toggle life bars with its key", tostring(first().name) .. " " .. tostring(first().keyText), "Toggle life bars Alt + L")
        LOG("TEST done: " .. failed .. " failed")
    end)
    return
end

ForkThread(function()
    WaitSeconds(0.5)
    -- the key action
    check("Ctrl-Shift-P is the palette's key", moho64ui_KeyMapAction("Ctrl-Shift-P"), "remaster_palette")
    check("Ctrl-K stays Self-Destruct", string.find(moho64ui_KeyMapAction("Ctrl-K") or "", "ConfirmUnitDestruction", 1, true) ~= nil, true)
    local KeyMapper = import("/lua/keymap/keymapper.lua")
    check("a key action of the game's key map, with its name", KeyMapper.GetActionName('remaster_palette'),
        "Command palette (search commands, key actions and options)")
    check("remaster_palette is among the console's names", table.getn(ConTextMatches("remaster_pa")), 1)

    -- not while a text box has the focus
    local Edit = import("/lua/maui/edit.lua").Edit
    local other = Edit(GetFrame(0), "otherEdit")
    other:SetFont("Arial", 14)
    other.Left:Set(100) other.Top:Set(100) other.Width:Set(300) other.Height:Set(24)
    other:AcquireFocus()
    moho64ui_Key("p", "Ctrl Shift")
    check("not while another text box has the focus", Palette.State.open, false)
    other:AbandonFocus()
    other:Destroy()

    -- opened by its key
    moho64ui_Key("p", "Ctrl Shift")
    check("Ctrl-Shift-P opens it", Palette.State.open, true)
    local c = Palette.Controls
    check("its search line has the keyboard focus", c.edit and c.edit:GetCurrentFocusControl() == c.edit, true)
    check("nothing typed: the placeholder, no rows", tostring(c.placeholder:IsHidden()) .. " " .. table.getn(Palette.State.rows), "false 0")
    check("everything is searched: the console's names, the key actions, the options",
        table.getn(Palette.State.entries) > 435 + 300 + 50, true)

    -- typing filters, best first
    moho64ui_Type("ren_blo")
    check("typed into its search line", Palette.State.query, "ren_blo")
    local e = first()
    check("a console variable by its beginning, with its value", tostring(e.kind) .. " " .. tostring(e.name) .. " " .. tostring(e.varKind),
        "console ren_Bloom bool")
    query("a")
    check("at most 12 rows", table.getn(Palette.State.rows), 12)
    check("more match than are shown", Palette.State.count > 12, true)
    check("the rows' controls: 12, all shown", c.rowGroups[12]:IsHidden(), false)
    query("life bars")
    check("each word at a word's beginning: the key action first", tostring(first().kind) .. " " .. tostring(first().name),
        "action Toggle life bars")
    check("its key beside it (the key binding screen's name)", first().keyText, "Alt + L")
    query("lifebars")
    check("without the blanks too, and by the action's own id", string.find(names(Palette.State.rows, 3), "Toggle life bars", 1, true) ~= nil, true)
    query("wld_gamespeed")
    check("the whole name first", first().name, "WLD_GameSpeed")
    query("vertical sync")
    e = first()
    check("an option with its tab", tostring(e.kind) .. " " .. tostring(e.tab), "option Video")
    query("shadow")
    local options = 0
    for _, row in Palette.State.rows do if row.kind == 'option' then options = options + 1 end end
    check("options among a word's matches", options >= 2, true)
    query("zzqxv")
    check("nothing matches: no rows, its line", tostring(table.getn(Palette.State.rows)) .. " " .. c.empty:GetText(), "0 Nothing matches")

    -- Up and Down
    query("remaster_p")
    local n = table.getn(Palette.State.rows)
    check("several rows", n >= 4, true)
    moho64ui_Key(DOWN)
    moho64ui_Key(DOWN)
    check("Down moves down", Palette.State.selected, 3)
    moho64ui_Key(UP)
    check("Up moves up", Palette.State.selected, 2)
    moho64ui_Key(UP)
    moho64ui_Key(UP)
    check("Up from the first goes round to the last", Palette.State.selected, n)

    -- Tab completes a console name
    query("wld_gamesp")
    moho64ui_Key(TAB)
    check("Tab completes the console name, a blank after it", c.edit:GetText(), "WLD_GameSpeed ")

    -- a console variable: run as the console runs it; what it says stays in the palette
    local bloom = moho64ui_Looks().bloom
    query("ren_bloom")
    moho64ui_Key(ENTER)
    check("Enter on a switch turns it over", moho64ui_Looks().bloom, not bloom)
    check("the palette stays open with the console's line", tostring(Palette.State.open) .. " " .. Palette.State.result,
        "true toggled ren_Bloom is now " .. (bloom and "off" or "on"))
    check("the row shows the new value", first().value, bloom and "off" or "on")
    moho64ui_Key(ENTER)
    check("again: back as it was", moho64ui_Looks().bloom, bloom)
    query("cam_ZoomAmount")
    moho64ui_Key(ENTER)
    check("a variable alone says its value", string.sub(Palette.State.result, 1, 21), "float cam_ZoomAmount ")

    -- a line typed with words after a console name runs as typed
    query("CON_Echo hello palette")
    e = first()
    check("the typed line comes first", tostring(e.kind) .. " " .. tostring(e.name), "line CON_Echo hello palette")
    moho64ui_Key(ENTER)
    check("it ran: the console's line", Palette.State.result, "hello palette")
    -- a command that prints nothing: done, the palette closes
    query("remaster_season autumn")
    moho64ui_Key(ENTER)
    check("a command with words ran", moho64ui_Looks().season, 1)
    check("it printed nothing: the palette closed", Palette.State.open, false)
    ConExecute("remaster_season summer")

    -- a key action: the palette closes and the action runs as its key does
    local bars = moho64ui_LifeBars()
    moho64ui_Key("p", "Ctrl Shift")
    query("toggle life bars")
    moho64ui_Key(ENTER)
    check("Enter on a key action closes the palette", Palette.State.open, false)
    check("and runs the action (UI_RenderUnitBars, Alt-L's)", moho64ui_LifeBars(), not bars)
    ConExecute("UI_RenderUnitBars")
    check("(the life bars as they were)", moho64ui_LifeBars(), bars)

    -- the recently run, while nothing is typed
    moho64ui_Key("p", "Ctrl Shift")
    check("opened again: the recently run first", names(Palette.State.rows, 3), "Toggle life bars|cam_ZoomAmount|ren_Bloom")
    local lines = 0
    for _, row in Palette.State.rows do if row.kind == 'line' then lines = lines + 1 end end
    check("(a line typed with words is not kept)", lines, 0)

    -- closing: Escape, its own key, a click outside, the console command
    moho64ui_Key(ESC)
    check("Escape closes it", Palette.State.open, false)
    check("and gives the keyboard focus back", GetFrame(0):GetCurrentFocusControl() == nil, true)
    moho64ui_Key("p", "Ctrl Shift")
    moho64ui_Type("abc")
    moho64ui_Key("p", "Ctrl Shift")
    check("its own key closes it while its search line has the focus", Palette.State.open, false)
    moho64ui_Key("p", "Ctrl Shift")
    local panel = Palette.Controls.panel
    local px, py = panel.Left() + 30, panel.Top() + 30
    moho64ui_MouseButton("Left", true, px, py)
    moho64ui_MouseButton("Left", false, px, py)
    check("a click on the panel leaves it open", Palette.State.open, true)
    check("and its search line keeps the keyboard focus", Palette.Controls.edit:GetCurrentFocusControl() == Palette.Controls.edit, true)
    query("ren_bloom")
    local row = Palette.Controls.rowGroups[1]
    local before = moho64ui_Looks().bloom
    moho64ui_MouseButton("Left", true, row.Left() + 200, row.Top() + 10)
    moho64ui_MouseButton("Left", false, row.Left() + 200, row.Top() + 10)
    check("a click on a row runs it", moho64ui_Looks().bloom, not before)
    ConExecute("ren_Bloom")
    moho64ui_MouseButton("Left", true, 60, 1000)
    moho64ui_MouseButton("Left", false, 60, 1000)
    check("a click outside closes it", Palette.State.open, false)
    ConExecute("remaster_palette on")
    check("remaster_palette on", Palette.State.open, true)
    ConExecute("remaster_palette")
    check("remaster_palette alone turns it over", Palette.State.open, false)

    -- an option: the Options dialog at its tab
    moho64ui_Key("p", "Ctrl Shift")
    query("vertical sync")
    e = first()
    moho64ui_Key(ENTER)
    check("Enter on an option closes the palette", Palette.State.open, false)
    WaitSeconds(1)
    local video = import("/lua/options/options.lua").options.video
    local tab, cancel = false, false
    GetFrame(0):ApplyFunction(function(control)
        if control.tabData == video and control.OnClick then tab = control end
        if control.cancelBtn then cancel = control.cancelBtn end
    end)
    check("the Options dialog is open", tab ~= false, true)
    check("at the option's tab (the chosen tab's button hides under its lit picture)", tab and tab:IsHidden(), true)
    check("at the option itself", Palette.State.shownOption, e.optionKey)
    if cancel then cancel:OnClick() end

    -- left open with a few letters typed (for a --screenshot)
    moho64ui_Key("p", "Ctrl Shift")
    moho64ui_Type("cam")
    LOG("TEST done: " .. failed .. " failed")
end)
