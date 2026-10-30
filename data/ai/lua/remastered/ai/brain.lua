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
local kReclaimPerEngineer = 400               -- one engineer more per this much reclaim near the start (at most 20)
local kGroundFade = 0.998                      -- what was seen of the enemy's ground army fades by this a step (10 ticks)
local kMassNear = 120                         -- world units: a free mass point this near an engineer comes before reclaim

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

    -- The reclaim map (phase 6): the mass lying on the ground (props of kReclaimMin or more) per cell of
    -- kReclaimCell, one row of cells read each economy step, so the whole map is read every (rows x kThink) ticks
    -- without one big scan. The duels showed why: M28 made ~130 mass / s from reclaim at minute 10, with ~100
    -- engineers, while ours picked up only what lay within 40 units of an idle builder. Each cell keeps its zone
    -- (land) and whether our land units reach it, worked out once.
    ReclaimScan = function(self)
        local R = self.Remaster
        local g = R.reclaim
        if not g then
            local w, h = GetMapSize()
            g = { w = w, h = h, cols = math.ceil(w / kReclaimCell), rows = math.ceil(h / kReclaimCell), row = 0,
                mass = {}, zone = {}, near = {}, nearMass = 0 }
            for i = 1, g.cols * g.rows do
                local cx = (math.mod(i - 1, g.cols) + 0.5) * kReclaimCell
                local cz = (math.floor((i - 1) / g.cols) + 0.5) * kReclaimCell
                local z = Remastered.AIZoneAt(cx, cz, 'land')
                if z > 0 and R.startZone > 0 and Remastered.AIZoneDistance(R.startZone, z, 'land') < 0 then z = 0 end
                g.zone[i] = z
                local dx, dz = cx - R.startX, cz - R.startZ
                g.near[i] = dx * dx + dz * dz <= kReclaimNear * kReclaimNear
                g.mass[i] = 0
            end
            R.reclaim = g
        end
        local row = g.row
        g.row = math.mod(row + 1, g.rows)
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
        local enemyAt = R.enemyAt or {}
        local best, bestScore
        for i = 1, g.cols * g.rows do
            local m = g.mass[i]
            local z = g.zone[i]
            if m >= kReclaimField and z > 0 and not enemyAt[z] and (workers[i] or 0) < 1 + m / kReclaimPerWorker then
                local cx = (math.mod(i - 1, g.cols) + 0.5) * kReclaimCell
                local cz = (math.floor((i - 1) / g.cols) + 0.5) * kReclaimCell
                local dx, dz = cx - q[1], cz - q[3]
                local score = m / (math.sqrt(dx * dx + dz * dz) + 150)
                if not bestScore or score > bestScore then best, bestScore = i, score end
            end
        end
        if not best then return false end
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
        R.reclaimers[u:GetEntityId()] = { cell = best, tick = R.tick }
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
        if self:GetEconomyStoredRatio('MASS') < 0.5 and (self:ReclaimField(u) or self:Reclaim(u)) then return end
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
        self:ReclaimScan()
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
        -- the engineers reclaiming now, and how many should (phase 6, K.reclaimShare)
        ctx.reclaimers = 0
        for _, w in pairs(R.reclaimers or {}) do  -- (a count: the order does not matter)
            if R.tick - w.tick < kReclaimWorkerTicks then ctx.reclaimers = ctx.reclaimers + 1 end
        end
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
        ctx.wantReclaimers = math.min(math.floor(table.getn(engineers) *
            import("/lua/remastered/ai/personality.lua").Knobs(self).reclaimShare),
            math.floor((R.reclaim and R.reclaim.nearMass or 0) / kReclaimPerWorker))
        if commander and Free(commander) then self:BuilderJob(commander, factories, true, ctx) end
        for _, e in ipairs(engineers) do
            if Free(e) then self:BuilderJob(e, factories, false, ctx) end
        end
        -- land factories: engineers up to a cap that grows with the game (4 + 2.5 a minute x K.engineerRate, at most
        -- 50), then the cheapest counter that wins against the enemy's known army (choices.lua; the tech's default mix
        -- while nothing is known); a land scout first when the investments wait for one. Air factories: 2 scouts kept
        -- up, then interceptors while the enemy's air is at least 300 mass and 30% of its army. While mass is short and
        -- extractors are upgrading, only the first half of the land factories (id order) take new army orders, so
        -- that the upgrades are not starved (the engine shares a shortage among all who spend).
        -- Map control (phase 6): while our land army is short of what holds the map (K.armyMatch x the enemy's known
        -- ground army, at least K.armyPerMinute a minute from minute 2), only every third land factory makes engineers,
        -- and only up to the floor of 4 + 1 a minute; the others make units. The duels showed why: engineers first up
        -- to the cap left 150 mass of army at minute 8 against 2,800, and the enemy's army took our expansions.
        local short = self:GetEconomyStoredRatio('MASS') < 0.05 and ctx.extractorUpgrades > 0
        local landIndex = 0
        local K = import("/lua/remastered/ai/personality.lua").Knobs(self)
        -- (phase 6: one engineer more per kReclaimPerEngineer of reclaim near the start, at most 20, to pick it up)
        local reclaimEngineers = math.min(20, math.floor((R.reclaim and R.reclaim.nearMass or 0) / kReclaimPerEngineer))
        local engineerCap = math.min(50, 4 + math.floor(2.5 * K.engineerRate * ctx.minutes) + reclaimEngineers)
        local engineerFloor = math.min(engineerCap, 4 + math.floor(ctx.minutes) + math.floor(reclaimEngineers / 2))
        -- (with fog of war the enemy's army is mostly out of sight: what was seen of it fades slowly, about half in 6
        -- minutes, so the army kept does not shrink each time the scouts lose sight of it)
        R.enemyGroundPeak = math.max(enemyGround, (R.enemyGroundPeak or 0) * kGroundFade)
        local armyWanted = math.max(K.armyMatch * R.enemyGroundPeak,
            ctx.minutes >= 2 and K.armyPerMinute * (ctx.minutes - 2) or 0)
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
        for _, f in ipairs(factories) do
            local land = EntityCategoryContains(categories.LAND, f)
            if land then landIndex = landIndex + 1 end
            if f:IsIdleState() and land then
                local tech = self:Tech(f)
                if wanted < engineerCap and (not armyShort or math.mod(landIndex, 3) == 1 and wanted < engineerFloor) then
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
