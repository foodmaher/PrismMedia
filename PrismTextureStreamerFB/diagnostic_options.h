#pragma once
#include <cstdint>
#include <map>
#include <sstream>
#include <string>

namespace diagnostic_options
{
    // Shared with portable parser tests. Reject partial numbers, signs,
    // overflow, duplicate keys and unknown options before touching hooks.
    inline bool number(const std::string& text, uint64_t& value)
    {
        if (text.empty() || text[0] == '-' || text[0] == '+') return false;
        size_t used{};
        try {
            value = std::stoull(text, &used,
                text.size() > 2 && text[0] == '0' &&
                (text[1] == 'x' || text[1] == 'X') ? 16 : 10);
        } catch (...) { return false; }
        return used == text.size();
    }
    inline bool label(const std::string& text)
    {
        if (text.empty() || text.size() > 64) return false;
        for (unsigned char c : text)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
        return true;
    }
    struct capture
    {
        std::string name;
        unsigned seconds{5}, slot{6}, every{1}, limit{256}, budget{30000};
        unsigned width{}, height{}, count{};
        uint64_t caller{}, ps{}, ib{}, resource{};
    };
    inline bool parse(std::istringstream& input, capture& out, std::string& error)
    {
        std::string seconds;
        uint64_t duration{};
        if (!(input >> out.name >> seconds) || !label(out.name) ||
            !number(seconds, duration) || duration < 1 || duration > 30) {
            error = "Use capture start <label> <1..30 seconds> [key=value ...]";
            return false;
        }
        out.seconds = static_cast<unsigned>(duration);
        std::map<std::string, uint64_t> seen;
        std::string token;
        while (input >> token) {
            auto split = token.find('=');
            uint64_t value{};
            if (split == std::string::npos ||
                !number(token.substr(split + 1), value) ||
                !seen.emplace(token.substr(0, split), value).second) {
                error = "Options require unique key=integer pairs (decimal or 0xhex).";
                return false;
            }
            auto key = token.substr(0, split);
            if (key == "slot" && value <= 127) out.slot = static_cast<unsigned>(value);
            else if (key == "every" && value >= 1 && value <= 10000) out.every = static_cast<unsigned>(value);
            else if (key == "limit" && value >= 1 && value <= 512) out.limit = static_cast<unsigned>(value);
            else if (key == "budget" && value >= 1 && value <= 100000) out.budget = static_cast<unsigned>(value);
            else if (key == "width" && value <= 16384) out.width = static_cast<unsigned>(value);
            else if (key == "height" && value <= 16384) out.height = static_cast<unsigned>(value);
            else if (key == "count" && value <= 0xffffffffULL) out.count = static_cast<unsigned>(value);
            else if (key == "caller") out.caller = value;
            else if (key == "ps") out.ps = value;
            else if (key == "ib") out.ib = value;
            else if (key == "resource") out.resource = value;
            else { error = "Unknown option or value outside its allowed range: " + key; return false; }
        }
        return true;
    }
}
