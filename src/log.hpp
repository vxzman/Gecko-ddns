#pragma once

#include <cstdio>
#include <format>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace logger {

enum class LogLevel {
    Debug = 0,
    Info = 1,
    Warning = 2,
    Error = 3,
    Fatal = 4,
    Success = 5,
};

inline constexpr std::string_view COLOR_RESET = "\033[0m";
inline constexpr std::string_view COLOR_RED = "\033[31m";
inline constexpr std::string_view COLOR_GREEN = "\033[32m";
inline constexpr std::string_view COLOR_YELLOW = "\033[33m";
inline constexpr std::string_view COLOR_CYAN = "\033[36m";
inline constexpr std::string_view COLOR_GRAY = "\033[90m";

[[nodiscard]] bool init();
void set_level(LogLevel level);
[[nodiscard]] LogLevel get_level();

/// Send later log lines to this FILE. The probe child uses stderr so stdout stays JSON.
void set_output(FILE* file);

void log_line(std::string_view level_str,
              std::string_view color,
              LogLevel level,
              const std::string& msg,
              const std::source_location& loc);

namespace detail {

template <typename... Args>
struct FormatWithLoc {
    std::format_string<Args...> fmt;
    std::source_location loc;

    template <typename T>
    consteval FormatWithLoc(const T& text, std::source_location where = std::source_location::current())
        : fmt(text), loc(where) {}
};

} // namespace detail

template <typename... Args>
    requires (sizeof...(Args) > 0)
void debug(detail::FormatWithLoc<std::remove_cvref_t<Args>...> fmt, Args&&... args) {
    if (std::to_underlying(LogLevel::Debug) >= std::to_underlying(get_level())) {
        log_line("[DEBUG]", COLOR_GRAY, LogLevel::Debug,
                 std::vformat(fmt.fmt.get(), std::make_format_args(args...)), fmt.loc);
    }
}

template <typename... Args>
    requires (sizeof...(Args) > 0)
void info(detail::FormatWithLoc<std::remove_cvref_t<Args>...> fmt, Args&&... args) {
    if (std::to_underlying(LogLevel::Info) >= std::to_underlying(get_level())) {
        log_line("[INFO]", COLOR_CYAN, LogLevel::Info,
                 std::vformat(fmt.fmt.get(), std::make_format_args(args...)), fmt.loc);
    }
}

template <typename... Args>
    requires (sizeof...(Args) > 0)
void error(detail::FormatWithLoc<std::remove_cvref_t<Args>...> fmt, Args&&... args) {
    if (std::to_underlying(LogLevel::Error) >= std::to_underlying(get_level())) {
        log_line("[ERROR]", COLOR_RED, LogLevel::Error,
                 std::vformat(fmt.fmt.get(), std::make_format_args(args...)), fmt.loc);
    }
}

template <typename... Args>
    requires (sizeof...(Args) > 0)
void success(detail::FormatWithLoc<std::remove_cvref_t<Args>...> fmt, Args&&... args) {
    if (std::to_underlying(LogLevel::Success) >= std::to_underlying(get_level())) {
        log_line("[SUCCESS]", COLOR_GREEN, LogLevel::Success,
                 std::vformat(fmt.fmt.get(), std::make_format_args(args...)), fmt.loc);
    }
}

template <typename... Args>
    requires (sizeof...(Args) > 0)
void warning(detail::FormatWithLoc<std::remove_cvref_t<Args>...> fmt, Args&&... args) {
    if (std::to_underlying(LogLevel::Warning) >= std::to_underlying(get_level())) {
        log_line("[WARNING]", COLOR_YELLOW, LogLevel::Warning,
                 std::vformat(fmt.fmt.get(), std::make_format_args(args...)), fmt.loc);
    }
}

inline void debug(std::string_view msg, std::source_location loc = std::source_location::current()) {
    if (std::to_underlying(LogLevel::Debug) >= std::to_underlying(get_level())) {
        log_line("[DEBUG]", COLOR_GRAY, LogLevel::Debug, std::string{msg}, loc);
    }
}

inline void info(std::string_view msg, std::source_location loc = std::source_location::current()) {
    if (std::to_underlying(LogLevel::Info) >= std::to_underlying(get_level())) {
        log_line("[INFO]", COLOR_CYAN, LogLevel::Info, std::string{msg}, loc);
    }
}

inline void error(std::string_view msg, std::source_location loc = std::source_location::current()) {
    if (std::to_underlying(LogLevel::Error) >= std::to_underlying(get_level())) {
        log_line("[ERROR]", COLOR_RED, LogLevel::Error, std::string{msg}, loc);
    }
}

inline void success(std::string_view msg, std::source_location loc = std::source_location::current()) {
    if (std::to_underlying(LogLevel::Success) >= std::to_underlying(get_level())) {
        log_line("[SUCCESS]", COLOR_GREEN, LogLevel::Success, std::string{msg}, loc);
    }
}

inline void warning(std::string_view msg, std::source_location loc = std::source_location::current()) {
    if (std::to_underlying(LogLevel::Warning) >= std::to_underlying(get_level())) {
        log_line("[WARNING]", COLOR_YELLOW, LogLevel::Warning, std::string{msg}, loc);
    }
}

[[noreturn]] void fatal(std::string_view msg,
                        std::source_location loc = std::source_location::current());

} // namespace logger
