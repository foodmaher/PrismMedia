# Independent Prism3D render submission test

Version: `3.11.19-independent-snapshot-lifetime`

This build validates the engine-owned render-job path discovered in the exact
ETS2 executable with PE timestamp `0x6A426DE5` and image size `0x0382D000`.

The **Run diagnosis** action submits one additional Prism3D render task via RVA
`0x00722BA0`. The task owns its own `0x28590`-byte renderer state and is
executed through RVA `0x00722B80` into the scene renderer at RVA `0x00722020`.
Only one task is submitted per diagnosis run.

Normal mirror scheduling is not paused. Prism3D performs the task's D3D work
asynchronously and reuses a shared engine render resource, so a unique target
identity is not always exposed while the dispatch hook is active. The plugin
first accepts a directly tagged task target when available; otherwise it
accepts only the first final 256x256 RGBA16F copy within 250 ms of the confirmed
independent dispatch. The verified runs produced that copy after 16-82 ms. The
result is immediately snapshotted into a plugin-owned shader texture.

The legacy A/B/C/D candidate selector is diagnostic-only in this build. It can
observe and log changing mirror copy paths, but none can become the GPS image.
If the independent copy is not proven, the display remains waiting rather than
silently falling back to slot 7.

The plugin-owned snapshot has a separate lifetime from the game's mirror and
visual-interior resources. A camera or mirror resource rebuild drops only the
legacy observations and preserves the owned snapshot. Full plugin shutdown,
disabling park rendering, or starting a new diagnosis still releases it.

Expected `game.log.txt` messages:

```text
independent submit=ready
Independent Prism3D render task submitted
Independent Prism3D render task entered the engine renderer
Independent output isolated into the plugin-owned target
Independent submit attempted=yes succeeded=yes dispatched=1; isolated-output=yes copies=1 correlated-copies=1 tagged-targets=0
Game mirror resources rebuilt; preserved the plugin-owned independent snapshot
```

This validation still seeds one immutable render command from the custom park
camera path. After diagnosis, the GPS must display only the plugin-owned
snapshot and remain stable when left/right mirrors or the player camera change.
The snapshot is intentionally one frame; continuous independent updates and
fully synthetic camera-matrix updates come only after strict source ownership
is visually confirmed.

The cursor, audio, Spotify/Web Helper, and DirectInput behavior are unchanged
from the supplied working source.
