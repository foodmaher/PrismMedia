# Prism call-path trace test

This build removes the legacy internal-camera runtime from execution. It does
not install an extra game camera, modify the camera-active mask, capture a park
texture, perform diagnostic readback, or upload diagnostic pixels to the GPS.

The diagnostic uses an ordinary non-park mirror command only as a control
template. Prism3D's own submission routine creates a separate task from that
template. For ten seconds the plugin compares the native and submitted paths:

- verified mirror-worker, scheduler, submit-constructor and dispatch boundaries;
- one bounded call stack for the native control, nested submit and confirmed task;
- hashes of the command, owner and render-context objects;
- the exact task pointer returned by the submit constructor;
- render-target and depth-target bindings;
- viewports;
- vertex-shader constant-buffer bindings;
- compressed draw batches on the primary GPU submission thread;
- resource updates, copies, resolves, and command-list execution.

Draw calls are aggregated instead of logged individually. This prevents the
multi-million-event overflow seen in 3.11.25 while retaining frame timing,
target transitions, draw counts and a sequence hash.

## Test

1. Build and install the DLL and its companion folder.
2. Load into the truck and wait until the scene is stable.
3. Open Camera Lab and press **Start new diagnostic** once.
4. Do not change cameras or open mirrors for ten seconds.
5. Wait until Camera Lab says **GPU trace saved**.
6. Send both files:

   - `Documents\ETS2\PrismIndependentGpuTrace.bin`
   - `Documents\ETS2\PrismIndependentGpuTrace.txt`

The binary trace contains only API identities, dimensions/counts, timestamps,
thread IDs, and call arguments. It does not contain texture pixels or executable
bytes. Each run overwrites the previous trace.

The source package also contains `tools/analyse_gpu_trace.py` for deterministic
offline summaries. It is not required on the gaming PC.
