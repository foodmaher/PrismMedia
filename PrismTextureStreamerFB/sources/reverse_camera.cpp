#define NOMINMAX
#include "reverse_camera.h"

#include "wgc_window.h"
#include "../scs_logging.h"

#include <Windows.h>
#include <string>

using namespace scs_logging;

namespace {
    std::string current_executable_name()
    {
        char path[MAX_PATH]{};
        if (!GetModuleFileNameA(nullptr, path, MAX_PATH))
            return {};

        std::string result(path);
        const auto separator = result.find_last_of("\\/");
        if (separator != std::string::npos)
            result.erase(0, separator + 1);
        return result;
    }
}

namespace sources {
    std::unique_ptr<IContentSource> CreateReverseCameraSource(
        uint8_t framerate,
        uint32_t output_width,
        uint32_t output_height,
        float crop_left,
        float crop_top,
        float crop_width,
        float crop_height)
    {
        const std::string executable = current_executable_name();
        if (executable.empty())
        {
            scs_log(2, "[ReverseCamera] Could not identify the game executable");
            return nullptr;
        }

        auto source = CreateWgcWindowSource(
            executable.c_str(), nullptr, framerate,
            output_width, output_height, true,
            crop_left, crop_top, crop_width, crop_height);
        if (!source)
            scs_log(2, "[ReverseCamera] Could not capture the game window");
        return source;
    }
}
