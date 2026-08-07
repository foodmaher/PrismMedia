# PrismTextureStreamerFB 3.10.1 cursor alignment fix

This source includes the 3.10.1 audio fixes plus a menu cursor fix.

## Problem

The plugin menu disabled ImGui's software cursor (`MouseDrawCursor = false`) and relied on the Windows/game cursor for the visible pointer. ImGui still used its own mouse coordinates for hover/click hit testing. Under Windows DPI scaling, ETS2/ATS render scaling, fullscreen/borderless transitions, or game cursor handling, the visible cursor could therefore be invisible or appear above/left of the control ImGui was actually selecting.

## Fix

- Use ImGui's software cursor while the plugin menu is visible.
- Hide the native Win32 client-area cursor while ImGui owns the menu mouse.
- Synchronize the actual Windows cursor position with ImGui immediately before `ImGui::NewFrame()` using `GetCursorPos()` and `ScreenToClient()`.
- Continue blocking ETS2/ATS cursor recentering while the menu is open through the existing `SetCursorPos` hook.
- Release menu mouse ownership when the menu closes so the game resumes normal cursor behavior.

The visible menu cursor and ImGui hit testing now use the same coordinate space, so the cursor should sit on the control that is actually highlighted/clicked.
