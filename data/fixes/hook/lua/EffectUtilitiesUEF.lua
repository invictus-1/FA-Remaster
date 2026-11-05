-- FA Remastered, the engine's script fixes (docs/deviations.md 13, "A UEF builder's lost beam end"). Joined to the end
-- of /lua/EffectUtilitiesUEF.lua when Sim::fixes is on; never in the playback of the original's replays.
--
-- A UEF builder keeps one beam-end projectile for all its builds (builder.UEFBuildProjectile, and a second one,
-- UEFBuildProjectile2, for the commanders' slice beams), made once and only warped and given a new velocity at each
-- build. When a build ends, the thread that steers it is killed and the projectile keeps its last velocity: it drifts
-- on, through the ground (it collides with nothing), for as long as the builder stands idle. Under FAF 3840's rules a
-- shell below height 0 impacts (Projectile::MotionTick's hook 0x69bdda, engine-ref/specs/faf3840.md S3), and this one's
-- OnImpact (DummyProjectile) destroys it; the cached handle then names a destroyed object, every later build's
-- ProjectileSetVelocity on it raises "Game object has been destroyed", and the builder shows no beams again. 3839's
-- rules never destroy it (it drifts on under the map, unseen).
-- Here, when one of the three functions starts, a cached beam end that is destroyed, or has drifted down to below
-- height 1 (it would cross 0 within the tick the function first waits: it falls about 0.1 a beat), is let go and
-- forgotten, so the function makes a new one, as it does for a builder's first build. Every beam end in use lies on
-- the unit being built, well above that; a builder whose beam end is in place is not touched.
do
    local IsDestroyed = IsDestroyed
    local EntityGetPositionXYZ = moho.entity_methods.GetPositionXYZ

    local function Fresh(p)
        if not p then return nil end
        if IsDestroyed(p) then return nil end
        local _, y = EntityGetPositionXYZ(p)
        if y < 1 then
            p:Destroy()
            return nil
        end
        return p
    end

    -- (the three are the module's own globals: EffectUtilities.lua and the unit classes take them when they import it,
    -- after this file has run)
    local function Wrap(original)
        return function(builder, unitBeingBuilt, buildEffectBones, buildEffectsBag)
            if builder then
                builder.UEFBuildProjectile = Fresh(builder.UEFBuildProjectile)
                builder.UEFBuildProjectile2 = Fresh(builder.UEFBuildProjectile2)
            end
            return original(builder, unitBeingBuilt, buildEffectBones, buildEffectsBag)
        end
    end
    CreateDefaultBuildBeams = Wrap(CreateDefaultBuildBeams)
    CreateUEFBuildSliceBeams = Wrap(CreateUEFBuildSliceBeams)
    CreateUEFCommanderBuildSliceBeams = Wrap(CreateUEFCommanderBuildSliceBeams)
end
