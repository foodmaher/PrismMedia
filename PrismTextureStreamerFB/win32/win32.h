#pragma once

namespace win32 {
	void init(void* hwnd);
	void shutdown();
	void set_menu_mouse_capture(bool enabled);
	void sync_menu_mouse_position();
}
