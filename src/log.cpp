#include "log.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>

#if defined(__cpp_lib_print)
#include <print>
#endif

namespace logger {

namespace {

std::atomic<bool> g_is_terminal{false};
std::atomic<int> g_level{static_cast<int>(LogLevel::Info)};
std::atomic<FILE*> g_output{stdout};

bool check_is_terminal(FILE* file) {
    return file != nullptr && isatty(fileno(file)) != 0;
}

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    return std::format("{:%Y/%m/%d %H:%M:%S}.{:03d}",
                       std::chrono::time_point_cast<std::chrono::seconds>(now),
                       std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
}

char ascii_lower(char c) {
    if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
    return c;
}

bool ieq_at(std::string_view text, std::size_t pos, std::string_view lower_key) {
    if (pos + lower_key.size() > text.size()) return false;
    for (std::size_t i = 0; i < lower_key.size(); ++i) {
        if (ascii_lower(text[pos + i]) != lower_key[i]) return false;
    }
    return true;
}

bool is_alnum(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

enum class ValueKind { Token, Ltai, Secret };

bool value_char(char c, ValueKind kind) {
    if (is_alnum(c)) return true;
    switch (kind) {
    case ValueKind::Token: return c == '_' || c == '-';
    case ValueKind::Ltai: return false;
    case ValueKind::Secret: return c == '_' || c == '-' || c == '+' || c == '/';
    }
    return false;
}

struct KeyRule {
    std::string_view key;
    std::size_t min_len;
    ValueKind kind;
};

constexpr KeyRule kRules[] = {
    {"access_key_id", 12, ValueKind::Ltai},
    {"access-key-id", 12, ValueKind::Ltai},
    {"accesskeyid", 12, ValueKind::Ltai},
    {"private_key", 8, ValueKind::Secret},
    {"private-key", 8, ValueKind::Secret},
    {"credential", 8, ValueKind::Secret},
    {"password", 8, ValueKind::Secret},
    {"api_key", 20, ValueKind::Token},
    {"api-key", 20, ValueKind::Token},
    {"apikey", 20, ValueKind::Token},
    {"passwd", 8, ValueKind::Secret},
    {"secret", 20, ValueKind::Token},
    {"token", 20, ValueKind::Token},
    {"auth", 20, ValueKind::Token},
    {"pwd", 8, ValueKind::Secret},
    {"ak", 12, ValueKind::Ltai},
};

std::optional<std::size_t> redact_key_at(std::string& out, std::size_t pos) {
    const std::string_view view{out};
    for (const auto& rule : kRules) {
        if (!ieq_at(view, pos, rule.key)) continue;
        std::size_t cursor = pos + rule.key.size();
        if (cursor < out.size() && (is_alnum(out[cursor]) || out[cursor] == '_' || out[cursor] == '-')) continue;
        while (cursor < out.size() && (out[cursor] == ' ' || out[cursor] == '\t')) ++cursor;
        if (cursor >= out.size() || (out[cursor] != '=' && out[cursor] != ':')) continue;
        ++cursor;
        while (cursor < out.size() && (out[cursor] == ' ' || out[cursor] == '\t')) ++cursor;
        if (cursor < out.size() && (out[cursor] == '\'' || out[cursor] == '"')) ++cursor;
        if (rule.kind == ValueKind::Ltai && !ieq_at(out, cursor, "ltai")) continue;
        const std::size_t value_begin = cursor;
        while (cursor < out.size() && value_char(out[cursor], rule.kind)) ++cursor;
        if (cursor - value_begin < rule.min_len) continue;
        constexpr std::string_view kRedacted = "***REDACTED***";
        out.replace(value_begin, cursor - value_begin, kRedacted);
        return value_begin + kRedacted.size();
    }
    return std::nullopt;
}

void redact_keyed(std::string& out) {
    std::size_t i = 0;
    while (i < out.size()) {
        if (const auto next = redact_key_at(out, i)) i = *next;
        else ++i;
    }
}

void redact_bearer(std::string& out) {
    std::size_t i = 0;
    while (i < out.size()) {
        if (!ieq_at(out, i, "bearer")) {
            ++i;
            continue;
        }
        std::size_t cursor = i + 6;
        if (cursor >= out.size() || (out[cursor] != ' ' && out[cursor] != '\t')) {
            ++i;
            continue;
        }
        while (cursor < out.size() && (out[cursor] == ' ' || out[cursor] == '\t')) ++cursor;
        const std::size_t value_begin = cursor;
        while (cursor < out.size() &&
               (is_alnum(out[cursor]) || out[cursor] == '_' || out[cursor] == '-' || out[cursor] == '.')) {
            ++cursor;
        }
        if (cursor == value_begin) {
            ++i;
            continue;
        }
        constexpr std::string_view kRedacted = "***REDACTED***";
        out.replace(value_begin, cursor - value_begin, kRedacted);
        i = value_begin + kRedacted.size();
    }
}

bool is_base64_char(char c) {
    return is_alnum(c) || c == '+' || c == '/';
}

void redact_base64(std::string& out) {
    std::size_t i = 0;
    while (i < out.size()) {
        if (!is_base64_char(out[i])) {
            ++i;
            continue;
        }
        std::size_t cursor = i;
        while (cursor < out.size() && is_base64_char(out[cursor])) ++cursor;
        std::size_t equals = 0;
        while (equals < 2 && cursor + equals < out.size() && out[cursor + equals] == '=') ++equals;
        if (cursor - i >= 40) {
            constexpr std::string_view kRedacted = "***REDACTED***";
            out.replace(i, (cursor - i) + equals, kRedacted);
            i += kRedacted.size();
        } else {
            i = cursor + equals;
        }
    }
}

std::string sanitize(std::string_view msg) {
    std::string out{msg};
    redact_keyed(out);
    redact_bearer(out);
    redact_base64(out);
    return out;
}

template <typename... Args>
void write_out(FILE* out, std::format_string<Args...> fmt, Args&&... args) {
    const std::string line = std::format(fmt, std::forward<Args>(args)...);
#if defined(__cpp_lib_print)
    std::println(out, "{}", line);
#else
    std::fputs(line.c_str(), out);
    std::fputc('\n', out);
#endif
    std::fflush(out);
}

} // namespace

bool init() {
    g_is_terminal.store(check_is_terminal(g_output.load(std::memory_order_relaxed)), std::memory_order_relaxed);
    return true;
}

void set_level(LogLevel level) {
    g_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

LogLevel get_level() {
    return static_cast<LogLevel>(g_level.load(std::memory_order_relaxed));
}

void set_output(FILE* file) {
    FILE* out = file != nullptr ? file : stdout;
    g_output.store(out, std::memory_order_relaxed);
    g_is_terminal.store(check_is_terminal(out), std::memory_order_relaxed);
}

void log_line(std::string_view level_str,
              std::string_view color,
              LogLevel level,
              const std::string& msg,
              const std::source_location& loc) {
    if (std::to_underlying(level) < g_level.load(std::memory_order_relaxed)) return;

    const std::string sanitized = sanitize(msg);
    const std::string text = level == LogLevel::Debug
        ? std::format("{} ({}:{})", sanitized, loc.file_name(), loc.line())
        : sanitized;
    FILE* out = g_output.load(std::memory_order_relaxed);
    if (out == nullptr) out = stdout;
    if (g_is_terminal.load(std::memory_order_relaxed)) {
        write_out(out, "{} {}{}{} {}", timestamp(), color, level_str, COLOR_RESET, text);
    } else {
        write_out(out, "{} {} {}", timestamp(), level_str, text);
    }
}

void fatal(std::string_view msg, std::source_location loc) {
    log_line("[FATAL]", COLOR_RED, LogLevel::Fatal, std::string{msg}, loc);
    std::exit(1);
}

} // namespace logger
