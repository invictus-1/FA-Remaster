-- The graphics device lost, and every picture made anew (ours: app/game_session.h, GameSession::ServeDevice; the
-- renderer's request 71 B, render/world_picture.h [devicelost]). The host's own test functions:
--   moho64ui_RecoverDevice([mode])  asks for the program's recovery at the next frame boundary: "check" (or none)
--     though the device is not lost; "lose" with the program's own device REALLY lost first (only this program's, by
--     Direct3D's own call for it; allowed only in a run with --shader-cache);
--   moho64ui_DeviceState()          how the pictures stand, what the last recovery did, and the switches this program
--     sets on the world as the world shown has them now.
-- ALWAYS run it with --shader-cache <a folder of the test's own>: a run that makes its device anew must not keep its
-- shaders and pipelines in the player's own folder (the renderer's first loss test wrote a short pipelines file over
-- the player's). Without the option this test makes no device anew: it only checks that "lose" is refused, and ends.
-- Without a window (the pictures read back) the window's own picture, its second window and a map opening behind are
-- NOT reached. In a LIVE WINDOW (add --window, for the window checks) the same cases run on the window's picture: the
-- game case without the pixel samples (the interface's own drawing has no world in it there), the front cases on the
-- menu's world and the flight (moho64ui_MenuFlight's world* fields). The window branches were run once in a live
-- window (2026-11-11: `lose` 49 ok, `front` 16 ok); `frontlose`, `fail` and `frontfail` in a window: not run.
-- The case after "--" (/devicecase):
--   game (the default)  a running game, the game's own picture:
--     moho64ui --init <init.lua> --prefs <a copy> --stage setup --size 1920x1080 --quiet --session <session_ai_cheats.lua>
--              --seed 7 --day 14 --day-length 0 --weather overcast --shader-cache <folder>
--              --game-lua ui/tests/device_lost.lua --frames 3000 --log out.log
--     a recovery while the game runs (its time goes on, the same units, the selection kept, the player's looks and the
--     game's sky on the new world before any frame is drawn, the world drawn again), then, the game paused, two more
--     right after each other with the picture sampled before and after, compared loosely; then one while the photo
--     mode makes a photo (the photo is dropped and the photo mode says so; the next photo is made and saved).
--   lose                the same game, then the device really lost and noticed by the next frame drawn (add
--                       "-- /devicecase lose").
--   front               the front end with the menu's background (its own picture, on its own thread) and the main
--                       menu's flight's battle running:
--     moho64ui ... --stage frontend --backdrop --menu-flight --maps <SETONS_R's folder> --shader-cache <folder>
--              --lua ui/tests/device_lost.lua --frames 4000 --log out.log -- /devicecase front
--   frontlose           the same with the device really lost (-- /devicecase frontlose).
--   fail, frontfail     the game, or the front end: a recovery that finds no device, played through ("fail": the
--                       pictures let go, five rounds two seconds apart, none asked for): the program closes in order.
--                       The script's last line comes before it; the runner then looks for the one sentence in the log
--                       ("error: moho64ui: FA Remastered has to close: ..."), the lines of an orderly end after it
--                       ("moho64ui: ran <n> frames", fewer than --frames) and the exit code 1.
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed". The log must hold no
-- "error:" line after it (the runner's own look: the script does not read the log).
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local function near(a, b, within) return type(a) == "number" and type(b) == "number" and math.abs(a - b) <= within end
local case = (GetCommandLineArg("/devicecase", 1) or {})[1] or "game"
local inGame = rawget(_G, "SessionIsActive") and SessionIsActive()
local function state() return moho64ui_DeviceState() or {} end
-- (until `test` holds, at most `seconds` of the PC's clock: a recovery takes seconds of it within one frame)
local function waitFor(seconds, test)
    local untilAt = moho64ui_WallSeconds() + seconds
    while moho64ui_WallSeconds() < untilAt do
        if test() then return true end
        WaitFrames(1)
    end
    return test()
end
-- A recovery asked for and waited for: true when one more is done. The frame it is asked in ends with it (the
-- recovery runs inside the host's step, however long it takes), so the next frame sees it done.
local function recover(mode)
    local before = state().recoveries or 0
    local asked, why = moho64ui_RecoverDevice(mode)
    if not asked then
        LOG("TEST note: moho64ui_RecoverDevice refused: " .. tostring(why))
        return false
    end
    return waitFor(120, function() return (state().recoveries or 0) > before end)
end
-- Without a shader folder of the run's own: no device is made anew by this test (the header says why).
local function noFolder()
    if state().shaderCache ~= "" then return false end
    local asked, why = moho64ui_RecoverDevice("lose")
    check("without --shader-cache the real loss is refused", tostring(asked) .. " " .. tostring(string.find(why or "", "--shader-cache", 1, true) ~= nil), "false true")
    check("and nothing is asked", tostring(state().pending) .. " " .. tostring(state().recoveries), "false 0")
    LOG("TEST note: no --shader-cache: this test makes no device anew in the player's shader folder; nothing else is checked")
    LOG("TEST done: " .. failed .. " failed")
    return true
end
local function describeLast(last)
    local parts = {}
    for _, p in last.pictures or {} do
        table.insert(parts, string.format("%s: %s, %.2f s (the device %.2f s after %d, the map %.2f s; %d props, %d effects, %d looks)", p.name,
            p.ok and "made anew" or "NOT made anew", p.seconds, p.deviceSeconds, p.deviceTries, p.mapSeconds, p.props, p.effects, p.looks))
    end
    return string.format("%.2f s, %d round(s), mode '%s', lost %s (%s); %s", last.seconds or -1, last.tries or -1, tostring(last.mode),
        tostring(last.wasLost), tostring(last.reason), table.concat(parts, "; "))
end

if not inGame then
    -- The front end: the menu's background's own picture, drawn on its own thread, and the flight's battle beside it.
    if case ~= "front" and case ~= "frontlose" and case ~= "frontfail" then return end
    ForkThread(function()
        WaitSeconds(2)
        local W, H = GetFrame(0).Width(), GetFrame(0).Height()
        -- (the menu's film asks the background for its picture whenever the interface is drawn: drawn here each frame,
        -- so that the background's thread goes on drawing; it rests half a second after the last ask)
        local function draw() return moho64ui_PixelAt(math.floor(W / 2), math.floor(H / 2)) end
        local function pictures() return (state().backdrop or {}).pictures or 0 end
        local function drawUntil(seconds, test)
            return waitFor(seconds, function()
                draw()
                return test()
            end)
        end
        check("the front end: the device's state is there", type(moho64ui_DeviceState()), "table")
        if noFolder() then return end
        if state().pictures.window then
            -- A LIVE WINDOW (UNTESTED, see the header): the window's own picture shows the menu's map, and in the
            -- Flight mode its battle; its switches on the world are moho64ui_MenuFlight's world* fields.
            local function menu() return moho64ui_MenuFlight() or {} end
            check("window: no picture but the window's", tostring(state().pictures.backdrop) .. " " .. tostring(state().pictures.game), "false false")
            check("window: the menu's world shows", waitFor(180, function() return menu().worldShows == "menu" and menu().worldPending == "none" end), true)
            waitFor(60, function() return menu().flightShows end)  -- (the Flight mode: its battle on the world)
            WaitSeconds(3)
            local m0 = menu()
            local function air(m) return string.format("%.3f %.3f %.6f", m.worldTimeOfDay or -1, m.worldCloudCover or -1, m.worldCinematicHaze or -1) end
            LOG("TEST note: window: mode " .. tostring(m0.mode) .. ", the flight shows " .. tostring(m0.flightShows) .. ", the world's hour, clouds and least haze " .. air(m0))
            check("window: before, the evening on the menu's world (17.4 h, clouds 0.45)", near(m0.worldTimeOfDay, 17.4, 0.01) and near(m0.worldCloudCover, 0.45, 0.01), true)
            if case == "frontfail" then
                check("the recovery that fails is asked for", moho64ui_RecoverDevice("fail"), true)
                LOG("TEST done: " .. failed .. " failed")
                return
            end
            for round = 1, 2 do
                check("window: recovery " .. round, recover(round == 1 and case == "frontlose" and "lose" or nil), true)
                local last = state().last or {}
                LOG("TEST note: window: recovery " .. round .. ": " .. describeLast(last))
                check("window: ok, one picture: the window's", tostring(last.ok) .. " " .. table.getn(last.pictures or {}) .. " " ..
                    tostring((last.pictures or {})[1] and last.pictures[1].name), "true 1 the window's picture")
                local m1 = menu()
                check("window: the menu's world shows still, the flight as before", tostring(m1.worldShows) .. " " .. tostring(m1.flightShows), "menu " .. tostring(m0.flightShows))
                check("window: the evening and the flight's air again (Backdrop::Dress, the flight's haze)", air(m1), air(m0))
                WaitSeconds(3)
                if m0.flightShows then
                    check("window: the flight's battle goes on", menu().tick > m1.tick or menu().loops > m1.loops, true)
                end
                check("window: nothing lost, nothing pending", tostring(state().lost) .. " " .. tostring(state().pending), "false false")
            end
            LOG("TEST done: " .. failed .. " failed")
            return
        end
        check("the background has a picture of its own (the read-back way), and no other picture is there",
            tostring(state().pictures.backdrop) .. " " .. tostring(state().pictures.window) .. " " .. tostring(state().pictures.game), "true false false")
        local flight = moho64ui_MenuFlight()
        LOG("TEST note: the menu's flight: " .. (flight and (tostring(flight.state) .. " at tick " .. tostring(flight.tick)) or "none"))
        local tick0 = flight and flight.tick or 0
        if case == "frontfail" then
            -- no device to be had: the program closes at this frame's end (the runner reads the log for it)
            check("the recovery that fails is asked for", moho64ui_RecoverDevice("fail"), true)
            LOG("TEST done: " .. failed .. " failed")
            return
        end

        -- a recovery though nothing is lost. (Its map may still be opening on its thread, seconds, or minutes with an
        -- empty shader folder: the recovery waits for that inside the frame it is asked in, so no frames pass.)
        check("a recovery is asked for", moho64ui_RecoverDevice(), true)
        check("and stands asked until the frame's end", state().pending, true)
        check("it is done", waitFor(900, function() return (state().recoveries or 0) >= 1 end), true)
        local last = state().last or {}
        LOG("TEST note: the recovery: " .. describeLast(last))
        check("the recovery: ok, a check (nothing was lost), one picture: the background's own",
            tostring(last.ok) .. " " .. tostring(last.wasLost) .. " " .. tostring(last.mode) .. " " .. table.getn(last.pictures or {}) .. " " ..
            tostring((last.pictures or {})[1] and last.pictures[1].name), "true false check 1 the menu background's own picture")
        check("its map was opened again", (last.pictures or {})[1] and last.pictures[1].world and last.pictures[1].mapSeconds > 0, true)
        check("the new world has the evening again (Backdrop::Dress: 17.4 h, clouds 0.45)",
            near(state().backdrop.timeOfDay, 17.4, 0.01) and near(state().backdrop.cloudCover, 0.45, 0.01), true)
        check("no picture is lost afterwards, nothing pending", tostring(state().lost) .. " " .. tostring(state().pending), "false false")
        local after = pictures()
        check("the background draws pictures again", drawUntil(60, function() return pictures() > after end), true)
        check("and the interface shows one (the window's middle is not black)", draw() ~= "ff000000" and draw() ~= "00000000", true)
        if flight then
            local now = moho64ui_MenuFlight()
            check("the flight's battle went on", now.tick > tick0 or now.loops > (flight.loops or 0), true)
        end

        -- a second one right after
        check("a second recovery right after", recover(), true)
        check("two done, the second ok", tostring(state().recoveries) .. " " .. tostring(state().last.ok), "2 true")
        after = pictures()
        check("the background draws pictures again after the second", drawUntil(60, function() return pictures() > after end), true)

        if case == "frontlose" then
            -- the device really lost: the background's own frame notices, the interface's thread recovers
            local done = state().recoveries
            local asked, why = moho64ui_RecoverDevice("lose")
            check("the real loss is asked for (a run with --shader-cache)", tostring(asked) .. " " .. tostring(why), "true nil")
            if asked then
                check("the background's next frame notices and the pictures are made anew",
                    drawUntil(240, function() return (state().recoveries or 0) > done end), true)
                last = state().last or {}
                LOG("TEST note: the real loss: " .. describeLast(last))
                check("the recovery after the loss: ok, the device was lost, with its reason",
                    tostring(last.ok) .. " " .. tostring(last.wasLost) .. " " .. tostring(last.mode) .. " " .. tostring(string.len(last.reason or "") > 0), "true true lose true")
                check("the evening again", near(state().backdrop.timeOfDay, 17.4, 0.01) and near(state().backdrop.cloudCover, 0.45, 0.01), true)
                after = pictures()
                check("the background draws pictures again after the loss", drawUntil(60, function() return pictures() > after end), true)
                check("nothing lost, nothing pending, not closing",
                    tostring(state().lost) .. " " .. tostring(state().pending) .. " " .. tostring(state().closing), "false false false")
            end
        end
        LOG("TEST done: " .. failed .. " failed")
    end)
    return
end

-- A running game.
if case ~= "game" and case ~= "lose" and case ~= "fail" then return end
ForkThread(function()
    WaitSeconds(5)
    local view = import("/lua/ui/game/worldview.lua").viewLeft
    local W, H = GetFrame(0).Width(), GetFrame(0).Height()
    -- (the interface drawn now, the world's picture with it: without a window nothing draws the world by itself)
    local function draw() return moho64ui_PixelAt(math.floor(W / 2), math.floor(H / 2)) end
    -- the world view's picture at a grid of points (colours as {r, g, b})
    local function sample()
        local out = {}
        local x0, y0, x1, y1 = view.Left(), view.Top(), view.Right(), view.Bottom()
        for j = 1, 5 do
            for i = 1, 8 do
                local c = moho64ui_PixelAt(math.floor(x0 + (x1 - x0) * i / 9), math.floor(y0 + (y1 - y0) * j / 6)) or "00000000"
                table.insert(out, { tonumber(string.sub(c, 3, 4), 16), tonumber(string.sub(c, 5, 6), 16), tonumber(string.sub(c, 7, 8), 16) })
            end
        end
        return out
    end
    local function lit(samples)  -- (how many are not black)
        local n = 0
        for _, c in samples do
            if c[1] + c[2] + c[3] > 24 then n = n + 1 end
        end
        return n
    end
    local function alike(a, b, within)  -- (how many differ by at most `within` in every colour)
        local n = 0
        for i, c in a do
            local d = b[i]
            if d and math.abs(c[1] - d[1]) <= within and math.abs(c[2] - d[2]) <= within and math.abs(c[3] - d[3]) <= within then n = n + 1 end
        end
        return n
    end
    -- the player's units (their ids, sorted)
    local function units()
        local kept = GetSelectedUnits()
        UISelectionByCategory("ALLUNITS", false, false, false, false)
        local ids = {}
        for _, u in GetSelectedUnits() or {} do table.insert(ids, tonumber(u:GetEntityId())) end
        SelectUnits(kept)
        table.sort(ids)
        for i, id in ids do ids[i] = tostring(id) end
        return table.concat(ids, " ")
    end
    -- the world's switches as one line (every one the state gives, by name)
    local function switches(world)
        local names, parts = {}, {}
        for k in world or {} do table.insert(names, k) end
        table.sort(names)
        for _, k in names do
            local v = world[k]
            table.insert(parts, k .. "=" .. (type(v) == "number" and string.format("%.4g", v) or tostring(v)))
        end
        return table.concat(parts, " ")
    end
    -- the player's looks against the world's own switches (those with one switch each)
    local function looksOnWorld(world)
        local looks = moho64ui_Looks()
        local wrong = {}
        for _, k in { "originalLook", "tracers", "snowTracks", "bloom", "waterRipples", "battleEffects", "energyShields", "lampsTeamColour",
                      "stormCells", "starryNight", "remasteredGate", "waterFxMirror" } do
            if world[k] ~= looks[k] then table.insert(wrong, k .. " " .. tostring(world[k]) .. "/" .. tostring(looks[k])) end
        end
        if (world.photoLook and 1 or 0) ~= (looks.photoLook ~= 0 and 1 or 0) then table.insert(wrong, "photoLook " .. tostring(world.photoLook) .. "/" .. tostring(looks.photoLook)) end
        for _, k in { "sharpen", "hazeMist", "iconScale", "bloomIntensity", "meshDetail" } do
            if not near(world[k], looks[k], 1e-4) then table.insert(wrong, k .. " " .. tostring(world[k]) .. "/" .. tostring(looks[k])) end
        end
        return table.concat(wrong, ", ")
    end

    check("the device's state is there", type(moho64ui_DeviceState()), "table")
    if noFolder() then return end
    -- (a LIVE WINDOW: the game's world is in the window's picture; UNTESTED, see the header)
    local windowed = state().pictures.window
    local pictureName = windowed and "the window's picture" or "the game's own picture"
    check("the game has a picture of its own (no window; in a window: the window's), and no other picture is there",
        tostring(state().pictures.game) .. " " .. tostring(state().pictures.gameOwn) .. " " .. tostring(state().pictures.window) .. " " ..
        tostring(state().pictures.backdrop), windowed and "true false true false" or "true true false false")
    LOG("TEST note: the run's shader folder: '" .. tostring(state().shaderCache) .. "'")
    -- some looks other than a new profile's, so that a world made anew without them would show
    ConExecute("remaster_photo_look 2")
    ConExecute("remaster_sharpen 0.35")
    ConExecute("remaster_haze_mist 1.5")
    ConExecute("remaster_tracers 0")
    ConExecute("remaster_energy_shields 0")
    ConExecute("remaster_night_sky 0")
    WaitFrames(2)
    draw()
    WaitFrames(2)
    local looks = moho64ui_Looks()
    check("the looks set: Photographic Look 2, Sharpen 0.35, Haze & Mist 1.5, no tracers, no energy shields, the plain night sky",
        string.format("%d %.2f %.2f %s %s %s", looks.photoLook, looks.sharpen, looks.hazeMist, tostring(looks.tracers), tostring(looks.energyShields),
            tostring(looks.starryNight)), "2 0.35 1.50 false false false")
    local world0 = state().world
    check("a world is shown", type(world0), "table")
    check("before: the world has the player's looks", looksOnWorld(world0 or {}), "")
    check("before: the map's edge as the game's (no frame of machinery, the land beyond)",
        tostring(world0.frame) .. " " .. tostring(world0.worldBeyond) .. " " .. tostring(world0.mapSurround), "false true true")
    local line0 = switches(world0)
    LOG("TEST note: the world's switches before: " .. line0)
    local stats0 = state().stats or {}
    check("before: the world is drawn (models in the last frame)", (stats0.models or 0) > 0, true)

    local cdr = GetArmyAvatars()[1]
    SelectUnits({cdr})
    for i = 1, 20 do  -- (a commander still arriving cannot be selected yet)
        if table.getn(GetSelectedUnits() or {}) == 1 then break end
        WaitSeconds(0.5)
        SelectUnits({cdr})
    end
    WaitFrames(2)
    check("the commander is selected", table.getn(GetSelectedUnits() or {}) == 1 and GetSelectedUnits()[1] == cdr, true)
    local units0 = units()
    check("the player has units", string.len(units0) > 0, true)

    if case == "fail" then
        -- no device to be had: the program closes at this frame's end (the runner reads the log for it)
        check("the recovery that fails is asked for", moho64ui_RecoverDevice("fail"), true)
        LOG("TEST done: " .. failed .. " failed")
        return
    end

    -- 1. a recovery while the game runs
    local tick0, time0 = GameTick(), GetGameTimeSeconds()
    check("a recovery is asked for", moho64ui_RecoverDevice(), true)
    check("and stands asked until the frame's end", state().pending, true)
    check("it is done", waitFor(180, function() return (state().recoveries or 0) >= 1 end), true)
    local last = state().last or {}
    LOG("TEST note: the recovery: " .. describeLast(last))
    check("the recovery: ok, a check (nothing was lost), one picture: the game's",
        tostring(last.ok) .. " " .. tostring(last.wasLost) .. " " .. tostring(last.mode) .. " " .. table.getn(last.pictures or {}) .. " " ..
        tostring((last.pictures or {})[1] and last.pictures[1].name), "true false check 1 " .. pictureName)
    check("its map was opened again, the game's looks carried over", (last.pictures or {})[1] and last.pictures[1].world and last.pictures[1].looks > 0, true)
    check("no picture is lost afterwards, nothing pending, not closing",
        tostring(state().lost) .. " " .. tostring(state().pending) .. " " .. tostring(state().closing), "false false false")
    -- (no frame is drawn between the recovery and here unless the game's own scripts asked the picture for a place:
    -- the Photographic Look's grade and the map's edge are given a world only by this program's recovery, not by the
    -- next scene)
    local world1 = state().world or {}
    check("after: the new world has the Photographic Look again (given on a change only: the recovery's own)", world1.photoLook, true)
    check("after: the map's edge again (the game's sky, the recovery's own)",
        tostring(world1.frame) .. " " .. tostring(world1.worldBeyond) .. " " .. tostring(world1.mapSurround), "false true true")
    check("after: the new world has the player's looks", looksOnWorld(world1), "")
    check("after: every switch as before", switches(world1), line0)
    draw()
    WaitFrames(2)
    check("after a frame: every switch still as before", switches(state().world), line0)
    local stats1 = state().stats or {}
    LOG(string.format("TEST note: the last frame's models before %d, after %d; draw calls %d, %d", stats0.models or -1, stats1.models or -1,
        stats0.drawCalls or -1, stats1.drawCalls or -1))
    check("after: the world is drawn again (models in the frame, at least half as many as before)", (stats1.models or 0) * 2 >= (stats0.models or 0) and (stats1.models or 0) > 0, true)
    check("the selection is kept", table.getn(GetSelectedUnits() or {}) == 1 and GetSelectedUnits()[1] == cdr, true)
    check("the same units are there", units(), units0)
    WaitSeconds(2)
    check("the game's time goes on across it", GameTick() >= tick0 + 15 and GetGameTimeSeconds() > time0 + 1.5, true)
    LOG(string.format("TEST note: tick %d before the recovery, %d two seconds after it", tick0, GameTick()))

    -- 2. the game paused: the picture before, two recoveries right after each other, the picture after
    SessionRequestPause()
    WaitSeconds(1)
    draw()
    WaitFrames(3)
    local pausedAt = GameTick()
    local before = windowed and {} or sample()
    if not windowed then check("paused: the world view's picture is there (most of 40 points lit)", lit(before) >= 30, true) end
    check("a second recovery", recover(), true)
    check("and a third right after it", recover(), true)
    check("three done, the last ok", tostring(state().recoveries) .. " " .. tostring(state().last.ok), "3 true")
    check("after two more: the Photographic Look and every switch as before", tostring(state().world.photoLook) .. " " .. switches(state().world), "true " .. line0)
    draw()
    WaitFrames(3)
    draw()  -- (a few frames: the clouds, the light in the air and the exposure settle again)
    WaitFrames(3)
    if not windowed then
        local after = sample()
        local same = alike(before, after, 48)
        LOG(string.format("TEST note: the picture after two recoveries: %d of 40 points lit (%d before), %d within 48 of 255 of the picture before, %d within 16",
            lit(after), lit(before), same, alike(before, after, 16)))
        check("the world view's picture is drawn again (as many points lit, within 4)", math.abs(lit(after) - lit(before)) <= 4, true)
        check("and is loosely the picture before (at least 24 of 40 points within 48 of 255)", same >= 24, true)
    else
        check("window: the world is drawn again (models in the last frame)", ((state().stats or {}).models or 0) > 0, true)
    end
    check("the game stood still meanwhile (paused)", GameTick(), pausedAt)
    check("the selection is kept still, the same units", tostring(table.getn(GetSelectedUnits() or {}) == 1 and GetSelectedUnits()[1] == cdr) .. " " .. units(), "true " .. units0)
    SessionResume()
    WaitSeconds(2)
    check("resumed: the game's time goes on", GameTick() >= pausedAt + 15, true)

    -- 3. a photo being made when the device goes: the renderer drops it, and the photo mode must not wait for it for ever
    ConExecute("remaster_photo_mode on")
    WaitSeconds(0.5)
    check("the photo mode is open", moho64ui_PhotoMode().open, true)
    check("a photo is begun", moho64ui_PhotoModeTake(), true)
    check("and is being made", moho64ui_PhotoMode().taking, true)
    check("a recovery while it is being made", recover(), true)
    for i = 1, 600 do
        if not moho64ui_PhotoMode().taking then break end
        WaitFrames(1)
    end
    check("the photo was dropped: the photo mode no longer waits for it, and says so",
        tostring(moho64ui_PhotoMode().taking) .. " " .. tostring(moho64ui_PhotoMode().note), "false The photo could not be made")
    check("the next photo is begun, on the new device", moho64ui_PhotoModeTake(), true)
    for i = 1, 3600 do
        if not (moho64ui_PhotoMode().taking or moho64ui_PhotoMode().saving) then break end
        WaitFrames(1)
    end
    LOG("TEST note: the photo after the recovery: " .. tostring(moho64ui_PhotoMode().note))
    check("and is made and saved", string.find(moho64ui_PhotoMode().note or "", "^Saved ") ~= nil, true)
    ConExecute("remaster_photo_mode off")
    WaitSeconds(1)
    check("the photo mode is left", moho64ui_PhotoMode().open, false)
    local left = GameTick()
    WaitSeconds(2)
    check("and the game goes on", GameTick() >= left + 15, true)

    if case == "lose" then
        -- 3. the device really lost: nothing notices until a picture draws a frame; that frame's end recovers
        local done = state().recoveries
        local asked, why = moho64ui_RecoverDevice("lose")
        check("the real loss is asked for (a run with --shader-cache)", tostring(asked) .. " " .. tostring(why), "true nil")
        if asked then
            -- (the frame's end loses it; the next frame that draws the world notices, whoever asks for it: the game's
            -- own scripts ask the picture for places most frames, and here the interface is drawn besides)
            check("the frame that draws notices, and its end makes the pictures anew", waitFor(240, function()
                draw()
                return (state().recoveries or 0) > done
            end), true)
            last = state().last or {}
            LOG("TEST note: the real loss: " .. describeLast(last))
            check("the recovery after the loss: ok, the device was lost, with its reason",
                tostring(last.ok) .. " " .. tostring(last.wasLost) .. " " .. tostring(last.mode) .. " " .. tostring(string.len(last.reason or "") > 0), "true true lose true")
            check("after the loss: the Photographic Look and every switch as before", tostring(state().world.photoLook) .. " " .. switches(state().world), "true " .. line0)
            draw()
            WaitFrames(3)
            local stats2 = state().stats or {}
            check("after the loss: the world is drawn again", (stats2.models or 0) > 0 and (windowed or lit(sample()) >= 30), true)
            check("after the loss: the selection kept, the same units", tostring(table.getn(GetSelectedUnits() or {}) == 1 and GetSelectedUnits()[1] == cdr) .. " " .. units(), "true " .. units0)
            local t = GameTick()
            WaitSeconds(2)
            check("after the loss: the game's time goes on", GameTick() >= t + 15, true)
            check("nothing lost, nothing pending, not closing",
                tostring(state().lost) .. " " .. tostring(state().pending) .. " " .. tostring(state().closing), "false false false")
        end
    else
        local asked, why = moho64ui_RecoverDevice("nonsense")
        check("an unknown mode is refused", tostring(asked) .. " " .. tostring(why), 'false the mode is "check", "lose" or "fail"')
    end
    LOG("TEST done: " .. failed .. " failed")
end)
