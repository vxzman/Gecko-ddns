#include "cloudflare.hpp"

#include "../curl_pool.hpp"
#include "../curl_raii.hpp"
#include "../log.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <format>
#include <string>
#include <thread>

using json = nlohmann::json;

namespace provider {

namespace {

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* ud) {
    reinterpret_cast<std::string*>(ud)->append(ptr, size * nmemb);
    return size * nmemb;
}

} // namespace

CloudflareProvider::CloudflareProvider(std::string api_token, std::string proxy_url)
    : api_token_(std::move(api_token)), proxy_url_(std::move(proxy_url)) {}

std::expected<CloudflareProvider::HttpResponse, std::string>
CloudflareProvider::cf_request(std::string_view method,
                               std::string_view url,
                               std::string_view body_json) {
    constexpr int kMaxRetries = 3;
    const std::string method_owned{method};
    const std::string url_owned{url};
    const std::string body_owned{body_json};

    for (int attempt = 0; attempt <= kMaxRetries; ++attempt) {
        auto curl_handle = curl_pool::ConnectionPool::instance().acquire();
        CURL* curl = curl_handle.get();
        if (curl == nullptr) return std::unexpected("curl_easy_init failed");

        std::string response_body;
        curl_easy_setopt(curl, CURLOPT_URL, url_owned.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);

        SlistPtr headers;
        if (!append_slist(headers, std::format("Authorization: Bearer {}", api_token_)) ||
            !append_slist(headers, "Content-Type: application/json")) {
            return std::unexpected("failed to append header");
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers.get());

        if (method_owned == "POST") {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_owned.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_owned.size()));
        } else if (method_owned == "PUT") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_owned.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_owned.size()));
        } else if (method_owned == "DELETE") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
        }

        if (!proxy_url_.empty()) curl_easy_setopt(curl, CURLOPT_PROXY, proxy_url_.c_str());

        const CURLcode res = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (res != CURLE_OK) {
            if (attempt < kMaxRetries) {
                std::this_thread::sleep_for(std::chrono::seconds(1 << attempt));
                continue;
            }
            return std::unexpected(std::format("curl error: {}", curl_easy_strerror(res)));
        }
        if (http_code >= 500 && attempt < kMaxRetries) {
            std::this_thread::sleep_for(std::chrono::seconds(1 << attempt));
            continue;
        }
        return HttpResponse{.code = http_code, .body = std::move(response_body)};
    }
    return std::unexpected("max retries exceeded");
}

std::expected<std::string, std::string>
CloudflareProvider::get_zone_id(std::string_view zone_name, std::string_view zone_id_hint) {
    if (!zone_id_hint.empty()) return std::string{zone_id_hint};

    const std::string url = std::format("https://api.cloudflare.com/client/v4/zones?name={}", zone_name);
    return cf_request("GET", url).and_then([&](const HttpResponse& resp) -> std::expected<std::string, std::string> {
        if (resp.code != 200) {
            return std::unexpected(std::format("GET zones returned HTTP {}", resp.code));
        }
        try {
            const auto parsed = json::parse(resp.body);
            if (!parsed.value("success", false) || !parsed.contains("result") || parsed["result"].empty()) {
                std::string err = std::format("Zone not found for: {}", zone_name);
                if (parsed.contains("errors") && !parsed["errors"].empty()) {
                    err = std::format("{}. API error: {}", err, parsed["errors"][0].value("message", ""));
                }
                return std::unexpected(std::move(err));
            }
            return parsed["result"][0].at("id").get<std::string>();
        } catch (const json::exception& ex) {
            return std::unexpected(std::format("JSON parse error: {}", ex.what()));
        }
    });
}

std::expected<void, std::string>
CloudflareProvider::upsert_record(std::string_view zone,
                                  std::string_view record_name,
                                  std::string_view ip,
                                  int ttl,
                                  const ProviderOpts& opts) {
    return std::visit(overloaded{
        [&](const CloudflareOpts& opt) -> std::expected<void, std::string> {
            const std::string type = opt.type.empty() ? std::string{"AAAA"} : opt.type;
            if (!opt.zone_id.empty()) {
                return upsert_record_with_zone_id(zone, record_name, ip, opt.zone_id, ttl, opt.proxied, type);
            }
            return get_zone_id(zone, {}).transform_error([&](const std::string& err) {
                const std::string wrapped = std::format("Cloudflare: failed to get zone_id for {}: {}", zone, err);
                logger::error("{}", wrapped);
                return wrapped;
            }).and_then([&](const std::string& zone_id) {
                return upsert_record_with_zone_id(zone, record_name, ip, zone_id, ttl, opt.proxied, type);
            });
        },
        [](const AliyunOpts&) -> std::expected<void, std::string> {
            return std::unexpected("Cloudflare provider received Aliyun options");
        },
    }, opts);
}

std::expected<void, std::string>
CloudflareProvider::upsert_record_with_zone_id(std::string_view zone,
                                               std::string_view record_name,
                                               std::string_view ip,
                                               std::string_view zone_id,
                                               int ttl,
                                               bool proxied,
                                               std::string_view type) {
    const std::string fqdn = record_name == "@" ? std::string{zone} : std::format("{}.{}", record_name, zone);
    const std::string search_url = std::format(
        "https://api.cloudflare.com/client/v4/zones/{}/dns_records?type={}&name={}",
        zone_id, type, fqdn);

    return cf_request("GET", search_url).and_then([&](const HttpResponse& search_resp) -> std::expected<void, std::string> {
        if (search_resp.code != 200) {
            const std::string err = std::format("Cloudflare: search DNS record returned HTTP {}", search_resp.code);
            logger::error("{}", err);
            return std::unexpected(err);
        }

        const json new_record = {
            {"type", std::string{type}},
            {"name", fqdn},
            {"content", std::string{ip}},
            {"ttl", ttl},
            {"proxied", proxied},
        };

        try {
            const auto found = json::parse(search_resp.body);
            if (!found.value("success", false)) {
                std::string message = "unknown";
                if (found.contains("errors") && !found["errors"].empty()) {
                    message = found["errors"][0].value("message", "unknown");
                }
                const std::string err = std::format("Cloudflare: DNS search failed: {}", message);
                logger::error("{}", err);
                return std::unexpected(err);
            }

            std::string method;
            std::string endpoint;
            if (!found["result"].empty()) {
                const auto& existing = found["result"][0];
                const std::string existing_ip = existing.value("content", "");
                const bool existing_proxied = existing.value("proxied", false);
                const int existing_ttl = existing.value("ttl", 0);
                if (existing_ip == ip && existing_proxied == proxied && existing_ttl == ttl) {
                    logger::info("Cloudflare: record {} already up-to-date", fqdn);
                    return {};
                }
                const std::string record_id = existing.at("id").get<std::string>();
                method = "PUT";
                endpoint = std::format("https://api.cloudflare.com/client/v4/zones/{}/dns_records/{}",
                                       zone_id, record_id);
            } else {
                method = "POST";
                endpoint = std::format("https://api.cloudflare.com/client/v4/zones/{}/dns_records", zone_id);
            }

            return cf_request(method, endpoint, new_record.dump()).and_then(
                [&](const HttpResponse& put_resp) -> std::expected<void, std::string> {
                    try {
                        const auto written = json::parse(put_resp.body);
                        if (!written.value("success", false)) {
                            std::string message = "unknown";
                            if (written.contains("errors") && !written["errors"].empty()) {
                                message = written["errors"][0].value("message", "unknown");
                            }
                            const std::string err = std::format("Cloudflare: {} failed: {}", method, message);
                            logger::error("{}", err);
                            return std::unexpected(err);
                        }
                        return {};
                    } catch (const json::exception& ex) {
                        const std::string err = std::format("Cloudflare: JSON error: {}", ex.what());
                        logger::error("{}", err);
                        return std::unexpected(err);
                    }
                });
        } catch (const json::exception& ex) {
            const std::string err = std::format("Cloudflare: JSON error: {}", ex.what());
            logger::error("{}", err);
            return std::unexpected(err);
        }
    });
}

} // namespace provider
