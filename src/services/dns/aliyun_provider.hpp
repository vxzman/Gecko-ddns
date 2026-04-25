#pragma once

#include "services/dns/dns_provider.hpp"
#include "http/http_client.hpp"
#include <memory>
#include <string>

namespace alasia::services {

/// Aliyun DNS provider implementation
class AliyunDnsProvider : public DnsProvider {
public:
    AliyunDnsProvider(std::string access_key_id, std::string access_key_secret);
    
    std::string name() const override { return "aliyun"; }
    
    Result<void> upsert_record(const std::string& zone,
                                const std::string& record_name,
                                const std::string& ip,
                                int ttl,
                                const std::map<std::string, std::string>& extra) override;

private:
    Result<std::string> get_record_id(const std::string& full_domain);
    
    struct HttpResponse {
        long code;
        std::string body;
    };
    
    Result<HttpResponse> sign_and_request(std::map<std::string, std::string> params);
    
    std::string hmac_sha1_base64(const std::string& key, const std::string& data);
    std::string url_encode(const std::string& s);
    std::string generate_signature(const std::map<std::string, std::string>& params);
    
    std::string access_key_id_;
    std::string access_key_secret_;
    std::unique_ptr<http::HttpClient> http_client_;
};

} // namespace alasia::services
