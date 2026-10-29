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
-- - defenceShare: all our defences may weigh at most this share of our mobile army's mass (plus one T1 PD).

local kBase = {
    attackRatio = 1.2, pushRatio = 2.0,
    launch = 0.6, launchPush = 0.5, commit = 0.4, commitPush = 0.3, defend = 0.35,
    investSafe = 0.5, investShare = 0.5, stallTolerance = 900,
    defenceFactor = 0.6, defencePerSite = 3, defenceBuilding = 1, defenceShare = 0.25,
}

-- the personalities: changes to the base
local kPersonalities = {
    balanced = {},
    rusher = { attackRatio = 0.9, pushRatio = 1.6, investSafe = 0.6, investShare = 0.4, defenceFactor = 0.5 },
    turtle = { attackRatio = 1.6, pushRatio = 3.0, investSafe = 0.4, investShare = 0.6, defenceFactor = 0.75,
        defencePerSite = 4, defenceShare = 0.4 },
}
local kNames = { 'balanced', 'rusher', 'turtle' }

-- the strategy layer's stances (strategy.lua): changes on top of the personality, multiplied for ratios and margins
-- of attack, added for the rest; nil fields leave the knob as it is
local kStances = {
    standard = {},
    -- we are behind in tech: hold with what we have, defences count more, economy and tech first, attack once matched
    ['catch-up'] = { attackRatio = 1.5, pushRatio = 1.5, defenceFactor = 1.4, investShare = 1.3, investSafe = 0.8,
        defenceShare = 1.5 },
    -- a greedy or turtling enemy we can hurt: attack at a lower bar
    harass = { attackRatio = 0.8, pushRatio = 0.85, launch = 0.9 },
    -- a greedy enemy we cannot hurt yet: out-eco it behind defences, attack later
    ['out-eco'] = { attackRatio = 1.3, investShare = 1.4, investSafe = 0.8, defenceFactor = 1.2 },
    -- a rushing enemy: hold, defences count more
    hold = { attackRatio = 1.3, pushRatio = 1.2, defenceFactor = 1.3, investSafe = 1.2 },
}

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
        LOG(string.format('aiop %d %d personality %s teacher %s', R.army, R.tick, name,
            tostring(brain.RemasterOptions and brain.RemasterOptions.teacher)))
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
    R.knobs, R.knobsFor = k, name .. '/' .. stance
    return k
end
