# The game's window: what ours adds to the original's

Chris, 2026-11-13: "For those ideas lets add them" (the list of the review of 2026-11-09, section 5 "In the
window"). Five small things, all in `ui/app/window.cpp` (Win32), `ui/app/window_sdl.cpp` (SDL: Linux, Mac),
`ui/app/run_window.cpp` and `ui/app/main.cpp`. None changes a game.

## What the original does (read 2026-11-13, FA exe)

- `CScApp::CreateDevice` 0x8d0370, for a window (Primary Adapter "windowed"): the inside's size is the preferences'
  `Windows.Main.width` x `Windows.Main.height` (0x8d09ee, 0x8d0a32), 1024 x 768 without them (0xf57878, 0xf5787c);
  maximized when `Windows.Main.maximized` says so (0x8d0af5); the corner is the command line's `/position x y`
  (0x8d0b7e), else `Windows.Main.x` / `Windows.Main.y` (0x8d0bdc, 0x8d0c2b), put into the rectangle of the screen
  nearest to it (MonitorFromPoint, GetMonitorInfo: 0x8d0c7f..0x8d0cdd).
- The frame's own handler 0x8cdd40 writes `Windows.Main.width`, `height` and `maximized` when a window changes size.
  Nothing in the exe writes `Windows.Main.x` or `y`: the original remembers how large its window was and whether it
  was maximized, not where it stood.
- `Windows.Main.Previous.width` / `height` (0x8d34d0): the window's size kept when the Options go to a full screen,
  for the way back (ours since October).
- Its smallest screen mode in the lists is 1024 x 768. No smallest size for a dragged window was looked for.
- Mouse buttons 4 and 5 and Alt+Enter: its window library hands neither on (not read further: no handler names them).
  FAF's key names do have Windows' codes 05 and 06 as "XButton1" and "XButton2" (`/lua/keymap/keynames.lua`).

## Ours

| What | How | Where |
|---|---|---|
| A smallest window | A framed window cannot be dragged or set smaller than 1024 x 768 inside (WM_GETMINMAXINFO; SDL_SetWindowMinimumSize). A run that asks for a smaller one on the command line (tests) keeps its own size as the limit. A window from the preferences opens at 1024 x 768 at least | `Window::minWidth`, `minHeight`; RunWindow sets them |
| The window's icon | The game's own, read at the start from the game's program file beside the init file (`ForgedAlliance.exe`, else `SupremeCommander.exe`): its first icon group, in the two sizes Windows shows (title bar, taskbar, Alt+Tab). Nothing of the game is in ours. None found: Windows' plain icon, as before. Not on SDL | `Window::SetIconFromExe`, `WindowHooks::iconFrom` |
| Alt+Enter | The other display mode, as if chosen as the Options' Primary Adapter: from a window to a full screen on the screen the window is on, from a full screen to a window where and as large as it last was. The profile's option follows (so the Options show what is, and the next start opens the same way), unless the command line chose the mode. The key goes to no script (a binding of Alt-Enter never runs) | `Callbacks::toggleFullScreen`, `TogglePrimaryMode` (keymap.cpp) |
| A remembered place | A live window whose mode the command line left free opens with the preferences' `Windows.Main.*`: size, maximized, and the corner when both x and y are there (kept on a screen: the whole window inside the work area of the nearest one). They are written when the window goes to a full screen and when it closes: size and maximized as the original, the corner ours | `DisplayMode::placeKept`, `ReadWindowPlace`, RunWindow's `keepPlace` |
| Mouse buttons 4 and 5 | Key events with Windows' codes 5 and 6, so the key bindings dialog can give them an action as "XButton1" / "XButton2" (with Ctrl, Alt, Shift too). No action is bound at first | window.cpp `WM_XBUTTONDOWN`, window_sdl.cpp |

A window that was maximized, sent to a full screen and brought back is maximized again; its plain size and corner
are kept beside (`Window::Windowed`).

## Tested

- `ui/tests/lobby.lua`: the place read from the preferences (no keys, size + maximized, a corner left of the main
  screen); Alt+Enter both ways with the profile's option following.
- `ui/tests/keys.lua`: bindings on XButton1 and Ctrl-XButton2 run from key codes 5 and 6.
- In a real window: the lab's window check `wc_window` (`ui/tests/window_live.lua`, three runs on one preferences
  copy, about 25 s on Chris's screen): 1024x768 kept when 640x480 is set, the icon there, the two buttons' messages
  running their bindings, Alt+Enter to a full screen and back to the same place, a maximized window maximized again
  after it, the place taken up by the next start. Its hooks: `moho64ui_WindowProbe()`, `moho64ui_WindowPoke(what)`
  (window.cpp); in a run without a window `moho64ui_WindowPlaceRead()`, `moho64ui_AltEnter(toFull, screen)`.
- Left to Chris's own hand: a drag of the frame, the icon's look in the taskbar, a real mouse's side buttons.
- SDL (Linux, Mac): written, not run: no such PC here (docs/ui-platforms.md).
