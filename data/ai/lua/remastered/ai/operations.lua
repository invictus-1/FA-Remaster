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
local kGatherTicks = 450   -- a gather commits (or is dropped) after this long at most, plus the walk (phase 6)
local kGatherSpeed = 2.5   -- world units a second: the walk to the staging zone that the gather waits for
local kGatherLeast = 0.3   -- when the time is up, at least this share of the army must be at the staging zone
local kGuardGroup = 8      -- a guard post's units when nothing of the enemy can come to it soon
local kGuardMax = 24       -- a guard post's units at most, however much of the enemy can come
local kGuardRadius = 40    -- world units around a zone's centre that count as "at its guard post"
local kPostHorizon = 300   -- world units of travel that count as "can come to this post soon"
local kLostMemory = 3000   -- ticks a zone where we lost our economy stays a post (to take back and rebuild)
local kBaseZone = 150      -- world units of travel from the start: a zone this near is the base (phase 7b)
local kRecallMass = 300    -- enemy mass that can come to a base zone that recalls the army from an operation
local kRecallTicks = 600   -- after a recall, the base's defence takes every free unit this long
local kScoutFresh = 1200  -- a target looked at by our scouts this recently needs no new look before an attack (7b)
local kScoutWait = 300    -- after sending scouts to a target, the attack waits this long for their look
local kTradeWindow = 600   -- ticks over which an attack's real trade is judged (phase 9)
local kTradeLoss = 2.0     -- an attack that lost this many times the mass it killed over kTradeWindow ...
local kTradeMin = 600      -- ... and at least this much mass, withdraws: the estimate is missing what fog hides
local kTradeMemory = 3000  -- ticks a zone we withdrew from is not picked as a target again
local kPeakFade = 0.99     -- the enemy's strength seen fades by this a think (5 s): half in about 6 minutes

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
        -- (phase 7: the overall ratio counts everything that hits the ground, air and navy too: against M28 its
        -- gunships and ships were outside the land-only ratio, which said "ahead" while they shelled our base)
        ours = ours + f.own.land.ground + f.ally.land.ground + f.own.air.ground + f.ally.air.ground +
            f.own.naval.ground + f.ally.naval.ground
        local e = f.enemy.land.ground + f.enemy.structure.ground
        theirs = theirs + e + f.enemy.air.ground + f.enemy.naval.ground
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
        local bad = R.badTarget and R.badTarget[e.zone] and R.tick - R.badTarget[e.zone] < kTradeMemory
        if d >= 0 and not bad then
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

-- our army's mass lost and enemy mass killed so far (the army stats), for the trade check
local function Trade(brain)
    local function st(n)
        local v = brain:GetArmyStat(n, 0)
        return type(v) == 'table' and v.Value or v or 0
    end
    return st('Units_MassValue_Lost'), st('Enemies_MassValue_Destroyed')
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

-- GUARD POSTS (phase 5: the map we own, not only the base; phase 6: map control). The duels showed the army away on
-- attacks while the enemy's army took our outlying extractors one by one (FAF held 60-80, we 20-30). So the posts come
-- before any operation, with a share of the army:
-- - a post is an outlying economy zone of ours, or one where we lost our economy within kLostMemory (the guards take
--   it back, and the engineers, who keep out of zones the enemy is known in, rebuild behind them);
-- - its size follows the enemy that can come to it soon (the zone estimate within kPostHorizon): units until our mass
--   there is K.postShare x theirs (at least 2 units), kGuardGroup when nothing can come, at most kGuardMax;
-- - the posts most in danger are manned first (the enemy mass that can come, then the farthest, then zone id), but
--   not a post the estimate says we cannot hold (margin under K.defend with all of ours that can come): its guards
--   would die one post at a time;
-- - all guards together weigh at most K.guardShare of the army; a post takes the free units nearest to it (entity
--   order on ties), and a guard keeps its post while the zone is still a post and the post is not over-full.
-- Returns the units that are not guards (for the operations).
local function Posts(R, K, units, ownEconomy, ratio, armyMass)
    R.hadEconomy = R.hadEconomy or {}
    R.lostAt = R.lostAt or {}
    local econ = {}
    for _, z in ipairs(ownEconomy) do
        econ[z] = true
        R.hadEconomy[z] = true
        R.lostAt[z] = nil
    end
    local info = Remastered.AIMapInfo()
    for z = 1, info.zones do
        if R.hadEconomy[z] and not econ[z] and not R.lostAt[z] then R.lostAt[z] = R.tick end
    end
    local postList = {}
    for z = 1, info.zones do
        local lost = R.lostAt[z] and R.tick - R.lostAt[z] <= kLostMemory
        if (econ[z] or lost) and z ~= R.startZone then
            local d = Remastered.AIZoneDistance(R.startZone, z, 'land')
            if d >= 0 then
                local margin, _, _, _, _, theirsMass = Remastered.AIZoneEstimate(R.army, z, kPostHorizon)
                if theirsMass <= 0 or margin >= K.defend then
                    table.insert(postList, { zone = z, d = d, threat = theirsMass, lost = lost })
                end
            end
        end
    end
    table.sort(postList, function(a, b)
        if a.threat ~= b.threat then return a.threat > b.threat end
        if a.d ~= b.d then return a.d > b.d end
        return a.zone < b.zone
    end)
    local posts, mass, count = {}, {}, {}
    for _, p in ipairs(postList) do
        posts[p.zone] = p
        mass[p.zone] = 0
        count[p.zone] = 0
    end
    local function Full(p)
        if count[p.zone] >= kGuardMax then return true end
        if p.threat <= 0 then return count[p.zone] >= kGuardGroup end
        return mass[p.zone] >= K.postShare * p.threat and count[p.zone] >= 2
    end
    R.guardOf = R.guardOf or {}
    local budget = K.guardShare * armyMass
    local guardMass = 0
    local guard, rest, fresh = {}, {}, {}
    -- the guards that keep their post (entity order), while the post is not full and the share allows
    for _, u in ipairs(units) do
        local id = u:GetEntityId()
        local z = R.guardOf[id]
        if z and posts[z] and not Full(posts[z]) and guardMass + MassOf(R, u) <= budget then
            count[z] = count[z] + 1
            mass[z] = mass[z] + MassOf(R, u)
            guardMass = guardMass + MassOf(R, u)
            guard[id] = z
        else
            R.guardOf[id] = nil
        end
    end
    -- the posts that are not full take the free units nearest to them
    for _, p in ipairs(postList) do
        if guardMass >= budget then break end
        if not Full(p) then
            local zone = Remastered.AIZone(p.zone)
            local near = {}
            for i, u in ipairs(units) do
                if not guard[u:GetEntityId()] then
                    local q = u:GetPosition()
                    local dx, dz = q[1] - zone.x, q[3] - zone.z
                    table.insert(near, { u = u, d = dx * dx + dz * dz, i = i })
                end
            end
            table.sort(near, function(x, y)
                if x.d ~= y.d then return x.d < y.d end
                return x.i < y.i
            end)
            for _, n in ipairs(near) do
                if Full(p) or guardMass + MassOf(R, n.u) > budget then break end
                local id = n.u:GetEntityId()
                guard[id] = p.zone
                R.guardOf[id] = p.zone
                fresh[id] = true
                count[p.zone] = count[p.zone] + 1
                mass[p.zone] = mass[p.zone] + MassOf(R, n.u)
                guardMass = guardMass + MassOf(R, n.u)
            end
        end
    end
    if R.tick - (R.postsLogged or -10000) >= 600 and postList[1] then
        local parts = {}
        for i = 1, math.min(6, table.getn(postList)) do
            local q = postList[i]
            table.insert(parts, string.format('%d%s:%.0f/%.0f', q.zone, q.lost and '(lost)' or '', mass[q.zone], q.threat))
        end
        Log(R, 'posts', 0, 0, ratio, string.format('%d posts, guards %.0f of %.0f mass; our mass / enemy that can come: %s',
            table.getn(postList), guardMass, armyMass, table.concat(parts, ' ')))
        R.postsLogged = R.tick
    end
    -- the guards go to their posts: a new guard at once, an idle one when it is away from its post
    for _, u in ipairs(units) do
        local id = u:GetEntityId()
        local z = guard[id]
        if z then
            if fresh[id] or u:IsIdleState() then
                local zone = Remastered.AIZone(z)
                local q = u:GetPosition()
                local dx, dz = q[1] - zone.x, q[3] - zone.z
                if dx * dx + dz * dz > kGuardRadius * kGuardRadius then
                    IssueClearCommands({ u })
                    IssueAggressiveMove({ u }, { zone.x, GetTerrainHeight(zone.x, zone.z), zone.z })
                end
            end
        else
            table.insert(rest, u)
        end
    end
    return rest
end

function Think(brain)
    local R = brain.Remaster
    if not R or R.startZone <= 0 then return end
    R.op = R.op or nil
    local units = CombatUnits(brain)
    local armyMass = 0
    for _, u in ipairs(units) do armyMass = armyMass + MassOf(R, u) end
    R.armyMass = armyMass  -- (the economy's map-control floor reads it: brain.lua)
    local ours, theirs, enemyZones, threatened, ownEconomy = Front(R)
    -- (phase 6, fog of war: what the army saw of the enemy's strength fades slowly, kPeakFade a think, so a few
    -- minutes without sight of its army are not "far ahead"; the duels with fog launched decisive pushes at a ratio
    -- of 20-50 into armies they had not seen for a while)
    R.theirsPeak = math.max(theirs, (R.theirsPeak or 0) * kPeakFade)
    theirs = R.theirsPeak
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
        -- the base first (phase 7b): an enemy force of kRecallMass or more at a base zone recalls the army from its
        -- operation, and for kRecallTicks the base's defence takes every free unit. The no-cheat duel against M28
        -- was lost with the army away on an attack while M28's army reached the base and killed the commander.
        local base = Remastered.AIZoneDistance(R.startZone, z, 'land') <= kBaseZone
        if base and theirsMass >= kRecallMass then
            if R.op then
                Log(R, 'recall', z, margin, ratio, string.format('the base is attacked (%.0f mass can come): the %s at zone %d is called off',
                    theirsMass, R.op.kind, R.op.zone))
                R.op = nil
            end
            R.recallUntil = R.tick + kRecallTicks
        end
        local all = base and R.recallUntil and R.tick < R.recallUntil
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
                if not all and mass >= 1.5 * theirsMass + 1 then break end
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
            -- (phase 7b: the base itself attacked by more than we would win against: every free unit comes home and
            -- fights beside the base's defences and the commander, instead of staying on posts and attacks)
            if all then
                local home = {}
                for _, u in ipairs(units) do
                    if not busy[u:GetEntityId()] then
                        busy[u:GetEntityId()] = true
                        table.insert(home, u)
                    end
                end
                if home[1] then IssueAggressiveMove(home, { R.startX, GetTerrainHeight(R.startX, R.startZ), R.startZ }) end
            end
        end
    end
    if not threatened[1] then R.defending = nil end
    local free = {}
    for _, u in ipairs(units) do
        if not busy[u:GetEntityId()] then table.insert(free, u) end
    end
    units = Posts(R, K, free, ownEconomy, ratio, armyMass)

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
        elseif (there > 0 and there >= kGatherShare * table.getn(units)) or R.tick - R.op.since >= (R.op.limit or kGatherTicks) then
            if there > 0 and there >= kGatherLeast * table.getn(units) and margin >= bar then
                R.op = { kind = R.op.push and 'push' or 'attack', zone = R.op.zone, stage = R.op.stage, since = R.tick,
                    logged = R.tick }
                R.op.lost0, R.op.killed0 = Trade(brain)
                R.op.tradeTick = R.tick
                Log(R, 'commit', R.op.zone, margin, ratio, string.format('gathered (%d of %d at the staging zone): %s',
                    there, table.getn(units), R.op.kind .. ' together'))
                if units[1] then IssueAggressiveMove(units, (ZonePos(R.op.zone))) end
                return
            else
                Log(R, 'dropped', R.op.zone, margin, ratio, there > 0 and
                    (margin < bar and string.format('gather: the estimate fell under %.2f', bar) or
                    string.format('gather: only %d of %d reached the staging zone', there, table.getn(units))) or
                    'gather: nobody reached the staging zone')
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
        -- the trade check (phase 9): over each kTradeWindow of the attack, the mass we lost against the mass we killed
        -- (all of the army's losses and kills in that time). Against M28 the estimate said 0.8 while the attack lost
        -- 10-20 times what it killed to artillery and missiles the army never saw; a fight the real numbers say we are
        -- losing ends whatever the estimate says. It is logged 'withdraw', not 'retreat': the enemy there is not
        -- weaker, only unseen, so the never-retreat-from-a-weaker-force rule does not apply.
        local withdraw
        if R.op.tradeTick and R.tick - R.op.tradeTick >= kTradeWindow then
            local lost, killed = Trade(brain)
            local dl, dk = lost - R.op.lost0, killed - R.op.killed0
            if dl >= kTradeMin and dl >= kTradeLoss * math.max(dk, 1) then
                withdraw = string.format('the fight trades badly: %.0f mass lost for %.0f killed in %d s (unseen enemies)',
                    dl, dk, kTradeWindow / 10)
            end
            R.op.lost0, R.op.killed0, R.op.tradeTick = lost, killed, R.tick
        end
        if withdraw then
            Log(R, 'withdraw', R.op.zone, margin, ratio, withdraw)
            R.badTarget = R.badTarget or {}
            R.badTarget[R.op.zone] = R.tick  -- (not a target again for kTradeMemory: PickTarget)
            R.op = nil
            if units[1] then IssueMove(units, { R.startX, GetTerrainHeight(R.startX, R.startZ), R.startZ }) end
            return
        elseif f.enemy.land.count + f.enemy.structure.count <= 0 then
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
        -- scouting before a push (phase 7b): under fog the estimate at the target is only what the army last saw there.
        -- A target our air scouts have not looked at within kScoutFresh gets up to 2 of them first, and the attack waits
        -- kScoutWait for their look; then the estimate decides again with what they saw. Without air scouts it goes.
        R.lookedAt = R.lookedAt or {}
        if target then
            local seen = R.lookedAt[target.zone]
            if not seen or R.tick - seen > kScoutFresh then
                local scouts = {}
                for _, u in ipairs(brain:GetListOfUnits(categories.SCOUT * categories.AIR, false)) do
                    if table.getn(scouts) < 2 and not u.Dead then table.insert(scouts, u) end
                end
                if scouts[1] then
                    R.lookedAt[target.zone] = R.tick
                    IssueClearCommands(scouts)
                    IssueMove(scouts, (ZonePos(target.zone)))
                    Log(R, 'scout', target.zone, margin, ratio, string.format('a look at the target before the %s (%d air scouts)',
                        push and 'push' or 'attack', table.getn(scouts)))
                    target = nil
                end
            elseif R.tick - seen < kScoutWait then
                target = nil  -- (the scouts are on their way: the attack waits for their look)
            end
        end
        if target then
            local what = target.structures > 0 and (target.economy > 0 and 'their economy' or 'their structures') or
                'their army in the field'
            local stage = StagingZone(R, target.zone)
            -- (phase 6: the gather waits for the walk from the start, kGatherSpeed, as well as kGatherTicks)
            local walk = math.max(0, Remastered.AIZoneDistance(R.startZone, stage, 'land'))
            R.op = { kind = 'gather', push = push, zone = target.zone, stage = stage, since = R.tick,
                limit = kGatherTicks + math.floor(10 * walk / kGatherSpeed) }
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

    -- nothing to do: the units that are neither guards nor in an operation gather at the start
    local home = {}
    for _, u in ipairs(units) do
        if u:IsIdleState() then table.insert(home, u) end
    end
    if home[1] then IssueAggressiveMove(home, { R.startX, GetTerrainHeight(R.startX, R.startZ), R.startZ }) end
end
