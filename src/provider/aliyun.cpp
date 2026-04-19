#include "aliyun.hpp"
#include "../curl_pool.hpp"
#include "../log.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <algorithm>
#include <format>
#include <chrono>
#include <format>
#include <iomanip>
#include <map>
#include <ranges>
#include <sstream>
#include <thread>

using json = nlohmann::json;

namespace provider {

namespace {

static size_t write_cb(char* ptr, size_t sz, size_t nmemb, void* ud) {
    reinterpret_cast<std::string*>(ud)->append(ptr, sz * nmemb);
    return sz * nmemb;
}

} // anonymous namespace

AliyunProvider::AliyunProvider(std::string access_key_id, std::string access_key_secret)
    : access_key_id_(std::move(access_key_id)),
      access_key_secret_(std::move(access_key_secret)) {}

// ─── HMAC-SHA1 Base64 ─────────────────────────────────────────────────────────

std::string AliyunProvider::hmac_sha1_base64(const std::string& key, const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  digest_len = 0;
    HMAC(EVP_sha1(),
         key.c_str(),  (int)key.size(),
         reinterpret_cast<const unsigned char*>(data.c_str()), data.size(),
         digest, &digest_len);

    // Base64 encode
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

// ─── URL encode (RFC 3986 strict) ─────────────────────────────────────────────

std::string AliyunProvider::url_encode(const std::string& s) {
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

// ─── Signature generation ─────────────────────────────────────────────────────

std::string AliyunProvider::generate_signature(const std::map<std::string, std::string>& params) {
    // Sorted params (std::map is already sorted)
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

// ─── HTTP request (signed) ────────────────────────────────────────────────────

auto AliyunProvider::sign_and_request(
        std::map<std::string, std::string> params) -> std::expected<HttpResponse, std::string> {

    // Common parameters
    params["AccessKeyId"]      = access_key_id_;
    params["Format"]           = "JSON";
    params["SignatureMethod"]  = "HMAC-SHA1";
    params["SignatureVersion"] = "1.0";
    params["Version"]          = "2015-01-09";

    // Nonce: nanoseconds
    auto now = std::chrono::system_clock::now();
    auto ns  = std::chrono::duration_cast<std::chrono::nanoseconds>(
                   now.time_since_epoch()).count();
    params["SignatureNonce"] = std::to_string(ns);

    // Timestamp: UTC ISO 8601
    params["Timestamp"] = std::format("{:%Y-%m-%dT%H:%M:%SZ}",
                                    std::chrono::time_point_cast<std::chrono::seconds>(now));

    // Sign
    params["Signature"] = generate_signature(params);

    // Build URL using ranges to join parameters
    auto build_pair = [this](const auto& kv) {
        return kv.first + '=' + url_encode(kv.second);
    };

    // Collect transformed pairs and join with '&'
    std::vector<std::string> encoded_pairs;
    std::ranges::transform(params, std::back_inserter(encoded_pairs), build_pair);
    
    // Join with '&' delimiter
    std::string qs;
    for (size_t i = 0; i < encoded_pairs.size(); ++i) {
        if (i > 0) qs += '&';
        qs += encoded_pairs[i];
    }
    std::string url = "https://alidns.aliyuncs.com/?" + qs;

    constexpr int MAX_RETRIES = 3;
    for (int attempt = 0; attempt <= MAX_RETRIES; ++attempt) {
        // Acquire CURL handle from connection pool
        auto curl_handle = curl_pool::ConnectionPool::instance().acquire();
        CURL* curl = curl_handle.get();
        if (!curl) return std::unexpected("curl_easy_init failed");

        std::string body;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);

        CURLcode res = curl_easy_perform(curl);
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        // Handle is automatically returned to pool via RAII

        if (res != CURLE_OK) {
            if (attempt < MAX_RETRIES) {
                std::this_thread::sleep_for(std::chrono::seconds(1 << attempt));
                continue;
            }
            return std::unexpected(curl_easy_strerror(res));
        }
        if (code >= 500 && attempt < MAX_RETRIES) {
            std::this_thread::sleep_for(std::chrono::seconds(1 << attempt));
            continue;
        }
        return HttpResponse{code, body};
    }
    return std::unexpected("max retries exceeded");
}

// ─── Get record ID ────────────────────────────────────────────────────────────

std::expected<std::string, std::string> AliyunProvider::get_record_id(const std::string& full_domain) {
    std::map<std::string, std::string> params{
        {"Action",    "DescribeSubDomainRecords"},
        {"SubDomain", full_domain}
    };

    auto resp = sign_and_request(params);
    if (!resp) {
        std::string err = "Aliyun: request failed: " + resp.error();
        logger::error("{}", err);
        return std::unexpected(err);
    }

    try {
        auto j = json::parse(resp->body);
        if (j.contains("Code") && j["Code"].is_string() && !j["Code"].get<std::string>().empty()) {
            std::string err = "Aliyun API error: " + j.value("Code", "") + " - " + j.value("Message", "");
            logger::error("{}", err);
            return std::unexpected(err);
        }
        if (!j.contains("DomainRecords")) return "";
        const auto& records = j["DomainRecords"]["Record"];
        if (records.empty()) return "";

        // Prefer AAAA record
        for (const auto& rec : records) {
            if (rec.value("Type", "") == "AAAA")
                return rec["RecordId"].get<std::string>();
        }
        return records[0]["RecordId"].get<std::string>();
    } catch (const std::exception& e) {
        std::string err = std::string("Aliyun: JSON parse error: ") + e.what();
        logger::error("{}", err);
        return std::unexpected(err);
    }
}

// ─── Upsert record ────────────────────────────────────────────────────────────

std::expected<void, std::string> AliyunProvider::upsert_record(const std::string& zone,
                                    const std::string& record_name,
                                    const std::string& ip,
                                    int                ttl,
                                    const std::map<std::string, std::string>& /*extra*/) {
    std::string full_domain = (record_name == "@") ? zone : (record_name + "." + zone);

    auto record_id_res = get_record_id(full_domain);
    if (!record_id_res) return std::unexpected(record_id_res.error());
    std::string record_id = *record_id_res;

    std::map<std::string, std::string> params;
    if (record_id.empty()) {
        // Create
        params = {
            {"Action",     "AddDomainRecord"},
            {"DomainName", zone},
            {"RR",         record_name},
            {"Type",       "AAAA"},
            {"Value",      ip}
        };
    } else {
        // Update
        params = {
            {"Action",   "UpdateDomainRecord"},
            {"RecordId", record_id},
            {"RR",       record_name},
            {"Type",     "AAAA"},
            {"Value",    ip}
        };
    }

    if (ttl > 0) params["TTL"] = std::to_string(ttl);

    auto resp = sign_and_request(params);
    if (!resp) {
        std::string err = "Aliyun: request failed: " + resp.error();
        logger::error("{}", err);
        return std::unexpected(err);
    }

    try {
        auto j = json::parse(resp->body);
        if (j.contains("Code") && j["Code"].is_string() && !j["Code"].get<std::string>().empty()) {
            std::string err = "Aliyun API error: " + j.value("Code", "") + " - " + j.value("Message", "");
            logger::error("{}", err);
            return std::unexpected(err);
        }
        return {};
    } catch (const std::exception& e) {
        std::string err = std::string("Aliyun: JSON parse error: ") + e.what();
        logger::error("{}", err);
        return std::unexpected(err);
    }
}

} // namespace provider
