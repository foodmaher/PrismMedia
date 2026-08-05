#define NOMINMAX
#include "diagnostic_log.h"

#include "thread_scheduling.h"

#include <Windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace
{
	constexpr size_t kMaximumQueuedLines = 512;
	constexpr LONGLONG kMaximumLogBytes = 4LL * 1024LL * 1024LL;

	std::atomic<bool> g_running{};
	std::atomic<uint32_t> g_dropped_lines{};
	std::mutex g_queue_mutex;
	std::condition_variable g_queue_ready;
	std::deque<std::string> g_lines;
	std::thread g_writer;
	std::wstring g_log_path;

	std::wstring executable_log_path()
	{
		wchar_t path[32768]{};
		constexpr DWORD pathCapacity =
			static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
		const DWORD length = GetModuleFileNameW(
			nullptr, path, pathCapacity);
		if (length == 0 || length >= pathCapacity)
			return L"PrismTextureStreamerFB.log";

		std::wstring result(path, length);
		const size_t separator = result.find_last_of(L"\\/");
		if (separator == std::wstring::npos)
			return L"PrismTextureStreamerFB.log";
		result.resize(separator + 1);
		result += L"PrismTextureStreamerFB.log";
		return result;
	}

	void rotate_large_log()
	{
		WIN32_FILE_ATTRIBUTE_DATA attributes{};
		if (!GetFileAttributesExW(
			g_log_path.c_str(), GetFileExInfoStandard, &attributes))
		{
			return;
		}

		ULARGE_INTEGER size{};
		size.LowPart = attributes.nFileSizeLow;
		size.HighPart = attributes.nFileSizeHigh;
		if (size.QuadPart < static_cast<ULONGLONG>(kMaximumLogBytes))
			return;

		std::wstring previous = g_log_path + L".old";
		MoveFileExW(
			g_log_path.c_str(), previous.c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	}

	void write_line(HANDLE file, const char* category, const std::string& text)
	{
		SYSTEMTIME time{};
		GetLocalTime(&time);
		char prefix[96]{};
		const int prefixLength = std::snprintf(
			prefix, sizeof(prefix),
			"%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] ",
			time.wYear, time.wMonth, time.wDay,
			time.wHour, time.wMinute, time.wSecond,
			time.wMilliseconds, category ? category : "info");
		if (prefixLength <= 0)
			return;

		DWORD written{};
		WriteFile(file, prefix, static_cast<DWORD>(prefixLength), &written, nullptr);
		WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
		static constexpr char newline[] = "\r\n";
		WriteFile(file, newline, 2, &written, nullptr);
	}

	void writer_loop()
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		thread_scheduling::refresh_current_thread_preference();
		rotate_large_log();

		const HANDLE file = CreateFileW(
			g_log_path.c_str(), FILE_APPEND_DATA,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			g_running = false;
			return;
		}

		for (;;)
		{
			std::deque<std::string> pending;
			{
				std::unique_lock<std::mutex> lock(g_queue_mutex);
				g_queue_ready.wait_for(
					lock, std::chrono::seconds(1), [] {
						return !g_running.load() || !g_lines.empty();
					});
				pending.swap(g_lines);
			}

			thread_scheduling::refresh_current_thread_preference();
			const uint32_t dropped = g_dropped_lines.exchange(0);
			if (dropped != 0)
			{
				write_line(
					file, "logger",
					"Dropped " + std::to_string(dropped) +
					" diagnostic events because the queue was busy or full.");
			}
			for (const std::string& line : pending)
			{
				const size_t separator = line.find('\t');
				if (separator == std::string::npos)
					write_line(file, "info", line);
				else
					write_line(
						file, line.substr(0, separator).c_str(),
						line.substr(separator + 1));
			}

			if (!g_running.load() && pending.empty())
				break;
		}

		FlushFileBuffers(file);
		CloseHandle(file);
	}
}

namespace diagnostic_log
{
	void start()
	{
		bool expected = false;
		if (!g_running.compare_exchange_strong(expected, true))
			return;

		g_log_path = executable_log_path();
		g_writer = std::thread(writer_loop);
	}

	void stop()
	{
		if (!g_running.exchange(false))
		{
			if (g_writer.joinable())
				g_writer.join();
			return;
		}
		g_queue_ready.notify_all();
		if (g_writer.joinable())
			g_writer.join();
	}

	void write(const char* category, const char* message)
	{
		if (!g_running.load() || !message)
			return;

		std::unique_lock<std::mutex> lock(
			g_queue_mutex, std::try_to_lock);
		if (!lock.owns_lock() || g_lines.size() >= kMaximumQueuedLines)
		{
			g_dropped_lines.fetch_add(1);
			return;
		}

		std::string line(category ? category : "info");
		line.push_back('\t');
		line += message;
		g_lines.emplace_back(std::move(line));
		lock.unlock();
		g_queue_ready.notify_one();
	}

	void writef(const char* category, const char* format, ...)
	{
		if (!g_running.load() || !format)
			return;

		char message[1024]{};
		va_list arguments;
		va_start(arguments, format);
		std::vsnprintf(message, sizeof(message), format, arguments);
		va_end(arguments);
		write(category, message);
	}
}
