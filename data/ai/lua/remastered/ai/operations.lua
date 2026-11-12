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
-- (2026-11-07, step 4: hold back, then come back out) the recall counts the enemy mass IN the base zone, not what can
-- come there; the defence sends 1.5 x the enemy in the zone + this share of the rest that can come
local kDefendCome = 0.25
-- (2026-11-07, step 4 second form) a new attack needs the free army's mass >= kEdge x the enemy land army known within
-- kNearTravel world units of land travel from our start (not only the fog-faded overall ratio)
local kEdge = 2.5
local kNearTravel = 600
local kScoutFresh = 1200  -- a target looked at by our scouts this recently needs no new look before an attack (7b)
local kScoutWait = 300    -- after sending scouts to a target, the attack waits this long for their look
local kTradeWindow = 600   -- ticks over which an attack's real trade is judged (phase 9)
local kTradeLoss = 2.0     -- an attack that lost this many times the mass it killed over kTradeWindow ...
local kTradeMin = 600      -- ... and at least this much mass, withdraws: the estimate is missing what fog hides
local kTradeMemory = 3000  -- ticks a zone we withdrew from is not picked as a target again
local kShellRange = 40     -- world units: an enemy land force reaching this far is artillery-like (phase 10)
local kShellSlack = 60     -- ... and shells a neighbouring zone of ours whose centre is within its range + this
local kDefenceSoak = true  -- (phase 12) the defence's and the posts' estimate counts our structures' health (soak);
                           -- attacks, pushes, raids and their commits read what fights only (unit_index.cpp)
local kPeakFade = 0.99     -- the enemy's strength seen fades by this a think (5 s): half in about 6 minutes
-- (2026-11-03, Chris's big test: 4v4 on SETONS_R, all our AI; 89 gathers dropped, "only 23 of 743 reached the staging
-- zone", the armies piled up and sat; see Order, StagingZone and the gather below)
local kGatherPerUnit = 3   -- world units of the gather's radius more per square root of the army's units
local kStageBack = 120     -- world units of land travel the staging zone keeps from the target (our side of it)
local kStageDetour = 1.25  -- ... on the way: from -> staging zone -> target at most this x the direct way (+ 40)
local kGatherExtend = 2    -- a gather still arriving may wait up to this x its time limit
local kAllyReach = 500     -- world units of land travel from our start: an ally's zone this near is defended (2026-11-03)
-- (2026-11-04, the estimate's calibration) attacks, pushes, gathers and raids read the zone estimate with the enemy
-- standing its ground (AIZoneEstimate's 6th argument): on the fight bench a defender that holds fares worse than one
-- that comes on, and the meeting fight's estimate read our attacks 0.17-0.22 too low. The defence and the posts keep
-- the meeting fight (the enemy comes to us).

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
    local ownStruct = {}
    R.enemyAt = {}  -- (the zones the enemy is known in: the engineers keep out of them)
    R.allyThreat = {}  -- (2026-11-03: zones with an ally's structures and none of ours that enemies are in, zone order)
    R.enemySeen = R.enemySeen or {}  -- zone -> the last tick the enemy was known there (defence sites)
    local naval = 0  -- (the enemy's ships known anywhere, below; summed in this loop since 2026-11-03, perf)
    -- (2026-11-03, perf: the numbers of every zone in one call, Remastered.AIZoneSummary - the same as AIZoneForces')
    local S = Remastered.AIZoneSummary(R.army)
    for z = 1, info.zones do
        naval = naval + S.enemyNavalMass[z]
        -- (phase 7: the overall ratio counts everything that hits the ground, air and navy too: against M28 its
        -- gunships and ships were outside the land-only ratio, which said "ahead" while they shelled our base)
        ours = ours + S.ownLandGround[z] + S.allyLandGround[z] + S.ownAirGround[z] + S.allyAirGround[z] +
            S.ownNavalGround[z] + S.allyNavalGround[z]
        local e = S.enemyLandGround[z] + S.enemyStructureGround[z]
        theirs = theirs + e + S.enemyAirGround[z] + S.enemyNavalGround[z]
        if z ~= R.startZone and S.ownEconomyMass[z] > 0 then table.insert(ownEconomy, z) end
        local present = S.enemyLandCount[z] + S.enemyStructureCount[z]
        if present > 0 then
            R.enemyAt[z] = true
            R.enemySeen[z] = R.tick
            table.insert(enemyZones, { zone = z, strength = e, economy = S.enemyEconomyMass[z],
                structures = S.enemyStructureCount[z], army = S.enemyLandMass[z], range = S.enemyLandMaxRange[z] })
            if S.ownStructureCount[z] > 0 then table.insert(threatened, z)
            elseif S.allyStructureCount[z] > 0 then table.insert(R.allyThreat, z) end  -- (2026-11-03: an ally's zone)
        end
        if S.ownStructureCount[z] > 0 then ownStruct[z] = true end
    end
    -- the shelled zones (phase 10): an enemy land force with a long reach (kShellRange or more) in a zone beside ours
    -- whose structures it can reach is treated as in our zone, so the defence goes for it. Artillery and missile
    -- launchers parked one zone out had shelled our structures from outside every zone the defence looked at: against
    -- M28 they destroyed 16,000 mass of our structures in 20 minutes with no answer (kill probe, seed 2002).
    local listed = {}
    for _, z in ipairs(threatened) do listed[z] = true end
    for _, e in ipairs(enemyZones) do
        if not listed[e.zone] and e.range >= kShellRange then
            local ez = Remastered.AIZone(e.zone)
            for _, n in ipairs(Remastered.AIZoneNeighbours(e.zone)) do
                if ownStruct[n.zone] and not n.coast then
                    local nz = Remastered.AIZone(n.zone)
                    local dx, dz = nz.x - ez.x, nz.z - ez.z
                    if dx * dx + dz * dz <= (e.range + kShellSlack) * (e.range + kShellSlack) then
                        table.insert(threatened, e.zone)
                        listed[e.zone] = true
                        if R.tick - ((R.shellLogged or {})[e.zone] or -10000) >= 600 then
                            R.shellLogged = R.shellLogged or {}
                            R.shellLogged[e.zone] = R.tick
                            LOG(string.format('aiop %d %d shelled zone %d margin 0.00 ratio 0.00 | an enemy force of range %.0f beside our zone %d: the defence goes for it',
                                R.army, R.tick, e.zone, e.range, n.zone))
                        end
                        break
                    end
                end
            end
        end
    end
    -- the coast under fire (phase 10): water zones beside a zone with our structures where enemy ships that can hit
    -- the ground are known (the index's naval strength against ground); the air answers them (brain.lua). The kill
    -- probe against M28 (seed 2002): one modded T2 ship (bss0206, range 110) destroyed 13,400 mass of our coastal base,
    -- and nothing of ours could reach it.
    R.zoneDomain = R.zoneDomain or {}
    local coast, coastMass = {}, 0
    for z, _ in pairs(ownStruct) do  -- (collected into a sorted list below: the order does not matter here)
        for _, n in ipairs(Remastered.AIZoneNeighbours(z)) do
            if n.coast and not coast[n.zone] then coast[n.zone] = true end
        end
    end
    -- (and the water zones one step further out: a ship of range 110 shells our coast from beyond the first water
    -- zone; those are also the zones our air scouts now look at, R.coastWater)
    local outer = {}
    for w, _ in pairs(coast) do
        for _, n in ipairs(Remastered.AIZoneNeighbours(w)) do
            if not n.coast and not coast[n.zone] then outer[n.zone] = true end
        end
    end
    for w, _ in pairs(outer) do coast[w] = true end
    local coastList = {}
    for w, _ in pairs(coast) do table.insert(coastList, w) end
    table.sort(coastList)
    R.coastWater = coastList
    R.coastThreat = {}
    for _, w in ipairs(coastList) do
        if S.enemyNavalGround[w] > 0 then
            table.insert(R.coastThreat, { zone = w, mass = S.enemyNavalMass[w], ground = S.enemyNavalGround[w] })
            coastMass = coastMass + S.enemyNavalMass[w]
        end
    end
    table.sort(R.coastThreat, function(a, b)
        if a.ground ~= b.ground then return a.ground > b.ground end
        return a.zone < b.zone
    end)
    R.coastMass = coastMass
    -- (phase 10, the navy) the enemy's ships known anywhere, what we saw of them fading by kPeakFade a think
    R.enemyNavalPeak = math.max(naval, (R.enemyNavalPeak or 0) * kPeakFade)
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
        local margin = Remastered.AIZoneEstimate(R.army, e.zone, kHorizon, false, false, true)
        if margin >= bar then return e, margin end
        if not bestMargin or margin > bestMargin then bestMargin, bestZone = margin, e.zone end
    end
    return nil, bestMargin or 0, bestZone or 0
end

-- the staging zone for a target (2026-11-03): on our side of it, on the way there from the army's zone `from`: the land
-- zone with no enemy known in it whose land travel to the target is at least kStageBack and that lies on the way
-- (from -> it -> target at most kStageDetour x the direct way, + 40), the nearest such to the target (id on ties);
-- nil (no gather: the army goes from where it is) when there is none, or it is the army's own zone.
-- Before, it was the target's neighbour nearest our start, else our start zone. Chris's big test (4v4 on SETONS_R):
-- the neighbour lay beside the enemy's base, ~800 world units away through the isthmus, and the army walked there
-- with plain moves past the enemy without firing (17 of 444 arrived); our start zone was counted 45 world units around
-- its centre while the army stood spread over the base (13 of 747): every gather was dropped, and the army sat.
local function StagingZone(R, target, from)
    local direct = Remastered.AIZoneDistance(from, target, 'land')
    if direct < 0 or direct <= kStageBack + 60 then return nil end
    local best, bestD
    for z = 1, Remastered.AIMapInfo().zones do
        if not R.enemyAt[z] then
            local dt = Remastered.AIZoneDistance(z, target, 'land')
            if dt >= kStageBack and (not bestD or dt < bestD) then
                local ds = Remastered.AIZoneDistance(from, z, 'land')
                if ds >= 0 and ds + dt <= kStageDetour * direct + 40 then best, bestD = z, dt end
            end
        end
    end
    if best == from then return nil end
    return best
end

-- the zone the army stands in: the land zone at the middle of `units` when our land units reach it, else our start
local function ArmyZone(R, units)
    local n, sx, sz = 0, 0, 0
    for _, u in ipairs(units) do
        local p = u:GetPosition()
        n, sx, sz = n + 1, sx + p[1], sz + p[3]
    end
    if n == 0 then return R.startZone end
    local z = Remastered.AIZoneAt(sx / n, sz / n, 'land')
    if z > 0 and Remastered.AIZoneDistance(R.startZone, z, 'land') >= 0 then return z end
    return R.startZone
end

-- where an attack aims (2026-11-03): the enemy unit the army knows in the target zone nearest the middle of `units`
-- (Remastered.AIZoneTargets: fair intel), structures and units alike; the zone's centre when it knows none. An
-- attack-move to the zone's centre had left the army standing there while structures stood beyond its reach.
local function AimPoint(R, zone, units)
    local n, sx, sz = 0, 0, 0
    for _, u in ipairs(units) do
        local p = u:GetPosition()
        n, sx, sz = n + 1, sx + p[1], sz + p[3]
    end
    local zz = Remastered.AIZone(zone)
    if n == 0 then return zz.x, zz.z end
    sx, sz = sx / n, sz / n
    local bx, bz, bd = zz.x, zz.z
    local ux, uz, ud
    for _, t in ipairs(Remastered.AIZoneTargets(R.army, zone)) do
        local dx, dz = t.x - sx, t.z - sz
        local d = dx * dx + dz * dz
        if t.unknown then
            if not ud or d < ud then ux, uz, ud = t.x, t.z, d end
        elseif not bd or d < bd then bx, bz, bd = t.x, t.z, d end
    end
    -- (2026-11-04, fog: with no unit of known type there, the nearest contact of unknown type - a radar blip - so the
    -- army walks up to it and sees what it is; before, the zone's centre)
    if not bd and ud then return ux, uz end
    return bx, bz
end

-- (2026-11-04, fog) the mass an enemy contact of unknown type is counted at: the mean of the enemy's known mobile land
-- units (AIEnemyMix), at least kUnknownLeast. The index counts such contacts in a zone but gives them no mass, so a
-- zone with only radar blips in it was "defended" by one unit (1.5 x 0 mass + 1): the kill probe of the judge set
-- (M28 seeds 1001-4004, the first 15 minutes) found 62% of our lost mass killed by units we did not see, 45% by M28's
-- T1 artillery (range 30, beyond our extractors' vision of 20), and our T2 extractors the main victims.
local kUnknownLeast = 50
local function UnknownMass(R)
    local n, m = 0, 0
    for _, e in ipairs(Remastered.AIEnemyMix(R.army)) do
        local bp = __blueprints[e[1]]
        local land = false
        if bp and bp.Categories then
            for _, c in ipairs(bp.Categories) do if c == 'LAND' then land = true end end
        end
        if land then n, m = n + e[2], m + e.mass end
    end
    return math.max(kUnknownLeast, n > 0 and m / n or 0)
end

-- ORDERS (2026-11-03). An order replaces what the unit was doing (IssueClearCommands first), and is given again only
-- when it changes (its key) or the unit has stopped farther than `near` from the point. Every Issue* adds to the
-- unit's queue: the gather gave IssueMove to the whole army every think (5 s), so a 700-unit army carried dozens of
-- stale moves to a staging zone, and a defence's order waited behind them. R.orderOf: entity id -> key.
local function Order(R, units, aggressive, pos, key, near)
    local give = {}
    local n2 = (near or 0) * (near or 0)
    for _, u in ipairs(units) do
        local id = u:GetEntityId()
        if R.orderOf[id] ~= key then
            table.insert(give, u)
        elseif u:IsIdleState() then
            local p = u:GetPosition()
            local dx, dz = p[1] - pos[1], p[3] - pos[3]
            if dx * dx + dz * dz > n2 then table.insert(give, u) end
        end
    end
    if give[1] then
        for _, u in ipairs(give) do R.orderOf[u:GetEntityId()] = key end
        IssueClearCommands(give)
        if aggressive then IssueAggressiveMove(give, pos) else IssueMove(give, pos) end
    end
end

local function Pos3(x, z)
    return { x, GetTerrainHeight(x, z), z }
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
                -- (phase 12: the defence reads the estimate with soak, every unit of ours taking fire by its health,
                -- as before phase 12; with the fighters-only read here the army held back far more often and the
                -- veteran duel against FAF was lost at minute 23.5-37.4 instead of 47.5)
                local margin, _, _, _, _, theirsMass = Remastered.AIZoneEstimate(R.army, z, kPostHorizon, false, kDefenceSoak)
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
    -- (2026-11-03, perf: a post takes at most kGuardMax - its count, so only that many nearest are kept, in (distance,
    -- list order) order as the full sort gave them - the full sort of every free unit for every post, with a Lua
    -- comparator, was the AI's heaviest single cost in Chris's 4v4: 752,000 comparisons in 48 beats. The positions
    -- and ids are read once.)
    local px, pz, pid = {}, {}, {}
    for i, u in ipairs(units) do
        local q = u:GetPosition()
        px[i], pz[i], pid[i] = q[1], q[3], u:GetEntityId()
    end
    for _, p in ipairs(postList) do
        if guardMass >= budget then break end
        if not Full(p) then
            local zone = Remastered.AIZone(p.zone)
            local want = kGuardMax - count[p.zone]
            local near, k = {}, 0
            for i, u in ipairs(units) do
                if not guard[pid[i]] then
                    local dx, dz = px[i] - zone.x, pz[i] - zone.z
                    local d = dx * dx + dz * dz
                    -- (kept sorted by d, then i; i only grows, so an equal d goes after the ones kept)
                    if k < want or d < near[k].d then
                        local j = k < want and k + 1 or k
                        while j > 1 and near[j - 1].d > d do
                            near[j] = near[j - 1]
                            j = j - 1
                        end
                        near[j] = { u = u, d = d, i = i }
                        if k < want then k = k + 1 end
                    end
                end
            end
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
                elseif fresh[id] then
                    IssueClearCommands({ u })  -- (2026-11-03: a new guard already there drops the order it had)
                end
                R.orderOf[id] = 'post'
            end
        else
            table.insert(rest, u)
        end
    end
    return rest
end

-- HUNT (2026-11-07, step 5: plan by what the army owns). The enemy army mass a player can have by a minute: the median of
-- the five 1v1 pro replays (procurve: 0 at 2, 819 at 5, 4,230 at 10, 10,084 at 15, 25,654 at 20 minutes; +3,000 a
-- minute after). With no operation and no target known, an army of at least kHuntShare x that (or of the enemy army
-- known, if more) goes to the nearest enemy start: a Fatboy given at minute 1 stood at home for 6 minutes before.
local kHuntShare = 2
local kProCurve = { { 2, 0 }, { 5, 819 }, { 10, 4230 }, { 15, 10084 }, { 20, 25654 } }
local function ExpectedArmy(minutes)
    if minutes <= kProCurve[1][1] then return 0 end
    for i = 2, table.getn(kProCurve) do
        local a, b = kProCurve[i - 1], kProCurve[i]
        if minutes <= b[1] then return a[2] + (b[2] - a[2]) * (minutes - a[1]) / (b[1] - a[1]) end
    end
    local last = kProCurve[table.getn(kProCurve)]
    return last[2] + 3000 * (minutes - last[1])
end

-- SHIELD UPKEEP (2026-11-07, step 5): shields our energy cannot carry go off, and come back on when it can
local kShieldOff = 0.1
local kShieldOn = 0.6
local function ShieldUpkeep(brain, R)
    local list = {}
    for _, u in ipairs(brain:GetListOfUnits(categories.MOBILE, false)) do
        if not u.Dead and u:GetFractionComplete() >= 1 then
            local bp = u:GetBlueprint()
            local up = bp.Economy and bp.Economy.MaintenanceConsumptionPerSecondEnergy or 0
            if up > 0 and u:TestToggleCaps('RULEUTC_ShieldToggle') then
                table.insert(list, { u = u, up = up, on = u:GetScriptBit('RULEUTC_ShieldToggle') })
            end
        end
    end
    if not list[1] then return end
    local stored = brain:GetEconomyStoredRatio('ENERGY')
    local income, asked = brain:GetEconomyIncome('ENERGY') * 10, brain:GetEconomyRequested('ENERGY') * 10
    if stored < kShieldOff and asked > income then
        local n = 0
        for _, e in ipairs(list) do
            if e.on then
                e.u:SetScriptBit('RULEUTC_ShieldToggle', false)
                n = n + 1
            end
        end
        if n > 0 then
            LOG(string.format('aishield %d %d %d shields off (energy %.0f%%, income %.0f, asked %.0f)', R.army, R.tick, n,
                100 * stored, income, asked))
        end
    elseif stored > kShieldOn then
        local best
        for _, e in ipairs(list) do
            if not e.on and e.up <= income - asked and (not best or e.up < best.up) then best = e end
        end
        if best then
            best.u:SetScriptBit('RULEUTC_ShieldToggle', true)
            LOG(string.format('aishield %d %d a shield on (%s, upkeep %.0f; energy %.0f%%, income %.0f, asked %.0f)', R.army,
                R.tick, best.u:GetUnitId(), best.up, 100 * stored, income, asked))
        end
    end
end

-- the nearest enemy start (land travel from ours; army order on ties) of an enemy still in the game, or nil
local function HuntZone(R)
    local best, bestD
    for i, b in ipairs(ArmyBrains) do
        if i ~= R.army and IsEnemy(R.army, i) and not ArmyIsOutOfGame(i) then
            local x, z = b:GetArmyStartPos()
            local zone = x and Remastered.AIZoneAt(x, z, 'land') or 0
            local d = zone > 0 and Remastered.AIZoneDistance(R.startZone, zone, 'land') or -1
            if d >= 0 and (not bestD or d < bestD) then best, bestD = zone, d end
        end
    end
    return best
end

-- (Lua 5.0 allows a function 32 upvalues: Think reaches the helpers of 2026-11-03 through this one table)
local X = { Order = Order, AimPoint = AimPoint, ArmyZone = ArmyZone, Pos3 = Pos3, gatherPerUnit = kGatherPerUnit,
    gatherExtend = kGatherExtend, allyReach = kAllyReach, UnknownMass = UnknownMass, defendCome = kDefendCome,
    edge = kEdge, nearTravel = kNearTravel, ExpectedArmy = ExpectedArmy,
    HuntZone = HuntZone, huntShare = kHuntShare, ShieldUpkeep = ShieldUpkeep }

function Think(brain)
    local R = brain.Remaster
    if not R or R.startZone <= 0 then return end
    R.op = R.op or nil
    X.ShieldUpkeep(brain, R)  -- (2026-11-07, step 5)
    local units = CombatUnits(brain)
    local armyMass = 0
    for _, u in ipairs(units) do armyMass = armyMass + MassOf(R, u) end
    R.armyMass = armyMass  -- (the economy's map-control floor reads it: brain.lua)
    R.armyCount = table.getn(units)  -- (2026-11-03: the land factories' soft and hard caps read it: brain.lua)
    -- (the orders given, ORDERS above: kept for the units alive, so dead ones leave the table)
    local orderOf = {}
    for _, u in ipairs(units) do
        local id = u:GetEntityId()
        orderOf[id] = R.orderOf and R.orderOf[id]
    end
    R.orderOf = orderOf
    R.opSeq = R.opSeq or 0
    R.alive = {}  -- (the combat units alive now, by entity id: the trade check's members)
    for _, u in ipairs(units) do R.alive[u:GetEntityId()] = true end
    local ours, theirs, enemyZones, threatened, ownEconomy = Front(R)
    -- (phase 6, fog of war: what the army saw of the enemy's strength fades slowly, kPeakFade a think, so a few
    -- minutes without sight of its army are not "far ahead"; the duels with fog launched decisive pushes at a ratio
    -- of 20-50 into armies they had not seen for a while)
    R.theirsPeak = math.max(theirs, (R.theirsPeak or 0) * kPeakFade)
    theirs = R.theirsPeak
    local ratio = ours / math.max(theirs, 1)
    local K = Knobs(brain)
    if math.mod(R.tick, 600) < 50 then  -- (a status line a minute: tests score "pushes when ahead" from these; the
        -- operations think on ticks 10-19 of each 50 since 2026-11-03, still once in the minute's first 50)
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
    local startPos = X.Pos3(R.startX, R.startZ)

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
    local defendedNow = false
    R.raided = R.raided or {}
    -- the units nearest a zone's centre, until 1.5x the enemy mass that can come there (all of them when `all`)
    local function Defenders(z, theirsMass, all, hereMass)
        local zone = Remastered.AIZone(z)
        -- (2026-11-03, perf: sorted natively, Remastered.AISortIndex - distance, then list order as before)
        local near, keys = {}, {}
        for _, u in ipairs(units) do
            if not busy[u:GetEntityId()] then
                local p = u:GetPosition()
                local dx, dz = p[1] - zone.x, p[3] - zone.z
                table.insert(near, u)
                table.insert(keys, dx * dx + dz * dz)
            end
        end
        local sent, mass = {}, 0
        for _, k in ipairs(Remastered.AISortIndex(keys)) do
            -- (2026-11-07: 1.5 x the enemy in the zone + kDefendCome x the rest that can come; before, 1.5 x all that
            -- can come within 400: against M28 every unit, every think, chasing one raider at home)
            local need = hereMass and (1.5 * hereMass + X.defendCome * math.max(0, theirsMass - hereMass)) or 1.5 * theirsMass
            if not all and mass >= need + 1 then break end
            local u = near[k]
            table.insert(sent, u)
            busy[u:GetEntityId()] = true
            mass = mass + MassOf(R, u)
        end
        return sent
    end
    local unknownMass = threatened[1] and X.UnknownMass(R) or 0
    for _, z in ipairs(threatened) do
        if z ~= R.startZone then R.raided[z] = R.tick end
        local margin, _, _, _, _, theirsMass = Remastered.AIZoneEstimate(R.army, z, kHorizon, false, kDefenceSoak)
        -- (2026-11-04, fog: the contacts of unknown type there count at unknownMass each, UnknownMass above)
        local zf = Remastered.AIZoneForces(R.army, z)
        local unknown = zf.enemy.land.unknown
        theirsMass = theirsMass + unknown * unknownMass
        -- (2026-11-07) the enemy mass known IN the zone (its land units and unknown contacts)
        local hereMass = math.min(theirsMass, (zf.enemy.land.mass or 0) + unknown * unknownMass)
        -- the base first (phase 7b): an enemy force of kRecallMass or more at a base zone recalls the army from its
        -- operation, and for kRecallTicks the base's defence takes every free unit. The no-cheat duel against M28
        -- was lost with the army away on an attack while M28's army reached the base and killed the commander.
        local base = Remastered.AIZoneDistance(R.startZone, z, 'land') <= kBaseZone
        if base and hereMass >= kRecallMass then
            if R.op then
                Log(R, 'recall', z, margin, ratio, string.format('the base is attacked (%.0f mass can come): the %s at zone %d is called off',
                    theirsMass, R.op.kind, R.op.zone))
                R.op = nil
                R.recalledAt = R.tick
            end
            R.recallUntil = R.tick + kRecallTicks
        end
        local all = base and R.recallUntil and R.tick < R.recallUntil
        if margin >= K.defend then
            if not (R.defending == z) then
                Log(R, 'defend', z, margin, ratio, 'enemies in our zone; the estimate lets us hold it')
                R.defending = z
            end
            -- (2026-11-03: at the enemies the army knows there, not the zone's centre; orders replace, ORDERS above)
            local sent = Defenders(z, theirsMass, all, hereMass)
            defendedNow = true
            if sent[1] then
                local x, zz = X.AimPoint(R, z, sent)
                X.Order(R, sent, true, X.Pos3(x, zz), string.format('d%d_%d_%d', z, math.floor(x / 16), math.floor(zz / 16)), 10)
            end
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
                if home[1] then X.Order(R, home, true, startPos, 'home', 40) end
            end
        end
    end
    if not threatened[1] then R.defending = nil end
    -- ALLIES (2026-11-03, Chris's big test: "the allied AI is building a massive army but doesn't fight the small army
    -- of red units attacking" its ally): the zone of an ally's within kAllyReach of our start by land that enemies
    -- are in, nearest first, when none of ours is defended this think and the estimate there (ours and the allies'
    -- that can come) lets us hold it: the nearest units, 1.5x the enemy mass that can come, as for our own zones.
    if not defendedNow and not (R.recallUntil and R.tick < R.recallUntil) then
        local list = {}
        for _, z in ipairs(R.allyThreat) do
            local d = Remastered.AIZoneDistance(R.startZone, z, 'land')
            if d >= 0 and d <= X.allyReach then table.insert(list, { z = z, d = d }) end
        end
        table.sort(list, function(a, b)
            if a.d ~= b.d then return a.d < b.d end
            return a.z < b.z
        end)
        for _, q in ipairs(list) do
            local margin, _, _, _, _, theirsMass = Remastered.AIZoneEstimate(R.army, q.z, kHorizon, false, kDefenceSoak)
            if margin >= K.defend and theirsMass > 0 then
                if R.allyDefending ~= q.z then
                    Log(R, 'ally', q.z, margin, ratio, string.format('an ally is attacked (%.0f mass can come there): we help', theirsMass))
                    R.allyDefending = q.z
                end
                local sent = Defenders(q.z, theirsMass, false)
                if sent[1] then
                    local x, zz = X.AimPoint(R, q.z, sent)
                    X.Order(R, sent, true, X.Pos3(x, zz), string.format('y%d_%d_%d', q.z, math.floor(x / 16), math.floor(zz / 16)), 10)
                end
                break
            end
        end
        if not list[1] then R.allyDefending = nil end
    end
    local free = {}
    for _, u in ipairs(units) do
        if not busy[u:GetEntityId()] then table.insert(free, u) end
    end
    units = Posts(R, K, free, ownEconomy, ratio, armyMass)

    -- a GATHER in progress: the army goes to the staging zone; once most of it is there (or the time is up) it
    -- commits if the estimate still lets it, else the gather is dropped. (2026-11-03: "there" is a radius that grows
    -- with the army - a 700-unit army does not stand within 45 world units - the walk is an attack-move, so it fights
    -- what it meets on the way, and a gather still arriving when the time is up waits on, up to kGatherExtend x)
    if R.op and R.op.kind == 'gather' then
        local margin = Remastered.AIZoneEstimate(R.army, R.op.zone, kHorizon, false, false, true)
        local f = Remastered.AIZoneForces(R.army, R.op.zone)
        local stage = Remastered.AIZone(R.op.stage)
        -- (2026-11-03: the share is of the units the gather started with, R.op.n0: in the 4v4 the factories added
        -- hundreds while the army walked, and 302 of 800 never reached 70%)
        local n = math.min(table.getn(units), R.op.n0 or table.getn(units))
        local radius = kGatherRadius + X.gatherPerUnit * math.sqrt(n)
        local there = Near(units, stage, radius)
        local bar = R.op.push and K.commitPush or K.commit
        local age = R.tick - R.op.since
        local limit = R.op.limit or kGatherTicks
        local timeUp = age >= limit and not (there > (R.op.there or 0) and age < X.gatherExtend * limit)
        R.op.there = there
        if f.enemy.land.count + f.enemy.structure.count <= 0 then
            Log(R, 'dropped', R.op.zone, margin, ratio, 'gather: no enemy known at the target any more')
            R.op = nil
        elseif (there > 0 and there >= kGatherShare * n) or timeUp then
            if there > 0 and there >= kGatherLeast * n and margin >= bar then
                R.op = { kind = R.op.push and 'push' or 'attack', zone = R.op.zone, stage = R.op.stage, since = R.tick,
                    logged = R.tick, seq = R.op.seq }
                R.op.lost0, R.op.killed0 = Trade(brain)
                R.op.tradeTick = R.tick
                Log(R, 'commit', R.op.zone, margin, ratio, string.format('gathered (%d of %d at the staging zone): %s',
                    there, n, R.op.kind .. ' together'))
                -- (the attack's orders follow below, this think)
            else
                Log(R, 'dropped', R.op.zone, margin, ratio, there > 0 and
                    (margin < bar and string.format('gather: the estimate fell under %.2f', bar) or
                    string.format('gather: only %d of %d reached the staging zone', there, n)) or
                    'gather: nobody reached the staging zone')
                R.op = nil
            end
        else
            if units[1] then X.Order(R, units, true, X.Pos3(stage.x, stage.z), 'g' .. R.op.seq, 0.5 * radius) end
            return
        end
    end

    -- an ATTACK or PUSH in progress: stay committed while its margin holds
    if R.op and (R.op.kind == 'attack' or R.op.kind == 'push') then
        local margin = Remastered.AIZoneEstimate(R.army, R.op.zone, kHorizon, false, false, true)
        local f = Remastered.AIZoneForces(R.army, R.op.zone)
        local commit = R.op.kind == 'push' and K.commitPush or K.commit
        -- the trade check (phase 9): over each kTradeWindow of the attack, the mass we lost against the mass we killed
        -- (all of the army's losses and kills in that time). Against M28 the estimate said 0.8 while the attack lost
        -- 10-20 times what it killed to artillery and missiles the army never saw; a fight the real numbers say we are
        -- losing ends whatever the estimate says. It is logged 'withdraw', not 'retreat': the enemy there is not
        -- weaker, only unseen, so the never-retreat-from-a-weaker-force rule does not apply.
        local withdraw
        -- (2026-11-03: the mass lost is that of the units that fought in the operation, R.op.members, not the whole
        -- army's: in the 4v4 with 800 units the army lost 1,500 mass a minute elsewhere - posts, the base, allies'
        -- fights - and every push was withdrawn after 1-2 minutes whatever it traded)
        if R.op.tradeTick and R.tick - R.op.tradeTick >= kTradeWindow then
            local lost, killed = Trade(brain)
            local dk = killed - R.op.killed0
            local dl, keep = 0, {}
            for _, m in ipairs(R.op.members or {}) do
                if R.alive[m.id] then table.insert(keep, m) else dl = dl + m.mass end
            end
            R.op.members = keep
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
            if units[1] then X.Order(R, units, false, startPos, 'back', 40) end
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
                if units[1] then X.Order(R, units, false, startPos, 'back', 40) end
                return
            end
        else
            if R.tick - (R.op.logged or 0) >= 300 then
                Log(R, 'committed', R.op.zone, margin, ratio, R.op.kind .. ' goes on')
                R.op.logged = R.tick
            end
            -- reinforcements (phase 5): a unit farther from the target than the staging zone is (plus the gather's
            -- radius) goes there first, so new units join the army instead of reaching the fight one by one
            local tzone = Remastered.AIZone(R.op.zone)
            local stage = R.op.stage and Remastered.AIZone(R.op.stage)
            local sd = stage and math.sqrt((stage.x - tzone.x) * (stage.x - tzone.x) + (stage.z - tzone.z) * (stage.z - tzone.z))
            local radius = kGatherRadius + X.gatherPerUnit * math.sqrt(table.getn(units))
            local fight, join = {}, {}
            for _, u in ipairs(units) do
                local p = u:GetPosition()
                local dx, dz = p[1] - tzone.x, p[3] - tzone.z
                local d = math.sqrt(dx * dx + dz * dz)
                if stage and d > sd + radius then table.insert(join, u) else table.insert(fight, u) end
            end
            -- (2026-11-03: the fight goes at the enemy the army knows in the zone nearest it, AimPoint, not its centre)
            R.op.members = R.op.members or {}
            R.op.memberOf = R.op.memberOf or {}
            for _, u in ipairs(fight) do
                local id = u:GetEntityId()
                if not R.op.memberOf[id] then
                    R.op.memberOf[id] = true
                    table.insert(R.op.members, { id = id, mass = MassOf(R, u) })
                end
            end
            if fight[1] then
                local x, z = X.AimPoint(R, R.op.zone, fight)
                X.Order(R, fight, true, X.Pos3(x, z), string.format('a%d_%d_%d', R.op.seq, math.floor(x / 16), math.floor(z / 16)), 10)
            end
            if join[1] then X.Order(R, join, true, X.Pos3(stage.x, stage.z), 'j' .. R.op.seq, 0.5 * radius) end
            return
        end
    end

    -- (2026-11-07, step 4) the real edge: the free army against the enemy army known near
    local edgeOk = true
    if not R.op and ratio >= kAttackRatio and units[1] then
        local free, near = 0, 0
        for _, u in ipairs(units) do free = free + MassOf(R, u) end
        for _, e in ipairs(enemyZones) do
            local d = Remastered.AIZoneDistance(R.startZone, e.zone, 'land')
            if d >= 0 and d <= X.nearTravel then near = near + e.army end
        end
        edgeOk = free >= X.edge * near
        if not edgeOk and R.tick - (R.edgeLogged or -10000) >= 600 then
            Log(R, 'edge', 0, 0, ratio, string.format('ahead overall, but the free army %.0f mass < %.1f x the enemy army known near %.0f: it waits',
                free, X.edge, near))
            R.edgeLogged = R.tick
        end
    end
    -- a new ATTACK / PUSH when ahead, by the estimate at the target
    if not R.op and ratio >= kAttackRatio and units[1] and edgeOk then
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
            -- (2026-11-03: the staging zone on our side of the target, on the way from where the army is; none when
            -- the army is that near already: it goes at once)
            local from = X.ArmyZone(R, units)
            local stage = StagingZone(R, target.zone, from)
            R.opSeq = R.opSeq + 1
            if stage then
                -- (phase 6: the gather waits for the walk, kGatherSpeed, as well as kGatherTicks)
                local walk = math.max(0, Remastered.AIZoneDistance(from, stage, 'land'))
                R.op = { kind = 'gather', push = push, zone = target.zone, stage = stage, since = R.tick,
                    limit = kGatherTicks + math.floor(10 * walk / kGatherSpeed), seq = R.opSeq, n0 = table.getn(units) }
                if R.recalledAt then
                    Log(R, 'resume', target.zone, margin, ratio, string.format('out again %d s after the recall', math.floor((R.tick - R.recalledAt) / 10)))
                    R.recalledAt = nil
                end
                Log(R, push and 'push' or 'attack', target.zone, margin, ratio, string.format('%s; target: %s; gathering at zone %d (from zone %d)',
                    push and 'far ahead overall: a decisive push' or 'ahead overall and the estimate favours us', what, stage, from))
                X.Order(R, units, true, (ZonePos(stage)), 'g' .. R.opSeq, 0)
            else
                R.op = { kind = push and 'push' or 'attack', zone = target.zone, since = R.tick, logged = R.tick, seq = R.opSeq }
                R.op.lost0, R.op.killed0 = Trade(brain)
                R.op.tradeTick = R.tick
                Log(R, push and 'push' or 'attack', target.zone, margin, ratio, string.format('%s; target: %s; near: at once (from zone %d)',
                    push and 'far ahead overall: a decisive push' or 'ahead overall and the estimate favours us', what, from))
                local x, z = X.AimPoint(R, target.zone, units)
                X.Order(R, units, true, X.Pos3(x, z), string.format('a%d_%d_%d', R.opSeq, math.floor(x / 16), math.floor(z / 16)), 10)
            end
            return
        elseif bestZone and bestZone > 0 and R.tick - (R.lastWait or 0) >= 300 then
            Log(R, 'wait', bestZone, margin, ratio, 'ahead overall, but no target the estimate favours yet')
            R.lastWait = R.tick
        end
    end

    -- HUNT (2026-11-07, step 5): no operation, and no target the attack took above; an army far stronger than what the
    -- enemy can have by now goes to the nearest enemy start (attack-move: it fights what it meets on the way)
    if not R.op and units[1] then
        local known = 0
        for _, e in ipairs(enemyZones) do known = known + e.army end
        local want = X.huntShare * math.max(X.ExpectedArmy(R.tick / 600), known)
        local free = 0
        for _, u in ipairs(units) do free = free + MassOf(R, u) end
        if free >= want and free > 0 then
            local hz = X.HuntZone(R)
            if hz then
                if R.tick - (R.huntLogged or -10000) >= 300 then
                    Log(R, 'hunt', hz, 0, ratio, string.format('no target known; our free army %.0f mass >= %.0f (%gx what the enemy can have by now): to the nearest enemy start',
                        free, want, X.huntShare))
                    R.huntLogged = R.tick
                end
                X.Order(R, units, true, (ZonePos(hz)), 'h' .. hz, 30)
                return
            end
        end
    end

    -- RAID: an enemy zone with economy and no known defenders, with up to 6 of our lightest combat units
    if not R.op and units[1] then
        for _, e in ipairs(enemyZones) do
            if e.economy > 0 and e.strength <= 0 and Remastered.AIZoneDistance(R.startZone, e.zone, 'land') >= 0 then
                local margin = Remastered.AIZoneEstimate(R.army, e.zone, kHorizon, false, false, true)
                if margin >= 0.8 then
                    local light = {}
                    for _, u in ipairs(units) do
                        if table.getn(light) < 6 and u:IsIdleState() then table.insert(light, u) end
                    end
                    if light[1] then
                        Log(R, 'raid', e.zone, margin, ratio, 'enemy economy without known defenders')
                        local x, z = X.AimPoint(R, e.zone, light)
                        X.Order(R, light, true, X.Pos3(x, z), 'r' .. e.zone, 10)
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
    if home[1] then X.Order(R, home, true, startPos, 'home', 60) end
end
