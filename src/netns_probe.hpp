#pragma once

/// Network-namespace probe helpers.
/// Path checks and JSON parsing are available on every platform.
/// Entering a namespace (setns / fork / exec) is Linux-only.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

#ifndef AF_INET6
#define AF_INET6 10
#endif

namespace netns {

inline constexpr int kExitOk = 0;
inline constexpr int kExitMissing = 10;
inline constexpr int kExitPermission = 11;
inline constexpr int kExitNoAddress = 12;
inline constexpr int kExitNoIface = 13;
inline constexpr int kExitBadOutput = 14;

struct ProbeResult {
    std::string ip;
    std::string source;
    std::string iface;
    std::string netns;
    long preferred_lft = 0;
    long valid_lft = 0;
};

namespace detail {

[[nodiscard]] inline bool netns_name_ok(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    if (name == "." || name == ".." || name.find("..") != std::string_view::npos) return false;
    for (const char c : name) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

} // namespace detail

/// `vpn` becomes `/run/netns/vpn`. An absolute path must stay under `/run/netns` after normalization.
[[nodiscard]] inline std::expected<std::filesystem::path, std::string>
resolve_netns_path(std::string_view name) {
    if (name.empty()) return std::unexpected("invalid netns name");
    if (name.find("..") != std::string_view::npos) return std::unexpected("invalid netns name");

    std::filesystem::path path{std::string{name}};
    if (path.is_absolute()) {
        path = path.lexically_normal();
        if (path.parent_path() != "/run/netns") {
            return std::unexpected("netns path must be under /run/netns");
        }
        if (!detail::netns_name_ok(path.filename().string())) {
            return std::unexpected("invalid netns name");
        }
        return path;
    }
    if (!detail::netns_name_ok(name)) return std::unexpected("invalid netns name");
    return std::filesystem::path{"/run/netns"} / path;
}

/// Parse the single JSON line written by the probe child.
[[nodiscard]] inline std::expected<ProbeResult, std::string>
parse_probe_output(std::string_view out) {
    try {
        const auto json = nlohmann::json::parse(out);
        ProbeResult result{
            .ip = json.at("ip").get<std::string>(),
            .source = json.at("source").get<std::string>(),
            .iface = json.at("iface").get<std::string>(),
            .netns = json.at("netns").get<std::string>(),
        };
        const auto preferred = json.at("preferred_lft").get<std::int64_t>();
        const auto valid = json.at("valid_lft").get<std::int64_t>();
        if (preferred < 0 || valid < 0) return std::unexpected("probe lifetime is negative");
        result.preferred_lft = static_cast<long>(preferred);
        result.valid_lft = static_cast<long>(valid);
        if (result.source != "interface" && result.source != "api") {
            return std::unexpected("probe source is invalid");
        }
        if (result.ip.empty()) return std::unexpected("probe ip is empty");
        return result;
    } catch (const nlohmann::json::exception& ex) {
        return std::unexpected(std::format("probe output parse failed: {}", ex.what()));
    }
}

/// Global unicast IPv6: not IPv4-mapped, not ULA, not link-local, not loopback.
[[nodiscard]] inline std::expected<void, std::string>
validate_global_ipv6(std::string_view ip) {
    const std::string owned{ip};
    std::array<unsigned char, 16> addr{};
    if (inet_pton(AF_INET6, owned.c_str(), addr.data()) != 1) {
        return std::unexpected("probe ip is not IPv6");
    }

    bool v4_mapped = true;
    for (std::size_t i = 0; i < 10; ++i) {
        if (addr[i] != 0) v4_mapped = false;
    }
    if (v4_mapped && addr[10] == 0xff && addr[11] == 0xff) {
        return std::unexpected("probe ip is IPv4-mapped");
    }

    bool loopback = true;
    for (std::size_t i = 0; i < 15; ++i) {
        if (addr[i] != 0) loopback = false;
    }
    if (loopback && addr[15] == 1) return std::unexpected("probe ip is loopback");

    if (addr[0] == 0xfe && (addr[1] & 0xc0) == 0x80) {
        return std::unexpected("probe ip is link-local");
    }
    if ((addr[0] & 0xfe) == 0xfc) return std::unexpected("probe ip is ULA");
    if ((addr[0] & 0xe0) != 0x20) return std::unexpected("probe ip is not global unicast");
    return {};
}

#if defined(__linux__)

/// Fork and exec this binary's `__probe` inside the named netns.
/// Failure is a probe exit code (10–14, or 127 when exec fails). The host APIs are not consulted.
[[nodiscard]] std::expected<ProbeResult, int>
run_probe(std::string_view netns_name,
          std::string_view iface,
          const std::vector<std::string>& fallback_urls,
          int timeout_sec);

/// `__probe` body. Runs only after exec, in a fresh single-threaded process.
[[nodiscard]] int execute_probe(std::string_view netns_name,
                                std::string_view iface,
                                const std::vector<std::string>& fallback_urls,
                                int timeout_sec);

#endif

} // namespace netns
