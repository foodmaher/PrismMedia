# PrismTextureStreamerFB 4.0.0 performance notes

| Mode | Typical impact | Best use |
|---|---:|---|
| Native direct media | Lowest | Local files and direct media streams |
| Integrated media | Low to medium | YouTube and Spotify Web |
| Window capture (WGC) | Medium | Any visible desktop application |
| Compatibility capture | Medium to high | Windows that WGC cannot capture |

The recommended profile is 1280×720 at 60 FPS. Lower the profile before
changing game graphics if GPU frame time is already near its limit.

Adaptive audio runs at 50 Hz in the helper. Gain and pan converge over roughly
650 ms, and Web Audio low-pass filters use the same 220 ms time constant. This
avoids audible discontinuities while remaining far below one millisecond of CPU
work per update on typical systems. Engine-off and Pause/Freeze bypass the
fade in the downward direction so playback becomes silent immediately; the
smooth curve is retained when audio resumes.

Automatic brightness samples game lighting asynchronously at half the main GPS
display rate and only while a screen requests it. Media brightness is normally
handled in WebView2’s GPU compositor; other sources use the existing CPU
fallback. The output follows a phone-like asymmetric curve: about 2.5 seconds
to brighten and 4 seconds to dim.

The optional SPF bridge now publishes only camera, truck, and trailer placement.
AI traffic enumeration and the traffic-radio helper were removed, reducing
shared-memory size and eliminating that periodic vehicle scan.

The UI animates only while open. Auto-save is debounced after interaction, so
sliders do not write the configuration on every frame. The Live Performance
page snapshots source data at 4 Hz and the logical-processor map at 2 Hz instead
of querying them at the game's frame rate. Its FPS-loss estimate includes only
measured render-thread upload work, not the monitor's own ImGui draw. Controller
poll results are shared by media bindings and ImGui within each frame; Steam,
XInput, and WinMM are therefore not polled twice while the console is open.
