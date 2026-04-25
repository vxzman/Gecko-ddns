#pragma once

#include "core/result.hpp"
#include <map>
#include <string>

namespace alasia::services {

/// DNS provider interface - abstracts DNS record management
class DnsProvider {
public:
    virtual ~DnsProvider() = default;
    
    /// Get provider name (e.g., "cloudflare", "aliyun")
    virtual std::string name() const = 0;
    
    /// Create or update a DNS AAAA record
    /// @param zone Domain zone (e.g., "example.com")
    /// @param record_name Subdomain record (e.g., "www", "@")
    /// @param ip IPv6 address to set
    /// @param ttl Time to live in seconds
    /// @param extra Provider-specific extra parameters
    /// @return Success or error message
    virtual Result<void> upsert_record(const std::string& zone,
                                        const std::string& record_name,
                                        const std::string& ip,
                                        int ttl,
                                        const std::map<std::string, std::string>& extra) = 0;
};

} // namespace alasia::services
