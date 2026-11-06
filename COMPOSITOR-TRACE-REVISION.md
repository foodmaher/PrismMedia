# PrismMedia 4.0.0 — compositor-trace revision

This historical note describes the compositor-trace experiment. The current
4.0.0 source supersedes automatic compositor activation with the safer,
RenderDoc-confirmed final-draw calibration in `FINAL-DRAW-ROUTING.md`.

## What changed

- Training observes `Draw`, `DrawIndexed`, both instanced forms, `DrawAuto`,
  and both indirect forms.
- The configured custom media resource is tracked across all 128 pixel-shader
  resource slots.
- A same-size render target written while sampling that resource is treated as
  one UI-compositor hop. A stable final draw sampling that target is learned.
- Active mode enables only the learned draw detour, verifies the learned native
  resource and state, substitutes the media SRV for one draw, then restores the
  original SRV immediately.
- Training keeps the unsupported legacy global fallback off, so unrelated
  smartphone and GPS displays retain their original game content.
- The superseding build reports
  `perDrawRoute=3 finalDrawSwap=1 routeCalibration=1`.
- A failed `route status` includes `traceDirect`, `traceTargets`,
  `traceDerived`, `candidates`, and observed output sizes so one run identifies
  which stage was not observed.

## One-run validation

1. Build `Release|x64` with Visual Studio 2022 or the included Windows workflow.
2. Install only the newly built `PrismMedia.dll`; move renamed/old Prism DLLs
   outside the game plugin folder, then restart the game.
3. Enable exactly one custom display and keep it visible in the truck for at
   least 15 seconds.
4. Run `capabilities`; require
   `perDrawRoute=3 finalDrawSwap=1 routeCalibration=1`.
5. Run `route status` twice. Success is `detail=active` with an increasing
   `routed` value while unconfigured GPS/smartphone screens stay native.

If routing does not activate, send the complete `route status` line and the
current `PrismMedia.log`. Do not run the legacy fallback comparison first.

## Validation completed in the authoring workspace

- Portable diagnostic-option tests passed with GCC and warnings as errors.
- The new router passed a standalone C++17 syntax check with D3D/Windows test
  stubs and warnings as errors.
- Project XML and workflow YAML parsed successfully.
- Static integration checks confirmed seven `begin_draw`/`end_draw` paths and
  the new console capability marker.

The Windows SDK build and actual ETS2 interception still require the included
Windows build workflow and one in-game run.
