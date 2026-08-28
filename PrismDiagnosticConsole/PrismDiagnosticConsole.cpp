#define NOMINMAX
#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    std::vector<DWORD> game_processes()
    {
        std::vector<DWORD> result;
        const HANDLE snapshot = CreateToolhelp32Snapshot(
            TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return result;

        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry))
        {
            do
            {
                if (_wcsicmp(entry.szExeFile, L"eurotrucks2.exe") == 0 ||
                    _wcsicmp(entry.szExeFile, L"amtrucks.exe") == 0)
                {
                    result.push_back(entry.th32ProcessID);
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return result;
    }

    std::wstring pipe_name(DWORD processId)
    {
        return L"\\\\.\\pipe\\PrismMediaDiagnostic-" +
            std::to_wstring(processId);
    }

    bool send_command(
        DWORD processId,
        const std::string& command,
        std::string& response)
    {
        const std::wstring name = pipe_name(processId);
        if (!WaitNamedPipeW(name.c_str(), 3000))
            return false;

        const HANDLE pipe = CreateFileW(
            name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (pipe == INVALID_HANDLE_VALUE)
            return false;

        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
        DWORD written{};
        const bool sent = WriteFile(
            pipe, command.data(), static_cast<DWORD>(command.size()),
            &written, nullptr) != FALSE;
        if (!sent)
        {
            CloseHandle(pipe);
            return false;
        }

        response.clear();
        for (;;)
        {
            char buffer[4096]{};
            DWORD read{};
            const BOOL ok = ReadFile(
                pipe, buffer, static_cast<DWORD>(sizeof(buffer)),
                &read, nullptr);
            if (read != 0)
                response.append(buffer, read);
            if (ok)
                break;
            if (GetLastError() != ERROR_MORE_DATA)
            {
                CloseHandle(pipe);
                return false;
            }
        }
        CloseHandle(pipe);
        return true;
    }

    DWORD parse_pid(const char* value)
    {
        try
        {
            const unsigned long parsed = std::stoul(value ? value : "");
            return static_cast<DWORD>(parsed);
        }
        catch (...)
        {
            return 0;
        }
    }

    std::string joined_command(int first, int argc, char** argv)
    {
        std::ostringstream command;
        for (int index = first; index < argc; ++index)
        {
            if (index != first)
                command << ' ';
            command << argv[index];
        }
        return command.str();
    }
}

int main(int argc, char** argv)
{
    SetConsoleTitleW(L"PrismMedia 4.0.0 Diagnostic Console");

    DWORD processId{};
    int commandStart = 1;
    if (argc >= 3 && std::string(argv[1]) == "--pid")
    {
        processId = parse_pid(argv[2]);
        commandStart = 3;
    }
    if (processId == 0)
    {
        const std::vector<DWORD> processes = game_processes();
        if (processes.empty())
        {
            std::cerr << "ETS2/ATS is not running. Start the game and wait "
                "for PrismMedia to load.\n";
            return 2;
        }
        processId = processes.front();
        if (processes.size() > 1)
        {
            std::cout << "Multiple games detected; using PID "
                << processId << ". Use --pid to select another.\n";
        }
    }

    if (commandStart < argc)
    {
        std::string response;
        if (!send_command(
                processId,
                joined_command(commandStart, argc, argv), response))
        {
            std::cerr << "Could not connect to PrismMedia in PID "
                << processId << ".\n";
            return 3;
        }
        std::cout << response << '\n';
        return response.rfind("ERROR", 0) == 0 ? 1 : 0;
    }

    std::cout << "PrismMedia 4.0.0 Diagnostic Console\n"
        << "Connected target PID: " << processId << "\n"
        << "Enter help for commands, or quit to close this console.\n\n";
    for (;;)
    {
        std::cout << "prism> " << std::flush;
        std::string command;
        if (!std::getline(std::cin, command))
            break;
        if (command == "quit" || command == "exit")
            break;
        if (command.empty())
            continue;

        std::string response;
        if (!send_command(processId, command, response))
        {
            std::cerr << "Connection failed. The game may have closed or "
                "the plugin may still be loading.\n";
            continue;
        }
        std::cout << response << "\n\n";
    }
    return 0;
}
