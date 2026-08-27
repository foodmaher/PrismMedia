# Custom-display per-instance diagnostic

This source revision contains a bounded diagnostic probe for the global custom
display render branch. It is intended to determine whether ETS2/ATS exposes a
stable screen, material, or texture-related pointer that can replace the legacy
global `JE` to `JMP` patch with a selective per-display detour.

## Test

1. Build and install `PrismMedia.dll` normally.
2. Start the game with the primary GPS and at least one native cabin accessory
   screen visible.
3. Enable media replacement on one custom display and reload installed truck
   textures once.
4. Wait at least three seconds, then close the game normally.
5. Provide `PrismMedia.log` from the game executable directory.

The probe starts automatically after the first exact custom TOBJ/GPU texture
match in each plugin session. For at most 192 branch executions or 1.5 seconds,
it restores and emulates the game's original conditional branch while recording
CPU register and stack identities. It then removes itself and restores the
working global compatibility patch, so custom media should return after the
brief diagnostic window.

Useful log records use the `[probe]` category:

- `Queued one per-instance render capture`
- `Per-instance custom render capture started`
- `Per-instance capture completed`
- `signature[...]`

Each signature includes the original branch decision, relevant registers, an
exact match against the selected Direct3D texture pointer, and whether a likely
object register contains that texture pointer within its first 256 bytes.

The probe is diagnostic only. The final selective detour must be based on the
captured runtime identities; this build deliberately does not guess an object
layout or force only one unverified pointer.
