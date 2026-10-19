-- The lobby test map's own options (FAF's maputil.lua LoadScenarioOptionsFile reads <map>_options.lua), in
-- both forms maps write them: values with keys, and plain values with one text for all.
options = {
    {
        default = 1,
        label = "Test: supply drops",
        help = "How many supply drops the test map would make.",
        key = 'lobby_test_drops',
        pref = 'lobby_test_drops',
        values = {
            { text = "none", help = "No supply drops.", key = 1, },
            { text = "few", help = "A few supply drops.", key = 2, },
            { text = "many", help = "Many supply drops.", key = 3, },
        },
    },
    {
        default = 2,
        label = "Test: waves",
        help = "How many waves the test map would send.",
        key = 'lobby_test_waves',
        pref = 'lobby_test_waves',
        value_text = "%s",
        value_help = "Waves to send.",
        values = { '1', '2', '3', '4', },
    },
}
