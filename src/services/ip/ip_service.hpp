#pragma once

#include "core/result.hpp"
#include "core/types.hpp"
#include <memory>
#include <string>
#include <vector>

namespace alasia::services {

/// IP service interface - abstracts IP acquisition from interfaces or APIs
class IpService {
public:
    virtual ~IpService() = default;
    
    /// Get current IPv6 address
    /// @return IPv6 address or error message
    virtual Result<std::string> get_current_ip() = 0;
};

/// Configuration for IP service
struct IpServiceConfig {
    std::string              interface_name;  ///< Network interface name (optional)
    std::vector<std::string> api_urls;        ///< Fallback HTTP API URLs
    
    bool use_interface() const { return !interface_name.empty(); }
    bool has_api_urls() const { return !api_urls.empty(); }
};

/// Create default IP service instance
Result<std::unique_ptr<IpService>> create_ip_service(const IpServiceConfig& config);

} // namespace alasia::services
