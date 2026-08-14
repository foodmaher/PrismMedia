# PrismTextureStreamerFB 4.0.0

PrismTextureStreamerFB streams YouTube, Spotify Web, local media, direct streams,
or a desktop window onto supported ETS2/ATS truck displays.

## What changed in 4.0.0

- New hybrid dashboard UI with a Home overview, custom vector icons, elevated
  cards and simulated soft shadows, animated audio waves, mouse/gamepad
  navigation, auto-save, smooth page transitions, and a live option-effect
  inspector.
- The complete retained control set remains available: GPS/Dashboard/Custom
  displays, Pause/Freeze, media mute and volume, hotkey target selection,
  performance profiles, scaling modes, backups, adaptive audio tuning, update
  notifications, detailed live performance, and sampled per-core activity.
- Media URLs load immediately. A YouTube playlist URL is supported as one live
  source, but the old saved multi-playlist library and all bundled personal URLs
  are removed.
- Spotify now uses only the official full Web Player. The embedded Spotify path
  and its duplicated playback state are removed.
- Play/Pause is state-neutral in the plugin, while actual browser media events
  resynchronize playback intent to prevent a stale pause indicator.
- Adaptive gain, stereo pan, and low-pass changes use a 650 ms crossfade so
  cabin/exterior camera transitions do not produce an abrupt “DJ” effect.
- Engine-off playback pauses immediately with no fade; engine-start can still
  recover smoothly without changing cabin/exterior transition behavior.
- Automatic brightness now behaves like a phone sensor: sunlight changes settle
  over about 2.5 seconds and dimming over about 4 seconds instead of jumping.
- Normal options apply live and auto-save. Game texture identity changes and
  restored backups are the only actions that expose a texture reload button.
- The media helper receives its shutdown request at the beginning of game
  teardown for faster, cleaner exit.
- Reverse camera, Camera Lab/GPU tracing, AI traffic radio, the diagnostic
  viewer, and their accessory/runtime code are completely removed.

## Install

1. Build `Release|x64` or run `build-release.ps1` on Windows.
2. Copy `PrismTextureStreamerFB.dll` and the `PrismTextureStreamerFB` folder to
   `<game>\bin\win_x64\plugins`.
3. Keep the DLL directly in `plugins`; keep the helper files in the adjacent
   `PrismTextureStreamerFB` folder.
4. Open the UI with `Ctrl+F8` or the configured gamepad chord.

`config-recommended.ini` is a sanitized copy of the recommended 4.0.0 profile.
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
