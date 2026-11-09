-- The main menu's staged battle on Seton's Clutch Remastered (ours, under the repository's MIT licence: docs/ui-
-- menuflight.md; the scenario's script, setons_r_menu_scenario.lua). It uses only the game's own simulation functions
-- (CreateUnitHPR, IssueAggressiveMove, IssueNuke, GiveNukeSiloAmmo, ForkThread, WaitTicks); no AI of any kind and no
-- orders from outside: the storyboard below says where and when, and the units fire at what comes into their range by
-- themselves. With the same seed and files every run plays the same (the weapons' scatter is the simulation's seeded
-- random), so the menu's camera path (setons_r_flight.path) meets the action where this storyboard puts it.
--
-- The clock is the game's tick (ten a second); the camera path's time is battle seconds (tick / 10).
-- Markers for the tests (ui/tests/menu_flight.lua, the suite's menu_flight_battle): every line begins "MENUFLIGHT":
--   MENUFLIGHT spawn <name> tick <t>            a group of the storyboard made
--   MENUFLIGHT order <what> tick <t>             an order of the storyboard given
--   MENUFLIGHT first loss tick <t>               the first unit of either side destroyed (the fronts have met)
--   MENUFLIGHT at tick <t>: uef <n> (x,z) cybran <n> (x,z) ...   every 100 ticks: the units alive (the middle of
--                                                them) and where the named ones are
--   MENUFLIGHT nuke launch tick <t> / nuke impact tick <t>
--   MENUFLIGHT end tick <t>                      the storyboard's end: the host starts the battle over
-- The host reads MenuFlight.ended (the end's tick, 0 before) between beats: a true global (this script runs in the
-- scenario's environment, ScenarioInfo.Env, simInit.lua's doscript; its own globals are that table's).
local ScenarioUtils = import('/lua/sim/ScenarioUtilities.lua')

local MenuFlight = { ended = 0 }
_G.MenuFlight = MenuFlight

-- The map: the north-east half is the UEF's (ARMY_1, its base round the map's start 1 at 672, 346), the south-west
-- the Cybran's (ARMY_2, round 357, 673); the land bridge between them runs from 610, 448 over the middle (513, 510)
-- to 416, 574 (the map's land path nodes LandPN02, the large expansion area, LandPN01).
local NUKE_TARGET = { 338, 702 }  -- the Cybran rear base (the map's protected experimental construction 03)

local function Ground(x, z)
    return GetSurfaceHeight(x, z)
end
local function Note(text)
    LOG('MENUFLIGHT ' .. text)
end

-- Units by the storyboard's names: a group is a list of { blueprint, x, z }, made facing `yaw` (radians: 0 north).
local Groups = {}
local Named = {}  -- single units the markers follow
local function Spawn(name, army, yaw, list)
    local units = {}
    for _, u in list do
        local unit = CreateUnitHPR(u[1], army, u[2], Ground(u[2], u[3]), u[3], 0, yaw, 0)
        if unit then
            table.insert(units, unit)
            if u[4] then Named[u[4]] = unit end
        end
    end
    Groups[name] = units
    Note(string.format('spawn %s (%d units) tick %d', name, table.getn(units), GetGameTick()))
    return units
end
local function Alive(units)
    local out = {}
    for _, u in units or {} do
        if not u.Dead and not u:BeenDestroyed() then table.insert(out, u) end
    end
    return out
end
local function Push(name, x, z)
    local units = Alive(Groups[name])
    if table.getn(units) > 0 then IssueAggressiveMove(units, { x, Ground(x, z), z }) end
    Note(string.format('order %s to %d, %d (%d units) tick %d', name, x, z, table.getn(units), GetGameTick()))
end

-- (a line of units round x, z: `n` of `bp`, `across` apart, the line along `dx`, `dz`)
local function Line(bp, n, x, z, dx, dz, name)
    local list = {}
    for i = 1, n do
        local k = i - (n + 1) / 2
        table.insert(list, { bp, x + k * dx, z + k * dz, name and (name .. i) or nil })
    end
    return list
end
local function Join(...)
    local all = {}
    for _, list in arg do
        if type(list) == 'table' then
            for _, u in list do table.insert(all, u) end
        end
    end
    return all
end

local UEF_FACING, CYBRAN_FACING = 3.93, 0.79  -- (south-west, north-east)

-- The storyboard: { tick, what, ... }, in order.
local Board = {
    { 1, function()
        -- the bases: the UEF's strategic missile launcher and its power, the Cybran's rear base (the nuke's target)
        Spawn('uef_base', 'ARMY_1', UEF_FACING, {
            { 'ueb2305', 690, 312, 'silo' }, { 'ueb1301', 712, 300 }, { 'ueb1301', 712, 330 }, { 'ueb4202', 676, 330 },
        })
        Spawn('cybran_base', 'ARMY_2', CYBRAN_FACING, {
            { 'urb0301', NUKE_TARGET[1], NUKE_TARGET[2], 'witness' }, { 'urb1301', 318, 690 }, { 'urb1301', 318, 718 },
            { 'urb1301', 360, 722 }, { 'urb2301', 372, 686 }, { 'urb2301', 352, 676 }, { 'urb4202', 334, 680 },
        })
        -- the armies at their ends of the land bridge
        Spawn('uef_front', 'ARMY_1', UEF_FACING, Join(
            { { 'uel0401', 640, 418, 'fatboy' } },
            Line('uel0303', 4, 612, 448, 10, -8),
            Line('uel0202', 6, 600, 462, 8, -7),
            Line('xel0306', 2, 652, 432, 12, -10)))
        Spawn('cybran_front', 'ARMY_2', CYBRAN_FACING, Join(
            { { 'url0402', 404, 598, 'monkeylord' }, { 'xrl0403', 384, 612, 'megalith' } },
            Line('url0303', 6, 428, 572, 8, -7),
            Line('url0202', 6, 438, 560, 8, -7)))
    end },
    { 30, function()
        Push('uef_front', 513, 510)
        Push('cybran_front', 513, 510)
    end },
    { 600, function()
        Spawn('uef_air', 'ARMY_1', UEF_FACING, Line('uea0305', 3, 700, 340, 12, -10))
        Spawn('cybran_air', 'ARMY_2', CYBRAN_FACING, Line('xra0305', 3, 340, 680, 12, -10))
        Push('uef_air', 480, 540)
        Push('cybran_air', 545, 480)
    end },
    { 900, function()
        Spawn('uef_second', 'ARMY_1', UEF_FACING, Line('xel0305', 3, 630, 432, 12, -10, 'percival'))
        Spawn('cybran_second', 'ARMY_2', CYBRAN_FACING, Line('xrl0305', 3, 410, 590, 12, -10, 'brick'))
        Push('uef_second', 513, 510)
        Push('cybran_second', 513, 510)
    end },
    { 1250, function()
        -- a third wave each, so that the bridge still fights while the camera leaves it for the nuke (measured
        -- 2026-11-09: without it both sides were nearly spent by tick 1300 and gone by 1700)
        Spawn('uef_third', 'ARMY_1', UEF_FACING, Join(Line('uel0303', 3, 640, 425, 10, -8, 'titan'), Line('uel0202', 4, 626, 440, 8, -7)))
        Spawn('cybran_third', 'ARMY_2', CYBRAN_FACING, Join(Line('url0303', 4, 410, 594, 8, -7, 'loyalist'), Line('url0202', 4, 424, 582, 8, -7)))
    end },
    { 1300, function()
        -- whatever is left of both pushes on to the other's side of the bridge
        for _, name in { 'uef_front', 'uef_second', 'uef_air', 'uef_third' } do Push(name, 430, 575) end
        for _, name in { 'cybran_front', 'cybran_second', 'cybran_air', 'cybran_third' } do Push(name, 600, 455) end
    end },
    { 1700, function()
        local silo = Named.silo
        if silo and not silo.Dead then
            silo:GiveNukeSiloAmmo(1)
            IssueNuke({ silo }, { NUKE_TARGET[1], Ground(NUKE_TARGET[1], NUKE_TARGET[2]), NUKE_TARGET[2] })
            Note(string.format('order nuke at %d, %d tick %d', NUKE_TARGET[1], NUKE_TARGET[2], GetGameTick()))
        end
    end },
    { 2400, 'end' },
}

local NAMES = { 'fatboy', 'monkeylord', 'megalith', 'percival1', 'brick1', 'titan2', 'loyalist2' }
local function Census()
    local text = ''
    for _, side in { { 'uef', { 'uef_front', 'uef_second', 'uef_air', 'uef_third' } },
                       { 'cybran', { 'cybran_front', 'cybran_second', 'cybran_air', 'cybran_third' } } } do
        -- (the count, and the middle of what is alive: where the camera path's keys look)
        local n, x, z = 0, 0, 0
        for _, g in side[2] do
            for _, u in Alive(Groups[g]) do
                local p = u:GetPosition()
                n, x, z = n + 1, x + p[1], z + p[3]
            end
        end
        text = text .. string.format(' %s %d', side[1], n)
        if n > 0 then text = text .. string.format(' (%.0f,%.0f)', x / n, z / n) end
    end
    for _, name in NAMES do
        local u = Named[name]
        if u and not u.Dead and not u:BeenDestroyed() then
            local p = u:GetPosition()
            text = text .. string.format(' %s %.0f,%.0f', name, p[1], p[3])
        elseif u then
            text = text .. ' ' .. name .. ' dead'
        end
    end
    return text
end

local function Run()
    local step = 1
    local lost, launched, hit = false, false, false
    while true do
        local tick = GetGameTick()
        while Board[step] and Board[step][1] <= tick do
            local what = Board[step][2]
            if what == 'end' then
                Note(string.format('end tick %d', tick))
                MenuFlight.ended = tick
                return
            end
            what()
            step = step + 1
        end
        if not lost then
            for _, g in { 'uef_front', 'cybran_front' } do
                if table.getn(Alive(Groups[g])) < table.getn(Groups[g] or {}) then lost = true end
            end
            if lost then Note(string.format('first loss tick %d', tick)) end
        end
        local silo = Named.silo
        if not launched and tick > 1700 and silo and (silo.Dead or silo:GetNukeSiloAmmoCount() == 0) then
            launched = true
            Note(string.format('nuke launch tick %d', tick))
        end
        local witness = Named.witness
        if launched and not hit and (not witness or witness.Dead or witness:BeenDestroyed()) then
            hit = true
            Note(string.format('nuke impact tick %d', tick))
        end
        if math.mod(tick, 100) == 0 then Note(string.format('at tick %d:%s', tick, Census())) end
        WaitTicks(1)
    end
end

function OnPopulate()
    -- The map's armies as a skirmish has them (alliances, the wrecks of the remastered map), without their commanders:
    -- their making asks for prebuilt units only so that no commander warps in (CreateInitialArmyGroup's warp thread);
    -- the option goes back at once, and the commanders are taken away before the first beat.
    local options = ScenarioInfo.Options
    local prebuilt = options.PrebuiltUnits
    options.PrebuiltUnits = 'On'
    ScenarioUtils.InitializeArmies()
    options.PrebuiltUnits = prebuilt
    for _, brain in ArmyBrains do
        if brain.CDR and not brain.CDR:BeenDestroyed() then brain.CDR:Destroy() end
        brain.CDR = nil
    end
end

function OnStart(self)
    ForkThread(Run)
end
