#include "services/dns/dns_factory.hpp"
#include "services/dns/cloudflare_provider.hpp"
#include "services/dns/aliyun_provider.hpp"
#include "config/config.hpp"

namespace gecko::services {

Result<std::unique_ptr<DnsProvider>> DnsProviderFactory::create(
    const std::string& provider_name,
    const config::RecordConfig& record_config) {

    if (provider_name == "cloudflare") {
        if (record_config.api_token.empty()) {
            return Result<std::unique_ptr<DnsProvider>>::error(
                "Cloudflare provider requires api_token");
        }

        std::string proxy_url;
        if (record_config.use_proxy) {
            // Proxy would be passed from caller
        }

        return Result<std::unique_ptr<DnsProvider>>(
            std::make_unique<CloudflareDnsProvider>(
                record_config.api_token,
                proxy_url));
    }

    if (provider_name == "aliyun") {
        if (record_config.access_key_id.empty() || record_config.access_key_secret.empty()) {
            return Result<std::unique_ptr<DnsProvider>>::error(
                "Aliyun provider requires access_key_id and access_key_secret");
        }

        return Result<std::unique_ptr<DnsProvider>>(
            std::make_unique<AliyunDnsProvider>(
                record_config.access_key_id,
                record_config.access_key_secret));
    }

    return Result<std::unique_ptr<DnsProvider>>::error("Unsupported DNS provider: " + provider_name);
}

} // namespace gecko::services
