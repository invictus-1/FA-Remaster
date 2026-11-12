-- Run in the simulation by ui/tests/orders_work.lua, orders_air.lua and orders_missiles.lua
-- (ExecLuaInSim('dofile', 'ui/tests/orders_sim.lua'), which the simulation carries out only with cheats on). It makes
-- what the order tests need and tells them facts of the simulation; the orders themselves are given by the tests
-- through the interface, never here. Every answer is one console line "ORDERS <key> <value>" (print reaches the
-- interface's console: transport.lua's last check), which the tests read with AddConsoleOutputReciever.
--   OrdersEconomy({army, energy storage, energy given})
--                                             energy storage and energy for the army (GiveStorage / GiveResource);
--                                             answers "economy <energy stored> <mass stored>"
--   OrdersMake({army, bp, x, z, key})         a finished unit (CreateUnitHPR); answers "<key> <id>"
--   OrdersWreck({army, bp, x, z})             a unit of that army killed where it stands; when its wreck lies there:
--                                             "wreck <id> <x> <z> <mass>"
--   OrdersTree({x, z, r})                     the tree nearest the place within r: "tree <id> <x> <z> <mass> <energy>"
--   OrdersHalfBuilt({army, x, z, bp})         an engineer of the army begins the structure bp there; once it is about a
--                                             tenth done the engineer is taken away: "halfbuilt <id> <fraction>"
--   OrdersEntity(id)                          "entity_<id> alive" or "entity_<id> gone"
--   OrdersFraction(id)                        "fraction_<id> <fraction complete>"
--   OrdersQueue(id)                           the unit's queue in the simulation: "queue_<id> <type>:<targetId> ..."
--   OrdersSilo({id, kind, count})             missiles into the unit's silo (kind "nuke" / "tactical"); its launches are
--                                             counted from then on (the unit's OnNukeLaunched; every weapon's
--                                             projectiles, CreateProjectileAtMuzzle)
--   OrdersSiloState(id)                       "silo_<id> <nuke count> <tactical count> <projectiles> <nuke launches>"
--   OrdersEnhance({id, name})                 the enhancement on the unit (its own CreateEnhancement):
--                                             "enhanced_<id> <does it have RULEUCC_Teleport now>"
--   OrdersHeight({key, x, z})                 "height_<key> <the surface's height there (water's surface over water)>"
--   OrdersBeacons(army)                      the army's ferry beacons: "beacons <id>:<bp>:<x>:<y>:<z> ..." or "beacons none"
--   OrdersWhere(id)                           "where_<id> <x> <z> <attached: 1/0>"
local made = {}
local launches = {}

local function say(key, value)
    print("ORDERS " .. key .. " " .. tostring(value))
end
local function ground(x, z) return x, GetTerrainHeight(x, z), z end

function OrdersEconomy(a)
    local brain = ArmyBrains[a[1]]
    -- (the brain's storage is ONE amount of both: each GiveStorage replaces the last, FA exe 0x588460 as the engine's
    -- economy.cpp reads it; so energy only. GiveResource goes in with the next share-out.)
    brain:GiveStorage('ENERGY', a[2])
    brain:GiveResource('ENERGY', a[3])
    ForkThread(function()  -- (storage given counts from the next tick on)
        WaitTicks(3)
        say("economy", brain:GetEconomyStored('ENERGY') .. " " .. brain:GetEconomyStored('MASS'))
    end)
end

function OrdersMake(a)
    local x, y, z = ground(a[3], a[4])
    local u = CreateUnitHPR(a[2], a[1], x, y, z, 0, 0, 0)
    made[a[5]] = u
    say(a[5], u and u:GetEntityId() or "none")
end

function OrdersWreck(a)
    local x, y, z = ground(a[3], a[4])
    local u = CreateUnitHPR(a[2], a[1], x, y, z, 0, 0, 0)
    ForkThread(function()
        WaitTicks(2)
        u:Kill()
        for i = 1, 100 do
            WaitTicks(2)
            for _, v in GetReclaimablesInRect(Rect(x - 6, z - 6, x + 6, z + 6)) or {} do
                if v.IsWreckage and not v:BeenDestroyed() then
                    local p = v:GetPosition()
                    say("wreck", v:GetEntityId() .. " " .. p[1] .. " " .. p[3] .. " " .. tostring(v.MaxMassReclaim) .. " " .. p[2])
                    return
                end
            end
        end
        say("wreck", "none")
    end)
end

function OrdersTree(a)
    local x, z, r = a[1], a[2], a[3]
    local best, bestD
    for _, v in GetReclaimablesInRect(Rect(x - r, z - r, x + r, z + r)) or {} do
        if v.IsTree and not v.IsTreeGroup and not v:BeenDestroyed() then
            local p = v:GetPosition()
            local d = (p[1] - x) * (p[1] - x) + (p[3] - z) * (p[3] - z)
            if not bestD or d < bestD then best, bestD = v, d end
        end
    end
    if not best then
        say("tree", "none")
        return
    end
    local p = best:GetPosition()
    say("tree", best:GetEntityId() .. " " .. p[1] .. " " .. p[3] .. " " .. tostring(best.MaxMassReclaim) .. " " .. tostring(best.MaxEnergyReclaim) .. " " .. p[2])
end

function OrdersHalfBuilt(a)
    local army, x, z, bp = a[1], a[2], a[3], a[4]
    local ex, ey, ez = ground(x - 8, z)
    local engineer = CreateUnitHPR('uel0105', army, ex, ey, ez, 0, 0, 0)
    local bx, by, bz = ground(x, z)
    IssueBuildMobile({engineer}, {bx, by, bz}, bp, {})
    ForkThread(function()
        for i = 1, 300 do
            WaitTicks(1)
            local s = engineer:GetFocusUnit()
            if s and not s:BeenDestroyed() and s:GetFractionComplete() >= 0.1 then
                IssueClearCommands({engineer})
                engineer:Destroy()
                WaitTicks(2)
                made.halfbuilt = s
                say("halfbuilt", s:GetEntityId() .. " " .. s:GetFractionComplete())
                return
            end
        end
        say("halfbuilt", "none")
    end)
end

function OrdersEntity(id)
    local e = GetEntityById(id)
    say("entity_" .. id, (e and not e:BeenDestroyed() and not e.Dead) and "alive" or "gone")
end

function OrdersFraction(id)
    local e = GetEntityById(id)
    say("fraction_" .. id, e and not e:BeenDestroyed() and e:GetFractionComplete() or "gone")
end

function OrdersQueue(id)
    local u = GetEntityById(id)
    local s = ""
    for _, c in (u and not u:BeenDestroyed() and u:GetCommandQueue()) or {} do
        s = s .. " " .. tostring(c.commandType) .. ":" .. tostring(c.targetId)
    end
    say("queue_" .. id, s == "" and "empty" or string.sub(s, 2))
end

function OrdersSilo(a)
    local u = GetEntityById(a[1])
    if not u then
        say("silo", "none")
        return
    end
    local count = launches[a[1]] or {projectiles = 0, nukes = 0}
    launches[a[1]] = count
    if not count.hooked then
        count.hooked = true
        local onNuke = u.OnNukeLaunched
        u.OnNukeLaunched = function(self)
            count.nukes = count.nukes + 1
            if onNuke then return onNuke(self) end
        end
        for i = 1, u:GetWeaponCount() do
            local w = u:GetWeapon(i)
            local own = w.CreateProjectileAtMuzzle
            w.CreateProjectileAtMuzzle = function(self, muzzle)
                count.projectiles = count.projectiles + 1
                return own(self, muzzle)
            end
        end
    end
    if a[2] == "nuke" then u:GiveNukeSiloAmmo(a[3]) else u:GiveTacticalSiloAmmo(a[3]) end
    say("silo", u:GetNukeSiloAmmoCount() .. " " .. u:GetTacticalSiloAmmoCount())
end

function OrdersSiloState(id)
    local u = GetEntityById(id)
    local count = launches[id] or {projectiles = 0, nukes = 0}
    if not u or u:BeenDestroyed() then
        say("silo_" .. id, "gone")
        return
    end
    say("silo_" .. id, u:GetNukeSiloAmmoCount() .. " " .. u:GetTacticalSiloAmmoCount() .. " " .. count.projectiles .. " " .. count.nukes)
end

function OrdersEnhance(a)
    local u = GetEntityById(a[1])
    if u then u:CreateEnhancement(a[2]) end
    say("enhanced_" .. a[1], u and tostring(u:TestCommandCaps('RULEUCC_Teleport')) or "none")
end

function OrdersHeight(a)
    say("height_" .. a[1], GetSurfaceHeight(a[2], a[3]))
end

function OrdersBeacons(army)
    local s = ""
    for _, u in ArmyBrains[army]:GetListOfUnits(categories.FERRYBEACON, false) or {} do
        if not u:BeenDestroyed() then
            local p = u:GetPosition()
            s = s .. " " .. u:GetEntityId() .. ":" .. u:GetBlueprint().BlueprintId .. ":" .. p[1] .. ":" .. p[2] .. ":" .. p[3]
        end
    end
    say("beacons", s == "" and "none" or string.sub(s, 2))
end

function OrdersWhere(id)
    local u = GetEntityById(id)
    if not u or u:BeenDestroyed() then
        say("where_" .. id, "gone")
        return
    end
    local p = u:GetPosition()
    say("where_" .. id, p[1] .. " " .. p[3] .. " " .. (u:IsUnitState('Attached') and 1 or 0))
end
