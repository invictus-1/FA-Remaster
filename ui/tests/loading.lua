local framework = import('/testing/framework.lua')
-- A launched game loads as the original's does: WorldIsLoading() is true from the launch call
-- until the world is there; the game's loading screen comes up the frame after the launch, in
-- the front end's own state (uimain.SetupUI, uimain.StartGameUI, provider:StartLoadingDialog);
-- headless, the world is then loaded and the loading screen has one frame before the game.
--   moho64ui_test --init <init.lua> --stage setup --lua ui/tests/loading.lua --frames 60 --log out.log
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local test = framework.suite('loading')
local check = function(what, got, want) return test:equal(what, got, want) end

-- what the engine calls in this state while the loading screen comes up
local calls = {}
local uimain = import("/lua/ui/uimain.lua")
local setupUI, startGameUI = uimain.SetupUI, uimain.StartGameUI
uimain.SetupUI = function()
    table.insert(calls, "SetupUI")
    setupUI()
end
uimain.StartGameUI = function()
    table.insert(calls, "StartGameUI")
    startGameUI()
end

test:fork(function()
    WaitFrames(2)
    check("before a launch the world is not loading", WorldIsLoading(), false)
    local stateBefore = GetCurrentUIState()
    local LobbyComm = import("/lua/ui/lobby/lobbyComm.lua")
    local lobby = LobbyComm.CreateLobbyComm("None", 0, "Tester", nil, nil)
    lobby:HostGame()
    local heard = {}
    lobby.LaunchFailed = function(self, reasonKey) table.insert(heard, "failed:" .. tostring(reasonKey)) end
    lobby.GameLaunched = function(self) table.insert(heard, "launched") end
    lobby:LaunchGame({
        GameOptions = { ScenarioFile = "/maps/scmp_009/scmp_009_scenario.lua", FogOfWar = "explored", Victory = "demoralization",
                        CheatsEnabled = "false", GameSpeed = "normal", UnitCap = "1000", Share = "ShareUntilDeath" },
        PlayerOptions = {
            [1] = { PlayerName = "Tester", Human = true, Faction = 1, StartSpot = 1, Team = 1, PlayerColor = 1, ArmyColor = 1,
                    OwnerID = "0", AIPersonality = "", Civilian = false, ObserverListIndex = -1 },
            [2] = { PlayerName = "AI", Human = false, Faction = 3, StartSpot = 2, Team = 1, PlayerColor = 2, ArmyColor = 2,
                    OwnerID = "0", AIPersonality = "medium", Civilian = false, ObserverListIndex = -1 },
        },
        Observers = {}, GameMods = {},
    })
    check("the launch is taken", table.concat(heard, " "), "launched")
    check("from the launch call the world is loading", WorldIsLoading(), true)
    check("the interface's state is still the front end's until the next frame", GetCurrentUIState(), stateBefore)
    check("nothing of the game is called in the launch call itself", table.getn(calls), 0)
    WaitFrames(1)
    -- the frame after: the loading screen, in this state
    check("the engine ran SetupUI, then StartGameUI, in this state", table.concat(calls, " "), "SetupUI StartGameUI")
    check("the interface is in the game's mode now", GetCurrentUIState(), "game")
    check("the world is still loading while the loading screen shows", WorldIsLoading(), true)
    local _, _, _, hidden = moho64ui_Cursor()
    check("the loading screen hides the pointer (FAF's StartLoadingDialog ran here)", hidden, true)
    test:finish()
end)
