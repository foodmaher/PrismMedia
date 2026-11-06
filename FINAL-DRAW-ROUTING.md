# PrismMedia 4.0.0 — final-draw GPS routing

RenderDoc confirmed that the left and right addon GPS surfaces sample separate
1024×1024 R8G8B8A8 resources from pixel-shader slot 6. They share the same
pixel shader, but their final draw signatures and source resources differ.

This build therefore leaves the game's addon update and every unmatched screen
untouched. A physical GPS can have more than one shader pass, so selection
groups every stable pass that shares that screen's native resource and draw
selector. For each grouped pass it saves the native slot-6 SRV, binds the Prism
media SRV, executes the original draw once, and immediately restores the native
SRV. This prevents a later native pass from drawing GPS over the media. It does
not use the global compatibility branch or skip native addon processing.

## Recommended visual picker (no TOBJ selection)

1. Add or select one custom Prism display and choose its media.
2. In **System > Truck screen routing**, enable **Use visual truck-screen
   picker** and press **Pick truck screen**.
3. Keep the cockpit and all candidate GPS screens visible, then double-click
   near the centre of the screen that should show this media.
4. PrismMedia briefly measures that clicked point while substituting white and
   black probes into one physical candidate at a time. It never saves an
   ambiguous result.
5. Confirm the three-second media preview. If the automatic response is not
   unique, use the numbered fallback included in the same build.

The visual path creates a private dynamic media texture and therefore does not
need a game `.tobj` path or a truck-texture reload. Only the selected grouped
final draw receives that texture; every unmatched screen keeps its native SRV.

The old console calibration remains available for expert recovery. `capabilities`
for this source must report
`perDrawRoute=5 groupedPassRouting=1 visualPicker=1 privateMediaTexture=1`.

The selected stable draw arguments are saved with the custom display. Pointer
identities are never saved; after a game or truck reload, the router observes
the new resources and restores the saved selection only when it has one unique
match. If a game/mod update changes the draw, enter `route clear` and rerun the
script. No DLL rebuild is needed.

Legacy manual equivalents are `route candidates`, `route preview <n> 10`,
`route preview off`, and `route select <n>`.
