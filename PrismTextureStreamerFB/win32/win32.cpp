#include "win32.h"

#include <Windows.h>
#include <atomic>
#include <ImGui/imgui.h>
#include <ImGui/imgui_impl_win32.h>

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static HWND newHwnd{};
static WNDPROC originalWindowProc{};
static std::atomic<bool> menuMouseCapture{};

LRESULT CALLBACK hookedWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    const bool imguiHandled =
        ImGui_ImplWin32_WndProcHandler(hwnd, uMsg, wParam, lParam) != 0;

    if (menuMouseCapture.load(std::memory_order_relaxed) &&
        uMsg == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT)
    {
        // ImGui draws the menu cursor itself. Keep the native/game Win32
        // cursor hidden in the client area so there is never a second cursor
        // whose DPI/render-scale coordinates can disagree with ImGui.
        SetCursor(nullptr);
        return TRUE;
    }

    if (imguiHandled)
        return TRUE; // handled by ImGui

    return CallWindowProc(originalWindowProc, hwnd, uMsg, wParam, lParam);
}


namespace win32 {
    void init(void* hwnd)
    {
        newHwnd = (HWND)hwnd;
        originalWindowProc = (WNDPROC)SetWindowLongPtr(
            newHwnd, GWLP_WNDPROC, (LONG_PTR)hookedWindowProc);
    }

    void shutdown()
    {
        menuMouseCapture.store(false, std::memory_order_relaxed);
        if (originalWindowProc)
            SetWindowLongPtr(
                newHwnd, GWLP_WNDPROC, (LONG_PTR)originalWindowProc);
    }

    void set_menu_mouse_capture(bool enabled)
    {
        menuMouseCapture.store(enabled, std::memory_order_relaxed);

        if (enabled && newHwnd)
        {
            // Hide the native cursor immediately instead of waiting for the
            // next WM_SETCURSOR. ImGui's software cursor is rendered in the
            // same coordinate system as the menu hit testing.
            SetCursor(nullptr);
        }
    }

    void sync_menu_mouse_position()
    {
        if (!menuMouseCapture.load(std::memory_order_relaxed) ||
            !newHwnd || !ImGui::GetCurrentContext())
            return;

        POINT point{};
        if (!GetCursorPos(&point) || !ScreenToClient(newHwnd, &point))
            return;

        // Feed a fresh client-space position immediately before ImGui::NewFrame.
        // This avoids stale/raw-input positions and lets Windows perform the
        // correct screen-to-client/DPI conversion for the actual game window.
        ImGui::GetIO().AddMousePosEvent(
            static_cast<float>(point.x),
            static_cast<float>(point.y));
    }
}
