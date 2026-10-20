local framework = import('/testing/framework.lua')
-- A bitmap's animation, as the original's (FA exe, CMauiBitmap, read 2026-10-16): the patterns of
-- frames, one step per frame update once a step's time has passed, the end of the pattern, the
-- script's OnAnimationFrame / OnAnimationFinished / OnAnimationStopped.
--   moho64ui_test --init <init.lua> --stage setup --size 1920x1080 --lua ui/tests/bitmap.lua --frames 60 --log out.log
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local Bitmap = import("/lua/maui/bitmap.lua").Bitmap

local test = framework.suite('bitmap')
local check = function(what, got, want) return test:equal(what, got, want) end

local folder = "/textures/ui/common/scx_menu/large-no-bracket-btn/"
local four = {folder .. "large_btn_up.dds", folder .. "large_btn_down.dds", folder .. "large_btn_over.dds", folder .. "large_btn_dis.dds"}
local b = Bitmap(GetFrame(0))
b.Left:Set(100) b.Top:Set(100)
b:SetTexture(four)
check("four textures: the forward pattern, four places", b:GetNumFrames(), 4)
b:SetPingPongPattern()
check("ping-pong: 0..3..0, seven places", b:GetNumFrames(), 7)
b:SetLoopPingPongPattern()
check("looping ping-pong: 0..3..1, six places", b:GetNumFrames(), 6)
b:SetBackwardPattern()
check("backward: four places", b:GetNumFrames(), 4)
b:SetFrame(10)
check("SetFrame is held inside the pattern", b:GetFrame(), 3)
b:SetFrame(-2)
check("and not below its start", b:GetFrame(), 0)
b:SetFramePattern({0, 1, 5, -1, 3})
check("SetFramePattern: numbers outside the textures are held to them, not left out", b:GetNumFrames(), 5)
check("a pattern of something else than numbers is an error", pcall(b.SetFramePattern, b, {0, "x"}), false)
check("one argument, no more", pcall(b.SetFramePattern, b, {0}, {1}), false)

local one = Bitmap(GetFrame(0))
one.Left:Set(500) one.Top:Set(100)
one:SetTexture(folder .. "large_btn_up.dds")
test:cleanup(function() b:Destroy() one:Destroy() end)
local stoppedOne = 0
one.OnAnimationStopped = function(self) stoppedOne = stoppedOne + 1 end
one:Play()
check("Play on one texture does nothing", one:NeedsFrameUpdate(), false)
one:Stop()
check("nor Stop", stoppedOne, 0)

local frames, finished, stopped = {}, 0, 0
b.OnAnimationFrame = function(self, frame) table.insert(frames, tostring(frame)) end
b.OnAnimationFinished = function(self) finished = finished + 1 end
b.OnAnimationStopped = function(self) stopped = stopped + 1 end
b:SetForwardPattern()
b:SetFrame(0)
b:SetFrameRate(100000)  -- (a step at every frame update)
check("Play gives the bitmap back", b:Play() == b, true)
check("and asks for frame updates", b:NeedsFrameUpdate(), true)
test:fork(function()
    WaitFrames(8)
    check("one place a frame; at the end it stays on the last, told twice", table.concat(frames, " "), "1 2 3 3")
    check("OnAnimationFinished once", finished, 1)
    check("it stopped on the last place", b:GetFrame(), 3)
    check("and its frame updates with it", b:NeedsFrameUpdate(), false)
    frames = {}
    b:SetFrame(0)
    b:Loop(true)
    b:Play()
    WaitFrames(6)
    check("looping: past the end to the start", string.sub(table.concat(frames, " "), 1, 9), "1 2 3 0 1")
    check("each time round OnAnimationFinished", finished >= 2, true)
    b:Stop()
    check("Stop tells the script", stopped, 1)
    check("and ends the frame updates", b:NeedsFrameUpdate(), false)
    local at = b:GetFrame()
    WaitFrames(3)
    check("stopped: it stays where it was", b:GetFrame(), at)
    b:SetFrameRate(1 / 3600)  -- (a step an hour)
    frames = {}
    b:Play()
    WaitFrames(3)
    check("no step before its time", table.getn(frames), 0)
    b:Stop()
    b:SetTexture(four)
    check("a new texture: back to the start", b:GetFrame(), 0)
    test:finish()
end)
