-- The debug keys that reach the simulation (FAF's debugKeyActions.lua with the GPG preset's debug keys,
-- defaultKeyMap.lua): Ctrl-Alt-B runs BlingBling, an alias of the simulation's (keymap.cpp kSimCommandAliases), so
-- DoSimCommand sends the stream's DebugCommand (20) with the command line, the place under the mouse, the focus army
-- and the selection (user_units.cpp SendSimCommand; FA exe 0x88e440, 0x6e7070); Alt-T runs the interface's own
-- TeleportSelectedUnits: a WarpEntity (10) for each selected unit of the focus army to the place under the mouse
-- (0x833f90), which the simulation carries out with cheats on; Alt-Delete runs DestroySelectedUnits (the order
-- DestroySelf, 31, the queue cleared; 0x833d60), and KillSelectedUnits gives KillSelf (30; 0x833c70). The messages are
-- read as the simulation was handed them (moho64ui_SentMessages); what the simulation does with BlingBling (the
-- engine's) is checked by the army's storage and the console's lines. Run it as a launched game with cheats
-- (session_ai_cheats.lua), the world's picture, --frames 2400.
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end

-- the stream's little-endian numbers and NUL-ended strings
local function byte(s, at) return string.byte(s, at) or 0 end
local function u32(s, at) return byte(s, at) + byte(s, at + 1) * 256 + byte(s, at + 2) * 65536 + byte(s, at + 3) * 16777216 end
local function f32(s, at)
    local b = u32(s, at)
    local sign = 1
    if b >= 2147483648 then sign, b = -1, b - 2147483648 end
    local e, m = math.floor(b / 8388608), math.mod(b, 8388608)
    if e == 0 then return sign * math.ldexp(m, -149) end
    return sign * math.ldexp(1 + m / 8388608, e - 127)
end
local function cstring(s, at)
    local z = string.find(s, "\0", at, true) or (string.len(s) + 1)
    return string.sub(s, at, z - 1), z + 1
end
-- a DebugCommand's fields: {command, x, y, z, army, units = {ids}}
local function debugCommand(m)
    local d = {}
    local at
    d.command, at = cstring(m.payload, 1)
    d.x, d.y, d.z = f32(m.payload, at), f32(m.payload, at + 4), f32(m.payload, at + 8)
    d.army = byte(m.payload, at + 12)
    d.units = {}
    local n = u32(m.payload, at + 13)
    for i = 1, n do table.insert(d.units, u32(m.payload, at + 13 + 4 * i)) end
    d.size = string.len(m.payload) - (at - 1)  -- (what follows the string: 12 + 1 + 4 + 4n)
    return d
end
local function ofType(list, t)
    local out = {}
    for _, m in list do
        if m.type == t then table.insert(out, m) end
    end
    return out
end
local function near(a, b, tolerance) return math.abs(a - b) <= tolerance end

ForkThread(function()
    WaitSeconds(6)
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local cam = GetCamera("WorldCamera")
    local cdr = GetArmyAvatars()[1]
    local cdrId = tonumber(cdr:GetEntityId())
    local p = cdr:GetPosition()
    cam:SnapTo(p, nil, 120)
    WaitSeconds(0.5)
    local function mouseOver(place)
        local s = view:Project(place)
        moho64ui_MouseMove(s.x, s.y)
        WaitSeconds(0.2)
        return GetMouseWorldPos()
    end

    check("Ctrl-Alt-B is FAF's BlingBling (the GPG preset's debug keys)", moho64ui_KeyMapAction("Ctrl-Alt-B"), "BlingBling")
    check("Alt-T is TeleportSelectedUnits", moho64ui_KeyMapAction("Alt-T"), "TeleportSelectedUnits")
    check("Alt-Delete is DestroySelectedUnits", moho64ui_KeyMapAction("Alt-Delete"), "DestroySelectedUnits")

    -- BlingBling: the command line, the place under the mouse, the focus army counted from 0, the selection
    SelectUnits({cdr})
    local mouse = mouseOver({p[1] + 20, p[2], p[3] + 10})
    moho64ui_SentMessages()
    local heard = {}
    local handle = AddConsoleOutputReciever(function(text) table.insert(heard, text) end)
    local before = GetEconomyTotals()
    moho64ui_Key("b", "Ctrl Alt")
    WaitSeconds(0.5)
    RemoveConsoleOutputReciever(handle)
    local after = GetEconomyTotals()
    local sent = ofType(moho64ui_SentMessages(), 20)
    check("Ctrl-Alt-B sends one DebugCommand", table.getn(sent), 1)
    if sent[1] then
        local d = debugCommand(sent[1])
        check("whose command is the alias's own", d.command, "BlingBling")
        check("with the place under the mouse", near(d.x, mouse[1], 0.01) and near(d.y, mouse[2], 0.01) and near(d.z, mouse[3], 0.01), true)
        check("the focus army counted from 0", d.army, GetFocusArmy() - 1)
        check("and the selection's units", table.getn(d.units) .. " " .. tostring(d.units[1]), "1 " .. cdrId)
        check("and nothing after them", d.size, 12 + 1 + 4 + 4 * table.getn(d.units))
    end
    -- what the simulation made of it (the engine's Sim::ExecuteDebugCommand, BlingBling 0x64bbe0): 10000 more of
    -- each storage, filled; its print "<source>: BlingBling" (0x734870) and FAF's "<name> is cheating!" from the
    -- beat's Sync.Cheaters (UserSync 409), both on the console and so on its echo on the screen
    local name = tostring((SessionGetCommandSourceNames() or {})[1])
    check("BlingBling: 10000 more mass storage", near(after.maxStorage.MASS - before.maxStorage.MASS, 10000, 1), true)
    check("and 10000 more energy storage", near(after.maxStorage.ENERGY - before.maxStorage.ENERGY, 10000, 1), true)
    check("filled", near(after.stored.MASS, after.maxStorage.MASS, 1) and near(after.stored.ENERGY, after.maxStorage.ENERGY, 1), true)
    local lines = table.concat(heard, "|")
    check("the simulation's print names the cheater and the command", string.find(lines, name .. ": BlingBling", 1, true) ~= nil, true)
    check("and FAF's line says who is cheating", string.find(lines, name .. " is cheating!", 1, true) ~= nil, true)
    LOG("TEST note: the console heard: " .. lines)

    -- an alias with words after it, a word in quotes, and a command with nothing selected
    SelectUnits(nil)
    WaitSeconds(0.2)
    ConExecute("ScenarioMethod OnF3")
    ConExecute('SimLua "print(\'a b\')"')
    ConExecute("dbg navpath")
    WaitSeconds(0.3)
    sent = ofType(moho64ui_SentMessages(), 20)
    local lines = {}
    for _, m in sent do table.insert(lines, debugCommand(m).command) end
    check("the aliases' words go along, made into a line again (a word with a blank in quotes)", table.concat(lines, " | "),
        "ScenarioMethod OnF3 | SimLua \"print('a b')\" | dbg navpath")
    check("with nothing selected: no units", sent[3] and table.getn(debugCommand(sent[3]).units), 0)

    -- in the order written among the other orders: a DebugCommand, a Stop, a DebugCommand
    SelectUnits({cdr})
    WaitSeconds(0.2)
    moho64ui_SentMessages()
    ConExecute("BlingBling")
    IssueCommand("UNITCOMMAND_Stop")
    ConExecute("SallyShears")
    WaitSeconds(0.3)
    local kinds = {}
    for _, m in moho64ui_SentMessages() do
        if m.type == 1 or m.type == 12 or m.type == 20 then table.insert(kinds, tostring(m.type)) end
    end
    check("one batch, its source first, the messages in the order written", table.concat(kinds, " "), "1 20 12 20")

    -- Alt-T: the commander to the place under the mouse
    local to = mouseOver({p[1] + 40, p[2], p[3] - 30})
    moho64ui_SentMessages()
    moho64ui_Key("t", "Alt")
    WaitSeconds(0.3)
    sent = ofType(moho64ui_SentMessages(), 10)
    check("Alt-T sends a WarpEntity for the selected unit", table.getn(sent), 1)
    if sent[1] then
        local m = sent[1].payload
        check("the commander's", u32(m, 1), cdrId)
        local w, x, y, z = f32(m, 5), f32(m, 9), f32(m, 13), f32(m, 17)
        check("its turn as it is (a unit quaternion)", near(w * w + x * x + y * y + z * z, 1, 0.001), true)
        check("to the place under the mouse", near(f32(m, 21), to[1], 0.01) and near(f32(m, 29), to[3], 0.01), true)
        check("on the ground there (a land unit: the terrain's height)", near(f32(m, 25), to[2], 0.5), true)
        check("28 bytes after the id", string.len(m), 32)
    end
    WaitSeconds(1)
    local q = cdr:GetPosition()
    check("the simulation moved the commander there (cheats are on)", near(q[1], to[1], 1) and near(q[3], to[3], 1), true)

    -- Alt-Delete and KillSelectedUnits: a tank each
    local function tank(dx)
        SimCallback({Func = "CheatSpawnUnit", Args = {army = GetFocusArmy(), pos = {q[1] + dx, q[2], q[3] + 10}, bpId = "uel0201", count = 1, yaw = 0}})
        WaitSeconds(1)
        UISelectionByCategory("LAND MOBILE TECH1 DIRECTFIRE", false, false, false, false)
        local list = GetSelectedUnits() or {}
        return list[1]
    end
    local function lastOrder()
        local orders = ofType(moho64ui_SentMessages(), 12)
        local m = orders[table.getn(orders)]
        if not m then return "none" end
        local n = u32(m.payload, 1)
        local at = 5 + 4 * n       -- after the units: id, coordinated-with id, then the type
        local typ = byte(m.payload, at + 8)
        local clear = byte(m.payload, string.len(m.payload))
        return n .. " " .. typ .. " " .. clear
    end
    local t1 = tank(10)
    check("a tank to destroy", t1 ~= nil, true)
    if t1 then
        SelectUnits({t1})
        WaitSeconds(0.2)
        moho64ui_SentMessages()
        moho64ui_Key(127, "Alt")  -- (Delete)
        WaitSeconds(0.3)
        check("Alt-Delete: an order for the tank, DestroySelf (31), its queue cleared", lastOrder(), "1 31 1")
        WaitSeconds(1)
        check("and the tank is gone", t1:IsDead() or GetUnitById(t1:GetEntityId()) == nil, true)
    end
    local t2 = tank(-10)
    check("a tank to kill", t2 ~= nil, true)
    if t2 then
        SelectUnits({t2})
        WaitSeconds(0.2)
        moho64ui_SentMessages()
        ConExecute("KillSelectedUnits")
        WaitSeconds(0.3)
        check("KillSelectedUnits: KillSelf (30), the queue cleared", lastOrder(), "1 30 1")
        WaitSeconds(1)
        check("and the tank dies", t2:IsDead() or GetUnitById(t2:GetEntityId()) == nil, true)
    end
    LOG("TEST done: " .. failed .. " failed")
end)
