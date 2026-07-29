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

// Optional SPF camera bridge state.
inline std::atomic<bool> g_camera_bridge_connected{};
inline std::atomic<float> g_camera_world_x{};
inline std::atomic<float> g_camera_world_y{};
inline std::atomic<float> g_camera_world_z{};
inline std::atomic<int> g_camera_type{ -1 };
inline std::atomic<uint64_t> g_last_camera_bridge_tick{};
inline std::atomic<bool> g_camera_bridge_mapping_present{};
inline std::atomic<bool> g_camera_bridge_activated{};
inline std::atomic<bool> g_camera_bridge_telemetry_registered{};
inline std::atomic<bool> g_camera_bridge_trailer_valid{};
inline std::atomic<uint32_t> g_camera_bridge_trailer_count{};
inline std::atomic<double> g_last_trailer_world_x{};
inline std::atomic<double> g_last_trailer_world_y{};
inline std::atomic<double> g_last_trailer_world_z{};
inline std::atomic<double> g_last_trailer_heading{};
inline std::atomic<double> g_last_trailer_pitch{};
inline std::atomic<double> g_last_trailer_roll{};

// SCS truck placement and the per-truck driver-head anchor calibrated from
// the live interior camera. The anchor follows the truck while the player
// uses an exterior camera.
inline std::atomic<double> g_truck_world_x{};
inline std::atomic<double> g_truck_world_y{};
inline std::atomic<double> g_truck_world_z{};
inline std::atomic<float> g_truck_heading{};
inline std::atomic<uint64_t> g_last_truck_placement_tick{};
inline std::atomic<bool> g_head_anchor_calibrated{};
inline std::atomic<bool> g_head_anchor_uses_truck_local{};
inline std::atomic<float> g_head_anchor_local_x{};
inline std::atomic<float> g_head_anchor_local_y{};
inline std::atomic<float> g_head_anchor_local_z{};
inline std::atomic<float> g_head_reference_camera_x{};
inline std::atomic<float> g_head_reference_camera_y{};
inline std::atomic<float> g_head_reference_camera_z{};
inline std::atomic<double> g_head_reference_truck_x{};
inline std::atomic<double> g_head_reference_truck_y{};
inline std::atomic<double> g_head_reference_truck_z{};
inline std::atomic<float> g_external_camera_distance{};
inline std::atomic<float> g_adaptive_audio_distance_gain{ 1.0f };
inline std::atomic<float> g_adaptive_audio_lowpass_hz{ 20000.0f };
