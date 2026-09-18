# PrismMedia 4.0.0 — per-display routing validation

The completed isolation test showed the same configured accessory geometry in
both modes:

- compatibility fallback on: PrismMedia's 1280×720 resource was drawn;
- compatibility fallback off: the game's native 1024×1024 GPS resource was
  drawn on the same geometry.

That proves media capture and upload are healthy, while the global fallback is
what affects unrelated GPS accessories. This build replaces that global steady
state with a learned per-draw substitution.

## One validation run

1. Build and install `Release|x64` while keeping version 4.0.0.
2. Start ETS2 and enter the same truck with one enabled custom display.
3. Keep the custom display and the extra smartphone visible for 3–5 seconds.
4. Confirm the configured display shows media and the extra smartphone shows
   its original game GPS instead of black/static replacement content.
5. Optionally open `PrismDiagnosticConsole.exe` and enter `route status`.

Success is `detail=active` with an increasing `routed` count. Training is
automatic and does not issue the game's texture-reload command.

If it reports `fallback`, copy `PrismMedia.log`; the plugin intentionally kept
the prior compatibility path. If the truck was reconfigured or reloaded,
`route retrain` starts a fresh bounded learning window without rebuilding.

The current router intentionally handles one enabled custom display. With two
or more enabled custom displays it retains compatibility mode rather than risk
routing media to the wrong accessory.
