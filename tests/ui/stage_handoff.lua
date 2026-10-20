local framework = import('/testing/framework.lua')
if not SessionIsActive() then
    WaitFrames(5)
    SetPreference('framework_stage_ready', true)
    SavePreferences()
else
    local test = framework.suite('stage handoff')
    test:equal('preparation completed before the game state was created', GetPreference('framework_stage_ready', false), true)
    test:finish()
end
