# Independent Prism3D render submission test

Version: `3.11.16-independent-target-snapshot`

This build validates the engine-owned render-job path discovered in the exact
ETS2 executable with PE timestamp `0x6A426DE5` and image size `0x0382D000`.

The **Run diagnosis** action submits one additional Prism3D render task via RVA
`0x00722BA0`. The task owns its own `0x28590`-byte renderer state and is
executed through RVA `0x00722B80` into the scene renderer at RVA `0x00722020`.
Only one task is submitted per diagnosis run.

Before submission, normal mirror scheduling is paused for 150 ms so previously
queued mirror copies can drain. The independent job's final 256x256 RGBA16F
copy is then snapshotted into a plugin-owned shader texture. Normal mirror
scheduling resumes immediately, and later mirror/player renders cannot alter
the owned snapshot.

Expected `game.log.txt` messages:

```text
independent submit=ready
Independent Prism3D render task submitted
Independent Prism3D render task entered the engine renderer
Independent output isolated into the plugin-owned target
Independent submit attempted=yes succeeded=yes dispatched=1; isolated-output=yes copies=1
```

This validation still reuses one live park command as an immutable camera-state
template. After diagnosis, the GPS should display the plugin-owned snapshot and
remain stable when left/right mirrors or the player camera change. The snapshot
is intentionally one frame; continuous independent updates are enabled only
after this isolated frame is visually confirmed.

The cursor, audio, Spotify/Web Helper, and DirectInput behavior are unchanged
from the supplied working source.
