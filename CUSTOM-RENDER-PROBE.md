# Custom-display per-instance diagnostic

This source revision contains a bounded early diagnostic probe for the global
custom-display render branch. Runtime testing of the previous revision showed
that arming only after the exact GPU texture match was too late (`events=0`).
The revised probe arms before the truck/accessory reload, retains the branch's
real register and stack identities, then correlates those identities after the
prepared display's exact Direct3D texture is created.

The current 4.0.0 diagnostic incorporates the result of that first wide run.
The empty-list JE occurs roughly 22 seconds before the exact texture draw and
its `R9` value is stale. The new second breakpoint is placed on the verified
`mov r9,rsi` instruction inside the list loop. It emulates that instruction,
captures every real list slot, `[RSI]`, `[[RSI]]`, `[[RSI]+8]`, adjacent entry
words, and before/match/draw fingerprints, then correlates them with the exact
slot-6 Direct3D draw. The earlier branch, exact SRV identity, independent
bound-resource inspection, all seven D3D11 draw variants and call stacks remain
active as separate checks in the same run.

## Test

1. Build and install `PrismMedia.dll` normally.
2. Start the game with the primary GPS and at least one native cabin accessory
   screen visible.
3. Enable media replacement on one custom display and make sure its unique game
   TOBJ is selected.
4. While driving, open **System** and select **Run final list-entry
   diagnostic** once.
   Do not use the ordinary game console reload command, because the probe must
   be installed first.
5. Wait until the truck finishes reloading and the custom screen returns, then
   close the game normally.
6. Provide `PrismMedia.log` from the game executable directory.

The diagnostic action synchronously installs both breakpoints before the
reload. It is bounded to 2,048 branch executions, 256 post-R9 list entries,
60 seconds overall, and 10 seconds after the exact texture appears. Before the
match it preserves the original branch decision. After the match it temporarily
emulates the working compatibility jump so the selected texture can reach the
SRV/bind/draw hooks. Six correlated draw samples complete the test early.
Temporary high-frequency hooks and both breakpoints are then removed, and the
working global fallback returns.

Useful log records use the `[probe]` category:

- `Preparing early per-instance render capture`
- `Per-instance custom render capture started`
- `Early branch capture phase completed`
- `Exact custom route matched for the prepared early capture`
- `Per-instance capture completed`
- `branch-code[...]`
- `List-entry correlation summary`
- `list-entry[...]`
- `DX correlation summary`
- `DX sample[...]`
- `signature[...]`

Each list entry reports the owning branch, list ordinal, verified `R9 == RSI`,
`RAX == [RSI]`, four slot words, eight entry words, memory fingerprints at
capture/texture-match/draw time, and any exact texture path found through the
slot, entry, next slot, or an entry-word child. Each DX sample reports whether
the tracked SRV or independent resource inspection found the texture, the
pixel-shader slot, following draw type, and bind/draw call stacks.

The probe is diagnostic only. The final selective detour must be based on the
captured runtime identities; this build deliberately does not guess an object
layout or force only one unverified pointer.
