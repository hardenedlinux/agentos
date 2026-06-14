#pragma once

#include <cstdlib>
#include <string>
#include <string_view>
#include <unistd.h>

namespace agentos::cli {

namespace detail {

inline bool& color_enabled_flag()
{
    static bool value = []() -> bool {
        if (!isatty(STDOUT_FILENO))             return false;
        const char* no_color = std::getenv("NO_COLOR");
        if (no_color && no_color[0] != '\0')    return false;
        const char* term = std::getenv("TERM");
        if (!term || std::string_view(term) == "dumb") return false;
        return true;
    }();
    return value;
}

} // namespace detail

inline bool use_color()
{
    return detail::color_enabled_flag();
}

inline void set_color_enabled(bool enabled)
{
    detail::color_enabled_flag() = enabled;
}

namespace color {

inline std::string green(std::string_view s)
{
    if (!use_color()) return std::string(s);
    return "\033[32m" + std::string(s) + "\033[0m";
}

inline std::string yellow(std::string_view s)
{
    if (!use_color()) return std::string(s);
    return "\033[33m" + std::string(s) + "\033[0m";
}

inline std::string red(std::string_view s)
{
    if (!use_color()) return std::string(s);
    return "\033[31m" + std::string(s) + "\033[0m";
}

inline std::string cyan(std::string_view s)
{
    if (!use_color()) return std::string(s);
    return "\033[36m" + std::string(s) + "\033[0m";
}

inline std::string grey(std::string_view s)
{
    if (!use_color()) return std::string(s);
    return "\033[90m" + std::string(s) + "\033[0m";
}

inline std::string bold(std::string_view s)
{
    if (!use_color()) return std::string(s);
    return "\033[1m" + std::string(s) + "\033[0m";
}

} // namespace color

[[noreturn]]
inline void die(int exit_code, std::string_view message)
{
    if (use_color())
        fprintf(stderr, "\033[31merror:\033[0m %.*s\n",
                static_cast<int>(message.size()), message.data());
    else
        fprintf(stderr, "error: %.*s\n",
                static_cast<int>(message.size()), message.data());
    std::exit(exit_code);
}

} // namespace agentos::cli
