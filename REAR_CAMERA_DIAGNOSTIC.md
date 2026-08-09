# Rear-camera one-run diagnostic

This build observes both D3D11 output-merger target-binding paths plus
`CopySubresourceRegion`, `CopyResource`, and `ResolveSubresource` on ETS2's
actual device context. Broad recording is enabled only during the trace, and
slot 7 remains at its configured frame rate.

## Test sequence

1. Load into the truck and open the plugin menu.
2. Enable the internal park-camera method and Preview.
3. Select **Start 20-second comprehensive RTT trace**.
4. During the timer: enter reverse, show the left mirror, show the right
   mirror, switch to camera 2 and return, then rotate the interior view.
5. Allow the timer to finish and exit ETS2 normally.
6. Provide `Documents/Euro Truck Simulator 2/game.log.txt`.

## Result fields

- `api-om/uav`: standard `OMSetRenderTargets` binds versus
  `OMSetRenderTargetsAndUnorderedAccessViews` binds.
- `slot7-during/after`: binds inside the exact slot-7 render call versus the
  following 100 ms correlation window.
- `threads`: first and last Windows thread IDs that bound the resource.
- `slot-match`: descriptor-size matches for the engine camera slots.
- `[RTT lineage]`: camera-sized resource movement shown as source to
  destination, with separate region/copy/resolve counters.

The 100 ms post-job window is diagnostic-only. A target seen only in that
window cannot become the live reverse-camera source.
