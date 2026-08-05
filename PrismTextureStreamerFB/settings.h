#pragma once

#include <array>
#include <cstddef>
#include <string>

struct configuration_backup_info_t
{
    bool available{};
    std::string description;
};

namespace settings {
    bool load();
    bool save();
    std::array<configuration_backup_info_t, 3> backup_history();
    bool restore_backup(size_t index);
}
