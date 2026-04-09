#pragma once
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace logger {

/// Log levels (matching Go version)
enum class LogLevel {
    Debug = 0,
    Info  = 1,
    Warning = 2,
    Error = 3,
    Fatal = 4,
    Success = 5,
};

// Color codes (exported for use in inline functions)
extern const char* COLOR_RESET;
extern const char* COLOR_RED;
extern const char* COLOR_GREEN;
extern const char* COLOR_YELLOW;
extern const char* COLOR_CYAN;
extern const char* COLOR_GRAY;

/// Initialize logging. output = "shell" → stdout with color;
/// any other value is treated as a file path.
bool init(const std::string& output);

/// Set minimum log level to display
void set_level(LogLevel level);

/// Get current log level
LogLevel get_level();

// Internal function (not for direct use)
void log_line(const char* level_str, const char* color, LogLevel level, const std::string& msg);

// C++23: Using std::format for type-safe formatting
template<typename... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    if (std::to_underlying(LogLevel::Debug) >= std::to_underlying(get_level())) {
        log_line("[DEBUG]", COLOR_GRAY, LogLevel::Debug, std::format(fmt, std::forward<Args>(args)...));
    }
}

template<typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    if (std::to_underlying(LogLevel::Info) >= std::to_underlying(get_level())) {
        log_line("[INFO]", COLOR_CYAN, LogLevel::Info, std::format(fmt, std::forward<Args>(args)...));
    }
}

template<typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    if (std::to_underlying(LogLevel::Error) >= std::to_underlying(get_level())) {
        log_line("[ERROR]", COLOR_RED, LogLevel::Error, std::format(fmt, std::forward<Args>(args)...));
    }
}

template<typename... Args>
void success(std::format_string<Args...> fmt, Args&&... args) {
    if (std::to_underlying(LogLevel::Success) >= std::to_underlying(get_level())) {
        log_line("[SUCCESS]", COLOR_GREEN, LogLevel::Success, std::format(fmt, std::forward<Args>(args)...));
    }
}

template<typename... Args>
void warning(std::format_string<Args...> fmt, Args&&... args) {
    if (std::to_underlying(LogLevel::Warning) >= std::to_underlying(get_level())) {
        log_line("[WARNING]", COLOR_YELLOW, LogLevel::Warning, std::format(fmt, std::forward<Args>(args)...));
    }
}

void fatal(std::string_view msg);  ///< logs then exit(1)

} // namespace logger
