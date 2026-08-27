# Custom-display per-instance diagnostic

This source revision contains a bounded early diagnostic probe for the global
custom-display render branch. Runtime testing of the previous revision showed
that arming only after the exact GPU texture match was too late (`events=0`).
The revised probe arms before the truck/accessory reload, retains the branch's
real register and stack identities, then correlates those identities after the
prepared display's exact Direct3D texture is created.

## Test

1. Build and install `PrismMedia.dll` normally.
2. Start the game with the primary GPS and at least one native cabin accessory
   screen visible.
3. Enable media replacement on one custom display and make sure its unique game
   TOBJ is selected.
4. While driving, open **System** and select **Run early render diagnostic**.
   Do not use the ordinary game console reload command, because the probe must
   be installed first.
5. Wait until the truck finishes reloading and the custom screen returns, then
   close the game normally.
6. Provide `PrismMedia.log` from the game executable directory.

The diagnostic action synchronously installs the probe before the reload. For
at most 192 branch executions or 60 seconds, it restores and emulates the
game's original conditional branch while recording CPU register and stack
identities. The exception hook is removed as soon as either bound is reached,
and the working global compatibility patch is restored. If the exact GPU
texture appears later, correlation is performed then without reinstalling the
exception hook.

Useful log records use the `[probe]` category:

- `Preparing early per-instance render capture`
- `Per-instance custom render capture started`
- `Early branch capture phase completed`
- `Exact custom route matched for the prepared early capture`
- `Per-instance capture completed`
- `signature[...]`

Each signature includes the original branch decision, relevant registers, an
exact match against the selected Direct3D texture pointer, and whether a likely
object register contains that texture pointer within its first 256 bytes.

The probe is diagnostic only. The final selective detour must be based on the
captured runtime identities; this build deliberately does not guess an object
layout or force only one unverified pointer.
