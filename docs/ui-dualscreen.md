# Dual screen (the original's head 1): design

Written 2026-11-08 06:12 (PC clock) by a ui helper, read-only pass. Sources: FAF 3840's Lua (`gamedata3840\lua.nx2`),
the original exe (`bin\ForgedAlliance.exe`, read with capstone + `ghidra\data\fafre_names.tsv`), the ui worktree
(`fa-remaster-ui`, branch ui, e32a12da) and the renderer's worktree (`fa-remaster-render`, branch render, 22d0dea4;
its `render\` headers are newer than the copy in the ui worktree). READ = seen in code; INFERRED / GUESS = not proven.

Status: Chris approved the mode on 2026-11-01 for "later, after the UEF polish" (memory
`supcomlab-dual-screen-later`). His PC: two 3840x2160 screens, main 240 Hz, second 160 Hz.

---

## 1. What the original and FAF do

### 1.1 The exe (READ)

`CScApp::CreateDevice` 0x8d0370 builds a `DeviceContext` (0x8e6d40) and adds `gpg::gal::Head`s (0x8e6d80,
`DeviceContext::AddHead` 0x8e7530). A head's fields as CreateDevice writes them: +0xc full-screen byte, +0x10 width,
+0x14 height, +0x18 refresh rate.

- Two heads come from two places:
  - the command line `/dual`, `/multi` or `/two` (the option names at 0xf5bca8.., set up at 0xbe91d8:
    "dual", "multi", "two", one more not printable by the dump) with 2 or 4 numbers (`func_ValidWindowOption`
    0x8d0160 -> 0x8d04d9..0x8d05fa): both heads full screen; with 2 numbers both get that size, with 4 the second
    pair is head 1's. (Clamped below by the desktop minimum at 0xf57878 / 0xf5787c.)
  - the options (0x8d0712..0x8d08fe): when `secondary_adapter` is not `"disabled"` (0x8d0721..0x8d0755) AND
    `primary_adapter` is not `"windowed"` (0x8d075a..0x8d0793), a second head is added (0x8d07e8), BOTH heads
    are set full screen (`mov byte [esi+0xc],1` / `[ebp+0xc],1` at 0x8d082e / 0x8d0838), head 0 takes its mode
    from `primary_adapter` (0x8d0841, `Resolution` parser 0x8cd6c0), head 1 from `secondary_adapter`
    (0x8d08b3). Otherwise one head (0x8d0903..).
- The device is made once at start-up; the Secondary Adapter option is `restart = true` in FAF's options.lua
  (so a change applies at the next start).
- `StartupHelpers::SetupSecondaryAdapterSettings` 0x8d26d0: "Disabled" + adapter 1's modes when the primary is not
  windowed; `SC_SecondaryAdapter <windowed>` 0x8d37c0 refills it (FAF's options.lua sends it on every primary
  change).
- Root frames: the UI manager's init 0x84cc50 makes one `CMauiFrame` per head (0x84cdfc `CMauiFrame::Create`),
  render pass 8 (`+0xf8 = 8`, 0x84ce94), Left = Top = 0 and Width / Height from THAT head's own size (a virtual at
  +0x198 of the head's object in `[ctx+0x38][i]`, 0x84ceae..0x84cf3d). So each head is its own coordinate space
  starting at 0,0, at its own size.
- Mouse routing: without input capture the target is `GetTopmostControl` (0x7872e0) of the event mapper's own
  root frame (`[edi+0x2c]`, 0x7a4ab2; mapper class `CMauiWxEventMapper`, dtor 0x7a48d0). INFERRED: one event mapper
  per head window, so the mouse over head 1's window hits head 1's frame, in head 1's pixels. With input capture
  the capture stack is global (0x10bdbac, 0x7a4a77), whatever the head.
- Keys: the same mapper hands keys to the focus control / capture stack (0x7a4ef0 etc.), which are global. INFERRED:
  keys work the same whichever head the mouse is on.
- `Moho::CON_UI_CreateHead1Map` 0x834da0: a console command that imports `/lua/ui/game/multihead.lua` and calls
  `CreateSecondView()` (strings 0xe433bc, 0xe433d8). FAF calls the same function itself (below), so the command
  is only a manual way in.
- `sub_8D3870` (the Level Of Detail option) sends `cam_SetLOD WorldCamera`, `cam_SetLOD WorldCamera2` and
  `cam_SetLOD CameraHead2` (0xe4f058 / 0xe4f070 / 0xe4f08c): head 1's camera gets the same LOD scale.
- GUESS: the original's gal layer made a D3D9 adapter-group device (strings "invalid head count specified in device
  context" 0x8f2080 / 0x8ff5b0, "invalid primary adapter index" 0x8f1480): two exclusive full-screen heads of one
  card, which is why it allows a second head only in full screen. Not read further; it does not change our design
  (we use two borderless windows, see 3).

### 1.2 FAF's Lua (READ, FAF 3840 `lua.nx2`)

| Where | What |
|---|---|
| `lua/ui/game/multihead.lua:37-46` `ShowLogoInHead1()` | if `GetNumRootFrames() >= 2`: a `Bitmap` of `/marketing/splash.dds` on `GetFrame(1)`, `FillParentPreserveAspectRatio` |
| `lua/ui/game/multihead.lua:51-65` `CreateSecondView()` ("referenced directly by the engine") | `ClearFrame(1)`; a Group filling `GetFrame(1)`; `WorldView(secondHeadGroup, 'CameraHead2', 1, false)`; `view:Register('CameraHead2', nil, '<LOC map_view_0003>Secondary View', 3)`; `view:SetRenderPass(UIRP_UnderWorld \| UIRP_PostGlow)`; fills the group |
| `lua/ui/uimain.lua:61-63` | front end: `ShowLogoInHead1()` after the main menu |
| `lua/ui/game/gamemain.lua:455-459` | loading: the splash on `GetFrame(1)` |
| `lua/ui/game/gamemain.lua:310-312` | game interface: `CreateSecondView()` when `GetNumRootFrames() > 1` |
| `lua/ui/uiutil.lua:1183-1230` `CreateWorldCover` | covers EVERY root frame (`GetFrame(index)` for 0..n-1) while a dialog is up |
| `lua/ui/controls/popups/popup.lua:30`, `dialogs/options.lua:299`, `saveload.lua:44`, `replay.lua:31`, `campaign/campaignmovies.lua:65`, `game/worldview.lua:241,257` | `GetFrame(x:GetRootFrame():GetTargetHead())` - dialogs and the split screen go to the root frame's TARGET head |
| `lua/keymap/keyactions.lua:153-156` | `track_unit_second_mon` = `UI_TrackUnit CameraHead2` |
| `lua/options/options.lua:1436-1495` | Primary Adapter (sends `SC_PrimaryAdapter`, `SC_SecondaryAdapter <windowed>`), Secondary Adapter (`default 'disabled'`, `restart = true`, keys `"w,h,rate"`) |
| `lua/user/prefs.lua:59-75` | a new profile copies `primary_adapter` / `secondary_adapter` |
| `lua/ui/uimain.lua:300` | `OnApplicationResize(head, width, height)` (deselects) |

What the Lua needs from the engine for head 1: `GetNumRootFrames()` = 2, `GetFrame(1)` (a root frame of head 1's
size), `ClearFrame(1)`, `Frame:GetTargetHead()`, a `WorldView` on frame 1 drawing the world through the camera
`CameraHead2` (made on first use, like WorldCamera2), its marks (icons, bars, orders), its mouse input (select,
orders, zoom, pan) in head 1's pixels, `cam_SetLOD CameraHead2`, `UI_TrackUnit CameraHead2`, and the world view
manager registration (Lua only). No minimap and no other panels on head 1: FAF puts only the world view there.
Head 1 exists from the start to the end of the program (front end: logo; loading: splash; game: the second view).

---

## 2. What we have today (ui worktree, READ)

| Piece | Where | State |
|---|---|---|
| Screens list | `ui/app/main.cpp:621-654`, `ui/user/user_state.h:509-526` (`DisplayMode`) | every screen of the PC (Win32 / SDL), `MOHO64UI_SCREENS` fakes them |
| Primary Adapter | `main.cpp:677-696`, `run_window.cpp:398-426` | full screen on the chosen screen, live moves; windowed |
| Secondary Adapter list | `ui/user/keymap.cpp:806-821` (`SetupSecondaryAdapterSettings`), `keymap.cpp:1631-1636` (`SC_SecondaryAdapter`) | "Disabled" alone, the other screens only with `MOHO64UI_SECOND_HEAD=1` (`main.cpp:623`, `DisplayMode::secondHeadShown`) |
| The original's head rule | `main.cpp:697-705` | `secondScreen` worked out (not windowed, a different screen) and only LOGGED: "a second head is not shown yet (one root frame)" |
| Root frames | `ui/maui/ui_manager.cpp:87-113` `CreateRootFrames` (one call: one frame), `ui_manager.cpp:115-124` `Resize` (ALL frames one size), `user/user_state.cpp:389` (one call at StartMainScript), `user_state.cpp:321` `SetHeadSize` | one head |
| `GetFrame`, `GetNumRootFrames`, `ClearFrame`, `GetTargetHead`/`SetTargetHead` | `ui_manager.cpp:391-424` | written for N heads already; `Frame::targetHead` is 0 for every frame (`maui/control.h:176`) |
| Drawing a head | `UiManager::Render(L, target, head)` (`control.h:199`) | takes a head; the host only ever passes 0 (`app/run_window.cpp:444, 468`; `main.cpp:1313, 1317`) |
| Mouse | `maui/input.cpp:68-75` `Target` -> `ControlAt(ui, 0, ...)`; `input.cpp:297-345`; `UiEvent` (`input.h:23-31`) has no head; `UiManager::mouseX/mouseY/hover/cursorView` single | head 0 only |
| Keys | `input.cpp:130-140` `KeyEvent`: key map, then focus / capture (global) | fine for 2 heads as is |
| Window | `ui/app/window.h:18-120` "the host's one window"; Win32 `window.cpp:356-397` (`Pump` = `PeekMessageW(nullptr)`: every window of the thread; WndProc finds its `Window` by `GWLP_USERDATA`, so a second `Window` already gets its own callbacks); SDL `window_sdl.cpp:400-` (`Pump` takes every SDL event into THIS window: a second SDL window needs dispatch by `windowID`) | one window |
| Frame loop | `run_window.cpp:184-614` `RunWindow`: one `SoftwareTarget`, one recorder / painter, `hooks.present(target, changed)`; pacing `run_window.cpp:564-567` | one head |
| Focus / mouse lock / edge pan | `window.cpp:119-121` (`WM_ACTIVATE` -> `KeepMouseInside`, `WindowFocus`), `run_window.cpp:427-428` (`edgeScroll = window.fullScreen`, `LockMouse`), `maui/worldview.cpp:2983` | one window |
| The renderer on the window | `app/game_session.cpp:2576-2584` `UseGameWindow` (`UseWindow(hwnd)` / `UseHostSurface`), `game_session.cpp:2818-2870` `PresentToWindow` (`GiveLayer` -> `SetInterfaceLayer`, `DrawToWindow(camera, w, h, moment)`), `main.cpp:1176-1189` hooks | one window, one layer |
| Views of one picture (request 35) | `maui/worldview.cpp:924-1042` (`GatherViews` collects world views of `GetFrame(0)` ONLY, line 985), `worldview.cpp:1088-1106` (`EnsurePicture`: a non-main view goes through the main view; draw size = head 0's), `maui/world_source.h:235-247` (`WorldViewSpec`: no window), `game_session.cpp:415-438` (`Picture::SetViews` -> `WorldPicture::SetViews`) | split + inset on head 0 |
| Cameras | `worldview.cpp:277-295` `GetCameraObject` (any name, made on first use), `worldview.cpp:3297` `cam_SetLOD <camera>`, `UI_TrackUnit <camera>` (worldview.cpp:3298-) | `CameraHead2` works as a name already |
| `OnApplicationResize(head, w, h)` | not called anywhere in ui/*.cpp | missing (minor) |
| Tests | `ui/tests/screens.lua` (lists, headless, `MOHO64UI_SCREENS`), `ui/tests/screens_window.lua` (live window), `ui/tests/views.lua` (split / inset), suite `_scratch/ui/suite/suite.sh:174-187, 366, 387`, `window_checks.sh:26-34` | |

### What is missing on the ui side

1. Head count and head 1's size at start: `DisplayMode` gets `heads` (1 or 2) and head 1's screen and size (the
   secondary screen's full size: a full screen here is always the screen's own size, as for the primary).
2. Root frames per head with their own sizes: `CreateRootFrames(L, sizes)`, `Resize(head, w, h)`; every new
   `UserState` (front end, game, front end again: `main.cpp:591-593`) makes both frames at `StartMainScript`.
3. A second window on the chosen screen (Win32: owned popup, see 3.3), its own callbacks tagged head 1.
4. Mouse routing by head: `UiEvent::head`, `UiManager::mouseHead`, `Target` / `ControlAt` with that head; hover
   moves across heads naturally (MouseExit on head 0's control, MouseEnter on head 1's).
5. Keys: unchanged (global), but window focus must count both windows as "ours" (3.3).
6. Head 1's interface drawn into its own `SoftwareTarget` (recorder + incremental painter per head) and handed to
   the renderer as a second layer.
7. `CameraHead2`'s world view gathered as a view of the ONE picture, tagged for the second window (`GatherViews`
   walks `GetFrame(1)` too; `WorldViewSpec::window`).
8. The option on: Secondary Adapter lists the other screens whenever there are 2+ screens and the renderer can show
   a second window (in place of `MOHO64UI_SECOND_HEAD=1`); applied at the next start (`restart = true`, the
   original's way).
9. One screen only: "Disabled" alone (as the original with one adapter); `GetNumRootFrames()` = 1; nothing else
   changes.
10. Windowed primary: no head 1 (the original's rule, 0x8d075a), except the test mode (5).
11. Alt-tab, mouse lock, edge pan, pointer image on both windows (3.3).
12. `OnApplicationResize(head, w, h)` for head 0 and 1 when a head's size changes.

---

## 3. Design

### 3.1 Shape

One process, one UI thread, one `UiManager` with two root frames, one world (one `WorldPicture`, one device),
two OS windows, two swap chains. The second screen's world is one more VIEW of the same picture (request 35's
machinery: `views.h` - "views of one world share all of that, and only what depends on the camera is done for
each"), drawn into the second window instead of laid into the first. No second WorldPicture (that would step
effects, particles and weather twice and hold two of everything on the card, as views.h:3-5 says).

```
frame:  pump both windows -> UserState::Frame (both frames' OnFrame) -> Render(head 0) -> target0
        -> Render(head 1) -> target1 -> GameSession::PresentToWindow(target0, changed0, target1, changed1)
        renderer: views of window 0 + layer0 -> swap chain 0 (paced, vsync / low latency / frame gen as today)
                  views of window 1 + layer1 -> swap chain 1 (never waited for; see 4.3)
```

### 3.2 Head 1 lifetime and contents

- Made at start when (a) 2+ screens, (b) `secondary_adapter` names a screen other than the primary's, (c) the
  primary is full screen, (d) the renderer's window picture is in use (`directWindow`). Else one head and a log
  line saying which condition failed (today's line at `main.cpp:704` becomes this).
- Test mode (ours): `MOHO64UI_SECOND_WINDOW=WxH` (or `--second-window WxH`) makes head 1 a framed window of that
  size on the primary's screen, also with a windowed primary: for the suite and for a one-screen PC.
- Front end: FAF's `ShowLogoInHead1` puts the splash bitmap on frame 1: drawn by our interface into layer 1, no
  world. Loading: the splash again (gamemain.lua:455). Game: `CreateSecondView` -> world view `CameraHead2`.
  Back to the front end: the new UserState makes frame 1 again; uimain shows the logo again.
- The window lives for the whole run (as the original's device head); the UserStates come and go on it.
- GDI path (`--gdi`, no renderer window): head 1's window gets `Window::Present(target1)` (logo / interface only),
  its world view stays black (one read-back picture only). Acceptable; log it.

### 3.3 The second window (Win32; SDL in 3.6)

- `Window::Create(..., fullScreen=true, on=&secondScreen)` plus two new options: `owner` (head 0's HWND) and
  `noActivate` (`WS_EX_NOACTIVATE`). Owned: it is not in the taskbar or the alt-tab list and rises with head 0;
  no-activate: a click on screen 2 does not take the keyboard away from head 0, so keys keep working while the mouse
  is on head 1 (matches the original's global keys, 1.1). GUESS to try: `SetCapture` during a drag on the
  no-activate window works because the thread owns the foreground window; `WM_MOUSEWHEEL` goes to the focus window
  (head 0), so head 0's WndProc must forward a wheel whose screen point lies in head 1's window (or use
  `WindowFromPoint` in the wheel handler).
- Mouse: head 1's callbacks call `MouseMove/MouseButton/MouseWheel` with `e.head = 1`, coordinates in head 1's
  client pixels (each root frame starts at 0,0, 1.1).
- Pointer: `updatePointer` sets the same interface pointer on both windows.
- Focus: `WindowFocus(true)` while EITHER window is the foreground one (`GetForegroundWindow()` is head 0 or head 1);
  `false` only when neither is (alt-tab away): keys released as today (`input.cpp:158-161`).
- Alt-tab: borderless full-screen windows (our way: `window.cpp:369` WS_POPUP, no exclusive mode, and the renderer
  sets `DXGI_MWA_NO_ALT_ENTER`, gfx_d3d12.cpp:1208) stay on their screens when another program comes to the front;
  nothing is lost or reset. Coming back: activating head 0 raises head 1 with it (owned). (The original with
  exclusive heads probably minimised both: GUESS; ours is kinder and Chris already runs borderless.)
- "Lock Fullscreen Cursor To Window" with two heads: clip to the bounding rectangle of both screens
  (`KeepMouseInside`), so the mouse can cross to head 1 but not to a third screen.
- Edge pan: only while the mouse is on head 0 (`ui.edgeScroll && ui.mouseHead == 0`). Without this, the mouse
  leaving head 0 across the shared edge leaves `mouseX` at the edge and the main view keeps panning. The edge
  toward head 1 cannot be "rested on" anyway (the mouse goes over). Head 1's view does not edge-pan (FAF's option
  is "Screen Edge Pans Main View").
- Screen changes (`WM_DISPLAYCHANGE`): if head 1's screen is gone, hide window 2 and drop its views (the frame
  stays: `GetNumRootFrames` must not change inside a UserState); if the primary is moved by Options onto head 1's
  screen, the same (and log "the second screen comes back at the next start").

### 3.4 Interface (maui) changes

- `UiManager::CreateRootFrames(L, const std::vector<std::pair<int,int>>& sizes)`: frames 0..n-1, each `head = i`,
  pass 8, its own size; `Resize(head, w, h)`; `width(head)/height(head)`.
- `UiEvent::head` (0 default); `UiManager::mouseHead`; `Target()` uses `ControlAt(ui, e.head, ...)` without
  capture, the global capture stack with it (as 0x7a4a77); `mouseX/mouseY` stay "in mouseHead's pixels".
  `moho64ui_MouseMove/MouseButton(..., head)` take an optional head for tests.
- `Frame::targetHead`: keep 0 for every frame (INFERRED: FAF's dialogs then open on head 0 even when called from
  head 1, which is what a player wants; `CreateWorldCover` covers both frames regardless). To verify against the
  exe later: what writes `+targetHead` (read by 0x796a50).
- World views: `GatherViews` also walks `GetFrame(1)`; a world view there becomes a view with `window = 1`, kind
  Split (a full view with its own history), rect in head 1's pixels. `MainView` stays the first view of frame 0;
  head 1's view's `EnsurePicture` goes through it (as the split screen's right view does, `worldview.cpp:1094`).
  The head-1 view's picking uses the existing `Src*` calls by view number (`worldview.cpp:943-976`), already in the
  view's own pixels. `WorldSource::SetViews(specs, w0, h0, w1, h1)`.
- `GameSession::Picture::SetViews` passes `window`; `PresentToWindow` gets the second layer and hands it over;
  `OnApplicationResize(head, w, h)` called for each head's size change.
- `RunWindow`: a second target / recorder / painter for head 1 (head 1 is mostly one world view, so after the first
  frame its layer is nearly empty and the painter's "only where changed" keeps it cheap); `hooks.present` gets both.
- Options: `SetupSecondaryAdapterSettings` lists the other screens when `screens >= 2` and the renderer's second
  window is available; `MOHO64UI_SECOND_HEAD=1` stays for tests (and forces the list). The change applies at the
  next start (FAF shows its restart note because of `restart = true`).

### 3.5 Behaviour table

| Situation | Result |
|---|---|
| One screen | Secondary lists "Disabled" only; 1 root frame |
| 2 screens, Secondary "Disabled" | 1 root frame (as today) |
| 2 screens, Secondary = screen 2, primary full screen on screen 1 | head 1 full screen on screen 2 at its size (3840x2160 @160 on Chris's PC) |
| Primary windowed | no head 1 (the original's rule) |
| Secondary names the primary's screen, or a screen not there | no head 1, logged |
| `MOHO64UI_SECOND_WINDOW=1280x720` | head 1 as a framed window on the primary's screen (tests / one screen) |
| `--gdi` | head 1's interface only, no world in it |
| Alt-tab away / back | both windows stay; keys released; mouse lock off / on |

### 3.6 SDL (Linux / Mac; untestable here)

`Window::Pump` must route each SDL event by `ev.window.windowID` (a static map windowID -> Window), else the second
window's events land in the first. Head 1's surface via `UseSecondHostSurface` (4.2). Mark untested, as the rest of
the SDL path.

---

## 4. What the RENDERER must provide (proposal for ui request 48)

Smallest addition the existing machinery allows: a second swap chain on the same device, and a view (or the
interface layer) that goes to it instead of being laid into the main window. `views.h` already draws every view that
does not fill the window into its own `ViewSide` picture and lays it in with `ViewLayer::Lay`
(`views.h:7-14, 41-95`); here the lay goes into window 2's back buffer instead.

### 4.1 gfx::Device (`render/gfx.h`)

The fixed convention "One window, one swap chain" (`gfx.h:5`) becomes "one main window, plus extra windows":

```cpp
struct WindowDesc {                       // as DeviceDesc's window fields (gfx.h:222-241)
  DeviceDesc::WindowSystem windowSystem;
  void* window = nullptr; void* display = nullptr;
  std::function<uint64_t(void* instance)> hostSurface;   // Host (SDL / Vulkan)
  int width = 0, height = 0;
};
// [head2] An extra window on this device (0 is DeviceDesc's). -1 and `error` when it cannot be made.
virtual int AddWindow(const WindowDesc& desc, std::string* error) { (void)desc; *error = "not written"; return -1; }
virtual void RemoveWindow(int window) {}
virtual void ResizeWindow(int window, int width, int height) {}
// Within a frame: the passes into "the window" from here on go to this window's back buffer (0: the main one).
// For an extra window: false (and nothing drawn) when its swap chain has no buffer free this frame - never waits.
virtual bool SetPassWindow(int window) { return window == 0; }
// EndFrame presents window 0 as today (vsync, latency limiter, frame generation) and every extra window drawn
// this frame without waiting for its screen (see 4.3).
```

- Direct3D 12: a second `CreateSwapChainForHwnd` on the same queue (gfx_d3d12.cpp:1200 does the first), flip model,
  `MakeWindowAssociation(NO_ALT_ENTER)`, its own `ResizeBuffers`; the frame-latency waitable object, checked with a
  0 timeout, answers "buffer free".
- Vulkan: a second `VkSurfaceKHR` from the same instance (the host makes it, as `hostSurface` today), check
  `vkGetPhysicalDeviceSurfaceSupportKHR` for the present queue (gfx_vulkan.cpp:1460), a second swapchain;
  `vkAcquireNextImageKHR` with timeout 0 = "buffer free?"; both swapchains can go in ONE `vkQueuePresentKHR`
  (`VkPresentInfoKHR::swapchainCount = 2`). Present mode for window 2: MAILBOX, else IMMEDIATE, else FIFO.

### 4.2 WorldPicture (`render/world_picture.h`)

```cpp
// [head2] The original's head 1 (FAF's multihead.lua: a world view of camera CameraHead2 on the second screen):
// a second window on this picture's device. Before or after Open; closed with CloseSecondWindow or the picture.
bool UseSecondWindow(void* hwnd, int width, int height);                        // Win32
bool UseSecondHostSurface(std::function<uint64_t(void* vkInstance)> createSurface, int width, int height); // SDL
void CloseSecondWindow();
bool SecondWindowOn() const;
// Its interface layer, exactly as SetInterfaceLayer (world_picture.h:166): premultiplied 0xAARRGGBB, the changed
// part; its width x height is the second window's size (a new size resizes its swap chain).
void SetSecondInterfaceLayer(const uint32_t* pixels, int width, int height, int pitch, int x, int y, int w, int h);
// PictureView (world_picture.h:576-582) gains:
//   int window = 0;   // 0: the main window; 1: the second. rect in that window's pixels.
// DrawToWindow (world_picture.h:180) also draws the views of window 1 into their pictures, lays them and the second
// layer into window 1's back buffer and shows it - only when its swap chain has a buffer free; else window 1's views
// are skipped this frame (nothing drawn for them) and the second window keeps its last picture.
// With no view for window 1 (front end, loading) only the layer (+ black) goes there.
bool SecondWindowDrawnLastFrame() const;                          // (stats, tests)
void CaptureNextSecondWindowFrame(std::vector<uint8_t>* out);     // (checks: as CaptureNextWindowFrame)
```

- `ViewAt(x, y)` (world_picture.h:590) answers for window 0's views only. `ViewProject / ViewGroundAt / ViewUnitAt`
  (598-) work for a window-1 view unchanged (view-local pixels). `ViewLifeBarAnchors(view)` likewise.
- Kind of a window-1 view: Split semantics (a full view, its own temporal / motion / clouds / bounce history:
  views.h `ViewSide`). Frame generation: the rule "a Split view turns frame generation off" (world_picture.h:562-564)
  is about smearing across a line INSIDE window 0, so a window-1 view need not turn it off; frame generation and the
  latency limiter stay window 0's only. (Renderer's call.)
- `SetBackdrop` (the film / cover) stays window 0's; head 1's splash comes through its layer.
- Optional, later (Chris: optimise at the end): `PictureView::every` honoured for window-1 views, or a lower scene
  scale for the second screen, if the cost measured is too high.

### 4.3 Refresh rates: 240 Hz main, 160 Hz second

Window 0 paces the loop as today (vsync / latency limiter / frame generation on swap chain 0). Window 1 must never
make the loop wait: with two vsync'd presents in a row the loop would be held by whichever vblank comes later and
drop toward the slower screen's rate. So window 1 is presented without a vsync wait (flip model, sync interval 0,
no tearing flag: DWM shows its newest picture at screen 2's own 160 Hz; GUESS on exact DXGI behaviour, renderer to
confirm) AND only drawn when a buffer is free (4.1). Expected result: head 1 drawn about 160 times a second (its
buffers free up at its refresh), head 0 at 240; the card does the second view only as often as screen 2 can show
it. Both views show the same moment (`tick`) of the frame they were drawn in.

### 4.4 Cost (to measure, not to chase now)

A second 4K full view is roughly a second scene pass (shadows, ground, models, post) per head-1 frame: GUESS 60-90 %
more card time on a 160 Hz share of the frames. The renderer's frame stats should show it per window (`Stats`
gets `secondMs` or a second `gpuStages` group) so the ui overlay can print it.

---

## 5. Step plan (ui side)

Prerequisite: the renderer's 4.1-4.2 (request 48 in `ui/REQUESTS.md`, SendMessage to renderer). Steps 1-4 can be
built and tested before it lands (headless, test env).

1. `DisplayMode`: `heads`, `secondScreen`, `secondWidth/Height`, test env `MOHO64UI_SECOND_WINDOW=WxH` /
   `--second-window`; `main.cpp:697-705` decides head 1 by the rules in 3.2 and logs the reason when not.
2. `UiManager`: frames per head with own sizes, `Resize(head, ...)`; `UserState::StartMainScript`
   (`user_state.cpp:389`) and `SetHeadSize(head, ...)`; headless switch `MOHO64UI_HEADS=2` +
   `MOHO64UI_HEAD1_SIZE=WxH` (no window) for tests.
3. Input: `UiEvent::head`, `mouseHead`, `Target`/`ControlAt` by head, test functions with a head argument; edge pan
   only on head 0; `OnApplicationResize(head, w, h)`.
4. World views: `GatherViews` over both frames, `WorldViewSpec::window`, `SetViews(specs, w0, h0, w1, h1)`;
   `moho64ui_Views()` reports `window`.
5. Window: second `Window` (owned, no-activate, full screen on screen 2 / framed in test mode), callbacks -> head 1,
   wheel forwarding, focus = either window, mouse lock over both screens, pointer on both; SDL dispatch by windowID.
6. `RunWindow`: second target / recorder / painter, `Render(head 1)`, both layers to `hooks.present`; the overlay
   (remaster_stats) stays on head 0.
7. `GameSession`: `UseSecondWindow` when the window picture opens (`OpenWindowPicture`), `SetSecondInterfaceLayer`
   in `PresentToWindow`, `CloseSecondWindow` at the end; GDI fallback `Window::Present(target1)`.
8. Options: Secondary lists screens when 2+ screens and the renderer's second window works; keep
   `MOHO64UI_SECOND_HEAD=1` for tests; `ui/tests/lobby.lua:323-324` and `screens.lua:85-88` expectations updated.
9. Docs: `ui/README.md` display-mode paragraph (`README.md:332-340`), `HANDOFF-ui.md`, the progress page.

---

## 6. Test plan

All runs follow the lab rules (at most 3 moho64ui, oracle HOLD, no GPU work while Chris plays).

A. Headless suite test `ui/tests/dualhead.lua` (no window, no second monitor; `MOHO64UI_HEADS=2
   MOHO64UI_HEAD1_SIZE=1280x720`, `--size 1920x1080`):
   - front end: `GetNumRootFrames() == 2`; frame 1 is 1280x720 at 0,0; frame 0 still 1920x1080; FAF's
     `ShowLogoInHead1` left a Bitmap on frame 1; `ClearFrame(1)` empties only frame 1.
   - game (`session_ai_cheats.lua`): `import('/lua/ui/game/multihead.lua').view` is a WorldView of camera
     `CameraHead2`, filling frame 1; it is registered in FAF's worldview manager; `moho64ui_Views()` lists a view
     with `window = 1` and head 1's rect; `cam_SetLOD CameraHead2 0.5` reaches that camera
     (`moho64ui_CameraLodScale`); `UI_TrackUnit CameraHead2` with a unit selected moves CameraHead2, not
     WorldCamera.
   - input: `moho64ui_ControlAt(x, y, 1)` is head 1's view; `moho64ui_MouseMove(x, y, 1)` then a left drag on head 1
     selects units under head 1's camera (compare with `GetMouseWorldPos`); a right click on head 1 orders a move to
     the ground under that point; a key press while the mouse is on head 1 still runs its key action; MouseExit /
     MouseEnter fire when the mouse goes from head 0 to head 1; the main view does not edge-pan while the mouse is
     on head 1.
   - `CreateWorldCover` (open the escape dialog) covers both frames; the dialog itself lands on frame 0.
   - one head (no env): `GetNumRootFrames() == 1`, `CreateSecondView` does nothing (as today).
B. `screens.lua` cases: "second" (2 made-up screens) lists screen 2 under Secondary; "pc" with one screen: Disabled
   alone; a kept Secondary choice with a windowed primary logs "no second head: the primary is windowed".
C. Live window check (`window_checks.sh`, renderer build, one screen is enough): `--window --size 1280x720
   MOHO64UI_SECOND_WINDOW=960x540` with a game: two framed windows on the same screen; after N frames
   `CaptureNextWindowFrame` and `CaptureNextSecondWindowFrame` saved as PNGs; checks: window 2 not black in its
   middle, differs from window 1 after CameraHead2 is moved elsewhere (`moho64ui` test function or Lua camera
   `MoveTo`), window 2's size followed after a resize; the log's frame lines show window 1's rate unchanged and
   `SecondWindowDrawnLastFrame` true in most frames.
D. Pacing check (live, renderer): window 1 vsync on; head 1 drawn about as often as its screen's rate (logged
   counts per second); window 1's frame-to-frame times not worse than without head 1 beyond the measured card cost.
E. Chris's own look (two 4K screens, 240 / 160 Hz): goes on the ONE playtest list (memory `supcomlab-playtests-wait`),
   not asked separately: Secondary Adapter -> screen 2, restart, menus show the logo on screen 2, a game shows the
   second view, mouse across both, orders from screen 2, alt-tab and back, frame rate on both screens.

---

## 7. Open points (to settle while building)

- What sets `Frame::targetHead` in the original (read 0x796a50's field and its writers); we keep 0.
- The fourth `/dual` alias name (0xbe924b) and whether to accept `/dual W H [W H]` on our command line (cheap; the
  lab's launchers do not use it).
- Wheel on a no-activate window (3.3): forward from head 0's WndProc or hit-test by screen point; try both.
- DXGI behaviour of a sync-interval-0 present on a 160 Hz screen while another chain paces at 240 Hz (4.3): the
  renderer to confirm with his frame statistics.
- Whether head 1 should also get FAF's minimap or other panels: FAF puts none there; keep as FAF (Chris's "original's
  way included"; a richer head 1 could be a later player option).

---

## 8. What is built (2026-11-08, ui helper "dualhead"; steps 1-5 and 8 of section 5)

- `DisplayMode::heads`, `secondWidth/secondHeight`, `secondWindowTest` (user_state.h); main.cpp's head rule (3.2)
  decides once at the start and logs `no second head: <why>` (the Secondary Adapter names no screen / the primary is
  windowed / the PC has one screen / its screen is not there / it names the primary's own screen / there is no
  window). Test switches: `--second-window WxH` or `MOHO64UI_SECOND_WINDOW=WxH` (a framed window on the primary's
  screen, with a window only); headless `MOHO64UI_HEADS=2` + `MOHO64UI_HEAD1_SIZE=WxH` (no window). Condition (d)
  of 3.2 (the renderer's window picture) is NOT applied: without the renderer's second window head 1 shows its
  interface by the plain copy anyway.
- `UiManager::CreateRootFrames(L, sizes)` (a frame per head at its own size), `Resize(head, w, h)`,
  `width(head)/height(head)`, `OnResize(L, head, w, h)` = the frame, then `uimain.OnApplicationResize(head, w, h)`
  (READ: IUIManager::OnResize 0x84cfc0 -> CMauiFrame::SetBounds 0x796550, MAUI_OnApplicationResize 0x83d9c0); the
  window's resize and `moho64ui_Resize(w, h [, head])` go through it. Every UserState makes `Config::heads` frames.
- Input: `UiEvent::head`, `UiManager::mouseHead`; `Target` hits the event's head's frame, the capture stack stays
  global; test functions take a head (`moho64ui_MouseMove(x, y, head)`, `MouseButton(b, p, x, y, mods, head)`,
  `MouseWheel(x, y, n, head)`, `ControlAt(x, y, head)`, `PixelAt(x, y, head)`, `InterfacePng(file, head)`), and
  `moho64ui_EdgeScroll(on)`. Edge pan only with the mouse on head 0. The world views' mouse tests (PointedView,
  the path preview's cursor, GetRightMouseButtonOrder) and MouseInView go by the mouse's head.
- World views: `GatherViews` walks the other heads' frames; their views get `WorldViewSpec::window = 1` in
  `g_viewsGiven` (`moho64ui_Views()` reports `window`). Without the renderer's second window (headless, `--gdi`, or
  it failed) they are NOT given to the source: a head-1 view has no picture (EnsurePicture false; only a selection box
  is drawn in it) and its picking is the plain view from above (ProjectPoint / Unproject / UnitNearPlace). With it,
  see part 2 below.
- `UI_TrackUnit <camera> ...` built (READ 0x834460: TargetNothing / GetTargetEntity / TargetEntities with the
  selection); `cam_SetLOD CameraHead2` worked already.
- Window: `Window::owner`, `noActivate` (WS_EX_NOACTIVATE, owned), `partner` (the mouse lock clips to the rectangle
  round both windows; the wheel over the partner is handed to it), `InFront()` (either window); a press on window 2
  while none of ours is in front brings the main one forward. RunWindow opens window 2 (full screen on the
  Secondary Adapter's screen, or the test window), routes its mouse to head 1, draws head 1 into its own target
  (recorder + incremental painter) and shows it by `Window::Present` when it changed (and once a second); the pointer
  and the held pointer on both. `MOHO64UI_SECOND_CAPTURE=<png>` saves head 1's picture at frame
  `MOHO64UI_CAPTURE_FRAME` (300). SDL: events go to their window by `windowID` (`SDL_WINDOW_NOT_FOCUSABLE` for
  window 2; the mouse grab is off with two windows) - UNTESTED.
- Options: `moho64ui_Looks()` gives `heads`, `secondWidth`, `secondHeight`. (The list's rule: part 2.)
- Test: `ui/tests/dualhead.lua` (cases front, game, one).

### Part 2 (2026-11-08, ui helper "dualhead2"; steps 6-8 with the renderer's second window, render f31424b4 + bbed51c0)

- GameSession: `UseSecondWindow(handle, w, h)` (RunWindow's `hooks.secondOpened`, once window 2 is open) asks the
  window's picture for `WorldPicture::UseSecondWindow` (Win32 HWND; SDL: `UseSecondHostSurface` with window 2's
  Vulkan surface, UNTESTED), at once on the picture there is and again before Open on every picture made after
  (EnsureWindowPicture: the Graphics API or the anti-aliasing changed). Failure: a warning with `SecondWindowError()`,
  and head 1 stays on the plain copy. `CloseSecondWindow` (`hooks.secondClosing`) at RunWindow's end, before window 2
  goes. `GiveSecondLayer` = `SetSecondInterfaceLayer` with the changed part (all of it the first time and after a
  change of size, as GiveLayer); a resize of window 2 hands the new target over at once (the renderer never reads a
  freed one), and its size resizes the second swap chain (the renderer's rule).
- RunWindow: head 1's interface is now drawn BEFORE the present: with the renderer's second window over a
  transparent background (premultiplied, as head 0's layer) and given as layer 1; the present (`DrawToWindow`) shows
  both windows. Without it (`hooks.secondOn` false: `--gdi`, no picture, it failed) over black and copied to window 2
  after the present (part 1's plain copy). A change between the two (a picture made anew) repaints head 1 whole and
  logs it.
- World views: while `WorldSource::SecondWindowOn()` (GameSession::Picture: the window's picture with its second
  window on), `GatherViews` puts the head-1 views into the picture's views after head 0's (before the follow camera's
  inset), kind Split, `window = 1`, rect in head 1's pixels: `Picture::SetViews` passes `PictureView::window`. Their
  EnsurePicture goes through the main view (as the split screen's right view), so they get a view number, and every
  pick on head 1 (select, box select, the unit under the mouse, right-click orders, GetMouseWorldPos, UnProject,
  Project, the marks' ETAs and names) goes through the renderer's ViewProject / ViewGroundAt / ViewUnitAt /
  ViewUnitsIn by that number: the perspective picture. The plain view from above stays only where no picture is.
  `moho64ui_Views()` entries say `given` (handed to the source); `moho64ui_ViewNumber(view)` -> its view number (-1:
  no picture) and whether the second window is on.
- Options (3.4, step 8): `DisplayMode::secondHeadShown` = the renderer's picture in a window (built with it,
  `--window`/`--fullscreen`, not `--gdi`) or `MOHO64UI_SECOND_HEAD=1`; the Secondary Adapter then lists the other
  screens on a PC with 2+ (one screen: Disabled alone). Not probed at the start: the second window is only made once
  a second head is chosen (it needs the window). INFERRED, ours. Headless runs (the suite's screens / lobby) have no
  window, so their expectations are unchanged.
- Statistics: the overlay's frame line ends with `window 2: N a second` (how often `SecondWindowDrawnLastFrame`
  was true over the last second; in the log with `MOHO64UI_OVERLAY_LOG=1`), and the run's end logs `window 2: the
  renderer drew it in D of F counted frames shown with it on (P%; N a second)`.
- Check picture: `MOHO64UI_SECOND_CAPTURE=<png>` with the renderer's second window saves window 2's next picture
  shown, read back by the renderer (`CaptureNextSecondWindowFrame`, render bbed51c0), else head 1's interface;
  `MOHO64UI_CAPTURE_FRAME=+N` counts the frames of a game's interface (plain N: all frames).
- Test: `dualhead.lua` case `window` (live window `--size 1280x720`, `MOHO64UI_SECOND_WINDOW=960x540`, a game; ends
  itself after 15 s of numbers) and `windowone` (the same with one head, for the numbers). The headless `game` case
  now also checks that head 1's view is not given and has no picture there.
- Measured (2026-11-08, framed test windows 1280x720 + 960x540 on one screen, vsync off, pinned to CPUs 16-31, the PC
  shared with another test run, so rough): window 2 drawn in 67-84% of frames (about 115-150 a second at 150-200
  frames a second); window 0's frame to frame 5.94 ms (middle) with head 1 against 3.03 ms without it, the difference
  in the renderer's DrawToWindow (the second view's preparing, drawing and card time; showing 4.21 against 1.89 ms a
  frame). Not optimised (Chris: optimise at the end; 4.4's `every` / a lower scale for window 1 are the levers).
- Not done: 3.3's WM_DISPLAYCHANGE handling (head 1's screen gone, or the primary moved onto it: nothing hides
  window 2 yet), the `/dual` command line, a full-screen second head on a real second screen (not tried: the checks
  use the framed test window on one screen), the SDL path (compiled only).
