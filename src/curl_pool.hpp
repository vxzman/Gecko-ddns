#pragma once

#include <curl/curl.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace curl_pool {

/// CURL 句柄的 RAII 包装器
class CurlHandle {
public:
    CurlHandle();
    ~CurlHandle();

    // 禁止拷贝
    CurlHandle(const CurlHandle&) = delete;
    CurlHandle& operator=(const CurlHandle&) = delete;

    // 允许移动
    CurlHandle(CurlHandle&& other) noexcept;
    CurlHandle& operator=(CurlHandle&& other) noexcept;

    CURL* get() const { return handle_; }

    // 重置句柄状态，便于复用
    void reset();

    // 检查句柄是否有效
    bool valid() const { return handle_ != nullptr; }

private:
    CURL* handle_;
};

/// 连接池返回的池化句柄 — RAII 归还
class PooledHandle {
public:
    PooledHandle() : handle_() {}
    explicit PooledHandle(CurlHandle handle) : handle_(std::move(handle)) {}
    ~PooledHandle();

    PooledHandle(const PooledHandle&) = delete;
    PooledHandle& operator=(const PooledHandle&) = delete;

    PooledHandle(PooledHandle&& other) noexcept : handle_(std::move(other.handle_)) {}
    PooledHandle& operator=(PooledHandle&& other) noexcept {
        if (this != &other) {
            return_to_pool();
            handle_ = std::move(other.handle_);
        }
        return *this;
    }

    CURL* get() const { return handle_.get(); }
    bool valid() const { return handle_.valid(); }

private:
    CurlHandle handle_;
    void return_to_pool();
};

/// 线程局部的 CURL 连接池
/// 每个线程维护一个 CURL 句柄缓存，避免锁开销
class ConnectionPool {
public:
    static ConnectionPool& instance();

    /// 获取一个 CURL 句柄（从线程局部缓存或新建）
    PooledHandle acquire();

    /// 归还句柄到线程局部缓存
    void release(CurlHandle handle);

    /// 清除当前线程的句柄缓存（在 curl_global_cleanup 前调用）
    static void clear_thread_cache();

    /// 统计信息
    struct Stats {
        size_t total_acquisitions = 0;
        size_t cache_hits = 0;
        size_t cache_misses = 0;
    };

    Stats get_stats() const;

private:
    ConnectionPool() = default;
    ~ConnectionPool();

    mutable std::mutex stats_mutex_;
    Stats stats_;

    void record_hit();
    void record_miss();

    // thread_local 句柄缓存（空 std::optional 表示已借出）
    static thread_local std::optional<CurlHandle> tls_handle_;
};

/// 初始化 CURL 库（程序启动时调用一次）
bool initialize();

/// 清理 CURL 库（程序退出时调用）
void cleanup();

/// 便捷函数：获取线程局部的池化 CURL 句柄
PooledHandle get_handle();

} // namespace curl_pool
