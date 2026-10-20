local game_root = assert(os.getenv('FA_GAME_ROOT'), 'FA_GAME_ROOT must point to your stock game installation')
path = {
    {dir = game_root .. '/gamedata/*.scd', mountpoint = '/'},
    {dir = game_root, mountpoint = '/'},
}
hook = {'/schook'}
