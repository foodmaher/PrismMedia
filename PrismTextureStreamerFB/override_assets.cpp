#define NOMINMAX
#include "override_assets.h"

#include "diagnostic_log.h"
#include "screens.h"

#include <Windows.h>
#include <ShlObj.h>
#include <Shellapi.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cwctype>
#include <cstring>
#include <string>

namespace
{
    std::wstring game_home_directory()
    {
        int argumentCount{};
        LPWSTR* arguments = CommandLineToArgvW(
            GetCommandLineW(), &argumentCount);
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
        std::wstring lowerProcessPath(processPath);
        std::transform(
            lowerProcessPath.begin(), lowerProcessPath.end(),
            lowerProcessPath.begin(),
            [](wchar_t character)
            {
                return static_cast<wchar_t>(std::towlower(character));
            });
        const wchar_t* gameFolder =
            lowerProcessPath.find(L"amtrucks") != std::wstring::npos
            ? L"American Truck Simulator"
            : L"Euro Truck Simulator 2";
        std::wstring result(documents);
        CoTaskMemFree(documents);
        result += L"\\";
        result += gameFolder;
        return result;
    }

    std::string safe_identity(const std::string& value)
    {
        std::string result;
        for (const unsigned char character : value)
        {
            if (std::isalnum(character) || character == '-' ||
                character == '_')
                result.push_back(static_cast<char>(character));
        }
        if (result.size() > 40)
            result.resize(40);
        return result.empty() ? "display" : result;
    }

    std::string file_stem(const std::string& virtualPath)
    {
        const size_t slash = virtualPath.find_last_of("/\\");
        const size_t start = slash == std::string::npos ? 0 : slash + 1;
        const size_t extension = virtualPath.find_last_of('.');
        if (extension == std::string::npos || extension <= start)
            return {};
        return virtualPath.substr(start, extension - start);
    }

    bool dimensions_used(
        uint32_t width,
        uint32_t height,
        const std::vector<std::pair<uint32_t, uint32_t>>& used)
    {
        return std::find(used.begin(), used.end(),
            std::make_pair(width, height)) != used.end();
    }

    bool generated_identity_dimensions(uint32_t width, uint32_t height)
    {
        return width >= 132U && width <= 384U &&
            height >= 2052U && height <= 2304U &&
            width % 4U == 0 && height % 4U == 0;
    }

    uint32_t fnv1a(const std::string& value)
    {
        uint32_t hash = 2166136261U;
        for (const unsigned char character : value)
        {
            hash ^= character;
            hash *= 16777619U;
        }
        return hash;
    }

    void write_u32(std::array<uint8_t, 128>& bytes,
        size_t offset, uint32_t value)
    {
        bytes[offset + 0] = static_cast<uint8_t>(value);
        bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
        bytes[offset + 2] = static_cast<uint8_t>(value >> 16);
        bytes[offset + 3] = static_cast<uint8_t>(value >> 24);
    }

    bool write_tobj(
        const std::wstring& path,
        const std::string& ddsVirtualPath)
    {
        constexpr std::array<uint8_t, 40> prefix = {
            0x01, 0x0a, 0xb1, 0x70, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
            0x02, 0x00, 0x03, 0x03, 0x02, 0x00, 0x02, 0x02,
            0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00
        };
        FILE* file{};
        if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file)
            return false;
        const uint64_t pathLength = ddsVirtualPath.size();
        const bool written =
            std::fwrite(prefix.data(), 1, prefix.size(), file) ==
                prefix.size() &&
            std::fwrite(&pathLength, 1, sizeof(pathLength), file) ==
                sizeof(pathLength) &&
            std::fwrite(ddsVirtualPath.data(), 1, ddsVirtualPath.size(), file) ==
                ddsVirtualPath.size();
        std::fclose(file);
        return written;
    }

    bool write_dds(
        const std::wstring& path,
        uint32_t width,
        uint32_t height)
    {
        if (width < 4 || height < 4 || width % 4 != 0 || height % 4 != 0)
            return false;
        std::array<uint8_t, 128> header{};
        header[0] = 'D'; header[1] = 'D'; header[2] = 'S'; header[3] = ' ';
        write_u32(header, 4, 124);
        write_u32(header, 8, 0x000A1007);
        write_u32(header, 12, height);
        write_u32(header, 16, width);
        write_u32(header, 20, width * height);
        write_u32(header, 28, 1);
        write_u32(header, 76, 32);
        write_u32(header, 80, 4);
        write_u32(header, 84, 0x35545844); // DXT5
        write_u32(header, 108, 0x1000);

        constexpr std::array<uint8_t, 16> block = {
            0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x03, 0xf8, 0x02, 0xf8, 0xaa, 0xaa, 0xaa, 0xaa
        };
        FILE* file{};
        if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file)
            return false;
        bool written = std::fwrite(
            header.data(), 1, header.size(), file) == header.size();
        const uint64_t blockCount =
            static_cast<uint64_t>(width) * height / 16U;
        for (uint64_t index = 0; written && index < blockCount; ++index)
        {
            written = std::fwrite(
                block.data(), 1, block.size(), file) == block.size();
        }
        std::fclose(file);
        return written;
    }
}

namespace override_assets
{
    bool ensure(
        screen_t& screen,
        const std::vector<std::pair<uint32_t, uint32_t>>& usedDimensions,
        const std::vector<std::string>& usedOverridePaths,
        bool requireGeneratedIdentity,
        std::string& status)
    {
        std::string stem = file_stem(screen.override_texture);
        const std::string gameTextureStem =
            safe_identity(file_stem(screen.original_texture));
        const bool pluginManagedPath =
            screen.override_texture.rfind(
                "/home/PrismTextureStreamer/", 0) == 0;
        const bool hasGeneratedDimensionSignature =
            generated_identity_dimensions(
                screen.override_texture_size_w,
                screen.override_texture_size_h);
        const bool invalidIdentity = stem.empty() ||
            screen.override_texture_size_w < 4 ||
            screen.override_texture_size_h < 4 ||
            screen.override_texture_size_w % 4 != 0 ||
            screen.override_texture_size_h % 4 != 0;
        const std::string readableGeneratedPath =
            "/home/PrismTextureStreamer/" + gameTextureStem + ".tobj";
        const bool readableNameConflict =
            std::find(
                usedOverridePaths.begin(), usedOverridePaths.end(),
                readableGeneratedPath) != usedOverridePaths.end();
        requireGeneratedIdentity =
            requireGeneratedIdentity || readableNameConflict;
        if (pluginManagedPath && !gameTextureStem.empty())
            stem = gameTextureStem;
        if (requireGeneratedIdentity)
        {
            // Two different virtual paths can have the same basename. Keep the
            // readable game texture name and add only a short stable suffix in
            // that uncommon case.
            std::string suffix = safe_identity(screen.mediaClientId);
            if (suffix.size() > 8)
                suffix = suffix.substr(suffix.size() - 8);
            stem = (gameTextureStem.empty() ? "display" : gameTextureStem) +
                "_" + suffix;
            if (readableNameConflict)
            {
                diagnostic_log::writef(
                    "render",
                    "Generated override name %s is already reserved; %s "
                    "will use a stable suffixed identity.",
                    readableGeneratedPath.c_str(),
                    screen.original_texture.c_str());
            }
        }
        if (requireGeneratedIdentity || invalidIdentity || pluginManagedPath)
        {
            if (stem.empty())
                stem = "display_" + safe_identity(screen.mediaClientId);
            if (requireGeneratedIdentity || invalidIdentity ||
                !hasGeneratedDimensionSignature)
            {
                // Use a stable two-dimensional non-power-of-two signature.
                // Older builds varied only the width while keeping 2048 as
                // the height, which could collide with a native GPS/accessory
                // texture during a full truck-resource reload.
                constexpr uint32_t identitySlots = 64U * 64U;
                const uint32_t firstSlot =
                    fnv1a(screen.mediaClientId) % identitySlots;
                for (uint32_t attempt = 0; attempt < identitySlots; ++attempt)
                {
                    const uint32_t slot =
                        (firstSlot + attempt) % identitySlots;
                    const uint32_t width =
                        132U + (slot % 64U) * 4U;
                    const uint32_t height =
                        2052U + (slot / 64U) * 4U;
                    if (!dimensions_used(width, height, usedDimensions))
                    {
                        screen.override_texture_size_w = width;
                        screen.override_texture_size_h = height;
                        diagnostic_log::writef(
                            "route",
                            "Assigned stable GPU identity %ux%u to display "
                            "%s (%s).",
                            width, height,
                            screen.mediaClientId.c_str(),
                            screen.original_texture.c_str());
                        break;
                    }
                }
            }
            screen.override_texture =
                "/home/PrismTextureStreamer/" + stem + ".tobj";
        }

        const std::wstring gameHome = game_home_directory();
        if (gameHome.empty())
        {
            status = "Could not locate the ETS2/ATS home directory.";
            return false;
        }
        const std::wstring assetDirectory =
            gameHome + L"\\PrismTextureStreamer";
        if (!CreateDirectoryW(assetDirectory.c_str(), nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS)
        {
            status = "Could not create the PrismTextureStreamer asset folder.";
            return false;
        }

        std::wstring wideStem(stem.begin(), stem.end());
        const std::wstring tobjPath =
            assetDirectory + L"\\" + wideStem + L".tobj";
        const std::wstring ddsPath =
            assetDirectory + L"\\" + wideStem + L".dds";
        const std::string ddsVirtualPath =
            "/home/PrismTextureStreamer/" + stem + ".dds";
        if (!write_tobj(tobjPath, ddsVirtualPath) ||
            !write_dds(
                ddsPath,
                screen.override_texture_size_w,
                screen.override_texture_size_h))
        {
            status = "Could not write the generated TOBJ/DDS override pair.";
            diagnostic_log::writef(
                "error", "Failed to generate override assets for %s.",
                screen.original_texture.c_str());
            return false;
        }

        status = "Override ready: " + screen.override_texture + " + " +
            ddsVirtualPath;
        diagnostic_log::writef(
            "render", "Generated override pair %s (%ux%u).",
            screen.override_texture.c_str(),
            screen.override_texture_size_w,
            screen.override_texture_size_h);
        return true;
    }
}
