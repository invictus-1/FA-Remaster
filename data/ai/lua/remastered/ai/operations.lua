-- FA Remastered's own AI: operations (docs/ai-architecture.md 2.1-2.2, phase 4). Part of FA Remastered, MIT licence
-- (see LICENSE); our own design and code, nothing taken from M28AI.
--
-- Every 5 seconds the brain looks at the whole front from what its army knows (the index: fair intel and the memory of
-- lost contacts) and decides, by the battle estimate of each fight (Remastered.AIZoneEstimate, ours = 1):
-- (The margins and ratios below, K.defend, K.launch and so on, are the knobs of personality.lua: the personality,
-- the strategy layer's stance and later the player's sliders lean them; the rules stay these.)
-- - DEFEND an own zone (one with our structures) that enemies are in, when the estimate gives us at least K.defend;
-- - ATTACK the most valuable enemy zone in reach when we are ahead overall (our strength / theirs >= K.attackRatio)
--   and the estimate there gives us K.launch or more; a PUSH when far ahead (>= K.pushRatio) launches at K.launchPush.
--   The value counts the enemy's field army there as well as its economy and structures (phase 5: armies and
--   expansions, not only bases), so a won fight against an army in the open is picked like a base;
-- - GATHER first (phase 5): the army meets at a staging zone beside the target (the target's neighbour nearest our
--   start with no known enemy in it) and commits together once most of it is there (kGatherShare within
--   kGatherRadius) or kGatherTicks have passed, if the estimate still lets it;
-- - RAID an enemy zone with economy and no known defenders with a few light units;
-- - stay COMMITTED (decision 5): an attack goes on while its margin stays above K.commit (a push: K.commitPush), so it
--   does not turn at the first bad second; it RETREATS only below that, and never from a force the estimate says is
--   clearly weaker (a margin of kWeaker or more is never a reason to retreat).
-- Each decision prints one line, "aiop <army> <tick> <what> zone <z> margin <m> ratio <r> | <reason>", so tests can
-- score how it fights (tools: no "retreat" line with a margin of kWeaker or more).

local kWeaker = 0.6        -- a margin this high means the enemy there is clearly weaker: never a retreat
local kHorizon = 400       -- world units of travel that count as "can come to this fight"
local kCandidates = 6      -- enemy zones looked at by the estimate for a new attack (the best by value and distance)
local kGatherShare = 0.7   -- the share of the army at the staging zone that lets it commit
local kGatherRadius = 45   -- world units around the staging zone's centre that count as "there"
local kGatherTicks = 450   -- a gather commits (or is dropped) after this long at most
local kGuardGroup = 8      -- idle units hold our outlying economy zones, at most this many per zone
local kGuardRadius = 40    -- world units around a zone's centre that count as "at its guard post"

-- Personalities (docs/ai-architecture.md 2.7) and their knobs: personality.lua. Teacher mode (2.8): never a push
-- against a human enemy; when far ahead of one, it attacks at most and says so in the log.
local function Knobs(brain)
    return import("/lua/remastered/ai/personality.lua").Knobs(brain)
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
    local enemyZones, threatened, ownEconomy = {}, {}, {}
    R.enemyAt = {}  -- (the zones the enemy is known in: the engineers keep out of them)
    R.enemySeen = R.enemySeen or {}  -- zone -> the last tick the enemy was known there (defence sites)
    for z = 1, info.zones do
        local f = Remastered.AIZoneForces(R.army, z)
        ours = ours + f.own.land.ground + f.ally.land.ground
        local e = f.enemy.land.ground + f.enemy.structure.ground
        theirs = theirs + e
        if z ~= R.startZone and f.own.economyMass > 0 then table.insert(ownEconomy, z) end
        local present = f.enemy.land.count + f.enemy.structure.count
        if present > 0 then
            R.enemyAt[z] = true
            R.enemySeen[z] = R.tick
            table.insert(enemyZones, { zone = z, strength = e, economy = f.enemy.economyMass,
                structures = f.enemy.structure.count, army = f.enemy.land.mass })
            if f.own.structure.count > 0 then table.insert(threatened, z) end
        end
    end
    R.enemyKnownZones = table.getn(enemyZones)
    R.ownEconomy = ownEconomy
    return ours, theirs, enemyZones, threatened, ownEconomy
end

-- the attack target (phase 5): the enemy zones our land units can walk to, by value (its economy, its structures and
-- its field army: what a won fight there destroys), worth more near; the best kCandidates by that are estimated and
-- the best one whose margin reaches `bar` is the target (zone id on ties). Returns the target and its margin, or
-- nil, the best candidate's margin and its zone when none reaches the bar.
local function PickTarget(R, enemyZones, bar)
    local list = {}
    for _, e in ipairs(enemyZones) do
        local d = Remastered.AIZoneDistance(R.startZone, e.zone, 'land')
        if d >= 0 then
            table.insert(list, { e = e, score = (e.economy + 100 * e.structures + e.army + 10) / (d + 100) })
        end
    end
    table.sort(list, function(a, b)
        if a.score ~= b.score then return a.score > b.score end
        return a.e.zone < b.e.zone
    end)
    local bestMargin, bestZone
    for i = 1, math.min(kCandidates, table.getn(list)) do
        local e = list[i].e
        local margin = Remastered.AIZoneEstimate(R.army, e.zone, kHorizon)
        if margin >= bar then return e, margin end
        if not bestMargin or margin > bestMargin then bestMargin, bestZone = margin, e.zone end
    end
    return nil, bestMargin or 0, bestZone or 0
end

-- the staging zone for a target: its neighbour nearest our start by land with no enemy known in it (id on ties);
-- our start zone when there is none
local function StagingZone(R, target)
    local best, bestD
    for _, n in ipairs(Remastered.AIZoneNeighbours(target)) do
        local d = Remastered.AIZoneDistance(R.startZone, n.zone, 'land')
        if d >= 0 then
            local f = Remastered.AIZoneForces(R.army, n.zone)
            if f.enemy.land.count + f.enemy.structure.count <= 0 and
                (not bestD or d < bestD or (d == bestD and n.zone < best)) then
                best, bestD = n.zone, d
            end
        end
    end
    return best or R.startZone
end

local function Near(units, zone, radius)
    local k = 0
    for _, u in ipairs(units) do
        local p = u:GetPosition()
        local dx, dz = p[1] - zone.x, p[3] - zone.z
        if dx * dx + dz * dz <= radius * radius then k = k + 1 end
    end
    return k
end

function Think(brain)
    local R = brain.Remaster
    if not R or R.startZone <= 0 then return end
    R.op = R.op or nil
    local units = CombatUnits(brain)
    local ours, theirs, enemyZones, threatened, ownEconomy = Front(R)
    local ratio = ours / math.max(theirs, 1)
    local K = Knobs(brain)
    if math.mod(R.tick, 600) < 50 then  -- (a status line a minute: tests score "pushes when ahead" from these)
        local scouts = table.getn(brain:GetListOfUnits(categories.SCOUT * categories.AIR, false))
        Log(R, 'status', R.op and R.op.zone or 0, 0, ratio, string.format('%s; enemy known in %d zones; %d air scouts',
            R.op and R.op.kind or 'none', table.getn(enemyZones), scouts))
    end
    local kAttackRatio, kPushRatio = K.attackRatio, K.pushRatio
    if brain.RemasterOptions and brain.RemasterOptions.teacher and HumanEnemy(R.army) then
        kPushRatio = 1e9  -- teacher mode: no decisive push against a human
        if ratio >= K.pushRatio and R.tick - (R.lastTeach or 0) >= 600 then
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
    R.raided = R.raided or {}
    for _, z in ipairs(threatened) do
        if z ~= R.startZone then R.raided[z] = R.tick end
        local margin, _, _, _, _, theirsMass = Remastered.AIZoneEstimate(R.army, z, kHorizon)
        if margin >= K.defend then
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

    -- a GATHER in progress: the army goes to the staging zone; once most of it is there (or the time is up) it
    -- commits if the estimate still lets it, else the gather is dropped
    if R.op and R.op.kind == 'gather' then
        local margin = Remastered.AIZoneEstimate(R.army, R.op.zone, kHorizon)
        local f = Remastered.AIZoneForces(R.army, R.op.zone)
        local stage = Remastered.AIZone(R.op.stage)
        local there = Near(units, stage, kGatherRadius)
        local bar = R.op.push and K.commitPush or K.commit
        if f.enemy.land.count + f.enemy.structure.count <= 0 then
            Log(R, 'dropped', R.op.zone, margin, ratio, 'gather: no enemy known at the target any more')
            R.op = nil
        elseif (there > 0 and there >= kGatherShare * table.getn(units)) or R.tick - R.op.since >= kGatherTicks then
            if there > 0 and margin >= bar then
                R.op = { kind = R.op.push and 'push' or 'attack', zone = R.op.zone, stage = R.op.stage, since = R.tick,
                    logged = R.tick }
                Log(R, 'commit', R.op.zone, margin, ratio, string.format('gathered (%d of %d at the staging zone): %s',
                    there, table.getn(units), R.op.kind .. ' together'))
                if units[1] then IssueAggressiveMove(units, (ZonePos(R.op.zone))) end
                return
            else
                Log(R, 'dropped', R.op.zone, margin, ratio, there > 0 and
                    string.format('gather: the estimate fell under %.2f', bar) or 'gather: nobody reached the staging zone')
                R.op = nil
            end
        else
            if units[1] then IssueMove(units, (ZonePos(R.op.stage))) end
            return
        end
    end

    -- an ATTACK or PUSH in progress: stay committed while its margin holds
    if R.op and (R.op.kind == 'attack' or R.op.kind == 'push') then
        local margin = Remastered.AIZoneEstimate(R.army, R.op.zone, kHorizon)
        local f = Remastered.AIZoneForces(R.army, R.op.zone)
        local commit = R.op.kind == 'push' and K.commitPush or K.commit
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
            -- reinforcements (phase 5): a unit farther from the target than the staging zone is (plus kGatherRadius)
            -- goes there first, so new units join the army instead of reaching the fight one by one
            local tpos, tzone = ZonePos(R.op.zone)
            local stage = R.op.stage and Remastered.AIZone(R.op.stage)
            local fight, join = {}, {}
            for _, u in ipairs(units) do
                local p = u:GetPosition()
                local dx, dz = p[1] - tzone.x, p[3] - tzone.z
                local d = math.sqrt(dx * dx + dz * dz)
                local sd = stage and math.sqrt((stage.x - tzone.x) * (stage.x - tzone.x) + (stage.z - tzone.z) * (stage.z - tzone.z))
                if stage and d > sd + kGatherRadius then table.insert(join, u) else table.insert(fight, u) end
            end
            if fight[1] then IssueAggressiveMove(fight, tpos) end
            if join[1] then IssueMove(join, (ZonePos(R.op.stage))) end
            return
        end
    end

    -- a new ATTACK / PUSH when ahead, by the estimate at the target
    if not R.op and ratio >= kAttackRatio and units[1] then
        local push = ratio >= kPushRatio
        local target, margin, bestZone = PickTarget(R, enemyZones, push and K.launchPush or K.launch)
        if target then
            local what = target.structures > 0 and (target.economy > 0 and 'their economy' or 'their structures') or
                'their army in the field'
            local stage = StagingZone(R, target.zone)
            R.op = { kind = 'gather', push = push, zone = target.zone, stage = stage, since = R.tick }
            Log(R, push and 'push' or 'attack', target.zone, margin, ratio, string.format('%s; target: %s; gathering at zone %d',
                push and 'far ahead overall: a decisive push' or 'ahead overall and the estimate favours us', what, stage))
            IssueMove(units, (ZonePos(stage)))
            return
        elseif bestZone and bestZone > 0 and R.tick - (R.lastWait or 0) >= 300 then
            Log(R, 'wait', bestZone, margin, ratio, 'ahead overall, but no target the estimate favours yet')
            R.lastWait = R.tick
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

    -- nothing to do (phase 5): idle units hold guard posts at our outlying economy zones (the map we own, not only
    -- the base), the farthest from the start first (zone id on ties), at most kGuardGroup a zone; a unit keeps its
    -- post while the zone still has our economy; the rest gather at the start
    table.sort(ownEconomy, function(a, b)
        local da = Remastered.AIZoneDistance(R.startZone, a, 'land')
        local db = Remastered.AIZoneDistance(R.startZone, b, 'land')
        if da ~= db then return da > db end
        return a < b
    end)
    local posts, count = {}, {}
    for _, z in ipairs(ownEconomy) do
        if Remastered.AIZoneDistance(R.startZone, z, 'land') >= 0 then
            posts[z] = true
            count[z] = 0
        end
    end
    R.guardOf = R.guardOf or {}
    for _, u in ipairs(units) do
        local z = R.guardOf[u:GetEntityId()]
        if z and posts[z] then count[z] = count[z] + 1 end
    end
    local home = {}
    for _, u in ipairs(units) do
        if u:IsIdleState() then
            local id = u:GetEntityId()
            local z = R.guardOf[id]
            if not (z and posts[z]) then
                z = nil
                for _, c in ipairs(ownEconomy) do
                    if posts[c] and count[c] < kGuardGroup then z = c break end
                end
                R.guardOf[id] = z
                if z then count[z] = count[z] + 1 end
            end
            if z then
                local pos, zone = ZonePos(z)
                local p = u:GetPosition()
                local dx, dz = p[1] - zone.x, p[3] - zone.z
                if dx * dx + dz * dz > kGuardRadius * kGuardRadius then IssueAggressiveMove({ u }, pos) end
            else
                table.insert(home, u)
            end
        end
    end
    if home[1] then IssueAggressiveMove(home, { R.startX, GetTerrainHeight(R.startX, R.startZ), R.startZ }) end
end
