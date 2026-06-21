// Shared platform-independent IPv6 address retrieval:
//   - HTTP API getter (fallback when platform interface API fails)
//   - select_best (choose best candidate from results)
//
// Platform-specific get_from_interface() implementations live in:
//   ip_getter_linux.cpp    (Linux, Netlink socket)
//   ip_getter_freebsd.cpp  (FreeBSD, Netlink SNL API)
//   ip_getter_bsd.cpp      (OpenBSD, ioctl SIOCGIFALIFETIME_IN6)

#include "ip_getter.hpp"
#include "config.hpp"
#include "curl_pool.hpp"
#include "http_client.hpp"
#include "log.hpp"
#include "signal.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <chrono>
#include <cstring>
#include <future>
#include <ranges>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ip_getter {

// ─── HTTP API getter ──────────────────────────────────────────────────────────

namespace {

static std::string fetch_ip_from_url(const std::string& url, std::string& err) {
    auto result = HttpClient::get(url, {}, config::HTTP_TIMEOUT_SECONDS, config::HTTP_MAX_RETRIES);
    if (!result) {
        err = "HTTP error: " + result.error();
        return "";
    }

    const std::string& body = result->body;

    // Strip carriage returns (handle \r\n line endings)
    std::string cleaned;
    cleaned.reserve(body.size());
    for (char c : body) {
        if (c != '\r') cleaned += c;
    }

    // Extract first non-empty line
    std::istringstream stream(cleaned);
    std::string line;
    std::string ip;
    while (std::getline(stream, line)) {
        // Trim whitespace from both ends
        size_t start = line.find_first_not_of(" \t\n\v\f\r");
        if (start == std::string::npos) continue;  // blank line
        size_t end = line.find_last_not_of(" \t\n\v\f\r");
        ip = line.substr(start, end - start + 1);
        break;
    }

    if (ip.empty()) {
        err = "Empty response from API";
        return "";
    }

    // Validate that the result is a well-formed IPv6 address
    unsigned char addr_buf[sizeof(struct in6_addr)];
    if (inet_pton(AF_INET6, ip.c_str(), addr_buf) != 1) {
        err = "API returned non-IPv6 address: '" + ip + "'";
        logger::warning("API {} returned invalid IPv6: '{}'", url, ip);
        return "";
    }

    // Normalize to canonical IPv6 format (handles ::, leading zeros, etc.)
    char normalized[INET6_ADDRSTRLEN];
    if (inet_ntop(AF_INET6, addr_buf, normalized, sizeof(normalized)) == nullptr) {
        err = "Failed to normalize IPv6 address: " + std::string(strerror(errno));
        return "";
    }

    return std::string(normalized);
}

} // anonymous namespace

std::expected<std::vector<IPv6Info>, std::string> get_from_apis(const std::vector<std::string>& urls, int timeout_sec) {
    if (urls.empty()) { return std::unexpected("No API URLs configured"); }

    // Use packaged_task + future for interruptible waiting
    struct ApiResult { std::string ip; std::string error; };
    std::vector<std::pair<std::thread, std::future<ApiResult>>> tasks;
    tasks.reserve(urls.size());

    for (size_t i = 0; i < urls.size(); ++i) {
        logger::info("Querying API: {}", urls[i]);
        std::packaged_task<ApiResult()> task([url = urls[i]]() -> ApiResult {
            ApiResult r;
            r.ip = fetch_ip_from_url(url, r.error);
            return r;
        });
        auto future = task.get_future();
        tasks.emplace_back(std::thread(std::move(task)), std::move(future));
    }

    auto start_time = std::chrono::steady_clock::now();

    // Wait with polling — check for shutdown and timeout
    for (auto& [thread, future] : tasks) {
        while (future.wait_for(std::chrono::milliseconds(500)) != std::future_status::ready) {
            if (is_shutdown_requested()) {
                logger::warning("Shutdown requested, aborting API queries");
                // Detach remaining threads (they write to their own stack, which is safe here
                // because each packaged_task owns its result)
                for (auto& [t, f] : tasks) {
                    if (t.joinable()) t.detach();
                }
                return std::unexpected("Shutdown requested during API query");
            }
            if (timeout_sec > 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - start_time).count();
                if (elapsed >= timeout_sec) {
                    logger::warning("API query timeout ({}s)", timeout_sec);
                    for (auto& [t, f] : tasks) {
                        if (t.joinable()) t.detach();
                    }
                    return std::unexpected("API query timeout");
                }
            }
        }
    }

    // Collect results — return first success
    for (auto& [thread, future] : tasks) {
        if (thread.joinable()) thread.join();
    }
    for (size_t i = 0; i < tasks.size(); ++i) {
        auto result = tasks[i].second.get();
        if (!result.ip.empty()) {
            logger::info("API {} succeeded: {}", urls[i], result.ip);
            IPv6Info info;
            info.ip            = result.ip;
            info.preferred_lft = config::INFINITE_LIFETIME_SECONDS;
            info.valid_lft     = config::INFINITE_LIFETIME_SECONDS;
            populate_info(&info);
            return std::vector<IPv6Info>{info};
        }
        logger::error("API {} failed: {}", urls[i], result.error);
    }
    return std::unexpected("All API requests failed");
}

// ─── Select best ─────────────────────────────────────────────────────────────

std::expected<std::string, std::string> select_best(const std::vector<IPv6Info>& infos) {
    auto candidates = infos | std::views::filter([](const IPv6Info& info) { return info.is_candidate; });

    if (candidates.empty()) {
        return std::unexpected("No suitable global unicast IPv6 candidate found");
    }

    auto best = std::ranges::max_element(candidates, {}, &IPv6Info::preferred_lft);
    return best->ip;
}

} // namespace ip_getter
