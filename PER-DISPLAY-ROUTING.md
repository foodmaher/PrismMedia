# PrismMedia 4.0.0 — per-display routing validation

The completed isolation test showed the same configured accessory geometry in
both modes:

- compatibility fallback on: PrismMedia's 1280×720 resource was drawn;
- compatibility fallback off: the game's native 1024×1024 GPS resource was
  drawn on the same geometry.

That proves media capture and upload are healthy, while the global fallback is
what affects unrelated GPS accessories. ETS2 1.62 no longer matches the legacy
global branch and the original router observed no matching `DrawIndexed` call.
This revision therefore does not depend on that branch. A RenderDoc capture
confirmed that the two visible addon GPS surfaces use independent 1024x1024
R8G8B8A8 SRVs at pixel-shader slot 6. The router discovers those final draws,
previews one candidate at a time, and saves the explicitly selected draw.

## One validation run

1. Build and install `Release|x64` while keeping version 4.0.0.
2. Start ETS2 and enter the same truck with one enabled custom display.
3. Keep the custom display and the extra smartphone visible for 3–5 seconds.
4. Open PrismMedia's System page, enable the visual truck-screen picker, press
   **Pick truck screen**, and double-click the intended screen in the cockpit.
5. Confirm the selected display shows media and every unmatched GPS/smartphone
   shows its original game content, then enter `route status`.

The optional console `capabilities` command should report
`perDrawRoute=5 groupedPassRouting=1 visualPicker=1 privateMediaTexture=1` so an older 4.0.0 DLL
cannot be mistaken for this revision. Success is `detail=active` with an
increasing `routed` count. The selection is stored in the normal 4.0.0 config;
new runtime resource identities are rediscovered automatically after restart.

If it reports `fallback`, copy `PrismMedia.log`, `route candidates`, and the
complete `route status` line. Original GPS/smartphone displays remain enabled.
Use the visual picker again when a truck/mod update changes the stable final
draw; rebuilding the plugin is not required. Its automatic click probe accepts
only a clear white/black pixel response. Unsupported backbuffer formats and
ambiguous clicks go directly to the numbered fallback without saving a route.

The current router intentionally learns one enabled custom display at a time.
With two or more enabled custom displays it leaves original displays untouched
rather than risk routing media to the wrong accessory.
