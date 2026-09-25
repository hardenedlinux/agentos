#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <rapidjson/document.h>

namespace agentos::cli::fmt {

inline std::string time_ago(int64_t unix_ts) {
    int64_t diff = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count())
        - unix_ts;
    if (diff < 0)     return "future";
    if (diff < 60)    return std::to_string(diff) + "s ago";
    if (diff < 3600)  return std::to_string(diff / 60) + "min ago";
    if (diff < 86400) return std::to_string(diff / 3600) + "hr ago";
    return std::to_string(diff / 86400) + "d ago";
}

inline std::string ts(const rapidjson::Value& v, const char* key) {
    if (!v.HasMember(key) || !v[key].IsInt64()) return "-";
    return time_ago(v[key].GetInt64());
}

inline std::string str(const rapidjson::Value& v, const char* key) {
    if (!v.HasMember(key) || !v[key].IsString()) return "-";
    return v[key].GetString();
}

// Plain column: pads s to width w, appends two spaces.
// Use for headers and uncoloured data.
inline std::string col(const std::string& s, size_t w) {
    if (s.size() >= w) return s + "  ";
    return s + std::string(w - s.size(), ' ') + "  ";
}

// Coloured column: display may contain ANSI escape codes.
// raw is the plain text used to calculate visible width.
inline std::string col_colored(const std::string& display,
                                const std::string& raw, size_t w) {
    size_t visible = raw.size();
    if (visible >= w) return display + "  ";
    return display + std::string(w - visible, ' ') + "  ";
}

// Separator line using U+2500 BOX DRAWINGS LIGHT HORIZONTAL (─).
inline std::string separator(size_t w) {
    std::string s;
    s.reserve(w * 3);
    for (size_t i = 0; i < w; ++i) s += "\xe2\x94\x80";
    return s;
}

} // namespace agentos::cli::fmt
