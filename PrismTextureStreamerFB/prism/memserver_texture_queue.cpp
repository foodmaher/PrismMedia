#include "prism.h"

#include "../bmem.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "../scs_logging.h"
using namespace scs_logging;

#include "../diagnostic_log.h"
#include "../screens.h"

typedef char(__fastcall* memserver_texture_queue_processor_t)(uint8_t* memserver);
static memserver_texture_queue_processor_t original_memserver_texture_queue_processor{};

namespace
{
    constexpr size_t kMaximumDiscoveredTextures = 4096;
    constexpr uint64_t kDiscoveryWindowMilliseconds = 15000;
    std::mutex g_discoveryMutex;
    std::vector<prism::memserver_texture_queue::discovered_texture_t>
        g_discoveredTextures;
    std::unordered_set<std::string> g_discoveredTexturePaths;
    std::atomic<uint64_t> g_discoveryExpiresTick{};

    bool ends_with_tobj(const std::string& value)
    {
        if (value.size() < 5)
            return false;
        const size_t offset = value.size() - 5;
        return value[offset] == '.' &&
            std::tolower(static_cast<unsigned char>(value[offset + 1])) == 't' &&
            std::tolower(static_cast<unsigned char>(value[offset + 2])) == 'o' &&
            std::tolower(static_cast<unsigned char>(value[offset + 3])) == 'b' &&
            std::tolower(static_cast<unsigned char>(value[offset + 4])) == 'j';
    }

    bool likely_display_texture(const std::string& path)
    {
        std::string lower;
        lower.reserve(path.size());
        for (const unsigned char character : path)
            lower.push_back(static_cast<char>(std::tolower(character)));

        constexpr const char* tokens[] = {
            "gps", "dashboard", "display", "screen", "tablet",
            "navigation", "navigator", "infotainment", "monitor",
            "computer", "phone"
        };
        for (const char* token : tokens)
        {
            if (lower.find(token) != std::string::npos)
                return true;
        }
        return false;
    }

    void record_discovered_texture(const std::string& path)
    {
        const uint64_t now = GetTickCount64();
        const uint64_t expires = g_discoveryExpiresTick.load();
        if (expires == 0 || now >= expires || !ends_with_tobj(path) ||
            path.size() > 1024)
            return;

        std::lock_guard<std::mutex> lock(g_discoveryMutex);
        if (g_discoveredTextures.size() >= kMaximumDiscoveredTextures ||
            !g_discoveredTexturePaths.insert(path).second)
            return;
        g_discoveredTextures.push_back({ path, likely_display_texture(path) });
    }
}

char memserver_texture_queue_processor(uint8_t* memserver)
{
    prism::list_node_t<prism::mem_tobj_t>* first_node = *(prism::list_node_t<prism::mem_tobj_t>**)(memserver + 0x170);
    void* fake_node = (void*)(memserver + 0x180);

    if (first_node != fake_node)
    {
        const bool discoveryActive =
            prism::memserver_texture_queue::display_discovery_active();
        prism::list_node_t<prism::mem_tobj_t>* target = first_node;
        while (target != fake_node)
        {
            prism::mem_tobj_t* tobj = target->m_item;

            if (!tobj || !tobj->m_file_path.m_string)
            {
                target = target->m_next;
                continue;
            }

            std::string discoveredPath;
            if (discoveryActive &&
                tobj->m_file_path.m_size > 0 &&
                tobj->m_file_path.m_size <= 1024)
            {
                discoveredPath.assign(
                    tobj->m_file_path.m_string,
                    tobj->m_file_path.m_size);
            }

            std::lock_guard<std::mutex> lock(g_screens_mutex);
            for (auto& screen : g_screens) {
                if (!screen.source.get()) continue;

                if (screen.original_texture == std::string_view(tobj->m_file_path.m_string))
                {
                    tobj->m_file_path.allocate(screen.override_texture.size() + 1);
                    memcpy(tobj->m_file_path.m_string, screen.override_texture.data(), screen.override_texture.size());
                    tobj->m_file_path.m_string[screen.override_texture.size()] = '\0';

                    tobj->m_file_path.m_size =
                        static_cast<uint32_t>(screen.override_texture.size());

                    scs_log(0, "[prism::memserver_texture_queue] Replaced '%s' with '%s'", screen.original_texture.c_str(), screen.override_texture.c_str());
                    const uint64_t now = GetTickCount64();
                    if (screen.lastTextureRedirectTick == 0 ||
                        now - screen.lastTextureRedirectTick >= 5000)
                    {
                        diagnostic_log::writef(
                            "render", "Redirected game texture %s to %s.",
                            screen.original_texture.c_str(),
                            screen.override_texture.c_str());
                        screen.lastTextureRedirectTick = now;
                    }
                }
            }

            if (!discoveredPath.empty())
                record_discovered_texture(discoveredPath);

            target = target->m_next;
        }
    }

    return original_memserver_texture_queue_processor(memserver);
}


namespace prism::memserver_texture_queue {
	void begin_display_discovery()
	{
		std::lock_guard<std::mutex> lock(g_discoveryMutex);
		g_discoveredTextures.clear();
		g_discoveredTexturePaths.clear();
		g_discoveryExpiresTick =
			GetTickCount64() + kDiscoveryWindowMilliseconds;
		diagnostic_log::write(
			"render", "Started 15-second loaded TOBJ display discovery.");
	}

	bool display_discovery_active()
	{
		const uint64_t expires = g_discoveryExpiresTick.load();
		return expires != 0 && GetTickCount64() < expires;
	}

	std::vector<discovered_texture_t> discovered_textures()
	{
		std::lock_guard<std::mutex> lock(g_discoveryMutex);
		auto result = g_discoveredTextures;
		std::stable_sort(
			result.begin(), result.end(),
			[](const auto& left, const auto& right)
			{
				if (left.likely_display != right.likely_display)
					return left.likely_display > right.likely_display;
				return left.path < right.path;
			});
		return result;
	}

	bool init() {
        // 1.60
        uint64_t memserver_texture_queue_processor_address = bmem::patternScan("48 8D 68 ?? 48 81 EC ?? ?? ?? ?? 48 8B F9 4C") - 19;

        MH_CreateHook(
            (LPVOID)memserver_texture_queue_processor_address,
            &memserver_texture_queue_processor,
            reinterpret_cast<void**>(&original_memserver_texture_queue_processor)
        );

        MH_EnableHook((LPVOID)memserver_texture_queue_processor_address);

        return true;
	}
}
