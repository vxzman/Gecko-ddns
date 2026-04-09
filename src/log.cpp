#include "log.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <unistd.h>

namespace logger {

// Color codes (defined here for use in inline functions)
const char* COLOR_RESET   = "\033[0m";
const char* COLOR_RED     = "\033[31m";
const char* COLOR_GREEN   = "\033[32m";
const char* COLOR_YELLOW  = "\033[33m";
const char* COLOR_CYAN    = "\033[36m";
const char* COLOR_GRAY    = "\033[90m";

namespace {

FILE* g_out         = nullptr;
bool  g_is_terminal = false;
LogLevel g_level    = LogLevel::Info;  // Default to Info

bool check_is_terminal(FILE* f) {
    return isatty(fileno(f)) != 0;
}

std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    return std::format("{:%Y/%m/%d %H:%M:%S}.{:03d}",
                       std::chrono::time_point_cast<std::chrono::seconds>(now),
                       std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
}

std::string sanitize(const std::string& msg) {
    static const std::regex token_re(
        R"((?:token|api[_\-]?key|secret)[\s:=]+'?"?([a-zA-Z0-9_\-]{20,})'?"?)",
        std::regex::icase);
    static const std::regex ak_re(
        R"((?:access[_\-]?key[_\-]?id)[\s:=]+'?"?([a-zA-Z0-9]{12,})'?"?)",
        std::regex::icase);
    std::string out = std::regex_replace(msg, token_re, "***REDACTED***");
    out = std::regex_replace(out, ak_re, "***REDACTED***");
    return out;
}

} // anonymous namespace

bool init(const std::string& output) {
    if (output.empty() || output == "shell") {
        g_out         = nullptr;
        g_is_terminal = check_is_terminal(stdout);
    } else {
        g_out = fopen(output.c_str(), "a");
        if (!g_out) {
            fprintf(stderr, "Failed to open log file: %s\n", output.c_str());
            return false;
        }
        g_is_terminal = false;
    }
    return true;
}

void set_level(LogLevel level) {
    g_level = level;
}

LogLevel get_level() {
    return g_level;
}

void log_line(const char* level_str, const char* color, LogLevel level, const std::string& msg) {
    if (std::to_underlying(level) < std::to_underlying(g_level)) {
        return;
    }

    std::string sanitized = sanitize(msg);

    FILE* target = g_out ? g_out : stdout;

    if (g_is_terminal) {
        std::string txt = std::format("{} {}{}{} {}\n", timestamp(), color, level_str, COLOR_RESET, sanitized);
        std::fputs(txt.c_str(), target);
    } else {
        std::string txt = std::format("{} {} {}\n", timestamp(), level_str, sanitized);
        std::fputs(txt.c_str(), target);
    }
    std::fflush(target);
}

void fatal(std::string_view msg) {
    log_line("[FATAL]", COLOR_RED, LogLevel::Fatal, std::string(msg));
    exit(1);
}

} // namespace logger
