#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace prism::memserver_texture_queue {
	struct discovered_texture_t
	{
		std::string path;
		bool likely_display{};
		bool unsafe_candidate{};
		bool seen_during_current_scan{};
		uint64_t first_seen_tick{};
		uint64_t last_seen_tick{};
		uint32_t seen_count{};
	};

	bool init();
	void begin_display_discovery();
	bool display_discovery_active();
	std::vector<discovered_texture_t> discovered_textures();
}
