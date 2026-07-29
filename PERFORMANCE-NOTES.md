# PrismTextureStreamerFB 2.6 Performance and Adaptive Features Guide

## Choose the playback method

| Method | Typical impact | Supported content | Notes |
| --- | --- | --- | --- |
| Native Direct Media | Lowest | Local video files and direct media/stream URLs | Media Foundation hardware decode; bypasses window capture |
| Integrated Media Client | Low to medium | YouTube videos/playlists, direct URLs | Recommended for YouTube; removes the full browser but retains one optimized WGC transfer |
| Window Capture | Medium to high | Any visible application | Maximum compatibility; the external app and capture path both consume resources |

YouTube page URLs must use the Integrated Media Client. The plugin does not
extract or bypass YouTube media streams; it uses the official embedded player.

## Recommended profile

Start with **Balanced**:

- Capture/output limit: 1280 x 720
- Capture rate: 30 FPS
- Modern Windows Graphics Capture
- Scaling: Fit or Crop

This is normally sufficient for a GPS or dashboard-sized display.

## Profiles

| Profile | Resolution | FPS | Best use |
| --- | ---: | ---: | --- |
| Economy | 854 x 480 | 20 | Lowest CPU/memory bandwidth |
| Balanced | 1280 x 720 | 30 | Recommended default |
| Quality | 1920 x 1080 | 30 | Large or close-up screens |
| Smooth | 1280 x 720 | 60 | Motion-heavy video |
| Custom | User-defined | User-defined | Manual tuning |

## Performance controls

- **Pause / Freeze** retains the last dashboard image and stops plugin frame
  processing. A valid first frame is still captured after startup.
- **Fit** preserves the source aspect ratio and adds bars where necessary.
- **Crop** preserves aspect ratio and fills the texture by cropping the centre.
- **Stretch** is the least visually accurate but fills the entire texture.
- **Screen Brightness** is saved per screen. Integrated Media Client playback
  uses a GPU-composited black overlay below `100%` and a GPU brightness filter
  above `100%`, so the game's upload thread keeps its fast-copy path. Window
  Capture and Native Direct Media retain the compatible CPU colour lookup.
  At `100%`, every mode uses the existing fast-copy path.
- **Edge Colour-Bleed Guard** writes only the outer 0–16 pixels after upload.
  The recommended `2 px` prevents clamp-sampled video colours from tinting the
  truck GPS bezel and has negligible cost.
- Leave **Legacy Capture** disabled unless Windows Graphics Capture fails for a
  specific application.
- The **Live Performance Monitor** reports measured game-thread upload time,
  source-worker CPU time, GPU readback time, delivered FPS, and dropped frames.
  The FPS-loss value is estimated from measured render-thread cost; decoder/GPU
  contention cannot be converted into an exact FPS number on every system.

## Media controls and hotkeys

Play/Pause, Next, Previous, Mute, Volume Up, and Volume Down are available in
the in-game menu. Every action can be rebound to a key or key combination and
the bindings persist in `%LOCALAPPDATA%\PrismTextureStreamerFB\config.ini`.

For YouTube playlists, Next and Previous change playlist items. For native
direct media, they seek forward or backward by 30 seconds.

## Why playback cannot have literally zero impact

Displaying changing content always requires decoding and updating a DirectX
texture. Version 2.0.0 offers a direct Media Foundation path that avoids window
capture, plus a small WebView2-based YouTube client that avoids running a full
browser. The existing capture path still limits resolution, skips duplicates,
uses non-blocking triple-buffered GPU readback, and caches scaling data.

## Adaptive cabin audio

Adaptive audio is available for the Integrated Media Client. It uses
`truck.head.offset` telemetry to:

- move the media sound left/right as the driver turns;
- reduce sound when the driver faces away from the dashboard speaker;
- fade or mute media when the camera moves outside the cab.
- reduce media to a configurable level before driving or while the game is in
  a menu/paused state.

The effect controls only the WebView2 media audio sessions. It does not modify
engine, navigation, traffic, radio, or other game sounds. Use **Speaker
direction** to place the apparent dashboard source, **Spatial strength** to
blend the effect, **Volume when outside** = `0` for silence outside, and
**Volume in menus / before driving** for the game UI.

External cameras no longer reuse a frozen head value. The plugin combines head
telemetry freshness with the default `1` interior and `2-9/0` external camera
keys. When it detects an external camera, panning is centred and the configured
outside volume is applied.

For true camera distance, install the optional SPF Framework companion from
the artifact's `SPF-OPTIONAL` folder. It reads SPF's active camera type and
world coordinates, calibrates the driver-head position when camera `1` is
active, and follows the truck while outside. The menu then provides:

- **Full-volume distance**
- **Mute / minimum-volume distance**
- **Minimum volume when far away**
- **Distance low-pass**
- **Far-distance cutoff**

Volume follows a smooth distance curve. The cutoff is interpolated
logarithmically from full bandwidth near the driver to the configured
far-distance frequency. This processing is applied only to media inside the
Integrated Media Client; game audio sessions are not filtered.

The companion performs no image capture or rendering. If it is not installed
or cannot supply a current camera position, the plugin continues with the
existing fixed outside-volume fallback.

## Engine-powered media

For Integrated Media Client and Native Direct Media, enable **Play media only
while truck engine is running**. Inside gameplay, `truck.engine.enabled`
telemetry temporarily pauses playback while the engine is off and resumes it
when the engine starts. The user's own Play/Pause choice is preserved. The
Integrated Media Client capture worker also sleeps while engine-paused.

Engine control is inactive in game menus and before the driving session starts,
so **Volume in menus / before driving** continues to control that state.

## Automatic reverse camera

The legacy reverse view captures a calibrated rectangle of the game window and
is paused outside reverse gear. This is intentionally cheaper and more compatible
than creating another 3D scene render.

1. Enable the game's virtual mirror with **F2**.
2. Open **Automatic Reverse Camera** on the target plugin screen.
3. Enable **Show reverse view on this screen**.
4. Use **Preview / calibrate now**.
5. Adjust left, top, width, and height until only the mirror is visible.
6. Disable preview. The source now wakes automatically in reverse.

Use one of the saved reverse profiles:

- **Economy:** 426x240 at 10 FPS — very low impact
- **Balanced:** 640x360 at 15 FPS — recommended
- **Quality:** 960x540 at 20 FPS — medium impact
- **Ultra:** 1280x720 at 30 FPS — high impact
- **Custom:** user-controlled resolution and FPS

The mirror view is truck/trailer aware because ETS2/ATS renders it; it is not
a physically mounted trailer-tail camera. The SPF bridge publishes
the live final-trailer transform. SPF 1.2 does not expose an independent
camera render texture, so using its free camera directly would replace the
driver's main view. Prism deliberately avoids that unsafe/flickering approach.
