#include "http/http_client.hpp"
#include "core/logger.hpp"
#include "core/types.hpp"

#include <chrono>
#include <thread>
#include <curl/curl.h>

namespace alasia::http {

namespace {

// Write callback for CURL response body
size_t curl_write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    static_cast<std::string*>(userp)->append(static_cast<char*>(contents), realsize);
    return realsize;
}

/// Default HTTP client implementation
class DefaultHttpClient : public HttpClient {
public:
    explicit DefaultHttpClient(int timeout = HTTP_TIMEOUT_SECONDS,
                               int max_retries = HTTP_MAX_RETRIES)
        : timeout_(timeout), max_retries_(max_retries) {}
    
    Result<Response> send(Method method,
                          const std::string& url,
                          const std::string& body,
                          const std::map<std::string, std::string>& headers) override;
    
    void set_proxy(const std::string& proxy_url) override {
        proxy_url_ = proxy_url;
    }

private:
    Result<Response> execute_request(Method method,
                                     const std::string& url,
                                     const std::string& body,
                                     const std::map<std::string, std::string>& headers);
    
    int timeout_;
    int max_retries_;
    std::string proxy_url_;
};

Result<Response> DefaultHttpClient::send(Method method,
                                          const std::string& url,
                                          const std::string& body,
                                          const std::map<std::string, std::string>& headers) {
    for (int attempt = 0; attempt <= max_retries_; ++attempt) {
        auto result = execute_request(method, url, body, headers);
        
        if (result.is_ok()) {
            return result;
        }
        
        if (attempt < max_retries_) {
            int delay = 1 << attempt;  // Exponential backoff: 1s, 2s, 4s...
            logger::debug("HTTP request failed (attempt {}), retrying in {}s: {}",
                         attempt + 1, delay, result.error_message());
            std::this_thread::sleep_for(std::chrono::seconds(delay));
        }
    }

    return Result<Response>::error("HTTP request failed after " + std::to_string(max_retries_) + " retries");
}

Result<Response> DefaultHttpClient::execute_request(
    Method method,
    const std::string& url,
    const std::string& body,
    const std::map<std::string, std::string>& headers) {
    
    CURL* curl = curl_easy_init();
    if (!curl) {
        return Result<Response>::error("Failed to initialize CURL");
    }

    std::string response_body;
    struct curl_slist* header_list = nullptr;

    // Set custom headers
    for (const auto& [key, value] : headers) {
        std::string header = key + ": " + value;
        header_list = curl_slist_append(header_list, header.c_str());
    }

    // Configure CURL
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 60L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 60L);

    if (header_list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    // Set proxy if configured
    if (!proxy_url_.empty()) {
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy_url_.c_str());
    }

    // Set method-specific options
    if (method == Method::POST) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    }

    // Execute request
    CURLcode res = curl_easy_perform(curl);
    
    // Cleanup
    if (header_list) curl_slist_free_all(header_list);

    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        return Result<Response>::error(std::string("CURL error: ") + curl_easy_strerror(res));
    }

    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    curl_easy_cleanup(curl);

    if (response_code < 200 || response_code >= 300) {
        return Result<Response>::error("HTTP error: " + std::to_string(response_code));
    }

    Response resp;
    resp.status_code = static_cast<int>(response_code);
    resp.body = std::move(response_body);
    
    return Result<Response>(std::move(resp));
}

} // anonymous namespace

std::unique_ptr<HttpClient> create_default_client() {
    return std::make_unique<DefaultHttpClient>();
}

} // namespace alasia::http
