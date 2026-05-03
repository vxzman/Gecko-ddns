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

bool g_is_terminal = false;
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
    std::string out = msg;
    
    // Pattern 1: API tokens, secrets (20+ chars)
    // Matches: token=xxx, api_key=xxx, secret=xxx, api-token=xxx, etc.
    static const std::regex token_re(
        R"(((?:token|api[_\-]?key|secret|auth)[\s:=]+)['\"]?([a-zA-Z0-9_\-]{20,})['\"]?)",
        std::regex::icase);
    out = std::regex_replace(out, token_re, "$1***REDACTED***");
    
    // Pattern 2: Access Key ID (Alibaba Cloud style: LTAI + 12+ chars)
    static const std::regex ak_re(
        R"(((?:access[_\-]?key[_\-]?id|ak)[\s:=]+)['\"]?(LTAI[a-zA-Z0-9]{8,})['\"]?)",
        std::regex::icase);
    out = std::regex_replace(out, ak_re, "$1***REDACTED***");
    
    // Pattern 3: Generic key=value with long alphanumeric values (potential secrets)
    static const std::regex kv_re(
        R"(((?:password|passwd|pwd|credential|private[_\-]?key)[\s:=]+)['\"]?([a-zA-Z0-9_\-+/]{8,})['\"]?)",
        std::regex::icase);
    out = std::regex_replace(out, kv_re, "$1***REDACTED***");
    
    // Pattern 4: Bearer tokens in headers
    static const std::regex bearer_re(
        R"(Bearer\s+[a-zA-Z0-9_\-\.]+)",
        std::regex::icase);
    out = std::regex_replace(out, bearer_re, "Bearer ***REDACTED***");
    
    // Pattern 5: Base64-like strings that might be credentials (40+ chars)
    // Only redact if they look like they could be secrets
    static const std::regex base64_re(
        R"([A-Za-z0-9+/]{40,}={0,2})");
    out = std::regex_replace(out, base64_re, "***REDACTED***");
    
    return out;
}

} // anonymous namespace

bool init() {
    g_is_terminal = check_is_terminal(stdout);
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

    if (g_is_terminal) {
        std::string txt = std::format("{} {}{}{} {}\n", timestamp(), color, level_str, COLOR_RESET, sanitized);
        std::fputs(txt.c_str(), stdout);
    } else {
        std::string txt = std::format("{} {} {}\n", timestamp(), level_str, sanitized);
        std::fputs(txt.c_str(), stdout);
    }
    std::fflush(stdout);
}

void fatal(std::string_view msg) {
    log_line("[FATAL]", COLOR_RED, LogLevel::Fatal, std::string(msg));
    exit(1);
}

} // namespace logger
