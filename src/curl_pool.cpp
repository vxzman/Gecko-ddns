#include "curl_pool.hpp"

#include "log.hpp"

namespace curl_pool {

namespace {

void apply_handle_defaults(CURL* handle) {
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "gecko-ddns/2.0");
    curl_easy_setopt(handle, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(handle, CURLOPT_TCP_KEEPIDLE, 60L);
    curl_easy_setopt(handle, CURLOPT_TCP_KEEPINTVL, 60L);
    curl_easy_setopt(handle, CURLOPT_DNS_CACHE_TIMEOUT, 60L);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(handle, CURLOPT_MAXCONNECTS, 10L);
}

} // namespace

CurlHandle::CurlHandle() : handle_(curl_easy_init()) {
    if (handle_) apply_defaults();
}

void CurlHandle::apply_defaults() {
    if (handle_) apply_handle_defaults(handle_.get());
}

void CurlHandle::reset() {
    if (!handle_) return;
    curl_easy_reset(handle_.get());
    apply_defaults();
}

PooledHandle::~PooledHandle() {
    return_to_pool();
}

void PooledHandle::return_to_pool() {
    if (handle_.valid()) ConnectionPool::instance().release(std::move(handle_));
}

thread_local std::optional<CurlHandle> ConnectionPool::tls_handle_;

ConnectionPool& ConnectionPool::instance() {
    static ConnectionPool pool;
    return pool;
}

PooledHandle ConnectionPool::acquire() {
    if (tls_handle_.has_value() && tls_handle_->valid()) {
        record_hit();
        CurlHandle handle = std::move(*tls_handle_);
        tls_handle_.reset();
        handle.reset();
        return PooledHandle{std::move(handle)};
    }
    record_miss();
    return PooledHandle{CurlHandle{}};
}

void ConnectionPool::release(CurlHandle handle) {
    if (handle.valid()) tls_handle_.emplace(std::move(handle));
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

bool initialize() {
    const CURLcode res = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (res != CURLE_OK) {
        logger::error("Failed to initialize libcurl: {}", curl_easy_strerror(res));
        return false;
    }
    logger::debug("libcurl initialized, version: {}", curl_version());
    return true;
}

void cleanup() {
    ConnectionPool::clear_thread_cache();
    curl_global_cleanup();
    logger::debug("libcurl cleaned up");
}

PooledHandle get_handle() {
    return ConnectionPool::instance().acquire();
}

} // namespace curl_pool
