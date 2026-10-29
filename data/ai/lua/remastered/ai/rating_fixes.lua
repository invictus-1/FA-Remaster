-- FA Remastered's own AI: Chris's corrections of the unit ratings (docs/ai-architecture.md 3.3). Part of FA Remastered,
-- MIT licence (see LICENSE); our own data.
--
-- The brain hands these to the engine once per game (Remastered.AISetRatingFix / AISetBeats), before it reads any
-- rating, so the same corrections apply on every machine and in the replay.
--
-- Fix: a multiplier on every rating of a unit (its threat, its seven scores, perMass; the battle estimate scales its
--   damage by the multiplier squared). The tuning list's `fix` column: write the units whose rating feels wrong, e.g.
--     Fix = { xel0305 = 1.25, url0303 = 0.9 }
-- Beats: "A beats B" for matchups the model gets wrong: A's damage against B in the battle estimate x the factor, e.g.
--     Beats = { { 'xel0305', 'url0303', 1.5 } }
-- Ids are blueprint ids in lower case. Empty until Chris tunes the list.

Fix = {
}

Beats = {
}
