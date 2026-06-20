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

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

namespace ip_getter {

// ─── HTTP API getter ──────────────────────────────────────────────────────────

namespace {

static std::string fetch_ip_from_url(const std::string& url, std::string& err) {
    auto result = HttpClient::get(url, {}, config::HTTP_TIMEOUT_SECONDS, config::HTTP_MAX_RETRIES);
    if (!result) {
        err = result.error();
        return "";
    }

    // Parse response - extract first line and trim
    std::string body = result->body;
    auto newline_pos = body.find('\n');
    if (newline_pos != std::string::npos) {
        body = body.substr(0, newline_pos);
    }

    // Trim whitespace
    auto is_space = [](unsigned char c) { return std::isspace(c); };
    auto trimmed = body | std::views::drop_while(is_space)
                         | std::views::reverse
                         | std::views::drop_while(is_space)
                         | std::views::reverse;
    std::string ip(trimmed.begin(), trimmed.end());

    if (ip.empty()) {
        err = "Empty response from API";
        return "";
    }

    return ip;
}

} // anonymous namespace

std::expected<std::vector<IPv6Info>, std::string> get_from_apis(const std::vector<std::string>& urls) {
    if (urls.empty()) { return std::unexpected("No API URLs configured"); }

    // Query all URLs concurrently
    std::vector<std::thread> threads;
    std::vector<std::string> results(urls.size());
    std::vector<std::string> errors(urls.size());

    for (size_t i = 0; i < urls.size(); ++i) {
        logger::info("Querying API: {}", urls[i]);
        threads.emplace_back([&urls, &results, &errors, i]() {
            std::string err;
            std::string ip = fetch_ip_from_url(urls[i], err);
            results[i] = std::move(ip);
            errors[i]  = std::move(err);
        });
    }

    // Wait for all and collect results
    for (auto& t : threads) {
        t.join();
    }

    // Return the first successful result
    for (size_t i = 0; i < urls.size(); ++i) {
        if (!results[i].empty()) {
            logger::info("API {} succeeded: {}", urls[i], results[i]);
            IPv6Info info;
            info.ip            = results[i];
            info.preferred_lft = config::INFINITE_LIFETIME_SECONDS;
            info.valid_lft     = config::INFINITE_LIFETIME_SECONDS;
            populate_info(&info);
            return std::vector<IPv6Info>{info};
        }
        logger::error("API {} failed: {}", urls[i], errors[i]);
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
