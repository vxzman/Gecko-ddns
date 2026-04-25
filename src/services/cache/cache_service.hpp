#pragma once

#include "core/result.hpp"
#include <map>
#include <string>

namespace alasia::cache {

/// Cache service interface
class CacheService {
public:
    virtual ~CacheService() = default;
    
    /// Read cached value
    /// @return Cached value or empty string if not found
    virtual std::string read(const std::string& key) = 0;
    
    /// Write value to cache
    /// @return true on success
    virtual bool write(const std::string& key, const std::string& value) = 0;
    
    /// Clear cache
    virtual void clear() = 0;
};

/// File-based cache service implementation
class FileCacheService : public CacheService {
public:
    /// @param base_dir Base directory for cache files
    explicit FileCacheService(std::string base_dir);
    
    std::string read(const std::string& key) override;
    bool write(const std::string& key, const std::string& value) override;
    void clear() override;

private:
    std::string get_cache_path(const std::string& key) const;
    
    std::string base_dir_;
};

/// Read last IP from cache file (legacy API)
std::string read_last_ip(const std::string& path);

/// Write IP to cache file (legacy API)
bool write_last_ip(const std::string& path, const std::string& ip);

/// Read ZoneID cache (map of zone -> zone_id)
std::map<std::string, std::string> read_zone_id_cache(const std::string& path);

/// Update ZoneID cache for a specific zone
bool update_zone_id_cache(const std::string& path, const std::string& zone, const std::string& zone_id);

/// Get cache file path
std::string get_cache_file_path(const std::string& config_abs_path, const std::string& work_dir);

/// Get ZoneID cache file path
std::string get_zone_id_cache_path(const std::string& config_abs_path);

} // namespace alasia::cache
