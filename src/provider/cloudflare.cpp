#include "cloudflare.hpp"
#include "../curl_pool.hpp"
#include "../log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <string>
#include <thread>

using json = nlohmann::json;

namespace provider {

namespace {

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* ud) {
    reinterpret_cast<std::string*>(ud)->append(ptr, size * nmemb);
    return size * nmemb;
}

} // anonymous namespace

CloudflareProvider::CloudflareProvider(std::string api_token, std::string proxy_url)
    : api_token_(std::move(api_token)), proxy_url_(std::move(proxy_url)) {}

auto CloudflareProvider::cf_request(const std::string& method,
                                const std::string& url,
                                const std::string& body_json) -> std::expected<HttpResponse, std::string> {
    constexpr int MAX_RETRIES = 3;

    for (int attempt = 0; attempt <= MAX_RETRIES; ++attempt) {
        // Acquire CURL handle from connection pool
        auto curl_handle = curl_pool::ConnectionPool::instance().acquire();
        CURL* curl = curl_handle.get();
        if (!curl) return std::unexpected("curl_easy_init failed");

        std::string response_body;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);

        // Auth & content-type headers
        struct curl_slist* headers = nullptr;
        std::string auth_hdr = "Authorization: Bearer " + api_token_;
        headers = curl_slist_append(headers, auth_hdr.c_str());
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        // Method / body
        if (method == "POST") {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_json.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_json.size());
        } else if (method == "PUT") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_json.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_json.size());
        } else if (method == "DELETE") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
        }
        // GET is the default

        // Proxy
        if (!proxy_url_.empty()) {
            curl_easy_setopt(curl, CURLOPT_PROXY, proxy_url_.c_str());
        }

        CURLcode res = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        curl_slist_free_all(headers);
        // Handle is automatically returned to pool via RAII

        if (res != CURLE_OK) {
            if (attempt < MAX_RETRIES) {
                std::this_thread::sleep_for(std::chrono::seconds(1 << attempt));
                continue;
            }
            return std::unexpected(std::string("curl error: ") + curl_easy_strerror(res));
        }

        if (http_code >= 500 && attempt < MAX_RETRIES) {
            std::this_thread::sleep_for(std::chrono::seconds(1 << attempt));
            continue;
        }

        return HttpResponse{http_code, response_body};
    }
    return std::unexpected("max retries exceeded");
}

// ─── Get Zone ID ──────────────────────────────────────────────────────────────

std::expected<std::string, std::string> CloudflareProvider::get_zone_id(const std::string& zone_name,
                                             const std::string& zone_id_hint) {
    if (!zone_id_hint.empty()) return zone_id_hint;

    std::string url = "https://api.cloudflare.com/client/v4/zones?name=" + zone_name;
    auto resp = cf_request("GET", url);
    if (!resp) return std::unexpected(resp.error());
    if (resp->code != 200) {
        return std::unexpected("GET zones returned HTTP " + std::to_string(resp->code));
    }

    try {
        auto j = json::parse(resp->body);
        if (!j.value("success", false) || !j.contains("result") || j["result"].empty()) {
            std::string err = "Zone not found for: " + zone_name;
            if (j.contains("errors") && !j["errors"].empty())
                err += ". API error: " + j["errors"][0].value("message", "");
            return std::unexpected(err);
        }
        return j["result"][0]["id"].get<std::string>();
    } catch (const std::exception& e) {
        return std::unexpected(std::string("JSON parse error: ") + e.what());
    }
}

// ─── Upsert record ────────────────────────────────────────────────────────────

std::expected<void, std::string> CloudflareProvider::upsert_record(const std::string& zone,
                                         const std::string& record_name,
                                         const std::string& ip,
                                         int                ttl,
                                         const std::map<std::string, std::string>& extra) {
    bool proxied = false;
    auto it = extra.find("proxied");
    if (it != extra.end()) proxied = (it->second == "true" || it->second == "1");

    // We need zone_id; resolve it now (callers from main pass it via extra too)
    std::string zone_id;
    auto zit = extra.find("zone_id");
    if (zit != extra.end()) zone_id = zit->second;

    if (zone_id.empty()) {
        auto z_res = get_zone_id(zone, "");
        if (!z_res) {
            std::string err = "Cloudflare: failed to get zone_id for " + zone + ": " + z_res.error();
            logger::error("{}", err);
            return std::unexpected(err);
        }
        zone_id = *z_res;
    }

    return upsert_record_with_zone_id(zone, record_name, ip, zone_id, ttl, proxied);
}

std::expected<void, std::string> CloudflareProvider::upsert_record_with_zone_id(const std::string& zone,
                                                     const std::string& record_name,
                                                     const std::string& ip,
                                                     const std::string& zone_id,
                                                     int                ttl,
                                                     bool               proxied) {
    std::string fqdn = (record_name == "@") ? zone : (record_name + "." + zone);
    std::string search_url = "https://api.cloudflare.com/client/v4/zones/"
                           + zone_id + "/dns_records?type=AAAA&name=" + fqdn;

    auto search_resp = cf_request("GET", search_url);
    if (!search_resp) return std::unexpected(search_resp.error());
    if (search_resp->code != 200) {
        std::string err = "Cloudflare: search DNS record returned HTTP " + std::to_string(search_resp->code);
        logger::error("{}", err);
        return std::unexpected(err);
    }

    json new_record = {
        {"type",    "AAAA"},
        {"name",    fqdn},
        {"content", ip},
        {"ttl",     ttl},
        {"proxied", proxied}
    };

    try {
        auto sj = json::parse(search_resp->body);
        if (!sj.value("success", false)) {
            std::string msg;
            if (sj.contains("errors") && !sj["errors"].empty())
                msg = sj["errors"][0].value("message", "unknown");
            std::string err = "Cloudflare: DNS search failed: " + msg;
            logger::error("{}", err);
            return std::unexpected(err);
        }

        std::string method, endpoint;

        if (!sj["result"].empty()) {
            auto& existing = sj["result"][0];
            std::string existing_ip      = existing.value("content", "");
            bool        existing_proxied = existing.value("proxied", false);
            int         existing_ttl     = existing.value("ttl", 0);

            if (existing_ip == ip && existing_proxied == proxied && existing_ttl == ttl) {
                logger::info("Cloudflare: record {} already up-to-date", fqdn);
                return {};
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
        auto put_resp = cf_request(method, endpoint, body);
        if (!put_resp) return std::unexpected(put_resp.error());

        auto rj = json::parse(put_resp->body);
        if (!rj.value("success", false)) {
            std::string msg;
            if (rj.contains("errors") && !rj["errors"].empty())
                msg = rj["errors"][0].value("message", "unknown");
            std::string err = "Cloudflare: " + method + " failed: " + msg;
            logger::error("{}", err);
            return std::unexpected(err);
        }
        return {};

    } catch (const std::exception& e) {
        std::string err = std::string("Cloudflare: JSON error: ") + e.what();
        logger::error("{}", err);
        return std::unexpected(err);
    }
}

} // namespace provider
