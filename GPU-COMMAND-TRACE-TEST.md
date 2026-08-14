# GPU command trace test

This build removes the legacy internal-camera runtime from execution. It does
not install an extra game camera, modify the camera-active mask, capture a park
texture, perform diagnostic readback, or upload diagnostic pixels to the GPS.

The diagnostic uses an ordinary non-park mirror command only as a control
template. Prism3D's own submission routine creates a separate task from that
template. For ten seconds the plugin records the surrounding D3D11 API stream:

- Prism3D job entry and exit;
- render-target and depth-target bindings;
- viewports;
- vertex-shader constant-buffer bindings;
- draw and instanced-draw calls;
- resource updates, copies, resolves, and command-list execution.

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
