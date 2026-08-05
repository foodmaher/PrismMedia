#define NOMINMAX
#include "update_checker.h"

#include "diagnostic_log.h"
#include "version.h"

#include <Windows.h>
#include <Shellapi.h>
#include <Winhttp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <mutex>
#include <string>
#include <thread>

#pragma comment(lib, "winhttp.lib")

namespace
{
    constexpr wchar_t kApiHost[] = L"api.github.com";
    constexpr wchar_t kLatestReleasePath[] =
        L"/repos/foodmaher/PrismTextureStreamerFB-Performance/"
        L"releases/latest";

    std::thread checkThread;
    std::atomic<bool> available{};
    std::atomic<bool> dismissed{};
    std::atomic<uint64_t> detectedTick{};
    std::mutex resultMutex;
    std::string latestTag;

    struct semantic_version_t
    {
        std::array<int, 3> value{};
        bool valid{};
    };

    semantic_version_t parse_version(const std::string& text)
    {
        semantic_version_t result;
        size_t cursor = 0;
        while (cursor < text.size() &&
            !std::isdigit(static_cast<unsigned char>(text[cursor])))
            ++cursor;
        if (cursor == text.size())
            return result;

        for (size_t part = 0; part < result.value.size(); ++part)
        {
            if (cursor >= text.size() ||
                !std::isdigit(static_cast<unsigned char>(text[cursor])))
            {
                if (part == 0)
                    return result;
                break;
            }

            int number = 0;
            while (cursor < text.size() &&
                std::isdigit(static_cast<unsigned char>(text[cursor])))
            {
                number = (std::min)(999999,
                    number * 10 + (text[cursor] - '0'));
                ++cursor;
            }
            result.value[part] = number;
            if (cursor >= text.size() || text[cursor] != '.')
                break;
            ++cursor;
        }
        result.valid = true;
        return result;
    }

    bool is_newer_version(
        const std::string& candidate,
        const std::string& current)
    {
        const auto candidateVersion = parse_version(candidate);
        const auto currentVersion = parse_version(current);
        return candidateVersion.valid && currentVersion.valid &&
            candidateVersion.value > currentVersion.value;
    }

    bool extract_json_string(
        const std::string& json,
        const char* key,
        std::string& value)
    {
        const std::string marker = "\"" + std::string(key) + "\"";
        size_t cursor = json.find(marker);
        if (cursor == std::string::npos)
            return false;
        cursor = json.find(':', cursor + marker.size());
        if (cursor == std::string::npos)
            return false;
        cursor = json.find('"', cursor + 1);
        if (cursor == std::string::npos)
            return false;
        ++cursor;

        value.clear();
        bool escaped = false;
        for (; cursor < json.size(); ++cursor)
        {
            const char character = json[cursor];
            if (escaped)
            {
                switch (character)
                {
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                default: value.push_back(character); break;
                }
                escaped = false;
            }
            else if (character == '\\')
                escaped = true;
            else if (character == '"')
                return true;
            else
                value.push_back(character);
        }
        return false;
    }

    bool download_latest_release(std::string& response)
    {
        HINTERNET session = WinHttpOpen(
            L"PrismTextureStreamerFB update checker/1.0",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session)
            return false;
        WinHttpSetTimeouts(session, 2500, 2500, 2500, 3500);

        HINTERNET connection = WinHttpConnect(
            session, kApiHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!connection)
        {
            WinHttpCloseHandle(session);
            return false;
        }

        HINTERNET request = WinHttpOpenRequest(
            connection, L"GET", kLatestReleasePath,
            nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE);
        if (!request)
        {
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return false;
        }

        const wchar_t headers[] =
            L"Accept: application/vnd.github+json\r\n"
            L"X-GitHub-Api-Version: 2022-11-28\r\n";
        bool success = WinHttpSendRequest(
            request, headers, static_cast<DWORD>(-1L),
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != FALSE &&
            WinHttpReceiveResponse(request, nullptr) != FALSE;

        DWORD statusCode{};
        DWORD statusSize = sizeof(statusCode);
        if (success)
        {
            success = WinHttpQueryHeaders(
                request,
                WINHTTP_QUERY_STATUS_CODE |
                    WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &statusCode, &statusSize,
                WINHTTP_NO_HEADER_INDEX) != FALSE &&
                statusCode == 200;
        }

        response.clear();
        while (success && response.size() < 256 * 1024)
        {
            DWORD availableBytes{};
            if (!WinHttpQueryDataAvailable(request, &availableBytes))
            {
                success = false;
                break;
            }
            if (availableBytes == 0)
                break;
            const size_t oldSize = response.size();
            const size_t wanted = (std::min)(
                static_cast<size_t>(availableBytes),
                256 * 1024 - oldSize);
            response.resize(oldSize + wanted);
            DWORD read{};
            if (!WinHttpReadData(
                request, response.data() + oldSize,
                static_cast<DWORD>(wanted), &read))
            {
                success = false;
                break;
            }
            response.resize(oldSize + read);
            if (read == 0)
                break;
        }

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return success;
    }

    void check_for_update()
    {
        std::string response;
        if (!download_latest_release(response))
        {
            diagnostic_log::write(
                "update", "GitHub release check unavailable; continuing "
                "without an update notification.");
            return;
        }

        std::string tag;
        if (!extract_json_string(response, "tag_name", tag) || tag.empty())
        {
            diagnostic_log::write(
                "update", "GitHub release response did not contain a tag.");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(resultMutex);
            latestTag = tag;
        }
        if (is_newer_version(tag, g_version))
        {
            detectedTick = GetTickCount64();
            available = true;
            diagnostic_log::writef(
                "update", "New release available: %s (installed %s).",
                tag.c_str(), g_version);
        }
        else
        {
            diagnostic_log::writef(
                "update", "Release check complete: latest=%s, installed=%s.",
                tag.c_str(), g_version);
        }
    }
}

namespace update_checker
{
    void start()
    {
        if (checkThread.joinable())
            return;
        available = false;
        dismissed = false;
        detectedTick = 0;
        {
            std::lock_guard<std::mutex> lock(resultMutex);
            latestTag.clear();
        }
        try
        {
            checkThread = std::thread(check_for_update);
        }
        catch (...)
        {
            diagnostic_log::write(
                "update", "Could not start the background release check.");
        }
    }

    void shutdown()
    {
        if (checkThread.joinable())
            checkThread.join();
    }

    bool update_available()
    {
        return available.load();
    }

    bool should_show_toast()
    {
        if (!available.load() || dismissed.load())
            return false;
        const uint64_t detected = detectedTick.load();
        const uint64_t now = GetTickCount64();
        return detected != 0 && now >= detected && now - detected <= 15000;
    }

    bool is_dismissed()
    {
        return dismissed.load();
    }

    std::string latest_tag()
    {
        std::lock_guard<std::mutex> lock(resultMutex);
        return latestTag;
    }

    void dismiss()
    {
        dismissed = true;
    }

    void open_releases_page()
    {
        ShellExecuteA(
            nullptr, "open", kReleasesUrl,
            nullptr, nullptr, SW_SHOWNORMAL);
    }
}
