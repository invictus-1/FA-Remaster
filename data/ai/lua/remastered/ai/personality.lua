-- FA Remastered's own AI: the few knobs (docs/ai-architecture.md 2.7; Chris's decision 16 in docs/ai-design-goals.md).
-- Part of FA Remastered, MIT licence (see LICENSE); our own design and code, nothing taken from M28AI.
--
-- Every weight that makes the AI more careful or bolder lives here, in one place, so that the personalities, the
-- difficulty, the strategy layer's stances and the player's future sliders (defensive <-> aggressive, eco <-> army)
-- all lean the same few knobs. A knob only moves a weight the AI already uses; none overrides its judgement (the
-- battle estimate still decides every fight, and a retreat from a clearly weaker force stays impossible).
--
-- The knobs:
-- - attackRatio / pushRatio: how far ahead overall (our strength / theirs) the army must be to attack / to push;
-- - launch / launchPush: the battle-estimate margin at the target that launches an attack / a push;
-- - commit / commitPush: the margin an attack / a push stays committed at;
-- - defend: the margin at which an own zone is defended (below it the army waits at home);
-- - investSafe: the zone estimate within the payback window at which an investment is safe enough (investment risk);
-- - investShare: the share of the smoothed mass income over the planned dip that investments may take;
-- - stallTolerance: ticks of empty mass storage that count as a sustained deficit (shorter dips are planned);
-- - defenceFactor: how much a defence's build value counts against units' (fixed, so marked down; the defence share);
-- - defencePerSite / defenceBuilding: point defences at most per site / ordered or under construction at once;
-- - defenceShare: all our defences may weigh at most this share of our mobile army's mass (plus one T1 PD);
-- - armyMatch (phase 6): the land army's mass we keep per mass of the enemy's known ground army (map control: below
--   it, most factories make units, not engineers); armyPerMinute: a standing army's mass per minute of the game, the
--   floor from minute 5 while little of the enemy is known; engineerRate: how fast the engineer cap grows (x the base 2.5 a minute);
-- - postShare (phase 6): a guard post at an outlying economy zone wants this much of our mass per mass of the enemy
--   that can come to it soon; guardShare: all guard posts together take at most this share of the army (the rest
--   is free for the operations); reclaimShare: the share of the engineers kept reclaiming while there is reclaim.
--
-- The player's sliders (phase 6, docs/ai-architecture.md 2.7): two per-slot lobby options lean these knobs after the
-- personality and the stance, in steps from -2 to 2 (0, the default, changes nothing):
-- - RemasterAggression: defensive (-) <-> aggressive (+): the attack and push ratios and the launch, commit and defend
--   margins lower (bolder) or higher (more careful), and defences count less or more;
-- - RemasterEconomy: army (-) <-> economy (+): the investment share and risk, the army kept per enemy army and the
--   engineers' growth.
-- Each step is a fixed factor, and every margin stays within bounds, so no slider setting breaks the AI: the battle
-- estimate still decides, and the never-retreat-from-a-weaker-force rule (operations.lua kWeaker) is not a knob.

local kBase = {
    attackRatio = 1.2, pushRatio = 2.0,
    launch = 0.6, launchPush = 0.5, commit = 0.4, commitPush = 0.3, defend = 0.35,
    investSafe = 0.5, investShare = 0.5, stallTolerance = 900,
    defenceFactor = 0.6, defencePerSite = 3, defenceBuilding = 1, defenceShare = 0.25,
    armyMatch = 0.8, armyPerMinute = 80, engineerRate = 1, postShare = 1.2, guardShare = 0.4,
    reclaimShare = 0.3,
}

-- the personalities: changes to the base
local kPersonalities = {
    balanced = {},
    rusher = { attackRatio = 0.9, pushRatio = 1.6, investSafe = 0.6, investShare = 0.4, defenceFactor = 0.5,
        armyMatch = 1.0, armyPerMinute = 110, engineerRate = 0.85, guardShare = 0.3, reclaimShare = 0.25 },
    turtle = { attackRatio = 1.6, pushRatio = 3.0, investSafe = 0.4, investShare = 0.6, defenceFactor = 0.75,
        defencePerSite = 4, defenceShare = 0.4, guardShare = 0.5, reclaimShare = 0.35 },
}
local kNames = { 'balanced', 'rusher', 'turtle' }

-- the strategy layer's stances (strategy.lua): changes on top of the personality, multiplied for ratios and margins
-- of attack, added for the rest; nil fields leave the knob as it is
local kStances = {
    standard = {},
    -- we are behind in tech: hold with what we have, defences count more, economy and tech first, attack once matched
    ['catch-up'] = { attackRatio = 1.5, pushRatio = 1.5, defenceFactor = 1.4, investShare = 1.3, investSafe = 0.8,
        defenceShare = 1.5, armyMatch = 0.9, guardShare = 1.2 },
    -- a greedy or turtling enemy we can hurt: attack at a lower bar
    harass = { attackRatio = 0.8, pushRatio = 0.85, launch = 0.9, armyMatch = 1.1, guardShare = 0.9 },
    -- a greedy enemy we cannot hurt yet: out-eco it behind defences, attack later
    ['out-eco'] = { attackRatio = 1.3, investShare = 1.4, investSafe = 0.8, defenceFactor = 1.2, armyMatch = 0.9 },
    -- a rushing enemy: hold, defences count more
    hold = { attackRatio = 1.3, pushRatio = 1.2, defenceFactor = 1.3, investSafe = 1.2, armyMatch = 1.25,
        guardShare = 1.2 },
}

-- the sliders (phase 6): per step of RemasterAggression (+ = aggressive) a factor on the ratios and defences, an
-- offset on the margins; per step of RemasterEconomy (+ = economy) factors on the investments and the army kept
local kAggressionRatio = 0.85   -- attackRatio, pushRatio x this per step
local kAggressionMargin = 0.04  -- launch, launchPush, commit, commitPush, defend minus this per step
local kAggressionDefence = 0.9  -- defenceFactor, defenceShare, guardShare x this per step
local kEconomyInvest = 1.15     -- investShare x this per step; engineerRate and reclaimShare too
local kEconomySafe = 0.05       -- investSafe minus this per step
local kEconomyArmy = 0.85       -- armyMatch, armyPerMinute x this per step
-- bounds every margin keeps whatever the sliders and stances say (kWeaker = 0.6 in operations.lua stays above
-- every commit bar, so a retreat from a clearly weaker force stays impossible)
local kBounds = {
    launch = { 0.45, 0.8 }, launchPush = { 0.4, 0.75 }, commit = { 0.25, 0.55 }, commitPush = { 0.2, 0.5 },
    defend = { 0.25, 0.5 }, investSafe = { 0.3, 0.9 }, guardShare = { 0.15, 0.7 },
}
local kBoundNames = { 'commit', 'commitPush', 'defend', 'guardShare', 'investSafe', 'launch', 'launchPush' }

-- a slider's step from the slot's option: a number or a numeric string from -2 to 2, else 0
local function Step(v)
    local n = tonumber(v)
    if not n then return 0 end
    n = math.floor(n + 0.5)
    if n < -2 then return -2 end
    if n > 2 then return 2 end
    return n
end

-- the slider steps of the army (read once from the slot's options: brain.lua)
function Sliders(brain)
    local o = brain.RemasterOptions or {}
    return Step(o.aggression), Step(o.economy)
end

local function Copy(t)
    local c = {}
    for k, v in pairs(t) do c[k] = v end  -- (a copy of plain values: order does not matter)
    return c
end

-- the army's personality name: the slot's option, else drawn once from its own stream
function Name(brain)
    local R = brain.Remaster
    if not R.personality then
        local name = brain.RemasterOptions and brain.RemasterOptions.personality or 'random'
        if not kPersonalities[name] then name = kNames[Remastered.AIRandom(R.army, table.getn(kNames))] end
        R.personality = name
        local ag, ec = Sliders(brain)
        LOG(string.format('aiop %d %d personality %s teacher %s aggression %d economy %d', R.army, R.tick, name,
            tostring(brain.RemasterOptions and brain.RemasterOptions.teacher), ag, ec))
    end
    return R.personality
end

-- the knobs as they stand: base, then the personality, then the stance (multipliers); cached until either changes
function Knobs(brain)
    local R = brain.Remaster
    local name = Name(brain)
    local stance = R.stance or 'standard'
    if R.knobs and R.knobsFor == name .. '/' .. stance then return R.knobs end
    local k = Copy(kBase)
    for key, v in pairs(kPersonalities[name]) do k[key] = v end
    for key, v in pairs(kStances[stance] or {}) do k[key] = k[key] * v end
    k.defencePerSite = math.floor(k.defencePerSite + 0.5)
    -- the player's sliders, after the personality and the stance
    local ag, ec = Sliders(brain)
    if ag ~= 0 then
        local f = math.pow(kAggressionRatio, ag)
        k.attackRatio, k.pushRatio = k.attackRatio * f, k.pushRatio * f
        for _, key in ipairs({ 'launch', 'launchPush', 'commit', 'commitPush', 'defend' }) do
            k[key] = k[key] - kAggressionMargin * ag
        end
        local d = math.pow(kAggressionDefence, ag)
        k.defenceFactor, k.defenceShare, k.guardShare = k.defenceFactor * d, k.defenceShare * d, k.guardShare * d
    end
    if ec ~= 0 then
        local f = math.pow(kEconomyInvest, ec)
        k.investShare, k.engineerRate, k.reclaimShare = k.investShare * f, k.engineerRate * f, k.reclaimShare * f
        k.investSafe = k.investSafe - kEconomySafe * ec
        local a = math.pow(kEconomyArmy, ec)
        k.armyMatch, k.armyPerMinute = k.armyMatch * a, k.armyPerMinute * a
    end
    for _, key in ipairs(kBoundNames) do
        local b = kBounds[key]
        if k[key] < b[1] then k[key] = b[1] elseif k[key] > b[2] then k[key] = b[2] end
    end
    R.knobs, R.knobsFor = k, name .. '/' .. stance
    return k
end
