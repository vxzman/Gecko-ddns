#include "http_client.hpp"

#include "curl_raii.hpp"
#include "log.hpp"
#include "signal.hpp"

#include <chrono>
#include <format>
#include <string>
#include <thread>

namespace {

size_t curl_write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    const size_t realsize = size * nmemb;
    static_cast<std::string*>(userp)->append(static_cast<char*>(contents), realsize);
    return realsize;
}

struct Progress {
    const std::stop_token* stop = nullptr;
};

int curl_progress_cb(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto* progress = static_cast<const Progress*>(clientp);
    if (progress != nullptr && progress->stop != nullptr && progress->stop->stop_requested()) return 1;
    if (is_shutdown_requested()) return 1;
    return 0;
}

} // namespace

std::expected<HttpResponse, std::string> HttpClient::request(
    std::string_view url,
    std::string_view method,
    std::string_view post_body,
    const std::map<std::string, std::string>& headers,
    int timeout,
    int max_retries,
    std::stop_token stop) {

    CurlPtr curl{curl_easy_init()};
    if (!curl) return std::unexpected("Failed to initialize CURL");

    const std::string url_owned{url};
    const std::string method_owned{method};
    const std::string body_owned{post_body};
    std::string proxy;
    {
        std::lock_guard<std::mutex> lock(proxy_mutex_);
        proxy = proxy_url_;
    }

    SlistPtr header_list;
    for (const auto& [key, value] : headers) {
        if (!append_slist(header_list, std::format("{}: {}", key, value))) {
            return std::unexpected("failed to append header");
        }
    }

    std::string response_body;
    Progress progress{.stop = &stop};
    curl_easy_setopt(curl.get(), CURLOPT_URL, url_owned.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, static_cast<long>(timeout));
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "curl/8.0.0");
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, curl_progress_cb);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &progress);
    if (header_list) curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, header_list.get());
    if (!proxy.empty()) curl_easy_setopt(curl.get(), CURLOPT_PROXY, proxy.c_str());
    if (method_owned == "POST") curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body_owned.c_str());

    auto wait_retry = [&](int seconds) {
        const int slices = seconds * 10;
        for (int i = 0; i < slices; ++i) {
            if (stop.stop_requested() || is_shutdown_requested()) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return true;
    };

    for (int attempt = 0; attempt <= max_retries; ++attempt) {
        if (stop.stop_requested() || is_shutdown_requested()) {
            return std::unexpected(std::format("HTTP {} cancelled", method_owned));
        }
        response_body.clear();
        const CURLcode res = curl_easy_perform(curl.get());
        if (res == CURLE_ABORTED_BY_CALLBACK || stop.stop_requested() || is_shutdown_requested()) {
            return std::unexpected(std::format("HTTP {} cancelled", method_owned));
        }
        if (res == CURLE_OK) {
            long response_code = 0;
            curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &response_code);
            if (response_code >= 200 && response_code < 300) {
                return HttpResponse{.status_code = static_cast<int>(response_code), .body = response_body};
            }
            if (attempt < max_retries) {
                const int delay = 1 << attempt;
                logger::debug("HTTP {} {} returned {}, retrying in {}s...", method_owned, url_owned, response_code, delay);
                if (!wait_retry(delay)) return std::unexpected(std::format("HTTP {} cancelled", method_owned));
                continue;
            }
            return std::unexpected(std::format("HTTP {} failed with status {}", method_owned, response_code));
        }
        if (attempt < max_retries) {
            const int delay = 1 << attempt;
            logger::debug("HTTP {} {} failed: {}, retrying in {}s...", method_owned, url_owned, curl_easy_strerror(res), delay);
            if (!wait_retry(delay)) return std::unexpected(std::format("HTTP {} cancelled", method_owned));
            continue;
        }
        return std::unexpected(std::format("HTTP {} failed after {} retries: {}",
                                           method_owned, max_retries, curl_easy_strerror(res)));
    }
    return std::unexpected(std::format("HTTP {} failed: unknown error", method_owned));
}

std::expected<HttpResponse, std::string> HttpClient::get(
    std::string_view url,
    const std::map<std::string, std::string>& headers,
    int timeout,
    int max_retries,
    std::stop_token stop) {
    return request(url, "GET", {}, headers, timeout, max_retries, std::move(stop));
}

std::expected<HttpResponse, std::string> HttpClient::post(
    std::string_view url,
    std::string_view body,
    const std::map<std::string, std::string>& headers,
    int timeout,
    int max_retries,
    std::stop_token stop) {
    return request(url, "POST", body, headers, timeout, max_retries, std::move(stop));
}

void HttpClient::set_proxy(std::string_view proxy_url) {
    std::lock_guard<std::mutex> lock(proxy_mutex_);
    proxy_url_ = std::string{proxy_url};
}

void HttpClient::clear_proxy() {
    std::lock_guard<std::mutex> lock(proxy_mutex_);
    proxy_url_.clear();
}
