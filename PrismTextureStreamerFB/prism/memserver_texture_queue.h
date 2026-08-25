#pragma once

#include <string>
#include <vector>

namespace prism::memserver_texture_queue {
	struct discovered_texture_t
	{
		std::string path;
		bool likely_display{};
	};

	bool init();
	void begin_display_discovery();
	bool display_discovery_active();
	std::vector<discovered_texture_t> discovered_textures();
}
