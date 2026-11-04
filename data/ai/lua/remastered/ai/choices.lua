-- FA Remastered's own AI: the build choices of phase 5 (docs/ai-architecture.md 2.4-2.5; Chris's decisions 13 and 14
-- in docs/ai-design-goals.md). Part of FA Remastered, MIT licence (see LICENSE); our own design and code, nothing
-- taken from M28AI.
--
-- - COUNTER: what a land factory builds is the cheapest counter that wins against the enemy's army as the army knows
--   it (Remastered.AIEnemyMix): the candidate with the most enemy mass killed per own mass lost
--   (Remastered.AIBuildValue); its anti-air answer gets a share of the factories as large as the air's share of the
--   enemy's mass. With nothing known yet, the tech's default mix.
-- - DEFENCES: point defence is a build candidate beside the units, cheaper per threat but fixed: it is worth it only
--   where the enemy will come (an expansion of ours that was raided, a choke beside our economy that the enemy was
--   seen beyond), and only when its build value, marked down for being fixed, beats the best mobile counter.
-- - INVESTMENTS (decision 14): an extractor upgrade is weighed by its payback (cost / extra mass a second) against the
--   danger to its zone within that time (the zone estimate with the enemy that can travel there in the payback
--   window). A planned dip in stored mass is fine; a sustained shortage (K.stallTolerance) or a spend the income
--   cannot cover within kDipSeconds is not. With no enemy known at all the army scouts first. A big enemy army that
--   cannot reach the zone in time is no reason to wait. Each decision logs "aiinv ..." with payback, danger, reason.
--
-- Determinism (docs/ai-architecture.md 5): lists in fixed order (candidate tables, entity id order), ties by id,
-- random choices from the army's own stream only.

local kCounterTicks = 300      -- the counter-pick is worked out again after this long
local kMixMass = 150           -- below this much known enemy army mass the tech's default mix is built
local kRaidMemory = 3000       -- ticks a raid on an expansion keeps it a defence site
local kSeenMemory = 3000       -- ticks an enemy seen in a zone keeps a choke beside it a defence site
local kPendingTicks = 450      -- a defence ordered counts as under way this long before it is started
local kDipSeconds = 120        -- the investments under way may cost at most K.investShare of this many seconds of
                               -- mass income (smoothed), plus half of what is stored (the planned dip)
-- (the defence mark-down, the defences per site and under construction, the stall tolerance, the investment share
-- and its safe margin are the knobs of personality.lua)
local kLandSpeed = 3           -- world units a second: how far the enemy's land army travels in the payback window
-- (personality.lua's kEconomy, with its range)
local kInvestFloat = import("/lua/remastered/ai/personality.lua").Tuning().investFloat  -- mass bar share from which the
                               -- overflow goes to extractor upgrades first (phase 13)
local kInvestEnergy = 0.5      -- stored energy share under which no new extractor upgrade starts (phase 8)
local kHoldLogTicks = 600      -- a "hold" decision for the same kind of investment is logged at most this often
-- (2026-11-04, item 1: the mass sinks) the planned dip: with no extractor upgrade under way, one whose payback is at most
-- this many seconds goes ahead past the budget and past an empty bar (decision 14: "the risk is a short mass dip, the
-- reward a doubled mass income"). In the 4v4 of all our AI five of the eight armies sat on 10-13 T2 extractors from
-- minute 15 to 35 at 60-80 mass a second: a T3 upgrade (4,600 mass, payback 383 s) was over the budget (half of 120 s
-- of income, halved again while the army is short: 1,000-2,700) in 79 decisions and blocked by the empty bar - all
-- the mass spent on units, the bar at 0-4%, counted as a "sustained deficit" - in 120 more. Only from minute
-- kDipFrom (the opening's mass is the opening's: a first try upgraded a T1 extractor at minute 2) and for an upgrade
-- that costs at most kDipSeconds of the income now (a dip, not a stall).
local kDipPayback = 600
local kDipFrom = 8

-- the land candidates by factory tech (blueprint suffixes; the faction prefix is added): direct-fire, artillery,
-- anti-air, assault bots; a higher-tech factory can build the lower tiers' too. Missing ids are left out.
local kLand = {
    { 'l0201', 'l0106', 'l0107', 'l0103', 'l0104' },
    { 'l0202', 'l0203', 'l0111', 'l0205' },
    { 'l0303', 'l0304', 'l0305' },
}
local function Knobs(brain)
    return import("/lua/remastered/ai/personality.lua").Knobs(brain)
end

local kDefault = {  -- the tech's mix while nothing is known: suffix, weight
    { { 'l0201', 0.6 }, { 'l0103', 0.2 }, { 'l0104', 0.2 } },
    { { 'l0202', 0.8 }, { 'l0205', 0.2 } },
    { { 'l0303', 0.75 }, { 'l0304', 0.25 } },
}

local function Valid(id)
    local bp = __blueprints[id]
    return bp and bp.Economy and (bp.Economy.BuildCostMass or 0) > 0
end

local function Candidates(brain, tech)
    local R = brain.Remaster
    R.candidates = R.candidates or {}
    if not R.candidates[tech] then
        local list = {}
        for t = tech, 1, -1 do
            for _, s in ipairs(kLand[t]) do
                local id = brain:Bp(s)
                if Valid(id) then table.insert(list, id) end
            end
        end
        R.candidates[tech] = list
    end
    return R.candidates[tech]
end

local function IsAntiAir(id)
    local bp = __blueprints[id]
    if not bp or not bp.Categories then return false end
    for _, c in ipairs(bp.Categories) do if c == 'ANTIAIR' then return true end end
    return false
end

-- the enemy army as known: the ground part and the air part (AIBuildValue's force form) and their masses
local function Mix(R)
    local ground, air, gm, am = {}, {}, 0, 0
    for _, e in ipairs(Remastered.AIEnemyMix(R.army)) do
        local bp = __blueprints[e[1]]
        local isAir = false
        if bp and bp.Categories then
            for _, c in ipairs(bp.Categories) do if c == 'AIR' then isAir = true end end
        end
        if isAir then
            table.insert(air, { e[1], e[2] })
            am = am + e.mass
        else
            table.insert(ground, { e[1], e[2] })
            gm = gm + e.mass
        end
    end
    return ground, air, gm, am
end

-- the best candidate by build value against a force (id order on ties); nil when none kills anything
local function Best(list, force, filter)
    local best, bestV
    for _, id in ipairs(list) do
        if filter == nil or filter(id) then
            local v = Remastered.AIBuildValue(id, force)
            if v > 0 and (not bestV or v > bestV or (v == bestV and id < best)) then best, bestV = id, v end
        end
    end
    return best, bestV or 0
end

-- The counter-pick of a tech, worked out every kCounterTicks: {ground = id, groundValue, air = id, airShare}
function Counter(brain, tech)
    local R = brain.Remaster
    R.counter = R.counter or {}
    local c = R.counter[tech]
    if c and R.tick - c.tick < kCounterTicks then return c end
    local ground, air, gm, am = Mix(R)
    c = { tick = R.tick }
    if gm + am >= kMixMass then
        local list = Candidates(brain, tech)
        if gm > 0 then c.ground, c.groundValue = Best(list, ground, function(id) return not IsAntiAir(id) end) end
        if am > 0 then c.air, c.airValue = Best(list, air, IsAntiAir) end
        c.airShare = am / (gm + am)
        LOG(string.format('aicounter %d %d T%d ground %s value %.2f vs %.0f mass | air %s value %.2f share %.2f',
            R.army, R.tick, tech, tostring(c.ground), c.groundValue or 0, gm, tostring(c.air), c.airValue or 0,
            c.airShare))
    end
    R.counter[tech] = c
    return c
end

-- what one idle land factory of `tech` builds next
function LandPick(brain, tech)
    local R = brain.Remaster
    local c = Counter(brain, tech)
    local roll = Remastered.AIRandom(R.army)
    if c.ground or c.air then
        if c.air and (roll < c.airShare or not c.ground) then return c.air end
        return c.ground
    end
    local acc = 0
    for _, w in ipairs(kDefault[tech]) do
        acc = acc + w[2]
        if roll < acc then return brain:Bp(w[1]) end
    end
    return brain:Bp(kDefault[tech][1][1])
end

-- the enemy's known air and ground army mass (for the air factories)
function EnemyAir(brain)
    local _, _, gm, am = Mix(brain.Remaster)
    return am, gm
end

-- ---- the air answer to ships (phase 10) ----

-- the air candidates against ships by air factory tech (blueprint suffixes): bombers, gunships, torpedo bombers
local kNavalAir = { { 'a0103' }, { 'a0203', 'a0204' }, { 'a0304' } }

-- the enemy's ships as known ({id, count}), from the whole known enemy
local function NavalMix(R)
    local list, mass = {}, 0
    for _, e in ipairs(Remastered.AIEnemyMix(R.army, true)) do
        local bp = __blueprints[e[1]]
        local naval = false
        if bp and bp.Categories then
            for _, c in ipairs(bp.Categories) do if c == 'NAVAL' then naval = true end end
        end
        if naval and bp.Categories then
            local mobile = false
            for _, c in ipairs(bp.Categories) do if c == 'MOBILE' then mobile = true end end
            if mobile then
                table.insert(list, { e[1], e[2] })
                mass = mass + e.mass
            end
        end
    end
    return list, mass
end

-- what an air factory of `tech` builds against the ships (the best build value against the known ships, id on ties),
-- worked out again every kCounterTicks; nil when no candidate kills anything
function NavalAirPick(brain, tech)
    local R = brain.Remaster
    R.navalPick = R.navalPick or {}
    local c = R.navalPick[tech]
    if c and R.tick - c.tick < kCounterTicks then return c.id end
    local ships = NavalMix(R)
    local best, bestV
    -- (2026-11-03, Chris's big test: against his battleships the air factories made T1 bombers. Against ships above T1
    -- a T1 bomber is no answer: none is picked, the T1 air factory stays free and goes to T2 (brain.lua
    -- RemasterUpgrades) for torpedo bombers)
    local low = 1
    for _, sh in ipairs(ships) do
        local r = Remastered.AIUnitRating(sh[1])
        if r and r.tier ~= 'T1' then low = 2 end
    end
    if ships[1] then
        for t = tech, low, -1 do
            for _, sfx in ipairs(kNavalAir[t] or {}) do
                local id = brain:Bp(sfx)
                if Valid(id) then
                    local v = Remastered.AIBuildValue(id, ships)
                    if v > 0 and (not bestV or v > bestV or (v == bestV and id < best)) then best, bestV = id, v end
                end
            end
        end
        LOG(string.format('ainaval %d %d T%d pick %s value %.2f vs %d ship types, coast threat %.0f mass', R.army, R.tick,
            tech, tostring(best), bestV or 0, table.getn(ships), R.coastMass or 0))
    end
    R.navalPick[tech] = { tick = R.tick, id = best }
    return best
end

-- ---- defences ----

-- the place a defence goes in a zone: the free 2-unit build spot nearest (x, z) (spot order on ties), or nil
local function SpotNear(brain, zone, x, z, bp)
    local best, bestD
    for _, p in ipairs(Remastered.AIBuildSpots(zone, 2)) do
        local dx, dz = p[1] - x, p[2] - z
        local d = dx * dx + dz * dz
        if (not bestD or d < bestD) and d <= 40 * 40 and
            brain:CanBuildStructureAt(bp, { p[1], GetTerrainHeight(p[1], p[2]), p[2] }) and
            not brain:Claimed(p[1], p[2]) then
            best, bestD = p, d
        end
    end
    return best
end

local function DefencesNear(brain, x, z, cat)
    local list = brain:GetUnitsAroundPoint(categories.STRUCTURE * categories.DEFENSE * cat,
        { x, GetTerrainHeight(x, z), z }, 20, 'Ally')
    return list and table.getn(list) or 0
end

-- One defence job for a builder of `tech`, or nil: {bp, x, z, why}. The sites: our raided expansions (operations mark
-- R.raided), and the chokes beside our economy zones whose other side the enemy was seen in (R.enemySeen); the PD of
-- the builder's tech (T1 / T2; anti-air instead against a mostly-air enemy) only when its build value against that
-- part of the enemy's army, times K.defenceFactor, beats
-- the best mobile counter's; at most K.defenceBuilding ordered or under construction at once, and all our defences
-- at most K.defenceShare of our mobile army's mass.
-- (2026-11-03, perf: everything up to the sites is the same for every builder of a tech in one economy step - the
-- units' lists, the mix, the build values - so it is worked out once a step per tech (ctx.defPlan); each builder then
-- only looks at the sites, whose spots and claims change as builders take jobs. DefenceJob was the economy's
-- second cost in the 4v4: every builder's call summed the mass of every unit of the army.)
local function DefencePlan(brain, tech, ctx)
    local R = brain.Remaster
    local K = Knobs(brain)
    -- defences ordered but not started yet count too (a builder may still be walking there): kept kPendingTicks
    local pending = {}
    for _, d in ipairs(R.defPending or {}) do
        if R.tick - d.tick < kPendingTicks then table.insert(pending, d) end
    end
    R.defPending = pending
    if (ctx.defenceBuilding or 0) + table.getn(pending) >= K.defenceBuilding then return end
    -- the defence share: what defences we have may weigh at most K.defenceShare of our mobile army (+ one T1 PD)
    local defMass, armyMass = 0, 0
    for _, u in ipairs(brain:GetListOfUnits(categories.STRUCTURE * categories.DEFENSE, false)) do
        if not u.Dead then defMass = defMass + (u:GetBlueprint().Economy.BuildCostMass or 0) end
    end
    for _, u in ipairs(brain:GetListOfUnits(categories.MOBILE - categories.ENGINEER - categories.COMMAND, false)) do
        if not u.Dead then armyMass = armyMass + (u:GetBlueprint().Economy.BuildCostMass or 0) end
    end
    if defMass > K.defenceShare * armyMass + 250 then return end
    -- point defence against a ground-heavy enemy, anti-air against an air-heavy one
    local ground, air, gm, am = Mix(R)
    local vsAir = am > gm
    local bp = brain:Bp(vsAir and (tech >= 2 and 'b2204' or 'b2104') or (tech >= 2 and 'b2301' or 'b2101'))
    if not Valid(bp) then return end
    if (vsAir and am or gm) < kMixMass then return end
    local pdValue = Remastered.AIBuildValue(bp, vsAir and air or ground) * K.defenceFactor
    local c = Counter(brain, math.min(tech, 3))
    local mobileValue = (vsAir and c.airValue or c.groundValue) or 0
    if pdValue <= mobileValue then return end
    local cat = vsAir and categories.ANTIAIR or categories.DIRECTFIRE
    local sites = {}
    for _, z in ipairs(R.ownEconomy or {}) do
        if R.raided and R.raided[z] and R.tick - R.raided[z] <= kRaidMemory then
            local zone = Remastered.AIZone(z)
            table.insert(sites, { zone = z, x = zone.x, z = zone.z, why = 'raided expansion' })
        end
    end
    local own = {}
    own[R.startZone] = true
    for _, z in ipairs(R.ownEconomy or {}) do own[z] = true end
    for _, ch in ipairs(Remastered.AIChokepoints()) do
        local mine, other
        if own[ch.a] and not own[ch.b] then mine, other = ch.a, ch.b
        elseif own[ch.b] and not own[ch.a] then mine, other = ch.b, ch.a end
        if mine and R.enemySeen and R.enemySeen[other] and R.tick - R.enemySeen[other] <= kSeenMemory then
            table.insert(sites, { zone = mine, x = ch.x, z = ch.z, why = 'choke the enemy comes through' })
        end
    end
    return { K = K, pending = pending, bp = bp, cat = cat, sites = sites, pdValue = pdValue, mobileValue = mobileValue,
        vsAir = vsAir, c = c }
end

function DefenceJob(brain, tech, ctx)
    local R = brain.Remaster
    if ctx.defenceGiven then return end
    ctx.defPlan = ctx.defPlan or {}
    local plan = ctx.defPlan[tech]
    if plan == nil then
        plan = DefencePlan(brain, tech, ctx) or false
        ctx.defPlan[tech] = plan
    end
    if not plan then return end
    local K, pending, bp, cat, pdValue, mobileValue, vsAir, c = plan.K, plan.pending, plan.bp, plan.cat, plan.pdValue,
        plan.mobileValue, plan.vsAir, plan.c
    for _, s in ipairs(plan.sites) do
        local near = DefencesNear(brain, s.x, s.z, cat)
        for _, d in ipairs(pending) do
            if math.abs(d.x - s.x) <= 20 and math.abs(d.z - s.z) <= 20 then near = near + 1 end
        end
        if near < K.defencePerSite then
            local p = SpotNear(brain, s.zone, s.x, s.z, bp)
            if p then
                ctx.defenceGiven = true
                table.insert(R.defPending, { tick = R.tick, x = s.x, z = s.z })
                LOG(string.format('aidef %d %d %s zone %d value %.2f vs mobile %.2f (%s) | %s', R.army, R.tick, bp,
                    s.zone, pdValue, mobileValue, tostring(vsAir and c.air or c.ground), s.why))
                return { bp = bp, x = p[1], z = p[2], why = s.why }
            end
        end
    end
end

-- ---- investments (decision 14) ----

local function Economy(id)
    local bp = __blueprints[id]
    local e = bp and bp.Economy or {}
    return e.BuildCostMass or 0, e.ProductionPerSecondMass or 0
end

local function LogHold(R, key, text)
    R.holdLogged = R.holdLogged or {}
    if not R.holdLogged[key] or R.tick - R.holdLogged[key] >= kHoldLogTicks then
        R.holdLogged[key] = R.tick
        LOG(text)
    end
end

-- The extractor upgrades: each idle T1 / T2 extractor (id order) is an investment candidate.
function Investments(brain, ctx)
    local R = brain.Remaster
    local K = Knobs(brain)
    local income = brain:GetEconomyIncome('MASS') * 10
    local stored = brain:GetEconomyStored('MASS')
    -- a sustained shortage: stored mass near zero for K.stallTolerance
    if brain:GetEconomyStoredRatio('MASS') < 0.02 then
        R.shortSince = R.shortSince or R.tick
    else
        R.shortSince = nil
    end
    local sustained = R.shortSince and R.tick - R.shortSince >= K.stallTolerance
    -- (phase 8: an extractor upgrade draws a lot of energy as it builds; while energy is under kInvestEnergy stored, no
    -- new one starts. The opening set's minutes 5-10 were energy-stalled 20-45% of the time with 7-9 upgrades at once.)
    local energyLow = brain:GetEconomyStoredRatio('ENERGY') < kInvestEnergy
    -- what the upgrades under way still cost
    local committed = 0
    for _, m in ipairs(ctx.upgrading) do
        if EntityCategoryContains(categories.MASSEXTRACTION, m) then
            local to = m.UpgradingTo or (EntityCategoryContains(categories.TECH1, m) and brain:Bp('b1202') or brain:Bp('b1302'))
            local cost = Economy(to)
            committed = committed + cost * (1 - (m:GetWorkProgress() or 0))
        end
    end
    -- the income the budget counts is smoothed (about a minute), so a burst of reclaim does not pass for an economy
    R.massAvg = (R.massAvg or income) + (income - (R.massAvg or income)) * 0.02
    -- (phase 9: while the army is short of what the economy should keep, the investments get half the share)
    -- (phase 13: unless mass piles up, kInvestFloat of the bar or more: then the whole share and all that is stored -
    -- the overflow goes to extractor upgrades before T1 factories (brain.lua reads R.upgradesOpen). Against M28 the
    -- bar sat full at minutes 10-13 and 2,300-3,600 mass overflowed while it ran 9-15 upgrades at once and we 1-5;
    -- spending the overflow on upgrades raised our income 10-20% in the judge games.)
    local floating = brain:GetEconomyStoredRatio('MASS') >= kInvestFloat
    local budget = R.massAvg * kDipSeconds * K.investShare * ((R.armyShort and not floating) and 0.5 or 1) +
        (floating and 1 or 0.5) * stored - committed
    local enemyKnown = (R.enemyKnownZones or 0) > 0
    local danger = {}  -- zone -> margin, theirs mass (one estimate per zone and payback window)
    -- (phase 13: the candidates in order of payback, then distance from the start, then entity id - the cheapest and
    -- safest gain first; before, the engine's list order, and the first one over the budget stopped them all)
    local list = {}
    for _, m in ipairs(brain:GetListOfUnits(categories.MASSEXTRACTION * categories.STRUCTURE, false)) do
        if m:GetFractionComplete() >= 1 and not m.Dead and m:IsIdleState() and
            not EntityCategoryContains(categories.TECH3, m) then
            local from = m:GetUnitId()
            local to = brain:Bp(EntityCategoryContains(categories.TECH1, m) and 'b1202' or 'b1302')
            local cost, gain = Economy(to)
            local _, now = Economy(from)
            gain = gain - now
            if cost > 0 and gain > 0 then
                local p = m:GetPosition()
                local dx, dz = p[1] - R.startX, p[3] - R.startZ
                table.insert(list, { m = m, from = from, to = to, cost = cost, payback = cost / gain,
                    d = dx * dx + dz * dz, id = tonumber(m:GetEntityId()) or 0 })
            end
        end
    end
    table.sort(list, function(a, b)
        if a.payback ~= b.payback then return a.payback < b.payback end
        if a.d ~= b.d then return a.d < b.d end
        return a.id < b.id
    end)
    R.upgradesOpen = false
    R.dipGiven = nil
    for _, c in ipairs(list) do
        local m, from, to, cost = c.m, c.from, c.to, c.cost
        do
            do
                local payback = c.payback
                local p = m:GetPosition()
                local z = Remastered.AIZoneAt(p[1], p[3], 'land')
                if z <= 0 then z = R.startZone end
                local what = string.format('%s->%s', from, to)
                -- (item 1: the planned dip, kDipPayback: the first upgrade when none is under way)
                local dip = committed <= 0 and payback <= kDipPayback and not R.dipGiven and ctx.minutes >= kDipFrom and
                    cost <= kDipSeconds * income
                if energyLow then
                    LogHold(R, 'energy', string.format('aiinv %d %d hold %s zone %d payback %.0fs danger - | energy under %.0f%% stored: the upgrades wait for power',
                        R.army, R.tick, what, z, payback, 100 * kInvestEnergy))
                    return
                end
                if sustained and not dip then
                    LogHold(R, 'stall', string.format('aiinv %d %d hold %s zone %d payback %.0fs danger - | mass short for %.0f s: a sustained deficit, not a planned dip',
                        R.army, R.tick, what, z, payback, (R.tick - R.shortSince) / 10))
                    return
                end
                if cost > budget and not dip then
                    LogHold(R, 'budget', string.format('aiinv %d %d hold %s zone %d payback %.0fs danger - | costs %.0f, more than half of %ds of income can cover now (%.0f left)',
                        R.army, R.tick, what, z, payback, cost, kDipSeconds, budget))
                    return
                end
                if not enemyKnown and ctx.minutes >= 3 then
                    R.scoutFirst = true
                    LogHold(R, 'scout', string.format('aiinv %d %d hold %s zone %d payback %.0fs danger unknown | no enemy known anywhere: scout first',
                        R.army, R.tick, what, z, payback))
                    return
                end
                local key = z .. ':' .. math.floor(payback / 60)
                if not danger[key] then
                    local horizon = math.min(1500, payback * kLandSpeed)
                    local margin, _, _, _, _, theirs = Remastered.AIZoneEstimate(R.army, z, horizon)
                    danger[key] = { margin = margin, theirs = theirs }
                end
                local d = danger[key]
                if d.theirs <= 0 or d.margin >= K.investSafe then
                    R.upgradesOpen = true  -- (an upgrade can go here: piling mass waits for it, not for a factory)
                    IssueUpgrade({ m }, to)
                    local over = cost > budget or sustained
                    if dip then R.dipGiven = true end
                    budget = budget - cost
                    LOG(string.format('aiinv %d %d invest %s zone %d payback %.0fs danger %.2f enemy %.0f | %s',
                        R.army, R.tick, what, z, payback, d.margin, d.theirs, (d.theirs <= 0 and
                        'nothing of the enemy can reach it within the payback time' or
                        'we hold the zone against what can reach it within the payback time') ..
                        (over and ' (the planned dip: past the budget, nothing else upgrading)' or '')))
                else
                    LogHold(R, 'danger' .. z, string.format('aiinv %d %d hold %s zone %d payback %.0fs danger %.2f enemy %.0f | the enemy that can reach it within the payback time would win there',
                        R.army, R.tick, what, z, payback, d.margin, d.theirs))
                end
            end
        end
    end
end
