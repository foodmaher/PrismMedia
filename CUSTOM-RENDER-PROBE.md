# Custom-display per-instance diagnostic

This source revision contains a bounded early diagnostic probe for the global
custom-display render branch. Runtime testing of the previous revision showed
that arming only after the exact GPU texture match was too late (`events=0`).
The revised probe arms before the truck/accessory reload, retains the branch's
real register and stack identities, then correlates those identities after the
prepared display's exact Direct3D texture is created.

The current 4.0.0 diagnostic records the complete instruction window in one
queue entry, separates signatures before and after the exact texture match,
clears the observed `+2` tag from `R9`, scans each likely object through 512
bytes, and follows one bounded child level. It also runs several independent
Direct3D correlations in the same test: exact SRV creation, bound-resource
identity inspection, pixel-shader binding, all seven Direct3D 11 draw-call
variants, game call stacks, nearest-branch timing, and object fingerprints.

## Test

1. Build and install `PrismMedia.dll` normally.
2. Start the game with the primary GPS and at least one native cabin accessory
   screen visible.
3. Enable media replacement on one custom display and make sure its unique game
   TOBJ is selected.
4. While driving, open **System** and select **Run wide render diagnostic**
   once.
   Do not use the ordinary game console reload command, because the probe must
   be installed first.
5. Wait until the truck finishes reloading and the custom screen returns, then
   close the game normally.
6. Provide `PrismMedia.log` from the game executable directory.

The diagnostic action synchronously installs the probe before the reload. It is
bounded to 2,048 branch executions, 60 seconds overall, and 10 seconds after
the exact texture appears. Before the match it preserves the original branch
decision. After the match it temporarily emulates the working compatibility
jump so the selected texture can reach the SRV/bind/draw hooks. Six correlated
draw samples complete the test early. Temporary high-frequency hooks and the
exception handler are then removed, and the working global fallback returns.

Useful log records use the `[probe]` category:

- `Preparing early per-instance render capture`
- `Per-instance custom render capture started`
- `Early branch capture phase completed`
- `Exact custom route matched for the prepared early capture`
- `Per-instance capture completed`
- `branch-code[...]`
- `DX correlation summary`
- `DX sample[...]`
- `signature[...]`

Each DX sample reports whether the exact tracked SRV or independent resource
inspection found the texture, the pixel-shader slot, the following draw type,
bind/draw call stacks, the nearest branch timing and decision, tag-cleared R9,
and memory fingerprints. The older register/object scan is retained as another
independent result rather than being trusted by itself.

The probe is diagnostic only. The final selective detour must be based on the
captured runtime identities; this build deliberately does not guess an object
layout or force only one unverified pointer.
