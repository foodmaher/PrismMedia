#pragma once

#include <atomic>
#include <cstdint>

inline std::atomic<float> g_head_offset_x{};
inline std::atomic<float> g_head_offset_y{};
inline std::atomic<float> g_head_offset_z{};
inline std::atomic<float> g_head_heading{};
inline std::atomic<uint64_t> g_last_head_update_tick{};
inline std::atomic<bool> g_telemetry_driving{};
inline std::atomic<bool> g_camera_interior_hint{ true };
inline std::atomic<bool> g_engine_enabled{};
inline std::atomic<int> g_selected_gear{};
inline std::atomic<bool> g_reverse_light{};
inline std::atomic<bool> g_reverse_active{};
