-- A bitmap's animation, as the original's (FA exe, CMauiBitmap, read 2026-10-16): the patterns of
-- frames, one step per frame update once a step's time has passed, the end of the pattern, the
-- script's OnAnimationFrame / OnAnimationFinished / OnAnimationStopped.
--   moho64ui --init <init.lua> --stage setup --size 1920x1080 --lua ui/tests/bitmap.lua --frames 60 --log out.log
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local Bitmap = import("/lua/maui/bitmap.lua").Bitmap

local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end

local folder = "/textures/ui/common/scx_menu/large-no-bracket-btn/"
local four = {folder .. "large_btn_up.dds", folder .. "large_btn_down.dds", folder .. "large_btn_over.dds", folder .. "large_btn_dis.dds"}
local b = Bitmap(GetFrame(0))
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
one:SetTexture(folder .. "large_btn_up.dds")
local stoppedOne = 0
one.OnAnimationStopped = function(self) stoppedOne = stoppedOne + 1 end
one:Play()
check("Play on one texture does nothing", one:NeedsFrameUpdate(), false)
one:Stop()
check("nor Stop", stoppedOne, 0)

-- ShareTextures (FA exe 0x782d60, 0x77fcf0; the review's toolkit M5): the other bitmap's textures, and BitmapWidth
-- and BitmapHeight with them (the first texture's size), the bitmap given back. The frame pattern is not shared and
-- not changed: a bitmap that had none draws nothing until a script gives it one, as the original's.
local function place(bitmap, l, t)
    bitmap.Left:Set(l) bitmap.Top:Set(t) bitmap.Width:Set(100) bitmap.Height:Set(40)
    return bitmap
end
-- (the bitmaps above get a place too: moho64ui_PixelAt draws the whole head, and a bitmap with no layout cannot be)
place(b, 100, 100)
place(one, 100, 200)
local tw, th = GetTextureDimensions(folder .. "large_btn_up.dds")
local share = place(Bitmap(GetFrame(0)), 1000, 600)
check("a new bitmap has no size", share.BitmapWidth() .. " x " .. share.BitmapHeight(), "0 x 0")
check("ShareTextures gives the bitmap back", share:ShareTextures(one) == share, true)
check("and takes the size with the textures", share.BitmapWidth() .. " x " .. share.BitmapHeight(), tw .. " x " .. th)
check("which is a size", tw > 0 and th > 0, true)
check("the pattern is not shared: it had none, it has none", share:GetNumFrames(), 0)
check("so nothing is drawn yet", moho64ui_PixelAt(1050, 620), "00000000")
share:SetForwardPattern()
check("a forward pattern over the shared texture", share:GetNumFrames(), 1)
check("and the shared texture is drawn", moho64ui_PixelAt(1050, 620) ~= "00000000", true)
-- (FAF's use, AutolobbyMapPreview.lua: a bitmap made with a solid colour takes another's texture)
local icon = place(Bitmap(GetFrame(0)), 1200, 600)
icon:SetSolidColor("ff20c040")
check("a solid colour draws", moho64ui_PixelAt(1250, 620), "ff20c040")
icon:ShareTextures(one)
check("sharing into a bitmap of a solid colour: its one-place pattern stays", icon:GetNumFrames(), 1)
check("the shared texture's size", icon.BitmapWidth() .. " x " .. icon.BitmapHeight(), tw .. " x " .. th)
check("and the texture is drawn, not the colour", moho64ui_PixelAt(1250, 620), moho64ui_PixelAt(1050, 620))
icon:ShareTextures(b)
check("an animation's four textures shared: the pattern still one place", icon:GetNumFrames(), 1)
icon:SetForwardPattern()
check("until a pattern is asked for: four", icon:GetNumFrames(), 4)
local colour = place(Bitmap(GetFrame(0)), 100, 300)
colour:SetSolidColor("ffc02040")
local tint = place(Bitmap(GetFrame(0)), 1400, 600)
tint:ShareTextures(colour)
check("a solid colour shared: the colour texture's size, 2 x 2", tint.BitmapWidth() .. " x " .. tint.BitmapHeight(), "2 x 2")
check("no pattern, nothing drawn, a colour neither", tint:GetNumFrames() .. " " .. moho64ui_PixelAt(1450, 620), "0 00000000")
tint:SetForwardPattern()
check("a forward pattern over the one colour texture", tint:GetNumFrames(), 1)
check("and its colour drawn", moho64ui_PixelAt(1450, 620), "ffc02040")
check("two arguments, no more", pcall(share.ShareTextures, share, one, one), false)
check("the other must be a bitmap", pcall(share.ShareTextures, share, GetFrame(0)), false)
for _, x in {share, icon, colour, tint} do x:Destroy() end

local frames, finished, stopped = {}, 0, 0
b.OnAnimationFrame = function(self, frame) table.insert(frames, tostring(frame)) end
b.OnAnimationFinished = function(self) finished = finished + 1 end
b.OnAnimationStopped = function(self) stopped = stopped + 1 end
b:SetForwardPattern()
b:SetFrame(0)
b:SetFrameRate(100000)  -- (a step at every frame update)
check("Play gives the bitmap back", b:Play() == b, true)
check("and asks for frame updates", b:NeedsFrameUpdate(), true)
ForkThread(function()
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
    LOG("TEST done: " .. failed .. " failed")
end)
