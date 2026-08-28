# PrismMedia 4.0.0 diagnostic console

`PrismDiagnosticConsole.exe` is an optional local controller for the bounded
custom-display diagnostic already built into `PrismMedia.dll`. It lets multiple
tests run during one ETS2/ATS session and changes safe probe parameters without
rebuilding the plugin.

It is not a general game scripting or memory-editing console. The plugin
accepts only the commands listed below, executes game-sensitive actions on its
telemetry thread, rejects remote pipe clients, and retains the existing event,
time, and sample limits.

## Start

1. Install `PrismMedia.dll` and the `PrismMedia` folder normally.
2. Start ETS2 or ATS and enter the truck.
3. Run `PrismDiagnosticConsole.exe` from anywhere.
4. Enter `displays`, then use `run auto` or `run <display-id>`.
5. Use `status` while the truck textures reload. The complete evidence remains
   in `PrismMedia.log` beside the game executable.

The console automatically detects `eurotrucks2.exe` or `amtrucks.exe`. If both
are running, launch it with `PrismDiagnosticConsole.exe --pid <number>`.

## Commands

| Command | Effect |
| --- | --- |
| `status` | Shows probe state, event counts, validated Release count, draw samples, timing window, and fallback mode. |
| `displays` | Lists custom-display IDs, selected TOBJ paths, and whether each display is enabled/live. |
| `run auto` | Resets completed evidence, selects the first enabled live custom display, arms the safe probe, and requests one texture reload. |
| `run <display-id>` | Runs the same test for one exact custom-display ID. |
| `abort` | Removes temporary hooks and stops the current diagnostic safely. |
| `reset` | Clears a completed/inactive diagnostic so another run is allowed. `run` does this automatically. |
| `set release_window_us <value>` | Sets the same-thread list-to-Release validity window. Allowed range: 1,000–5,000,000 microseconds. Default: 250,000. |
| `fallback auto` | Uses the global compatibility fallback only while a live custom display needs it. This is the normal setting. |
| `fallback on` | Keeps the compatibility fallback enabled for comparison. |
| `fallback off` | Restores the game’s original custom branch for comparison; custom media may disappear until changed back to `auto` or `on`. |
| `snapshot` | Writes current custom-display routing identities to `PrismMedia.log`. |
| `help` | Prints the accepted command list. |
| `quit` | Closes only the external console; the plugin continues normally. |

Commands can also be sent once from Command Prompt, for example:

```text
PrismDiagnosticConsole.exe status
PrismDiagnosticConsole.exe displays
PrismDiagnosticConsole.exe run auto
```

## Timing validation

Each exact old-texture `Release` now records both the last candidate list entry
and its elapsed time in microseconds. A Release is reported as a validated list
correlation only when it occurs on the same thread within
`release_window_us`. A stale final loop entry is retained for diagnosis but is
reported as unscoped instead of being treated as proof for a targeted fix.

The important log fields are:

- `candidateLoop`: the most recently observed entry on that thread;
- `validatedLoop`: the entry accepted by the configured timing window;
- `scopeValid`: `1` only for a timing-valid correlation;
- `loopDelta`: elapsed microseconds from entry capture to Release.

Keep `fallback auto` for normal use. `fallback off` is diagnostic-only and does
not become a saved configuration.
