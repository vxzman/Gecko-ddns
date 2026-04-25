#pragma once

#include "services/dns/dns_provider.hpp"
#include "http/http_client.hpp"
#include <memory>
#include <string>

namespace alasia::services {

/// Cloudflare DNS provider implementation
class CloudflareDnsProvider : public DnsProvider {
public:
    CloudflareDnsProvider(std::string api_token, std::string proxy_url = "");
    
    std::string name() const override { return "cloudflare"; }
    
    Result<void> upsert_record(const std::string& zone,
                                const std::string& record_name,
                                const std::string& ip,
                                int ttl,
                                const std::map<std::string, std::string>& extra) override;
    
    /// Get zone ID (with optional hint to skip API call)
    Result<std::string> get_zone_id(const std::string& zone_name,
                                     const std::string& zone_id_hint = "");
    
    /// Upsert record with pre-resolved zone ID
    Result<void> upsert_record_with_zone_id(const std::string& zone,
                                             const std::string& record_name,
                                             const std::string& ip,
                                             const std::string& zone_id,
                                             int ttl,
                                             bool proxied);

private:
    struct HttpResponse {
        long code;
        std::string body;
    };
    
    Result<HttpResponse> make_request(const std::string& method,
                                       const std::string& url,
                                       const std::string& body_json = "");
    
    std::string api_token_;
    std::string proxy_url_;
    std::unique_ptr<http::HttpClient> http_client_;
};

} // namespace alasia::services
