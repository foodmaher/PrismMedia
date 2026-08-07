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
    const bool capture =
        menuMouseCapture.load(std::memory_order_relaxed);

    if (capture)
    {
        if (uMsg == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT)
        {
            // ImGui draws the visible overlay cursor. Hide the native client
            // cursor so a differently scaled Win32 pointer cannot appear next
            // to it or above/left of the actual ImGui hit-test position.
            SetCursor(nullptr);
            return TRUE;
        }

        // ImGui has already received these messages. Do not also forward them
        // to the SCS window, otherwise a click can operate the game UI behind
        // the plugin menu. DirectInput X/Y filtering remains handled by the
        // existing 3.12.1 dinput8 hook.
        switch (uMsg)
        {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_XBUTTONDBLCLK:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            return TRUE;
        default:
            break;
        }
    }

    if (imguiHandled)
        return TRUE;

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
            SetCursor(nullptr);
    }

    void sync_menu_mouse_position()
    {
        if (!menuMouseCapture.load(std::memory_order_relaxed) ||
            !newHwnd || !ImGui::GetCurrentContext())
            return;

        POINT point{};
        if (!GetCursorPos(&point) || !ScreenToClient(newHwnd, &point))
            return;

        // ImGui's Win32 backend can receive a position from a different input
        // path than ETS2/ATS. Force a fresh client-space position immediately
        // before NewFrame so the rendered cursor and widget hit testing always
        // consume exactly the same coordinates.
        ImGui::GetIO().AddMousePosEvent(
            static_cast<float>(point.x),
            static_cast<float>(point.y));
    }
}
