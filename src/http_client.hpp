#pragma once

#include <curl/curl.h>
#include <expected>
#include <string>
#include <map>

struct HttpResponse {
    int status_code;
    std::string body;
};

/// HTTP client with retry support
class HttpClient {
public:
    /// GET request with retry
    static std::expected<HttpResponse, std::string> get(
        const std::string& url,
        const std::map<std::string, std::string>& headers = {},
        int timeout = 15,
        int max_retries = 3);
    
    /// POST request with retry
    static std::expected<HttpResponse, std::string> post(
        const std::string& url,
        const std::string& body,
        const std::map<std::string, std::string>& headers = {},
        int timeout = 15,
        int max_retries = 3);
    
    /// Set global proxy (affects all requests)
    static void set_proxy(const std::string& proxy_url);
    
    /// Clear global proxy
    static void clear_proxy();

private:
    static std::expected<HttpResponse, std::string> request(
        const std::string& url,
        const std::string& method,
        const std::string& post_body,
        const std::map<std::string, std::string>& headers,
        int timeout,
        int max_retries);
    
    static size_t write_callback(void* contents, size_t size, size_t nmemb, std::string* userp);
    
    static inline std::string proxy_url_;
};
