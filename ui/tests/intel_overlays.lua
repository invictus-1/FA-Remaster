-- The intel overlays and the other range rings, as the original draws them (user/range_rings.cpp): the
-- switches of FAF's multifunction panel (its filter list: the conditions, the military and intel filters,
-- their combinations) and the console variables (range_RenderSelected, range_RenderHighlighted,
-- range_RenderBuild, range_Fill, range_InnerThicknessCoeff, range_OuterThicknessCoeff, ren_Ranges), as the
-- host's test function moho64ui_RangeRings() reports what the picture is given: the passes in their order,
-- each filter's rings as one group, their radii, colours and thickness. Units made by the game's cheat
-- callback: radars, an omni sensor, a stealth field, tanks and artillery. Run it like orderlines.lua (a
-- launched game with CheatsEnabled = "true", the world's picture), with --frames 4000; it ends with the
-- radar, omni and counter-intel overlays switched on over the base, for a --screenshot.
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local function made(bp, category)
    UISelectionByCategory(category, false, false, false, false)
    local found = {}
    for _, u in GetSelectedUnits() or {} do
        if u:GetBlueprint().BlueprintId == bp then table.insert(found, u) end
    end
    SelectUnits(nil)
    return found
end
local function id(u) return tonumber(u:GetEntityId()) end
local function color(c) return string.lower(string.sub(c, 3)) end
-- the rings of a pass (and filter), as the picture was last given them
local function rings(pass, filter)
    local found = {}
    local all, state = moho64ui_RangeRings("scene")
    for _, r in all do
        if (not pass or r.pass == pass) and (not filter or r.filter == filter) then table.insert(found, r) end
    end
    return found, state, all
end
local function ringOf(list, u)
    for _, r in list do
        if r.unit == id(u) then return r end
    end
    return nil
end
local function groups(list)
    local g, n = {}, 0
    for _, r in list do
        if not g[r.group] then g[r.group] = true; n = n + 1 end
    end
    return n
end
local function frame() WaitSeconds(0.1) end
local function switches() local _, s = moho64ui_RangeRings() return s end
local function everything() local _, _, e = rings() return e end

ForkThread(function()
    WaitSeconds(6)
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local MF = import("/lua/ui/game/multifunction.lua")
    local CM = import("/lua/ui/game/commandmode.lua")
    local filters = import("/lua/ui/game/rangeoverlayparams.lua").RangeOverlayParams
    local cam = GetCamera("WorldCamera")
    local cdr = GetArmyAvatars()[1]
    local p = cdr:GetPosition()
    local army = GetFocusArmy()
    local function at(dx, dz) return {p[1] + dx, p[2], p[3] + dz} end
    local spawn = function(bp, place, count)
        SimCallback({Func = "CheatSpawnUnit", Args = {army = army, pos = place, bpId = bp, count = count or 1, yaw = 0}})
    end
    spawn("ueb3101", at(30, 0))     -- T1 radar, 116
    spawn("ueb3201", at(30, 20))    -- T2 radar, 200
    spawn("ueb3104", at(50, 0))     -- T3 omni sensor: omni 200, radar 600
    spawn("ueb4203", at(50, 20))    -- stealth field: radar and sonar stealth fields 24
    spawn("uel0103", at(-20, 20))   -- mobile artillery: indirect fire 30, its least range 5
    spawn("uel0201", at(-20, 30), 2)
    WaitSeconds(3)
    local radar1 = made("ueb3101", "STRUCTURE")[1]
    local radar2 = made("ueb3201", "STRUCTURE")[1]
    local omni = made("ueb3104", "STRUCTURE")[1]
    local stealth = made("ueb4203", "STRUCTURE")[1]
    local arty = made("uel0103", "LAND")[1]
    local tanks = made("uel0201", "LAND")
    check("the radars, the omni sensor, the stealth field, the artillery and two tanks are made",
        tostring(radar1 ~= nil) .. tostring(radar2 ~= nil) .. tostring(omni ~= nil) .. tostring(stealth ~= nil) ..
        tostring(arty ~= nil) .. table.getn(tanks), "truetruetruetruetrue2")
    if not (radar1 and radar2 and omni and stealth and arty) then
        LOG("TEST done: " .. failed .. " failed")
        return
    end
    cam:SnapTo(at(15, 10), nil, 260)
    moho64ui_MouseMove(view.Left() + 5, view.Top() + 5)
    WaitSeconds(0.5)

    -- the switches as the game's script left them (multifunction.lua: the three conditions on unless the
    -- profile says otherwise; nothing else switched on), and the console's others as the original's at first
    local all, state = moho64ui_RangeRings("scene")
    check("the script switched the three conditions on", tostring(state.selected) .. " " .. tostring(state.highlighted) .. " " .. tostring(state.build), "true true true")
    check("range_Fill off, ren_Ranges on", tostring(state.fill) .. " " .. tostring(state.ranges), "false true")
    check("the thickness coefficients 1/1024", string.format("%.7f %.7f", state.innerCoeff, state.outerCoeff), "0.0009766 0.0009766")
    check("no overlay switched on", table.getn(state.active), 0)
    check("nothing selected, nothing under the mouse: no rings", table.getn(all), 0)

    -- the console's words for them (the original's registry: bools and floats)
    local heard = {}
    local handle = AddConsoleOutputReciever(function(text) table.insert(heard, text) end)
    local function take() local t = table.concat(heard, "|"); heard = {}; return t end
    ConExecute("range_Fill")
    check("range_Fill alone turns over and says so", take() .. " " .. tostring(switches().fill), "toggled range_Fill is now on true")
    ConExecute("range_fill off")
    check("range_Fill off", tostring(switches().fill), "false")
    ConExecute("range_InnerThicknessCoeff")
    check("a coefficient alone says its value", take(), "float range_InnerThicknessCoeff == 0.0010")
    ConExecute("range_RenderBuild show")
    check("show says a switch's value", take(), "bool range_RenderBuild is on")
    ConExecute("ren_Ranges show")
    check("ren_Ranges is on", take(), "bool ren_Ranges is on")

    -- the selection's rings: each filter of the unit's, in the select colour, in the order of the filters' names
    local playable = math.max(SessionGetScenarioInfo().size[1], SessionGetScenarioInfo().size[2])
    SelectUnits({radar1})
    frame()
    local sel, state2 = rings("selected")
    local r = ringOf(sel, radar1)
    check("the selected radar: one ring", table.getn(sel), 1)
    check("of the Radar filter, its radius, the select colour",
        r and (r.filter .. " " .. r.radius .. " " .. r.color), "Radar " .. radar1:GetBlueprint().Intel.RadarRadius .. " " .. color(filters.Radar.SelectColor))
    local rp = radar1:GetPosition()
    check("round the radar", r ~= nil and math.abs(r.x - rp[1]) < 0.1 and math.abs(r.z - rp[3]) < 0.1, true)
    check("the glow: the colour's first byte", r and r.glow, tonumber(string.sub(filters.Radar.SelectColor, 1, 2), 16))
    -- the thickness: near + (far x coefficient x the playable map's longer side - near) x zoom / farthest zoom
    local share = cam:GetZoom() / cam:GetMaxZoom()
    check("the camera's zoom share is the one the rings were worked out with", math.abs(state2.zoomShare - share) < 1e-4, true)
    check("and the playable map's longer side", state2.playable, playable)
    local o = filters.Radar.Outer
    local want = o[1] + (o[2] * state2.outerCoeff * playable - o[1]) * state2.zoomShare
    check("the outline's thickness by the original's rule", r ~= nil and math.abs(r.outer - want) < 1e-4, true)
    LOG(string.format("TEST note: zoom %.1f of %.1f, the radar's outline %.4f world units", cam:GetZoom(), cam:GetMaxZoom(), r and r.outer or -1))
    ConExecute("range_OuterThicknessCoeff 0.001953125")
    frame()
    r = ringOf(rings("selected", "Radar"), radar1)
    want = o[1] + (o[2] * 0.001953125 * playable - o[1]) * share
    check("range_OuterThicknessCoeff changes it", r ~= nil and math.abs(r.outer - want) < 1e-4, true)
    ConExecute("range_OuterThicknessCoeff 0.0009765625")
    cam:SnapTo(at(15, 10), nil, 120)
    WaitSeconds(0.3)
    local r2 = ringOf(rings("selected", "Radar"), radar1)
    want = o[1] + (o[2] * (1 / 1024) * playable - o[1]) * (cam:GetZoom() / cam:GetMaxZoom())
    check("nearer, thinner by the same rule", r2 ~= nil and math.abs(r2.outer - want) < 1e-4 and r2.outer < r.outer, true)
    cam:SnapTo(at(15, 10), nil, 260)
    frame()

    -- the radar's intel switched off (its toggle, script bit 3): no rings (UserUnit slot 0x48)
    ToggleScriptBit({radar1}, 3, false)
    WaitSeconds(1)
    check("the radar's intel is off", GetScriptBit({radar1}, 3), true)
    check("its ring is gone", table.getn(rings("selected")), 0)
    ToggleScriptBit({radar1}, 3, true)
    WaitSeconds(1)
    check("on again: back", table.getn(rings("selected", "Radar")), 1)

    -- the commander: its weapons as the simulation keeps them, an enhancement's included (the original asks the
    -- unit's weapons, enabled or not); its omni; its build range
    SelectUnits({cdr})
    frame()
    sel = rings("selected")
    local bp = cdr:GetBlueprint()
    local direct, indirect, least = 0, 0, nil
    for _, w in bp.Weapon do
        if not w.DummyWeapon then
            if w.RangeCategory == 'UWRC_DirectFire' and w.MaxRadius > direct then direct = w.MaxRadius end
            if w.RangeCategory == 'UWRC_IndirectFire' then
                if w.MaxRadius > indirect then indirect = w.MaxRadius end
                least = math.min(least or w.MinRadius or 0, w.MinRadius or 0)
            end
        end
    end
    local byFilter = {}
    for _, x in sel do byFilter[x.filter] = x end
    check("the commander's direct fire ring", byFilter.DirectFire and byFilter.DirectFire.radius, direct)
    check("its indirect fire ring takes the missile an enhancement brings", byFilter.IndirectFire and byFilter.IndirectFire.radius, indirect > 0 and indirect or nil)
    check("its omni ring", byFilter.Omni and byFilter.Omni.radius, bp.Intel.OmniRadius)
    check("its build range (Miscellaneous: AI.StagingPlatformScanRadius)", byFilter.Miscellaneous and byFilter.Miscellaneous.radius, bp.AI.StagingPlatformScanRadius)
    check("no AllMilitary nor AllIntel ring for a selection", tostring(byFilter.AllMilitary) .. tostring(byFilter.AllIntel), "nilnil")
    local order = {}
    for _, x in sel do order[x.group] = x.filter end
    local names = {}
    for _, n in order do table.insert(names, n) end
    local sorted = true
    for i = 2, table.getn(names) do
        if names[i - 1] >= names[i] then sorted = false end
    end
    check("one group per filter, in the order of the filters' names (the original's map)", sorted and groups(sel) == table.getn(sel), true)
    LOG("TEST note: the commander's rings " .. table.concat(names, ", "))

    -- the artillery: its indirect fire ring with the least range as an inner ring
    SelectUnits({arty})
    frame()
    r = ringOf(rings("selected", "IndirectFire"), arty)
    check("the artillery's indirect fire ring, its inner ring at its MinRadius", r and (r.radius .. " " .. r.inner), "30 5")
    check("the hole's outline by the Inner pair", r ~= nil and math.abs(r.innerLine - (filters.IndirectFire.Inner[1] +
        (filters.IndirectFire.Inner[2] * state2.innerCoeff * playable - filters.IndirectFire.Inner[1]) * (cam:GetZoom() / cam:GetMaxZoom()))) < 1e-4, true)

    -- the stealth field: its counter-intel ring (the greatest of its fields); its toggle off: none
    SelectUnits({stealth})
    frame()
    r = ringOf(rings("selected", "CounterIntel"), stealth)
    check("the stealth field's counter-intel ring", r and r.radius, 24)
    ToggleScriptBit({stealth}, 5, false)
    WaitSeconds(1)
    check("its stealth toggle set: gone", table.getn(rings("selected", "CounterIntel")), 0)
    ToggleScriptBit({stealth}, 5, true)
    WaitSeconds(1)

    -- range_RenderSelected off and on (FAF's "Selection" condition does the same through its list)
    SelectUnits({radar1, radar2})
    frame()
    sel = rings("selected", "Radar")
    check("two selected radars: their rings in one group", table.getn(sel) .. " " .. groups(sel), "2 1")
    ConExecute("range_RenderSelected false")
    frame()
    check("range_RenderSelected false: none", table.getn(rings("selected")), 0)
    ConExecute("range_RenderSelected true")
    SelectUnits(nil)
    frame()

    -- the unit under the mouse: its rings in the rollover colour
    local s = view:Project(radar2:GetPosition())
    moho64ui_MouseMove(s.x, s.y)
    WaitSeconds(0.3)
    local hl = rings("highlighted")
    r = ringOf(hl, radar2)
    check("the radar under the mouse: its ring in the rollover colour", r and (r.filter .. " " .. r.radius .. " " .. r.color),
        "Radar 200 " .. color(filters.Radar.RolloverColor))
    ConExecute("range_RenderHighlighted false")
    frame()
    check("range_RenderHighlighted false: none", table.getn(rings("highlighted")), 0)
    ConExecute("range_RenderHighlighted true")
    moho64ui_MouseMove(view.Left() + 5, view.Top() + 5)
    WaitSeconds(0.3)
    check("the mouse away: none", table.getn(rings("highlighted")), 0)

    -- the building about to be placed: at the place under the mouse, by its blueprint, the normal colour
    SelectUnits({cdr})
    CM.StartCommandMode("build", {name = "ueb3101"})
    local spot = at(-40, -30)
    s = view:Project(spot)
    moho64ui_MouseMove(s.x, s.y)
    WaitSeconds(0.3)
    local b = rings("build")
    local mouse = GetMouseWorldPos()
    check("placing a radar: its Radar ring", table.getn(b) == 1 and b[1].filter .. " " .. b[1].radius .. " " .. b[1].color,
        "Radar 116 " .. color(filters.Radar.NormalColor))
    check("at the place under the mouse", b[1] ~= nil and math.abs(b[1].x - mouse[1]) < 0.5 and math.abs(b[1].z - mouse[3]) < 0.5, true)
    local _, _, every = rings()
    check("drawn first", every[1] ~= nil and every[1].pass, "build")
    ConExecute("range_RenderBuild false")
    frame()
    check("range_RenderBuild false: none", table.getn(rings("build")), 0)
    ConExecute("range_RenderBuild true")
    CM.EndCommandMode(true)
    SelectUnits(nil)
    moho64ui_MouseMove(view.Left() + 5, view.Top() + 5)
    WaitSeconds(0.3)
    check("the build mode left: none", table.getn(rings("build")), 0)

    -- the overlays through FAF's own list (the military button's drop-out: multifunction.lua CreateFilterDropout)
    local btn = MF.GetButton('military')
    local function item(key)
        if not btn.dropout.list then MF.CreateFilterDropout(btn.dropout) end
        for _, e in btn.dropout.list.items do
            if e.Data and e.Data.key == key then return e end
        end
    end
    local function press(key)
        local e = item(key)
        if e then e:HandleEvent({Type = 'ButtonPress'}) end
        frame()
        return e ~= nil
    end
    -- its three conditions are the console's range_RenderSelected, range_RenderHighlighted, range_RenderBuild
    for _, c in {{'selection', 'selected'}, {'rollover', 'highlighted'}, {'buildpreview', 'build'}} do
        press(c[1])
        local off = switches()[c[2]]
        press(c[1])
        check("FAF's condition '" .. c[1] .. "' switches its console variable off and on again", tostring(off) .. " " .. tostring(switches()[c[2]]), "false true")
    end
    check("the list has the radar entry", press('radar'), true)
    local act, st = rings("active")
    check("Radar switched on: the script asks for it", st.active[1], "Radar")
    local radars = {}
    for _, x in act do
        if x.filter == "Radar" then radars[x.unit] = x.radius end
    end
    check("every radar of the player's in view, normal colour, one group",
        tostring(radars[id(radar1)]) .. " " .. tostring(radars[id(radar2)]) .. " " .. tostring(radars[id(omni)]) .. " " ..
        groups(act) .. " " .. (act[1] and act[1].color or ""), "116 200 600 1 " .. color(filters.Radar.NormalColor))
    -- a selected radar is drawn in its own colour only (FAF leaves it out of the overlay's rings)
    SelectUnits({radar1})
    frame()
    act = rings("active", "Radar")
    check("selected: the radar leaves the overlay's group", tostring(ringOf(act, radar1) == nil) .. " " .. tostring(ringOf(act, radar2) ~= nil), "true true")
    check("and is in the selection's", ringOf(rings("selected", "Radar"), radar1) ~= nil, true)
    local _, _, every2 = rings()
    check("the overlays before the selection", every2[1] and every2[1].pass .. " " .. every2[table.getn(every2)].pass, "active selected")
    SelectUnits(nil)
    frame()
    -- out of the camera's view: no overlay rings (the original takes the units in the view's frustum)
    cam:SnapTo(at(-400, -400), nil, 60)
    WaitSeconds(0.3)
    check("the camera elsewhere: no overlay rings", table.getn(rings("active")), 0)
    cam:SnapTo(at(15, 10), nil, 260)
    WaitSeconds(0.3)
    check("back: there again", table.getn(rings("active", "Radar")) >= 3, true)
    -- the omni filter beside it: two groups, in the order switched on
    check("the list has the omni entry", press('omni'), true)
    act, st = rings("active")
    check("Radar and Omni switched on", table.getn(st.active), 2)
    check("the omni sensor's omni ring", ringOf(rings("active", "Omni"), omni) and ringOf(rings("active", "Omni"), omni).radius, 200)
    check("two groups", groups(act), 2)
    -- Combine Intel: one AllIntel group of every unit in the switched-on intel filters, the greatest of its
    -- omni, radar and sonar radii, in the combination's colour
    check("the list has Combine Intel", press('allintel'), true)
    act, st = rings("active")
    check("combined: only AllIntel is asked for", table.getn(st.active) .. " " .. tostring(st.active[1]), "1 AllIntel")
    r = ringOf(act, omni)
    check("the omni sensor's combined ring: its radar's 600", r and (r.filter .. " " .. r.radius .. " " .. r.color), "AllIntel 600 " .. color(filters.AllIntel.NormalColor))
    check("and the T1 radar's", ringOf(act, radar1) and ringOf(act, radar1).radius, 116)
    press('allintel')
    -- the military button off and on (FAF: SetOverlayFilters({}), then its list again)
    btn:ToggleCheck()
    frame()
    check("the military button off: no overlay rings", table.getn(rings("active")), 0)
    btn:ToggleCheck()
    frame()
    check("on: back", table.getn(rings("active", "Radar")) >= 3, true)
    -- ren_Ranges off: nothing at all
    ConExecute("ren_Ranges false")
    frame()
    check("ren_Ranges false: no rings", table.getn(everything()), 0)
    ConExecute("ren_Ranges true")
    frame()
    check("true: back", table.getn(rings("active")) > 0, true)
    -- the counter-intel filter: the stealth field's
    press('counterintel')
    check("Counter Intelligence switched on: the stealth field's ring", ringOf(rings("active", "CounterIntel"), stealth) and ringOf(rings("active", "CounterIntel"), stealth).radius, 24)
    press('counterintel')
    -- a military filter beside the intel ones: the tanks' direct fire
    press('directfire')
    act = rings("active", "DirectFire")
    check("Direct Fire switched on: the tanks' and the commander's rings", ringOf(act, tanks[1]) ~= nil and ringOf(act, cdr) ~= nil, true)
    -- Combine Military: a unit in OVERLAYMISC (engineers, commanders) has its build range there, the others their
    -- weapons (CombinedMilitaryExtractor 0x7ec3a0)
    press('allmilitary')
    act = rings("active", "AllMilitary")
    local function radius(u) local x = ringOf(act, u) return x and x.radius end
    check("Combine Military: the tanks' weapons, the commander's build range", tostring(radius(tanks[1])) .. " " .. tostring(radius(cdr)),
        "18 " .. tostring(cdr:GetBlueprint().AI.StagingPlatformScanRadius))
    press('allmilitary')
    press('directfire')
    RemoveConsoleOutputReciever(handle)

    -- for the screenshot: the radar, omni and counter-intel overlays on over the base, the camera over it; the omni
    -- sensor's intel switched off (its 600 radar would hold the other radars' rings: one outline round what the
    -- rings of a filter cover together), so that the two radars' rings show as one outline
    press('counterintel')
    ToggleScriptBit({omni}, 3, false)
    WaitSeconds(1)
    check("the omni sensor's intel off: its overlay rings are gone", tostring(ringOf(rings("active", "Radar"), omni)) .. " " ..
        tostring(ringOf(rings("active", "Omni"), omni)), "nil nil")
    if btn.dropout.list then btn.dropout.list:Destroy(); btn.dropout.list = nil end
    cam:SnapTo(at(25, 10), nil, 520)
    WaitSeconds(0.5)
    local final, fs = rings()
    LOG("TEST note: at the end " .. table.getn(final) .. " rings, switched on: " .. table.concat(fs.active, ", "))
    LOG("TEST done: " .. failed .. " failed")
end)
