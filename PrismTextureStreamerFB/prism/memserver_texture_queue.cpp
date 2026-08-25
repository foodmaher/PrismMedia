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

    bool plugin_managed_texture(const std::string& path)
    {
        std::string lower;
        lower.reserve(path.size());
        for (const unsigned char character : path)
            lower.push_back(static_cast<char>(std::tolower(character)));
        return lower.rfind("/home/prismtexturestreamer/", 0) == 0;
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
        // Generated PrismMedia files are outputs. Offering them as truck
        // display inputs can create an override-to-override chain and hijack
        // an already configured GPS during a reload.
        if (!ends_with_tobj(path) || path.size() > 1024 ||
            plugin_managed_texture(path))
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
                if (screen.original_texture != discoveredPath)
                    continue;

                const uint64_t now = GetTickCount64();
                if (!screen.enabled || !screen.source.get())
                {
                    if (screen.lastTextureRouteSkipLogTick == 0 ||
                        now - screen.lastTextureRouteSkipLogTick >= 5000)
                    {
                        diagnostic_log::writef(
                            "route",
                            "Passed through native TOBJ %s for display %s "
                            "because media replacement is %s.",
                            screen.original_texture.c_str(),
                            screen.mediaClientId.c_str(),
                            !screen.enabled ? "disabled" :
                                "waiting for a media source");
                        screen.lastTextureRouteSkipLogTick = now;
                    }
                    continue;
                }
                if (plugin_managed_texture(screen.original_texture))
                {
                    if (screen.lastTextureRouteSkipLogTick == 0 ||
                        now - screen.lastTextureRouteSkipLogTick >= 5000)
                    {
                        diagnostic_log::writef(
                            "error",
                            "Blocked override-to-override route for display "
                            "%s: %s is a generated PrismMedia output, not a "
                            "game display TOBJ.",
                            screen.mediaClientId.c_str(),
                            screen.original_texture.c_str());
                        screen.lastTextureRouteSkipLogTick = now;
                    }
                    continue;
                }

                {
                    if (screen.textureRouteArmed &&
                        now >= screen.textureRouteArmedTick &&
                        now - screen.textureRouteArmedTick >
                            kTextureRouteArmTimeoutMilliseconds)
                    {
                        diagnostic_log::writef(
                            "route",
                            "Expired unconsumed route #%llu for display %s "
                            "before a new %s request arrived.",
                            static_cast<unsigned long long>(
                                screen.textureRouteSequence),
                            screen.mediaClientId.c_str(),
                            screen.original_texture.c_str());
                        screen.textureRouteArmed = false;
                    }
                    if (!screen.textureRouteArmed)
                    {
                        screen.textureRouteArmed = true;
                        screen.textureRouteArmedTick = now;
                        ++screen.textureRouteSequence;
                        diagnostic_log::writef(
                            "route",
                            "Armed route #%llu for display %s: %s -> %s "
                            "(identity %ux%u, timeout %llums).",
                            static_cast<unsigned long long>(
                                screen.textureRouteSequence),
                            screen.mediaClientId.c_str(),
                            screen.original_texture.c_str(),
                            screen.override_texture.c_str(),
                            screen.override_texture_size_w,
                            screen.override_texture_size_h,
                            static_cast<unsigned long long>(
                                kTextureRouteArmTimeoutMilliseconds));
                    }

                    tobj->m_file_path.allocate(screen.override_texture.size() + 1);
                    memcpy(tobj->m_file_path.m_string, screen.override_texture.data(), screen.override_texture.size());
                    tobj->m_file_path.m_string[screen.override_texture.size()] = '\0';

                    tobj->m_file_path.m_size =
                        static_cast<uint32_t>(screen.override_texture.size());

                    if (screen.lastTextureRedirectTick == 0 ||
                        now - screen.lastTextureRedirectTick >= 5000)
                    {
                        scs_log(0, "[prism::memserver_texture_queue] Replaced '%s' with '%s'", screen.original_texture.c_str(), screen.override_texture.c_str());
                        diagnostic_log::writef(
                            "route", "Redirected game texture %s to %s.",
                            screen.original_texture.c_str(),
                            screen.override_texture.c_str());
                        screen.lastTextureRedirectTick = now;
                    }

                    // A texture request belongs to exactly one display. Do not
                    // allow another configured entry to rewrite it a second
                    // time after its path has changed.
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
