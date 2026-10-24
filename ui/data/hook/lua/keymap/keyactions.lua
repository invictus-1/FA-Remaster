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
-- The photo mode, open or closed (ui/maui/worldview.cpp, its panel /moho64ui/lua/photomode.lua): a free camera, the
-- picture's own time of day, weather and lens, photos up to four times the screen's size; Esc leaves it too.
keyActions['remaster_photo_mode'] = {
    action = 'remaster_photo_mode',
    category = 'ui',
}
-- The command palette, open or closed (its panel /moho64ui/lua/palette.lua, the console command remaster_palette:
-- ui/user/keymap.cpp): a search over the console's commands and variables, the key map's actions and the Options
-- dialog's options; Esc or a click outside closes it too.
keyActions['remaster_palette'] = {
    action = 'remaster_palette',
    category = 'ui',
}
-- The replay timeline, shown or hidden (ui/user/replay_timeline.cpp, its bar /moho64ui/lua/timeline.lua): the
-- replay's length, its moments and jumps to any point; only while a replay plays.
keyActions['remaster_timeline'] = {
    action = 'remaster_timeline',
    category = 'ui',
}
-- The follow camera, open or closed (its frame /moho64ui/lua/followcam.lua, its camera ui/maui/worldview.cpp): a small
-- picture in a corner that follows the first selected unit; a left click on it jumps the main camera there.
keyActions['remaster_follow_cam'] = {
    action = 'remaster_follow_cam',
    category = 'camera',
}
