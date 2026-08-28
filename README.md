# PrismMedia 4.0.0

This revision includes the optional restricted runtime controller documented
in [DIAGNOSTIC-CONSOLE.md](DIAGNOSTIC-CONSOLE.md). It can repeat bounded custom
render tests, select a custom display, adjust the same-thread Release timing
window, inspect status, and switch the compatibility fallback without
rebuilding the plugin. It does not expose arbitrary scripts or native calls.

This 4.0.0 source revision includes the guarded combined one-cycle test
described in [CUSTOM-RENDER-PROBE.md](CUSTOM-RENDER-PROBE.md). One reload now
captures the list entries, the exact old texture's standard COM `Release`, and
the replacement texture's bind/draw path. An
entry is still bypassed only after an exact bounded match through the game's
own loop-advance sequence. The working compatibility fallback is restored
automatically.

PrismMedia streams YouTube, Spotify Web, local media, direct streams,
or a desktop window onto supported ETS2/ATS truck displays.

## Current combined-test revision

- System now includes a dedicated **Run combined one-cycle test** action while
  driving. It requires an active old live custom texture and arms the bounded
  breakpoints, exact COM `Release` correlation and Direct3D correlation before
  issuing one truck reload command. It never detours a private cleanup function.
- The one-click capture is bounded to 2,048 branch events, 256 post-R9 list
  entries, 60 seconds overall, and 10 seconds after the exact texture match.
  After that match the probe temporarily emulates the working compatibility
  decision so the selected texture reaches the Direct3D bind/draw path, then
  restores the normal fallback automatically.
- The earlier post-match 1.5-second probe is replaced because runtime testing
  proved that it armed too late and captured zero branch executions.
- The latest capture proved that two cleanup entries are cleared before the new
  texture exists. The targeted test therefore searches each entry for the old
  live texture through a bounded three-child graph. It requires exact pointer
  equality and a verified native RSI-advance/backedge before selectively
  bypassing an entry; unmatched entries retain original game behavior.
- Because the bounded pointer graph did not expose the old texture in either
  Custom 1 or Custom 2, a temporary hook on the old texture's standard COM
  `Release` function associates an exact release with the list entry most
  recently captured on the same thread. It records the list index and release
  stack, never suppresses a release, and is removed with the bounded window.
  The earlier experimental private cleanup-function detours were removed after
  runtime testing showed that entering that path could terminate the game.
- The primary correlation follows the exact replacement texture through
  `CreateShaderResourceView` and `PSSetShaderResources`. Independent fallback
  resource inspection and all standard, instanced, automatic, and indirect
  draw hooks capture multiple real draw samples, game-relative call stacks,
  the earlier branch event and the captured post-R9 list-entry identities.
  These high-frequency hooks are disabled outside the bounded test.

## Retained features

- New hybrid dashboard UI with a Home overview, custom vector icons, elevated
  cards and simulated soft shadows, animated audio waves, mouse/gamepad
  navigation, auto-save, smooth page transitions, and a live option-effect
  inspector. The console re-reads the game viewport whenever it opens,
  auto-centres against the full viewport, clamps moved windows on-screen,
  can be resized, and reflows its navigation for shorter displays.
- System > Display discovery keeps a passive history of `.tobj` paths actually
  loaded by the truck and accessories. A 30-second observation marks paths used
  while an installed accessory or infotainment mode is switched; it never
  reloads the game or applies an unconfirmed configurator basket. Likely GPS,
  dashboard, tablet, navigation, infotainment and YTHQ paths are prioritised,
  while traffic/world textures are marked unsafe and blocked from automatic
  assignment. PrismMedia validates and regenerates the paired DXT5 `.dds` and
  `.tobj` override assets for every display at startup. Asset repair and the
  final critical texture reload are deliberately separate actions.
- The complete retained control set remains available: GPS/Dashboard/Custom
  displays, Pause/Freeze, media mute and volume, hotkey target selection,
  performance profiles, scaling modes, backups, adaptive audio tuning, update
  notifications, detailed live performance, and sampled per-core activity.
- Media URLs load immediately. Lightweight saved-link libraries are available
  separately for YouTube and Spotify. Every truck display now owns an isolated
  PrismMediaClient/WebView2 profile and audio session, so GPS, dashboard and
  tablet/custom displays can play different sources simultaneously. A
  gamepad-accessible Media Key Target selector routes transport keys without
  disturbing the other displays. Per-display **Open client** and **Hide client**
  controls expose or dismiss the interactive helper without stopping playback.
  **Close client** terminates only that display's helper, capture and audio
  session and releases their resources; **Open client** can start it again
  without reloading the game. Bundled personal URLs remain removed.
- Generated override assets and client data use the discovered game-texture
  name. For example, `driver_plate.tobj` produces `driver_plate.tobj` and
  `driver_plate.dds` in the game's PrismMedia override directory, plus an
  isolated `%LOCALAPPDATA%\PrismMedia\Clients\driver_plate` profile and log.
  A short stable suffix is added only when two different game paths share the
  same basename. Existing WebView2 and Spotify session data is migrated from
  the old suffix-based folder on first use.
- Generated-name reservation is evaluated against the final readable path,
  not only the screen's previous override. Thus the stock
  `/vehicle/truck/share/gps.tobj` may retain `gps.tobj`, while an accessory such
  as `/vehicle/truck/upgrade/bagps/gps.tobj` automatically receives a stable
  `gps_<id>.tobj`/`.dds` pair instead of blacking or replacing the primary GPS.
- Independent clients still require independent Prism3D texture paths. If a
  tablet mod reuses `/vehicle/truck/share/gps.tobj`, the tablet and primary GPS
  are two surfaces of one texture and cannot show different media. PrismMedia
  now detects that collision, stops the duplicate helper/audio source, protects
  the primary GPS from being overwritten, and asks for the accessory's unique
  material TOBJ. New secondary GPS/dashboard entries start unassigned instead
  of silently claiming the generic path.
- Integrated WebView video overlays are forced into the capturable compositor
  so a second isolated media client does not produce valid-but-black WGC frames.
  Runtime diagnostics now distinguish persistent black capture, magenta capture,
  stale frames and successful texture matches; TOBJ repair is no longer offered
  as a misleading remedy for a black source frame.
- Spotify now uses only the official full Web Player. The embedded Spotify path
  and its duplicated playback state are removed.
- Play/Pause is state-neutral in the plugin, while actual browser media events
  resynchronize playback intent to prevent a stale pause indicator.
- Controller navigation probes all installed XInput providers so Steam Input's
  virtual pad is found even when it hooks a different XInput DLL than ETS2/ATS.
  WinMM and keyboard-emulation fallbacks remain available. D-pad or left stick
  moves, A activates, B goes back, LB/RB changes pages, and LT/RT cycles the
  three Audio tabs; merely focusing an Audio card also selects it. Media chords
  are suppressed while the menu owns the controller.
- Adaptive gain, stereo pan, and low-pass changes use a 650 ms crossfade so
  cabin/exterior camera transitions do not produce an abrupt “DJ” effect. Each
  isolated display receives its own adaptive-audio update, while media keys
  continue to control only the selected Media Key Target.
- Display names use compact numbering per type. Removing Custom 1 and creating
  another custom display produces Custom 1 again; GPS and Dashboard numbering
  is compacted independently.
- Engine-off playback pauses immediately with no fade; engine-start can still
  recover smoothly without changing cabin/exterior transition behavior.
- Automatic brightness now behaves like a phone sensor: sunlight changes settle
  over about 2.5 seconds and dimming over about 4 seconds instead of jumping.
- Normal options apply live and auto-save. Game texture identity changes and
  restored backups are the only actions that expose a texture reload button.
- The media helper receives its shutdown request at the beginning of game
  teardown for faster, cleaner exit, and silent startup uses a tool-window style
  so the helper does not create a taskbar button.
- Reverse camera, Camera Lab/GPU tracing, AI traffic radio, the diagnostic
  viewer, and their accessory/runtime code are completely removed.

## Install

1. Build `Release|x64` or run `build-release.ps1` on Windows.
2. Remove an older `PrismTextureStreamerFB.dll`, then copy `PrismMedia.dll` and
   the `PrismMedia` folder to
   `<game>\bin\win_x64\plugins`.
3. Keep the DLL directly in `plugins`; keep the helper files in the adjacent
   `PrismMedia` folder.
4. Open the UI with `Ctrl+F8` or the configured gamepad chord.

`config-recommended.ini` is a sanitized copy of the recommended 4.0 profile.
It contains no media URL or account data.

## Playback choices

- Integrated media: recommended for YouTube and Spotify Web.
- Native direct media: lowest overhead for files and direct stream URLs.
- Window capture: broadest compatibility, with higher capture overhead.

The optional SPF camera bridge improves exact exterior-camera distance for
adaptive audio. See `SPF-BRIDGE-INSTALL.txt` in the packaged optional folder.

## Build requirements

- Visual Studio 2022 with Desktop development with C++
- Windows 10/11 SDK
- .NET Framework 4.8 targeting pack
- WebView2 NuGet restore access

Third-party SPF material remains subject to `ThirdParty/SPF/LICENSE`.
