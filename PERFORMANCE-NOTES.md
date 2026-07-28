# PrismTextureStreamerFB 1.3.0 Performance Guide

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

## Why the plugin cannot have literally zero impact

Displaying changing external content requires Windows to capture a frame and
the plugin to update a DirectX texture. Version 1.3.0 reduces avoidable work by
limiting capture resolution, skipping duplicate frames, avoiding a full
capture-to-render CPU copy, using non-blocking triple-buffered GPU readback,
and caching scaling lookup tables.
