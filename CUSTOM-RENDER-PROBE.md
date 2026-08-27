# Custom-display targeted-fix test

This source revision contains a bounded early probe for the global
custom-display render branch. Runtime testing of the previous revision showed
that arming only after the exact GPU texture match was too late (`events=0`).
The revised probe arms before the truck/accessory reload, retains the branch's
real register and stack identities, then correlates those identities after the
prepared display's exact Direct3D texture is created.

The current 4.0.0 targeted-fix test incorporates the next runtime result. The
loop captured two cleanup entries, and both had their first two fields cleared
roughly 20 seconds before the new texture existed. Therefore the relevant
identity is the old live texture from before reload, not the replacement
texture created afterward.

The test now carries both the raw old `ID3D11Texture2D*` and its canonical
`IUnknown` identity into the verified `mov r9,rsi` loop. It does not retain an
extra COM reference. Each entry is searched independently through a bounded
three-child pointer graph. Only an exact old-texture pointer match can mark an
entry for skipping. Even then, skipping is enabled only if the plugin also
verifies the game's native `add/lea RSI,+8`, compare, and backedge sequence;
the handler rejoins that native sequence instead of guessing loop-exit register
state. Unmatched entries always execute normally.

## Test

1. Build and install `PrismMedia.dll` normally.
2. Start the game with the primary GPS and at least one native cabin accessory
   screen visible.
3. Enable media replacement on one custom display and make sure its unique game
   TOBJ is selected.
4. While driving, open **System** and select **Run targeted fix test** once.
   Do not use the ordinary game console reload command, because the probe must
   be installed first.
5. Wait until the truck finishes reloading and the custom screen returns, then
   close the game normally.
6. Provide `PrismMedia.log` from the game executable directory.

The action synchronously records the old live texture and installs both
breakpoints before reload. It is bounded to 2,048 branch executions, 256
post-R9 list entries, 60 seconds overall, and 10 seconds after the replacement
texture appears. Exact old-texture matches selectively bypass only their
cleanup bodies through the verified native loop advance. Six correlated draws
complete the test early. Temporary hooks and both breakpoints are then removed,
and the working global fallback returns regardless of the match result.

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
`RAX == [RSI]`, captured fields, fingerprints, old-texture path depth and
offsets, whether the raw or canonical identity matched, and whether the entry
was selectively skipped. Each DX sample still confirms the replacement texture
on pixel-shader slot 6 and its following draw.

This is a guarded targeted-fix test. No entry is skipped on a heuristic,
fingerprint, ordinal, or guessed layout. Failure to verify either the exact old
texture path or the game's native advance sequence leaves original behavior
untouched and restores the existing compatibility fallback.
