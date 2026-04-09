#pragma once
#include "provider.hpp"
#include <expected>
#include <string>

namespace provider {

class AliyunProvider : public DNSProvider {
public:
    AliyunProvider(std::string access_key_id, std::string access_key_secret);

    std::string name() const override { return "aliyun"; }

    std::expected<void, std::string> upsert_record(const std::string& zone,
                                                   const std::string& record_name,
                                                   const std::string& ip,
                                                   int                ttl,
                                                   const std::map<std::string, std::string>& extra) override;

private:
    std::string access_key_id_;
    std::string access_key_secret_;

    std::expected<std::string, std::string> get_record_id(const std::string& full_domain);

    struct HttpResponse { long code; std::string body; };
    std::expected<HttpResponse, std::string> sign_and_request(std::map<std::string, std::string> params);

    std::string hmac_sha1_base64(const std::string& key, const std::string& data);
    std::string url_encode(const std::string& s);
    std::string generate_signature(const std::map<std::string, std::string>& params);
};

} // namespace provider
