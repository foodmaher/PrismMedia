#include "win32.h"

#include <Windows.h>
#include <ImGui/imgui_impl_win32.h>

#include "../menu/menu.h"
#include "../telemetry_state.h"

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static HWND newHwnd{};
static WNDPROC originalWindowProc{};

LRESULT CALLBACK hookedWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    const bool handledByImGui =
        ImGui_ImplWin32_WndProcHandler(hwnd, uMsg, wParam, lParam) != 0;

    if (Gui::is_visible())
    {
        if (uMsg == WM_SETCURSOR && !g_telemetry_driving.load())
        {
            // In menus SCS already renders its own cursor. Keep the Win32
            // pointer active for ImGui coordinates but make its image
            // invisible, leaving one visible cursor instead of two aligned
            // cursor shapes. While driving, SCS has no menu cursor and the
            // normal Win32 pointer remains visible for the overlay.
            SetCursor(nullptr);
            return TRUE;
        }

        // ImGui has already consumed the event. Do not also deliver pointer
        // movement, buttons or wheel messages to the SCS window: DirectInput
        // receives X/Y movement only, which keeps its software cursor aligned
        // without allowing a click on the menu behind the overlay.
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

    if (handledByImGui)
        return TRUE; // handled by ImGui

    return CallWindowProc(originalWindowProc, hwnd, uMsg, wParam, lParam);
}


namespace win32 {
	void init(void* hwnd)
	{
		newHwnd = (HWND)hwnd;
		originalWindowProc = (WNDPROC)SetWindowLongPtr(newHwnd, GWLP_WNDPROC, (LONG_PTR)hookedWindowProc);
	}

	void shutdown()
	{
		if (originalWindowProc)
			SetWindowLongPtr(newHwnd, GWLP_WNDPROC, (LONG_PTR)originalWindowProc);
	}
}
