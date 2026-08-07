<h1 align="center">PrismTextureStreamer</h1>

<p align="center">
A Prism3D plugin that takes over cabin screens (GPS, dashboard, or custom) in
<b>Euro Truck Simulator 2</b> and <b>American Truck Simulator</b>. It can
capture a window, play YouTube or Spotify through a lightweight integrated client, or
decode direct media without window capture.
</p>

---

<img width="2560" height="1600" alt="Screenshot_20260806_000354_Artemis" src="https://github.com/user-attachments/assets/6c2a7ef7-fc14-4f3d-85da-e763cdb7d058" />

## What it does
Press **Ctrl+F8** in game to open an ImGui overlay. From there you can:
- Add a **GPS** screen, **Dashboard** screen, or a **Custom** screen (The target `.tobj` MUST be a functional screen from a UI Script, such as `/ui/gps.sii`)
- Pick a running application window as the source for that screen
- Play YouTube videos/playlists without running a full browser
- Play Spotify through either the low-impact Embed player or an experimental
  persistent-login Full Web Player
- Play local files or direct stream URLs through Windows Media Foundation
- Control media with assignable keyboard keys or configurable XInput gamepad
  chords for Play/Pause, Next, Previous, Mute and Volume
- Toggle the plugin menu with an assignable gamepad chord
- Spatialize Integrated Media Client sound from the driver's live head pose
- Fade and muffle outside-camera media by true camera-to-driver distance with
  the optional SPF companion
- Apply separate media volume in menus and detect external cameras reliably
- Keep the game's own environment audio clear by reducing media dynamically
  from live speed and wheel-contact telemetry
- Adjust and save brightness independently for every streamed screen
- Optionally adjust screen brightness automatically from live game lighting
- Replace engine-off media with the current truck brand/model standby logo
- Set interior-cab media volume independently from exterior near/far volume
- Pause and resume media automatically with the truck engine
- Prevent full-screen video colours from bleeding into the GPS bezel
- Automatically replace media with a calibrated reverse-mirror view in reverse
- Diagnose ETS2's internal `park`/`park_360` render-to-texture path without
  changing the driver's camera
- See live measured plugin CPU/readback cost, smoothed estimated FPS loss,
  render health, and a collapsed sampled logical-processor activity map
- Adjust target resolution and framerate
- Apply changes
- Restore any of the previous three distinct saved configurations

Whatever's rendering in the picked window gets captured and blitted onto the truck's screen every frame.

## How it works
### 1. Redirecting the texture file
A hook on Prism3D's `memserver_texture_queue_processor` walks the engine's pending texture object queue every tick. If a queued `tobj`'s path matches a screen's `original_texture` (e.g. `/vehicle/truck/share/gps.tobj`), the path is swapped for `override_texture` before the engine loads it.

### 2. Catching the texture at creation time
The override `.tobj` still gets built into a real DirectX texture eventually, there's a hook on `ID3D11Device::CreateTexture2D`. Every creation call is checked against a fignerprint. The real game textures never match the fingerprint, so it's whats used to capture the screens texture.

*This method is a bit "jank"... but it does work ;)*

Once matched, the description is rewritten before the real `CreateTexture2D` is called:
```cpp
D3D11_TEXTURE2D_DESC modifiedDesc = *pDesc;
modifiedDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
modifiedDesc.Usage = D3D11_USAGE_DYNAMIC;
modifiedDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
modifiedDesc.MiscFlags = 0;
modifiedDesc.Width = screen.targetLiveTextureWidth;
modifiedDesc.Height = screen.targetLiveTextureHeight;
```

This gives us a CPU writable arbitrarily sized texture instead of the default game texture -  then the resulting texture and its immediate context are cached on the `screen_t` for later.

### 3. Feeding it frames
Every present call, each screen with a live texture and an active source:
- Pulls the latest RGBA8 frame from its content source
- `Map()`s the live texture with `D3D11_MAP_WRITE_DISCARD`
- Copies row by row using nearest neighbor sampling if source and destination sizes differ, with an optional vertical flip
- `Unmap()`s it

### 4. Forcing the screens to actually render
The GPS/dashboard/custom screens are normally only drawn under specific in game conditions. `dllmain.cpp` patches the relevant conditional jump in the games process, flipping `JE` to `JMP` (and back) at runtime so the screen's render path is unconditionally taken whenever a screen of that type exists, and restored to normal when it doesn't. Addresses are found via pattern scanning, so it hopefully survives most game updates.

## Content sources

- **Window Capture** uses modern Windows Graphics Capture by default, with
  `PrintWindow` available only as a compatibility fallback.
- **Integrated Media Client** hosts the official YouTube or Spotify embedded
  player, or an HTML5 video element in a minimal hardware-accelerated WebView2 process. The
  plugin captures this one clean surface rather than a full browser.
- **Native Direct Media** uses Media Foundation and D3D11 hardware decoding for
  local files and direct media URLs. It bypasses window capture completely.

All backends implement `IContentSource`, including shared media controls and
performance statistics.

## Requirements
- MinHook
- Dear ImGui
- SCS Telemetry SDK
- DirectX 11
- Microsoft Edge WebView2 Runtime for the Integrated Media Client (normally
  already installed on Windows 10/11)
- Optional: SPF Framework 1.2+ for exact external-camera position. The main
  plugin retains fixed outside-volume fallback without it.
- ReShade is not required. The plugin creates a private hidden DirectX 11
  probe window to install its own Present and texture hooks.

## Known issues
- Fingerprinting textures by dimensions/format means any other texture in the game that happens to match GPS/dashboard's size and format exactly would get caught too which is unlikely (if using unqiue dimentions), but not impossible.
- Custom screens have no override_texture_size_w/h wiring in the menu yet, unlike GPS/Dashboard.

## Usage

1. Copy `PrismTextureStreamerFB.dll` and the `PrismTextureStreamerFB` folder
   from the release package into the ETS2/ATS `bin\win_x64\plugins` folder.
   The main DLL stays directly in `plugins`; helper files remain organized in
   the subfolder.
2. Launch the game, Ctrl+F8 to open the menu
3. Add a screen, pick a source window, hit Apply

## Version 3.10 visual, audio and input reliability

Version `3.10.1-engine-standby-logo` reapplies the saved brightness after every
WebView navigation, including Spotify after a game restart. It adds optional
automatic brightness using a dormant, non-blocking 4 x 4 game-lighting sample,
plus a generated engine-off standby logo using the active truck's localized
brand and model name. Its brightness is configurable, and media returns when
the engine starts.

The update also adds an independent interior-cab media-volume slider and moves
Volume Up/Down to the WebView Windows audio session so YouTube and both Spotify
modes respond even when webpage controls are protected. Keyboard media keys now
use plugin-owned edge detection, fixing missed first presses. The overlay uses
one Win32 cursor, and the legacy `LB + Start/Menu` default migrates to
`LB + Right Stick Click` so ETS2/ATS does not open its own cursor.

See `PrismTextureStreamerFB\docs\V3.10-VISUAL-AUDIO-INPUT.md`.

## Version 3.9.4 media controls and update notifications

Version `3.9.4-media-controls-updates` fixes single-press Spotify Play/Pause,
Next and Previous behavior by synchronizing commands with the observed media
state and retrying once only when Spotify is rebuilding a disabled control.
Spotify volume changes now use the native input setter required by React, with
a direct observed-media fallback. A non-blocking startup check compares the
installed semantic version with the latest GitHub release and shows an in-game
toast plus an **Open GitHub Releases** menu action when an update is available.

Version 3.9.3 makes Spotify Full Web receive the same
dynamic outside low-pass processing as YouTube. It remembers audio elements at
their `play()` and `load()` boundary even when Spotify keeps them detached from
the visible DOM, then routes those remembered elements through the adaptive
filter. The page-owned Web Audio output hook and normal DOM media scan remain
as fallbacks. The helper and plugin logs report the selected cutoff, observed
media elements and how many filter routes are attached.

Version 3.9 also fixes the supplied Full Spotify Web Player
logs: WebView2 is initialized only once, Spotify authentication cookies receive
an encrypted per-Windows-user checkpoint, and private login query strings are
removed from diagnostics. Chromium occlusion/background throttling is disabled
for the silent helper so the GPS keeps receiving live frames while the helper
is behind the game.

The plugin now retains three timestamped rolling configuration restore points
outside the active `config.ini`. They can be saved or restored from
**Configuration Backups**, and they remain available if only the active config
is deleted. An assignable **Plugin menu** gamepad chord is also available under
**Media Hotkeys**; version 3.10 migrates its former `LB + Start/Menu` default
to `LB + Right Stick Click` to avoid opening the game cursor.

See `PrismTextureStreamerFB\docs\V3.9-SPOTIFY-SESSION-BACKUPS.md`.

## Version 3.8 Spotify, gamepad and render diagnostics

Version `3.8.0-spotify-gamepad-diagnostics` adds two selectable Spotify
experiences. **Embed** remains the recommended low-impact option. The
experimental **Full Web Player** loads the normal Spotify website in the
integrated helper, preserves its login in a dedicated WebView2 profile, and
routes Play/Pause, Next, Previous, Mute and Volume through page controls with a
Windows media-key fallback. Buttons in the in-game menu open the helper for
login and return it to silent capture mode.

Media commands can also use XInput chords. Every command has an independent
modifier (None/LB/RB/LT/RT) and button, trigger, D-pad or left/right-stick
direction. The defaults use RB+A for Play/Pause and RB+right-stick directions
for track and volume controls. Input remains visible to ETS2/ATS.

The render diagnostics now report texture redirects and matches, WGC startup,
resize and staging events, stale sources, D3D11 mapping failures, periodic
render summaries, WebView navigation/process failures, and sampled
magenta/pink frames. The Live Performance Monitor uses a 2.5-second-smoothed
FPS-loss model and contains a collapsed CPU logical-processor list: red is an
observed game render-thread sample, green is an in-process plugin-worker
sample, and yellow is both. These are low-overhead two-second samples rather
than a full ETW profiler. Soft ideal-processor hints are also forwarded to the
helper and WebView browser threads without changing game affinity.

See `PrismTextureStreamerFB\docs\V3.8-SPOTIFY-GAMEPAD-DIAGNOSTICS.md`.

## Version 3.6 telemetry environment ducking

Version `3.6.0-telemetry-environment` removes all external stationary, city,
highway and wind audio files. ETS2/ATS remains the only environment-sound
source. The plugin estimates the live environment level from official truck
speed and per-wheel ground-contact telemetry, then smoothly reduces integrated
YouTube/Spotify volume. Interior and exterior effect strengths are configured
separately, and the live estimate is visible in the ImGui menu. This path does
not capture game audio, decode extra files, or add work to the screen upload.
See `PrismTextureStreamerFB\docs\V3.6-TELEMETRY-ENVIRONMENT.md` in the release
package.

## Version 3.7 event playlist refresh and core balance

Version `3.7.0-event-playlist-core-balance` refreshes a YouTube playlist only
when its current video ends or the user presses Next/Previous. This allows
videos added from a phone to appear without restarting the game and adds no
playlist polling while a video is playing.

The environment-volume estimator now performs its complete calculation at a
maximum of 20 Hz and never waits for its settings mutex. Plugin-owned capture
and decoder threads receive soft ideal-processor hints based on recent render
thread placement, while the separate media helper runs Below Normal. The game
process affinity is never restricted. Unchanged helper audio values are now
true no-ops, and asynchronous native/media diagnostic logs are stored beside
the game executable for issue reports. See
`PrismTextureStreamerFB\docs\V3.7-PLAYLIST-CORE-BALANCE.md` for details.

## Version 2.0 performance and media changes

Version `2.0.0-performance` includes:

- Selectable Window Capture, Integrated Media Client and Native Direct Media
- Official YouTube IFrame player support for videos and playlists
- Native Media Foundation playback with audio and D3D11 frame transfer
- Live CPU/readback/dropped-frame meter and estimated FPS loss
- Assignable persistent media hotkeys

- Persistent WGC staging textures instead of allocating one every frame
- FPS limiting for Windows Graphics Capture as well as legacy capture
- Uploading only newly captured frames rather than repeating work at game FPS
- Native BGRA row copies without a per-pixel red/blue channel conversion
- Cached horizontal scaling indices
- Atomic frame/dimension snapshots to avoid resize-related corruption
- Last-valid-frame retention through temporary zero-size/resize events
- Configuration persistence in `%LOCALAPPDATA%\PrismTextureStreamerFB\config.ini`
- Automatic restoration of screens, source window, resolution, FPS, flip state, and capture mode
- Zero-copy buffer swaps between capture and rendering threads
- Triple-buffered, non-blocking WGC GPU readback
- Aspect-preserving capture downscaling so low-resolution profiles avoid copying full 1080p/4K frames through CPU memory
- Pause/freeze mode that keeps the last image while stopping plugin frame work
- Economy, Balanced, Quality, Smooth, and Custom performance profiles
- Stretch, aspect-correct Fit, and centre Crop scaling modes
- Persistent settings for performance profile, pause state, and scaling mode

## Version 2.4 exact external-camera distance audio

Version `2.4.0-distance-audio` includes all 2.3 features plus:

- An optional SPF-compatible `PrismCameraBridge.dll` that publishes the
  active camera type and world coordinates through a tiny shared-memory block
- Per-truck driver-head calibration from interior camera 1
- Live camera-to-driver distance while outside the cab, including while the
  truck is moving
- Configurable full-volume and mute/minimum-volume distances
- A logarithmic distance-controlled low-pass filter for realistic muffling
- Live metres, applied gain, and cutoff readouts in the ImGui menu
- Automatic fixed-volume fallback when SPF or the companion is not installed

The normal plugin files remain in `bin\win_x64\plugins`. For exact distance,
install the [SPF Framework](https://github.com/TrackAndTruckDevs/SPF-Framework/releases)
and copy `PrismCameraBridge.dll` to
`bin\win_x64\plugins\spfPlugins\PrismCameraBridge`. See
`SPF-BRIDGE-INSTALL.txt` in the build artifact.

## Version 2.5 reverse profiles and trailer tracking

Version `2.5.0-reverse-profiles` adds:

- Economy `426x240 @ 10 FPS`, Balanced `640x360 @ 15 FPS`, Quality
  `960x540 @ 20 FPS`, Ultra `1280x720 @ 30 FPS`, and Custom reverse profiles
- Separate reverse-capture resolution, independent of the normal media profile
- Deferred configuration writes so dragging a slider no longer writes the INI
  once per rendered frame
- SPF tracking for the live placement and orientation of the final connected
  trailer in singles, doubles, triples, and articulated combinations
- Clear SPF bridge lifecycle and trailer-telemetry diagnostics in the menu

The documented SPF 1.2 camera API controls the active player camera and does
not expose a second camera render target. For that reason, this release does
not switch the player's view or present an unstable flickering camera as a
finished feature. The working low-overhead reverse source remains a selected
F2 virtual side mirror. The published tail transform is the foundation for a
future validated mirror-render hook.

## Version 2.6 internal render-to-texture probe

Version `2.6.0-rtt-probe` adds a read-only, version-guarded diagnostic for the
true in-game reverse-camera path. The supplied ETS2 1.60.1.7 executable contains
nine internal camera slots, including `park` and `park_360`. This build:

- enables the internal scheduler hook only for the exact analyzed executable;
- reports the camera slots that ETS2 actually creates for the current truck;
- records D3D11 render-target bindings for a user-controlled 10-second trace;
- ranks candidate GPU textures and writes full results to `game.log.txt`; and
- performs no pixel readback and never moves the player's active camera.

This diagnostic is deliberately separate from the finished camera. The runtime
trace is required to select the correct engine-owned texture before enabling
camera creation or GPU-to-GPS transfer. See `V2.6-INTERNAL-RTT-PROBE.md`.

## Version 2.7 park-camera activation test

The 2.6 runtime log proved that the tested truck creates slots 0-5 but leaves
the internal `park` and `park_360` slots empty. Version
`2.7.0-park-activation-test` adds the next guarded stage:

- a selectable **Internal park-camera activation (experimental)** reverse
  method;
- exact byte guards for ETS2's park-resource initializer, camera-mask
  function, mirror-camera vtable, and clone method;
- creation of engine-owned slot 7 while the truck interior loads;
- scheduling of the extra camera only during the explicit 10-second trace; and
- candidate matching for both nominal and 2x mirror-scale render targets.

This build still does not replace the GPS pixels. Its purpose is to identify
the exact engine-owned park texture before a GPU-to-GPU conversion is added.
The stable Window crop reverse method remains available. See
`V2.7-PARK-ACTIVATION-TEST.md`.

## Version 2.8 internal park-camera display test

Version `2.8.0-internal-camera-test` completes the first GPU display path for
the guarded ETS2 park camera:

- captures the slot 7 `R11G11B10_FLOAT` colour target while ETS2 creates the
  truck's mirror resources;
- transfers and tone-maps that texture directly to the selected GPS through
  D3D11, with no window capture and no CPU pixel readback;
- provides 10, 15, 20 and 30 FPS internal-camera profiles;
- schedules slot 7 only in reverse or while Preview is enabled, retaining zero
  park-camera render cost in forward gear;
- restores the previous media frame immediately after leaving reverse; and
- keeps the normal GPS media visible if target discovery or GPU compositor
  initialization fails.

The internal offsets remain locked to the exact supported ETS2 executable.
Other ETS2 builds and ATS continue to use **Window crop (stable)**. See
`V2.8-INTERNAL-CAMERA-TEST.md`.

### Version 2.8.1 loading-crash hotfix

The supplied v2.8 crash log showed `DXGI_ERROR_DEVICE_REMOVED` during the
loading-screen swap-chain resize. Version `2.8.1-loading-crash-hotfix` restores
the stable v2.7 dynamic GPS upload texture and disables direct internal-camera
GPU output. The internal slot remains available for diagnostics; use **Window
crop (stable)** for a visible reverse feed in this hotfix. See
`V2.8.1-LOADING-CRASH-HOTFIX.md`.

### Version 2.9 safe staged-readback test

Version `2.9.0-safe-readback-test` reconnects the internal park camera without
changing the stable GPS texture:

- observes ETS2's live 256x256 `R11G11B10_FLOAT` park target only while slot 7
  is explicitly scheduled;
- copies it into a three-texture staging ring;
- uses `D3D11_MAP_FLAG_DO_NOT_WAIT`, dropping a busy frame instead of stalling
  the game render thread;
- tone-maps the small HDR image to BGRA on the CPU; and
- uploads it through the same dynamic GPS path used by the working v2.8.1
  hotfix.

The staging resources exist only after reverse/Preview is requested. Forward
driving retains zero park-camera render/readback work. See
`V2.9-SAFE-READBACK-TEST.md`.

### Version 2.9.1 park-target selector

The first v2.9 test proved that slot 7 renders and that safe readback remains
stable, but ETS2 exposes more than one 256x256 HDR target during the same park
pass. Following the last matching target can therefore show a flat grey
intermediate buffer.

Version `2.9.1-park-target-selector-test` records the distinct matching buffers
as Candidate A-D and adds a saved **Internal colour target** setting. Candidate
A is the default for ETS2 1.60.1.7 based on the supplied render trace. If it is
not the rear colour image, switch to B, then C/D while Preview remains enabled.
Changing candidates does not add another render or readback pass. See
`V2.9.1-PARK-TARGET-SELECTOR.md`.

## Version 2.3 engine-powered media and GPS edge fix

Version `2.3.1-adaptive` added:

- Head-orientation stereo panning and directional attenuation for the
  Integrated Media Client
- Configurable outside-cab fade/mute based on the driver's telemetry head offset
- Original Windows mixer levels are preserved and restored when the effect is
  disabled or the media source closes
- Automatic reverse detection from selected gear and reverse-light telemetry
- A low-overhead reverse mirror that captures only the configured virtual-mirror
  rectangle and sleeps while driving forward
- In-game reverse-feed preview, crop calibration and framerate control
- Persistence for every adaptive-audio and reverse-view setting
- Configurable media volume in game menus and before entering the truck
- External-camera detection that avoids reusing stale head telemetry
- A saved 10–200% brightness scale for each GPS, dashboard, or custom screen
- GPU black-overlay brightness for Integrated Media Client playback, avoiding
  the per-pixel game-thread brightness cost when darkening YouTube/media
- Optional engine-follow playback using `truck.engine.enabled` telemetry
- A configurable opaque edge guard that stops video-colour bleed around GPS
  displays

The legacy reverse feature captures a user-calibrated region of the game
window. Use **Preview / calibrate now** to define that region. Versions 2.6
through 2.8 provide the guarded migration path and first GPU display test for
a genuine independent rear-camera texture that does not depend on an on-screen
mirror.

Use `build-release.ps1` on Windows with Visual Studio 2022 Build Tools and the
**Desktop development with C++** workload to produce the x64 release DLL.

Alternatively, open the repository's **Actions** tab, select
**Build Windows DLL**, choose **Run workflow**, and download the resulting
`PrismTextureStreamerFB-2.9.1-park-target-selector-test` artifact. This cloud build
requires no local Visual Studio installation.

## Contributing
PRs and issues are welcome, keep in mind:

- If a pattern scan fails to resolve on a newer patch, that's the first thing to check before assuming something else is broken. (Same with any Prism3D structures inside `prism/prism.h`)
- DO NOT use hard coded file offsets etc, keep with using pattern scans where possible.
- If you're adding a new `IContentSource` (video file, monitor capture, etc.), implement it against the existing interface in `sources/content_source.h`.
- Match existing code style so diffs stay reviewable and the project isnt a mess of multiple people.
- Test on an actual ETS2 install before opening a PR.
