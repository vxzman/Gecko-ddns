#include "ip_getter.hpp"

#include "config.hpp"
#include "http_client.hpp"
#include "log.hpp"
#include "signal.hpp"
#include "text.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ip_getter {

namespace {

std::string fetch_ip_from_url(std::string_view url, std::string& err, std::stop_token stop) {
    auto result = HttpClient::get(url, {}, config::HTTP_TIMEOUT_SECONDS, config::HTTP_MAX_RETRIES, std::move(stop));
    if (!result) {
        err = std::format("HTTP error: {}", result.error());
        return "";
    }

    std::string cleaned;
    cleaned.reserve(result->body.size());
    for (const char c : result->body) {
        if (c != '\r') cleaned.push_back(c);
    }

    std::string ip;
    for (auto&& part : cleaned | std::views::split('\n')) {
        const std::string piece(part.begin(), part.end());
        const std::string trimmed{text::trim(piece)};
        if (trimmed.empty()) continue;
        ip = trimmed;
        break;
    }
    if (ip.empty()) {
        err = "Empty response from API";
        return "";
    }

    std::array<unsigned char, sizeof(in6_addr)> addr_buf{};
    if (inet_pton(AF_INET6, ip.c_str(), addr_buf.data()) != 1) {
        err = std::format("API returned non-IPv6 address: '{}'", ip);
        logger::warning("API {} returned invalid IPv6: '{}'", url, ip);
        return "";
    }

    std::array<char, INET6_ADDRSTRLEN> normalized{};
    if (inet_ntop(AF_INET6, addr_buf.data(), normalized.data(), static_cast<socklen_t>(normalized.size())) == nullptr) {
        err = std::format("Failed to normalize IPv6 address: {}", std::strerror(errno));
        return "";
    }
    return std::string{normalized.data()};
}

} // namespace

std::expected<std::vector<IPv6Info>, std::string>
get_from_apis(const std::vector<std::string>& urls, int timeout_sec) {
    if (urls.empty()) return std::unexpected("No API URLs configured");

    struct Slot {
        std::string ip;
        std::string error;
        std::atomic<bool> done{false};
        bool noted = false;
    };

    std::vector<std::unique_ptr<Slot>> slots;
    slots.reserve(urls.size());
    for (std::size_t i = 0; i < urls.size(); ++i) slots.push_back(std::make_unique<Slot>());

    std::vector<std::jthread> workers;
    workers.reserve(urls.size());
    for (std::size_t i = 0; i < urls.size(); ++i) {
        logger::info("Querying API: {}", urls[i]);
        workers.emplace_back([&, i](std::stop_token stop) {
            if (stop.stop_requested() || is_shutdown_requested()) {
                slots[i]->error = "stopped";
                slots[i]->done.store(true, std::memory_order_release);
                return;
            }
            std::string err;
            std::string ip = fetch_ip_from_url(urls[i], err, stop);
            if (!ip.empty()) slots[i]->ip = std::move(ip);
            else slots[i]->error = std::move(err);
            slots[i]->done.store(true, std::memory_order_release);
        });
    }

    const auto start = std::chrono::steady_clock::now();
    std::string last_error;
    std::optional<std::size_t> winner;

    auto cancel = [&] {
        for (auto& worker : workers) worker.request_stop();
        workers.clear();
    };

    while (!winner.has_value()) {
        if (is_shutdown_requested()) {
            logger::warning("Shutdown requested, aborting API queries");
            cancel();
            return std::unexpected("Shutdown requested during API query");
        }
        if (timeout_sec > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed >= timeout_sec) {
                logger::warning("API query timeout ({}s)", timeout_sec);
                cancel();
                return std::unexpected("API query timeout");
            }
        }

        std::size_t finished = 0;
        for (std::size_t i = 0; i < slots.size(); ++i) {
            if (!slots[i]->done.load(std::memory_order_acquire)) continue;
            ++finished;
            if (!slots[i]->noted) {
                slots[i]->noted = true;
                if (slots[i]->ip.empty()) {
                    logger::error("API {} failed: {}", urls[i], slots[i]->error);
                    last_error = slots[i]->error;
                }
            }
            if (!slots[i]->ip.empty()) {
                winner = i;
                break;
            }
        }
        if (winner.has_value()) break;
        if (finished == slots.size()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (winner.has_value()) {
        const std::string ip = slots[*winner]->ip;
        const std::string url = urls[*winner];
        cancel();
        logger::info("API {} succeeded: {}", url, ip);
        IPv6Info info{
            .ip = ip,
            .preferred_lft = config::INFINITE_LIFETIME_SECONDS,
            .valid_lft = config::INFINITE_LIFETIME_SECONDS,
        };
        populate_info(&info);
        return std::vector<IPv6Info>{std::move(info)};
    }

    cancel();
    return std::unexpected(std::format("All API requests failed: {}", last_error));
}

std::expected<std::string, std::string> select_best(const std::vector<IPv6Info>& infos) {
    auto candidates = infos | std::views::filter([](const IPv6Info& info) { return info.is_candidate; });

    if (candidates.empty()) {
        return std::unexpected("No suitable global unicast IPv6 candidate found");
    }

    auto best = std::ranges::max_element(candidates, {}, &IPv6Info::preferred_lft);
    return best->ip;
}

} // namespace ip_getter
