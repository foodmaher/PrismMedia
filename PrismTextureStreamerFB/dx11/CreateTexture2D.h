#pragma once

namespace dx11::create_texture_2d {
	bool init();

	// The high-frequency SRV/bind/draw detours are enabled only for the
	// bounded custom-render diagnostic window, so normal gameplay keeps the
	// same hook overhead as the regular 4.0.0 build.
	bool set_custom_probe_hooks_enabled(bool enabled);
}
