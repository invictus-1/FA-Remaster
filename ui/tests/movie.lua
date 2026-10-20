local framework = import('/testing/framework.lua')
-- The Movie control (the game's .sfd movies) as the original's CMauiMovie: loaded and paused on its
-- first picture after Set, Stop pauses (OnStopped), Play runs on from there, the end calls
-- OnFinished at the movie's own length; GetNumFrames from the Sofdec header.
--   moho64ui_test --init <init.lua> --stage frontend --lua ui/tests/movie.lua --frames 900 --log out.log
local test = framework.suite('movie')
local check = function(what, got, want) return test:equal(what, got, want) end
local Movie = import("/lua/maui/movie.lua").Movie
local m = Movie(GetFrame(0), "/movies/Brackman.sfd")
m.Left:Set(100) m.Top:Set(100)
m.Width:Set(m.MovieWidth) m.Height:Set(m.MovieHeight)
check("a talking head loads", m:IsLoaded(), true)
check("its size", m.MovieWidth() .. "x" .. m.MovieHeight(), "192x192")
check("its frames, from the Sofdec header", m:GetNumFrames(), 150)
check("its frame rate (29.97)", math.abs(m:GetFrameRate() - 29.97) < 0.01, true)
local bad = Movie(GetFrame(0))
bad.Left:Set(400) bad.Top:Set(100) bad.Width:Set(1) bad.Height:Set(1)
test:cleanup(function() m:Destroy() bad:Destroy() end)
check("a movie that is not there does not load", bad:InternalSet("/movies/no_such_movie.sfd"), false)
local stopped, finished = nil, nil
m.OnStopped = function(self) stopped = GetSystemTimeSeconds() end
m.OnFinished = function(self) finished = GetSystemTimeSeconds() end
test:fork(function()
    local start = GetSystemTimeSeconds()
    m:Play()
    WaitSeconds(1)
    m:Stop()
    WaitSeconds(0.2)
    check("Stop: OnStopped on the next frame step", stopped ~= nil, true)
    WaitSeconds(1)
    check("a stopped movie does not finish", finished, nil)
    m:Play()  -- (runs on from where it stood: about 4 s left)
    while not finished and GetSystemTimeSeconds() - start < 12 do
        WaitSeconds(0.1)
    end
    local played = finished and finished - start or -1
    -- 5.0 s of movie, 1.2 s stopped
    check("Play runs on from where it stood; OnFinished at its end", played > 5.8 and played < 6.8, true)
    LOG("TEST note: finished after " .. string.format("%.2f", played) .. " s (5.0 s of movie, 1.2 s stopped)")
    test:finish()
end)
