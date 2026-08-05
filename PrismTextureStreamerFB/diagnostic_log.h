#pragma once

#include <cstdarg>

namespace diagnostic_log
{
	// Starts a best-effort asynchronous log beside the game executable.
	// Failure to create the log never prevents the plugin from loading.
	void start();
	void stop();
	void write(const char* category, const char* message);
	void writef(const char* category, const char* format, ...);
}
