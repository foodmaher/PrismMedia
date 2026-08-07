#include "dinput8.h"

#include <MinHook/MinHook.h>

#include <atomic>

#include "../scs_logging.h"
using namespace scs_logging;

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#pragma comment(lib, "Dinput8.lib")
#pragma comment(lib, "Dxguid.lib")

static std::atomic_bool mouse_visible{};
static void* getDeviceDataFunc{};

typedef BOOL(WINAPI* SetCursorPosT)(int, int);
SetCursorPosT originalSetCursorPos{};

BOOL WINAPI hookSetCursorPos(int x, int y) {
    if (mouse_visible.load(std::memory_order_relaxed))
        return true; // Stop windows from resetting the mouse position

    return originalSetCursorPos(x, y);
}


typedef HRESULT(__stdcall* GetDeviceDataT)(IDirectInputDevice8*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
GetDeviceDataT originalGetDeviceData{};

HRESULT hookedGetDeviceData(IDirectInputDevice8* pThis, DWORD cbObjectData, LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD dwFlags) {
    HRESULT result = originalGetDeviceData(pThis, cbObjectData, rgdod, pdwInOut, dwFlags);

    if (SUCCEEDED(result) &&
        mouse_visible.load(std::memory_order_relaxed) && pdwInOut) {
        // ETS2/ATS renders a separate software cursor in its menus. Dropping
        // every event froze that cursor at its old position while the Win32
        // pointer continued moving over ImGui, producing the two separated
        // cursors shown by users. Pass only X/Y movement so the game cursor
        // follows the same physical pointer. Buttons and the wheel remain
        // blocked, preventing interaction with the game UI behind ImGui.
        if (!rgdod) {
            *pdwInOut = 0;
            return result;
        }

        const DWORD received = *pdwInOut;
        DWORD retained{};
        for (DWORD index = 0; index < received; ++index) {
            if (rgdod[index].dwOfs != DIMOFS_X &&
                rgdod[index].dwOfs != DIMOFS_Y)
                continue;
            if (retained != index)
                rgdod[retained] = rgdod[index];
            ++retained;
        }
        *pdwInOut = retained;
    }

    return result;
}

namespace dinput8 {
    bool init() {
        IDirectInput8* pDirectInput = nullptr;
        if (FAILED(DirectInput8Create(GetModuleHandle(NULL), DIRECTINPUT_VERSION, IID_IDirectInput8, (LPVOID*)&pDirectInput, NULL))) {
            scs_log(2, "[dinput8] DirectInput8Create failed");
            return false;
        }

        LPDIRECTINPUTDEVICE8 lpdiMouse;
        pDirectInput->CreateDevice(GUID_SysMouse, &lpdiMouse, NULL);

        void** vtable = *reinterpret_cast<void***>(lpdiMouse);
        getDeviceDataFunc = vtable[10];

        MH_CreateHook(getDeviceDataFunc, hookedGetDeviceData, (LPVOID*)&originalGetDeviceData);
        MH_EnableHook(getDeviceDataFunc);

        lpdiMouse->Release();
        pDirectInput->Release();


        // Unrelated to dinput8, however hook the set cursor pos as its constantly reset preventing us moving our mouse
        LPVOID setCursorPosFunc = GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetCursorPos");
        MH_CreateHook(setCursorPosFunc, &hookSetCursorPos, (LPVOID*)&originalSetCursorPos);
        MH_EnableHook(setCursorPosFunc);
        return true;
    }

    void shutdown() {
        MH_RemoveHook(getDeviceDataFunc);

        LPVOID setCursorPosFunc = GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetCursorPos");
        MH_RemoveHook(setCursorPosFunc);
    }


    void hide_mouse() {
        mouse_visible.store(false, std::memory_order_relaxed);
    }
    void show_mouse() {
        mouse_visible.store(true, std::memory_order_relaxed);
    }
    void set_mouse(bool visible) {
        mouse_visible.store(visible, std::memory_order_relaxed);
    }
}
