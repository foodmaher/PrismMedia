#define NOMINMAX
#include "accessory_bridge.h"

#include "diagnostic_log.h"
#include "override_assets.h"
#include "screens.h"
#include "settings.h"

#include <Windows.h>
#include <ShlObj.h>
#include <Shellapi.h>
#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace
{
    struct adapter_t
    {
        std::string id;
        std::string name;
        std::string uiTexture;
        std::string mediaTobj;
        uint32_t width{};
        uint32_t height{};
    };

    HANDLE g_process{};
    std::wstring g_resultPath;
    std::string g_status =
        "Automatic adapter discovery will check local and Workshop TJS mods.";
    std::string g_sourceName;
    std::string g_toastMessage;
    size_t g_adapterCount{};
    bool g_modActive{};
    uint64_t g_toastTick{};

    std::wstring quote(const std::wstring& value)
    {
        std::wstring result = L"\"";
        unsigned backslashes{};
        for (const wchar_t character : value)
        {
            if (character == L'\\')
            {
                ++backslashes;
                continue;
            }
            if (character == L'\"')
            {
                result.append(backslashes * 2 + 1, L'\\');
                result.push_back(L'\"');
                backslashes = 0;
                continue;
            }
            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(character);
        }
        result.append(backslashes * 2, L'\\');
        result.push_back(L'\"');
        return result;
    }

    std::wstring module_directory()
    {
        HMODULE module{};
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&module_directory), &module))
            return {};
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(module, path, MAX_PATH))
            return {};
        std::wstring result(path);
        const size_t slash = result.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring{} :
            result.substr(0, slash);
    }

    std::wstring game_home_directory()
    {
        int argumentCount{};
        LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
        if (arguments)
        {
            for (int index = 0; index + 1 < argumentCount; ++index)
            {
                if (_wcsicmp(arguments[index], L"-homedir") == 0)
                {
                    std::wstring result(arguments[index + 1]);
                    LocalFree(arguments);
                    return result;
                }
            }
            LocalFree(arguments);
        }

        PWSTR documents{};
        if (FAILED(SHGetKnownFolderPath(
                FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents)) ||
            !documents)
            return {};
        wchar_t processPath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, processPath, MAX_PATH);
        std::wstring lower(processPath);
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](wchar_t value)
            {
                return static_cast<wchar_t>(std::towlower(value));
            });
        std::wstring result(documents);
        CoTaskMemFree(documents);
        result += lower.find(L"amtrucks") != std::wstring::npos
            ? L"\\American Truck Simulator"
            : L"\\Euro Truck Simulator 2";
        return result;
    }

    std::string read_ini(const char* section, const char* key,
        const std::wstring& path)
    {
        std::vector<char> value(32768);
        const std::string narrowPath(path.begin(), path.end());
        GetPrivateProfileStringA(section, key, "", value.data(),
            static_cast<DWORD>(value.size()), narrowPath.c_str());
        return value.data();
    }

    bool dimensions_used(uint32_t width, uint32_t height,
        const screen_t* except)
    {
        return std::any_of(g_screens.begin(), g_screens.end(),
            [&](const screen_t& screen)
            {
                return &screen != except &&
                    screen.override_texture_size_w == width &&
                    screen.override_texture_size_h == height;
            });
    }

    bool assign_unique_dimensions(screen_t& screen,
        uint32_t suggestedWidth, uint32_t suggestedHeight)
    {
        const bool currentValid =
            screen.override_texture_size_w >= 4 &&
            screen.override_texture_size_h >= 4 &&
            screen.override_texture_size_w % 4 == 0 &&
            screen.override_texture_size_h % 4 == 0 &&
            !dimensions_used(screen.override_texture_size_w,
                screen.override_texture_size_h, &screen);
        if (currentValid)
            return false;

        uint32_t firstSlot{};
        if (suggestedWidth >= 132 && suggestedWidth <= 384 &&
            suggestedHeight >= 2052 && suggestedHeight <= 2304)
        {
            firstSlot = ((suggestedHeight - 2052) / 4) * 64 +
                ((suggestedWidth - 132) / 4);
        }
        for (uint32_t attempt = 0; attempt < 4096; ++attempt)
        {
            const uint32_t slot = (firstSlot + attempt) % 4096;
            const uint32_t width = 132 + (slot % 64) * 4;
            const uint32_t height = 2052 + (slot / 64) * 4;
            if (!dimensions_used(width, height, &screen))
            {
                screen.override_texture_size_w = width;
                screen.override_texture_size_h = height;
                return true;
            }
        }
        return false;
    }

    bool synchronize_adapters(const std::vector<adapter_t>& adapters)
    {
        bool changed{};
        std::lock_guard<std::mutex> lock(g_screens_mutex);
        for (const adapter_t& adapter : adapters)
        {
            const std::string identity = "auto:" + adapter.id;
            auto found = std::find_if(g_screens.begin(), g_screens.end(),
                [&](const screen_t& screen)
                {
                    return screen.adapterMode &&
                        screen.adapterDataPath == identity;
                });
            if (found == g_screens.end())
            {
                screen_t screen;
                screen.type = screen_type_t::CUSTOM;
                screen.enabled = false;
                screen.adapterMode = true;
                screen.adapterDataPath = identity;
                screen.adapterName = adapter.name;
                screen.original_texture = adapter.uiTexture;
                screen.override_texture = adapter.mediaTobj;
                screen.override_texture_size_w = adapter.width;
                screen.override_texture_size_h = adapter.height;
                screen.mediaClientId = "adapter-" + adapter.id;
                screen.targetLiveTextureWidth = 1280;
                screen.targetLiveTextureHeight = 720;
                assign_unique_dimensions(screen, adapter.width, adapter.height);
                g_screens.push_back(std::move(screen));
                found = g_screens.end() - 1;
                changed = true;
            }
            else
            {
                if (found->adapterName != adapter.name ||
                    found->original_texture != adapter.uiTexture ||
                    found->override_texture != adapter.mediaTobj)
                {
                    found->adapterName = adapter.name;
                    found->original_texture = adapter.uiTexture;
                    found->override_texture = adapter.mediaTobj;
                    changed = true;
                }
                changed = assign_unique_dimensions(
                    *found, adapter.width, adapter.height) || changed;
            }

            std::string assetStatus;
            if (!override_assets::write_exact(*found, assetStatus))
                diagnostic_log::writef("error", "%s", assetStatus.c_str());
        }
        return changed;
    }

    void load_result()
    {
        const std::string state = read_ini("Result", "Status", g_resultPath);
        g_status = read_ini("Result", "Message", g_resultPath);
        if (state != "ok")
        {
            if (g_status.empty())
                g_status = "Automatic adapter discovery did not return a result.";
            diagnostic_log::writef("adapter", "%s", g_status.c_str());
            return;
        }

        g_sourceName = read_ini("Result", "Source", g_resultPath);
        g_modActive = GetPrivateProfileIntW(
            L"Result", L"Active", 0, g_resultPath.c_str()) != 0;
        const bool generated = GetPrivateProfileIntW(
            L"Result", L"Generated", 0, g_resultPath.c_str()) != 0;
        const int count = (std::min)(GetPrivateProfileIntW(
            L"Result", L"Count", 0, g_resultPath.c_str()), 64U);
        std::vector<adapter_t> adapters;
        adapters.reserve(count);
        for (int index = 0; index < count; ++index)
        {
            const std::string section = "Adapter" + std::to_string(index);
            adapter_t adapter;
            adapter.id = read_ini(section.c_str(), "Id", g_resultPath);
            adapter.name = read_ini(section.c_str(), "Name", g_resultPath);
            adapter.uiTexture = read_ini(section.c_str(), "UiTexture", g_resultPath);
            adapter.mediaTobj = read_ini(section.c_str(), "MediaTobj", g_resultPath);
            adapter.width = GetPrivateProfileIntA(section.c_str(), "Width", 0,
                std::string(g_resultPath.begin(), g_resultPath.end()).c_str());
            adapter.height = GetPrivateProfileIntA(section.c_str(), "Height", 0,
                std::string(g_resultPath.begin(), g_resultPath.end()).c_str());
            if (!adapter.id.empty() && !adapter.mediaTobj.empty())
                adapters.push_back(std::move(adapter));
        }
        g_adapterCount = adapters.size();
        const bool changed = synchronize_adapters(adapters);
        if (changed)
            settings::save();

        if (generated || !g_modActive)
        {
            g_toastMessage = g_status;
            g_toastTick = GetTickCount64();
        }
        diagnostic_log::writef(
            "adapter", "Automatic bridge complete: source='%s', adapters=%zu, "
            "generated=%d, active=%d.",
            g_sourceName.c_str(), g_adapterCount,
            generated ? 1 : 0, g_modActive ? 1 : 0);
    }
}

namespace accessory_bridge
{
    void start_auto()
    {
        if (g_process)
            return;
        const std::wstring root = module_directory();
        const std::wstring executable = root + L"\\PrismMedia\\PrismMediaClient.exe";
        if (GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            g_status = "PrismMediaClient.exe is missing from the PrismMedia folder.";
            return;
        }

        wchar_t temporary[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temporary);
        g_resultPath = std::wstring(temporary) + L"PrismMediaAccessoryBridge-" +
            std::to_wstring(GetCurrentProcessId()) + L".ini";
        DeleteFileW(g_resultPath.c_str());
        wchar_t gameExe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, gameExe, MAX_PATH);
        const std::wstring gameHome = game_home_directory();
        std::wstring command = quote(executable) +
            L" --bridge-mode auto --result " + quote(g_resultPath) +
            L" --game-home " + quote(gameHome) +
            L" --game-exe " + quote(gameExe);
        std::vector<wchar_t> writable(command.begin(), command.end());
        writable.push_back(L'\0');

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), writable.data(), nullptr, nullptr,
                FALSE, CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &process))
        {
            g_status = "Could not start automatic accessory discovery.";
            return;
        }
        CloseHandle(process.hThread);
        g_process = process.hProcess;
        g_status = "Checking local mods and every Steam Workshop library...";
    }

    void update()
    {
        if (!g_process || WaitForSingleObject(g_process, 0) != WAIT_OBJECT_0)
            return;
        CloseHandle(g_process);
        g_process = nullptr;
        load_result();
        DeleteFileW(g_resultPath.c_str());
    }

    void shutdown()
    {
        if (g_process)
        {
            CloseHandle(g_process);
            g_process = nullptr;
        }
    }

    bool busy() { return g_process != nullptr; }
    bool mod_active() { return g_modActive; }
    size_t adapter_count() { return g_adapterCount; }
    const std::string& source_name() { return g_sourceName; }
    const std::string& status() { return g_status; }
    const std::string& toast_message() { return g_toastMessage; }

    bool should_show_toast()
    {
        if (g_toastMessage.empty() || g_toastTick == 0)
            return false;
        const uint64_t now = GetTickCount64();
        return now >= g_toastTick && now - g_toastTick <= 20000;
    }
}
