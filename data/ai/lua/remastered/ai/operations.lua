-- FA Remastered's own AI: operations (docs/ai-architecture.md 2.1-2.2, phase 4). Part of FA Remastered, MIT licence
-- (see LICENSE); our own design and code, nothing taken from M28AI.
--
-- Every 5 seconds the brain looks at the whole front from what its army knows (the index: fair intel and the memory of
-- lost contacts) and decides, by the battle estimate of each fight (Remastered.AIZoneEstimate, ours = 1):
-- - DEFEND an own zone (one with our structures) that enemies are in, when the estimate gives us at least kDefend;
-- - ATTACK the most valuable enemy zone in reach when we are ahead overall (our strength / theirs >= kAttackRatio)
--   and the estimate there gives us kLaunch or more; a PUSH when far ahead (>= kPushRatio) launches at a lower bar;
-- - RAID an enemy zone with economy and no known defenders with a few light units;
-- - stay COMMITTED (decision 5): an attack goes on while its margin stays above kCommit (a push: kCommitPush), so it
--   does not turn at the first bad second; it RETREATS only below that, and never from a force the estimate says is
--   clearly weaker (a margin of kWeaker or more is never a reason to retreat).
-- Each decision prints one line, "aiop <army> <tick> <what> zone <z> margin <m> ratio <r> | <reason>", so tests can
-- score how it fights (tools: no "retreat" line with a margin of kWeaker or more).

local kDefend = 0.35       -- defend when the estimate gives us at least this
local kLaunch = 0.6        -- launch an attack at this margin or more
local kLaunchPush = 0.5    -- ... a push when far ahead
local kCommit = 0.4        -- an attack stays committed while its margin is at least this
local kCommitPush = 0.3    -- ... a push
local kWeaker = 0.6        -- a margin this high means the enemy there is clearly weaker: never a retreat
local kHorizon = 400       -- world units of travel that count as "can come to this fight"

-- Personalities (docs/ai-architecture.md 2.7, phase 5): how far ahead overall the army must be to attack and to push.
-- 'random' is drawn once from the army's own stream. Teacher mode (2.8): never a push against a human enemy; when far
-- ahead of one, it attacks at most and says so in the log.
local kPersonalities = {
    balanced = { attackRatio = 1.2, pushRatio = 2.0 },
    rusher = { attackRatio = 0.9, pushRatio = 1.6 },
    turtle = { attackRatio = 1.6, pushRatio = 3.0 },
}
local kPersonalityNames = { 'balanced', 'rusher', 'turtle' }

local function Personality(brain, R)
    if not R.personality then
        local name = brain.RemasterOptions and brain.RemasterOptions.personality or 'random'
        if not kPersonalities[name] then
            name = kPersonalityNames[Remastered.AIRandom(R.army, table.getn(kPersonalityNames))]
        end
        R.personality = name
        LOG(string.format('aiop %d %d personality %s teacher %s', R.army, R.tick, name,
            tostring(brain.RemasterOptions and brain.RemasterOptions.teacher)))
    end
    return kPersonalities[R.personality]
end

local function HumanEnemy(army)
    for i, b in ipairs(ArmyBrains) do
        if i ~= army and IsEnemy(army, i) and b.BrainType == 'Human' then return true end
    end
    return false
end

local function Log(R, what, zone, margin, ratio, reason)
    LOG(string.format('aiop %d %d %s zone %d margin %.2f ratio %.2f | %s', R.army, R.tick, what, zone, margin, ratio,
        reason))
end

local function CombatUnits(brain)
    local list = {}
    for _, u in ipairs(brain:GetListOfUnits(categories.MOBILE * categories.LAND - categories.ENGINEER - categories.COMMAND, false)) do
        if not u.Dead and u:GetFractionComplete() >= 1 then table.insert(list, u) end
    end
    return list
end

local function MassOf(R, u)
    R.massOf = R.massOf or {}
    local id = u:GetUnitId()
    if not R.massOf[id] then
        local r = Remastered.AIUnitRating(id)
        R.massOf[id] = r and r.mass or 0
    end
    return R.massOf[id]
end

local function ZonePos(z)
    local zone = Remastered.AIZone(z)
    return { zone.x, GetTerrainHeight(zone.x, zone.z), zone.z }, zone
end

-- the whole front: our strength and theirs (summed ratings against ground of land units and structures that hit it),
-- the zones the enemy is known in, and our zones with structures that enemies are in
local function Front(R)
    local info = Remastered.AIMapInfo()
    local ours, theirs = 0, 0
    local enemyZones, threatened = {}, {}
    for z = 1, info.zones do
        local f = Remastered.AIZoneForces(R.army, z)
        ours = ours + f.own.land.ground + f.ally.land.ground
        local e = f.enemy.land.ground + f.enemy.structure.ground
        theirs = theirs + e
        local present = f.enemy.land.count + f.enemy.structure.count
        if present > 0 then
            table.insert(enemyZones, { zone = z, strength = e, economy = f.enemy.economyMass,
                structures = f.enemy.structure.count })
            if f.own.structure.count > 0 then table.insert(threatened, z) end
        end
    end
    return ours, theirs, enemyZones, threatened
end

-- the attack target: by value (economy + structures, worth more near) among zones our land units can walk to
local function PickTarget(R, enemyZones)
    local best, bestScore
    for _, e in ipairs(enemyZones) do
        local d = Remastered.AIZoneDistance(R.startZone, e.zone, 'land')
        if d >= 0 then
            local score = (e.economy + 100 * e.structures + 10) / (d + 100)
            if not bestScore or score > bestScore or (score == bestScore and e.zone < best.zone) then
                best, bestScore = e, score
            end
        end
    end
    return best
end

function Think(brain)
    local R = brain.Remaster
    if not R or R.startZone <= 0 then return end
    R.op = R.op or nil
    local units = CombatUnits(brain)
    local ours, theirs, enemyZones, threatened = Front(R)
    local ratio = ours / math.max(theirs, 1)
    local P = Personality(brain, R)
    if math.mod(R.tick, 600) < 50 then  -- (a status line a minute: tests score "pushes when ahead" from these)
        Log(R, 'status', R.op and R.op.zone or 0, 0, ratio, R.op and R.op.kind or 'none')
    end
    local kAttackRatio, kPushRatio = P.attackRatio, P.pushRatio
    if brain.RemasterOptions and brain.RemasterOptions.teacher and HumanEnemy(R.army) then
        kPushRatio = 1e9  -- teacher mode: no decisive push against a human
        if ratio >= P.pushRatio and R.tick - (R.lastTeach or 0) >= 600 then
            Log(R, 'teacher', 0, 0, ratio, 'far ahead of a human player: easing off (no push)')
            R.lastTeach = R.tick
        end
    end

    -- DEFEND: our zones with enemies in them, nearest the start first (zone order on ties). Only as many units as
    -- the fight needs go (the nearest, until 1.5x the enemy mass that can come there); the rest stay free for the
    -- attack, so a raid on an outlying extractor does not stop the army's push.
    table.sort(threatened, function(a, b)
        local da = Remastered.AIZoneDistance(R.startZone, a, 'land')
        local db = Remastered.AIZoneDistance(R.startZone, b, 'land')
        if da ~= db then return da < db end
        return a < b
    end)
    local busy = {}  -- units given to the defence this think, by entity id
    for _, z in ipairs(threatened) do
        local margin, _, _, _, _, theirsMass = Remastered.AIZoneEstimate(R.army, z, kHorizon)
        if margin >= kDefend then
            if not (R.defending == z) then
                Log(R, 'defend', z, margin, ratio, 'enemies in our zone; the estimate lets us hold it')
                R.defending = z
            end
            local pos, zone = ZonePos(z)
            local near = {}
            for i, u in ipairs(units) do
                local p = u:GetPosition()
                local dx, dz = p[1] - zone.x, p[3] - zone.z
                table.insert(near, { u = u, d = dx * dx + dz * dz, i = i })
            end
            table.sort(near, function(x, y)
                if x.d ~= y.d then return x.d < y.d end
                return x.i < y.i
            end)
            local sent, mass = {}, 0
            for _, n in ipairs(near) do
                if mass >= 1.5 * theirsMass + 1 then break end
                table.insert(sent, n.u)
                busy[n.u:GetEntityId()] = true
                mass = mass + MassOf(R, n.u)
            end
            if sent[1] then IssueAggressiveMove(sent, pos) end
            break
        else
            if not R.lastAvoid or R.lastAvoid ~= z or R.tick - (R.lastAvoidTick or 0) >= 300 then
                Log(R, 'hold-back', z, margin, ratio, 'enemies in our zone are clearly stronger; the army waits at home')
                R.lastAvoid, R.lastAvoidTick = z, R.tick
            end
        end
    end
    if not threatened[1] then R.defending = nil end
    local free = {}
    for _, u in ipairs(units) do
        if not busy[u:GetEntityId()] then table.insert(free, u) end
    end
    units = free

    -- an ATTACK or PUSH in progress: stay committed while its margin holds
    if R.op and (R.op.kind == 'attack' or R.op.kind == 'push') then
        local margin = Remastered.AIZoneEstimate(R.army, R.op.zone, kHorizon)
        local f = Remastered.AIZoneForces(R.army, R.op.zone)
        local commit = R.op.kind == 'push' and kCommitPush or kCommit
        if f.enemy.land.count + f.enemy.structure.count <= 0 then
            Log(R, 'taken', R.op.zone, margin, ratio, 'no enemy known there any more')
            R.op = nil
        elseif margin < commit then
            -- a retreat is only ever a lost fight: never from a clearly weaker force
            if margin >= kWeaker then
                Log(R, 'committed', R.op.zone, margin, ratio, 'BUG GUARD: would retreat from a weaker force; stays')
            else
                Log(R, 'retreat', R.op.zone, margin, ratio, string.format('margin under the commit bar %.2f', commit))
                R.op = nil
                if units[1] then IssueMove(units, { R.startX, GetTerrainHeight(R.startX, R.startZ), R.startZ }) end
                return
            end
        else
            if R.tick - (R.op.logged or 0) >= 300 then
                Log(R, 'committed', R.op.zone, margin, ratio, R.op.kind .. ' goes on')
                R.op.logged = R.tick
            end
            if units[1] then IssueAggressiveMove(units, (ZonePos(R.op.zone))) end
            return
        end
    end

    -- a new ATTACK / PUSH when ahead, by the estimate at the target
    if not R.op and ratio >= kAttackRatio and units[1] then
        local target = PickTarget(R, enemyZones)
        if target then
            local margin = Remastered.AIZoneEstimate(R.army, target.zone, kHorizon)
            local push = ratio >= kPushRatio
            if margin >= (push and kLaunchPush or kLaunch) then
                R.op = { kind = push and 'push' or 'attack', zone = target.zone, since = R.tick, logged = R.tick }
                Log(R, R.op.kind, target.zone, margin, ratio,
                    push and 'far ahead overall: a decisive push' or 'ahead overall and the estimate favours us')
                IssueAggressiveMove(units, (ZonePos(target.zone)))
                return
            elseif R.tick - (R.lastWait or 0) >= 300 then
                Log(R, 'wait', target.zone, margin, ratio, 'ahead overall, but not enough there yet: gathering')
                R.lastWait = R.tick
            end
        end
    end

    -- RAID: an enemy zone with economy and no known defenders, with up to 6 of our lightest combat units
    if not R.op and units[1] then
        for _, e in ipairs(enemyZones) do
            if e.economy > 0 and e.strength <= 0 and Remastered.AIZoneDistance(R.startZone, e.zone, 'land') >= 0 then
                local margin = Remastered.AIZoneEstimate(R.army, e.zone, kHorizon)
                if margin >= 0.8 then
                    local light = {}
                    for _, u in ipairs(units) do
                        if table.getn(light) < 6 and u:IsIdleState() then table.insert(light, u) end
                    end
                    if light[1] then
                        Log(R, 'raid', e.zone, margin, ratio, 'enemy economy without known defenders')
                        IssueAggressiveMove(light, (ZonePos(e.zone)))
                    end
                    break
                end
            end
        end
    end

    -- nothing to do: idle units gather at the start
    local idle = {}
    for _, u in ipairs(units) do if u:IsIdleState() then table.insert(idle, u) end end
    if idle[1] then IssueAggressiveMove(idle, { R.startX, GetTerrainHeight(R.startX, R.startZ), R.startZ }) end
end
