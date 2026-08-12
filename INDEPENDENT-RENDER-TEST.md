# Independent Camera Lab

Version: `3.11.23-independent-camera-lab`

This build separates the experimental Prism3D camera work from the GPS display.
`PrismCameraMonitor.exe` is a companion window that receives pipeline telemetry
through a versioned local shared-memory channel. It shows the current stage,
render-job counters, a timeline, the exact failure/blocker text, and a live BGRA
preview only after both the camera state and the render target are verified as
independently owned.

## Slot 7 policy

Slot 7 is hard-disabled in the experimental path. The plugin does not install a
camera into it, force its active-mask bit, clone its render command, schedule it,
accept its texture, read it back, or send it to the GPS/monitor. Native slot-7
jobs may still be observed and counted so the monitor can prove they were
rejected. The old A/B/C/D copy selector is removed from the menu.

The normal GPS media path remains unchanged. Camera Lab never replaces it with
an unverified frame.

## Running the diagnosis

1. Install both `PrismTextureStreamerFB.dll` and the
   `PrismTextureStreamerFB` folder.
2. Open the plugin menu with Ctrl+F8.
3. Under **Automatic Reverse Camera**, open **Independent Camera Lab** and
   select **Open Camera Lab and start diagnosis**.
4. During the 20-second run, drive, switch player cameras, and show both side
   mirrors.

The companion can also start another run with **Start new diagnostic**. The
request is consumed by the plugin on its next Present frame.

## Meaning of the stages

- **Plugin ready / Slot 7 blocked**: the IPC channel is active and the legacy
  source cannot enter this path.
- **Discovering independent camera state**: Prism3D jobs are visible, but a
  safe camera constructor or writable matrix block has not been verified.
- **Independent camera state ready**: a camera object not owned by a mirror slot
  has been verified.
- **Plugin-owned render target ready / Job submitted / Renderer entered**:
  independent submission is progressing.
- **Unique render target tagged / Readback pending / Verified frame ready**:
  source ownership is proven and live monitor frames can be published.
- **Blocked / Failed**: the detail panel states the first missing prerequisite.

For the currently known ETS2 executable, the expected honest result is likely
**Blocked at custom camera-state discovery**. Earlier testing proved that GPU
copy and CPU readback work, but that test was seeded from slot 7. This build no
longer accepts that seed, so it deliberately shows where new reverse-engineering
work must continue instead of displaying the same slot-7 image again.

The cursor, audio, Spotify/Web Helper, and DirectInput behavior are unchanged
from the supplied working source.
