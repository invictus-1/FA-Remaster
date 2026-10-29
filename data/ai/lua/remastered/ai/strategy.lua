-- FA Remastered's own AI: the strategy layer (docs/ai-architecture.md 2.1; Chris's decision 15 in
-- docs/ai-design-goals.md). Part of FA Remastered, MIT licence (see LICENSE); our own design and code, nothing taken
-- from M28AI.
--
-- Every kReadTicks the brain reads the enemy from what its army knows (Remastered.AIEnemyMix with all: fair intel and
-- the memory of lost contacts, so scouting feeds it) and compares it with itself: extractors and their tiers, power,
-- the highest factory tier, the highest unit tier seen, the mobile army's mass and its air and naval shares, the
-- enemy's defences. From that:
-- - the enemy's STYLE: navy, air, tech (a tier above ours), rush (a big army early on a smaller economy), eco or
--   turtle (more economy than ours and a smaller army, or many defences), else standard;
-- - our STANCE, a set of changes to the knobs (personality.lua):
--   - catch-up when the enemy is a tier above us: the army holds (attack only on a clear win), defences count more,
--     the land factories go up a tier now (R.techRush), the economy goes full;
--   - harass or out-eco against a greedy or turtling enemy, whichever the battle estimate at its economy favours;
--   - hold against a rush; standard otherwise.
-- With nothing of the enemy known the army scouts first (R.scoutFirst) and stays standard. Each stance change logs one
-- line "aistrat <army> <tick> style <s> stance <st> | <evidence>", and every read logs a recheck line at least every
-- kRecheckLogTicks. All enemies count together for now (one read for "the enemy").

local kReadTicks = 600          -- the read is made again this often
local kRecheckLogTicks = 1800   -- an unchanged read is logged at least this often
local kMexWeight = { 1, 3, 9 }  -- extractors by tier, roughly their mass income relative to a T1 one

local function Cats(id)
    local bp = __blueprints[id]
    local set = {}
    if bp and bp.Categories then
        for _, c in ipairs(bp.Categories) do set[c] = true end
    end
    return set, bp
end

local function TierOf(c)
    if c.EXPERIMENTAL then return 4 end
    if c.TECH3 then return 3 end
    if c.TECH2 then return 2 end
    return 1
end

-- a side's read from a list of {id, count}: extractors by tier, power count, highest factory / unit / extractor
-- tier, mobile army mass (engineers and commanders left out) and its air / naval mass, defence mass
local function Read(list)
    local r = { mex = { 0, 0, 0 }, mexWeight = 0, power = 0, factoryTier = 0, unitTier = 0, mexTier = 0, army = 0,
        air = 0, naval = 0, defence = 0 }
    for _, e in ipairs(list) do
        local c, bp = Cats(e[1])
        local n = e[2]
        local mass = (bp and bp.Economy and bp.Economy.BuildCostMass or 0) * n
        local t = TierOf(c)
        if c.MASSEXTRACTION and c.STRUCTURE then
            local k = math.min(t, 3)
            r.mex[k] = r.mex[k] + n
            r.mexWeight = r.mexWeight + kMexWeight[k] * n
            r.mexTier = math.max(r.mexTier, k)
        elseif c.ENERGYPRODUCTION and c.STRUCTURE then
            r.power = r.power + n
        elseif c.FACTORY and c.STRUCTURE then
            r.factoryTier = math.max(r.factoryTier, t)
        elseif c.DEFENSE and c.STRUCTURE then
            r.defence = r.defence + mass
        elseif c.MOBILE and not c.ENGINEER and not c.COMMAND then
            r.army = r.army + mass
            r.unitTier = math.max(r.unitTier, t)
            if c.AIR then r.air = r.air + mass end
            if c.NAVAL then r.naval = r.naval + mass end
        end
    end
    r.tier = math.max(r.factoryTier, r.unitTier, r.mexTier)
    return r
end

-- our own side in the same form (finished units, entity id order)
local function OwnList(brain)
    local count, order = {}, {}
    for _, u in ipairs(brain:GetListOfUnits(categories.ALLUNITS, false)) do
        if not u.Dead and u:GetFractionComplete() >= 1 then
            local id = u:GetUnitId()
            if not count[id] then
                count[id] = 0
                table.insert(order, id)
            end
            count[id] = count[id] + 1
        end
    end
    local list = {}
    for _, id in ipairs(order) do table.insert(list, { id, count[id] }) end
    return list
end

-- the best margin of an attack on one of the enemy's economy zones (the nearest kProbe by travel)
local function BestEconomyMargin(R)
    local list = {}
    local info = Remastered.AIMapInfo()
    for z = 1, info.zones do
        if R.enemyAt and R.enemyAt[z] then
            local f = Remastered.AIZoneForces(R.army, z)
            local d = Remastered.AIZoneDistance(R.startZone, z, 'land')
            if f.enemy.economyMass > 0 and d >= 0 then table.insert(list, { z = z, d = d }) end
        end
    end
    table.sort(list, function(a, b)
        if a.d ~= b.d then return a.d < b.d end
        return a.z < b.z
    end)
    local best = 0
    for i = 1, math.min(4, table.getn(list)) do
        best = math.max(best, (Remastered.AIZoneEstimate(R.army, list[i].z, 400)))
    end
    return best
end

function Think(brain)
    local R = brain.Remaster
    if not R or R.startZone <= 0 then return end
    if R.lastRead and R.tick - R.lastRead < kReadTicks then return end
    R.lastRead = R.tick
    local theirs = Read(Remastered.AIEnemyMix(R.army, true))
    local ours = Read(OwnList(brain))
    local minutes = R.tick / 600
    local known = theirs.army + theirs.mexWeight + theirs.power + theirs.factoryTier > 0
    local style, stance = 'unknown', 'standard'
    if not known then
        R.scoutFirst = true
    else
        local mobile = math.max(theirs.army, 1)
        if theirs.naval / mobile > 0.4 then style = 'navy'
        elseif theirs.air / mobile > 0.5 then style = 'air'
        elseif theirs.tier > ours.tier then style = 'tech'
        elseif minutes < 12 and theirs.army > 1.5 * ours.army and theirs.mexWeight <= ours.mexWeight then style = 'rush'
        elseif (theirs.mexWeight > 1.2 * ours.mexWeight and theirs.army <= ours.army) or
            theirs.defence > 0.3 * mobile then style = 'eco'
        else style = 'standard' end
        if theirs.tier > ours.tier then
            stance = 'catch-up'
        elseif style == 'eco' then
            local K = import("/lua/remastered/ai/personality.lua").Knobs(brain)
            stance = BestEconomyMargin(R) >= K.launch and 'harass' or 'out-eco'
        elseif style == 'rush' then
            stance = 'hold'
        end
    end
    R.techRush = stance == 'catch-up' or nil
    local evidence = string.format('mex ours %d/%d/%d theirs %d/%d/%d; tier ours %d theirs %d; power ours %d theirs %d;' ..
        ' army mass ours %.0f theirs %.0f; their air %.0f navy %.0f defences %.0f',
        ours.mex[1], ours.mex[2], ours.mex[3], theirs.mex[1], theirs.mex[2], theirs.mex[3], ours.tier, theirs.tier,
        ours.power, theirs.power, ours.army, theirs.army, theirs.air, theirs.naval, theirs.defence)
    if stance ~= (R.stance or 'standard') or style ~= R.style then
        LOG(string.format('aistrat %d %d style %s stance %s (was %s) | %s', R.army, R.tick, style, stance,
            R.stance or 'standard', evidence))
        R.stance, R.style, R.stratLogged = stance, style, R.tick
    elseif R.tick - (R.stratLogged or 0) >= kRecheckLogTicks then
        LOG(string.format('aistrat %d %d recheck style %s stance %s | %s', R.army, R.tick, style, stance, evidence))
        R.stratLogged = R.tick
    end
end
