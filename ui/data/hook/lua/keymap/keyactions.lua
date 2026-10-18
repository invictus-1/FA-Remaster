-- The program's own key actions, beside the game's (their console commands: ui/user/keymap.cpp).
-- The performance statistics over the interface (docs/performance-lessons.md 5.5 and 6).
keyActions['remaster_stats'] = {
    action = 'remaster_stats',
    category = 'ui',
}
-- The cinematic camera for replays, on or off (ui/maui/worldview.cpp: any camera input hands the
-- camera back), and a photo of the world at twice the screen's size (in any game).
keyActions['remaster_cinecam'] = {
    action = 'remaster_cinecam',
    category = 'camera',
}
keyActions['remaster_photo'] = {
    action = 'remaster_photo',
    category = 'ui',
}
