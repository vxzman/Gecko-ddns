#include "services/dns/aliyun_provider.hpp"
#include "core/logger.hpp"
#include "core/result.hpp"

#include <nlohmann/json.hpp>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <chrono>
#include <sstream>
#include <iomanip>
#include <thread>

using json = nlohmann::json;

namespace alasia::services {

AliyunDnsProvider::AliyunDnsProvider(std::string access_key_id, std::string access_key_secret)
    : access_key_id_(std::move(access_key_id)),
      access_key_secret_(std::move(access_key_secret)),
      http_client_(http::create_default_client()) {}

std::string AliyunDnsProvider::hmac_sha1_base64(const std::string& key, const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  digest_len = 0;
    HMAC(EVP_sha1(),
         key.c_str(),  (int)key.size(),
         reinterpret_cast<const unsigned char*>(data.c_str()), data.size(),
         digest, &digest_len);

    static const char* b64chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((digest_len + 2) / 3) * 4);
    for (unsigned int i = 0; i < digest_len; i += 3) {
        uint32_t v = (uint32_t)digest[i] << 16;
        if (i + 1 < digest_len) v |= (uint32_t)digest[i + 1] << 8;
        if (i + 2 < digest_len) v |= (uint32_t)digest[i + 2];

        out += b64chars[(v >> 18) & 0x3F];
        out += b64chars[(v >> 12) & 0x3F];
        out += (i + 1 < digest_len) ? b64chars[(v >> 6) & 0x3F] : '=';
        out += (i + 2 < digest_len) ? b64chars[(v)      & 0x3F] : '=';
    }
    return out;
}

std::string AliyunDnsProvider::url_encode(const std::string& s) {
    std::ostringstream out;
    for (char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out << c;
        } else {
            out << '%' << std::uppercase << std::hex
                << std::setw(2) << std::setfill('0') << (int)c;
        }
    }
    return out.str();
}

std::string AliyunDnsProvider::generate_signature(const std::map<std::string, std::string>& params) {
    auto encode_pair = [this](const auto& kv) {
        return url_encode(kv.first) + '=' + url_encode(kv.second);
    };

    std::string canonicalized;
    for (const auto& pair_str : params | std::views::transform(encode_pair)) {
        if (!canonicalized.empty()) canonicalized += '&';
        canonicalized += pair_str;
    }

    std::string string_to_sign = "GET&" + url_encode("/") + "&" + url_encode(canonicalized);
    std::string key = access_key_secret_ + "&";
    return hmac_sha1_base64(key, string_to_sign);
}

auto AliyunDnsProvider::sign_and_request(std::map<std::string, std::string> params) -> Result<HttpResponse> {
    params["AccessKeyId"]      = access_key_id_;
    params["Format"]           = "JSON";
    params["SignatureMethod"]  = "HMAC-SHA1";
    params["SignatureVersion"] = "1.0";
    params["Version"]          = "2015-01-09";

    auto now = std::chrono::system_clock::now();
    auto ns  = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    params["SignatureNonce"] = std::to_string(ns);
    params["Timestamp"]      = std::format("{:%Y-%m-%dT%H:%M:%SZ}",
                                           std::chrono::time_point_cast<std::chrono::seconds>(now));
    params["Signature"]      = generate_signature(params);

    auto build_pair = [this](const auto& kv) {
        return kv.first + '=' + url_encode(kv.second);
    };

    std::vector<std::string> encoded_pairs;
    std::ranges::transform(params, std::back_inserter(encoded_pairs), build_pair);

    std::string qs;
    for (size_t i = 0; i < encoded_pairs.size(); ++i) {
        if (i > 0) qs += '&';
        qs += encoded_pairs[i];
    }
    std::string url = "https://alidns.aliyuncs.com/?" + qs;

    constexpr int MAX_RETRIES = 3;
    for (int attempt = 0; attempt <= MAX_RETRIES; ++attempt) {
        auto result = http_client_->get(url);
        
        if (result.is_error()) {
            if (attempt < MAX_RETRIES) {
                std::this_thread::sleep_for(std::chrono::seconds(1 << attempt));
                continue;
            }
            return Result<HttpResponse>::error(result.error_message());
        }
        
        auto resp = result.value();
        
        if (resp.status_code >= 500 && attempt < MAX_RETRIES) {
            std::this_thread::sleep_for(std::chrono::seconds(1 << attempt));
            continue;
        }
        
        return Result<HttpResponse>(HttpResponse{resp.status_code, resp.body});
    }
    
    return Result<HttpResponse>::error("max retries exceeded");
}

Result<std::string> AliyunDnsProvider::get_record_id(const std::string& full_domain) {
    std::map<std::string, std::string> params{
        {"Action",    "DescribeSubDomainRecords"},
        {"SubDomain", full_domain}
    };

    auto resp = sign_and_request(params);
    if (resp.is_error()) {
        std::string err = "Aliyun: request failed: " + resp.error_message();
        logger::error("{}", err);
        return Result<std::string>::error(err);
    }

    try {
        auto j = json::parse(resp.value().body);
        if (j.contains("Code") && !j["Code"].get<std::string>().empty()) {
            std::string err = "Aliyun API error: " + j["Code"].get<std::string>() + 
                             " - " + j["Message"].get<std::string>();
            logger::error("{}", err);
            return Result<std::string>::error(err);
        }
        if (!j.contains("DomainRecords")) {
            return Result<std::string>::error("");
        }
        
        const auto& records = j["DomainRecords"]["Record"];
        if (records.empty()) {
            return Result<std::string>::error("");
        }

        for (const auto& rec : records) {
            if (rec.value("Type", "") == "AAAA") {
                return Result<std::string>(rec["RecordId"].get<std::string>());
            }
        }
        return Result<std::string>(records[0]["RecordId"].get<std::string>());
    } catch (const std::exception& e) {
        std::string err = std::string("Aliyun: JSON parse error: ") + e.what();
        logger::error("{}", err);
        return Result<std::string>::error(err);
    }
}

Result<void> AliyunDnsProvider::upsert_record(const std::string& zone,
                                               const std::string& record_name,
                                               const std::string& ip,
                                               int ttl,
                                               const std::map<std::string, std::string>& /*extra*/) {
    std::string full_domain = (record_name == "@") ? zone : (record_name + "." + zone);

    auto record_id_res = get_record_id(full_domain);
    if (record_id_res.is_error()) {
        return Result<void>::error(record_id_res.error_message());
    }
    
    std::string record_id = record_id_res.value();

    std::map<std::string, std::string> params;
    if (record_id.empty()) {
        params = {
            {"Action",     "AddDomainRecord"},
            {"DomainName", zone},
            {"RR",         record_name},
            {"Type",       "AAAA"},
            {"Value",      ip}
        };
    } else {
        params = {
            {"Action",   "UpdateDomainRecord"},
            {"RecordId", record_id},
            {"RR",       record_name},
            {"Type",     "AAAA"},
            {"Value",    ip}
        };
    }

    if (ttl > 0) {
        params["TTL"] = std::to_string(ttl);
    }

    auto resp = sign_and_request(params);
    if (resp.is_error()) {
        std::string err = "Aliyun: request failed: " + resp.error_message();
        logger::error("{}", err);
        return Result<void>::error(err);
    }

    try {
        auto j = json::parse(resp.value().body);
        if (j.contains("Code") && !j["Code"].get<std::string>().empty()) {
            std::string err = "Aliyun API error: " + j["Code"].get<std::string>() +
                             " - " + j["Message"].get<std::string>();
            logger::error("{}", err);
            return Result<void>::error(err);
        }
        return Result<void>();
    } catch (const std::exception& e) {
        std::string err = std::string("Aliyun: JSON parse error: ") + e.what();
        logger::error("{}", err);
        return Result<void>::error(err);
    }
}

} // namespace alasia::services
