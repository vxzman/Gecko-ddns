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

/// 线程局部的 CURL 连接池
/// 每个线程维护一个 CURL 句柄，避免锁开销
class ConnectionPool {
public:
    static ConnectionPool& instance();
    
    /// 获取一个 CURL 句柄（线程局部）
    CurlHandle acquire();
    
    /// 统计信息
    struct Stats {
        size_t total_acquisitions = 0;
        size_t cache_hits = 0;
        size_t cache_misses = 0;
    };
    
    Stats get_stats() const { return stats_; }
    
    /// 更新统计（用于调试）
    void record_hit() { stats_.cache_hits++; stats_.total_acquisitions++; }
    void record_miss() { stats_.cache_misses++; stats_.total_acquisitions++; }

private:
    ConnectionPool() = default;
    ~ConnectionPool();
    
    mutable Stats stats_;
    
    // 每个线程的 CURL 句柄缓存
    struct ThreadLocalCache {
        std::unique_ptr<CurlHandle> handle;
        std::chrono::steady_clock::time_point last_used;
    };
    
    // 使用 thread_local 而非全局锁
    static thread_local std::unique_ptr<CurlHandle> tls_handle_;
};

/// 初始化 CURL 库（程序启动时调用一次）
bool initialize();

/// 清理 CURL 库（程序退出时调用）
void cleanup();

/// 便捷函数：获取线程局部的 CURL 句柄
CURL* get_handle();

} // namespace curl_pool
