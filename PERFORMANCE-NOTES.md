# PrismTextureStreamerFB 2.0.0 Performance Guide

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
