#include "curl_pool.hpp"
#include "log.hpp"

#include <cstdlib>

namespace curl_pool {

// ─── CurlHandle ──────────────────────────────────────────────────────────────

CurlHandle::CurlHandle() : handle_(curl_easy_init()) {
    if (handle_) {
        // 设置默认选项
        curl_easy_setopt(handle_, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(handle_, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(handle_, CURLOPT_TCP_KEEPIDLE, 60L);
        curl_easy_setopt(handle_, CURLOPT_TCP_KEEPINTVL, 60L);
    }
}

CurlHandle::~CurlHandle() {
    if (handle_) {
        curl_easy_cleanup(handle_);
    }
}

CurlHandle::CurlHandle(CurlHandle&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
}

CurlHandle& CurlHandle::operator=(CurlHandle&& other) noexcept {
    if (this != &other) {
        if (handle_) {
            curl_easy_cleanup(handle_);
        }
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

void CurlHandle::reset() {
    if (handle_) {
        curl_easy_reset(handle_);
        // 重新设置默认选项
        curl_easy_setopt(handle_, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(handle_, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(handle_, CURLOPT_TCP_KEEPIDLE, 60L);
        curl_easy_setopt(handle_, CURLOPT_TCP_KEEPINTVL, 60L);
    }
}

// ─── ConnectionPool ──────────────────────────────────────────────────────────

thread_local std::unique_ptr<CurlHandle> ConnectionPool::tls_handle_;

ConnectionPool& ConnectionPool::instance() {
    static ConnectionPool pool;
    return pool;
}

ConnectionPool::~ConnectionPool() {
    // 清理线程局部存储由 C++ 自动处理
}

CurlHandle ConnectionPool::acquire() {
    if (tls_handle_ && tls_handle_->valid()) {
        record_hit();
        tls_handle_->reset();
        return CurlHandle(std::move(*tls_handle_));
    }
    
    record_miss();
    tls_handle_ = std::make_unique<CurlHandle>();
    return CurlHandle(std::move(*tls_handle_));
}

// ─── Global functions ────────────────────────────────────────────────────────

bool initialize() {
    CURLcode res = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (res != CURLE_OK) {
        logger::error("Failed to initialize libcurl: {}", curl_easy_strerror(res));
        return false;
    }
    logger::debug("libcurl initialized, version: {}", curl_version());
    return true;
}

void cleanup() {
    curl_global_cleanup();
    logger::debug("libcurl cleaned up");
}

CURL* get_handle() {
    return ConnectionPool::instance().acquire().get();
}

} // namespace curl_pool
