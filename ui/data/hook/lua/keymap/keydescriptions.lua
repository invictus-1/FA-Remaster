-- The program's own key actions' names in the game's key binding screen.
keyDescriptions['remaster_stats'] = 'Show performance statistics'
keyDescriptions['remaster_cinecam'] = 'Cinematic camera in replays (on / off)'
keyDescriptions['remaster_photo'] = 'Take a photo of the world at twice the screen size'
keyDescriptions['remaster_photo_mode'] = 'Photo mode (open / close)'
keyDescriptions['remaster_palette'] = 'Command palette (search commands, key actions and options)'
keyDescriptions['remaster_timeline'] = 'Replay timeline (show / hide)'
keyDescriptions['remaster_bookmark_add'] = 'Replay bookmark: add one at this moment'
keyDescriptions['remaster_bookmark_next'] = 'Replay bookmark: jump to the next one'
keyDescriptions['remaster_bookmark_previous'] = 'Replay bookmark: jump to the one before'
keyDescriptions['remaster_follow_cam'] = 'Follow camera on the selected unit (open / close)'
keyDescriptions['remaster_radial_ping'] = 'Radial ping menu (hold, point, let go)'
-- FAF's own camera spots (/lua/usercamera.lua's SaveCameraPosition0..9 / RestoreCameraPosition0..9: FAF 3839 and 3840
-- give them no names, so the key binding screen and the command palette showed their ids), remembered per map by
-- this program (ui/data/hook/lua/usercamera.lua); only where the game's files leave them without a name.
for remasterSpot = 0, 9 do
    if not keyDescriptions['SaveCameraPosition' .. remasterSpot] then
        keyDescriptions['SaveCameraPosition' .. remasterSpot] = 'Save camera spot ' .. remasterSpot .. ' (remembered for this map)'
    end
    if not keyDescriptions['RestoreCameraPosition' .. remasterSpot] then
        keyDescriptions['RestoreCameraPosition' .. remasterSpot] = 'Go to camera spot ' .. remasterSpot
    end
end
if not keyDescriptions['RestorePreviousCameraPosition'] then
    keyDescriptions['RestorePreviousCameraPosition'] = 'Go back to where the camera was before a camera spot'
end
