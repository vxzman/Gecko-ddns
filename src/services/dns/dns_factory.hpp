#pragma once

#include "services/dns/dns_provider.hpp"
#include "config/config.hpp"
#include "core/result.hpp"
#include <memory>
#include <string>

namespace alasia::services {

/// DNS provider factory - creates appropriate provider based on configuration
class DnsProviderFactory {
public:
    /// Create provider based on provider name
    static Result<std::unique_ptr<DnsProvider>> create(
        const std::string& provider_name,
        const config::RecordConfig& record_config);
};

} // namespace alasia::services
