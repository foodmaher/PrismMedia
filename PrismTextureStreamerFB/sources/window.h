#pragma once

#include "content_source.h"
#include <memory>

namespace sources {
	std::unique_ptr<IContentSource> CreateWindowSource(
		const char* application_name,
		const char* application_title = nullptr,
		uint8_t framerate = 30,
		uint32_t output_width = 1280,
		uint32_t output_height = 720);
}
