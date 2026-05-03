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

#ifndef APP_VERSION
#  define APP_VERSION "dev"
#endif
#ifndef APP_COMMIT
#  define APP_COMMIT ""
#endif
#ifndef APP_BUILD_DATE
#  define APP_BUILD_DATE ""
#endif

static volatile sig_atomic_t g_shutdown_requested = 0;
static void signal_handler(int) { g_shutdown_requested = 1; }
static bool is_shutdown_requested() { return g_shutdown_requested != 0; }

static void print_version() {
    std::cout << "alasia " APP_VERSION "\n";
    if (std::string(APP_COMMIT).size() > 0)
        std::cout << "commit: " APP_COMMIT "\n";
    if (std::string(APP_BUILD_DATE).size() > 0)
        std::cout << "built:  " APP_BUILD_DATE "\n";
    if (std::string(APP_COMPILER).size() > 0)
        std::cout << "compiler: " APP_COMPILER "\n";
}

// ─── Update Result ─────────────────────────────────────────────────────────────

struct UpdateResult {
    std::string record_name;
    bool        success = false;
    std::string error;
};

// ─── Cloudflare Record Update ──────────────────────────────────────────────────

static UpdateResult update_cloudflare_record(
    const config::Config&       cfg,
    const config::RecordConfig& rec,
    const std::string&          current_ip,
    const std::string&          zone_id_cache_file) {
    
    UpdateResult result;
    result.record_name = rec.record + "." + rec.zone;

    logger::info("Processing record: {} ({})", result.record_name, rec.provider);

    if (!rec.cloudflare) {
        result.error = "missing cloudflare config";
        logger::error("Record {}: {}", result.record_name, result.error);
        return result;
    }

    std::string proxy_url = config::get_record_proxy(cfg, rec);
    auto provider = provider::CloudflareProvider(rec.cloudflare->api_token, proxy_url);
    std::string zone_id = rec.cloudflare->zone_id;

    int ttl = rec.cloudflare->ttl > 0 ? rec.cloudflare->ttl : config::get_record_ttl(rec);
    bool proxied = rec.proxied || rec.cloudflare->proxied;

    // Auto-fetch zone_id if not set (try cache first)
    if (zone_id.empty()) {
        auto cached = config::read_zone_id_cache(zone_id_cache_file);
        auto it = cached.find(rec.zone);
        if (it != cached.end() && !it->second.empty()) {
            zone_id = it->second;
            logger::debug("Zone ID loaded from cache for {}: {}", rec.zone, zone_id);
        }
    }

    if (zone_id.empty()) {
        logger::info("Zone ID not configured, fetching for zone: {}", rec.zone);
        auto z_res = provider.get_zone_id(rec.zone, "");
        if (!z_res) {
            result.error = "Failed to get Zone ID: " + z_res.error();
            logger::error("Record {}: {}", result.record_name, result.error);
            return result;
        }
        zone_id = *z_res;
        logger::info("Zone ID fetched: {}", zone_id);

        if (!config::update_zone_id_cache(zone_id_cache_file, rec.zone, zone_id)) {
            logger::warning("Warning: failed to save Zone ID cache");
        }
    }

    auto ok = provider.upsert_record_with_zone_id(rec.zone, rec.record, current_ip, zone_id, ttl, proxied);
    if (!ok) {
        result.error = "Cloudflare upsert failed: " + ok.error();
        logger::error("Failed to update {}", result.record_name);
        return result;
    }

    logger::success("Record {} updated successfully", result.record_name);
    result.success = true;
    return result;
}

// ─── Aliyun Record Update ──────────────────────────────────────────────────────

static UpdateResult update_aliyun_record(
    const config::Config&       cfg,
    const config::RecordConfig& rec,
    const std::string&          current_ip) {
    
    UpdateResult result;
    result.record_name = rec.record + "." + rec.zone;

    logger::info("Processing record: {} ({})", result.record_name, rec.provider);

    if (!rec.aliyun) {
        result.error = "missing aliyun config";
        logger::error("Record {}: {}", result.record_name, result.error);
        return result;
    }

    std::string proxy_url = config::get_record_proxy(cfg, rec);
    if (!proxy_url.empty()) {
        logger::warning("Aliyun provider does not support proxy, ignoring use_proxy setting");
    }

    int ttl = rec.aliyun->ttl > 0 ? rec.aliyun->ttl : config::get_record_ttl(rec);
    auto provider = provider::AliyunProvider(rec.aliyun->access_key_id, rec.aliyun->access_key_secret);
    std::map<std::string, std::string> extra;
    
    auto ok = provider.upsert_record(rec.zone, rec.record, current_ip, ttl, extra);
    if (!ok) {
        result.error = "Aliyun upsert failed: " + ok.error();
        logger::error("Failed to update {}", result.record_name);
        return result;
    }

    logger::success("Record {} updated successfully", result.record_name);
    result.success = true;
    return result;
}

// ─── Single Record Update ──────────────────────────────────────────────────────

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

    if (rec.provider == "cloudflare") {
        return update_cloudflare_record(cfg, rec, current_ip, zone_id_cache_file);
    } else if (rec.provider == "aliyun") {
        return update_aliyun_record(cfg, rec, current_ip);
    } else {
        result.error = "unsupported provider: " + rec.provider;
        logger::error("Record {}: {}", result.record_name, result.error);
        return result;
    }
}

// ─── Helper Functions ──────────────────────────────────────────────────────────

static std::string get_current_ip(const config::Config& cfg) {
    auto infos_res = cfg.general.get_ip.interface_name.empty()
        ? ip_getter::get_from_apis(cfg.general.get_ip.urls)
        : ip_getter::get_from_interface(cfg.general.get_ip.interface_name);

    if (!infos_res) {
        logger::info("Primary IP source failed: {}. Trying API fallback...", infos_res.error());
        infos_res = ip_getter::get_from_apis(cfg.general.get_ip.urls);
    }

    if (!infos_res) {
        return "";
    }

    auto ip_res = ip_getter::select_best(*infos_res);
    if (!ip_res) {
        return "";
    }
    return *ip_res;
}

static void check_and_log_cache(const std::string& current_ip, const std::string& cache_file, bool ignore_cache) {
    std::string last_ip = cache::read_last_ip(cache_file);

    if (!ignore_cache && !last_ip.empty()) {
        if (last_ip == current_ip) {
            logger::info("IP has not changed since last run: {}", current_ip);
        } else {
            logger::info("IP changed from {} to {}", last_ip, current_ip);
        }
    }
}

static void update_records_parallel(
    const config::Config& cfg,
    const std::string&    current_ip,
    const std::string&    zone_id_cache_file,
    int                   timeout_sec,
    int&                  success_count,
    int&                  fail_count) {

    std::vector<UpdateResult> results(cfg.records.size());
    std::vector<std::thread> threads;
    threads.reserve(cfg.records.size());

    std::atomic<bool> timed_out = false;

    for (size_t i = 0; i < cfg.records.size(); ++i) {
        threads.emplace_back([&, i]() {
            results[i] = update_single_record(cfg, cfg.records[i], current_ip, zone_id_cache_file, timed_out);
        });
    }

    auto start = std::chrono::steady_clock::now();
    
    // Join all threads, but stop waiting after timeout or shutdown
    for (auto& t : threads) {
        if (timed_out.load() || is_shutdown_requested()) {
            break;
        }
        
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed >= std::chrono::seconds(timeout_sec)) {
            timed_out.store(true);
            logger::warning("Timeout reached ({} seconds), forcing shutdown", timeout_sec);
            break;
        }
        
        t.join();
    }
    
    // Join any remaining threads (they should exit quickly after timed_out is set)
    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    for (const auto& r : results) {
        if (r.success) ++success_count; else ++fail_count;
    }
}

static void log_summary(int success_count, int fail_count) {
    logger::info("Update completed: {} succeeded, {} failed", success_count, fail_count);
}

static void update_cache(const std::string& cache_file, const std::string& current_ip, const std::string& last_ip) {
    if (last_ip != current_ip) {
        if (!cache::write_last_ip(cache_file, current_ip)) {
            logger::warning("Warning: failed to write cache file");
        }
    }
}

// ─── Run command ──────────────────────────────────────────────────────────────

static int run_cmd(const std::string& config_path, const std::string& dir_path,
                   bool ignore_cache, int timeout_sec) {

    if (!curl_pool::initialize()) {
        std::cerr << "Failed to initialize libcurl\n";
        return 1;
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::error_code ec;
    auto abs_config = std::filesystem::absolute(config_path, ec);
    if (ec) {
        std::cerr << "Failed to resolve config path: " << ec.message() << "\n";
        curl_pool::cleanup();
        return 1;
    }

    auto cfg_opt = config::read_config(abs_config.string());
    if (!cfg_opt) {
        std::cerr << "Failed to load configuration\n";
        curl_pool::cleanup();
        return 1;
    }
    config::Config cfg = std::move(*cfg_opt);

    // Determine base directory
    std::string base_dir = dir_path;
    if (base_dir.empty()) {
        base_dir = abs_config.parent_path().string();
    }

    // Initialize logger
    if (!logger::init()) {
        std::cerr << "Failed to initialize logging\n";
        curl_pool::cleanup();
        return 1;
    }

    logger::info("alasia starting with {} record(s)", cfg.records.size());

    // Get current IP
    std::string current_ip = get_current_ip(cfg);
    if (current_ip.empty()) {
        logger::error("Failed to get current IP");
        curl_pool::cleanup();
        return 1;
    }
    logger::info("Current IPv6 address: {}", current_ip);

    // Cache
    std::string cache_file = config::get_cache_file_path(abs_config.string(), base_dir);
    check_and_log_cache(current_ip, cache_file, ignore_cache);

    // Update records
    std::string zone_id_cache_file = config::get_zone_id_cache_path(abs_config.string());
    int success_count = 0, fail_count = 0;
    update_records_parallel(cfg, current_ip, zone_id_cache_file, timeout_sec, success_count, fail_count);
    log_summary(success_count, fail_count);

    // Update cache
    std::string last_ip = cache::read_last_ip(cache_file);
    if (success_count > 0) {
        update_cache(cache_file, current_ip, last_ip);
    }

    curl_pool::cleanup();
    return fail_count > 0 ? 1 : 0;
}

int main(int argc, char* argv[]) {
    argparse::ArgumentParser program("alasia", APP_VERSION);
    program.add_description("强大的动态 DNS 客户端 - 支持多域名多服务商");

    argparse::ArgumentParser run_cmd_parser("run");
    run_cmd_parser.add_description("运行 DDNS 更新");
    run_cmd_parser.add_argument("-c", "--config")
        .help("配置文件路径 (JSON 格式)")
        .default_value(std::string(""));
    run_cmd_parser.add_argument("-d", "--dir")
        .help("工作目录（用于存放缓存和相对日志路径）")
        .default_value(std::string(""));
    run_cmd_parser.add_argument("-i", "--ignore-cache")
        .help("忽略缓存 IP，强制更新")
        .default_value(false)
        .implicit_value(true);
    run_cmd_parser.add_argument("-t", "--timeout")
        .help("超时时间（秒），默认 300 秒")
        .default_value(config::DEFAULT_TIMEOUT_SECONDS);

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
        std::string dir_path    = run_cmd_parser.get<std::string>("--dir");

        if (config_path.empty()) {
            if (dir_path.empty()) {
                std::cerr << "错误：缺少配置文件参数 --config/-c，或请通过--dir/-d 指定工作目录以在其中查找 config.json\n";
                std::cerr << run_cmd_parser;
                return 1;
            }
            config_path = (std::filesystem::path(dir_path) / "config.json").string();
            if (!std::filesystem::exists(config_path)) {
                std::cerr << "配置文件未找到：" << config_path << "\n";
                return 1;
            }
        }

        bool ignore_cache = run_cmd_parser.get<bool>("--ignore-cache");
        int timeout = run_cmd_parser.get<int>("--timeout");
        return run_cmd(config_path, dir_path, ignore_cache, timeout);
    }

    std::cerr << program;
    return 1;
}
