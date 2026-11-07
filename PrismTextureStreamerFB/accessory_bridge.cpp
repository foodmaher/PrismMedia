#define NOMINMAX
#include "accessory_bridge.h"

#include "diagnostic_log.h"
#include "screens.h"

#include <Windows.h>
#include <ShlObj.h>
#include <Shellapi.h>
#include <algorithm>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

namespace
{
    HANDLE g_process{};
    enum class operation_t { none, scan, generate } g_operation{};
    std::wstring g_resultPath;
    std::vector<accessory_bridge::accessory_t> g_accessories;
    std::string g_status = "Scan the latest truck save to list installed display accessories.";

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
        return slash == std::wstring::npos ? std::wstring{} : result.substr(0, slash);
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
        GetPrivateProfileStringA(section, key, "", value.data(),
            static_cast<DWORD>(value.size()),
            std::string(path.begin(), path.end()).c_str());
        return value.data();
    }

    bool start_process(operation_t operation, std::wstring arguments)
    {
        if (g_process)
            return false;
        const std::wstring root = module_directory();
        const std::wstring executable = root + L"\\PrismMedia\\PrismMediaClient.exe";
        if (GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            g_status = "PrismMediaClient.exe is missing from the PrismMedia folder.";
            return false;
        }

        wchar_t temporary[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temporary);
        g_resultPath = std::wstring(temporary) + L"PrismMediaAccessoryBridge-" +
            std::to_wstring(GetCurrentProcessId()) + L".ini";
        DeleteFileW(g_resultPath.c_str());
        std::wstring command = quote(executable) + L" --bridge-mode " +
            (operation == operation_t::scan ? L"scan" : L"generate") +
            L" --result " + quote(g_resultPath) + arguments;
        std::vector<wchar_t> writable(command.begin(), command.end());
        writable.push_back(L'\0');

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), writable.data(), nullptr, nullptr,
                FALSE, CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &process))
        {
            g_status = "Could not start the installed-accessory helper.";
            return false;
        }
        CloseHandle(process.hThread);
        g_process = process.hProcess;
        g_operation = operation;
        g_status = operation == operation_t::scan
            ? "Reading the latest truck save and installed mods..."
            : "Generating the local display adapter mod...";
        return true;
    }

    void load_result()
    {
        const std::string state = read_ini("Result", "Status", g_resultPath);
        g_status = read_ini("Result", "Message", g_resultPath);
        if (state != "ok")
        {
            if (g_status.empty())
                g_status = "The accessory helper did not return a result.";
            return;
        }
        if (g_operation != operation_t::scan)
            return;

        g_accessories.clear();
        const int count = GetPrivateProfileIntW(
            L"Result", L"Count", 0, g_resultPath.c_str());
        for (int index = 0; index < count; ++index)
        {
            const std::string section = "Accessory" + std::to_string(index);
            accessory_bridge::accessory_t item;
            item.name = read_ini(section.c_str(), "Name", g_resultPath);
            item.dataPath = read_ini(section.c_str(), "DataPath", g_resultPath);
            item.source = read_ini(section.c_str(), "Source", g_resultPath);
            item.definition = read_ini(section.c_str(), "Definition", g_resultPath);
            item.interiorModel = read_ini(section.c_str(), "InteriorModel", g_resultPath);
            item.exteriorModel = read_ini(section.c_str(), "ExteriorModel", g_resultPath);
            item.uiTexture = read_ini(section.c_str(), "UiTexture", g_resultPath);
            if (!item.definition.empty())
                g_accessories.push_back(std::move(item));
        }
    }
}

namespace accessory_bridge
{
    void update()
    {
        if (!g_process || WaitForSingleObject(g_process, 0) != WAIT_OBJECT_0)
            return;
        CloseHandle(g_process);
        g_process = nullptr;
        load_result();
        DeleteFileW(g_resultPath.c_str());
        g_operation = operation_t::none;
    }

    void shutdown()
    {
        if (g_process)
        {
            CloseHandle(g_process);
            g_process = nullptr;
        }
        g_operation = operation_t::none;
    }

    bool start_scan()
    {
        wchar_t gameExe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, gameExe, MAX_PATH);
        const std::wstring gameHome = game_home_directory();
        return start_process(operation_t::scan,
            L" --game-home " + quote(gameHome) +
            L" --game-exe " + quote(gameExe));
    }

    bool start_generate(const screen_t& screen, size_t accessoryIndex)
    {
        if (accessoryIndex >= g_accessories.size())
        {
            g_status = "Select an installed display accessory first.";
            return false;
        }
        const accessory_t& item = g_accessories[accessoryIndex];
        const std::wstring gameHome = game_home_directory();
        auto wide = [](const std::string& value)
        {
            return std::wstring(value.begin(), value.end());
        };
        return start_process(operation_t::generate,
            L" --game-home " + quote(gameHome) +
            L" --source " + quote(wide(item.source)) +
            L" --definition " + quote(wide(item.definition)) +
            L" --display-id " + quote(wide(screen.mediaClientId)) +
            L" --media-tobj " + quote(wide(screen.override_texture)));
    }

    bool busy() { return g_process != nullptr; }
    const std::vector<accessory_t>& accessories() { return g_accessories; }
    const std::string& status() { return g_status; }
}
