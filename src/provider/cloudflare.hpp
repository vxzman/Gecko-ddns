#pragma once

#include "provider.hpp"

#include <expected>
#include <string>
#include <string_view>

namespace provider {

class CloudflareProvider : public DNSProvider {
public:
    CloudflareProvider(std::string api_token, std::string proxy_url = "");

    std::string name() const override { return "cloudflare"; }

    /// Returns zone_id. A non-empty hint is returned as-is.
    [[nodiscard]] std::expected<std::string, std::string> get_zone_id(std::string_view zone_name,
                                                                      std::string_view zone_id_hint);

    [[nodiscard]] std::expected<void, std::string> upsert_record(std::string_view zone,
                                                                 std::string_view record_name,
                                                                 std::string_view ip,
                                                                 int ttl,
                                                                 const ProviderOpts& opts) override;

    /// Upsert with a zone id that was already resolved or loaded from cache.
    [[nodiscard]] std::expected<void, std::string> upsert_record_with_zone_id(std::string_view zone,
                                                                              std::string_view record_name,
                                                                              std::string_view ip,
                                                                              std::string_view zone_id,
                                                                              int ttl,
                                                                              bool proxied,
                                                                              std::string_view type = "AAAA");

private:
    std::string api_token_;
    std::string proxy_url_;

    struct HttpResponse {
        long code = 0;
        std::string body;
    };

    [[nodiscard]] std::expected<HttpResponse, std::string> cf_request(std::string_view method,
                                                                      std::string_view url,
                                                                      std::string_view body_json = {});
};

} // namespace provider
