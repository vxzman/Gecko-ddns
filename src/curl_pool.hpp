#pragma once

#include "curl_raii.hpp"

#include <mutex>
#include <optional>
#include <utility>

namespace curl_pool {

/// One easy handle. Cleanup lives in CurlDeleter.
class CurlHandle {
public:
    struct Empty {};

    CurlHandle();
    explicit CurlHandle(Empty) noexcept {}

    CurlHandle(const CurlHandle&) = delete;
    CurlHandle& operator=(const CurlHandle&) = delete;
    CurlHandle(CurlHandle&&) noexcept = default;
    CurlHandle& operator=(CurlHandle&&) noexcept = default;

    [[nodiscard]] CURL* get() const { return handle_.get(); }
    void reset();
    [[nodiscard]] bool valid() const { return static_cast<bool>(handle_); }

private:
    CurlPtr handle_{};
    void apply_defaults();
};

/// Pool handle that returns itself to the thread-local cache.
class PooledHandle {
public:
    PooledHandle() : handle_(CurlHandle{CurlHandle::Empty{}}) {}
    explicit PooledHandle(CurlHandle handle) : handle_(std::move(handle)) {}
    ~PooledHandle();

    PooledHandle(const PooledHandle&) = delete;
    PooledHandle& operator=(const PooledHandle&) = delete;
    PooledHandle(PooledHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, CurlHandle{CurlHandle::Empty{}})) {}
    PooledHandle& operator=(PooledHandle&& other) noexcept {
        if (this != &other) {
            return_to_pool();
            handle_ = std::exchange(other.handle_, CurlHandle{CurlHandle::Empty{}});
        }
        return *this;
    }

    [[nodiscard]] CURL* get() const { return handle_.get(); }
    [[nodiscard]] bool valid() const { return handle_.valid(); }

private:
    CurlHandle handle_{};
    void return_to_pool();
};

class ConnectionPool {
public:
    static ConnectionPool& instance();

    PooledHandle acquire();
    void release(CurlHandle handle);
    static void clear_thread_cache();

    struct Stats {
        size_t total_acquisitions = 0;
        size_t cache_hits = 0;
        size_t cache_misses = 0;
    };

    [[nodiscard]] Stats get_stats() const;

private:
    ConnectionPool() = default;

    mutable std::mutex stats_mutex_;
    Stats stats_{};

    void record_hit();
    void record_miss();

    static thread_local std::optional<CurlHandle> tls_handle_;
};

[[nodiscard]] bool initialize();
void cleanup();
PooledHandle get_handle();

} // namespace curl_pool
