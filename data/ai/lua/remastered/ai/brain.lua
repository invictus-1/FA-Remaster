-- FA Remastered's own AI: the brain (docs/ai-architecture.md). Part of FA Remastered, MIT licence (see LICENSE); our
-- own design and code, nothing taken from M28AI.
--
-- Phase 3: the brain's own random stream and think thread, its options, Chris's rating corrections, and a first
-- economy built on the map analysis: the commander and the engineers take mass points by travel distance, power when
-- energy runs short, factories as income allows; the factories make engineers and a land mix; the army holds the base
-- and answers enemies in its own zones. The strategic and operation layers come in phase 4.
--
-- Determinism (docs/ai-architecture.md 5): the thread wakes on fixed ticks; unit lists are in entity id order (the
-- engine's GetListOfUnits) and every walk over a table keyed by strings is sorted first; every random choice is the
-- army's own stream (Remastered.AIRandom), never the sim's Random; no clock is read.
local StandardBrain = import("/lua/aibrain.lua").AIBrain

local kPrefix = { 'ue', 'ua', 'ur', 'xs' }  -- blueprint id prefixes by faction index (UEF, Aeon, Cybran, Seraphim)
local kThink = 10                             -- ticks between economy steps
local kClaimTicks = 600                       -- a build site promised to a builder stays its own this long

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

    -- the nearest mass point that is free (no extractor, not promised to another builder)
    NextMassPoint = function(self)
        local bp = self:Bp('b1103')
        for _, p in ipairs(self.Remaster.massPoints) do
            if not self:Claimed(p.x, p.z) and self:CanBuildStructureAt(bp, Pos(p.x, p.z)) then return p end
        end
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

    NeedPower = function(self)
        local income = self:GetEconomyIncome('ENERGY')
        local requested = self:GetEconomyRequested('ENERGY')
        local stored = self:GetEconomyStoredRatio('ENERGY')
        return stored < 0.1 or (income < requested * 1.05 and stored < 0.9)
    end,

    -- one builder's next job: the first factory, power when short, another factory while mass piles up, mass, else
    -- assist a factory
    BuilderJob = function(self, u, factories, isCommander)
        if table.getn(factories) == 0 and isCommander then
            local s = self:BuildSpot('b0101', 8)
            if s then
                self:Claim(s[1], s[2])
                IssueBuildMobile({ u }, Pos(s[1], s[2]), self:Bp('b0101'), {})
                return
            end
        end
        if self:NeedPower() then
            local s = self:BuildSpot('b1101', 4)
            if s then
                self:Claim(s[1], s[2])
                IssueBuildMobile({ u }, Pos(s[1], s[2]), self:Bp('b1101'), {})
                return
            end
        end
        local wantFactories = 1 + math.floor(self:GetEconomyIncome('MASS') * 10 / 8)  -- (a factory per ~8 mass / s)
        local p = self:NextMassPoint()
        if table.getn(factories) < math.min(wantFactories, 6) and self:GetEconomyStoredRatio('MASS') > 0.2 then
            local s = self:BuildSpot('b0101', 8)
            if s then
                self:Claim(s[1], s[2])
                IssueBuildMobile({ u }, Pos(s[1], s[2]), self:Bp('b0101'), {})
                return
            end
        end
        if p and not (isCommander and p.dist > 0) then  -- (the commander keeps to its own zone's mass)
            self:Claim(p.x, p.z)
            IssueBuildMobile({ u }, Pos(p.x, p.z), self:Bp('b1103'), {})
            return
        end
        if factories[1] then IssueGuard({ u }, factories[1]) end
    end,

    RemasterEconomy = function(self)
        local factories, engineers, commander = {}, {}, nil
        for _, u in ipairs(self:GetListOfUnits(categories.FACTORY * categories.STRUCTURE, false)) do
            if Alive(u) then table.insert(factories, u) end
        end
        for _, u in ipairs(self:GetListOfUnits(categories.ENGINEER + categories.COMMAND, false)) do
            if Alive(u) then
                if EntityCategoryContains(categories.COMMAND, u) then commander = u else table.insert(engineers, u) end
            end
        end
        if commander and commander:IsIdleState() then self:BuilderJob(commander, factories, true) end
        for _, e in ipairs(engineers) do
            if e:IsIdleState() then self:BuilderJob(e, factories, false) end
        end
        -- factories: engineers up to a cap that grows with the game, then the land mix by the army's own stream
        local minutes = GetGameTick() / 600
        local engineerCap = math.min(12, 3 + math.floor(minutes))
        local wanted = table.getn(engineers)
        for _, f in ipairs(factories) do
            if f:IsIdleState() and EntityCategoryContains(categories.LAND, f) then
                if wanted < engineerCap then
                    IssueBuildFactory({ f }, self:Bp('l0105'), 1)
                    wanted = wanted + 1
                else
                    local roll = Remastered.AIRandom(self.Remaster.army)
                    local what = roll < 0.6 and 'l0201' or roll < 0.8 and 'l0103' or 'l0104'
                    IssueBuildFactory({ f }, self:Bp(what), 1)
                end
            end
        end
    end,

    -- the army (until phase 4's operations): idle combat units gather at the start; enemies in the start zone or next
    -- to it are met with everything idle there
    RemasterArmy = function(self)
        local R = self.Remaster
        local idle = {}
        for _, u in ipairs(self:GetListOfUnits(categories.MOBILE * categories.LAND - categories.ENGINEER - categories.COMMAND, false)) do
            if Alive(u) and u:IsIdleState() then table.insert(idle, u) end
        end
        if not idle[1] or R.startZone <= 0 then return end
        local target
        local zones = { R.startZone }
        for _, n in ipairs(Remastered.AIZone(R.startZone).neighbours) do table.insert(zones, n) end
        for _, z in ipairs(zones) do
            local f = Remastered.AIZoneForces(R.army, z)
            if f.enemy.land.count + f.enemy.structure.count > 0 then
                local zone = Remastered.AIZone(z)
                target = Pos(zone.x, zone.z)
                break
            end
        end
        IssueAggressiveMove(idle, target or Pos(R.startX, R.startZ))
    end,
}
