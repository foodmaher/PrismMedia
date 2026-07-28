#pragma once

#include "content_source.h"
#include <memory>

namespace sources {
	std::unique_ptr<IContentSource> CreateWgcWindowSource(
		const char* application_name,
		const char* window_title = nullptr,
		uint8_t framerate = 30,
		uint32_t output_width = 1280,
		uint32_t output_height = 720,
		bool crop_enabled = false,
		float crop_left = 0.0f,
		float crop_top = 0.0f,
		float crop_width = 1.0f,
		float crop_height = 1.0f);
}
