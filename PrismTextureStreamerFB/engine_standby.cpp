#include "engine_standby.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>

namespace
{
    struct colour_t
    {
        uint8_t blue{};
        uint8_t green{};
        uint8_t red{};
    };

    constexpr std::array<std::array<uint8_t, 7>, 36> kGlyphs{ {
        // 0-9
        { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },
        { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
        { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },
        { 0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E },
        { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },
        { 0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E },
        { 0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E },
        { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
        { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },
        { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E },
        // A-Z
        { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
        { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },
        { 0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F },
        { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E },
        { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F },
        { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },
        { 0x0F, 0x10, 0x10, 0x17, 0x11, 0x11, 0x0F },
        { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
        { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E },
        { 0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E },
        { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 },
        { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },
        { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 },
        { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 },
        { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
        { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 },
        { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D },
        { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },
        { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E },
        { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
        { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
        { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 },
        { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A },
        { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 },
        { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 },
        { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F }
    } };

    const std::array<uint8_t, 7>* glyph(char character)
    {
        if (character >= '0' && character <= '9')
            return &kGlyphs[static_cast<size_t>(character - '0')];
        if (character >= 'A' && character <= 'Z')
        {
            return &kGlyphs[10 +
                static_cast<size_t>(character - 'A')];
        }
        return nullptr;
    }

    std::string display_text(
        const std::string& source,
        const char* fallback)
    {
        std::string result;
        result.reserve(source.size());
        bool previousSpace = true;
        for (const unsigned char raw : source)
        {
            char character = static_cast<char>(raw);
            if (raw < 128 && std::isalnum(raw))
            {
                result.push_back(static_cast<char>(std::toupper(raw)));
                previousSpace = false;
            }
            else if ((character == ' ' || character == '_' ||
                character == '-' || character == '.') && !previousSpace)
            {
                result.push_back(' ');
                previousSpace = true;
            }
        }
        while (!result.empty() && result.back() == ' ')
            result.pop_back();
        return result.empty() ? fallback : result;
    }

    void set_pixel(
        std::vector<uint8_t>& image,
        uint32_t width,
        uint32_t height,
        uint32_t x,
        uint32_t y,
        colour_t colour)
    {
        if (x >= width || y >= height)
            return;
        const size_t offset =
            (static_cast<size_t>(y) * width + x) * 4;
        image[offset + 0] = colour.blue;
        image[offset + 1] = colour.green;
        image[offset + 2] = colour.red;
        image[offset + 3] = 255;
    }

    void fill_rectangle(
        std::vector<uint8_t>& image,
        uint32_t width,
        uint32_t height,
        uint32_t left,
        uint32_t top,
        uint32_t right,
        uint32_t bottom,
        colour_t colour)
    {
        right = (std::min)(right, width);
        bottom = (std::min)(bottom, height);
        for (uint32_t y = top; y < bottom; ++y)
        {
            for (uint32_t x = left; x < right; ++x)
                set_pixel(image, width, height, x, y, colour);
        }
    }

    uint32_t text_units(const std::string& text)
    {
        if (text.empty())
            return 0;
        return static_cast<uint32_t>(text.size()) * 6 - 1;
    }

    void draw_text(
        std::vector<uint8_t>& image,
        uint32_t width,
        uint32_t height,
        const std::string& text,
        uint32_t top,
        uint32_t scale,
        colour_t colour)
    {
        if (text.empty() || scale == 0)
            return;
        const uint32_t totalWidth = text_units(text) * scale;
        uint32_t left = totalWidth < width ? (width - totalWidth) / 2 : 0;
        for (const char character : text)
        {
            const auto* rows = glyph(character);
            if (rows)
            {
                for (uint32_t row = 0; row < rows->size(); ++row)
                {
                    for (uint32_t column = 0; column < 5; ++column)
                    {
                        if (((*rows)[row] & (1u << (4 - column))) == 0)
                            continue;
                        fill_rectangle(
                            image, width, height,
                            left + column * scale,
                            top + row * scale,
                            left + (column + 1) * scale,
                            top + (row + 1) * scale,
                            colour);
                    }
                }
            }
            left += 6 * scale;
        }
    }

    uint32_t fitted_scale(
        const std::string& text,
        uint32_t availableWidth,
        uint32_t maximumScale)
    {
        const uint32_t units = text_units(text);
        if (units == 0)
            return 1;
        return (std::max)(1u, (std::min)(
            maximumScale,
            availableWidth / units));
    }

    colour_t accent_for(const std::string& brand)
    {
        uint32_t hash = 2166136261u;
        for (const unsigned char character : brand)
        {
            hash ^= character;
            hash *= 16777619u;
        }
        constexpr std::array<colour_t, 5> palette{ {
            { 236, 166, 54 },
            { 70, 190, 244 },
            { 84, 102, 235 },
            { 104, 205, 112 },
            { 216, 102, 178 }
        } };
        return palette[hash % palette.size()];
    }
}

void engine_standby::render_truck_logo(
    std::vector<uint8_t>& destination,
    uint32_t width,
    uint32_t height,
    const std::string& truckBrand,
    const std::string& truckName)
{
    if (width == 0 || height == 0)
    {
        destination.clear();
        return;
    }

    destination.resize(static_cast<size_t>(width) * height * 4);
    const std::string brand = display_text(truckBrand, "TRUCK");
    std::string name = display_text(truckName, "");
    if (name == brand)
        name.clear();
    const colour_t accent = accent_for(brand);

    // A quiet dark gradient makes the screen look powered down without being
    // completely black. It is generated only when truck identity or texture
    // size changes, not on every game frame.
    for (uint32_t y = 0; y < height; ++y)
    {
        const uint32_t shade = 5 + (height > 1 ? y * 8 / (height - 1) : 0);
        for (uint32_t x = 0; x < width; ++x)
        {
            const size_t offset =
                (static_cast<size_t>(y) * width + x) * 4;
            destination[offset + 0] = static_cast<uint8_t>(shade + 5);
            destination[offset + 1] = static_cast<uint8_t>(shade + 2);
            destination[offset + 2] = static_cast<uint8_t>(shade);
            destination[offset + 3] = 255;
        }
    }

    const uint32_t margin = (std::min)(
        (std::max)(2u, width / 28), width / 4);
    const uint32_t top = height / 5;
    const uint32_t bottom = (std::max)(top + 1, height - height / 5);
    const colour_t panel{ 19, 16, 13 };
    fill_rectangle(
        destination, width, height,
        margin, top, width - margin, bottom, panel);

    const uint32_t border = (std::max)(1u, height / 180);
    const colour_t dimAccent{
        static_cast<uint8_t>(accent.blue / 2),
        static_cast<uint8_t>(accent.green / 2),
        static_cast<uint8_t>(accent.red / 2)
    };
    fill_rectangle(
        destination, width, height,
        margin, top, width - margin, top + border, dimAccent);
    fill_rectangle(
        destination, width, height,
        margin, bottom - border, width - margin, bottom, dimAccent);
    fill_rectangle(
        destination, width, height,
        margin, top, margin + border, bottom, dimAccent);
    fill_rectangle(
        destination, width, height,
        width - margin - border, top, width - margin, bottom, dimAccent);

    const uint32_t availableWidth = width > margin * 4
        ? width - margin * 4 : width;
    const uint32_t brandMaximumScale = (std::max)(1u, height / 18);
    const uint32_t brandScale = fitted_scale(
        brand, availableWidth, brandMaximumScale);
    const uint32_t brandHeight = brandScale * 7;
    const uint32_t brandGap = (std::max)(2u, height / 36);
    uint32_t brandTop = name.empty()
        ? (height > brandHeight ? (height - brandHeight) / 2 : 0)
        : (height / 2 > brandHeight + brandGap
            ? height / 2 - brandHeight - brandGap : 0);
    draw_text(
        destination, width, height,
        brand, brandTop, brandScale, accent);

    if (!name.empty())
    {
        const uint32_t nameMaximumScale = (std::max)(1u, height / 32);
        const uint32_t nameScale = fitted_scale(
            name, availableWidth, nameMaximumScale);
        const uint32_t maximumNameTop = height > nameScale * 7
            ? height - nameScale * 7 : 0;
        const uint32_t nameTop =
            (std::min)(maximumNameTop,
                brandTop + brandHeight + (std::max)(3u, height / 22));
        draw_text(
            destination, width, height,
            name, nameTop, nameScale, { 194, 202, 210 });
    }
}
