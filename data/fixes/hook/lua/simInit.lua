
-- FA Remastered, the engine's script fixes (docs/deviations.md, "ScenarioInfo.HumanPlayers"). Joined to the
-- end of /lua/simInit.lua when Sim::fixes is on; never in the playback of the original's replays.
--
-- FAF's cinematics.lua (SetInvincible, lines 49 and 53) goes through ScenarioInfo.HumanPlayers, a list of
-- army indices that only FAF's co-op launch makes. A campaign operation started on its own has none, so the
-- first cinematic that ends stops there with an error (X1CA_001: IntroNISPart2, x1ca_001_script.lua:549,
-- and the operation never starts its first mission). The stock game's cinematics.lua named army 1 there.
-- Made here as the session begins, when nobody made it: the armies whose ArmySetup entry says Human, in army
-- order (the armies SimObjectives.lua picks for 'HumanPlayers').
do
    local BeginSessionUnfixed = BeginSession
    function BeginSession()
        if not ScenarioInfo.HumanPlayers then
            local humans = {}
            for index, name in ipairs(ListArmies()) do
                local setup = ScenarioInfo.ArmySetup[name]
                if setup and setup.Human then
                    table.insert(humans, setup.ArmyIndex or index)
                end
            end
            ScenarioInfo.HumanPlayers = humans
        end
        return BeginSessionUnfixed()
    end
end
