-- The order panel's switches as a player uses them, in a game: the toggle buttons (script bits: shields, intel,
-- stealth, production), the silos' automatic mode (a right click on the build button), the fire states (the
-- button's pop-up and FAF's "[" key), pausing (the construction panel's pause button and FAF's "Z" key) for a
-- factory, an engineer and an upgrade, and a mixed selection's buttons. Each through FAF's own orders.lua and
-- construction.lua (clicks on their buttons), and each checked in the simulation: the unit's state read back and
-- its effect (the energy it asks for, the work it does, the shots it fires).
-- Then the same game is played back as a replay (the second run, from the front end): every own unit's switches at
-- the same ticks ("TEST snapshot" lines, compared by the suite's job).
-- Units made by the game's cheat callback. Run it like orderlines.lua (session_ai_cheats.lua, seed 7) with
-- --frames 6800; then, with the same preferences folder, --stage frontend --lua ui/tests/toggles.lua --game-lua
-- ui/tests/toggles.lua --frames 7000 --no-world plays it back.
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local REPLAY = "moho64_toggles"
-- the ticks at which every own unit's switches are written down, in the game and in its replay
local SNAPSHOTS = {250, 400, 550, 700, 850, 1000}

-- Every own unit (army 1's ids from 0 up) with its switches: script bits, fire state, paused, automatic mode, and
-- its work's progress. Asked the same way in the game and in the replay (an observer there).
local function snapshot()
    local t = {}
    local misses = 0
    for i = 0, 2000 do
        local u = GetUnitById(tostring(i))
        if u and not u:IsDead() then
            misses = 0
            local bits = ""
            for b = 0, 8 do
                bits = bits .. (GetScriptBit({u}, b) and "1" or "0")
            end
            table.insert(t, string.format("%d:%s:b%s:f%d:p%s:a%s:w%.3f", i, u:GetBlueprint().BlueprintId, bits,
                GetFireState({u}), tostring(GetIsPaused({u})), tostring(u:IsAutoMode()), u:GetWorkProgress() or 0))
        else
            misses = misses + 1
            if misses > 200 then break end
        end
    end
    return table.concat(t, " ")
end
local function snapshots()
    ForkThread(function()
        for _, tick in SNAPSHOTS do
            while GameTick() < tick do WaitSeconds(0) end
            LOG("TEST snapshot " .. tick .. " " .. snapshot())
        end
    end)
end

if not rawget(_G, "SessionIsActive") or not SessionIsActive() then
    -- the front end (the second run): the game the first run kept is played back, as the Replay dialog does
    local profile = import("/lua/user/prefs.lua").GetCurrentProfile().Name
    local file = GetSpecialFilePath(profile, REPLAY, "Replay")
    LOG("TEST note the replay: " .. tostring(file))
    check("the kept game's replay starts", LaunchReplaySession(file), true)
    return
end
if SessionIsReplay() then
    snapshots()
    ForkThread(function()
        while GameTick() < SNAPSHOTS[table.getn(SNAPSHOTS)] + 5 do WaitSeconds(0.2) end
        check("the replay reached the last snapshot", GameTick() > SNAPSHOTS[table.getn(SNAPSHOTS)], true)
        -- the switches the game left changed are changed in its replay too
        local count = {}
        local function note(what) count[what] = (count[what] or 0) + 1 end
        for i = 0, 100 do
            local u = GetUnitById(tostring(i))
            local bp = u and u:GetBlueprint().BlueprintId
            if bp == "ueb3101" and GetScriptBit({u}, 3) then note("radar off") end
            if bp == "ueb4202" and not GetScriptBit({u}, 0) then note("shield off") end
            if bp == "ueb1104" and GetScriptBit({u}, 4) then note("fabricator off") end
            if bp == "uel0201" and GetFireState({u}) == 2 then note("tank on hold ground") end
            if bp == "ueb0101" and GetIsPaused({u}) then note("factory paused") end
        end
        check("in the replay: one radar's intel off", count["radar off"], 1)
        check("the shield generator off", count["shield off"], 1)
        check("the fabricator off", count["fabricator off"], 1)
        check("both tanks on hold ground", count["tank on hold ground"], 2)
        check("the factory paused", count["factory paused"], 1)
        LOG("TEST done: " .. failed .. " failed")
    end)
    return
end

local orders = import("/lua/ui/game/orders.lua")
local construction = import("/lua/ui/game/construction.lua")
local CM = import("/lua/ui/game/commandmode.lua")

local function made(bp, category)
    UISelectionByCategory(category, false, false, false, false)
    local found = {}
    for _, u in GetSelectedUnits() or {} do
        if u:GetBlueprint().BlueprintId == bp then table.insert(found, u) end
    end
    SelectUnits(nil)
    table.sort(found, function(a, b) return tonumber(a:GetEntityId()) < tonumber(b:GetEntityId()) end)
    return found
end
local function select(units)
    SelectUnits(units)
    WaitSeconds(0.3)
end
-- the order panel's button for an order or a toggle (orders.lua puts the key in _order)
local function button(key)
    local grid = orders.controls.orderButtonGrid
    local cols, rows = grid:GetDimensions()
    for c = 1, cols do
        for r = 1, rows do
            local b = grid:GetItem(c, r)
            if b and b._order == key then return b end
        end
    end
end
local function clickControl(ctrl, which)
    if not ctrl then return false end
    local x, y = (ctrl.Left() + ctrl.Right()) / 2, (ctrl.Top() + ctrl.Bottom()) / 2
    moho64ui_MouseMove(x, y)
    WaitSeconds(0.1)
    moho64ui_MouseButton(which or "Left", true, x, y)
    moho64ui_MouseButton(which or "Left", false, x, y)
    WaitSeconds(0.2)
    return true
end
-- waits up to `seconds` for test() to be true; its last value
local function waitFor(test, seconds)
    local waited = 0
    while not test() and waited < seconds do
        WaitSeconds(0.1)
        waited = waited + 0.1
    end
    return test()
end
local function bit(u, b) return GetScriptBit({u}, b) end
local function energy(u) return u:GetEconData().energyRequested end
local function progress(u) return u:GetWorkProgress() or 0 end
-- how far `u`'s work goes in `seconds`
local function advance(u, seconds)
    local a = progress(u)
    WaitSeconds(seconds)
    return progress(u) - a
end

-- One toggle button clicked twice: the script bit changed and back, the unit's energy, the button's check. `onIs` is
-- the bit while the unit's ability works: set for a shield (FAF's shields set their toggle's bit when they go up, so
-- the button is checked while the shield is on), clear for the others (the bit set switches them off).
local function toggleCase(name, u, key, b, onIs)
    select({u})
    local btn = button(key)
    check(name .. ": its button " .. key .. " is on the panel", btn ~= nil, true)
    if not btn then return end
    check(name .. ": at first the bit is " .. tostring(onIs), bit(u, b), onIs)
    check(name .. ": and the button shows it", btn:IsChecked(), onIs)
    check(name .. ": it asks for energy", waitFor(function() return energy(u) > 0 end, 10), true)
    local before = energy(u)
    clickControl(btn)
    check(name .. ": a click switches it off (the bit " .. tostring(not onIs) .. ")", waitFor(function() return bit(u, b) ~= onIs end, 2), true)
    check(name .. ": the button shows it", btn:IsChecked(), not onIs)
    check(name .. ": switched off it asks for no energy", waitFor(function() return energy(u) == 0 end, 3), true)
    LOG("TEST note: " .. name .. " asks for " .. before .. " energy on, " .. energy(u) .. " off")
    select(nil)
    select({u})
    btn = button(key)
    check(name .. ": selected again the button shows it (GetScriptBit)", btn and btn:IsChecked(), not onIs)
    clickControl(btn)
    check(name .. ": a second click switches it on", waitFor(function() return bit(u, b) == onIs end, 2), true)
    check(name .. ": on again it asks for its energy", waitFor(function() return energy(u) == before end, 10), true)
    check(name .. ": and the button shows it", btn and btn:IsChecked(), onIs)
end

ForkThread(function()
    WaitSeconds(6)
    snapshots()
    local army = GetFocusArmy()
    local enemy = army == 1 and 2 or 1
    local cdr = GetArmyAvatars()[1]
    local c = cdr:GetPosition()
    local function at(dx, dz) return {c[1] + dx, c[2], c[3] + dz} end
    local spawn = function(bp, pos, who)
        SimCallback({Func = "CheatSpawnUnit", Args = {army = who or army, pos = pos, bpId = bp, count = 1, yaw = 0}})
    end
    SetFireState({cdr}, 1)  -- (hold fire: the enemy's generator stays for the fire states' case)
    spawn("ueb4202", at(-20, 15))   -- T2 shield generator (toggle 0)
    spawn("ueb3101", at(-10, 25))   -- T1 radars (toggle 3)
    spawn("ueb3101", at(-4, 25))
    spawn("ueb3101", at(2, 32))     -- (upgraded later)
    spawn("ueb4203", at(-28, 28))   -- T2 stealth field (toggle 5)
    spawn("ueb1104", at(-35, 10))   -- T2 mass fabricator (toggle 4)
    spawn("ueb4302", at(-15, 40))   -- strategic missile defence (automatic mode)
    spawn("ueb0101", at(-40, -10))  -- land factory
    spawn("uel0105", at(-25, -5))   -- engineer
    spawn("uel0303", at(-25, -25))  -- Titan: a personal shield (toggle 0)
    spawn("ueb1301", at(-50, 30))   -- a T3 power generator: energy to spare (FAF's fabricators work only then)
    spawn("ueb1101", at(16, 12), enemy)  -- the enemy's power generator, for the fire states
    spawn("uel0201", at(6, 10))     -- two tanks within reach of it
    spawn("uel0201", at(6, 15))
    WaitSeconds(2)
    local shield = made("ueb4202", "STRUCTURE")[1]
    local radars = made("ueb3101", "STRUCTURE")
    local stealth = made("ueb4203", "STRUCTURE")[1]
    local fab = made("ueb1104", "STRUCTURE")[1]
    local smd = made("ueb4302", "STRUCTURE")[1]
    local factory = made("ueb0101", "FACTORY")[1]
    local engineer = made("uel0105", "ENGINEER")[1]
    local titan = made("uel0303", "LAND")[1]
    local tanks = made("uel0201", "LAND")
    local all = shield and table.getn(radars) == 3 and stealth and fab and smd and factory and engineer and titan and
        table.getn(tanks) == 2
    check("every unit is made", all and true or false, true)
    if not all then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    SetFireState({titan}, 1)
    SetFireState(tanks, 1)  -- (as a script does it: a number; the fire states' case starts from hold fire)
    local target
    for i = 0, 50 do
        local u = GetUnitById(tostring(1048576 * (enemy - 1) + i))
        if u and u:GetBlueprint().BlueprintId == "ueb1101" then target = u end
    end
    GetCamera("WorldCamera"):SnapTo(at(-10, 10), nil, 140)
    WaitSeconds(0.5)

    -- 1. toggles, each through its button: shield on/off (a shield generator, the Titan's own), intel, stealth,
    -- production
    toggleCase("the shield generator", shield, "RULEUTC_ShieldToggle", 0, true)
    toggleCase("the Titan's personal shield", titan, "RULEUTC_ShieldToggle", 0, true)
    toggleCase("a radar", radars[1], "RULEUTC_IntelToggle", 3, false)
    toggleCase("the stealth field", stealth, "RULEUTC_StealthToggle", 5, false)
    toggleCase("the mass fabricator", fab, "RULEUTC_ProductionToggle", 4, false)
    -- the fabricator's mass: none while its production is off
    select({fab})
    clickControl(button("RULEUTC_ProductionToggle"))
    waitFor(function() return bit(fab, 4) end, 2)
    check("the fabricator switched off makes no mass", waitFor(function() return fab:GetEconData().massProduced == 0 end, 3), true)
    clickControl(button("RULEUTC_ProductionToggle"))
    waitFor(function() return not bit(fab, 4) end, 2)
    check("on again it makes mass", waitFor(function() return fab:GetEconData().massProduced > 0 end, 10), true)
    -- FAF's key action for a toggle (toggle_intel: misckeyactions.toggleScript, GetScriptBit then ToggleScriptBit)
    select({radars[1]})
    import("/lua/keymap/misckeyactions.lua").toggleScript("Intel")
    check("toggle_intel's action switches the radar off", waitFor(function() return bit(radars[1], 3) end, 2), true)
    import("/lua/keymap/misckeyactions.lua").toggleScript("Intel")
    check("and on again", waitFor(function() return not bit(radars[1], 3) end, 2), true)

    -- 5. mixed selections: two radars, one switched off; the button's "mixed" mark, and a click sets both alike
    select({radars[2]})
    clickControl(button("RULEUTC_IntelToggle"))
    waitFor(function() return bit(radars[2], 3) end, 2)
    select({radars[1], radars[2]})
    local btn = button("RULEUTC_IntelToggle")
    check("two radars, one off: the intel button has the mixed mark", btn and btn._mixedIcon ~= nil, true)
    check("and is checked (FAF: mixed shows as checked)", btn and btn:IsChecked(), true)
    clickControl(btn)
    check("a click: both alike", waitFor(function() return bit(radars[1], 3) == bit(radars[2], 3) end, 2), true)
    check("both on (the one switched off is switched on)", bit(radars[1], 3) or bit(radars[2], 3), false)
    check("the mark is gone", btn and btn._mixedIcon, nil)
    -- mixed shields: FAF's special case for toggle 0, a click turns every shield on
    select({titan})
    clickControl(button("RULEUTC_ShieldToggle"))
    waitFor(function() return not bit(titan, 0) end, 2)
    select({shield, titan})
    btn = button("RULEUTC_ShieldToggle")
    check("a shield generator and a Titan, the Titan's shield off: mixed", btn and btn._mixedIcon ~= nil, true)
    clickControl(btn)
    check("a click: both shields on (ToggleScriptBit(units, 0, false))", waitFor(function() return bit(shield, 0) and bit(titan, 0) end, 2), true)
    check("the Titan asks for its energy again", waitFor(function() return energy(titan) > 0 end, 5), true)

    -- 3. fire states: the button's pop-up (SetFireState with the state's name), FAF's "[" key (ToggleFireState).
    -- The tanks were put on hold fire when made (SetFireState with a number, as scripts do).
    select({tanks[1]})
    btn = button("RULEUCC_RetaliateToggle")
    check("a tank: the fire state button shows hold fire", btn and btn._toggleState .. " " .. btn._curHelpText, "1 mode_hold_fire")
    local function popupButton(state)
        for _, b in (btn and btn._popup and btn._popup.buttons) or {} do
            if b.index == state then return b end
        end
    end
    local function pick(state)
        clickControl(btn)
        local opened = btn and btn._popup ~= nil
        clickControl(popupButton(state))
        return opened
    end
    check("a click opens the fire states' pop-up", pick(2), true)
    check("hold ground picked: the tank's state", waitFor(function() return GetFireState({tanks[1]}) == 2 end, 2), true)
    check("the button shows hold ground", btn and btn._toggleState .. " " .. btn._curHelpText, "2 mode_hold_ground")
    check("the pop-up closed", btn and btn._popup, nil)
    pick(1)
    check("hold fire picked again", waitFor(function() return GetFireState({tanks[1]}) == 1 end, 2), true)
    -- on hold fire neither tank shoots the enemy's generator within their reach
    check("the enemy's generator is there", target ~= nil and not target:IsDead(), true)
    if not target or target:IsDead() then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    local full = target:GetHealth()
    WaitSeconds(5)
    check("on hold fire it is not shot", target:GetHealth(), full)
    -- FAF's mode key: the next state (ToggleFireState(units, GetFireState(units)))
    check("FAF's mode key is \"[\"", moho64ui_KeyMapAction("LeftBracket"), import("/lua/keymap/keyactions.lua").keyActions.mode.action)
    moho64ui_Key("[", "", "", 219)
    check("the key: hold fire -> hold ground", waitFor(function() return GetFireState({tanks[1]}) == 2 end, 2), true)
    moho64ui_Key("[", "", "", 219)
    check("again: return fire", waitFor(function() return GetFireState({tanks[1]}) == 0 end, 2), true)
    moho64ui_Key("[", "", "", 219)
    check("again: hold fire", waitFor(function() return GetFireState({tanks[1]}) == 1 end, 2), true)
    -- mixed: one tank on hold ground, the other holding fire
    moho64ui_Key("[", "", "", 219)
    waitFor(function() return GetFireState({tanks[1]}) == 2 end, 2)
    select(nil)
    select({tanks[1], tanks[2]})
    btn = button("RULEUCC_RetaliateToggle")
    check("both tanks, one on hold ground, one holding fire: mixed (-1)", btn and btn._toggleState, -1)
    check("with its mixed mark", btn and btn.mixedIcon ~= nil, true)
    moho64ui_Key("[", "", "", 219)
    check("the key on a mixed selection: return fire for both", waitFor(function() return GetFireState(tanks) == 0 end, 2), true)
    -- on return fire they shoot it
    check("on return fire the generator is shot", waitFor(function() return target:IsDead() or target:GetHealth() < full end, 10), true)
    pick(1)
    check("hold fire for both through the pop-up", waitFor(function() return GetFireState(tanks) == 1 end, 2), true)
    -- the original's argument checks (FA exe 0x8bba10, 0x8bb760, 0x8bbe60)
    check("SetFireState wants two arguments", pcall(SetFireState, tanks), false)
    check("ToggleFireState wants a number", pcall(ToggleFireState, tanks, "HoldFire"), false)
    check("ToggleScriptBit wants three arguments", pcall(ToggleScriptBit, {fab}, 4), false)
    check("SetFireState takes a number as its digits", GetFireState(tanks), 1)

    -- 2. automatic mode: the strategic missile defence starts in it (InitialAutoMode), a right click on its build
    -- button switches it
    select({smd})
    btn = button("RULEUCC_SiloBuildTactical")
    check("the missile defence's build button", btn ~= nil, true)
    check("it starts in automatic mode", smd:IsAutoMode(), true)
    check("GetIsAutoMode says so", GetIsAutoMode({smd}), true)
    check("the button is checked", btn and btn:IsChecked(), true)
    LOG("TEST note: the missile defence's work after 0.5 s: " .. progress(smd))
    clickControl(btn, "Right")
    check("a right click: automatic mode off", waitFor(function() return not smd:IsAutoMode() end, 2), true)
    check("the button is not checked", btn and btn:IsChecked(), false)
    select(nil)
    select({smd})
    btn = button("RULEUCC_SiloBuildTactical")
    check("selected again it shows off (GetIsAutoMode)", btn and btn:IsChecked(), false)
    clickControl(btn, "Right")
    check("a right click again: on", waitFor(function() return smd:IsAutoMode() end, 2), true)
    check("with automatic mode it builds a missile", waitFor(function() return progress(smd) > 0 end, 5), true)
    -- GetIsAutoMode of a list (FA exe 0x8bac20): true when every unit has it, so a missile defence with a factory
    -- (none) is false, and with the commander (FAF's UEF commander starts in it, for its missile's enhancement) true
    check("GetIsAutoMode of a list: every unit must have it", GetIsAutoMode({smd, factory}), false)
    check("the missile defence and the commander: both have it", GetIsAutoMode({smd, cdr}), true)

    -- 4. pausing: a factory building (the construction panel's pause button, then FAF's "Z" key)
    select({factory})
    IssueBlueprintCommand("UNITCOMMAND_BuildFactory", "uel0105", 5)
    check("the factory builds", waitFor(function() return progress(factory) > 0 end, 5), true)
    select(nil)
    select({factory})
    local pause = construction.controls.extraBtn2
    check("the construction panel's pause button is there and enabled", pause ~= nil and not pause:IsDisabled(), true)
    check("not checked", pause and pause:IsChecked(), false)
    clickControl(pause)
    check("a click pauses the factory", waitFor(function() return GetIsPaused({factory}) end, 2), true)
    check("the button is checked", pause and pause:IsChecked(), true)
    WaitSeconds(0.5)
    check("paused, its work stands still", advance(factory, 2), 0)
    check("FAF's pause key is Z", moho64ui_KeyMapAction("Z"), import("/lua/keymap/keyactions.lua").keyActions.pause_unit.action)
    moho64ui_Key("z")
    check("Z: going on", waitFor(function() return not GetIsPaused({factory}) end, 2), true)
    check("and its work goes on", advance(factory, 2) > 0, true)
    check("the button is not checked", pause and pause:IsChecked(), false)
    -- its repeat: FAF's infinite build button beside it (ProcessInfo('SetRepeatQueue'), not an automatic mode)
    local rep = construction.controls.extraBtn1
    check("the infinite build button is there", rep ~= nil and not rep:IsDisabled(), true)
    clickControl(rep)
    check("a click: the factory repeats its queue", waitFor(function() return factory:IsRepeatQueue() end, 2), true)
    clickControl(rep)
    check("again: it does not", waitFor(function() return not factory:IsRepeatQueue() end, 2), true)
    -- an engineer building
    select({engineer})
    CM.StartCommandMode("build", {name = "ueb1101"})
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local e = engineer:GetPosition()
    local s = view:Project({e[1] - 8, e[2], e[3] - 6})
    moho64ui_MouseMove(s.x, s.y)
    WaitSeconds(0.3)
    moho64ui_MouseButton("Left", true, s.x, s.y)
    moho64ui_MouseButton("Left", false, s.x, s.y)
    WaitSeconds(0.3)
    CM.EndCommandMode(true)
    check("the engineer builds", waitFor(function() return progress(engineer) > 0 end, 15), true)
    select({engineer})
    pause = construction.controls.extraBtn2
    clickControl(pause)
    check("the pause button pauses the engineer", waitFor(function() return GetIsPaused({engineer}) end, 2), true)
    WaitSeconds(0.5)
    check("paused, the building stands still", advance(engineer, 2), 0)
    moho64ui_Key("z")
    check("Z: going on", waitFor(function() return not GetIsPaused({engineer}) end, 2), true)
    check("and the building goes on", advance(engineer, 2) > 0, true)
    -- an upgrade (the radar to a T2 radar)
    select({radars[3]})
    IssueBlueprintCommand("UNITCOMMAND_Upgrade", "ueb3201", 1, false)
    check("the radar upgrades", waitFor(function() return progress(radars[3]) > 0 end, 5), true)
    select(nil)
    select({radars[3]})
    moho64ui_Key("z")
    check("Z pauses the upgrade", waitFor(function() return GetIsPaused({radars[3]}) end, 2), true)
    WaitSeconds(0.5)
    check("paused, the upgrade stands still", advance(radars[3], 2), 0)
    pause = construction.controls.extraBtn2
    check("the pause button shows it", pause and pause:IsChecked(), true)
    clickControl(pause)
    check("the button: going on", waitFor(function() return not GetIsPaused({radars[3]}) end, 2), true)
    check("and the upgrade goes on", advance(radars[3], 2) > 0, true)
    -- a mixed pause: the factory paused, the engineer not; GetIsPaused is true when any is; the key pauses both
    select({factory})
    moho64ui_Key("z")
    waitFor(function() return GetIsPaused({factory}) end, 2)
    check("GetIsPaused of a list: any paused", GetIsPaused({factory, engineer}), true)

    -- 6. the replay: switches left changed (each through its button), so that the snapshots after them show them: a
    -- radar's intel off, the shield generator off, the fabricator's production off, the tanks on hold ground
    select({radars[1]})
    clickControl(button("RULEUTC_IntelToggle"))
    select({shield})
    clickControl(button("RULEUTC_ShieldToggle"))
    select({fab})
    clickControl(button("RULEUTC_ProductionToggle"))
    select(tanks)
    btn = button("RULEUCC_RetaliateToggle")
    pick(2)
    check("left changed for the replay: intel, shield and production off, the tanks on hold ground",
        waitFor(function() return bit(radars[1], 3) and not bit(shield, 0) and bit(fab, 4) and GetFireState(tanks) == 2 end, 3), true)
    -- the states at fixed ticks (the snapshots), then the game kept for the second run
    local last = SNAPSHOTS[table.getn(SNAPSHOTS)]
    check("the test ends before the last snapshot", GameTick() < last, true)
    LOG("TEST note: the cases took the game to tick " .. GameTick())
    while GameTick() < last + 30 do WaitSeconds(0.2) end  -- (kept a little past it: the replay's last beats)
    local profile = import("/lua/user/prefs.lua").GetCurrentProfile().Name
    CopyCurrentReplay(profile, REPLAY)
    local kept = GetSpecialFileInfo(profile, REPLAY, "Replay")
    check("the game is kept for its replay", kept and kept.SizeBytes > 100, true)
    LOG("TEST done: " .. failed .. " failed")
end)
