#include "services/dns/dns_factory.hpp"
#include "services/dns/cloudflare_provider.hpp"
#include "services/dns/aliyun_provider.hpp"
#include "config/config.hpp"

namespace alasia::services {

Result<std::unique_ptr<DnsProvider>> DnsProviderFactory::create(
    const std::string& provider_name,
    const config::RecordConfig& record_config) {
    
    if (provider_name == "cloudflare") {
        if (!record_config.cloudflare) {
            return Result<std::unique_ptr<DnsProvider>>::error(
                "Cloudflare provider requires cloudflare configuration");
        }
        
        std::string proxy_url;
        if (record_config.use_proxy) {
            // Proxy would be passed from general config
            // For now, it's handled by the caller
        }
        
        return Result<std::unique_ptr<DnsProvider>>(
            std::make_unique<CloudflareDnsProvider>(
                record_config.cloudflare->api_token,
                proxy_url));
    }
    
    if (provider_name == "aliyun") {
        if (!record_config.aliyun) {
            return Result<std::unique_ptr<DnsProvider>>::error(
                "Aliyun provider requires aliyun configuration");
        }
        
        return Result<std::unique_ptr<DnsProvider>>(
            std::make_unique<AliyunDnsProvider>(
                record_config.aliyun->access_key_id,
                record_config.aliyun->access_key_secret));
    }
    
    return Result<std::unique_ptr<DnsProvider>>::error("Unsupported DNS provider: " + provider_name);
}

} // namespace alasia::services
