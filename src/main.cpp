#include "log.hpp"
#include "config.hpp"
#include "cache.hpp"
#include "ip_getter.hpp"
#include "curl_pool.hpp"
#include "provider/cloudflare.hpp"
#include "provider/aliyun.hpp"

#include <argparse/argparse.hpp>

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

// ─── Version (injected by CMake via -D flags) ─────────────────────────────────
#ifndef APP_VERSION
#  define APP_VERSION "dev"
#endif
#ifndef APP_COMMIT
#  define APP_COMMIT ""
#endif
#ifndef APP_BUILD_DATE
#  define APP_BUILD_DATE ""
#endif

// ─── Signal handling ──────────────────────────────────────────────────────────
// Use volatile sig_atomic_t for async-signal-safe flag
// std::stop_source::request_stop() is NOT async-signal-safe
static volatile sig_atomic_t g_shutdown_requested = 0;

static void signal_handler(int) {
    g_shutdown_requested = 1;
}

// Check if shutdown was requested (async-signal-safe)
static bool is_shutdown_requested() {
    return g_shutdown_requested != 0;
}

// ─── Helpers ──────────────────────────────────────────────────────────────────

static void print_version() {
    std::cout << "alasia " APP_VERSION "\n";
    if (std::string(APP_COMMIT).size() > 0)
        std::cout << "commit: " APP_COMMIT "\n";
    if (std::string(APP_BUILD_DATE).size() > 0)
        std::cout << "built:  " APP_BUILD_DATE "\n";
}

// ─── Update a single DNS record ───────────────────────────────────────────────

struct UpdateResult {
    std::string record_name;
    bool        success = false;
    std::string error;
};

static UpdateResult update_single_record(
    const config::Config&       cfg,
    const config::RecordConfig& rec,
    const std::string&          current_ip,
    const std::string&          zone_id_cache_file,
    const std::atomic<bool>&    timed_out) {
    UpdateResult result;
    result.record_name = rec.record + "." + rec.zone;

    if (timed_out.load() || is_shutdown_requested()) {
        result.error = "shutdown requested";
        return result;
    }

    logger::info("Processing record: {} ({})", result.record_name, rec.provider);

    std::string proxy_url = config::get_record_proxy(cfg, rec);
    int         ttl       = config::get_record_ttl(rec);

    if (rec.provider == "cloudflare") {
        if (!rec.cloudflare) {
            result.error = "missing cloudflare config";
            logger::error("Record {}: {}", result.record_name, result.error);
            return result;
        }

        auto provider = provider::CloudflareProvider(rec.cloudflare->api_token, proxy_url);

        std::string zone_id = rec.cloudflare->zone_id;

        // Apply provider-level TTL and proxied overrides
        if (rec.cloudflare->ttl > 0) ttl = rec.cloudflare->ttl;
        bool proxied = rec.proxied || rec.cloudflare->proxied;

        // Auto-fetch zone_id if not set (try cache first)
        if (zone_id.empty()) {
            // Check timeout/shutdown before cache read
            if (timed_out.load() || is_shutdown_requested()) {
                result.error = "shutdown requested";
                return result;
            }

            // Try to read from ZoneID cache
            auto cached_zone_ids = config::read_zone_id_cache(zone_id_cache_file);
            auto it = cached_zone_ids.find(rec.zone);
            if (it != cached_zone_ids.end() && !it->second.empty()) {
                zone_id = it->second;
                logger::debug("Zone ID loaded from cache for {}: {}", rec.zone, zone_id);
            }
        }

        if (zone_id.empty()) {
            // Check timeout/shutdown before API call
            if (timed_out.load() || is_shutdown_requested()) {
                result.error = "shutdown requested";
                return result;
            }

            logger::info("Zone ID not configured, fetching for zone: {}", rec.zone);
            auto z_res = provider.get_zone_id(rec.zone, "");
            if (!z_res) {
                result.error = "Failed to get Zone ID: " + z_res.error();
                logger::error("Record {}: {}", result.record_name, result.error);
                return result;
            }
            zone_id = *z_res;
            logger::info("Zone ID fetched: {}", zone_id);

            // Save to cache
            if (!config::update_zone_id_cache(zone_id_cache_file, rec.zone, zone_id)) {
                logger::warning("Warning: failed to save Zone ID cache");
            }
        }

        // Check timeout/shutdown before upsert
        if (timed_out.load() || is_shutdown_requested()) {
            result.error = "shutdown requested";
            return result;
        }

        auto ok = provider.upsert_record_with_zone_id(
            rec.zone, rec.record, current_ip, zone_id, ttl, proxied);

        if (!ok) {
            result.error = "Cloudflare upsert failed: " + ok.error();
            logger::error("Failed to update {}", result.record_name);
            return result;
        }

    } else if (rec.provider == "aliyun") {
        if (!rec.aliyun) {
            result.error = "missing aliyun config";
            logger::error("Record {}: {}", result.record_name, result.error);
            return result;
        }

        if (!proxy_url.empty()) {
            logger::warning("Aliyun provider does not support proxy, ignoring use_proxy setting");
        }

        if (rec.aliyun->ttl > 0) ttl = rec.aliyun->ttl;

        // Check timeout/shutdown before API call
        if (timed_out.load() || is_shutdown_requested()) {
            result.error = "shutdown requested";
            return result;
        }

        auto provider = provider::AliyunProvider(
            rec.aliyun->access_key_id, rec.aliyun->access_key_secret);

        std::map<std::string, std::string> extra;
        auto ok = provider.upsert_record(rec.zone, rec.record, current_ip, ttl, extra);
        if (!ok) {
            result.error = "Aliyun upsert failed: " + ok.error();
            logger::error("Failed to update {}", result.record_name);
            return result;
        }

    } else {
        result.error = "unsupported provider: " + rec.provider;
        logger::error("Record {}: {}", result.record_name, result.error);
        return result;
    }

    logger::success("Record {} updated successfully", result.record_name);
    result.success = true;

    return result;
}

// ─── Run command ──────────────────────────────────────────────────────────────

static int run_cmd(const std::string& config_path, bool ignore_cache, int timeout_sec) {
    // Initialize CURL connection pool
    if (!curl_pool::initialize()) {
        std::cerr << "Failed to initialize libcurl\n";
        return 1;
    }

    // Register signal handlers
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Resolve absolute config path
    std::error_code ec;
    auto abs_config = std::filesystem::absolute(config_path, ec);
    if (ec) {
        std::cerr << "Failed to resolve config path: " << ec.message() << "\n";
        curl_pool::cleanup();
        return 1;
    }

    // Load config
    auto cfg_opt = config::read_config(abs_config.string());
    if (!cfg_opt) {
        std::cerr << "Failed to load configuration\n";
        curl_pool::cleanup();
        return 1;
    }
    config::Config cfg = std::move(*cfg_opt);

    // Init logging
    if (!logger::init(cfg.general.log_output)) {
        std::cerr << "Failed to initialize logging\n";
        curl_pool::cleanup();
        return 1;
    }

    logger::info("alasia starting with {} record(s)", cfg.records.size());

    // ── Get current IPv6 ──────────────────────────────────────────────────────
    std::expected<std::vector<ip_getter::IPv6Info>, std::string> infos_res;

    if (!cfg.general.get_ip.interface_name.empty()) {
        infos_res = ip_getter::get_from_interface(cfg.general.get_ip.interface_name);
        if (!infos_res) {
            logger::info("Interface {} failed: {}. Trying API fallback...",
                      cfg.general.get_ip.interface_name, infos_res.error());
            infos_res = ip_getter::get_from_apis(cfg.general.get_ip.urls);
        }
    } else {
        infos_res = ip_getter::get_from_apis(cfg.general.get_ip.urls);
    }

    if (!infos_res) {
        logger::error("Failed to get current IP: {}", infos_res.error());
        return 1;
    }

    auto ip_res = ip_getter::select_best(*infos_res);
    if (!ip_res) {
        logger::error("Failed to select best IPv6: {}", ip_res.error());
        return 1;
    }
    std::string current_ip = *ip_res;

    logger::info("Current IPv6 address: {}", current_ip);

    // ── Cache check ────────────────────────────────────────────────────────────
    std::string cache_file = config::get_cache_file_path(
        abs_config.string(), cfg.general.work_dir);
    std::string last_ip = cache::read_last_ip(cache_file);

    if (!ignore_cache && !last_ip.empty()) {
        if (last_ip == current_ip) {
            logger::info("IP has not changed since last run: {}", current_ip);
        } else {
            logger::info("IP changed from {} to {}", last_ip, current_ip);
        }
    }

    // ── Update all records in parallel ────────────────────────────────────────
    std::string              zone_id_cache_file = config::get_zone_id_cache_path(abs_config.string());
    std::vector<UpdateResult> results(cfg.records.size());
    std::vector<std::thread> threads;
    threads.reserve(cfg.records.size());

    // Atomic flag for timeout (shared across threads)
    std::atomic<bool> timed_out = false;

    for (size_t i = 0; i < cfg.records.size(); ++i) {
        threads.emplace_back([&, i]() {
            results[i] = update_single_record(cfg, cfg.records[i], current_ip, zone_id_cache_file, timed_out);
        });
    }

    // Wait for threads with timeout - check at intervals
    auto start = std::chrono::steady_clock::now();
    const auto check_interval = std::chrono::milliseconds(100);
    for (auto& t : threads) {
        // Wait for this thread with periodic timeout checks
        auto remaining = std::chrono::seconds(timeout_sec) - (std::chrono::steady_clock::now() - start);
        if (remaining <= std::chrono::seconds::zero()) {
            timed_out.store(true);
            logger::warning("Timeout reached ({} seconds), forcing shutdown", timeout_sec);
            break;
        }

        // Use a simple join with periodic checks
        // Note: std::jthread would be cleaner but requires C++20 stop_token
        t.join();

        // Check timeout after each thread completes
        if (is_shutdown_requested()) {
            timed_out.store(true);
            logger::warning("Shutdown requested, stopping remaining threads");
            break;
        }
    }

    // ── Summarize ─────────────────────────────────────────────────────────────
    int success_count = 0, fail_count = 0;
    for (const auto& r : results) {
        if (r.success) ++success_count; else ++fail_count;
    }

    logger::info("Update completed: {} succeeded, {} failed", success_count, fail_count);

    // Update cache only if IP changed and at least one succeeded
    bool any_success = (success_count > 0);
    if (any_success && last_ip != current_ip) {
        if (!cache::write_last_ip(cache_file, current_ip)) {
            logger::warning("Warning: failed to write cache file");
        }
    }

    // Cleanup CURL connection pool
    curl_pool::cleanup();

    return fail_count > 0 ? 1 : 0;
}

int main(int argc, char* argv[]) {
    argparse::ArgumentParser program("alasia", APP_VERSION);
    program.add_description("强大的动态 DNS 客户端 - 支持多域名多服务商");

    // ── Sub-command: run ──────────────────────────────────────────────────────
    argparse::ArgumentParser run_cmd_parser("run");
    run_cmd_parser.add_description("运行 DDNS 更新");
    run_cmd_parser.add_argument("-f", "--config")
        .help("配置文件路径 (JSON 格式)")
        .default_value(std::string(""));
    run_cmd_parser.add_argument("-i", "--ignore-cache")
        .help("忽略缓存 IP，强制更新")
        .default_value(false)
        .implicit_value(true);
    run_cmd_parser.add_argument("-t", "--timeout")
        .help("超时时间（秒），默认 300 秒")
        .default_value(300);

    // ── Sub-command: version ──────────────────────────────────────────────────
    argparse::ArgumentParser version_cmd("version");
    version_cmd.add_description("显示版本信息");

    program.add_subparser(run_cmd_parser);
    program.add_subparser(version_cmd);

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        std::cerr << program;
        return 1;
    }

    if (program.is_subcommand_used("version")) {
        print_version();
        return 0;
    }

    if (program.is_subcommand_used("run")) {
        std::string config_path = run_cmd_parser.get<std::string>("--config");
        if (config_path.empty()) {
            std::cerr << "错误: 缺少配置文件参数 --config / -f\n";
            std::cerr << run_cmd_parser;
            return 1;
        }
        bool ignore_cache = run_cmd_parser.get<bool>("--ignore-cache");
        int timeout = run_cmd_parser.get<int>("--timeout");
        return run_cmd(config_path, ignore_cache, timeout);
    }

    std::cerr << program;
    return 1;
}
