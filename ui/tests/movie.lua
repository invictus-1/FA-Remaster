-- The Movie control (the game's .sfd movies) as the original's CMauiMovie: loaded and paused on its
-- first picture after Set, Stop pauses (OnStopped), Play runs on from there, the end calls
-- OnFinished at the movie's own length; GetNumFrames from the Sofdec header.
--   moho64ui --init <init.lua> --stage frontend --lua ui/tests/movie.lua --frames 900 --log out.log
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local Movie = import("/lua/maui/movie.lua").Movie
local m = Movie(GetFrame(0), "/movies/Brackman.sfd")
check("a talking head loads", m:IsLoaded(), true)
check("its size", m.MovieWidth() .. "x" .. m.MovieHeight(), "192x192")
check("its frames, from the Sofdec header", m:GetNumFrames(), 150)
check("its frame rate (29.97)", math.abs(m:GetFrameRate() - 29.97) < 0.01, true)
local bad = Movie(GetFrame(0))
check("a movie that is not there does not load", bad:InternalSet("/movies/no_such_movie.sfd"), false)
local stopped, finished = nil, nil
m.OnStopped = function(self) stopped = GetSystemTimeSeconds() end
m.OnFinished = function(self) finished = GetSystemTimeSeconds() end
-- A movie played again from its own handler keeps its frame steps (the original switches them off BEFORE it runs the
-- script, FA exe 0x79f1e7 / 0x79f226 / 0x79f281, so the handler's Play, which switches them on, stands; the review's
-- toolkit M3). `chained`: Play with no film is OnFinished at the next frame step, and that handler sets a film and
-- plays it. `goesOn`: its OnStopped plays on.
local chained = Movie(GetFrame(0))
local chainedFinished = 0
chained.OnFinished = function(self)
    chainedFinished = chainedFinished + 1
    if chainedFinished == 1 then
        self:InternalSet("/movies/Brackman.sfd")
        self:Play()
    end
end
local goesOn = Movie(GetFrame(0), "/movies/Brackman.sfd")
local goesOnStopped, goesOnFinished = 0, nil
goesOn.OnStopped = function(self)
    goesOnStopped = goesOnStopped + 1
    self:Play()
end
goesOn.OnFinished = function(self) goesOnFinished = GetSystemTimeSeconds() end
ForkThread(function()
    local start = GetSystemTimeSeconds()
    m:Play()
    chained:Play()
    goesOn:Play()
    WaitSeconds(1)
    check("Play with no film: OnFinished at the next frame step", chainedFinished, 1)
    check("a film set and played from that OnFinished keeps its frame steps", chained:NeedsFrameUpdate(), true)
    m:Stop()
    goesOn:Stop()
    WaitSeconds(0.2)
    check("Stop: OnStopped on the next frame step", stopped ~= nil, true)
    check("the same for the movie whose OnStopped plays on, once", goesOnStopped, 1)
    check("which keeps its frame steps", goesOn:NeedsFrameUpdate(), true)
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
    -- (both began with the first movie and lost a frame step or two: over long before it)
    check("the film played from OnFinished ran to its own end: OnFinished a second time", chainedFinished, 2)
    local wentOn = goesOnFinished and goesOnFinished - start or -1
    check("the movie played on from OnStopped ran to its end", wentOn > 4.9 and wentOn < 5.5, true)
    LOG("TEST note: the movie played on from OnStopped finished after " .. string.format("%.2f", wentOn) .. " s")
    LOG("TEST done: " .. failed .. " failed")
end)
