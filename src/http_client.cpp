#include "http_client.hpp"
#include "log.hpp"

#include <chrono>
#include <thread>

// Write callback for CURL response body
static size_t curl_write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    static_cast<std::string*>(userp)->append(static_cast<char*>(contents), realsize);
    return realsize;
}

std::expected<HttpResponse, std::string> HttpClient::request(
    const std::string& url,
    const std::string& method,
    const std::string& post_body,
    const std::map<std::string, std::string>& headers,
    int timeout,
    int max_retries) {
    
    CURL* curl = curl_easy_init();
    if (!curl) {
        return std::unexpected("Failed to initialize CURL");
    }
    
    std::string response_body;
    struct curl_slist* header_list = nullptr;
    
    // Set headers
    for (const auto& [key, value] : headers) {
        std::string header = key + ": " + value;
        header_list = curl_slist_append(header_list, header.c_str());
    }
    
    // Configure CURL
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    
    if (header_list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }
    
    // Set proxy if configured
    if (!proxy_url_.empty()) {
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy_url_.c_str());
    }
    
    // Set method-specific options
    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_body.c_str());
    }
    
    // Execute request with retry
    for (int attempt = 0; attempt <= max_retries; ++attempt) {
        CURLcode res = curl_easy_perform(curl);
        
        if (res == CURLE_OK) {
            long response_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
            
            if (response_code >= 200 && response_code < 300) {
                HttpResponse resp;
                resp.status_code = static_cast<int>(response_code);
                resp.body = response_body;
                curl_easy_cleanup(curl);
                if (header_list) curl_slist_free_all(header_list);
                return resp;
            }
            
            if (attempt < max_retries) {
                int delay = 1 << attempt;
                logger::debug("HTTP {} {} returned {}, retrying in {}s...", 
                             method, url, response_code, delay);
                std::this_thread::sleep_for(std::chrono::seconds(delay));
                continue;
            }
            
            curl_easy_cleanup(curl);
            if (header_list) curl_slist_free_all(header_list);
            return std::unexpected("HTTP " + method + " failed with status " + std::to_string(response_code));
        }
        
        if (attempt < max_retries) {
            int delay = 1 << attempt;
            logger::debug("HTTP {} {} failed: {}, retrying in {}s...", 
                         method, url, curl_easy_strerror(res), delay);
            std::this_thread::sleep_for(std::chrono::seconds(delay));
        } else {
            curl_easy_cleanup(curl);
            if (header_list) curl_slist_free_all(header_list);
            return std::unexpected("HTTP " + method + " failed after " + 
                                  std::to_string(max_retries) + " retries: " + 
                                  curl_easy_strerror(res));
        }
    }
    
    curl_easy_cleanup(curl);
    if (header_list) curl_slist_free_all(header_list);
    return std::unexpected("HTTP " + method + " failed: unknown error");
}

std::expected<HttpResponse, std::string> HttpClient::get(
    const std::string& url,
    const std::map<std::string, std::string>& headers,
    int timeout,
    int max_retries) {
    return request(url, "GET", "", headers, timeout, max_retries);
}

std::expected<HttpResponse, std::string> HttpClient::post(
    const std::string& url,
    const std::string& body,
    const std::map<std::string, std::string>& headers,
    int timeout,
    int max_retries) {
    return request(url, "POST", body, headers, timeout, max_retries);
}

void HttpClient::set_proxy(const std::string& proxy_url) {
    std::lock_guard<std::mutex> lock(proxy_mutex_);
    proxy_url_ = proxy_url;
}

void HttpClient::clear_proxy() {
    std::lock_guard<std::mutex> lock(proxy_mutex_);
    proxy_url_.clear();
}
