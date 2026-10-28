-- FA Remastered's own AI: the brain (docs/ai-architecture.md). Part of FA Remastered, MIT licence (see LICENSE); our
-- own design and code, nothing taken from M28AI.
--
-- Phase 1b: a skeleton only. It plays as FAF's plain army brain (what human armies use) and marks itself; the layers
-- come in phase 3 (the think threads, own random stream, economy) and later. The engine gives this file to the sim
-- state only in a session where an army plays one of our keys (src/ai/scripts.h).
local StandardBrain = import("/lua/aibrain.lua").AIBrain

AIBrain = Class(StandardBrain) {

    RemasteredAI = true,

    OnCreateAI = function(self, planName)
        StandardBrain.OnCreateAI(self, planName)
        LOG('FA Remastered AI (phase 1b skeleton): army ' .. tostring(self:GetArmyIndex()) .. ' ' .. tostring(self.Name))
    end,
}
