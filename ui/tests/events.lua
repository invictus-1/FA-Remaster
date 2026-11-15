-- The interface's last events (ours: ui/user/events.h, the report bundle's first part, Chris 2026-11-14: logs
-- "extremely detailed so we don't have to keep coming back and forth"): a ring of what the player did last, written
-- into the log by remaster_events [n] and at the program's end. Checked here, in a game: the game's begin, a mouse
-- press with the control it landed on, a key that ran a key-map action (one event, not a second "console" one), a
-- console command, the same command again counted on one entry, an order with its type and its units, and that
-- remaster_events runs. (The log's own lines are read by the suite's job: events.sh.)
-- Run it like game.lua: a launched game with cheats (session_ai_cheats.lua) at 1920x1080, --frames 1500.
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
-- the newest event of a kind, or nil
local function newest(kind)
    local list = moho64ui_Events() or {}
    for i = table.getn(list), 1, -1 do
        if list[i].kind == kind then return list[i] end
    end
    return nil
end
local function count(kind)
    local n = 0
    for _, e in moho64ui_Events() or {} do
        if e.kind == kind then n = n + 1 end
    end
    return n
end
ForkThread(function()
    WaitSeconds(6)
    check("the host keeps the interface's last events", moho64ui_Events ~= nil, true)
    local screen = newest("screen")
    check("the game's begin is among them", screen and screen.text, "a game begun")

    -- a mouse press: where, and on which control
    moho64ui_MouseMove(300, 500)
    moho64ui_MouseButton("Left", true, 300, 500)
    moho64ui_MouseButton("Left", false, 300, 500)
    local click = newest("click")
    check("a left press is noted with its place", click ~= nil and string.find(click.text, "left press at 300,500 (head 0) on ", 1, true) == 1, true)
    LOG("TEST note the press: " .. tostring(click and click.text))
    check("with the control it landed on (its class)", click ~= nil and string.find(click.text, " on nothing", 1, true) == nil, true)

    -- a key that runs a key-map action: the palette's key opens it, and closes it again
    local action = moho64ui_KeyMapAction("Ctrl-Shift-P")
    check("Ctrl-Shift-P runs the command palette (the key the check uses)", type(action) == "string" and string.find(string.lower(action), "remaster_palette", 1, true) ~= nil, true)
    local consoles = count("console")
    moho64ui_Key("p", "Ctrl Shift")
    WaitSeconds(0.3)
    local key = newest("key")
    check("the key is noted with its action", key and key.text, "Ctrl-Shift-P -> " .. tostring(action))
    check("and not a second time as a console command", count("console"), consoles)
    moho64ui_Key("p", "Ctrl Shift")  -- (closes the palette again: its search line has the focus, the key still runs it)
    WaitSeconds(0.3)

    -- a console command, and the same one twice more: one entry, counted
    ConExecute("remaster_events 1")
    local console = newest("console")
    check("a console command is noted", console and console.text, "remaster_events 1")
    ConExecute("remaster_events 1")
    ConExecute("remaster_events 1")
    console = newest("console")
    check("the same again right after is counted on that entry", console and console.times, 3)

    -- an order: Stop for the commander
    local cdr = GetArmyAvatars()[1]
    SelectUnits({cdr})
    WaitSeconds(0.2)
    IssueCommand("Stop")
    local order = newest("order")
    check("an order is noted with its type and its units", order and order.text, "Stop to 1 unit(s), queue cleared")
    SelectUnits(nil)

    -- the ring keeps its order: times only grow
    local list = moho64ui_Events() or {}
    local ordered = true
    for i = 2, table.getn(list) do
        if list[i].seconds < list[i - 1].seconds then ordered = false end
    end
    check("the events are in the order they came", ordered, true)
    check("no more than 400 are kept", table.getn(list) <= 400, true)
    LOG("TEST done: " .. failed .. " failed")
end)
