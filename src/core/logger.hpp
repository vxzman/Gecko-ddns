#pragma once

#include "core/types.hpp"
#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <format>
#include <sstream>
#include <iomanip>

namespace alasia::logger {

namespace {

std::mutex g_mutex;
std::optional<std::ofstream> g_log_file;
bool g_initialized = false;

std::string get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    std::ostringstream oss;
    oss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

void write_log(const std::string& level, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_mutex);
    
    std::string line = std::format("[{}] [{}] {}\n", get_timestamp(), level, message);
    
    if (g_log_file.has_value()) {
        g_log_file->write(line.c_str(), static_cast<std::streamsize>(line.size()));
        g_log_file->flush();
    } else {
        std::cout << line << std::flush;
    }
}

} // anonymous namespace

/// Initialize logger with output path
inline bool init(std::string output) {
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (g_initialized) {
        return true;
    }
    
    if (output == "shell" || output.empty()) {
        g_log_file.reset();
    } else {
        g_log_file.emplace(output, std::ios::app);
        if (!g_log_file->is_open()) {
            g_log_file.reset();
            return false;
        }
    }
    
    g_initialized = true;
    return true;
}

/// Shutdown logger
inline void shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_log_file.reset();
    g_initialized = false;
}

/// Log info message
template<typename... Args>
inline void info(std::string_view format, Args&&... args) {
    std::string message = std::vformat(format, std::make_format_args(args...));
    write_log("INFO", message);
}

/// Log warning message
template<typename... Args>
inline void warning(std::string_view format, Args&&... args) {
    std::string message = std::vformat(format, std::make_format_args(args...));
    write_log("WARN", message);
}

/// Log error message
template<typename... Args>
inline void error(std::string_view format, Args&&... args) {
    std::string message = std::vformat(format, std::make_format_args(args...));
    write_log("ERROR", message);
}

/// Log success message
template<typename... Args>
inline void success(std::string_view format, Args&&... args) {
    std::string message = std::vformat(format, std::make_format_args(args...));
    write_log("SUCCESS", message);
}

/// Log debug message (only in debug build)
template<typename... Args>
inline void debug(std::string_view format, Args&&... args) {
#ifdef DEBUG
    std::string message = std::vformat(format, std::make_format_args(args...));
    write_log("DEBUG", message);
#else
    (void)format;
    ((void)args, ...);
#endif
}

} // namespace alasia::logger
