#pragma once
#include <d3d11.h>

#include "present.h"
#include "CreateTexture2D.h"
#include "internal_render_probe.h"

#pragma comment(lib, "d3d11.lib")

namespace dx11 {
	inline bool init()
	{
		// TODO Create fake devices here and pass from here, instead of each creating their own

		if (!present::init())
			return false;

		// The internal render-to-texture probe is optional and guarded to one
		// exact ETS2 executable. A mismatch never prevents the plugin loading.
		internal_render_probe::init();

		if (!create_texture_2d::init())
			return false;

		return true;
	}

	inline void shutdown()
	{
		internal_render_probe::shutdown();
		present::shutdown();
		// create_texture_2d::shutdown();
	}
}
