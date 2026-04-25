#include "services/dns/cloudflare_provider.hpp"
#include "core/logger.hpp"
#include "core/result.hpp"

#include <nlohmann/json.hpp>
#include <chrono>
#include <sstream>
#include <thread>

using json = nlohmann::json;

namespace alasia::services {

CloudflareDnsProvider::CloudflareDnsProvider(std::string api_token, std::string proxy_url)
    : api_token_(std::move(api_token)),
      proxy_url_(std::move(proxy_url)),
      http_client_(http::create_default_client()) {
    if (!proxy_url_.empty()) {
        http_client_->set_proxy(proxy_url_);
    }
}

auto CloudflareDnsProvider::make_request(const std::string& method,
                                          const std::string& url,
                                          const std::string& body_json) -> Result<HttpResponse> {
    constexpr int MAX_RETRIES = 3;

    for (int attempt = 0; attempt <= MAX_RETRIES; ++attempt) {
        std::map<std::string, std::string> headers;
        headers["Authorization"] = "Bearer " + api_token_;
        headers["Content-Type"]  = "application/json";

        http::Response resp;
        Result<http::Response> result = http::Response{};
        
        if (method == "GET") {
            result = http_client_->get(url, headers);
        } else if (method == "POST") {
            result = http_client_->post(url, body_json, headers);
        } else if (method == "PUT") {
            // Need to implement PUT in http_client if needed
            result = http_client_->post(url, body_json, headers);
        } else {
            return Result<HttpResponse>::error("Unsupported HTTP method: " + method);
        }
        
        if (result.is_error()) {
            if (attempt < MAX_RETRIES) {
                std::this_thread::sleep_for(std::chrono::seconds(1 << attempt));
                continue;
            }
            return Result<HttpResponse>::error(result.error_message());
        }
        
        resp = result.value();
        
        if (resp.status_code >= 500 && attempt < MAX_RETRIES) {
            std::this_thread::sleep_for(std::chrono::seconds(1 << attempt));
            continue;
        }

        return Result<HttpResponse>(HttpResponse{resp.status_code, resp.body});
    }
    
    return Result<HttpResponse>::error("max retries exceeded");
}

Result<std::string> CloudflareDnsProvider::get_zone_id(const std::string& zone_name,
                                                        const std::string& zone_id_hint) {
    if (!zone_id_hint.empty()) {
        return Result<std::string>(zone_id_hint);
    }

    std::string url = "https://api.cloudflare.com/client/v4/zones?name=" + zone_name;
    auto resp = make_request("GET", url);
    if (resp.is_error()) {
        return Result<std::string>::error(resp.error_message());
    }
    
    if (resp.value().code != 200) {
        return Result<std::string>::error("GET zones returned HTTP " + std::to_string(resp.value().code));
    }

    try {
        auto j = json::parse(resp.value().body);
        if (!j.value("success", false) || !j.contains("result") || j["result"].empty()) {
            std::string err = "Zone not found for: " + zone_name;
            if (j.contains("errors") && !j["errors"].empty()) {
                err += ". API error: " + j["errors"][0]["message"].get<std::string>();
            }
            return Result<std::string>::error(err);
        }
        return Result<std::string>(j["result"][0]["id"].get<std::string>());
    } catch (const std::exception& e) {
        return Result<std::string>::error(std::string("JSON parse error: ") + e.what());
    }
}

Result<void> CloudflareDnsProvider::upsert_record(const std::string& zone,
                                                   const std::string& record_name,
                                                   const std::string& ip,
                                                   int ttl,
                                                   const std::map<std::string, std::string>& extra) {
    bool proxied = false;
    auto it = extra.find("proxied");
    if (it != extra.end()) {
        proxied = (it->second == "true" || it->second == "1");
    }

    std::string zone_id;
    auto zit = extra.find("zone_id");
    if (zit != extra.end()) {
        zone_id = zit->second;
    }

    if (zone_id.empty()) {
        auto z_res = get_zone_id(zone, "");
        if (z_res.is_error()) {
            std::string err = "Cloudflare: failed to get zone_id for " + zone + ": " + z_res.error_message();
            logger::error("{}", err);
            return Result<void>::error(err);
        }
        zone_id = z_res.value();
    }

    return upsert_record_with_zone_id(zone, record_name, ip, zone_id, ttl, proxied);
}

Result<void> CloudflareDnsProvider::upsert_record_with_zone_id(const std::string& zone,
                                                                const std::string& record_name,
                                                                const std::string& ip,
                                                                const std::string& zone_id,
                                                                int ttl,
                                                                bool proxied) {
    std::string fqdn = (record_name == "@") ? zone : (record_name + "." + zone);
    std::string search_url = "https://api.cloudflare.com/client/v4/zones/"
                           + zone_id + "/dns_records?type=AAAA&name=" + fqdn;

    auto search_resp = make_request("GET", search_url);
    if (search_resp.is_error()) {
        return Result<void>::error(search_resp.error_message());
    }
    
    if (search_resp.value().code != 200) {
        std::string err = "Cloudflare: search DNS record returned HTTP " + 
                         std::to_string(search_resp.value().code);
        logger::error("{}", err);
        return Result<void>::error(err);
    }

    json new_record = {
        {"type",    "AAAA"},
        {"name",    fqdn},
        {"content", ip},
        {"ttl",     ttl},
        {"proxied", proxied}
    };

    try {
        auto sj = json::parse(search_resp.value().body);
        if (!sj.value("success", false)) {
            std::string msg;
            if (sj.contains("errors") && !sj["errors"].empty()) {
                msg = sj["errors"][0]["message"].get<std::string>();
            }
            std::string err = "Cloudflare: DNS search failed: " + msg;
            logger::error("{}", err);
            return Result<void>::error(err);
        }

        std::string method, endpoint;

        if (!sj["result"].empty()) {
            auto& existing = sj["result"][0];
            std::string existing_ip      = existing.value("content", "");
            bool        existing_proxied = existing.value("proxied", false);
            int         existing_ttl     = existing.value("ttl", 0);

            if (existing_ip == ip && existing_proxied == proxied && existing_ttl == ttl) {
                logger::info("Cloudflare: record {} already up-to-date", fqdn);
                return Result<void>();
            }
            std::string record_id = existing["id"].get<std::string>();
            method   = "PUT";
            endpoint = "https://api.cloudflare.com/client/v4/zones/"
                     + zone_id + "/dns_records/" + record_id;
        } else {
            method   = "POST";
            endpoint = "https://api.cloudflare.com/client/v4/zones/"
                     + zone_id + "/dns_records";
        }

        std::string body = new_record.dump();
        auto put_resp = make_request(method, endpoint, body);
        if (put_resp.is_error()) {
            return Result<void>::error(put_resp.error_message());
        }

        auto rj = json::parse(put_resp.value().body);
        if (!rj.value("success", false)) {
            std::string msg;
            if (rj.contains("errors") && !rj["errors"].empty()) {
                msg = rj["errors"][0]["message"].get<std::string>();
            }
            std::string err = "Cloudflare: " + method + " failed: " + msg;
            logger::error("{}", err);
            return Result<void>::error(err);
        }
        return Result<void>();

    } catch (const std::exception& e) {
        std::string err = std::string("Cloudflare: JSON error: ") + e.what();
        logger::error("{}", err);
        return Result<void>::error(err);
    }
}

} // namespace alasia::services
