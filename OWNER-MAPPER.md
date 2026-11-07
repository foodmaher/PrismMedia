# PrismMedia 4.0.0 — accessory owner mapper

The owner mapper is a bounded runtime diagnostic for connecting ETS2 addon
objects to their native 1024x1024 GPS resources. It does not suppress addon
updates, replace a resource, or select a route automatically.

Normal routing is now sticky. Once the visual picker activates an exact
resource route, head movement, an exterior camera, a temporarily hidden screen
and a short pause do not trigger white/black probes or candidate retraining.
If the resource is genuinely recreated, PrismMedia leaves every screen native
until ownership is confirmed or the user explicitly runs the picker again.

## One-build workflow

Run `PrismDiagnosticConsole.exe` while ETS2 is in the cockpit:

```text
capabilities
owner clear
owner arm 10
owner status
owner list
owner scan 0
owner watch 0
route retrain
owner resources
route candidates
owner dump
```

Wait until `owner status` reports `capture=0` before `owner list`. Repeat
`owner scan`, `owner watch`, `route retrain`, and `owner resources` for every
owner candidate. No DLL rebuild is required between candidates.

`owner resources` reports:

- `exact`: the final draw occurred while the watched addon call was on the
  current thread's call stack. This is a strong owner relationship.
- `near`: the draw occurred on the same thread within eight milliseconds after
  the watched call. This is diagnostic evidence only and is never allowed to
  activate routing by itself.

Compare each reported resource identity with `route candidates`. `owner scan`
also performs a guarded, bounded scan of the live addon object and descriptor
for useful ASCII definition, UI, `.sii`, `.tobj`, GPS or vehicle paths. It does
not follow an unbounded pointer graph.

After owner diagnostics, press **Pick truck screen** once to restore an active
media route. The owner mapper never guesses: zero or multiple relationships
leave native GPS processing unchanged.

The expected capability markers are:

```text
perDrawRoute=6 hookTransitionGuard=5 pickerInput=4 routeRecovery=2
ownerMapper=1 stickyRoute=1
```

