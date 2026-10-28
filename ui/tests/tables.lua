-- A recorded run's launch tables, one per game (Chris's item 20: game 1's replay.session.lua was overwritten by game
-- 2's): a game launched from the lobby writes <prefix>.session.lua, and the game started over after it gets its own
-- <prefix>.2.session.lua (copied, no lobby launches it), each named in its .seed. Both --lua and --game-lua (the
-- suite's "tables" job checks the files after the run):
--   moho64ui --init <init.lua> --stage frontend --prefs <a COPY> --replay-out <dir>/r --lua ui/tests/tables.lua
--            --game-lua ui/tests/tables.lua --frames 2500 --log out.log
-- Every check logs "TEST ok ..." or "TEST FAIL ..."; the last line is "TEST done: <n> failed".
local failed = 0
local function check(what, got, want)
    if got == want then
        LOG("TEST ok   " .. what)
    else
        failed = failed + 1
        LOG("TEST FAIL " .. what .. ": got " .. tostring(got) .. ", want " .. tostring(want))
    end
end

if GetCurrentUIState() ~= "game" then
    -- the front end: a game launched from a lobby, as loading.lua
    SetPreference("moho64ui_test_tables", false)
    ForkThread(function()
        WaitFrames(2)
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
        check("the lobby's game is launched", table.concat(heard, " "), "launched")
    end)
else
    ForkThread(function()
        WaitSeconds(3)
        if not GetPreference("moho64ui_test_tables", false) then
            check("the first game runs", GetGameTimeSeconds() > 1, true)
            SetPreference("moho64ui_test_tables", true)
            RestartSession()
            return
        end
        check("the second game runs", GetGameTimeSeconds() > 1, true)
        LOG("TEST done: " .. failed .. " failed")
    end)
end
