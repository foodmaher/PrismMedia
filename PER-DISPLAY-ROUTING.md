# PrismMedia 4.0.0 — per-display routing validation

The completed isolation test showed the same configured accessory geometry in
both modes:

- compatibility fallback on: PrismMedia's 1280×720 resource was drawn;
- compatibility fallback off: the game's native 1024×1024 GPS resource was
  drawn on the same geometry.

That proves media capture and upload are healthy, while the global fallback is
what affects unrelated GPS accessories. ETS2 1.62 no longer matches the legacy
global branch and the original router observed no matching `DrawIndexed` call.
This revision therefore does not depend on that branch: it traces the custom
media resource through one UI-compositor render target, observes all seven
D3D11 draw variants and learns the final draw plus pixel-shader resource slot.

## One validation run

1. Build and install `Release|x64` while keeping version 4.0.0.
2. Start ETS2 and enter the same truck with one enabled custom display.
3. Keep the custom display and the extra smartphone visible for 3–5 seconds.
4. Confirm the configured display shows media and the extra smartphone shows
   its original game GPS instead of black/static replacement content.
5. Optionally open `PrismDiagnosticConsole.exe` and enter `route status`.

First enter `capabilities` and confirm `perDrawRoute=2 compositorTrace=1` so an
older 4.0.0 DLL cannot be mistaken for this revision. Success is `detail=active`
with an increasing `routed` count. Training is automatic, does not issue the
game's texture-reload command and keeps the legacy global fallback off.

If it reports `fallback`, copy `PrismMedia.log` and the complete `route status`
line. Its bounded trace counters (`traceDirect`, `traceTargets`, `traceDerived`,
`candidates`, and observed output sizes) identify in one run whether the custom
texture reached the compositor, produced a target, or reached a final draw.
Original GPS/smartphone displays remain enabled. If the truck was reconfigured
or reloaded, `route retrain` starts a fresh bounded learning window without
rebuilding.

The current router intentionally learns one enabled custom display at a time.
With two or more enabled custom displays it leaves original displays untouched
rather than risk routing media to the wrong accessory.
