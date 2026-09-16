# PrismMedia 4.0.0 — reusable diagnostic host

This adds live draw captures and reloadable PowerShell scripts. It is a
diagnostic update, not a confirmed fix for the separate smartphone GPS.
The custom media display working while the native GPS is black is the problem
being investigated. The global fallback remains a comparison control.

## Install once, then change scripts

Build the x64 Release solution with the included GitHub Actions workflow or
Visual Studio 2022. Install the resulting PrismMedia.dll as usual and restart
ETS2/ATS once. Keep PrismDiagnosticConsole.exe and its `diagnostics` folder
together. Both are included in the Actions artifact and console build output.
This source ZIP does not contain compiled executables.

Park the truck, show both screens, run PrismDiagnosticConsole.exe, and enter:

```text
capabilities
script gps-ab.ps1
```

The first response must include `liveDraw=1` and `fallbackLease=1`. The script
tests fallback on, off, and on again. It allows eight seconds to return to the
cockpit before each phase, captures current draw state, and asks what happened
to CUSTOM MEDIA and SMARTPHONE GPS separately. It does not request a reload.
Type an observation and press Enter; Escape aborts a prompt. Keep the truck,
camera, and accessories unchanged throughout the comparison.

Results and an automatic ZIP are saved under `Documents\PrismMedia-Diagnostics`.
Send that ZIP plus the current PrismMedia.log. Keep the game
running while copying the files. The starting fallback mode is restored when
the script finishes or errors. A lost script has a 120-second fallback lease;
restoration runs on the next telemetry update after expiry if the game is paused.
Live captures expire after at most 30 seconds and are cleaned up by the pipe
worker even when telemetry is paused. `abort` stops a capture and restores a
script lease. A second console can send it during a script.

PowerShell 5.1 runs test files outside the game. The launcher respects the
machine's script execution policy and does not change it. If PowerShell refuses
scripts, its error explains the policy/signature requirement; direct capture
commands remain available. Do not change a managed machine's policy to run this.

## Change tests without recompiling

Edit a file under `diagnostics`, save, then run `script <name.ps1>` again.
Absolute script paths (including quoted paths with spaces) also work. Test
files are loaded fresh. Scripts have ordinary PowerShell variables, loops,
conditions, functions, JSON analysis and file output. The runner exposes:

| Function | Use |
| --- | --- |
| `Send-Prism 'command'` | Send any host command and return its response; errors throw. |
| `Get-PrismCapture -Name test -Seconds 5 -Options 'slot=6 every=7'` | Capture, wait, fetch all rows, save JSON and return the result. |
| `Wait-Prism 8` | Delay while renewing the fallback lease. |
| `Read-PrismObservation 'Question'` | Prompt while renewing the lease. Escape aborts. |
| `Compare-PrismCaptures $before $after` | Report candidate geometry/state differences. |
| `Update-PrismLease` | Renew before lengthy custom work. |

`$PrismOutputDirectory` identifies the result folder. The runner uses `try/finally`
to stop captures and restore fallback. Use `Wait-Prism` instead of long sleeps.
Example test file:

```powershell
foreach ($slot in @(0, 1, 6, 7)) {
    Get-PrismCapture -Name "slot-$slot" -Seconds 3 `
        -Options "slot=$slot every=7 limit=256" | Out-Null
}
```

`script survey-slots.ps1` surveys slots 0–15 without changing fallback. Change
the slot list to any subset of 0–127, or add resource, shader, geometry or caller
filters. Scripts change test logic; they cannot install an arbitrary new native
detour. A genuinely new low-level hook still needs compiled code. There is no
hot-loaded native-DLL or arbitrary game-memory patching interface in this host.

## Live capture commands

```text
capture start my-test 5 slot=6 every=7 limit=512 budget=30000
capture status
capture stop
capture row 0
```

`capture status` is JSON. When `active` is false, `rows` gives the number of
stored rows (indices 0 through rows−1). Fetch/export results before starting
another capture; the host stores one bounded capture at a time. Scripts do this
automatically. `capture stop` preserves accumulated results.

| Option | Meaning / allowed range |
| --- | --- |
| label, seconds | Label: 1–64 letters/digits/underscore/hyphen; duration: 1–30 seconds. |
| `slot` | PS slot whose resource description is inspected, 0–127; default 6. All 128 bound SRV identities are also recorded. |
| `every` | Inspect every Nth observed draw, 1–10000; default 1. Higher values reduce cost and coverage. |
| `limit` | Maximum distinct stored states, 1–512; default 256. Further distinct states increment overflow. |
| `budget` | Maximum candidate inspections, 1–100000; default 30000. Reaching it ends the capture early. |
| `width`, `height` | Filter selected-slot texture dimensions; 0 disables, max 16384. |
| `resource` | Filter selected-slot resource pointer; use `displays` or a previous capture. Supplied addresses are never dereferenced. |
| `ps`, `ib` | Filter pixel-shader or index-buffer identities from previous rows. |
| `count` | Filter draw vertex/index count; 0 disables. |
| `caller` | Filter draw return address as RVA from the game executable base; 0 disables. |

Numeric options accept decimal or `0x` hex. Duplicate/unknown options,
negative values and out-of-range inputs are rejected before enabling hooks.
Pointer identities apply only to the current game session and may be reused
after object destruction. Refresh filters after a reload or restart.

Rows include context type, draw arguments, VS/PS/GS/HS/DS identities, input
layout, index buffer and vertex-buffer slot 0, texture/resource and view formats,
eight render targets and depth target, blend/depth/raster state, viewports,
scissors, all PS SRVs, PS sampler and constant-buffer identities. Null state
descriptions mean D3D defaults, not zero-valued settings. Getters temporarily
acquire COM references and release them in the callback; results retain copied
values only. There is no GPU pixel readback, shader disassembly, constant-buffer
content dump or screenshot in this revision. Deferred context rows describe
recorded commands; they do not prove those commands were executed.

Sampling can miss draws and limits can end captures early. Check `reason`,
`overflow`, `busySkipped`, `inspected` and `matched`. The known-caller subtest in
gps-ab.ps1 uses the older log's `0x2A62BF` RVA; zero hits on a changed game version
do not establish that the screen was never rendered. Broad surveys run too.
Geometry matches and comparison differences are candidate evidence, not an
automatic identification of the native phone or proof of the root cause.
State inspection has temporary overhead; these captures are not FPS benchmarks.

## Other commands retained

`status`, `displays`, `snapshot`, `fallback auto|on|off`, `run auto|<display-id>`,
`abort`, `reset`, `set release_window_us <1000..5000000>`, `ping`, `help`, `quit`.
`run` is the older branch/list/Release diagnostic and requests a texture reload;
it is not used by the new comparison. Run one type of capture at a time.

`mark <label>` labels the log. `lease begin <10..300 seconds>`, `lease renew
<seconds>` and `lease end` expose restoration to custom clients. The runner
manages these automatically.

The Windows workflow compiles the full solution and runs a WARP capture smoke
test, parser checks and a named-pipe/script test before packaging. Actual ETS2
behavior and smartphone mapping still require the in-game comparison.
