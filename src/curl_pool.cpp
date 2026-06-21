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

        // DNS 缓存优化：缓存 60 秒
        curl_easy_setopt(handle_, CURLOPT_DNS_CACHE_TIMEOUT, 60L);

        // 连接超时（秒），防止连接卡住
        curl_easy_setopt(handle_, CURLOPT_CONNECTTIMEOUT, 10L);

        // 连接复用优化
        curl_easy_setopt(handle_, CURLOPT_MAXCONNECTS, 10L);
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

        // DNS 缓存优化：缓存 60 秒
        curl_easy_setopt(handle_, CURLOPT_DNS_CACHE_TIMEOUT, 60L);

        // 连接超时（秒），防止连接卡住
        curl_easy_setopt(handle_, CURLOPT_CONNECTTIMEOUT, 10L);

        // 连接复用优化
        curl_easy_setopt(handle_, CURLOPT_MAXCONNECTS, 10L);
    }
}

// ─── PooledHandle ────────────────────────────────────────────────────────────

PooledHandle::~PooledHandle() {
    return_to_pool();
}

void PooledHandle::return_to_pool() {
    if (handle_.valid()) {
        ConnectionPool::instance().release(std::move(handle_));
    }
}

// ─── ConnectionPool ──────────────────────────────────────────────────────────

thread_local std::optional<CurlHandle> ConnectionPool::tls_handle_;

ConnectionPool& ConnectionPool::instance() {
    static ConnectionPool pool;
    return pool;
}

ConnectionPool::~ConnectionPool() {
    // 线程局部存储在程序退出时由 C++ 自动清理
}

PooledHandle ConnectionPool::acquire() {
    if (tls_handle_.has_value() && tls_handle_->valid()) {
        record_hit();
        CurlHandle h = std::move(*tls_handle_);
        tls_handle_.reset();  // 标记为已借出
        h.reset();            // 重置状态以便复用
        return PooledHandle(std::move(h));
    }

    record_miss();
    return PooledHandle(CurlHandle{});
}

void ConnectionPool::release(CurlHandle handle) {
    if (handle.valid()) {
        tls_handle_.emplace(std::move(handle));
    }
}

ConnectionPool::Stats ConnectionPool::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void ConnectionPool::record_hit() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.cache_hits++;
    stats_.total_acquisitions++;
}

void ConnectionPool::clear_thread_cache() {
    tls_handle_.reset();
}

void ConnectionPool::record_miss() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.cache_misses++;
    stats_.total_acquisitions++;
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
    // 清理当前线程的句柄缓存（必须在 curl_global_cleanup 之前）
    ConnectionPool::clear_thread_cache();
    curl_global_cleanup();
    logger::debug("libcurl cleaned up");
}

PooledHandle get_handle() {
    return ConnectionPool::instance().acquire();
}

} // namespace curl_pool
