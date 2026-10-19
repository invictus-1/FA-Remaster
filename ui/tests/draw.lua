-- A game that ends in a draw, as FAF's victory rules make one: every army still in the game offers a
-- draw (the diplomacy panel's "Draw" box sends SimCallback SetOfferDraw; the victory condition's
-- RemainingBrainsForfeit then calls DrawForArmy for each, /lua/sim/victorycondition/), the game's
-- interface script says "It's a draw." and of the other army "<name> receives a draw.", the game ends
-- by itself and stands, and its Score button brings up the score screen (titled "Defeat": FAF's
-- gameresult.lua calls it with victory = (result == 'victory')).
-- With /drawcase together, the case the rules do NOT call a draw: both commanders destroyed in the same
-- beat. Each army is then without a commander, so each is defeated ("defeat -10"), nobody wins, and the
-- game ends ("All players are defeated"): the player hears "You have been defeated!".
-- Run it like victory.lua (a launched game with cheats on: the opponent's offer is made for it, which
-- OkayToMessWithArmy allows only with cheats, and the commanders are killed in the simulation through
-- ExecLuaInSim('dofile', 'ui/tests/draw_sim.lua'), from the program's working folder, the worktree),
-- with --frames 2400 [-- /drawcase together].
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
local case = (GetCommandLineArg("/drawcase", 1) or {})[1] or "offer"
local UIUtil = import("/lua/ui/uiutil.lua")
local texts = {}  -- every text the scripts make from here on (the Score and Yes buttons' labels among them)
local oldCreateText = UIUtil.CreateText
UIUtil.CreateText = function(parent, label, size, font, dropshadow)
    local t = oldCreateText(parent, label, size, font, dropshadow)
    table.insert(texts, t)
    return t
end
local function newestText(label)
    for i = table.getn(texts), 1, -1 do
        local t = texts[i]
        if not IsDestroyed(t) and t:GetText() == label then return t end
    end
end
local function click(c)
    if not c or IsDestroyed(c) then return false end
    local x, y = (c.Left() + c.Right()) / 2, (c.Top() + c.Bottom()) / 2
    moho64ui_MouseMove(x, y)
    WaitSeconds(0.2)
    moho64ui_MouseButton("Left", true, x, y)
    moho64ui_MouseButton("Left", false, x, y)
    WaitSeconds(0.5)
    return true
end
local function waitFor(cond, seconds)
    local waited = 0
    while not cond() and waited < seconds do
        WaitSeconds(0.5)
        waited = waited + 0.5
    end
    return cond()
end
ForkThread(function()
    WaitSeconds(8)
    LOG("TEST case " .. case)
    local results, announced, armyAnnounced = {}, nil, {}
    local gameresult = import("/lua/ui/game/gameresult.lua")
    local oldResult = gameresult.DoGameResult
    gameresult.DoGameResult = function(army, result)
        table.insert(results, army .. " " .. result)
        return oldResult(army, result)
    end
    local tabs = import("/lua/ui/game/tabs.lua")
    local oldAnnounce = tabs.TabAnnouncement
    tabs.TabAnnouncement = function(tab, text)
        announced = text
        return oldAnnounce(tab, text)
    end
    local scoreBoard = import("/lua/ui/game/score.lua")
    local oldArmyAnnounce = scoreBoard.ArmyAnnounce
    scoreBoard.ArmyAnnounce = function(army, text)
        armyAnnounced[army] = text
        return oldArmyAnnounce(army, text)
    end
    local me = GetFocusArmy()
    local other = me == 1 and 2 or 1
    local otherName = GetArmiesTable().armiesTable[other].nickname
    local cdr = GetArmyAvatars()[1]
    check("the game runs", SessionIsGameOver(), false)

    if case == "together" then
        ExecLuaInSim("dofile", "ui/tests/draw_sim.lua")
        WaitSeconds(1)
        check("the player's commander is killed", cdr:IsDead(), true)
        waitFor(function() return table.getn(results) >= 2 end, 20)
        table.sort(results)
        check("both armies are defeated", table.concat(results, ", "), "1 defeat -10, 2 defeat -10")
        check("the game's script tells the player", announced, LOC("<LOC GAMERESULT_0001>You have been defeated!"))
        check("and of the other army", armyAnnounced[other], LOCF("<LOC usersync_0002>%s has been defeated!", otherName))
    else
        SimCallback({Func = "SetOfferDraw", Args = {Army = me, Value = true}})
        WaitSeconds(3)
        check("the player's offer alone ends nothing", table.getn(results) .. " " .. tostring(SessionIsGameOver()), "0 false")
        -- the opponent's offer (an AI makes none: with cheats on the player may make it for it)
        SimCallback({Func = "SetOfferDraw", Args = {Army = other, Value = true}})
        waitFor(function() return table.getn(results) >= 2 end, 10)
        table.sort(results)
        check("both armies receive a draw", table.concat(results, ", "), "1 draw 0, 2 draw 0")
        check("the game's script says it is a draw", announced, LOC("<LOC GAMERESULT_0002>It's a draw."))
        check("and that the other army receives a draw", armyAnnounced[other], LOCF("<LOC usersync_0003>%s receives a draw.", otherName))
        check("the commanders live", cdr:IsDead(), false)
    end

    check("the game ends by itself", waitFor(SessionIsGameOver, 15), true)
    WaitSeconds(1)
    local stood = GetGameTimeSeconds()
    WaitSeconds(1)
    check("and stands", GetGameTimeSeconds(), stood)
    local final = import("/lua/ui/dialogs/hotstats.lua").scoreData
    check("with the final score handed over", final ~= nil and final.current ~= nil and final.history ~= nil, true)

    -- the end as the game shows it: the Score button the script put up, its question, the score screen
    check("the Score button is up", click(newestText(LOC("<LOC _Score>"))), true)
    check("it asks first", click(newestText(LOC("<LOC _Yes>"))), true)
    WaitSeconds(3)
    local score = import("/lua/ui/dialogs/score.lua")
    check("the score screen is up", score.scoreScreenActive, true)
    check("titled as the script titles a game not won", newestText(LOC("<LOC SCORE_0047>Defeat")) ~= nil, true)
    LOG("TEST done: " .. failed .. " failed")
end)
