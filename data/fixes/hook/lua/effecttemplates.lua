-- FA Remastered, the engine's script fixes (docs/deviations.md 12, "Remaster aliases of emitter files"). Joined to
-- the end of /lua/EffectTemplates.lua when Sim::fixes is on; never in the playback of the original's replays.
--
-- Chris (2026-11-04): each use of the stock /effects/emitters/small_test_fire.bp gets a fireball of its own in the
-- remastered look. A use is one table here that holds the file: ten distinct tables (found in a session's Lua, every
-- table reachable from _G); two of them have a second name that is the same table. Each names its own emitter path,
-- a remaster alias. The aliases' table is sim/effect_aliases.cpp: the engine registers each as the stock emitter's
-- blueprint, and the renderer draws the stock file for it unless a remastered look overrides it. Lua cannot read that
-- table, so the same ten paths are named here; Sim::Start warns about any alias found here that is not registered.
--
-- Only the one entry changes in each table, in place, at the same index; nothing is drawn. The tables are shared by
-- reference, so every local another file takes of one (defaultexplosions.lua's, the projectile classes' Fx lists)
-- sees the alias. Nothing takes one before this runs: this runs at the end of this file's own load, before any other
-- file can import it, and the six concatenated tables below were built by table.concatenate above, as copies holding
-- the stock path, so each gets its own alias too.
do
    local Stock = '/effects/emitters/small_test_fire.bp'
    local R = '/effects/emitters/remaster/'
    local aliases = {
        {'ExplosionSmall', R .. 'small_land_death_fire_01_emit.bp'},                -- small land deaths
        {'ExplosionSmallAir', R .. 'small_air_death_fire_01_emit.bp'},              -- air deaths, Cybran build bots
        {'FireCloudMed01', R .. 'medium_fire_cloud_fire_01_emit.bp'},               -- hit explosions
        {'FireCloudSml01', R .. 'small_fire_cloud_fire_01_emit.bp'},                -- the UEF phalanx shell's miss
        {'ExplosionEffectsSml01', R .. 'explosion_effects_sml01_fire_01_emit.bp'},
        {'ExplosionEffectsMed01', R .. 'explosion_effects_med01_fire_01_emit.bp'},  -- also ExplosionEffectsDefault01
        {'DefaultHitExplosion01', R .. 'default_hit_explosion01_fire_01_emit.bp'},
        {'DefaultHitExplosion02', R .. 'default_hit_explosion02_fire_01_emit.bp'},
        {'DefaultMissileHit01', R .. 'default_missile_hit01_fire_01_emit.bp'},      -- also CMissileHit01 (Cybran missiles)
        {'CMissileHit02', R .. 'cybran_missile_hit02_fire_01_emit.bp'},             -- Cybran missiles
    }
    -- (rawget: the game's config.lua makes reading an unset global an error)
    local env = getfenv(1)
    for _, a in aliases do
        local list = rawget(env, a[1])
        if type(list) == 'table' then
            for i, file in list do
                if file == Stock then list[i] = a[2] end
            end
        end
    end
end
