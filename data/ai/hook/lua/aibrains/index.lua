-- FA Remastered's own AI: its lobby keys (docs/ai-architecture.md 4.1). Part of FA Remastered, MIT licence (see
-- LICENSE); our own code. The engine joins this hook to /lua/aibrains/index.lua only in a session where an army
-- plays one of these keys (src/ai/scripts.h). Type and skill only; personality and teacher mode are per-slot options.
for _, skill in {'recruit', 'veteran', 'commander', 'supreme'} do
    keyToBrain['remastered_' .. skill] = { "/lua/remastered/ai/brain.lua", "AIBrain" }
    keyToBrain['remastered_' .. skill .. '_cheat'] = { "/lua/remastered/ai/brain.lua", "AIBrain" }
end
