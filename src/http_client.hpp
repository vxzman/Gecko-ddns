#pragma once

#include <curl/curl.h>
#include <expected>
#include <map>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>

struct HttpResponse {
    int status_code = 0;
    std::string body;
};

/// HTTP client with retry support.
class HttpClient {
public:
    [[nodiscard]] static std::expected<HttpResponse, std::string> get(
        std::string_view url,
        const std::map<std::string, std::string>& headers = {},
        int timeout = 15,
        int max_retries = 3,
        std::stop_token stop = {});

    [[nodiscard]] static std::expected<HttpResponse, std::string> post(
        std::string_view url,
        std::string_view body,
        const std::map<std::string, std::string>& headers = {},
        int timeout = 15,
        int max_retries = 3,
        std::stop_token stop = {});

    static void set_proxy(std::string_view proxy_url);
    static void clear_proxy();

private:
    [[nodiscard]] static std::expected<HttpResponse, std::string> request(
        std::string_view url,
        std::string_view method,
        std::string_view post_body,
        const std::map<std::string, std::string>& headers,
        int timeout,
        int max_retries,
        std::stop_token stop);

    static inline std::mutex proxy_mutex_;
    static inline std::string proxy_url_;
};
