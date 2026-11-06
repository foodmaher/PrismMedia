# PrismMedia 4.0.0 — final-draw GPS routing

RenderDoc confirmed that the left and right addon GPS surfaces sample separate
1024×1024 R8G8B8A8 resources from pixel-shader slot 6. They share the same
pixel shader, but their final draw signatures and source resources differ.

This build therefore leaves the game's addon update and every unmatched screen
untouched. For one selected final draw only, it saves the native slot-6 SRV,
binds the Prism media SRV, executes the original draw once, and immediately
restores the native SRV. It does not use the global compatibility branch and it
does not skip native addon processing.

## One-time calibration

1. Keep exactly one Prism custom display enabled and make all candidate GPS
   screens visible in the cockpit.
2. Start `PrismDiagnosticConsole.exe` while the game is running.
3. Enter `capabilities` and require
   `perDrawRoute=3 finalDrawSwap=1 routeCalibration=1`.
4. Enter `script gps-final-draw.ps1`.
5. During each ten-second preview, report which one physical screen shows the
   Prism media. Enter that candidate number when prompted.
6. Enter `route status`; success is `detail=active` and an increasing `routed`
   count while every unmatched GPS remains native.

The selected stable draw arguments are saved with the custom display. Pointer
identities are never saved; after a game or truck reload, the router observes
the new resources and restores the saved selection only when it has one unique
match. If a game/mod update changes the draw, enter `route clear` and rerun the
script. No DLL rebuild is needed.

Manual equivalents are `route candidates`, `route preview <n> 10`,
`route preview off`, and `route select <n>`.
