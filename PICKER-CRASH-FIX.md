# PrismMedia 4.0.0 — visual-picker crash and input fix

The matching Release DLL and PDB identify the 2026-11-07 picker crash as a
same-thread recursive mutex lock, not an RTSS failure and not a MinHook state
transition.

The crash stack's PE section offsets resolve as follows (the `.text` section
starts at RVA `0x1000`):

| Crash offset | RVA | Resolved function |
| --- | --- | --- |
| `0x28870` | `0x29870` | `HookedCreateTexture2D` |
| `0x13A34` | `0x14A34` | `ensure_private_media_textures` |
| `0x14486` | `0x15486` | `display_picker::on_present` |
| `0x2B866` | `0x2C866` | `hooked_present` |

`ensure_private_media_textures` held `g_screens_mutex` while calling
`ID3D11Device::CreateTexture2D`. That call is intercepted by PrismMedia's own
`HookedCreateTexture2D`, which reads the screen registry under the same
non-recursive mutex. MSVC detected the recursive lock and threw through the
Present callback, closing the game.

Revision 3 uses a two-phase operation:

1. Snapshot eligible display IDs and dimensions under `g_screens_mutex`.
2. Release the mutex and create the D3D texture.
3. Reacquire the mutex, revalidate the display, and attach the texture.
4. Release the newly created texture if the display changed meanwhile.

The normal texture hook remains active, so this does not weaken texture-route
matching. The stable DrawIndexed bridge from revision 2 is also retained.

After building and installing, run `capabilities` in
`PrismDiagnosticConsole.exe`. The result must include
`hookTransitionGuard=5`. The session log must contain `Picker deadlock guard
revision 3 active` before testing **Pick truck screen**.

## Picker input revision 3

The full-viewport invisible ImGui input surface was removed because it
overlapped and intercepted the three confirmation buttons. The controls are
now directly clickable and have keyboard and controller fallbacks:

- **Use this screen:** `1`, `Enter`, or gamepad `A`.
- **Numbered fallback:** `2` or gamepad `X`.
- **Try click again:** `3` or gamepad `Y`.
- **Cancel:** `Escape` or gamepad `B`.
- In numbered mode: Left/Right arrow or D-pad, then `Enter` or gamepad `A`.

When the picker saves, cancels, or closes, it now disables the temporary
mouse-unlock hook and returns mouse input to ETS2 automatically.

The `capabilities` result includes `pickerInput=4` in this revision.

