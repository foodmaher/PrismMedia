#pragma once

#include <string>

namespace update_checker
{
    inline constexpr const char* kReleasesUrl =
        "https://github.com/foodmaher/"
        "PrismTextureStreamerFB-Performance/releases/";

    void start();
    void shutdown();
    bool update_available();
    bool should_show_toast();
    bool is_dismissed();
    std::string latest_tag();
    void dismiss();
    void open_releases_page();
}
