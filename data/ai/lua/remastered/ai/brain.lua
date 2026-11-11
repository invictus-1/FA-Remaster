-- FA Remastered's own AI: the brain (docs/ai-architecture.md). Part of FA Remastered, MIT licence (see LICENSE); our
-- own design and code, nothing taken from M28AI.
--
-- Phase 3: the brain's own random stream and think thread, its options, Chris's rating corrections, and a first
-- economy built on the map analysis: the commander and the engineers take mass points by travel distance, power when
-- energy runs short, factories as income allows; the factories make engineers and a land mix; the army holds the base
-- and answers enemies in its own zones. Phase 4 added the operations (operations.lua). Phase 5 drives the economy at
-- the same time as scouting, the army and expansion, with the build choices in choices.lua (counters, defences,
-- investments), the strategy layer in strategy.lua and the knobs in personality.lua.
--
-- Determinism (docs/ai-architecture.md 5): the thread wakes on fixed ticks; unit lists are in entity id order (the
-- engine's GetListOfUnits) and every walk over a table keyed by strings is sorted first; every random choice is the
-- army's own stream (Remastered.AIRandom), never the sim's Random; no clock is read.
local StandardBrain = import("/lua/aibrain.lua").AIBrain

local kPrefix = { 'ue', 'ua', 'ur', 'xs' }  -- blueprint id prefixes by faction index (UEF, Aeon, Cybran, Seraphim)
local kThink = 10                             -- ticks between economy steps
local kClaimTicks = 600                       -- a build site promised to a builder stays its own this long
local kAssistTicks = 200                      -- an assisting builder is looked at again after this long
local kReclaimCell = 64                       -- world units: the cells of the reclaim map (phase 6)
local kReclaimMin = 5                         -- a prop counts for reclaim from this much mass (trees, rocks, wrecks)
local kReclaimField = 100                     -- a cell with this much reclaim is a field worth walking to
local kReclaimPerWorker = 300                 -- a field takes one engineer per this much mass (plus one)
local kReclaimWorkerTicks = 450               -- an engineer sent to a field counts there this long
local kReclaimNear = 350                      -- world units from the start: the reclaim that adds engineers
-- (2026-11-06, Chris's re-test: engineers of every army scattered over the sea and in enemy bases "to I guess reclaim".
-- His rule: reclaim only in controlled space - ReclaimControl and ReclaimField)
local kCtrlRadius = 120       -- world units around a zone's centre: the area whose force and enemies count for it
local kCtrlOn = 0             -- our and allied land force there, in T1 tanks' ratings, that makes a zone controlled
local kCtrlOff = 0            -- ... and under which a controlled zone loses the flag (hysteresis). (Both 0: a force
                              -- asked of 6 / 3 and of 3 / 1.5 tanks cut the duels' reclaim by up to 30% and the duels
                              -- ended earlier; no hostile sight nearby and no enemy start near are what keep them safe)
local kCtrlSeen = 600         -- ticks: an enemy seen in the area this recently keeps the zone uncontrolled
local kCtrlBase = 350         -- world units of land travel from the start (kReclaimNear): zones that need no force
local kCtrlStep = 50          -- ticks between two looks at the flags
local kCtrlEnemyBase = 220    -- world units around an enemy start that are never controlled (seen or not)
local kTripFree = 150         -- world units of a reclaim trip that need no more than kReclaimField
local kTripPerUnit = 1.0      -- mass a cell must hold more per world unit of the trip beyond kTripFree
local kTripDetour = 1.4       -- a trip whose land travel is more than this x the straight line (+ kTripSlack) is a
local kTripSlack = 60         -- ... sea shortcut for an amphibious or hover engineer: not taken
local kTripZoneMax = 3        -- engineers on reclaim trips in one zone at most
-- (the economy's knobs of phases 11-13 live in personality.lua's kEconomy, with ranges: read once here)
local kTuning = import("/lua/remastered/ai/personality.lua").Tuning()
local kReclaimPush = kTuning.reclaimPush       -- reclaim near the start per engineer built and reclaiming (phase 13)
local kReclaimEngineersMax = kTuning.reclaimEngineersMax  -- ... at most this many (phase 13; 20 before)
local kReclaimPerEngineer = 400               -- one engineer more per this much reclaim near the start (at most 20)
local kGroundFade = 0.998                      -- what was seen of the enemy's ground army fades by this a step (10 ticks)
local kEngineerAvoid = 600                    -- ticks an enemy seen in a zone or beside it keeps engineers out (phase 7)
local kAcuRetreat = 0.8                       -- the commander's health share under which it falls back (phase 7)
local kAcuBack = 0.95                         -- ... and the share at which it goes back to work
local kAcuRepairers = 3                       -- engineers that repair a hurt commander
-- (2026-11-03, ExperimentalJob) from: minutes; every: minutes between more; income: mass a second at least; stored:
-- the mass bar share that counts as piling up; pending: ticks an order counts as under way before its experimental
-- starts (the engineer's walk)
-- (2026-11-04, item 1 of the coordinator's round: the mass sinks) value: an experimental is built instead of units
-- when its build value is at least this share of the best T3 unit's (else only from piling mass or at the hard cap);
-- horizon: the base's estimate (world units of travel) that must let us hold it while one is built; seconds: it costs
-- at most this many seconds of the income now
local kExp = { from = 14, every = 6, income = 50, stored = 0.3, pending = 900, value = 0.5, horizon = 400, seconds = 300 }
local kAcuDanger = 80                         -- world units: enemies this near a hurt commander are what it backs away from
-- (2026-11-03, RemasterCommander) fight: known enemy land units this near it (least: at least this much mass of them,
-- not a lone scout) are fought when the estimate there is at least start (keep: it fights on while at least this) and
-- its health at least kAcuRetreat to start (hard: under this it falls back whatever the estimate); away: world units
-- from the start beyond which it does not fight (it defends its base, it does not hunt); naval: enemy ships' rating
-- against ground that reaches its zone sends it inland; horizon: the estimate's travel horizon
local kAcu = { fight = 30, least = 40, start = 0.6, keep = 0.45, hard = 0.7, away = 80, naval = 4, horizon = 150,
    home = 60, homeFrom = 10 }
-- (2026-11-04, item 3) home: world units from the start the commander's jobs keep within from minute homeFrom (see
-- CommanderJobSafe)
local kBaseRadius = 150                       -- world units from the start: a builder this near takes base jobs
local kArmyFrom = 5                           -- the standing army's floor starts at this minute (phase 7: before it the
                                              -- factories make engineers for the expansion, unless the enemy's army is seen)
local kWalkSpeed = 1.5                         -- world units a second an engineer's walk to a claimed site counts at
local kTechEngineers = 2                      -- engineers of T2 or above kept once we or the enemy are at T2 (phase 7b)
local kReclaimEarly = 0.5                     -- the share of the engineers reclaiming in the opening (phase 7b)
local kReclaimEarlyUntil = 8                  -- ... until this minute
local kEnergyShort = 0.3                      -- stored energy share under which the opening counts energy as short
local kOpeningPower = 2                       -- the opening's power floor: this many generators from the start ...
local kOpeningPowerRate = 2.2                 -- ... plus this many a minute (phase 8)
local kOpeningMexRange = 300                  -- world units: the opening's extractors within this of the engineer come first
local kCommanderPower = 2                     -- generators the commander builds right after the first factory (phase 8)
local kBaseZones = 200                        -- world units of travel from the start zone: base structures go there (phase 8)
local kArmyIncomeRate = 10                    -- the army kept: seconds of mass income per minute after the opening (9)
local kArmyIncomeMax = 120                    -- ... at most this many seconds of income
local kMaxFactories = 20                      -- land factories at most (phase 9; 12 before)
local kFactoryEvery = kTuning.factoryEvery    -- minutes per extra land factory of the floor after the opening (11)
local kOpening = 6                            -- minutes of the opening (phase 8): the energy rules below hold until then
local kTechEngineersMax = 8                   -- engineers of T2 or above kept at most (phase 9)
local kUpgradeEnergy = kTuning.upgradeEnergy  -- energy bar share a new land HQ upgrade waits for (phase 13)
local kT1FactoryFloat = kTuning.t1FactoryFloat  -- ... more T1 factories all the same with this much of the mass bar
local kT1FactoriesWithHQ = kTuning.t1FactoriesWithHQ  -- T1 land factories T1 builders add once a T2 HQ stands (13)
local kTechEngineersHQ = kTuning.techEngineersHQ  -- engineers of T2 or above kept at most with a T2 land HQ (13)
local kUpgradeIncome = 60                     -- mass a second per extra factory upgrade at once (phase 10)
local kEnergyPerMass = kTuning.energyPerMass  -- energy income kept ahead per mass income after the opening (11)
local kAllyBase = 150                         -- land travel from an ally's start: its base, whose mass points are its own
local kNavalAirShare = 1.5                    -- anti-ship aircraft kept per mass of enemy ships shelling our coast (10)
local kNavalAirFactory = 1500                 -- one air factory more per this much enemy ship mass there
local kNavalPendingTicks = 600                -- a naval factory ordered counts as under way this long before it starts
local kMassNear = 120                         -- world units: a free mass point this near an engineer comes before reclaim
-- (2026-11-04, item 1: the mass sinks) the T1 land factories' upgrade to support factories once an HQ stands: from:
-- minutes; income: mass a second per extra upgrade at once (1 + ...); share: at most this share of the land factories
-- upgrading at once; stored: the mass bar share from which they go while the army is short
local kSupport = { from = 10, income = 80, share = 0.25, stored = 0.5 }

local fixesApplied = false  -- (one game per sim state: the module lives with the state)

-- Chris's corrections (rating_fixes.lua) into the engine, once, ids in sorted order
local function ApplyRatingFixes()
    if fixesApplied then return end
    fixesApplied = true
    local fixes = import("/lua/remastered/ai/rating_fixes.lua")
    local ids = {}
    for id, _ in pairs(fixes.Fix or {}) do table.insert(ids, id) end  -- (sorted below)
    table.sort(ids)
    for _, id in ipairs(ids) do Remastered.AISetRatingFix(id, fixes.Fix[id]) end
    for _, b in ipairs(fixes.Beats or {}) do Remastered.AISetBeats(b[1], b[2], b[3]) end
end

-- (2026-11-03, perf) what all our AI's armies share in this Lua state: the reclaim map's mass per cell (ReclaimScan)
local kShared = {}

local function Alive(u)
    return u and not u.Dead and u:GetFractionComplete() >= 1
end

local function Pos(x, z)
    return { x, GetTerrainHeight(x, z), z }
end

local function Choices()  -- (the build choices of phase 5: counters, defences, investments)
    return import("/lua/remastered/ai/choices.lua")
end

local function Navy()  -- (the navy of phase 10)
    return import("/lua/remastered/ai/navy.lua")
end

AIBrain = Class(StandardBrain) {

    RemasterAI = true,

    OnCreateAI = function(self, planName)
        StandardBrain.OnCreateAI(self, planName)
        -- the slot's options (the lobby's PlayerOptions reach the sim as ScenarioInfo.ArmySetup[army]); personality and
        -- teacher mode are read now and used from phases 5 / 6
        local setup = ScenarioInfo.ArmySetup and ScenarioInfo.ArmySetup[self.Name] or {}
        self.RemasterOptions = {
            personality = setup.RemasterPersonality or 'random',
            teacher = setup.RemasterTeacher == 'on' or setup.RemasterTeacher == true,
            vision = setup.RemasterVision == 'on' or setup.RemasterVision == true,
            -- the player's sliders (phase 6, personality.lua): -2 .. 2, 0 by default
            aggression = setup.RemasterAggression,
            economy = setup.RemasterEconomy,
        }
        LOG('FA Remastered AI: army ' .. tostring(self:GetArmyIndex()) .. ' ' .. tostring(self.Name) .. ' (' ..
            tostring(setup.AIPersonality) .. ')')
        self:ForkThread(self.RemasterMain)
    end,

    -- The think thread. The index and the map exist from the end of the session's start, so the first wake (beat 1 on)
    -- is where the one-time set-up goes.
    RemasterMain = function(self)
        local army = self:GetArmyIndex()
        WaitTicks(1 + math.mod(army, kThink))  -- (the armies' steps fall on different beats)
        ApplyRatingFixes()
        if self.RemasterOptions.vision then Remastered.AISetVision(army, true) end
        local sx, sz = self:GetArmyStartPos()
        local prefix = kPrefix[self:GetFactionIndex()] or 'ue'
        self.Remaster = {
            army = army,
            prefix = prefix,
            startX = sx,
            startZ = sz,
            startZone = Remastered.AIZoneAt(sx, sz, 'land'),
            claims = {},   -- 'x:z' -> tick the site was promised
            sites = {},    -- (phase 11) base sites promised: { x, z, hx, hz (half the skirt), tick }, in promise order
            assist = {},   -- builder entity id -> tick its assist began
            tick = 0,
        }
        self:RemasterMassPoints()
        -- (2026-11-03) the land combat units at which T1 land factories / all land factories stop making them: shares
        -- of the lobby's unit cap (kTuning.landSoftShare / landHardShare; 1000 when the option is missing)
        local cap = tonumber(ScenarioInfo.Options and ScenarioInfo.Options.UnitCap) or 1000
        self.Remaster.landSoft = math.floor(kTuning.landSoftShare * cap)
        self.Remaster.landHard = math.floor(kTuning.landHardShare * cap)
        while not ArmyIsOutOfGame(army) do
            self.Remaster.tick = GetGameTick()
            self:RemasterEconomy()
            -- (2026-11-03, perf: the army's think - strategy, operations, navy - ran on one beat every 50 ticks beside
            -- the economy step, 40-55 ms in Chris's 4v4 at minutes 35-50; now one part on each of the army's next three
            -- economy steps, 10 ticks apart)
            local step = math.floor(math.mod(self.Remaster.tick, 50) / kThink)
            if step < 3 then self:RemasterArmy(step) end
            WaitTicks(kThink)
        end
    end,

    -- The mass points this army goes for, nearest first by land travel from its start zone (the map analysis), ties by
    -- zone and then by the point's place in the zone; points on islands it cannot walk to are left out
    RemasterMassPoints = function(self)
        local R = self.Remaster
        local list = {}
        local info = Remastered.AIMapInfo()
        -- (2026-11-10, Chris's rule of 2026-11-04, design goal 31: "with allies present, the AI does not build in its
        -- allies' bases". An ally's base: the zones within kAllyBase of land travel of its start that are nearer to its
        -- start than to ours. Their mass points are the ally's: left out of our list, so no engineer of ours is sent
        -- there. With no ally the list is as before.)
        local allyStarts = {}
        for i, b in ipairs(ArmyBrains) do
            if i ~= R.army and IsAlly(R.army, i) and not ArmyIsCivilian(i) then
                local ax, az = b:GetArmyStartPos()
                local zone = Remastered.AIZoneAt(ax, az)
                if zone > 0 then table.insert(allyStarts, zone) end
            end
        end
        local left = 0
        for z = 1, info.zones do
            local d = R.startZone > 0 and Remastered.AIZoneDistance(R.startZone, z, 'land') or -1
            local allied = false
            for _, az in ipairs(allyStarts) do
                local da = Remastered.AIZoneDistance(az, z, 'land')
                if da >= 0 and da <= kAllyBase and da < d then allied = true end
            end
            if d >= 0 then
                local zone = Remastered.AIZone(z)
                for i, p in ipairs(zone.massPoints) do
                    if allied then
                        left = left + 1
                    else
                        table.insert(list, { x = p[1], z = p[2], zone = z, i = i, dist = d })
                    end
                end
            end
        end
        if allyStarts[1] then
            LOG(string.format('aimex %d %d allies: %d mass points in %d allies\' bases are left to them', R.army, R.tick or 0,
                left, table.getn(allyStarts)))
        end
        table.sort(list, function(a, b)
            if a.dist ~= b.dist then return a.dist < b.dist end
            if a.zone ~= b.zone then return a.zone < b.zone end
            return a.i < b.i
        end)
        for _, p in ipairs(list) do p.key = p.x .. ':' .. p.z end  -- (the claims' key, Claimed)
        R.massPoints = list
    end,

    -- (a suffix starting with '!' is a whole blueprint id: the support factories, whose ids follow no faction prefix)
    Bp = function(self, suffix)
        if string.sub(suffix, 1, 1) == '!' then return string.sub(suffix, 2) end
        return self.Remaster.prefix .. suffix
    end,

    -- The land factory a builder adds (phase 13): with a T2 or T3 land HQ, a support factory of the builder's tech (at
    -- most the HQ's) - 580 mass at T2 against 1,410 for an HQ, and a T2 factory from the start, so the army's tier
    -- rises without stopping factories to upgrade them (upgrading many at once lost the M28 games, phases 11-12). A T1
    -- builder then adds a T1 factory only while we have fewer than kT1FactoriesWithHQ: M28 kept 7-8 T1 beside 7-8 T2
    -- where we spammed 29-38 T1 - unless mass piles up (kT1FactoryFloat of the bar): with the T1 builders held at 6
    -- factories and few T2 engineers, 3,000-7,300 mass overflowed in 3 of 4 judge games. nil: no factory from it.
    LandFactoryBp = function(self, u, ctx)
        local hq = ctx.landHQ or 0
        local tech = math.min(self:Tech(u), hq)
        if tech >= 2 then
            local letter = string.sub(self.Remaster.prefix, 2, 2)
            local id = 'z' .. letter .. (tech >= 3 and 'b9601' or 'b9501')
            local bp = __blueprints[id]
            if bp and bp.Economy and (bp.Economy.BuildCostMass or 0) > 0 then return '!' .. id end
        end
        if hq >= 2 and ctx.landFactories >= kT1FactoriesWithHQ and
            (self:GetEconomyStoredRatio('MASS') < kT1FactoryFloat or self.Remaster.upgradesOpen) then
            return nil
        end
        return 'b0101'
    end,

    Claimed = function(self, x, z)
        local t = self.Remaster.claims[x .. ':' .. z]
        return t and self.Remaster.tick - t < kClaimTicks
    end,

    -- (phase 7: `walker` adds the walk to the site, at kWalkSpeed, to the claim: the duels' engineers walked 100-300
    -- world units to far extractors, longer than kClaimTicks, and a second engineer was sent to the same point)
    Claim = function(self, x, z, walker)
        local extra = 0
        if walker then
            local q = walker:GetPosition()
            extra = math.floor(10 * math.sqrt((x - q[1]) * (x - q[1]) + (z - q[3]) * (z - q[3])) / kWalkSpeed)
        end
        self.Remaster.claims[x .. ':' .. z] = self.Remaster.tick + extra
    end,

    -- the next mass point that is free (no extractor, not promised to another builder): without a builder the first
    -- by travel from the start; with one (phase 5) the nearest to it in a straight line, leaving out the zones the
    -- enemy is known in (R.enemyAt, kept by the operations), list order on ties
    -- (2026-11-03, perf: once a scan without a builder finds no free point, none is free for the rest of the step -
    -- claims only grow within it and no extractor starts inside a Lua step - so R.noFreeMass answers at once; the claim
    -- keys are the points' own, p.key, made once. The game's builders had scanned all ~100 points twice each, every
    -- step, once the map was taken.)
    NextMassPoint = function(self, u)
        local bp = self:Bp('b1103')
        local R = self.Remaster
        if R.noFreeMass == R.tick then return end
        local claims = R.claims
        if not u then
            for _, p in ipairs(R.massPoints) do
                local t = claims[p.key]
                if not (t and R.tick - t < kClaimTicks) and self:CanBuildStructureAt(bp, Pos(p.x, p.z)) then return p end
            end
            R.noFreeMass = R.tick
            return
        end
        -- (once a step, the scan without a builder first: when no point is free at all, none is for this builder)
        if R.massChecked ~= R.tick then
            R.massChecked = R.tick
            if not self:NextMassPoint() then return end
        end
        local q = u:GetPosition()
        local best, bestD
        -- (2026-11-03, perf: the distance first - a point no nearer than the best so far needs no other test - then
        -- the zone's safety, the claim and the engine's test; the same point comes out)
        for _, p in ipairs(R.massPoints) do
            local dx, dz = p.x - q[1], p.z - q[3]
            local d = dx * dx + dz * dz
            local t = (not bestD or d < bestD) and claims[p.key]
            if (not bestD or d < bestD) and not self:Unsafe(p.zone) and not (t and R.tick - t < kClaimTicks) and
                self:CanBuildStructureAt(bp, Pos(p.x, p.z)) then
                best, bestD = p, d
            end
        end
        return best
    end,

    -- A zone an engineer should not walk into (phase 7): the enemy is known there now, or was seen there or in a land
    -- neighbour within kEngineerAvoid (R.enemySeen, kept by the operations). The start zone is never unsafe. The duels'
    -- kill counts showed why: against FAF's AI 5,700 mass of our engineers died to its T1 tanks by minute 20, walking
    -- to far mass points and reclaim the enemy had just been seen beside (only the zones it was known in now were
    -- left out).
    Unsafe = function(self, z)
        local R = self.Remaster
        if z == R.startZone then return false end
        if R.enemyAt and R.enemyAt[z] then return true end
        local seen = R.enemySeen or {}
        if seen[z] and R.tick - seen[z] < kEngineerAvoid then return true end
        R.landNeighbours = R.landNeighbours or {}
        local list = R.landNeighbours[z]
        if not list then
            list = {}
            for _, n in ipairs(Remastered.AIZoneNeighbours(z)) do
                if not n.coast then table.insert(list, n.zone) end
            end
            R.landNeighbours[z] = list
        end
        for _, n in ipairs(list) do
            if seen[n] and R.tick - seen[n] < kEngineerAvoid then return true end
        end
        return false
    end,

    -- a free build spot of at least `size` in the start zone for blueprint suffix `what`
    -- (phase 8: when the start zone is full, the land zones within kBaseZones of travel from it, nearest first, zone
    -- id on ties: the opening's power floor filled the start zone, and power then could not be built at all)
    BuildSpot = function(self, what, size)
        local R = self.Remaster
        if R.startZone <= 0 then return end
        if not R.baseZones then
            local list = {}
            local info = Remastered.AIMapInfo()
            for z = 1, info.zones do
                local d = Remastered.AIZoneDistance(R.startZone, z, 'land')
                if d >= 0 and d <= kBaseZones then table.insert(list, { z = z, d = d }) end
            end
            table.sort(list, function(a, b)
                if a.d ~= b.d then return a.d < b.d end
                return a.z < b.z
            end)
            R.baseZones = {}
            for _, e in ipairs(list) do table.insert(R.baseZones, e.z) end
        end
        local bp = self:Bp(what)
        -- (phase 11: a spot overlapping a site promised to another builder is not free either: the spots are 4 world
        -- units apart, a factory's skirt 8 or more, and the next builder was sent 4 units beside the last one's
        -- factory - against M28, 106 factory orders in 2 minutes started 13 factories, with the mass bar full)
        local live = {}
        for _, c in ipairs(R.sites) do
            if R.tick - c.tick < kClaimTicks then table.insert(live, c) end
        end
        R.sites = live
        local hx, hz = self:HalfSkirt(bp, size)
        -- (2026-11-03, Chris's big test: "builds too spread out, it should be using adjacency and building at the
        -- starting point". The spots were taken in each zone's grid order - the zone's first row first - so the
        -- factory and the generators could stand 30-50 world units from the start's extractors, in rows. Now the
        -- spots of all base zones go nearest first from an anchor: the start for everything, and for power our first
        -- factory, so generators ring it (adjacency: a factory beside power pays less energy). The sorted list is
        -- kept per size and anchor: the spots are terrain only.)
        local ax, az = R.startX, R.startZ
        if (what == 'b1101' or what == 'b1201' or what == 'b1301') and R.factoryAt then ax, az = R.factoryAt[1], R.factoryAt[2] end
        R.spotCache = R.spotCache or {}
        local key = size .. ':' .. math.floor(ax) .. ':' .. math.floor(az)
        local list = R.spotCache[key]
        if not list then
            list = {}
            for _, z in ipairs(R.baseZones) do
                for _, q in ipairs(Remastered.AIBuildSpots(z, size)) do
                    local dx, dz = q[1] - ax, q[2] - az
                    table.insert(list, { q[1], q[2], d = dx * dx + dz * dz, i = table.getn(list) + 1 })
                end
            end
            table.sort(list, function(m, n)
                if m.d ~= n.d then return m.d < n.d end
                return m.i < n.i
            end)
            R.spotCache[key] = list
        end
        for _, q in ipairs(list) do
            if not self:Claimed(q[1], q[2]) and not self:SiteTaken(q[1], q[2], hx, hz) and
                self:CanBuildStructureAt(bp, Pos(q[1], q[2])) then
                return { q[1], q[2] }
            end
        end
    end,

    -- (phase 11) half a blueprint's skirt (its footprint, else the spot size) on each axis
    HalfSkirt = function(self, bp, size)
        local b = __blueprints[bp]
        local ph = b and b.Physics or {}
        local fp = b and b.Footprint or {}
        return 0.5 * (ph.SkirtSizeX or fp.SizeX or size), 0.5 * (ph.SkirtSizeZ or fp.SizeZ or size)
    end,

    -- (phase 11) a promised base site overlaps a structure of half-skirt hx, hz centred at x, z
    SiteTaken = function(self, x, z, hx, hz)
        for _, c in ipairs(self.Remaster.sites) do
            if math.abs(c.x - x) < c.hx + hx and math.abs(c.z - z) < c.hz + hz then return true end
        end
        return false
    end,

    -- energy (phase 5): short now, or less than 1.2x what is asked plus a margin (built ahead of need), but never more
    -- than 12 energy per mass of income plus 30 a second while some is stored (the opening's mass comes first)
    NeedPower = function(self)
        local income = self:GetEconomyIncome('ENERGY')
        local requested = self:GetEconomyRequested('ENERGY')
        local stored = self:GetEconomyStoredRatio('ENERGY')
        -- (phase 9: the cap first, and an empty bar counts only while the asks are above what comes in: the bar empties
        -- for a moment when a big build starts, with income far ahead)
        -- (phase 11: the cap counts the smoothed mass income too: an energy stall cuts the mass income, which cut the
        -- cap, which stopped the power - 1,040 energy a second against 1,522 asked, "not needed")
        local mass = math.max(self:GetEconomyIncome('MASS') * 10, self.Remaster.massAvg or 0)
        if income * 10 > 12 * mass + 30 then return false end
        -- (phase 11: after the opening, power ahead for what the mass income can spend, kEnergyPerMass energy a mass:
        -- against M28 the energy bar was empty most of minutes 8.5-13 while the asks ran at 9-10 energy a mass and the
        -- T1 generators came 10-15 at a time behind them)
        if GetGameTick() >= kOpening * 600 and income * 10 < kEnergyPerMass * mass then return true end
        if stored < 0.15 then return income < requested * 1.2 end
        return income < requested * 1.2 + 1 and stored < 0.95
    end,

    -- mass storage (phase 5): a free site beside one of the start zone's extractors (the adjacency bonus), the
    -- extractors in entity id order, the four sides in a fixed order
    StorageSpot = function(self)
        local bp = self:Bp('b1106')
        local R = self.Remaster
        for _, m in ipairs(self:GetListOfUnits(categories.MASSEXTRACTION * categories.STRUCTURE, false)) do
            if Alive(m) then
                local q = m:GetPosition()
                if Remastered.AIZoneAt(q[1], q[3], 'land') == R.startZone then
                    for _, o in ipairs({ { 2, 0 }, { -2, 0 }, { 0, 2 }, { 0, -2 } }) do
                        local x, z = q[1] + o[1], q[3] + o[2]
                        if not self:Claimed(x, z) and self:CanBuildStructureAt(bp, Pos(x, z)) then return { x, z } end
                    end
                end
            end
        end
    end,

    Tech = function(self, u)
        return EntityCategoryContains(categories.TECH3, u) and 3 or EntityCategoryContains(categories.TECH2, u) and 2 or 1
    end,

    -- Base jobs (power, factories, radar, storage, anti-missile, assists) go only to builders within kBaseRadius of
    -- the start, and the commander (phase 7). The duels' engineer queues showed why: at minute 10, 26 of ~45
    -- engineers were walking more than 100 world units to a single build, most of them from the field back to the
    -- base, while the extractors waited; FAF's AI had 29 extractors at minute 8 to our 10.
    AtBase = function(self, u, strict)  -- (strict: the commander too must be within kBaseRadius)
        if not strict and EntityCategoryContains(categories.COMMAND, u) then return true end
        local R = self.Remaster
        local q = u:GetPosition()
        local dx, dz = q[1] - R.startX, q[3] - R.startZ
        return dx * dx + dz * dz <= kBaseRadius * kBaseRadius
    end,

    -- build `what` at the free spot of at least `size` in the start zone nearest (x, z) (spot order on ties)
    BuildNear = function(self, u, what, size, x, z)
        local R = self.Remaster
        if R.startZone <= 0 or not self:AtBase(u) then return false end
        local bp = self:Bp(what)
        local best, bestD
        for _, p in ipairs(Remastered.AIBuildSpots(R.startZone, size)) do
            local dx, dz = p[1] - x, p[2] - z
            local dd = dx * dx + dz * dz
            if (not bestD or dd < bestD) and not self:Claimed(p[1], p[2]) and self:CanBuildStructureAt(bp, Pos(p[1], p[2])) then
                best, bestD = p, dd
            end
        end
        if not best then return false end
        self:Claim(best[1], best[2])
        IssueBuildMobile({ u }, Pos(best[1], best[2]), bp, {})
        return true
    end,

    -- build `what` (a blueprint suffix) at a free spot of at least `size` in the start zone
    BuildAt = function(self, u, what, size)
        if not self:AtBase(u) then return false end
        -- (2026-11-03, Chris's big test: "sometimes builds too many things at once, causing issues with the eco": a
        -- new base structure other than power waits while R.unfinished of them are being built, at most 2 + one per
        -- 20 mass a second of income, unless mass piles up (30% of the bar); power has its own limits below)
        local R = self.Remaster
        local power = what == 'b1101' or what == 'b1201' or what == 'b1301'
        if not power and (R.unfinished or 0) >= (R.unfinishedMax or 1e9) and self:GetEconomyStoredRatio('MASS') < 0.3 then
            return false
        end
        local s = self:BuildSpot(what, size)
        if not s then return false end
        self:Claim(s[1], s[2], u)
        -- (phase 11: the site's whole skirt is promised, for the walk too)
        local hx, hz = self:HalfSkirt(self:Bp(what), size)
        table.insert(self.Remaster.sites,
            { x = s[1], z = s[2], hx = hx, hz = hz, tick = self.Remaster.claims[s[1] .. ':' .. s[2]] })
        IssueBuildMobile({ u }, Pos(s[1], s[2]), self:Bp(what), {})
        R.unfinished = (R.unfinished or 0) + 1
        return true
    end,

    -- reclaim (phase 5): the props with mass (trees, rocks, wrecks) within 40 units of the builder, nearest first
    -- (entity id on ties), up to 6 into its queue; false when there are none
    Reclaim = function(self, u)
        local p = u:GetPosition()
        local x, z = p[1], p[3]
        local list = GetReclaimablesInRect(x - 40, z - 40, x + 40, z + 40)
        if not list then return false end
        -- (2026-11-03, perf: only the 6 nearest are kept, in (distance, id) order as the full sort gave them - a base's
        -- trees and wrecks are hundreds within 40)
        local c, k = {}, 0
        for _, v in ipairs(list) do
            if not v.Dead and v.MaxMassReclaim and v.MaxMassReclaim * (v.ReclaimLeft or 1) >= 5 then
                local q = v:GetPosition()
                local dx, dz = q[1] - x, q[3] - z
                local e = { v = v, d = dx * dx + dz * dz, id = tonumber(v:GetEntityId()) or 0 }
                local last = c[k]
                if k < 6 or e.d < last.d or (e.d == last.d and e.id < last.id) then
                    local j = k < 6 and k + 1 or k
                    while j > 1 and (c[j - 1].d > e.d or (c[j - 1].d == e.d and c[j - 1].id > e.id)) do
                        c[j] = c[j - 1]
                        j = j - 1
                    end
                    c[j] = e
                    if k < 6 then k = k + 1 end
                end
            end
        end
        if not c[1] then return false end
        for i = 1, k do IssueReclaim({ u }, c[i].v) end
        return true
    end,

    -- The reclaim map (phase 6): the mass lying on the ground (props of kReclaimMin or more) per cell of
    -- kReclaimCell, one row of cells read each economy step, so the whole map is read every (rows x kThink) ticks
    -- without one big scan. The duels showed why: M28 made ~130 mass / s from reclaim at minute 10, with ~100
    -- engineers, while ours picked up only what lay within 40 units of an idle builder. Each cell keeps its zone
    -- (land) and whether our land units reach it, worked out once.
    -- (2026-11-03, perf: the props are the same for every army, so the mass per cell and the row being read are
    -- shared by all our armies (kShared.reclaim), and an army reads the next row only when no army has read one
    -- since its own last step: with 8 of our armies every one had read its own row each step - in Chris's 4v4 the
    -- largest single cost of our AI, 40% of its Lua time in the first 6 minutes. Each army still sees the whole map
    -- refreshed at least as often as before; an army alone reads exactly as before. (The first form skipped a read
    -- when one was made in the last kThink ticks: the step is 9 ticks apart, not 10 - WaitTicks(10) resumes after
    -- 9 - so an army alone read every other step, and a duel changed from minute 7; found 2026-11-03.)
    ReclaimScan = function(self)
        local R = self.Remaster
        local g = R.reclaim
        if not g then
            local w, h = GetMapSize()
            local S = kShared.reclaim
            if not S then
                S = { row = 0, mass = {}, lastTick = -1e9 }
                for i = 1, math.ceil(w / kReclaimCell) * math.ceil(h / kReclaimCell) do S.mass[i] = 0 end
                kShared.reclaim = S
            end
            R.reclaimShared = S
            g = { w = w, h = h, cols = math.ceil(w / kReclaimCell), rows = math.ceil(h / kReclaimCell),
                mass = S.mass, zone = {}, near = {}, nearMass = 0 }
            for i = 1, g.cols * g.rows do
                local cx = (math.mod(i - 1, g.cols) + 0.5) * kReclaimCell
                local cz = (math.floor((i - 1) / g.cols) + 0.5) * kReclaimCell
                local z = Remastered.AIZoneAt(cx, cz, 'land')
                if z > 0 and R.startZone > 0 and Remastered.AIZoneDistance(R.startZone, z, 'land') < 0 then z = 0 end
                g.zone[i] = z
                local dx, dz = cx - R.startX, cz - R.startZ
                g.near[i] = dx * dx + dz * dz <= kReclaimNear * kReclaimNear
            end
            R.reclaim = g
        end
        local S = R.reclaimShared
        local prev = R.reclaimStep
        R.reclaimStep = R.tick
        if prev and S.lastTick > prev then
            local near = 0
            for i = 1, g.cols * g.rows do
                if g.near[i] and g.zone[i] > 0 then near = near + g.mass[i] end
            end
            g.nearMass = near
            return
        end
        S.lastTick = R.tick
        local row = S.row
        S.row = math.mod(row + 1, g.rows)
        local z0, z1 = row * kReclaimCell, (row + 1) * kReclaimCell
        local sums = {}
        for c = 1, g.cols do sums[c] = 0 end
        for _, v in ipairs(GetReclaimablesInRect(0, z0, g.w, z1) or {}) do
            if not v.Dead and v.MaxMassReclaim then
                local m = v.MaxMassReclaim * (v.ReclaimLeft or 1)
                if m >= kReclaimMin then
                    local q = v:GetPosition()
                    if q[3] >= z0 and q[3] < z1 then  -- (the grid query also returns props of the cells it touches)
                        local c = math.floor(q[1] / kReclaimCell) + 1
                        if c >= 1 and c <= g.cols then sums[c] = sums[c] + m end
                    end
                end
            end
        end
        local near = 0
        for c = 1, g.cols do g.mass[row * g.cols + c] = sums[c] end
        for i = 1, g.cols * g.rows do
            if g.near[i] and g.zone[i] > 0 then near = near + g.mass[i] end
        end
        g.nearMass = near
    end,

    -- CONTROLLED SPACE (2026-11-06, Chris: "reclaim only in controlled space"). A zone is controlled when the land
    -- force of ours and our allies within kCtrlRadius of its centre reaches kCtrlOn T1 tanks' ratings (against
    -- ground; it keeps the flag down to kCtrlOff) and no enemy was seen within kCtrlRadius in the last kCtrlSeen
    -- ticks (R.enemySeen, the army's own sight); the base's zones (within kCtrlBase of land travel) need no force;
    -- a zone within kCtrlEnemyBase of an enemy start is never controlled, seen or not. Engineers on a reclaim trip
    -- whose zone loses the flag fall back at once: to the centre of the controlled zone nearest them, else home
    -- (entity id order). The sea probe (the all-AI 4v4 on SETONS_R, 17 looks over minutes 6-38): our engineers on
    -- the water away from their base 128 sightings -> 36, those reclaiming there 90 -> 14, engineers nearer an
    -- enemy start than their own a mean 12.2 -> 0.5 (at most 57 -> 5).
    ReclaimControl = function(self)
        local R = self.Remaster
        if R.startZone <= 0 or R.tick - (R.ctrlTick or -10000) < kCtrlStep then return end
        R.ctrlTick = R.tick
        local info = Remastered.AIMapInfo()
        if not R.ctrlNear then
            R.ctrlNear, R.ctrlBase = {}, {}
            for z = 1, info.zones do
                local a = Remastered.AIZone(z)
                local list = {}
                for n = 1, info.zones do
                    local b = Remastered.AIZone(n)
                    local dx, dz = a.x - b.x, a.z - b.z
                    if dx * dx + dz * dz <= kCtrlRadius * kCtrlRadius then table.insert(list, n) end
                end
                R.ctrlNear[z] = list
                local d = Remastered.AIZoneDistance(R.startZone, z, 'land')
                R.ctrlBase[z] = d >= 0 and d <= kCtrlBase
            end
            local r = Remastered.AIUnitRating(self:Bp('l0201'))
            R.ctrlRef = math.max(1e-6, r and r.rating and r.rating.ground or 1)
            -- (the zones within kCtrlEnemyBase of an enemy start: never controlled)
            R.ctrlEnemy = {}
            for j, b in ipairs(ArmyBrains) do
                if j ~= R.army and IsEnemy(R.army, j) and not ArmyIsCivilian(j) then
                    local ex, ez = b:GetArmyStartPos()
                    if ex then
                        for z = 1, info.zones do
                            local c = Remastered.AIZone(z)
                            local dx, dz = c.x - ex, c.z - ez
                            if dx * dx + dz * dz <= kCtrlEnemyBase * kCtrlEnemyBase then R.ctrlEnemy[z] = true end
                        end
                    end
                end
            end
        end
        local S = Remastered.AIZoneSummary(R.army)
        local seen = R.enemySeen or {}
        R.ctrl = R.ctrl or {}
        local on = 0
        for z = 1, info.zones do
            local force, hostile = 0, false
            for _, n in ipairs(R.ctrlNear[z]) do
                force = force + S.ownLandGround[n] + S.allyLandGround[n]
                if seen[n] and R.tick - seen[n] < kCtrlSeen then hostile = true end
            end
            force = force / R.ctrlRef
            if hostile or R.ctrlEnemy[z] then R.ctrl[z] = false
            elseif R.ctrlBase[z] then R.ctrl[z] = true
            elseif R.ctrl[z] then R.ctrl[z] = force >= kCtrlOff
            else R.ctrl[z] = force >= kCtrlOn end
            if R.ctrl[z] then on = on + 1 end
        end
        R.ctrlCount = on
        local g = R.reclaim
        if not g or not R.reclaimers then return end
        local ids = {}
        for id, w in pairs(R.reclaimers) do  -- (sorted below: the orders go in entity id order)
            local z = g.zone[w.cell] or 0
            if not R.ctrl[z] then table.insert(ids, id) end
        end
        table.sort(ids, function(a, b) return (tonumber(a) or 0) < (tonumber(b) or 0) end)
        for _, id in ipairs(ids) do
            local u = R.reclaimers[id].u
            R.reclaimers[id] = nil
            if u and not u.Dead then
                local q = u:GetPosition()
                local bx, bz, bd = R.startX, R.startZ
                for z = 1, info.zones do
                    if R.ctrl[z] then
                        local c = Remastered.AIZone(z)
                        local dx, dz = c.x - q[1], c.z - q[3]
                        local d = dx * dx + dz * dz
                        if not bd or d < bd then bx, bz, bd = c.x, c.z, d end
                    end
                end
                IssueClearCommands({ u })
                IssueMove({ u }, Pos(bx, bz))
                R.fallBacks = (R.fallBacks or 0) + 1
            end
        end
    end,

    -- A reclaim field for builder u (phase 6): the cell worth the most (its mass / (distance + 150)) among the cells
    -- of kReclaimField or more that our land units reach, with no enemy known in their zone (R.enemyAt) and fewer
    -- engineers sent there than its mass asks (one per kReclaimPerWorker, plus one); cell order on ties. The builder
    -- gets the cell's props, nearest first, up to 10. False when there is no such field.
    ReclaimField = function(self, u)
        local R = self.Remaster
        local g = R.reclaim
        if not g then return false end
        local workers, keep = {}, {}
        for id, w in pairs(R.reclaimers or {}) do  -- (counts only: the order does not matter)
            if R.tick - w.tick < kReclaimWorkerTicks then
                keep[id] = w
                workers[w.cell] = (workers[w.cell] or 0) + 1
            end
        end
        R.reclaimers = keep
        local q = u:GetPosition()
        -- (2026-11-06) THE TRIP: only into a controlled zone (ReclaimControl), and there only when (1) the cell's mass
        -- pays for the walk: kReclaimField plus kTripPerUnit a world unit beyond kTripFree; (2) its zone has fewer
        -- than kTripZoneMax of our engineers on trips; (3) the way is by land: the land travel from the engineer's
        -- zone is at most kTripDetour x the straight line (+ kTripSlack) - else an amphibious or hover engineer cuts
        -- over the sea. Each minute 'aireclaim' logs the trips given, the cells left out and the fall-backs.
        local zoneWorkers = {}
        for _, w in pairs(keep) do  -- (counts only)
            local wz = g.zone[w.cell] or 0
            zoneWorkers[wz] = (zoneWorkers[wz] or 0) + 1
        end
        R.tripStats = R.tripStats or { given = 0, ctrl = 0, value = 0, zone = 0, sea = 0 }
        local st = R.tripStats
        local uz = Remastered.AIZoneAt(q[1], q[3], 'land')
        if uz <= 0 then uz = R.startZone end
        local ctrl = R.ctrl or {}
        local best, bestScore
        for i = 1, g.cols * g.rows do
            local m = g.mass[i]
            local z = g.zone[i]
            if m >= kReclaimField and z > 0 and (workers[i] or 0) < 1 + m / kReclaimPerWorker and not self:Unsafe(z) then
                local cx = (math.mod(i - 1, g.cols) + 0.5) * kReclaimCell
                local cz = (math.floor((i - 1) / g.cols) + 0.5) * kReclaimCell
                local dx, dz = cx - q[1], cz - q[3]
                local d = math.sqrt(dx * dx + dz * dz)
                local score = m / (d + 150)
                if not bestScore or score > bestScore then
                    local why
                    if not ctrl[z] then why = 'ctrl'
                    elseif m < kReclaimField + kTripPerUnit * math.max(0, d - kTripFree) then why = 'value'
                    elseif (zoneWorkers[z] or 0) >= kTripZoneMax then why = 'zone'
                    else
                        local ld = Remastered.AIZoneDistance(uz, z, 'land')
                        if ld < 0 or ld > kTripDetour * d + kTripSlack then why = 'sea' end
                    end
                    if why then st[why] = st[why] + 1 else best, bestScore = i, score end
                end
            end
        end
        if R.tick - (R.tripLogged or -10000) >= 600 then
            R.tripLogged = R.tick
            LOG(string.format('aireclaim %d %d trips %d | left out: ctrl %d value %d zone %d sea %d | controlled zones %d, fall-backs %d',
                R.army, R.tick, st.given, st.ctrl, st.value, st.zone, st.sea, R.ctrlCount or 0, R.fallBacks or 0))
        end
        if not best then return false end
        st.given = st.given + 1
        local x0 = math.mod(best - 1, g.cols) * kReclaimCell
        local z0 = math.floor((best - 1) / g.cols) * kReclaimCell
        local c = {}
        for _, v in ipairs(GetReclaimablesInRect(x0, z0, x0 + kReclaimCell, z0 + kReclaimCell) or {}) do
            if not v.Dead and v.MaxMassReclaim and v.MaxMassReclaim * (v.ReclaimLeft or 1) >= kReclaimMin then
                local p = v:GetPosition()
                if p[1] >= x0 and p[1] < x0 + kReclaimCell and p[3] >= z0 and p[3] < z0 + kReclaimCell then
                    local dx, dz = p[1] - q[1], p[3] - q[3]
                    table.insert(c, { v = v, d = dx * dx + dz * dz, id = tonumber(v:GetEntityId()) or 0 })
                end
            end
        end
        if not c[1] then
            g.mass[best] = 0  -- (read again when its row comes round)
            return false
        end
        table.sort(c, function(m, n)
            if m.d ~= n.d then return m.d < n.d end
            return m.id < n.id
        end)
        for i = 1, math.min(10, table.getn(c)) do IssueReclaim({ u }, c[i].v) end
        R.reclaimers = R.reclaimers or {}
        R.reclaimers[u:GetEntityId()] = { cell = best, tick = R.tick, u = u }  -- (u: the fall-back, ReclaimControl)
        return true
    end,

    -- One builder's next job (phase 5), so that economy, army, scouting and expansion go on at the same time:
    -- 1. the first land factory (the commander), then its own zone's mass points in the opening;
    -- 2. power of the builder's tech when energy is short or not ahead of need (a few builders at a time);
    -- 3. land factories up to a floor that grows with the minutes, more while mass piles up;
    -- 4. a point defence where the enemy will come, when it is the cheaper answer (choices.lua);
    -- 5. an air factory (scouts) and a radar;
    -- 6. the nearest free mass point away from known enemies, and up to 2 more in its zone;
    -- 7. reclaim while mass is not piling up: the best reclaim field (phase 6), else around it; more factories; mass
    --    storage when mass is full;
    -- 8. assist an upgrade in progress (extractors first), else a factory, looked at again after kAssistTicks.
    BuilderJob = function(self, u, factories, isCommander, ctx)
        if table.getn(factories) == 0 and isCommander and self:BuildAt(u, 'b0101', 8) then return end
        -- the opening: the commander takes its own zone's mass before anything but the first factory, unless energy
        -- runs out
        -- (phase 8: right after the factory the commander builds kCommanderPower generators, so the factory's engineers
        -- and the first extractors run on power from the start: the opening probe's energy ran empty at 1:45. Tried
        -- 2026-11-03 for Chris's "factory, then extractors": the start zone's extractors before this power left the
        -- 8 armies of the 4v4 with 23 mass a second at minute 2 instead of 62, and 188 at minute 6 instead of 447 -
        -- the energy stalled; the commander's 4 extractors still come right after its 2 generators)
        if isCommander and ctx.minutes < kOpening and ctx.powerCount < kCommanderPower and
            self:BuildAt(u, 'b1101', 4) then
            ctx.powerCount = ctx.powerCount + 1
            ctx.powerBuilding = ctx.powerBuilding + 1
            return
        end
        local p = isCommander and self:NextMassPoint() or nil  -- (2026-11-03, perf: only the commander uses it here)
        if isCommander and ctx.minutes < 4 and p and p.dist <= 0 and self:GetEconomyStoredRatio('ENERGY') >= 0.3 then
            self:Claim(p.x, p.z)
            IssueBuildMobile({ u }, Pos(p.x, p.z), self:Bp('b1103'), {})
            return
        end
        -- tactical missile defence (phase 7; phase 8: before power, since a T2 engineer was always taken by T2
        -- power first and the rule built nothing): a T2+ engineer builds anti-missile beside the start, one from minute
        -- 12 once the enemy is known at T2 (launchers sit deep in its base, where fog hides them), and one more per
        -- launcher known (strategy.lua, R.enemyTml), at most 4, one at a time. The duels' kill counts: M28's
        -- launchers (xsb2108) destroyed 3,600 mass of our structures by minute 16 and then the commander, unseen; the
        -- battle estimate does not count missiles, so nothing else answered them.
        local wantTmd = math.min(4, ((self.Remaster.enemyTier or 0) >= 2 and ctx.minutes >= 12 and 1 or 0) +
            (self.Remaster.enemyTml or 0))
        if not isCommander and self:Tech(u) >= 2 and not ctx.tmdGiven and ctx.tmd < wantTmd and
            self:BuildNear(u, 'b4201', 2, self.Remaster.startX, self.Remaster.startZ) then
            ctx.tmdGiven = true
            ctx.tmd = ctx.tmd + 1
            return
        end
        -- The opening's extractors (phase 8): an extractor is the cheapest income there is (36 mass for 2 a second,
        -- back in 18 s), so in the opening an engineer takes the nearest free mass point within kOpeningMexRange before
        -- power and factories, while energy is not empty. The opening probe: our extractors were 3 at minute 3 and 8 at
        -- minute 5 (M28 7 and 15) while power, factories and the air factory came first.
        if not isCommander and ctx.minutes < kOpening and self:GetEconomyStoredRatio('ENERGY') >= 0.2 then
            local m = self:NextMassPoint(u)
            if m then
                local q = u:GetPosition()
                local dx, dz = m.x - q[1], m.z - q[3]
                if dx * dx + dz * dz <= kOpeningMexRange * kOpeningMexRange then
                    self:Claim(m.x, m.z, u)
                    IssueBuildMobile({ u }, Pos(m.x, m.z), self:Bp('b1103'), {})
                    local more, bp = 0, self:Bp('b1103')
                    for _, q2 in ipairs(self.Remaster.massPoints) do
                        if more >= 2 then break end
                        if q2.zone == m.zone and not self:Claimed(q2.x, q2.z) and self:CanBuildStructureAt(bp, Pos(q2.x, q2.z)) then
                            self:Claim(q2.x, q2.z, u)
                            IssueBuildMobile({ u }, Pos(q2.x, q2.z), bp, {})
                            more = more + 1
                        end
                    end
                    return
                end
            end
        end
        -- power, at most 1 + one per 40 energy / s of income being built at once (power costs energy to build: a
        -- crowd of builders on it at once starves everything). In the opening (phase 8), while energy is short (under
        -- kEnergyShort stored) or under the power floor below, power comes first for up to half the builders: the opening
        -- probe showed our energy empty in 52-58% of the first 5 minutes (FAF 2-12%, M28 0%), and every stalled second
        -- slows the extractors, the factory and the builders together.
        -- The opening's power floor (phase 8): kOpeningPower + kOpeningPowerRate a minute generators (built or being
        -- built) before kOpening minutes - about the curve both opponents keep (M28: 7 at minute 3, 12 at minute 5;
        -- ours had 4 at minute 3.5) - with up to half the builders on it while under the floor.
        local floorPower = ctx.minutes < kOpening and ctx.powerCount < kOpeningPower + math.floor(kOpeningPowerRate * ctx.minutes)
        local maxPower = 1 + math.floor(self:GetEconomyIncome('ENERGY') * 10 / 40)
        -- (and after the opening too, while energy is short, up to a third of the builders: the opening set still
        -- stalled 20-45% of minutes 5-10)
        local short = (ctx.minutes < kOpening and ctx.energyShort) or floorPower
        if short then
            maxPower = math.max(maxPower, 1 + math.floor(ctx.builders / 2))
        elseif ctx.energyShort then
            short = true
            maxPower = math.max(maxPower, 1 + math.floor(ctx.builders / 3))
        end
        if ctx.powerBuilding < maxPower and (short or self:NeedPower()) then
            local tech = self:Tech(u)
            if self:BuildAt(u, tech == 3 and 'b1301' or tech == 2 and 'b1201' or 'b1101', tech == 3 and 8 or 4) then
                ctx.powerBuilding = ctx.powerBuilding + 1
                ctx.powerCount = ctx.powerCount + 1
                return
            end
        end
        -- the navy (phase 10, navy.lua; before the land factories, which otherwise took every builder at the base):
        -- naval factories while the enemy has ships we know of or ships shell our coast,
        -- at the first point from the start toward our home water where one can stand
        -- (orders not yet started count for kPendingTicks: the builder may still be walking there)
        local R = self.Remaster
        local pendingNaval = 0
        R.navalPending = R.navalPending or {}
        local keep = {}
        for _, t in ipairs(R.navalPending) do
            if R.tick - t < kNavalPendingTicks then
                table.insert(keep, t)
                pendingNaval = pendingNaval + 1
            end
        end
        R.navalPending = keep
        if not isCommander and ctx.navalFactories + pendingNaval < Navy().WantFactories(self) and self:AtBase(u) then
            local site = Navy().FactorySite(self)
            if site then
                self:Claim(site[1], site[2])
                IssueBuildMobile({ u }, Pos(site[1], site[2]), self:Bp('b0103'), {})
                ctx.navalFactories = ctx.navalFactories + 1
                table.insert(R.navalPending, R.tick)
                LOG(string.format('ainavy %d %d factory ordered at %.0f, %.0f (%d wanted)', self.Remaster.army,
                    self.Remaster.tick, site[1], site[2], Navy().WantFactories(self)))
                return
            end
        end
        -- land factories (one per ~6 mass / s, at most 12): from minute 3 up to 1 + one per 1.5 minutes whatever is
        -- stored (the army grows with the economy), the rest while mass piles up; before the next far mass point
        local storedMass = self:GetEconomyStoredRatio('MASS')
        -- (phase 9: at most kMaxFactories instead of 12: with the phase-8 economy the mass bar filled at minute 9-10
        -- with 7-8 factories against 120 mass a second)
        local wantFactories = math.min(kMaxFactories, 1 + math.floor(self:GetEconomyIncome('MASS') * 10 / 6))
        -- (phase 11: after the opening the floor grows by one more every kFactoryEvery minutes: M28 had 22 factories
        -- at minute 10 to our 9-10; the cap stays the income's)
        local floorFactories = ctx.minutes >= 3 and math.min(wantFactories, 1 + math.floor(ctx.minutes / 1.5) +
            math.floor(math.max(0, ctx.minutes - kOpening) / kFactoryEvery)) or 1
        -- (phase 8: in the opening a new factory waits for a sound energy economy: a factory draws energy as it
        -- builds and as it works, and the second one had started the stall)
        local energyOk = ctx.minutes >= kOpening or not ctx.energyShort
        local landBp = self:LandFactoryBp(u, ctx)
        -- (phase 13: piling mass builds a factory only when no extractor upgrade can start, R.upgradesOpen in
        -- choices.lua: the upgrades are the better sink)
        local overflow = storedMass > 0.6 and not self.Remaster.upgradesOpen
        if landBp and energyOk and (overflow and ctx.landFactories < wantFactories or
            ctx.landFactories < floorFactories) and self:BuildAt(u, landBp, 8) then
            ctx.landFactories = ctx.landFactories + 1
            return
        end
        -- a defence where the enemy will come, when it is the cheaper answer than units (one builder a think)
        if not isCommander then
            local def = Choices().DefenceJob(self, self:Tech(u), ctx)
            if def then
                self:Claim(def.x, def.z)
                IssueBuildMobile({ u }, Pos(def.x, def.z), def.bp, {})
                return
            end
        end
        -- (2026-11-03, the unit counts) an experimental, by a T3 engineer at the base (ExperimentalJob)
        if not isCommander and self:Tech(u) >= 3 and self:ExperimentalJob(u, ctx) then return end
        -- scouting: an air factory from minute 3 (its scouts), a radar from minute 5
        if not ctx.airFactory and ctx.minutes >= 3 and energyOk and self:BuildAt(u, 'b0102', 8) then
            ctx.airFactory = true
            ctx.airFactories = ctx.airFactories + 1
            return
        end
        -- more air factories while the enemy's air grows (one more per 1500 of its known air mass, at most 4)
        if ctx.airFactories < ctx.wantAir and storedMass > 0.1 and self:BuildAt(u, 'b0102', 8) then
            ctx.airFactories = ctx.airFactories + 1
            return
        end
        if not ctx.radar and ctx.minutes >= 5 and self:BuildAt(u, 'b3101', 4) then
            ctx.radar = true
            return
        end
        -- reclaim as a job of its own (phase 6): while fewer engineers reclaim than K.reclaimShare of them (and than
        -- the fields near the start can feed), the next one goes to a reclaim field instead of a mass point farther
        -- than kMassNear from it (a near mass point is cheap and lasts: it comes first). The duels with fog showed no
        -- reclaimer at all for 12 minutes beside 26,000 mass of reclaim near the start: an engineer always found some
        -- far mass point first.
        p = self:NextMassPoint(not isCommander and u or nil)
        if not isCommander and ctx.reclaimers < ctx.wantReclaimers and self:GetEconomyStoredRatio('MASS') < 0.8 then
            local q = u:GetPosition()
            local far = not p or (p.x - q[1]) * (p.x - q[1]) + (p.z - q[3]) * (p.z - q[3]) > kMassNear * kMassNear
            if far and self:ReclaimField(u) then
                ctx.reclaimers = ctx.reclaimers + 1
                return
            end
        end
        if p and not (isCommander and p.dist > 0) then
            self:Claim(p.x, p.z, u)
            IssueBuildMobile({ u }, Pos(p.x, p.z), self:Bp('b1103'), {})
            -- an engineer takes up to 2 more free points of the same zone in the same walk (the zone's point order)
            if not isCommander then
                local more, bp = 0, self:Bp('b1103')
                for _, q in ipairs(self.Remaster.massPoints) do
                    if more >= 2 then break end
                    if q.zone == p.zone and not self:Claimed(q.x, q.z) and self:CanBuildStructureAt(bp, Pos(q.x, q.z)) then
                        self:Claim(q.x, q.z, u)
                        IssueBuildMobile({ u }, Pos(q.x, q.z), bp, {})
                        more = more + 1
                    end
                end
            end
            return
        end
        -- (phase 7: the commander reclaims only around itself at the base, never in a far field; the duels' commanders
        -- died 180-270 world units from the start, out reclaiming)
        if self:GetEconomyStoredRatio('MASS') < 0.5 and
            ((not isCommander and self:ReclaimField(u)) or (self:AtBase(u, true) and self:AcuHome(u) and self:Reclaim(u))) then
            return
        end
        if landBp and energyOk and ctx.landFactories < wantFactories and storedMass > 0.2 and
            not self.Remaster.upgradesOpen and self:BuildAt(u, landBp, 8) then
            ctx.landFactories = ctx.landFactories + 1
            return
        end
        if storedMass > 0.8 and ctx.minutes >= 6 and ctx.storages < 4 and self:AtBase(u) then
            local s = self:StorageSpot()
            if s then
                self:Claim(s[1], s[2])
                IssueBuildMobile({ u }, Pos(s[1], s[2]), self:Bp('b1106'), {})
                ctx.storages = ctx.storages + 1
                return
            end
        end
        -- assist: an upgrade in progress (extractors first), else the factories in turn; looked at again after
        -- kAssistTicks so that an assisting builder takes new work when it comes
        -- (phase 7: a builder away from the base assists only an upgrade within kBaseRadius of it, else it walks
        -- back to the base, where the base jobs are)
        local target = ctx.expBuilding or ctx.upgrading[1]  -- (2026-11-03: an experimental under construction first)
        if not self:AtBase(u) then
            target = nil
            local q = u:GetPosition()
            for _, m in ipairs(ctx.upgrading) do
                local mp = m:GetPosition()
                local dx, dz = mp[1] - q[1], mp[3] - q[3]
                if dx * dx + dz * dz <= kBaseRadius * kBaseRadius then target = m break end
            end
            if not target then
                IssueMove({ u }, Pos(self.Remaster.startX, self.Remaster.startZ))
                return
            end
        elseif not target and factories[1] then
            ctx.assistNext = math.mod(ctx.assistNext, table.getn(factories)) + 1
            target = factories[ctx.assistNext]
        end
        -- (2026-11-04, item 3: the commander assists nothing past kAcu.home from the start from minute kAcu.homeFrom -
        -- an upgrading extractor could stand 300 out - but the factories in turn, CommanderJobSafe's rule)
        if target and isCommander then
            local tp = target:GetPosition()
            if not self:AcuHome(u, tp[1], tp[3]) then
                target = nil
                for _, f in ipairs(factories) do
                    local fp = f:GetPosition()
                    if not target and self:AcuHome(u, fp[1], fp[3]) then target = f end
                end
            end
        end
        if target then
            IssueGuard({ u }, target)
            self.Remaster.assist[u:GetEntityId()] = self.Remaster.tick
        end
    end,

    -- The commander (phase 7; 2026-11-03, Chris's big test: "our ACU too focused on repairing instead of trying to
    -- fight back", and "sends the commander out despite ~10 battleships in range"). Each economy step, in this order:
    -- - SHIPS: enemy ships whose reach covers the commander's zone (Remastered.AIZoneThreat against ground from the
    --   navy, fair intel) of kAcu.naval or more: it goes to the base zone the ships reach least (inland first, nearest
    --   on ties) and takes work only there; a commander cannot answer a battle line from the shore;
    -- - FIGHT BACK: enemy land units the army knows within kAcu.fight of it (kAcu.least mass or more), and the estimate
    --   of the fight at its zone (soak, as the defence: ours and our allies' that can come within kAcu.horizon) at
    --   kAcu.start or more with its health at kAcuRetreat or more: it drops its work and stands, its guns on them
    --   (FAF's auto-overcharge on), repaired by engineers when hurt; it fights on while the estimate stays at kAcu.keep, its health at kAcu.hard
    --   and it within kAcu.away of the start, and never with an enemy commander known in its zone or beside it.
    --   (The first form fought from the defend margin 0.35 and down to a quarter of its health against a "clearly
    --   weaker" force: the judge set lost its commander in 9 of 10 duels, M28's at minutes 13-19 instead of 14-31,
    --   FAF's veteran at 20.7 instead of 40.7; a second, from 0.6 and down to 60%, still lost it chasing two units
    --   out of the base - 0.58 to dead in 25 s; a third, within 80 of the start, still at minutes 15.7 / 18.2.) Losing the commander loses the game, so here safety outweighs
    --   "never flee a weaker force", which stays the army's rule;
    -- - FALL BACK (phase 7): hurt under kAcuRetreat and losing (or under kAcu.hard, or hit by what it cannot see): it
    --   drops its work and backs away as before, repaired by up to kAcuRepairers engineers, until kAcuBack.
    -- Returns true while the commander is busy (no job this step). Logs 'aiacu <army> <tick> <what>' on each change.
    RemasterCommander = function(self, commander, ctx)
        local R = self.Remaster
        if not R.acuOc and commander.SetAutoOvercharge then
            commander:SetAutoOvercharge(true)  -- (FAF's own auto mode: it overcharges what it fights when energy allows)
            R.acuOc = true
        end
        local hp = commander:GetHealth() / math.max(commander:GetMaxHealth(), 1)
        local q = commander:GetPosition()
        local cz = Remastered.AIZoneAt(q[1], q[3], 'land')
        local function Mode(m, text)
            if R.acuMode ~= m then
                LOG(string.format('aiacu %d %d %s at health %.2f: %s', R.army, R.tick, m, hp, text))
                R.acuMode = m
                R.acuOrdered = nil
            end
        end
        -- ships
        local naval = cz > 0 and Remastered.AIZoneThreat(R.army, cz, 'ground', 0, 'naval') or 0
        R.acuSafeZones = nil
        if naval >= kAcu.naval and R.baseZones then
            local best, bestT, bestInland
            for _, z in ipairs(R.baseZones) do
                local t = Remastered.AIZoneThreat(R.army, z, 'ground', 0, 'naval')
                local inland = true
                for _, nb in ipairs(Remastered.AIZoneNeighbours(z)) do
                    if nb.coast then inland = false end
                end
                if not bestT or t < bestT or (t == bestT and inland and not bestInland) then
                    best, bestT, bestInland = z, t, inland
                end
            end
            if best and best ~= cz and bestT < naval then
                Mode('naval', string.format('enemy ships reach its zone (%.1f against ground): inland to zone %d (%.1f)',
                    naval, best, bestT))
                if not R.acuOrdered then
                    local zz = Remastered.AIZone(best)
                    IssueClearCommands({ commander })
                    IssueMove({ commander }, Pos(zz.x, zz.z))
                    R.acuOrdered = R.tick
                end
                R.acuHurt = hp < kAcuRetreat or nil
                return true
            end
        end
        -- the enemy land units the army knows within kAcu.fight: in its zone and the land zones beside it
        local fx, fz, fn, fm = 0, 0, 0, 0
        local foeAcu = false
        if cz > 0 then
            local zones = { cz }
            for _, nb in ipairs(Remastered.AIZoneNeighbours(cz)) do
                if not nb.coast then table.insert(zones, nb.zone) end
            end
            for _, z in ipairs(zones) do
                if Remastered.AIZoneForces(R.army, z).enemy.commanders > 0 then foeAcu = true end
                for _, t in ipairs(Remastered.AIZoneTargets(R.army, z)) do
                    if not t.structure and not t.unknown then  -- (2026-11-04: contacts of unknown type are not fought)
                        local dx, dz = t.x - q[1], t.z - q[3]
                        if dx * dx + dz * dz <= kAcu.fight * kAcu.fight then
                            fx, fz, fn, fm = fx + t.x, fz + t.z, fn + 1, fm + t.mass
                        end
                    end
                end
            end
        end
        if fm < kAcu.least then fn = 0 end  -- (a lone scout is not worth leaving the work for)
        local margin = fn > 0 and Remastered.AIZoneEstimate(R.army, cz, kAcu.horizon, false, true) or 0
        local sx, sz = q[1] - R.startX, q[3] - R.startZ
        local home = sx * sx + sz * sz <= kAcu.away * kAcu.away
        local fighting = R.acuMode == 'fight'
        if fn > 0 and home and not foeAcu and hp >= kAcu.hard and
            ((fighting and margin >= kAcu.keep) or (margin >= kAcu.start and hp >= kAcuRetreat)) then
            Mode('fight', string.format('%d enemy units within %d, the estimate there %.2f: it fights', fn, kAcu.fight, margin))
            -- (it drops its work and stands: its guns take what is in reach, it does not walk at them - the forms that
            -- attack-moved at them lost the commander out of the base in the judge set)
            if not R.acuOrdered then
                IssueClearCommands({ commander })
                R.acuOrdered = R.tick
            end
            R.acuHurt = hp < kAcuRetreat or nil  -- (the engineers repair it while it fights)
            return true
        end
        -- fall back (phase 7)
        if hp < kAcuRetreat and (R.acuMode ~= 'back' or hp < (R.acuHp or 1) - 0.05) then
            R.acuHurt = true
            R.acuHp = hp
            Mode('back', fn == 0 and 'hit by what it does not see' or hp < kAcu.hard and
                string.format('too hurt to fight on (estimate %.2f)', margin) or
                string.format('losing (estimate %.2f)', margin))
            local tx, tz = R.startX, R.startZ
            -- (phase 10: with ships shelling our coast, the fallback point is the base zone farthest from them,
            -- not the start: the start is on the coast on SETONS_R, and FAF's ships killed the commander there)
            local ct = R.coastThreat and R.coastThreat[1]
            -- (a base zone with no water beside it when there is one: a farthest zone on another shore lost the
            -- commander to a submarine's deck gun)
            if ct and R.baseZones then
                local cz0 = Remastered.AIZone(ct.zone)
                local bestD, bestInland
                for _, z in ipairs(R.baseZones) do
                    local inland = true
                    for _, nb in ipairs(Remastered.AIZoneNeighbours(z)) do
                        if nb.coast then inland = false end
                    end
                    local zz = Remastered.AIZone(z)
                    local dx, dz = zz.x - cz0.x, zz.z - cz0.z
                    local d = dx * dx + dz * dz
                    if (inland and not bestInland) or (inland == (bestInland or false) and (not bestD or d > bestD)) then
                        tx, tz, bestD, bestInland = zz.x, zz.z, d, inland
                    end
                end
            end
            -- (phase 10: away from the middle of the enemies it knows near it, and only onto land our engineers reach;
            -- the start when that point is not such land or none is near. The duel against FAF seed 1001 lost its
            -- commander backing away from a land unit into the guns of ships off the coast.)
            if fn > 0 and not ct then
                local cx, cz2 = fx / fn, fz / fn
                local dx, dz = q[1] - cx, q[3] - cz2
                local l = math.max(math.sqrt(dx * dx + dz * dz), 1)
                local ax, az = q[1] + dx / l * kAcuDanger, q[3] + dz / l * kAcuDanger
                local lz = Remastered.AIZoneAt(ax, az, 'land')
                if lz > 0 and Remastered.AIZoneDistance(R.startZone, lz, 'land') >= 0 then
                    local sx, sz = R.startX - cx, R.startZ - cz2
                    -- (the start when it is farther from the enemies than that point)
                    if sx * sx + sz * sz < (ax - cx) * (ax - cx) + (az - cz2) * (az - cz2) then tx, tz = ax, az end
                end
            end
            IssueClearCommands({ commander })
            IssueMove({ commander }, Pos(tx, tz))
            LOG(string.format('aiacu %d %d falls back at health %.2f to %.0f, %.0f', R.army, R.tick, hp, tx, tz))
            return true
        elseif R.acuHurt and hp >= kAcuBack then
            R.acuHurt = nil
            R.acuHp = nil
            Mode('work', 'back to work')
            return false
        elseif R.acuHurt then
            return true
        end
        if R.acuMode == 'fight' or R.acuMode == 'naval' then
            Mode('work', 'nothing near to fight, and out of the ships\' reach')
            IssueClearCommands({ commander })
        end
        R.acuMode = 'work'
        return false
    end,

    -- An experimental (2026-11-03, the unit counts: Chris's 4v4 had 300-450 land units an army even with the caps,
    -- and no experimental ever). A T3 engineer at the base starts one when none is under construction or ordered in
    -- the last kExp.pending ticks (the walk), from minute kExp.from, with kExp.income mass a second or more and mass
    -- piling up (kExp.stored of the bar), at most one per kExp.every minutes after kExp.from (the first at once): the
    -- land experimental of our faction with the best build value against the enemy's known army (blueprint id on
    -- ties). (The first form also started one whenever the land army was at its soft cap: with the bar empty there was
    -- no mass for it, and an order every step sent a new engineer to a new site, the 4v4's army 5 eight in 2 minutes.)
    -- The base's builders assist it first (the assists above). Logs 'aiexp <army> <tick> <id> value <v>'.
    -- (2026-11-04, item 1: the mass sinks) In the 4v4 of all our AI (Chris's session, 35 minutes) the mass bar sat at
    -- 0-4% in every army from minute 6 on: there is no piling mass to wait for, the income goes into units, and two
    -- experimentals were ordered in 35 minutes. An experimental is army too: it is bought instead of units when it is
    -- the better buy and the economy carries it (decision 14, risk against reward), beside the piling mass of before:
    -- - CARRIED: it costs at most kExp.seconds of the mass income now (a first form waited for the army to be past
    --   what the economy keeps - the army kept grows with the income, so it never was in the 4v4);
    -- - SAFE: the base is not under attack (no recall running) and the estimate at the start zone, with all the enemy
    --   that can come within kExp.horizon, lets us hold it (K.investSafe) or nothing of the enemy can come;
    -- - WORTH IT: the experimental's build value against the enemy's known army is at least kExp.value of the best T3
    --   unit's (Choices().Counter): with piling mass (kExp.stored) or the land army at its hard cap, the units are no
    --   alternative and the best experimental goes whatever its value.
    -- The candidates are every mobile land experimental this engineer can build (Unit:CanBuild), mods' too, by build
    -- value (blueprint id on ties). While it is under construction the T1 land factories make no new army units (their
    -- T1 spam had taken the income), so the mass reaches it (RemasterEconomy).
    -- Each decision logs 'aiexp <army> <tick> <id> value <v> ...' or, at most once a minute, 'aiexp ... hold | why'.
    ExperimentalCandidates = function(self, u)
        local R = self.Remaster
        R.expCandidates = R.expCandidates or {}
        local key = u:GetUnitId()
        if not R.expCandidates[key] then
            local ids = {}
            for id, bp in pairs(__blueprints) do  -- (collected, then sorted: the order of pairs does not matter)
                if type(id) == 'string' and bp.BlueprintId == id and bp.Categories and bp.Economy and
                    (bp.Economy.BuildCostMass or 0) > 0 then
                    local c = {}
                    for _, k in ipairs(bp.Categories) do c[k] = true end
                    if c.EXPERIMENTAL and c.MOBILE and c.LAND and not c.AIR and u:CanBuild(id) then table.insert(ids, id) end
                end
            end
            table.sort(ids)
            R.expCandidates[key] = ids
        end
        return R.expCandidates[key]
    end,

    ExperimentalJob = function(self, u, ctx)
        local R = self.Remaster
        if ctx.expBuilding or ctx.expGiven or ctx.minutes < kExp.from or not self:AtBase(u) then return false end
        if R.expOrdered and R.tick - R.expOrdered < kExp.pending then return false end
        if ctx.experimentals >= 1 + math.floor((ctx.minutes - kExp.from) / kExp.every) then return false end
        local income = self:GetEconomyIncome('MASS') * 10
        if income < kExp.income then return false end
        local function Hold(why)
            if R.tick - (R.expHoldLogged or -10000) >= 600 then
                LOG(string.format('aiexp %d %d hold | %s (income %.0f, mass bar %.2f, land units %d)', R.army, R.tick, why,
                    income, self:GetEconomyStoredRatio('MASS'), R.armyCount or 0))
                R.expHoldLogged = R.tick
            end
            return false
        end
        local piling = self:GetEconomyStoredRatio('MASS') >= kExp.stored
        local capped = (R.armyCount or 0) >= (R.landHard or 1e9)
        if not piling and not capped then
            if R.recallUntil and R.tick < R.recallUntil then return Hold('the base is under attack') end
            local K = import("/lua/remastered/ai/personality.lua").Knobs(self)
            local margin, _, _, _, _, theirs = Remastered.AIZoneEstimate(R.army, R.startZone, kExp.horizon, false, true)
            if theirs > 0 and margin < K.investSafe then
                return Hold(string.format('the base would not hold against what can come (%.2f, %.0f mass)', margin, theirs))
            end
        end
        local enemy = Remastered.AIEnemyMix(R.army)
        local best, bestV
        for _, id in ipairs(self:ExperimentalCandidates(u)) do
            local v = enemy[1] and Remastered.AIBuildValue(id, enemy) or 1
            if not bestV or v > bestV then best, bestV = id, v end
        end
        if not best then return false end
        local cost = __blueprints[best].Economy.BuildCostMass
        if not piling and not capped and cost > kExp.seconds * income then
            return Hold(string.format('%s costs %.0f, more than %d s of the income', best, cost, kExp.seconds))
        end
        local unitV = enemy[1] and (Choices().Counter(self, 3).groundValue or 0) or 0
        if not piling and not capped and bestV < kExp.value * unitV then
            return Hold(string.format('%s value %.2f, under %.1f x the best T3 unit\'s %.2f', best, bestV, kExp.value, unitV))
        end
        if not self:BuildAt(u, '!' .. best, 16) then return false end
        ctx.expGiven = true
        R.expOrdered = R.tick
        LOG(string.format('aiexp %d %d %s value %.2f against T3 units %.2f (%s; income %.0f, mass bar %.2f, land units %d)',
            R.army, R.tick, best, bestV or 0, unitV, piling and 'mass piles up' or capped and 'the land army at its cap' or
            'the better buy, carried by the income', income, self:GetEconomyStoredRatio('MASS'), R.armyCount or 0))
        return true
    end,

    -- (2026-11-03) the commander's new job, when enemy ships reach its place (its first order's point; an order on an
    -- entity has none and is kept): dropped, and it reclaims around itself instead. Chris's big test: the commander
    -- walked out into the reach of ~10 battleships.
    -- (2026-11-04, item 3) false for the commander past kAcu.home from the start from minute kAcu.homeFrom; true else
    AcuHome = function(self, u, x, z)
        if not EntityCategoryContains(categories.COMMAND, u) or GetGameTick() < kAcu.homeFrom * 600 then return true end
        local R = self.Remaster
        if not x then
            local q = u:GetPosition()
            x, z = q[1], q[3]
        end
        local dx, dz = x - R.startX, z - R.startZ
        return dx * dx + dz * dz <= kAcu.home * kAcu.home
    end,

    -- (2026-11-04, item 3: where we lose against M28.) Every judge game lost is lost when our commander dies
    -- (demoralization), and the kill probe of the judge set (base, 8 M28 games) found it killed 85-165 world units from
    -- the start in 9 of 12 deaths - by M28's T2 gunships (5), its T2 bots (4) - out at a base job or its reclaim (base
    -- jobs went to it anywhere within 150-200 of the start), falling back hurt across that distance under fire. From
    -- minute kAcu.homeFrom a new job whose place is farther than kAcu.home from the start is not taken: the commander
    -- assists the nearest factory within that radius instead (looked at again after kAssistTicks), else it walks home.
    CommanderJobSafe = function(self, commander)
        local R = self.Remaster
        local e = (commander:GetCommandQueue() or {})[1]
        if not e or not e.x or (e.x == 0 and e.z == 0) then return end
        if not self:AcuHome(commander, e.x, e.z) then
            IssueClearCommands({ commander })
            if R.tick - (R.acuFarLogged or -1000) >= 300 then
                LOG(string.format('aiacu %d %d job refused: its place is %.0f from the start (home %d)', R.army, R.tick,
                    math.sqrt((e.x - R.startX) * (e.x - R.startX) + (e.z - R.startZ) * (e.z - R.startZ)), kAcu.home))
                R.acuFarLogged = R.tick
            end
            local best, bd
            for _, f in ipairs(self:GetListOfUnits(categories.FACTORY * categories.STRUCTURE, false)) do
                if Alive(f) then
                    local q = f:GetPosition()
                    local d = (q[1] - R.startX) * (q[1] - R.startX) + (q[3] - R.startZ) * (q[3] - R.startZ)
                    if d <= kAcu.home * kAcu.home and (not bd or d < bd) then best, bd = f, d end
                end
            end
            if best then
                IssueGuard({ commander }, best)
                R.assist[commander:GetEntityId()] = R.tick
            else
                IssueMove({ commander }, Pos(R.startX, R.startZ))
            end
            return
        end
        local z = Remastered.AIZoneAt(e.x, e.z, 'land')
        if z <= 0 then return end
        local t = Remastered.AIZoneThreat(R.army, z, 'ground', 0, 'naval')
        if t >= kAcu.naval then
            IssueClearCommands({ commander })
            if R.tick - (R.acuRefused or -1000) >= 300 then
                LOG(string.format('aiacu %d %d job refused: enemy ships reach zone %d (%.1f against ground)', R.army, R.tick, z, t))
                R.acuRefused = R.tick
            end
            self:Reclaim(commander)
        end
    end,

    RemasterEconomy = function(self)
        self:ReclaimScan()
        self:ReclaimControl()  -- (2026-11-06: controlled space and the reclaimers' fall-back)
        local factories, engineers, commander = {}, {}, nil
        local ctx = { minutes = GetGameTick() / 600, landFactories = 0, upgrading = {}, storages = 0, assistNext = 0,
            factoryUpgrades = 0, extractorUpgrades = 0, airFactories = 0, landTier = 0, powerCount = 0,
            navalFactories = 0, landHQ = 0 }
        -- the enemy's air (choices.lua): our air factories and fighters answer it
        local enemyAir, enemyGround = Choices().EnemyAir(self)
        ctx.wantAir = math.min(4, 1 + math.floor(enemyAir / 1500))
        -- (phase 10) the air answer to ships shelling our coast: anti-ship aircraft up to kNavalAirShare x the known
        -- ships' mass there, and an air factory more per kNavalAirFactory of it
        ctx.navalWanted = kNavalAirShare * (self.Remaster.coastMass or 0)
        ctx.navalAir = 0
        for _, u in ipairs(self:GetListOfUnits(categories.AIR * categories.MOBILE * (categories.BOMBER + categories.GROUNDATTACK + categories.ANTINAVY), false)) do
            if not u.Dead then ctx.navalAir = ctx.navalAir + (u:GetBlueprint().Economy.BuildCostMass or 0) end
        end
        if (self.Remaster.coastMass or 0) > 0 then
            ctx.wantAir = math.min(6, math.max(ctx.wantAir, 1 + math.floor(self.Remaster.coastMass / kNavalAirFactory)))
        end
        ctx.fighters = enemyAir >= 300 and enemyAir >= 0.3 * (enemyAir + enemyGround)
        local R = self.Remaster
        -- (2026-11-07, step 5: an experimental's factory attachment - EXTERNALFACTORYUNIT - is no factory of ours: a
        -- gifted Fatboy's built T3 engineers from minute 1 and the economy never started)
        local ext = categories.EXTERNALFACTORYUNIT
        for _, u in ipairs(self:GetListOfUnits(categories.FACTORY * categories.STRUCTURE, false)) do
            if not u.Dead and not (ext and EntityCategoryContains(ext, u)) then
                -- (counted from the start of their building: no second factory for the same need)
                if EntityCategoryContains(categories.AIR, u) then
                    ctx.airFactory = true
                    ctx.airFactories = ctx.airFactories + 1
                    if Alive(u) and self:Tech(u) >= 2 then ctx.airT2 = true end  -- (2026-11-03: one T2+ air factory is enough)
                end
                if EntityCategoryContains(categories.NAVAL, u) then ctx.navalFactories = ctx.navalFactories + 1 end
                if EntityCategoryContains(categories.LAND, u) then
                    ctx.landFactories = ctx.landFactories + 1
                    if Alive(u) then ctx.landTier = math.max(ctx.landTier, self:Tech(u)) end
                    if Alive(u) and not EntityCategoryContains(categories.SUPPORTFACTORY, u) then
                        ctx.landHQ = math.max(ctx.landHQ, self:Tech(u))
                    end
                end
                if Alive(u) then table.insert(factories, u) end
            end
        end
        for _, u in ipairs(self:GetListOfUnits(categories.RADAR * categories.STRUCTURE, false)) do
            if not u.Dead then ctx.radar = true end
        end
        ctx.powerBuilding = 0
        -- the engineers reclaiming now, and how many should (phase 6, K.reclaimShare)
        ctx.reclaimers = 0
        for _, w in pairs(R.reclaimers or {}) do  -- (a count: the order does not matter)
            if R.tick - w.tick < kReclaimWorkerTicks then ctx.reclaimers = ctx.reclaimers + 1 end
        end
        for _, u in ipairs(self:GetListOfUnits(categories.ENERGYPRODUCTION * categories.STRUCTURE, false)) do
            if not u.Dead and u:GetFractionComplete() < 1 then ctx.powerBuilding = ctx.powerBuilding + 1 end
            if not u.Dead then ctx.powerCount = ctx.powerCount + 1 end
        end
        ctx.defenceBuilding = 0
        for _, u in ipairs(self:GetListOfUnits(categories.DEFENSE * categories.STRUCTURE, false)) do
            if not u.Dead and u:GetFractionComplete() < 1 then ctx.defenceBuilding = ctx.defenceBuilding + 1 end
        end
        ctx.storages = table.getn(self:GetListOfUnits(categories.MASSSTORAGE * categories.STRUCTURE, false))
        ctx.tmd = table.getn(self:GetListOfUnits(categories.ANTIMISSILE * categories.TECH2 * categories.STRUCTURE, false))
        -- (2026-11-03) our experimentals, and the one under construction (ExperimentalJob, assists)
        ctx.experimentals = 0
        for _, u in ipairs(self:GetListOfUnits(categories.EXPERIMENTAL, false)) do
            if not u.Dead then
                if u:GetFractionComplete() < 1 then
                    if not ctx.expBuilding then ctx.expBuilding = u end
                else
                    ctx.experimentals = ctx.experimentals + 1
                end
            end
        end
        -- upgrades in progress, extractors first (what idle builders assist)
        for kind, cat in ipairs({ categories.MASSEXTRACTION, categories.FACTORY }) do
            for _, u in ipairs(self:GetListOfUnits(cat * categories.STRUCTURE, false)) do
                if not u.Dead and u:IsUnitState('Upgrading') then
                    table.insert(ctx.upgrading, u)
                    if kind == 2 and EntityCategoryContains(categories.AIR, u) then ctx.airUpgrading = true end
                    if kind == 2 and EntityCategoryContains(categories.NAVAL, u) then ctx.navalUpgrading = true end
                    if kind == 2 then ctx.factoryUpgrades = ctx.factoryUpgrades + 1 end
                    if kind == 1 then ctx.extractorUpgrades = ctx.extractorUpgrades + 1 end
                end
            end
        end
        for _, u in ipairs(self:GetListOfUnits(categories.ENGINEER + categories.COMMAND, false)) do
            if Alive(u) then
                if EntityCategoryContains(categories.COMMAND, u) then commander = u else table.insert(engineers, u) end
            end
        end
        -- a builder is given work when idle, or when it has been assisting for kAssistTicks (its assist is dropped)
        local function Free(u)
            local id = u:GetEntityId()
            local t = R.assist[id]
            if t and R.tick - t >= kAssistTicks then
                IssueClearCommands({ u })
                R.assist[id] = nil
                return true
            end
            if u:IsIdleState() then
                R.assist[id] = nil
                return true
            end
            return false
        end
        -- (phase 7b: in the opening, before kReclaimEarlyUntil minutes, at least kReclaimEarly of the engineers reclaim:
        -- the trees and rocks near the start are the quickest mass there is; M28 had 31-56 mass / s at minute 5 to our 17)
        local share = import("/lua/remastered/ai/personality.lua").Knobs(self).reclaimShare
        if ctx.minutes < kReclaimEarlyUntil then share = math.max(share, kReclaimEarly) end
        -- (phase 8) the opening's energy check and the builders' count
        -- (phase 9: short = under kEnergyShort stored AND asked for more than comes in; a dip with income ahead of
        -- the asks is not short. The bar's dips alone had built 94 T1 generators by minute 14.5, 2,860 energy a second
        -- against 1,150 asked)
        -- (2026-11-03, item b) our first factory (entity order) anchors the power; the base structures being built
        -- (extractors and power aside) against 2 + one per 20 mass a second of income (BuildAt)
        R.factoryAt = nil
        if factories[1] then
            local fp = factories[1]:GetPosition()
            R.factoryAt = { fp[1], fp[3] }
        end
        R.unfinished = 0
        for _, u in ipairs(self:GetListOfUnits(categories.STRUCTURE - categories.MASSEXTRACTION - categories.ENERGYPRODUCTION, false)) do
            if not u.Dead and u:GetFractionComplete() < 1 then R.unfinished = R.unfinished + 1 end
        end
        R.unfinishedMax = 2 + math.floor(self:GetEconomyIncome('MASS') * 10 / 20)
        ctx.energyShort = self:GetEconomyStoredRatio('ENERGY') < kEnergyShort and
            self:GetEconomyIncome('ENERGY') < 1.2 * self:GetEconomyRequested('ENERGY')
        ctx.builders = table.getn(engineers) + (commander and 1 or 0)
        -- (phase 13: and at least one per kReclaimPush of the reclaim near the start, at most kReclaimEngineersMax,
        -- whatever the share: M28 reclaimed 27,000-32,000 mass by minute 15-17 with ~140 engineers - about half its
        -- income from minute 12 - largely the wrecks of the fights at our base; we reclaimed 2,000-5,000)
        local nearMass = R.reclaim and R.reclaim.nearMass or 0
        ctx.wantReclaimers = math.min(math.max(math.floor(table.getn(engineers) * share + 0.5),
            math.min(kReclaimEngineersMax, math.floor(nearMass / kReclaimPush))),
            math.floor(nearMass / kReclaimPerWorker))
        -- the commander's safety (phase 7): losing it loses the game. Under kAcuRetreat of its health it drops its
        -- work and backs away from the nearest enemy within kAcuDanger (to the start when none is that near or the
        -- start is away from it), and up to kAcuRepairers free engineers repair it; it takes work again at kAcuBack.
        -- The duels: the commander died at its work, to a frigate off the coast (FAF) and to missiles (M28).
        local repairers = 0
        -- the commander (2026-11-03: RemasterCommander - ships, fight back, fall back) and its work: a job whose
        -- place enemy ships reach is not taken (CommanderJobSafe)
        local acuBusy = commander and self:RemasterCommander(commander, ctx)
        if commander and not acuBusy and Free(commander) then
            self:BuilderJob(commander, factories, true, ctx)
            self:CommanderJobSafe(commander)
        end
        for _, e in ipairs(engineers) do
            if Free(e) then
                if R.acuHurt and repairers < kAcuRepairers then
                    IssueRepair({ e }, commander)
                    repairers = repairers + 1
                else
                    self:BuilderJob(e, factories, false, ctx)
                end
            end
        end
        -- land factories: engineers up to a cap that grows with the game (4 + 2.5 a minute x K.engineerRate, at most
        -- 50), then the cheapest counter that wins against the enemy's known army (choices.lua; the tech's default mix
        -- while nothing is known); a land scout first when the investments wait for one. Air factories: 2 scouts kept
        -- up, then interceptors while the enemy's air is at least 300 mass and 30% of its army. While mass is short and
        -- extractors are upgrading, only the first half of the land factories (id order) take new army orders, so
        -- that the upgrades are not starved (the engine shares a shortage among all who spend).
        -- Map control (phase 6): while our land army is short of what holds the map (K.armyMatch x the enemy's known
        -- ground army, at least K.armyPerMinute a minute from minute kArmyFrom), only every third land factory makes engineers,
        -- and only up to the floor of 4 + 1 a minute; the others make units. The duels showed why: engineers first up
        -- to the cap left 150 mass of army at minute 8 against 2,800, and the enemy's army took our expansions.
        local short = self:GetEconomyStoredRatio('MASS') < 0.05 and ctx.extractorUpgrades > 0
        local landIndex = 0
        local K = import("/lua/remastered/ai/personality.lua").Knobs(self)
        -- (phase 6: one engineer more per kReclaimPerEngineer of reclaim near the start, at most 20, to pick it up)
        -- (phase 13: one per kReclaimPush, at most kReclaimEngineersMax, all of them in the floor below: the floor's
        -- half had kept us at 25-37 engineers while 10,000+ mass lay near the start)
        local reclaimEngineers = math.min(kReclaimEngineersMax,
            math.floor((R.reclaim and R.reclaim.nearMass or 0) / kReclaimPush))
        local engineerCap = math.min(50, 4 + math.floor(2.5 * K.engineerRate * ctx.minutes) + reclaimEngineers)
        local engineerFloor = math.min(engineerCap, 4 + math.floor(ctx.minutes) + reclaimEngineers)
        -- (with fog of war the enemy's army is mostly out of sight: what was seen of it fades slowly, about half in 6
        -- minutes, so the army kept does not shrink each time the scouts lose sight of it)
        R.enemyGroundPeak = math.max(enemyGround, (R.enemyGroundPeak or 0) * kGroundFade)
        -- (phase 9: and a share of our own income - the army we keep grows with the economy, kArmyIncomeRate seconds of
        -- the smoothed mass income per minute after the opening, at most kArmyIncomeMax seconds. With fog the enemy's
        -- known army is a fraction of its real one: at minute 10 our army was 500-1,800 mass against 2,700-6,000, with
        -- the economy ahead of FAF's and three quarters of M28's.)
        local incomeSec = math.min(kArmyIncomeMax, kArmyIncomeRate * math.max(0, ctx.minutes - kOpening)) * K.armyMatch / 0.8
        local armyWanted = math.max(K.armyMatch * R.enemyGroundPeak,
            ctx.minutes >= kArmyFrom and K.armyPerMinute * (ctx.minutes - kArmyFrom + 1) or 0,
            incomeSec * (R.massAvg or 0))
        local armyShort = (R.armyMass or 0) < armyWanted
        if armyShort ~= (R.armyShort or false) and R.tick - (R.armyShortLogged or -10000) >= 600 then
            LOG(string.format('aiarmy %d %d %s army %.0f wanted %.0f (enemy ground %.0f) engineers %d floor %d cap %d',
                R.army, R.tick, armyShort and 'short' or 'enough', R.armyMass or 0, armyWanted, R.enemyGroundPeak,
                table.getn(engineers), engineerFloor, engineerCap))
            R.armyShortLogged = R.tick
        end
        R.armyShort = armyShort
        if math.mod(R.tick, 600) < kThink then  -- (a line a minute: what the economy's builders do)
            local reclaimers = 0
            for _, w in pairs(R.reclaimers or {}) do  -- (a count: the order does not matter)
                if R.tick - w.tick < kReclaimWorkerTicks then reclaimers = reclaimers + 1 end
            end
            LOG(string.format('aieco %d %d engineers %d cap %d floor %d reclaimers %d reclaim near %.0f army %.0f wanted %.0f',
                R.army, R.tick, table.getn(engineers), engineerCap, engineerFloor, reclaimers,
                R.reclaim and R.reclaim.nearMass or 0, R.armyMass or 0, armyWanted))
        end
        local wanted = table.getn(engineers)
        local scouts = table.getn(self:GetListOfUnits(categories.SCOUT * categories.MOBILE, false))
        -- (phase 7b: once we or the enemy are at T2, the T2+ land factories keep kTechEngineers engineers of T2 or
        -- above, whatever the cap: only they build T2 structures such as anti-missile. The duels' probe found no T2
        -- engineer alive from minute 13 to 17 against M28, so the anti-missile rule built nothing.)
        local techEngineers = table.getn(self:GetListOfUnits(categories.ENGINEER * (categories.TECH2 + categories.TECH3) -
            categories.COMMAND, false))
        -- (phase 9: and one more every 3 minutes after minute 8, at most kTechEngineersMax: T2 power gives 8 times a T1
        -- generator's energy for its build, and only T2 engineers build it)
        local wantTech = math.min(kTechEngineersMax, kTechEngineers + math.floor(math.max(0, ctx.minutes - 8) / 3))
        -- (phase 13: with a T2 land HQ, one more every 2 minutes after minute 6, at most kTechEngineersHQ: they build the
        -- support factories)
        if (ctx.landHQ or 0) >= 2 then
            wantTech = math.min(kTechEngineersHQ, kTechEngineers + math.floor(math.max(0, ctx.minutes - 6) / 2))
        end
        local needTech = ((R.enemyTier or 0) >= 2 or ctx.landTier >= 2) and techEngineers < wantTech
        -- (phase 13: the upgrades are chosen first, among the idle factories, and those take no other order: the
        -- production below gave every idle factory an order first, so an upgrade found none idle - one game had 38 T1
        -- land factories and none at T2 at minute 16)
        ctx.upgradeIssued = {}
        self:RemasterUpgrades(factories, ctx)
        for _, f in ipairs(factories) do
            local land = EntityCategoryContains(categories.LAND, f)
            if land then landIndex = landIndex + 1 end
            if ctx.upgradeIssued[f:GetEntityId()] then
                -- (upgrading from now on)
            elseif f:IsIdleState() and land then
                local tech = self:Tech(f)
                if needTech and tech >= 2 then
                    IssueBuildFactory({ f }, self:Bp(tech == 3 and 'l0309' or 'l0208'), 1)
                    techEngineers = techEngineers + 1
                    needTech = techEngineers < wantTech
                    wanted = wanted + 1
                elseif wanted < engineerCap and (not armyShort or math.mod(landIndex, 3) == 1 and wanted < engineerFloor) then
                    IssueBuildFactory({ f }, self:Bp(tech == 3 and 'l0309' or tech == 2 and 'l0208' or 'l0105'), 1)
                    wanted = wanted + 1
                elseif R.scoutFirst and scouts == 0 then
                    IssueBuildFactory({ f }, self:Bp('l0101'), 1)
                    R.scoutFirst = nil
                    scouts = 1
                elseif (not short or landIndex * 2 <= ctx.landFactories + 1) and
                    not ((R.armyCount or 0) >= R.landHard or ((R.armyCount or 0) >= R.landSoft and tech == 1)) and
                    not (tech == 1 and ctx.expBuilding) then
                    -- (2026-11-04, item 1: while an experimental is under construction the T1 factories make no new
                    -- units, so the income reaches the experimental: ExperimentalJob)
                    -- (2026-11-03: not past the soft cap with a T1 factory, nor past the hard cap: the unit spam)
                    IssueBuildFactory({ f }, Choices().LandPick(self, tech), 1)
                end
            elseif f:IsIdleState() and EntityCategoryContains(categories.NAVAL, f) then
                -- (phase 10: the naval factories build ships while ours weigh less than the enemy's: navy.lua)
                -- (a T1 naval factory due for T2 builds nothing, so its upgrade can start: frigates lost to FAF's
                -- destroyers, 0.7-1.9 by build value)
                local due = self:Tech(f) == 1 and ctx.landTier >= 2 and (R.enemyNavalPeak or 0) >= 1000 and
                    not ctx.navalUpgrading
                local ship = not due and Navy().FactoryPick(self, self:Tech(f))
                if ship then IssueBuildFactory({ f }, ship, 1) end
            elseif f:IsIdleState() and EntityCategoryContains(categories.AIR, f) then
                local naval = ctx.navalWanted > ctx.navalAir and Choices().NavalAirPick(self, self:Tech(f))
                if scouts < 2 then
                    IssueBuildFactory({ f }, self:Bp('a0101'), 1)
                    scouts = scouts + 1
                elseif naval then
                    -- (phase 10: ships shelling our coast: the air answers them first)
                    IssueBuildFactory({ f }, naval, 1)
                    ctx.navalAir = ctx.navalAir + (__blueprints[naval].Economy.BuildCostMass or 0)
                elseif ctx.fighters then
                    IssueBuildFactory({ f }, self:Bp('a0102'), 1)
                end
            end
        end
        self:RemasterScouts()
        self:RemasterFighters()
        self:RemasterNavalStrike()
    end,

    -- Tech (phases 4-5): the extractor upgrades are investments by payback against danger (choices.lua, decision
    -- 14); then one land factory at a time, with at least 3 land factories and mass not short, to T2 from minute 7
    -- (20 mass / s) and to T3 from minute 14 (50 mass / s), oldest first (the engine's id order); when the strategy
    -- layer has us catching up a tier behind, at once (2 land factories)
    RemasterUpgrades = function(self, factories, ctx)
        local income = self:GetEconomyIncome('MASS') * 10
        local minutes = ctx.minutes
        Choices().Investments(self, ctx)
        -- (catching up a tech behind, strategy.lua: the next tier now, with 2 land factories and whatever is stored)
        local rush = self.Remaster.techRush
        -- (phase 8: no gate on stored mass any more, the income thresholds below decide: with the opening's economy
        -- all mass is spent as it comes, the bar sat at 0, and no factory ever went to T2 - so no T2 engineer and no
        -- anti-missile)
        -- (phase 10: up to 1 + one per kUpgradeIncome mass a second of factory upgrades at once, instead of one: with up
        -- to 20 factories most stayed T1 and made T1 units against M28's T2 / T3 army - counter values of 0.6-0.7)
        -- (only while the army is not short: more upgrades at once when the army is short cost a game against M28)
        local maxUpgrades = 1 + (self.Remaster.armyShort and 0 or math.floor(income / kUpgradeIncome))
        -- (phase 10: with ships shelling our coast and our land at T2, the air factories go to T2 one at a time, for
        -- gunships and torpedo bombers)
        -- (phase 10: a naval factory goes to T2 when our land is at T2 and the enemy has 1,000 mass of ships or more)
        if (self.Remaster.enemyNavalPeak or 0) >= 1000 and ctx.landTier >= 2 and not ctx.navalUpgrading then
            for _, f in ipairs(factories) do
                if f:IsIdleState() and EntityCategoryContains(categories.NAVAL * categories.TECH1, f) then
                    IssueUpgrade({ f }, self:Bp('b0203'))
                    ctx.upgradeIssued[f:GetEntityId()] = true
                    break
                end
            end
        end
        -- (2026-11-03: also when the enemy's known ships reach 1,000 mass whatever our land's tier, and a busy T1 air
        -- factory too when none is idle - its queue is dropped: an air factory always building scouts, fighters or
        -- bombers was never idle, so it never went to T2, and the answer to Chris's battleships stayed T1 bombers)
        local seaThreat = (self.Remaster.enemyNavalPeak or 0) >= 1000
        if ((self.Remaster.coastMass or 0) > 0 and ctx.landTier >= 2 or seaThreat and not ctx.airT2) and
            not ctx.airUpgrading then
            local pick
            for _, f in ipairs(factories) do
                if not pick and EntityCategoryContains(categories.AIR * categories.TECH1, f) and f:IsIdleState() then pick = f end
            end
            for _, f in ipairs(factories) do
                if not pick and EntityCategoryContains(categories.AIR * categories.TECH1, f) then
                    pick = f
                    IssueClearCommands({ f })
                end
            end
            if pick then
                IssueUpgrade({ pick }, self:Bp('b0202'))
                ctx.upgradeIssued[pick:GetEntityId()] = true
            end
        end
        -- (phase 13: land factories upgrade only to make the next HQ, one at a time; the tier then grows by support
        -- factories (LandFactoryBp). A land factory upgrade under way blocks the next; a new one waits for
        -- kUpgradeEnergy of the energy bar unless on a tech rush. maxUpgrades no longer applies to land factories.)
        self:RemasterSupportUpgrades(factories, ctx, income)
        if ctx.landFactories < (rush and 2 or 3) then return end
        if not rush and self:GetEconomyStoredRatio('ENERGY') < kUpgradeEnergy then return end
        local hq = ctx.landHQ or 0
        for _, u in ipairs(ctx.upgrading) do
            -- (2026-11-04: a support factory's upgrade does not hold the next HQ's)
            if EntityCategoryContains(categories.LAND * categories.FACTORY, u) and
                not (self.Remaster.supportUp and self.Remaster.supportUp[u:GetEntityId()]) then return end
        end
        for _, f in ipairs(factories) do
            if f:IsIdleState() and EntityCategoryContains(categories.LAND, f) and
                not EntityCategoryContains(categories.SUPPORTFACTORY, f) then
                if hq < 2 and EntityCategoryContains(categories.TECH1, f) and (rush or minutes >= 7 and income >= 20) then
                    IssueUpgrade({ f }, self:Bp('b0201'))
                    ctx.upgradeIssued[f:GetEntityId()] = true
                    return
                elseif hq < 3 and EntityCategoryContains(categories.TECH2, f) and (rush or minutes >= 14 and income >= 50) then
                    IssueUpgrade({ f }, self:Bp('b0301'))
                    ctx.upgradeIssued[f:GetEntityId()] = true
                    return
                end
            end
        end
    end,

    -- Support factory upgrades (2026-11-04, item 1: the mass sinks). With a T2 land HQ, an idle T1 land factory goes to
    -- the faction's T2 support factory (zXb9501: 340 mass more, a minute at the factory's own build rate), and with a
    -- T3 HQ an idle T2 support factory to the T3 one (zXb9601). Chris's 4v4 of all our AI: army 7 had 17-18 T1 land
    -- factories beside its T3 HQ from minute 20 to 35 and no support factory - its land army stayed T1 while the
    -- others' was T2-T3; the M28 duels had 16-17 T1 factories and one T3 HQ at minute 20. The support factories
    -- otherwise came only from the few T2+ engineers (LandFactoryBp). At most 1 + one per kSupport.income mass a second
    -- of income at once, and kSupport.share of the land factories, from minute kSupport.from, with the energy for it
    -- (kUpgradeEnergy), and while the army is short only with mass piling up (kSupport.stored): an upgrading factory
    -- makes nothing for its minute. Logs 'aiup <army> <tick> <from> -> <to> (<n> at once)'.
    RemasterSupportUpgrades = function(self, factories, ctx, income)
        local R = self.Remaster
        local hq = ctx.landHQ or 0
        if hq < 2 or ctx.minutes < kSupport.from then return end
        if R.armyShort and self:GetEconomyStoredRatio('MASS') < kSupport.stored then return end
        if self:GetEconomyStoredRatio('ENERGY') < kUpgradeEnergy then return end
        local letter = string.sub(R.prefix, 2, 2)
        local t2, t3 = 'z' .. letter .. 'b9501', 'z' .. letter .. 'b9601'
        local t1 = self:Bp('b0101')
        local function Valid(id)
            local bp = __blueprints[id]
            return bp and bp.Economy and (bp.Economy.BuildCostMass or 0) > 0
        end
        -- the support upgrades under way (an order counts from the step it was given)
        local upgrading = {}
        for _, u in ipairs(ctx.upgrading) do upgrading[u:GetEntityId()] = true end
        local keep, running = {}, 0
        for _, f in ipairs(factories) do
            local id = f:GetEntityId()
            local t = R.supportUp and R.supportUp[id]
            if t and (upgrading[id] or R.tick - t < 3 * kThink) then
                keep[id] = t
                running = running + 1
            end
        end
        R.supportUp = keep
        local most = math.min(1 + math.floor(income / kSupport.income), math.floor(kSupport.share * ctx.landFactories))
        for _, f in ipairs(factories) do
            if running >= most then return end
            local id = f:GetEntityId()
            if f:IsIdleState() and not ctx.upgradeIssued[id] then
                local fid = f:GetUnitId()
                local to = (fid == t1 and Valid(t2) and t2) or (fid == t2 and hq >= 3 and Valid(t3) and t3) or nil
                if to then
                    IssueUpgrade({ f }, to)
                    ctx.upgradeIssued[id] = true
                    R.supportUp[id] = R.tick
                    running = running + 1
                    LOG(string.format('aiup %d %d %s -> %s (%d at once, income %.0f)', R.army, R.tick, fid, to, running, income))
                end
            end
        end
    end,

    -- Scouting (phase 5): idle scouts (air, and a land one when needed) go to the zone looked at longest ago, among the zones with mass and the
    -- other armies' start zones (zone id on ties), so the index and its memory know the enemy's forces and the
    -- operations' "ahead" is measured on what is really there
    RemasterScouts = function(self)
        local R = self.Remaster
        if not R.scoutZones then
            R.scoutZones, R.scouted = {}, {}
            local info = Remastered.AIMapInfo()
            for z = 1, info.zones do
                local zone = Remastered.AIZone(z)
                if z ~= R.startZone and (zone.massPoints[1] or zone.starts[1]) then table.insert(R.scoutZones, z) end
            end
        end
        if not R.scoutZones[1] then return end
        -- (phase 10: and from minute 8 the water beside our coast, R.coastWater (operations.lua), so ships that shell
        -- the base are seen; under fog they had been invisible at sea)
        local zones = R.scoutZones
        if R.coastWater and R.coastWater[1] and GetGameTick() >= 4800 then
            zones = {}
            for _, z in ipairs(R.scoutZones) do table.insert(zones, z) end
            for _, z in ipairs(R.coastWater) do table.insert(zones, z) end
        end
        for _, u in ipairs(self:GetListOfUnits(categories.SCOUT * categories.MOBILE, false)) do
            if Alive(u) and u:IsIdleState() then
                local best
                for _, z in ipairs(zones) do
                    if not best or (R.scouted[z] or -1) < (R.scouted[best] or -1) then best = z end
                end
                R.scouted[best] = R.tick
                local zone = Remastered.AIZone(best)
                IssueMove({ u }, Pos(zone.x, zone.z))
            end
        end
    end,

    -- The air answer to ships (phase 10): idle bombers, gunships and torpedo bombers attack-move to the coast's water
    -- zone with the most enemy ground fire (R.coastThreat, operations.lua); with none, they wait at the start.
    RemasterNavalStrike = function(self)
        local R = self.Remaster
        local idle = {}
        for _, u in ipairs(self:GetListOfUnits(categories.AIR * categories.MOBILE * (categories.BOMBER + categories.GROUNDATTACK + categories.ANTINAVY), false)) do
            if Alive(u) and u:IsIdleState() then table.insert(idle, u) end
        end
        if not idle[1] then return end
        local t = R.coastThreat and R.coastThreat[1]
        if t then
            local zone = Remastered.AIZone(t.zone)
            IssueAggressiveMove(idle, Pos(zone.x, zone.z))
            if R.tick - (R.navalLogged or -10000) >= 600 then
                LOG(string.format('ainaval %d %d strike zone %d with %d aircraft: enemy ships %.0f mass shell our coast',
                    R.army, R.tick, t.zone, table.getn(idle), t.mass))
                R.navalLogged = R.tick
            end
            return
        end
        -- (2026-11-04, Chris's second test: "anything I spawn should be treated as their own unit". Every unit of the
        -- army is in these lists each step - GetListOfUnits sweeps whatever the army owns, built, spawned by a cheat or
        -- CreateUnit, given by an ally or captured - but the bombers and gunships had a job only against ships: without
        -- ships they stood at the base, or patrolled as fighters. Now, with no ships to strike, the idle ones go where the
        -- land army fights: the zone it defends, else the target of its attack or push, else they guard the start.)
        local z = R.defending or (R.op and (R.op.kind == 'attack' or R.op.kind == 'push') and R.op.zone)
        local x, zz = R.startX, R.startZ
        if z then
            local zone = Remastered.AIZone(z)
            x, zz = zone.x, zone.z
        end
        IssueAggressiveMove(idle, Pos(x, zz))
        if z and R.tick - (R.airWingLogged or -10000) >= 600 then
            LOG(string.format('aiair %d %d %d ground-attack aircraft join the land army at zone %d', R.army, R.tick,
                table.getn(idle), z))
            R.airWingLogged = R.tick
        end
    end,

    -- Fighters (phase 5): idle interceptors guard the own economy zone raided last (zone id on ties), else the start
    RemasterFighters = function(self)
        local R = self.Remaster
        local idle = {}
        -- (2026-11-04: not the gunships and bombers that also carry anti-air - a CZAR, a gunship: RemasterNavalStrike)
        for _, u in ipairs(self:GetListOfUnits(categories.AIR * categories.MOBILE * categories.ANTIAIR - categories.SCOUT -
            categories.BOMBER - categories.GROUNDATTACK - categories.ANTINAVY, false)) do
            if Alive(u) and u:IsIdleState() then table.insert(idle, u) end
        end
        if not idle[1] then return end
        local best, bestTick
        for _, z in ipairs(R.ownEconomy or {}) do
            local t = R.raided and R.raided[z]
            if t and (not bestTick or t > bestTick or (t == bestTick and z < best)) then best, bestTick = z, t end
        end
        local x, z = R.startX, R.startZ
        if best then
            local zone = Remastered.AIZone(best)
            x, z = zone.x, zone.z
        end
        IssueAggressiveMove(idle, Pos(x, z))
    end,

    -- the army: the strategy layer's read of the enemy (strategy.lua, phase 5), then the operations (operations.lua)
    RemasterArmy = function(self, step)
        if step == 0 then import("/lua/remastered/ai/strategy.lua").Think(self)
        elseif step == 1 then import("/lua/remastered/ai/operations.lua").Think(self)
        else Navy().Think(self) end
    end,
}
