#include "prism.h"

#include "../bmem.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
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
    constexpr uint64_t kDiscoveryWindowMilliseconds = 30000;
    std::mutex g_discoveryMutex;
    std::vector<prism::memserver_texture_queue::discovered_texture_t>
        g_discoveredTextures;
    std::unordered_map<std::string, size_t> g_discoveredTextureIndexes;
    std::atomic<uint64_t> g_discoveryStartedTick{};
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
            "computer", "phone", "ythq"
        };
        for (const char* token : tokens)
        {
            if (lower.find(token) != std::string::npos)
                return true;
        }
        return false;
    }

    bool unsafe_display_candidate(const std::string& path)
    {
        std::string lower;
        lower.reserve(path.size());
        for (const unsigned char character : path)
            lower.push_back(static_cast<char>(std::tolower(character)));

        // These are ordinary world/traffic paint textures. Intercepting one
        // can replace every matching vehicle or prop and is never a sensible
        // automatic display choice. Keep it visible behind "Show all" for
        // expert diagnosis, but clearly mark it unsafe.
        constexpr const char* prefixes[] = {
            "/vehicle/ai/", "/vehicle/trailer", "/prefab/", "/road/",
            "/terrain/", "/building/"
        };
        for (const char* prefix : prefixes)
        {
            if (lower.rfind(prefix, 0) == 0)
                return true;
        }
        return false;
    }

    void record_discovered_texture(const std::string& path)
    {
        const uint64_t now = GetTickCount64();
        if (!ends_with_tobj(path) || path.size() > 1024)
            return;

        std::lock_guard<std::mutex> lock(g_discoveryMutex);
        const uint64_t scanStart = g_discoveryStartedTick.load();
        const uint64_t scanExpires = g_discoveryExpiresTick.load();
        const bool currentScan = scanStart != 0 && now >= scanStart &&
            now < scanExpires;
        const auto existing = g_discoveredTextureIndexes.find(path);
        if (existing != g_discoveredTextureIndexes.end())
        {
            auto& entry = g_discoveredTextures[existing->second];
            entry.last_seen_tick = now;
            if (entry.seen_count != 0xffffffffU)
                ++entry.seen_count;
            entry.seen_during_current_scan =
                entry.seen_during_current_scan || currentScan;
            return;
        }
        if (g_discoveredTextures.size() >= kMaximumDiscoveredTextures)
            return;

        prism::memserver_texture_queue::discovered_texture_t entry{};
        entry.path = path;
        entry.likely_display = likely_display_texture(path);
        entry.unsafe_candidate = unsafe_display_candidate(path);
        entry.seen_during_current_scan = currentScan;
        entry.first_seen_tick = now;
        entry.last_seen_tick = now;
        entry.seen_count = 1;
        g_discoveredTextureIndexes.emplace(
            entry.path, g_discoveredTextures.size());
        g_discoveredTextures.push_back(std::move(entry));
    }
}

char memserver_texture_queue_processor(uint8_t* memserver)
{
    prism::list_node_t<prism::mem_tobj_t>* first_node = *(prism::list_node_t<prism::mem_tobj_t>**)(memserver + 0x170);
    void* fake_node = (void*)(memserver + 0x180);

    if (first_node != fake_node)
    {
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
            if (tobj->m_file_path.m_size > 0 &&
                tobj->m_file_path.m_size <= 1024)
            {
                discoveredPath.assign(
                    tobj->m_file_path.m_string,
                    tobj->m_file_path.m_size);
            }

            // Record the game's original path before any PrismMedia rewrite.
            // Passive history means accessories already loaded before the user
            // opens System remain selectable and scanning no longer needs to
            // force a risky game reload from inside the configurator.
            if (!discoveredPath.empty())
                record_discovered_texture(discoveredPath);

            std::lock_guard<std::mutex> lock(g_screens_mutex);
            for (auto& screen : g_screens) {
                const bool gameGpsRoute =
                    screen.contentMode == content_mode_t::GAME_GPS;
                if (!screen.source.get() && !gameGpsRoute) continue;

                if (screen.original_texture == std::string_view(tobj->m_file_path.m_string))
                {
                    static constexpr std::string_view gameGpsTexture =
                        "/vehicle/truck/share/gps.tobj";
                    const std::string_view targetPath = gameGpsRoute
                        ? gameGpsTexture
                        : std::string_view(screen.override_texture);

                    // A primary GPS already points at the native texture. It
                    // only needs to be claimed so a later configured screen
                    // cannot rewrite the same request again.
                    if (std::string_view(tobj->m_file_path.m_string) != targetPath)
                    {
                        tobj->m_file_path.allocate(targetPath.size() + 1);
                        memcpy(tobj->m_file_path.m_string,
                            targetPath.data(), targetPath.size());
                        tobj->m_file_path.m_string[targetPath.size()] = '\0';

                        tobj->m_file_path.m_size =
                            static_cast<uint32_t>(targetPath.size());
                    }

                    scs_log(0,
                        "[prism::memserver_texture_queue] Replaced '%s' with '%.*s'%s",
                        screen.original_texture.c_str(),
                        static_cast<int>(targetPath.size()), targetPath.data(),
                        gameGpsRoute ? " (game GPS route)" : "");
                    const uint64_t now = GetTickCount64();
                    if (screen.lastTextureRedirectTick == 0 ||
                        now - screen.lastTextureRedirectTick >= 5000)
                    {
                        diagnostic_log::writef(
                            "render", "Redirected game texture %s to %s.",
                            screen.original_texture.c_str(),
                            std::string(targetPath).c_str());
                        screen.lastTextureRedirectTick = now;
                    }

                    // The path may now equal another configured screen's
                    // original path (most notably a tablet routed to gps.tobj).
                    // One game request belongs to exactly one selected display;
                    // never feed the rewritten path through the loop again.
                    break;
                }
            }

            target = target->m_next;
        }
    }

    return original_memserver_texture_queue_processor(memserver);
}


namespace prism::memserver_texture_queue {
	void begin_display_discovery()
	{
		std::lock_guard<std::mutex> lock(g_discoveryMutex);
		const uint64_t now = GetTickCount64();
		for (auto& entry : g_discoveredTextures)
			entry.seen_during_current_scan = false;
		g_discoveryStartedTick = now;
		g_discoveryExpiresTick = now + kDiscoveryWindowMilliseconds;
		diagnostic_log::write(
			"render", "Started 30-second passive TOBJ display observation; "
			"no game reload was requested.");
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
				if (left.seen_during_current_scan !=
						right.seen_during_current_scan)
					return left.seen_during_current_scan >
						right.seen_during_current_scan;
				if (left.likely_display != right.likely_display)
					return left.likely_display > right.likely_display;
				if (left.unsafe_candidate != right.unsafe_candidate)
					return left.unsafe_candidate < right.unsafe_candidate;
				if (left.last_seen_tick != right.last_seen_tick)
					return left.last_seen_tick > right.last_seen_tick;
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
