-- FA Remastered's own AI: the navy (phase 10; docs/ai-architecture.md 2.2, 2.4; Chris's decision 22 in
-- docs/ai-design-goals.md). Part of FA Remastered, MIT licence (see LICENSE); our own design and code, nothing taken
-- from M28AI.
--
-- The duels against M28 on SETONS_R showed about half of its army at minute 18 at sea (18,000-21,000 mass of ships)
-- and most of what we lost shelled from the water, with nothing of ours able to answer. The navy, in a first form:
-- - HOME WATER: the water zone beside our base nearest the start (from the operations' coast list, R.coastWater).
-- - NAVAL FACTORIES: while the enemy has ships we know of (what we saw fades slowly, R.enemyNavalPeak) or ships
--   shell our coast, 1 naval factory, plus 1 per kFactoryMass of the enemy's ships (at most kMaxFactories); the site
--   is the first point on the line from the start to the home water's centre where the factory can stand.
-- - SHIPS: the naval factories build the ship with the best build value against the enemy's known ships (frigates,
--   submarines, destroyers, cruisers, battleships by the factory's tech; the tech's default with no ship known), while
--   our ships weigh less than kNavyMatch x the enemy's.
-- - OPERATIONS (every think of the army): the ships go for the ships shelling our coast when the battle estimate there
--   gives us K.defend, else for the nearest enemy ships by sea when it gives us K.launch, else they hold the home water.
--   Each change logs "ainavy <army> <tick> <what> zone <z> margin <m> | <reason>".
-- Determinism (docs/ai-architecture.md 5): zone ids and entity id order, ties by id; no random choice.

local kFactoryMass = 2000     -- one naval factory more per this much enemy ship mass known
local kMaxFactories = 4       -- naval factories at most
local kNavyMatch = 1.2        -- our ships kept up to this times the enemy's known ship mass
local kNavyFloor = 600        -- ... and at least this much while the enemy has ships
local kHorizon = 400          -- world units of travel that count as "can come to this fight"
local kSiteStep = 8           -- world units between the points tried for a naval factory site
local kSiteRange = 160        -- world units from the start: the naval factory sites looked at
local kShore = 24             -- world units: land this near a site is its shore
local kBaseTravel = 200       -- the shore's land zone within this travel of the start zone
-- (2026-11-03, Chris's big test on SETONS_R: "the players at the water should be building navy and expanding into the
-- water but they aren't" - the navy came only once enemy ships were seen) A NAVAL MAP for this army: our home water's
-- pond has water within kNavalReach of an enemy's start. Then, from kNavalFrom minutes, 1 naval factory (2 from
-- kNavalSecond), ships kept up to a floor growing kNavalGrow mass a minute after kNavalFrom (at most kNavalMax) whether
-- or not enemy ships are known, and ships with no ship fight to take go for the enemy's coast (sea control).
local kNavalReach = 250
local kNavalFrom = 5
local kNavalSecond = 12
local kNavalGrow = 150
local kNavalMax = 4000

-- the naval candidates by naval factory tech (blueprint suffixes): frigate, submarine; destroyer, cruiser; battleship
local kShips = { { 's0103', 's0203' }, { 's0201', 's0202' }, { 's0302' } }
local kDefault = { 's0103', 's0201', 's0302' }

local function Knobs(brain)
    return import("/lua/remastered/ai/personality.lua").Knobs(brain)
end

local function Valid(id)
    local bp = __blueprints[id]
    return bp and bp.Economy and (bp.Economy.BuildCostMass or 0) > 0
end

local function Pos(x, z) return { x, GetTerrainHeight(x, z), z } end

-- the enemy's ships as known ({id, count}) and their mass
local function EnemyShips(R)
    local list, mass = {}, 0
    for _, e in ipairs(Remastered.AIEnemyMix(R.army, true)) do
        local bp = __blueprints[e[1]]
        local naval, mobile = false, false
        for _, c in ipairs(bp and bp.Categories or {}) do
            if c == 'NAVAL' then naval = true end
            if c == 'MOBILE' then mobile = true end
        end
        if naval and mobile then
            table.insert(list, { e[1], e[2] })
            mass = mass + e.mass
        end
    end
    return list, mass
end

-- the home water: the water zone of the coast list nearest the start (zone id on ties); nil on a map without one
function HomeWater(brain)
    local R = brain.Remaster
    if R.homeWater then return R.homeWater end
    if not R.coastWater or not R.coastWater[1] then return nil end  -- (looked at again until our coast is known)
    local best, bestD
    for _, w in ipairs(R.coastWater) do
        local zone = Remastered.AIZone(w)
        if zone.domain == 'naval' then
            local dx, dz = zone.x - R.startX, zone.z - R.startZ
            local d = dx * dx + dz * dz
            if not bestD or d < bestD then best, bestD = w, d end
        end
    end
    R.homeWater = best
    return best
end

-- our ships' mass and the list (entity id order)
local function OwnShips(brain)
    local list, mass = {}, 0
    for _, u in ipairs(brain:GetListOfUnits(categories.MOBILE * categories.NAVAL, false)) do
        if not u.Dead and u:GetFractionComplete() >= 1 then
            table.insert(list, u)
            mass = mass + (u:GetBlueprint().Economy.BuildCostMass or 0)
        end
    end
    return list, mass
end

-- (2026-11-03) a naval map for this army (kNavalReach above), worked out once our coast is known: the start
-- positions are what every player knows from the lobby
function NavalMap(brain)
    local R = brain.Remaster
    if R.navalMap ~= nil then return R.navalMap end
    local home = HomeWater(brain)
    if not home then return false end
    local pond = Remastered.AIZone(home).area
    local info = Remastered.AIMapInfo()
    local found = false
    for i, b in ipairs(ArmyBrains) do
        if not found and IsEnemy(R.army, i) and not ArmyIsCivilian(i) then
            local ex, ez = b:GetArmyStartPos()
            for z = 1, info.zones do
                local zone = Remastered.AIZone(z)
                if zone.domain == 'naval' and zone.area == pond then
                    local dx, dz = zone.x - ex, zone.z - ez
                    if dx * dx + dz * dz <= kNavalReach * kNavalReach then
                        found = true
                        break
                    end
                end
            end
        end
    end
    R.navalMap = found
    LOG(string.format('ainavy %d %d naval map: %s (home water %d, pond %d)', R.army, R.tick,
        found and 'our water reaches an enemy start' or 'no enemy start on our water', home, pond))
    return found
end

-- how many naval factories we want now
function WantFactories(brain)
    local R = brain.Remaster
    if not HomeWater(brain) then return 0 end
    local peak = R.enemyNavalPeak or 0
    local minutes = GetGameTick() / 600
    local map = NavalMap(brain) and minutes >= kNavalFrom and (minutes >= kNavalSecond and 2 or 1) or 0
    if peak < 200 and (R.coastMass or 0) <= 0 then return map end
    return math.max(map, math.min(kMaxFactories, 1 + math.floor(peak / kFactoryMass)))
end

-- a naval factory site: water within kSiteRange of the start where the factory can stand and whose shore (land within
-- kShore) is a land zone our engineers reach within kBaseTravel of the start - nearest by that travel, then straight
-- distance, then grid order. The candidates are found once (a grid every kSiteStep); each call takes the first that is
-- still free. (The first form took the first water on the line to the home water: on SETONS_R that shore was a cliff,
-- and the engineer walked a long way round into the enemy and died.)
function FactorySite(brain)
    local R = brain.Remaster
    if not HomeWater(brain) then return nil end
    local bp = brain:Bp('b0103')
    if not R.navalSites then
        local list = {}
        for gx = -kSiteRange, kSiteRange, kSiteStep do
            for gz = -kSiteRange, kSiteRange, kSiteStep do
                local x, z = R.startX + gx, R.startZ + gz
                if Remastered.AIZoneAt(x, z, 'naval') > 0 and brain:CanBuildStructureAt(bp, Pos(x, z)) then
                    local best
                    for _, o in ipairs({ { kShore, 0 }, { -kShore, 0 }, { 0, kShore }, { 0, -kShore } }) do
                        local lz = Remastered.AIZoneAt(x + o[1], z + o[2], 'land')
                        if lz > 0 then
                            local d = Remastered.AIZoneDistance(R.startZone, lz, 'land')
                            if d >= 0 and d <= kBaseTravel and (not best or d < best) then best = d end
                        end
                    end
                    if best then
                        table.insert(list, { x = x, z = z, d = best, s = gx * gx + gz * gz, i = table.getn(list) + 1 })
                    end
                end
            end
        end
        table.sort(list, function(a, b)
            if a.d ~= b.d then return a.d < b.d end
            if a.s ~= b.s then return a.s < b.s end
            return a.i < b.i
        end)
        R.navalSites = list
        LOG(string.format('ainavy %d %d %d naval factory sites found by our shore', R.army, R.tick, table.getn(list)))
    end
    for _, c in ipairs(R.navalSites) do
        if not brain:Claimed(c.x, c.z) and brain:CanBuildStructureAt(bp, Pos(c.x, c.z)) then return { c.x, c.z } end
    end
    return nil
end

-- what an idle naval factory of `tech` builds, or nil (our ships are enough)
function FactoryPick(brain, tech)
    local R = brain.Remaster
    local ships, enemyMass = EnemyShips(R)
    local _, ownMass = OwnShips(brain)
    local want = math.max(kNavyMatch * (R.enemyNavalPeak or enemyMass), (R.enemyNavalPeak or 0) > 0 and kNavyFloor or 0)
    if NavalMap(brain) then  -- (2026-11-03: sea control on a naval map, whether or not enemy ships are known)
        want = math.max(want, math.min(kNavalMax, kNavyFloor + kNavalGrow * math.max(0, GetGameTick() / 600 - kNavalFrom)))
    end
    if ownMass >= want then return nil end
    local best, bestV
    if ships[1] then
        for t = tech, 1, -1 do
            for _, sfx in ipairs(kShips[t] or {}) do
                local id = brain:Bp(sfx)
                if Valid(id) then
                    local v = Remastered.AIBuildValue(id, ships)
                    if v > 0 and (not bestV or v > bestV or (v == bestV and id < best)) then best, bestV = id, v end
                end
            end
        end
    end
    if not best then
        best = brain:Bp(kDefault[math.min(tech, 3)])
        if not Valid(best) then best = brain:Bp('s0103') end
    end
    if R.tick - (R.navyPickLogged or -10000) >= 600 then
        LOG(string.format('ainavy %d %d pick %s value %.2f | ours %.0f mass, enemy ships %.0f known (peak %.0f)', R.army,
            R.tick, tostring(best), bestV or 0, ownMass, enemyMass, R.enemyNavalPeak or 0))
        R.navyPickLogged = R.tick
    end
    return best
end

-- (2026-11-03) ship orders replace the queue and are given again only when they change or a ship stopped away from
-- the point: the ships got IssueAggressiveMove appended every think (the land army's stale-order pile, operations.lua)
local function ShipOrder(R, ships, pos, key)
    R.shipOrder = R.shipOrder or {}
    local keep, give = {}, {}
    for _, u in ipairs(ships) do
        local id = u:GetEntityId()
        keep[id] = R.shipOrder[id]
        if keep[id] ~= key then
            table.insert(give, u)
        elseif u:IsIdleState() then
            local p = u:GetPosition()
            local dx, dz = p[1] - pos[1], p[3] - pos[3]
            if dx * dx + dz * dz > 30 * 30 then table.insert(give, u) end
        end
    end
    for _, u in ipairs(give) do keep[u:GetEntityId()] = key end
    R.shipOrder = keep
    if give[1] then
        IssueClearCommands(give)
        IssueAggressiveMove(give, pos)
    end
end

local function Log(R, what, zone, margin, reason)
    if R.navyState == what .. zone then return end
    R.navyState = what .. zone
    LOG(string.format('ainavy %d %d %s zone %d margin %.2f | %s', R.army, R.tick, what, zone, margin, reason))
end

-- the ships' orders (every think of the army)
function Think(brain)
    local R = brain.Remaster
    local home = HomeWater(brain)
    if not home then return end
    local ships = OwnShips(brain)
    if not ships[1] then return end
    local K = Knobs(brain)
    -- 1. the ships shelling our coast
    local t = R.coastThreat and R.coastThreat[1]
    if t then
        local margin = Remastered.AIZoneEstimate(R.army, t.zone, kHorizon)
        if margin >= K.defend then
            Log(R, 'defend', t.zone, margin, 'enemy ships shell our coast; the estimate lets us take them')
            local zone = Remastered.AIZone(t.zone)
            ShipOrder(R, ships, Pos(zone.x, zone.z), 'd' .. t.zone)
            return
        end
    end
    -- 2. the nearest enemy ships by sea
    local info = Remastered.AIMapInfo()
    local best, bestD
    for z = 1, info.zones do
        local f = Remastered.AIZoneForces(R.army, z)
        if f.enemy.naval.count > 0 then
            local d = Remastered.AIZoneDistance(home, z, 'naval')
            if d >= 0 and (not bestD or d < bestD) then best, bestD = z, d end
        end
    end
    if best then
        local margin = Remastered.AIZoneEstimate(R.army, best, kHorizon)
        if margin >= K.launch then
            Log(R, 'attack', best, margin, 'enemy ships within reach by sea; the estimate favours us')
            local zone = Remastered.AIZone(best)
            ShipOrder(R, ships, Pos(zone.x, zone.z), 'a' .. best)
            return
        end
    end
    -- 2b. (2026-11-03) sea control on a naval map: the water zone of our pond beside an enemy coastal zone with
    -- structures, nearest our home water by sea, when the estimate there favours us; the ships shell what they reach
    if NavalMap(brain) then
        local pond = Remastered.AIZone(home).area
        local tgt, tgtD
        for z = 1, info.zones do
            local zone = Remastered.AIZone(z)
            if zone.domain == 'naval' and zone.area == pond then
                local d = Remastered.AIZoneDistance(home, z, 'naval')
                if d >= 0 and (not tgtD or d < tgtD) then
                    for _, nb in ipairs(Remastered.AIZoneNeighbours(z)) do
                        if nb.coast and Remastered.AIZoneForces(R.army, nb.zone).enemy.structure.count > 0 then
                            tgt, tgtD = z, d
                            break
                        end
                    end
                end
            end
        end
        if tgt then
            local margin = Remastered.AIZoneEstimate(R.army, tgt, kHorizon)
            if margin >= K.launch then
                Log(R, 'coast', tgt, margin, 'sea control: an enemy coast within reach by sea; the estimate favours us')
                local zone = Remastered.AIZone(tgt)
                ShipOrder(R, ships, Pos(zone.x, zone.z), 'c' .. tgt)
                return
            end
        end
    end
    -- 3. hold the home water (all of them when the state changes, else the idle ones)
    Log(R, 'hold', home, 0, 'no fight at sea the estimate favours: the ships hold our coast')
    local zone = Remastered.AIZone(home)
    ShipOrder(R, ships, Pos(zone.x, zone.z), 'h' .. home)
end
