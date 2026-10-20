local framework = import('/testing/framework.lua')
-- An operation of the campaign, started as the briefing's Launch button starts it
-- (operationbriefing.lua: MapUtil.LoadScenario, SinglePlayerLaunch.SetupCampaignSession,
-- LaunchSinglePlayerSession), then looked at from its interface: a single-player session of the
-- campaign with the player's army first and the others the computer's, and the operation's own
-- playable area. The same file serves both states:
--   moho64ui_test --stage frontend --lua ui/tests/campaign.lua --game-lua ui/tests/campaign.lua --frames 7200 ...
local test = framework.suite('campaign')
local check = function(what, got, want) return test:equal(what, got, want) end
local file = "/maps/X1CA_001/X1CA_001_scenario.lua"
if not rawget(_G, "SessionIsActive") or not SessionIsActive() then
    local scenario = import("/lua/ui/maputil.lua").LoadScenario(file)
    check("the operation's scenario loads", scenario ~= nil, true)
    local info = import("/lua/singleplayerlaunch.lua").SetupCampaignSession(scenario, 2, 1,
        {opKey = "X1CA_001", campaignID = "uef", difficulty = 2}, false)
    local ok, err = pcall(LaunchSinglePlayerSession, info)
    if not ok then LOG("TEST note: " .. tostring(err)) end
    check("LaunchSinglePlayerSession starts it", ok, true)
    check("the scenario's Options name its file", info.scenarioInfo.Options.ScenarioFile, file)
    local again, why = pcall(LaunchSinglePlayerSession, info)
    check("a second launch while it runs is an error", again, false)
    LOG("TEST note: " .. tostring(why))
    test:checkpoint()
else
    -- the operation opens with a cinematic (Sync.NISMode 'on' in the first sync): the interface
    -- goes into its cinematic mode (gamemain.NISMode, which needs the main world view made first)
    check("the opening cinematic switches the strategic icons off", moho64ui_Looks().strategicIcons, false)
    check("and the selection marks", moho64ui_Looks().selectionMarks, false)
    -- the setup's sync (Sync.CampaignMode) came before the screen was made: the objectives panel
    check("campaign mode known as the screen was made: the objectives panel is there", import("/lua/ui/game/objectives2.lua").controls.objectiveContainer ~= nil, true)
    -- the faction choice the operation asks for (factionselect.lua): its UEF button, caught as made
    local UIUtil = import("/lua/ui/uiutil.lua")
    local makeButton = UIUtil.CreateButton
    local uef = nil
    UIUtil.CreateButton = function(parent, up, a, b, c, d, e, f)
        local button = makeButton(parent, up, a, b, c, d, e, f)
        if type(up) == "string" and string.find(up, "logo-uef_btn_up", 1, true) then uef = button end
        return button
    end
    test:fork(function()
        check("a session is active and it is no replay", tostring(SessionIsActive()) .. tostring(SessionIsReplay()), "truefalse")
        check("the player's army is the first", GetFocusArmy(), 1)
        check("the player is no observer", IsObserver(), false)
        local armies = GetArmiesTable()
        LOG("TEST note: " .. armies.numArmies .. " armies")
        check("the operation's armies", armies.numArmies, 5)
        local me, others = armies.armiesTable[1], true
        for i = 2, armies.numArmies do others = others and not armies.armiesTable[i].human end
        check("the player's army is human, the others are not", tostring(me.human) .. tostring(others), "truetrue")
        local scenario = SessionGetScenarioInfo()
        check("a campaign's scenario", scenario.type, "campaign")
        check("its difficulty", scenario.Options.Difficulty, 2)
        check("the campaign's own information comes along", scenario.campaignInfo and scenario.campaignInfo.opKey, "X1CA_001")
        WaitSeconds(6)
        LOG("TEST note: the playable area " .. tostring(scenario.PlayableAreaWidth) .. " x " .. tostring(scenario.PlayableAreaHeight)
            .. " of " .. scenario.size[1] .. " x " .. scenario.size[2])
        check("the operation's start sets a smaller playable area", (scenario.PlayableAreaWidth or 0) > 0 and scenario.PlayableAreaWidth < scenario.size[1], true)
        -- the intro's first part (camera moves answered with OnCameraFinish, talking heads with
        -- OnMovieFinished) ends asking for a faction
        local waited = 0
        while not uef and waited < 90 do
            WaitSeconds(1)
            waited = waited + 1
        end
        LOG("TEST note: the faction choice after " .. waited .. " s more, game time " .. string.format("%.1f", GetGameTimeSeconds()))
        check("the opening cinematic runs to the faction choice", uef ~= nil, true)
        UIUtil.CreateButton = makeButton
        if uef then uef.OnClick(uef) end
        -- its second part: the commander gates in, the cinematic ends, the first mission begins
        waited = 0
        while not moho64ui_Looks().strategicIcons and waited < 60 do
            WaitSeconds(1)
            waited = waited + 1
        end
        LOG("TEST note: the cinematic's end after " .. waited .. " s more, game time " .. string.format("%.1f", GetGameTimeSeconds()))
        check("choosing UEF runs the cinematic to its end (the icons are back)", moho64ui_Looks().strategicIcons, true)
        check("the player's commander is there", GetArmyAvatars() ~= nil, true)
        WaitSeconds(5)
        local items = 0
        for _ in import("/lua/ui/game/objectives2.lua").controls.objItems or {} do items = items + 1 end
        LOG("TEST note: " .. items .. " objective(s) shown")
        check("the first mission's objectives are shown", items > 0, true)
        -- the operation's end as the simulation hands it (ScenarioFramework.EndOperation ->
        -- Sync.OperationComplete): the profile notes it (the next operation unlocks) and the next
        -- operation's briefing waits in the menu (our campaignmanager hook), then FAF's dialog
        local opData = import("/maps/X1CA_001/X1CA_001_operation.lua").operationData
        Sync = {OperationComplete = {success = true, difficulty = 2, allPrimary = true, allSecondary = false,
                                     allBonus = false, faction = "uef", opData = opData}}
        local ended, why = pcall(OnSync)
        if not ended then LOG("TEST note: " .. tostring(why)) end
        check("the operation's end reaches the interface", ended, true)
        local camp = import("/lua/user/prefs.lua").GetFromCurrentProfile("campaign") or {}
        local noted = camp.uef and camp.uef.X1CA_001 and camp.uef.X1CA_001[2]
        check("the profile notes the operation won (all primary objectives)", noted and noted.allPrimary, true)
        local nextOp = GetFrontEndData("NextOpBriefing")
        check("the next operation's briefing waits in the menu", type(nextOp) == "table" and nextOp.opID, "X1CA_002")
        test:finish()
    end)
end
