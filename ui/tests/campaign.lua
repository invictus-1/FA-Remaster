-- An operation of the campaign, started as the briefing's Launch button starts it
-- (operationbriefing.lua: MapUtil.LoadScenario, SinglePlayerLaunch.SetupCampaignSession,
-- LaunchSinglePlayerSession), then looked at from its interface: a single-player session of the
-- campaign with the player's army first and the others the computer's, and the operation's own
-- playable area. The same file serves both states:
--   moho64ui --stage frontend --lua ui/tests/campaign.lua --game-lua ui/tests/campaign.lua --frames 1500 ...
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end
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
    if failed > 0 then LOG("TEST done: " .. failed .. " failed (in the front end)") end
else
    -- the operation opens with a cinematic (Sync.NISMode 'on' in the first sync): the interface
    -- goes into its cinematic mode (gamemain.NISMode, which needs the main world view made first)
    check("the opening cinematic switches the strategic icons off", moho64ui_Looks().strategicIcons, false)
    check("and the selection marks", moho64ui_Looks().selectionMarks, false)
    ForkThread(function()
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
        LOG("TEST done: " .. failed .. " failed")
    end)
end
