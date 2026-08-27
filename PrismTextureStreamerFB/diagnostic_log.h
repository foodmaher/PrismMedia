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

	// Bounded diagnostic checkpoints may use the blocking variant after their
	// high-frequency hooks are disabled. This prevents a complete one-shot
	// probe from losing its critical code/correlation records to try_lock
	// contention with ordinary runtime telemetry.
	void write_important(const char* category, const char* message);
	void writef_important(const char* category, const char* format, ...);
}
