# Guided Camera-Memory Correlation

Version: `3.11.24-camera-memory-correlation`

This Camera Lab build performs one bounded, guided diagnosis to rank potential
Prism3D camera orientation, position, quaternion, and matrix blocks. The test is
read-only: it does not modify a game camera, submit a custom render, or replace
the GPS image.

## Slot 7 policy

Slot 7 remains hard-disabled. The plugin does not install a camera into it,
force its active bit, clone its render command, schedule it, capture it, read it
back, or display it. Native slot-7 jobs are counted only as rejected evidence.
The old A/B/C/D source paths cannot enter Camera Lab or the GPS.

## Running the guided diagnosis

Open the plugin menu with Ctrl+F8, then select **Automatic Reverse Camera →
Independent Camera Lab → Open Camera Lab and start diagnosis**. The companion
window gives one instruction at a time:

1. Centred cabin view, enlarged mirrors hidden.
2. Cabin camera rotated far left.
3. Cabin camera rotated far right.
4. Exterior player camera.
5. Cabin with the enlarged left mirror visible.
6. Cabin with the enlarged right mirror visible.
7. Return to the centred cabin view, enlarged mirrors hidden.

For each phase, create the requested view, hold it still briefly, and press
**Capture phase N and continue**. Each successful phase resets the three-minute
timeout. The plugin captures only recently observed readable objects.

## What is scanned

The engine samples writable camera-input, render-request, and render-command
objects exposed by the verified Prism3D worker/dispatch hooks. It also inspects a
strictly bounded one-pointer-depth set of writable, committed objects. Each
block is at most `0x600` bytes, there are at most 128 tracked blocks, sampling is
rate-limited, all reads are protected, and no memory is written.

After phase 7, Camera Lab ranks up to 16 non-overlapping candidates using:

- changes between left and right cabin views;
- differences in exterior and enlarged-mirror phases;
- return-to-baseline similarity in phase 7;
- plausible finite float4 values;
- unit-quaternion characteristics;
- plausible 4×4 matrix structure.

The **Candidates** tab shows address, reproducible pointer path, offset, score,
changed phases, and the latest float4 values. Completion automatically writes
`Documents\ETS2\PrismCameraLabReport.txt`; **Save report** can overwrite it
manually. Send that report for the next stage,
which will safely validate the highest-scoring offsets before any cloning or
custom rendering is attempted.

The cursor, audio, Spotify/Web Helper, and DirectInput files are unchanged from
the supplied working source.
