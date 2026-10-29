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

local function Alive(u)
    return u and not u.Dead and u:GetFractionComplete() >= 1
end

local function Pos(x, z)
    return { x, GetTerrainHeight(x, z), z }
end

local function Choices()  -- (the build choices of phase 5: counters, defences, investments)
    return import("/lua/remastered/ai/choices.lua")
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
            assist = {},   -- builder entity id -> tick its assist began
            tick = 0,
        }
        self:RemasterMassPoints()
        while not ArmyIsOutOfGame(army) do
            self.Remaster.tick = GetGameTick()
            self:RemasterEconomy()
            if math.mod(self.Remaster.tick, 50) < kThink then self:RemasterArmy() end
            WaitTicks(kThink)
        end
    end,

    -- The mass points this army goes for, nearest first by land travel from its start zone (the map analysis), ties by
    -- zone and then by the point's place in the zone; points on islands it cannot walk to are left out
    RemasterMassPoints = function(self)
        local R = self.Remaster
        local list = {}
        local info = Remastered.AIMapInfo()
        for z = 1, info.zones do
            local d = R.startZone > 0 and Remastered.AIZoneDistance(R.startZone, z, 'land') or -1
            if d >= 0 then
                local zone = Remastered.AIZone(z)
                for i, p in ipairs(zone.massPoints) do
                    table.insert(list, { x = p[1], z = p[2], zone = z, i = i, dist = d })
                end
            end
        end
        table.sort(list, function(a, b)
            if a.dist ~= b.dist then return a.dist < b.dist end
            if a.zone ~= b.zone then return a.zone < b.zone end
            return a.i < b.i
        end)
        R.massPoints = list
    end,

    Bp = function(self, suffix) return self.Remaster.prefix .. suffix end,

    Claimed = function(self, x, z)
        local t = self.Remaster.claims[x .. ':' .. z]
        return t and self.Remaster.tick - t < kClaimTicks
    end,

    Claim = function(self, x, z) self.Remaster.claims[x .. ':' .. z] = self.Remaster.tick end,

    -- the next mass point that is free (no extractor, not promised to another builder): without a builder the first
    -- by travel from the start; with one (phase 5) the nearest to it in a straight line, leaving out the zones the
    -- enemy is known in (R.enemyAt, kept by the operations), list order on ties
    NextMassPoint = function(self, u)
        local bp = self:Bp('b1103')
        local R = self.Remaster
        if not u then
            for _, p in ipairs(R.massPoints) do
                if not self:Claimed(p.x, p.z) and self:CanBuildStructureAt(bp, Pos(p.x, p.z)) then return p end
            end
            return
        end
        local q = u:GetPosition()
        local best, bestD
        local enemyAt = R.enemyAt or {}
        for _, p in ipairs(R.massPoints) do
            if not enemyAt[p.zone] or p.zone == R.startZone then
                local dx, dz = p.x - q[1], p.z - q[3]
                local d = dx * dx + dz * dz
                if (not bestD or d < bestD) and not self:Claimed(p.x, p.z) and self:CanBuildStructureAt(bp, Pos(p.x, p.z)) then
                    best, bestD = p, d
                end
            end
        end
        return best
    end,

    -- a free build spot of at least `size` in the start zone for blueprint suffix `what`
    BuildSpot = function(self, what, size)
        local R = self.Remaster
        if R.startZone <= 0 then return end
        local bp = self:Bp(what)
        for _, p in ipairs(Remastered.AIBuildSpots(R.startZone, size)) do
            if not self:Claimed(p[1], p[2]) and self:CanBuildStructureAt(bp, Pos(p[1], p[2])) then return p end
        end
    end,

    -- energy (phase 5): short now, or less than 1.2x what is asked plus a margin (built ahead of need), but never more
    -- than 12 energy per mass of income plus 30 a second while some is stored (the opening's mass comes first)
    NeedPower = function(self)
        local income = self:GetEconomyIncome('ENERGY')
        local requested = self:GetEconomyRequested('ENERGY')
        local stored = self:GetEconomyStoredRatio('ENERGY')
        if stored < 0.15 then return true end
        if income * 10 > 12 * self:GetEconomyIncome('MASS') * 10 + 30 then return false end
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

    -- build `what` (a blueprint suffix) at a free spot of at least `size` in the start zone
    BuildAt = function(self, u, what, size)
        local s = self:BuildSpot(what, size)
        if not s then return false end
        self:Claim(s[1], s[2])
        IssueBuildMobile({ u }, Pos(s[1], s[2]), self:Bp(what), {})
        return true
    end,

    -- reclaim (phase 5): the props with mass (trees, rocks, wrecks) within 40 units of the builder, nearest first
    -- (entity id on ties), up to 6 into its queue; false when there are none
    Reclaim = function(self, u)
        local p = u:GetPosition()
        local x, z = p[1], p[3]
        local list = GetReclaimablesInRect(x - 40, z - 40, x + 40, z + 40)
        if not list then return false end
        local c = {}
        for _, v in ipairs(list) do
            if not v.Dead and v.MaxMassReclaim and v.MaxMassReclaim * (v.ReclaimLeft or 1) >= 5 then
                local q = v:GetPosition()
                local dx, dz = q[1] - x, q[3] - z
                table.insert(c, { v = v, d = dx * dx + dz * dz, id = tonumber(v:GetEntityId()) or 0 })
            end
        end
        if not c[1] then return false end
        table.sort(c, function(m, n)
            if m.d ~= n.d then return m.d < n.d end
            return m.id < n.id
        end)
        for i = 1, math.min(6, table.getn(c)) do IssueReclaim({ u }, c[i].v) end
        return true
    end,

    -- One builder's next job (phase 5), so that economy, army, scouting and expansion go on at the same time:
    -- 1. the first land factory (the commander), then its own zone's mass points in the opening;
    -- 2. power of the builder's tech when energy is short or not ahead of need (a few builders at a time);
    -- 3. land factories up to a floor that grows with the minutes, more while mass piles up;
    -- 4. a point defence where the enemy will come, when it is the cheaper answer (choices.lua);
    -- 5. an air factory (scouts) and a radar;
    -- 6. the nearest free mass point away from known enemies, and up to 2 more in its zone;
    -- 7. reclaim around it while mass is not piling up; more factories; mass storage when mass is full;
    -- 8. assist an upgrade in progress (extractors first), else a factory, looked at again after kAssistTicks.
    BuilderJob = function(self, u, factories, isCommander, ctx)
        if table.getn(factories) == 0 and isCommander and self:BuildAt(u, 'b0101', 8) then return end
        -- the opening: the commander takes its own zone's mass before anything but the first factory, unless energy
        -- runs out
        local p = self:NextMassPoint()
        if isCommander and ctx.minutes < 4 and p and p.dist <= 0 and self:GetEconomyStoredRatio('ENERGY') >= 0.3 then
            self:Claim(p.x, p.z)
            IssueBuildMobile({ u }, Pos(p.x, p.z), self:Bp('b1103'), {})
            return
        end
        -- power, at most 1 + one per 40 energy / s of income being built at once (power costs energy to build: a
        -- crowd of builders on it at once starves everything)
        if ctx.powerBuilding < 1 + math.floor(self:GetEconomyIncome('ENERGY') * 10 / 40) and self:NeedPower() then
            local tech = self:Tech(u)
            if self:BuildAt(u, tech == 3 and 'b1301' or tech == 2 and 'b1201' or 'b1101', tech == 3 and 8 or 4) then
                ctx.powerBuilding = ctx.powerBuilding + 1
                return
            end
        end
        -- land factories (one per ~6 mass / s, at most 12): from minute 3 up to 1 + one per 1.5 minutes whatever is
        -- stored (the army grows with the economy), the rest while mass piles up; before the next far mass point
        local storedMass = self:GetEconomyStoredRatio('MASS')
        local wantFactories = math.min(12, 1 + math.floor(self:GetEconomyIncome('MASS') * 10 / 6))
        local floorFactories = ctx.minutes >= 3 and math.min(wantFactories, 1 + math.floor(ctx.minutes / 1.5)) or 1
        if (storedMass > 0.6 and ctx.landFactories < wantFactories or ctx.landFactories < floorFactories) and
            self:BuildAt(u, 'b0101', 8) then
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
        -- scouting: an air factory from minute 3 (its scouts), a radar from minute 5
        if not ctx.airFactory and ctx.minutes >= 3 and self:BuildAt(u, 'b0102', 8) then
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
        p = self:NextMassPoint(not isCommander and u or nil)
        if p and not (isCommander and p.dist > 0) then
            self:Claim(p.x, p.z)
            IssueBuildMobile({ u }, Pos(p.x, p.z), self:Bp('b1103'), {})
            -- an engineer takes up to 2 more free points of the same zone in the same walk (the zone's point order)
            if not isCommander then
                local more, bp = 0, self:Bp('b1103')
                for _, q in ipairs(self.Remaster.massPoints) do
                    if more >= 2 then break end
                    if q.zone == p.zone and not self:Claimed(q.x, q.z) and self:CanBuildStructureAt(bp, Pos(q.x, q.z)) then
                        self:Claim(q.x, q.z)
                        IssueBuildMobile({ u }, Pos(q.x, q.z), bp, {})
                        more = more + 1
                    end
                end
            end
            return
        end
        if self:GetEconomyStoredRatio('MASS') < 0.5 and self:Reclaim(u) then return end
        if ctx.landFactories < wantFactories and storedMass > 0.2 and self:BuildAt(u, 'b0101', 8) then
            ctx.landFactories = ctx.landFactories + 1
            return
        end
        if storedMass > 0.8 and ctx.minutes >= 6 and ctx.storages < 4 then
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
        local target = ctx.upgrading[1]
        if not target and factories[1] then
            ctx.assistNext = math.mod(ctx.assistNext, table.getn(factories)) + 1
            target = factories[ctx.assistNext]
        end
        if target then
            IssueGuard({ u }, target)
            self.Remaster.assist[u:GetEntityId()] = self.Remaster.tick
        end
    end,

    RemasterEconomy = function(self)
        local factories, engineers, commander = {}, {}, nil
        local ctx = { minutes = GetGameTick() / 600, landFactories = 0, upgrading = {}, storages = 0, assistNext = 0,
            factoryUpgrades = 0, extractorUpgrades = 0, airFactories = 0 }
        -- the enemy's air (choices.lua): our air factories and fighters answer it
        local enemyAir, enemyGround = Choices().EnemyAir(self)
        ctx.wantAir = math.min(4, 1 + math.floor(enemyAir / 1500))
        ctx.fighters = enemyAir >= 300 and enemyAir >= 0.3 * (enemyAir + enemyGround)
        local R = self.Remaster
        for _, u in ipairs(self:GetListOfUnits(categories.FACTORY * categories.STRUCTURE, false)) do
            if not u.Dead then
                -- (counted from the start of their building: no second factory for the same need)
                if EntityCategoryContains(categories.AIR, u) then
                    ctx.airFactory = true
                    ctx.airFactories = ctx.airFactories + 1
                end
                if EntityCategoryContains(categories.LAND, u) then ctx.landFactories = ctx.landFactories + 1 end
                if Alive(u) then table.insert(factories, u) end
            end
        end
        for _, u in ipairs(self:GetListOfUnits(categories.RADAR * categories.STRUCTURE, false)) do
            if not u.Dead then ctx.radar = true end
        end
        ctx.powerBuilding = 0
        for _, u in ipairs(self:GetListOfUnits(categories.ENERGYPRODUCTION * categories.STRUCTURE, false)) do
            if not u.Dead and u:GetFractionComplete() < 1 then ctx.powerBuilding = ctx.powerBuilding + 1 end
        end
        ctx.defenceBuilding = 0
        for _, u in ipairs(self:GetListOfUnits(categories.DEFENSE * categories.STRUCTURE, false)) do
            if not u.Dead and u:GetFractionComplete() < 1 then ctx.defenceBuilding = ctx.defenceBuilding + 1 end
        end
        ctx.storages = table.getn(self:GetListOfUnits(categories.MASSSTORAGE * categories.STRUCTURE, false))
        -- upgrades in progress, extractors first (what idle builders assist)
        for kind, cat in ipairs({ categories.MASSEXTRACTION, categories.FACTORY }) do
            for _, u in ipairs(self:GetListOfUnits(cat * categories.STRUCTURE, false)) do
                if not u.Dead and u:IsUnitState('Upgrading') then
                    table.insert(ctx.upgrading, u)
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
        if commander and Free(commander) then self:BuilderJob(commander, factories, true, ctx) end
        for _, e in ipairs(engineers) do
            if Free(e) then self:BuilderJob(e, factories, false, ctx) end
        end
        -- land factories: engineers up to a cap that grows with the game (4 + 2.5 a minute, at most 50), then the
        -- cheapest counter that wins against the enemy's known army (choices.lua; the tech's default mix while nothing
        -- is known); a land scout first when the investments wait for one. Air factories: 2 scouts kept up, then
        -- interceptors while the enemy's air is at least 300 mass and 30% of its army. While mass is short and extractors are upgrading, only the first
        -- half of the land factories (id order) take new army orders, so that the upgrades are not starved (the
        -- engine shares a shortage among all who spend).
        local short = self:GetEconomyStoredRatio('MASS') < 0.05 and ctx.extractorUpgrades > 0
        local landIndex = 0
        local engineerCap = math.min(50, 4 + math.floor(2.5 * ctx.minutes))
        local wanted = table.getn(engineers)
        local scouts = table.getn(self:GetListOfUnits(categories.SCOUT * categories.MOBILE, false))
        for _, f in ipairs(factories) do
            local land = EntityCategoryContains(categories.LAND, f)
            if land then landIndex = landIndex + 1 end
            if f:IsIdleState() and land then
                local tech = self:Tech(f)
                if wanted < engineerCap then
                    IssueBuildFactory({ f }, self:Bp(tech == 3 and 'l0309' or tech == 2 and 'l0208' or 'l0105'), 1)
                    wanted = wanted + 1
                elseif R.scoutFirst and scouts == 0 then
                    IssueBuildFactory({ f }, self:Bp('l0101'), 1)
                    R.scoutFirst = nil
                    scouts = 1
                elseif not short or landIndex * 2 <= ctx.landFactories + 1 then
                    IssueBuildFactory({ f }, Choices().LandPick(self, tech), 1)
                end
            elseif f:IsIdleState() and EntityCategoryContains(categories.AIR, f) then
                if scouts < 2 then
                    IssueBuildFactory({ f }, self:Bp('a0101'), 1)
                    scouts = scouts + 1
                elseif ctx.fighters then
                    IssueBuildFactory({ f }, self:Bp('a0102'), 1)
                end
            end
        end
        self:RemasterUpgrades(factories, ctx)
        self:RemasterScouts()
        self:RemasterFighters()
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
        if ctx.factoryUpgrades > 0 or ctx.landFactories < (rush and 2 or 3) or
            (not rush and self:GetEconomyStoredRatio('MASS') < 0.1) then return end
        for _, f in ipairs(factories) do
            if f:IsIdleState() and EntityCategoryContains(categories.LAND, f) then
                if EntityCategoryContains(categories.TECH1, f) and (rush or minutes >= 7 and income >= 20) then
                    IssueUpgrade({ f }, self:Bp('b0201'))
                    return
                elseif EntityCategoryContains(categories.TECH2, f) and (rush or minutes >= 14 and income >= 50) then
                    IssueUpgrade({ f }, self:Bp('b0301'))
                    return
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
        for _, u in ipairs(self:GetListOfUnits(categories.SCOUT * categories.MOBILE, false)) do
            if Alive(u) and u:IsIdleState() then
                local best
                for _, z in ipairs(R.scoutZones) do
                    if not best or (R.scouted[z] or -1) < (R.scouted[best] or -1) then best = z end
                end
                R.scouted[best] = R.tick
                local zone = Remastered.AIZone(best)
                IssueMove({ u }, Pos(zone.x, zone.z))
            end
        end
    end,

    -- Fighters (phase 5): idle interceptors guard the own economy zone raided last (zone id on ties), else the start
    RemasterFighters = function(self)
        local R = self.Remaster
        local idle = {}
        for _, u in ipairs(self:GetListOfUnits(categories.AIR * categories.MOBILE * categories.ANTIAIR - categories.SCOUT, false)) do
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
    RemasterArmy = function(self)
        import("/lua/remastered/ai/strategy.lua").Think(self)
        import("/lua/remastered/ai/operations.lua").Think(self)
    end,
}
