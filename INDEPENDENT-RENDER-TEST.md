# Independent Prism3D render submission test

Version: `3.11.15-independent-render-submit`

This build validates the engine-owned render-job path discovered in the exact
ETS2 executable with PE timestamp `0x6A426DE5` and image size `0x0382D000`.

The **Run diagnosis** action now submits one additional Prism3D render task via
RVA `0x00722BA0`. The task owns its own `0x28590`-byte renderer state and is
executed through RVA `0x00722B80` into the scene renderer at RVA `0x00722020`.
Only one task is submitted per diagnosis run.

Expected `game.log.txt` messages:

```text
independent submit=ready
Independent Prism3D render task submitted
Independent Prism3D render task entered the engine renderer
Independent submit attempted=yes succeeded=yes dispatched=1
```

This validation intentionally reuses the live park command as an immutable
template. It does not yet redirect the new task into the plugin's dedicated
render target, so it should not visibly change the park screen. The next stage
is enabled only after the log confirms that the separately allocated job was
accepted and executed by Prism3D.

The cursor, audio, Spotify/Web Helper, and DirectInput behavior are unchanged
from the supplied working source.
