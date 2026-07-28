#pragma once

#include <atomic>

inline std::atomic<float> g_head_offset_x{};
inline std::atomic<float> g_head_offset_y{};
inline std::atomic<float> g_head_offset_z{};
inline std::atomic<float> g_head_heading{};
inline std::atomic<int> g_selected_gear{};
inline std::atomic<bool> g_reverse_light{};
inline std::atomic<bool> g_reverse_active{};
