#pragma once
#include "provider.hpp"

#include <expected>
#include <map>
#include <string>
#include <string_view>

namespace provider {

class AliyunProvider : public DNSProvider {
public:
    AliyunProvider(std::string access_key_id, std::string access_key_secret, std::string proxy_url = "");

    std::string name() const override { return "aliyun"; }

    [[nodiscard]] std::expected<void, std::string> upsert_record(std::string_view zone,
                                                                 std::string_view record_name,
                                                                 std::string_view ip,
                                                                 int ttl,
                                                                 const ProviderOpts& opts) override;

private:
    std::string access_key_id_;
    std::string access_key_secret_;
    std::string proxy_url_;

    [[nodiscard]] std::expected<std::string, std::string> get_record_id(std::string_view full_domain);
    [[nodiscard]] std::expected<void, std::string> upsert_with_type(std::string_view zone,
                                                                    std::string_view record_name,
                                                                    std::string_view ip,
                                                                    int ttl,
                                                                    std::string_view type);

    struct HttpResponse { long code = 0; std::string body; };
    [[nodiscard]] std::expected<HttpResponse, std::string> sign_and_request(std::map<std::string, std::string> params);

    std::string hmac_sha1_base64(const std::string& key, const std::string& data);
    std::string url_encode(const std::string& s);
    std::string generate_signature(const std::map<std::string, std::string>& params);
};

} // namespace provider
