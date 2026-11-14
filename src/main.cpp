#include "log.hpp"
#include "config.hpp"
#include "cache.hpp"
#include "ip_getter.hpp"
#include "curl_pool.hpp"
#include "signal.hpp"
#include "netns_probe.hpp"
#include "provider/cloudflare.hpp"
#include "provider/aliyun.hpp"

#include <argparse/argparse.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <iostream>
#include <print>
#include <ranges>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif
#ifndef APP_COMMIT
#define APP_COMMIT ""
#endif
#ifndef APP_BUILD_DATE
#define APP_BUILD_DATE ""
#endif

namespace {

void print_version() {
    std::println("gekddns {}", APP_VERSION);
    if (std::string_view{APP_COMMIT}.size() > 0) std::println("commit: {}", APP_COMMIT);
    if (std::string_view{APP_BUILD_DATE}.size() > 0) std::println("built:  {}", APP_BUILD_DATE);
    if (std::string_view{APP_COMPILER}.size() > 0) std::println("compiler: {}", APP_COMPILER);
}

struct UpdateResult {
    std::string record_name{};
    bool success = false;
    std::string error{};
};

struct CurrentIp {
    std::string ip{};
    int failure_code = 1;
};

std::vector<std::string> split_csv(std::string_view csv) {
    std::vector<std::string> urls;
    if (csv.empty()) return urls;
    for (auto&& part : csv | std::views::split(',')) {
        std::string url(part.begin(), part.end());
        if (!url.empty()) urls.push_back(std::move(url));
    }
    return urls;
}

UpdateResult update_cloudflare_record(const config::Config& cfg,
                                      const config::RecordConfig& rec,
                                      const std::string& current_ip,
                                      const std::filesystem::path& zone_id_cache_file) {
    UpdateResult result{.record_name = config::record_fqdn(rec)};
    logger::info("Processing record: {} ({})", result.record_name, rec.provider);

    const std::string proxy_url = config::get_record_proxy(cfg, rec);
    auto provider = provider::CloudflareProvider(rec.api_token, proxy_url);
    std::string zone_id = rec.zone_id;
    const int ttl = config::get_record_ttl(rec);

    if (zone_id.empty()) {
        const auto cached = config::read_zone_id_cache(zone_id_cache_file);
        const auto it = cached.find(rec.zone);
        if (it != cached.end() && !it->second.empty()) {
            zone_id = it->second;
            logger::debug("Zone ID loaded from cache for {}: {}", rec.zone, zone_id);
        }
    }
    if (zone_id.empty()) {
        logger::info("Zone ID not configured, fetching for zone: {}", rec.zone);
        auto fetched = provider.get_zone_id(rec.zone, "");
        if (!fetched) {
            result.error = std::format("Failed to get Zone ID: {}", fetched.error());
            logger::error("Record {}: {}", result.record_name, result.error);
            return result;
        }
        zone_id = std::move(*fetched);
        logger::info("Zone ID fetched: {}", zone_id);
        if (!config::update_zone_id_cache(zone_id_cache_file, rec.zone, zone_id)) {
            logger::warning("Warning: failed to save Zone ID cache");
        }
    }

    const auto ok = provider.upsert_record_with_zone_id(
        rec.zone, rec.name, current_ip, zone_id, ttl, rec.proxied, rec.type.empty() ? "AAAA" : rec.type);
    if (!ok) {
        result.error = std::format("Cloudflare upsert failed: {}", ok.error());
        logger::error("Failed to update {}", result.record_name);
        return result;
    }
    logger::success("Record {} updated successfully", result.record_name);
    result.success = true;
    return result;
}

UpdateResult update_aliyun_record(const config::Config& cfg,
                                  const config::RecordConfig& rec,
                                  const std::string& current_ip) {
    UpdateResult result{.record_name = config::record_fqdn(rec)};
    logger::info("Processing record: {} ({})", result.record_name, rec.provider);

    const std::string proxy_url = config::get_record_proxy(cfg, rec);
    const int ttl = config::get_record_ttl(rec);
    auto provider = provider::AliyunProvider(rec.access_key_id, rec.access_key_secret, proxy_url);
    const provider::AliyunOpts opts{.type = rec.type.empty() ? std::string{"AAAA"} : rec.type};
    const auto ok = provider.upsert_record(rec.zone, rec.name, current_ip, ttl, opts);
    if (!ok) {
        result.error = std::format("Aliyun upsert failed: {}", ok.error());
        logger::error("Failed to update {}", result.record_name);
        return result;
    }
    logger::success("Record {} updated successfully", result.record_name);
    result.success = true;
    return result;
}

UpdateResult update_single_record(const config::Config& cfg,
                                  const config::RecordConfig& rec,
                                  const std::string& current_ip,
                                  const std::filesystem::path& zone_id_cache_file,
                                  const std::atomic<bool>& timed_out,
                                  std::stop_token stop) {
    UpdateResult result{.record_name = config::record_fqdn(rec)};
    if (timed_out.load() || stop.stop_requested() || is_shutdown_requested()) {
        result.error = "shutdown requested";
        return result;
    }
    if (rec.provider == "cloudflare") return update_cloudflare_record(cfg, rec, current_ip, zone_id_cache_file);
    if (rec.provider == "aliyun") return update_aliyun_record(cfg, rec, current_ip);
    result.error = std::format("unsupported provider: {}", rec.provider);
    logger::error("Record {}: {}", result.record_name, result.error);
    return result;
}

CurrentIp get_current_ip(const config::Config& cfg, int timeout_sec) {
    const int query_timeout = timeout_sec > 0 ? std::max(10, timeout_sec / 2) : 0;
    if (!cfg.netns.empty()) {
#if defined(__linux__)
        auto probed = netns::run_probe(cfg.netns, cfg.ip_source.interface_name,
                                       cfg.ip_source.fallback_urls, query_timeout);
        if (!probed) return CurrentIp{.failure_code = probed.error()};
        return CurrentIp{.ip = std::move(probed->ip), .failure_code = 0};
#else
        logger::error("netns is only supported on Linux");
        return {};
#endif
    }

    const bool use_interface = !cfg.ip_source.interface_name.empty();
    auto infos = use_interface
        ? ip_getter::get_from_interface(cfg.ip_source.interface_name)
        : ip_getter::get_from_apis(cfg.ip_source.fallback_urls, query_timeout);
    if (!infos && use_interface) {
        logger::info("Interface IP source failed: {}. Trying API fallback...", infos.error());
        infos = ip_getter::get_from_apis(cfg.ip_source.fallback_urls, query_timeout);
    }
    auto ip = infos.and_then([](const std::vector<ip_getter::IPv6Info>& found) {
        return ip_getter::select_best(found);
    });
    if (!ip) return {};
    return CurrentIp{.ip = std::move(*ip), .failure_code = 0};
}

struct CacheCheckResult {
    bool skip = false;
    std::string last_ip;
};

CacheCheckResult check_cache_and_decide_skip(const std::string& current_ip,
                                             const std::filesystem::path& cache_file,
                                             bool ignore_cache) {
    const auto cache_data = cache::parse_cache_file(cache_file);
    CacheCheckResult result{.last_ip = cache_data.last_ip};
    if (ignore_cache) {
        logger::info("Ignoring cache (--ignore), forcing DDNS update");
        return result;
    }
    if (!result.last_ip.empty()) {
        if (result.last_ip == current_ip) {
            logger::info("IP has not changed since last run: {} — skipping DDNS update", current_ip);
            result.skip = true;
        } else {
            logger::info("IP changed from {} to {}", result.last_ip, current_ip);
        }
    }
    return result;
}

void update_records_parallel(const config::Config& cfg,
                             const std::string& current_ip,
                             const std::filesystem::path& zone_id_cache_file,
                             int timeout_sec,
                             int& success_count,
                             int& fail_count) {
    std::vector<UpdateResult> results(cfg.records.size());
    for (std::size_t i = 0; i < cfg.records.size(); ++i) {
        results[i].record_name = config::record_fqdn(cfg.records[i]);
        results[i].error = "timeout or shutdown before completion";
    }

    std::atomic<bool> timed_out{false};
    std::atomic<std::size_t> finished{0};
    std::vector<std::jthread> threads;
    threads.reserve(cfg.records.size());
    for (std::size_t i = 0; i < cfg.records.size(); ++i) {
        threads.emplace_back([&, i](std::stop_token stop) {
            struct Done {
                std::atomic<std::size_t>& counter;
                ~Done() { counter.fetch_add(1, std::memory_order_relaxed); }
            } done{finished};
            results[i] = update_single_record(cfg, cfg.records[i], current_ip, zone_id_cache_file, timed_out, stop);
        });
    }

    const auto start = std::chrono::steady_clock::now();
    while (finished.load(std::memory_order_relaxed) < cfg.records.size()) {
        if (is_shutdown_requested()) {
            timed_out.store(true);
            for (auto& thread : threads) thread.request_stop();
            break;
        }
        if (std::chrono::steady_clock::now() - start >= std::chrono::seconds(timeout_sec)) {
            timed_out.store(true);
            for (auto& thread : threads) thread.request_stop();
            logger::warning("Timeout reached ({} seconds), forcing shutdown", timeout_sec);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    threads.clear();

    for (const auto& result : results) {
        if (result.success) ++success_count;
        else ++fail_count;
    }
}

void update_cache(const std::filesystem::path& cache_file,
                  const std::string& current_ip,
                  const std::string& last_ip,
                  bool ignore_cache) {
    if (ignore_cache) return;
    if (last_ip != current_ip && !cache::write_last_ip(cache_file, current_ip)) {
        logger::warning("Warning: failed to write cache file");
    }
}

int run_cmd(const std::filesystem::path& config_path,
            const std::filesystem::path& dir_path,
            bool ignore_cache,
            int timeout_sec,
            std::string_view netns_override) {
    if (!logger::init()) {
        std::println(stderr, "Failed to initialize logging");
        return 1;
    }
    if (!curl_pool::initialize()) {
        logger::error("Failed to initialize libcurl");
        return 1;
    }
    setup_shutdown_handlers();

    std::error_code ec;
    const auto abs_config = std::filesystem::absolute(config_path, ec);
    if (ec) {
        logger::error("Failed to resolve config path: {}", ec.message());
        curl_pool::cleanup();
        return 1;
    }

    auto cfg_opt = config::read_config(abs_config);
    if (!cfg_opt) {
        logger::error("Failed to load configuration");
        curl_pool::cleanup();
        return 1;
    }
    config::Config cfg = std::move(*cfg_opt);
    if (!config::apply_netns_override(cfg, netns_override)) {
        logger::error("Failed to load configuration");
        curl_pool::cleanup();
        return 1;
    }

    const std::filesystem::path base_dir = dir_path.empty() ? abs_config.parent_path() : dir_path;
    logger::info("gekddns starting with {} record(s)", cfg.records.size());

    const CurrentIp found = get_current_ip(cfg, timeout_sec);
    if (found.ip.empty()) {
        logger::error("Failed to get current IP");
        curl_pool::cleanup();
        return found.failure_code == 0 ? 1 : found.failure_code;
    }
    logger::info("Current IPv6 address: {}", found.ip);

    const auto cache_file = config::get_cache_file_path(abs_config, base_dir);
    const auto cache_check = check_cache_and_decide_skip(found.ip, cache_file, ignore_cache);
    if (cache_check.skip) {
        curl_pool::cleanup();
        return 0;
    }

    const auto zone_id_cache_file = config::get_zone_id_cache_path(cache_file);
    int success_count = 0;
    int fail_count = 0;
    update_records_parallel(cfg, found.ip, zone_id_cache_file, timeout_sec, success_count, fail_count);
    logger::info("Update completed: {} succeeded, {} failed", success_count, fail_count);
    if (success_count > 0) update_cache(cache_file, found.ip, cache_check.last_ip, ignore_cache);

    curl_pool::cleanup();
    return fail_count > 0 ? 1 : 0;
}

int run_probe_cmd(std::string_view netns_name,
                  std::string_view iface,
                  std::string_view fallback_urls,
                  int timeout_sec) {
    logger::set_output(stderr);
    if (!logger::init()) return 1;
#if !defined(__linux__)
    std::println(stderr, "netns is only supported on Linux");
    (void)netns_name;
    (void)iface;
    (void)fallback_urls;
    (void)timeout_sec;
    return 1;
#else
    if (!curl_pool::initialize()) return 1;
    const int code = netns::execute_probe(netns_name, iface, split_csv(fallback_urls), timeout_sec);
    curl_pool::cleanup();
    return code;
#endif
}

} // namespace

int main(int argc, char* argv[]) {
    argparse::ArgumentParser program("gekddns", APP_VERSION);
    program.add_description("强大的动态 DNS 客户端 - 支持多域名多服务商");

    argparse::ArgumentParser run_parser("run");
    run_parser.add_description("运行 DDNS 更新");
    run_parser.add_argument("-c", "--config")
        .help("配置文件路径 (JSON 格式)")
        .default_value(std::string{});
    run_parser.add_argument("-d", "--dir")
        .help("工作目录（用于存放缓存和相对日志路径）")
        .default_value(std::string{});
    run_parser.add_argument("-i", "--ignore")
        .help("忽略缓存，强制更新且不写入缓存")
        .default_value(false)
        .implicit_value(true);
    run_parser.add_argument("-t", "--timeout")
        .help("超时时间（秒），默认 300 秒")
        .default_value(config::DEFAULT_TIMEOUT_SECONDS)
        .scan<'d', int>();
    run_parser.add_argument("--netns")
        .help("在指定 Linux network namespace 内探测 IPv6，覆盖配置文件")
        .default_value(std::string{});

    argparse::ArgumentParser probe_parser("__probe");
    probe_parser.add_description("内部命令：在 netns 内探测 IPv6，向 stdout 写一行 JSON");
    probe_parser.add_argument("--netns").help("netns 名或 /run/netns 下的路径").required();
    probe_parser.add_argument("--iface").help("netns 内的网卡").required();
    probe_parser.add_argument("--fallback-urls")
        .help("逗号分隔的回显 URL")
        .default_value(std::string{});
    probe_parser.add_argument("--timeout")
        .help("API 兜底的总超时（秒），0 表示不另加总超时")
        .default_value(0)
        .scan<'d', int>();

    argparse::ArgumentParser version_cmd("version");
    version_cmd.add_description("显示版本信息");

    program.add_subparser(run_parser);
    program.add_subparser(probe_parser);
    program.add_subparser(version_cmd);

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& ex) {
        std::println(stderr, "{}", ex.what());
        std::cerr << program;
        return 1;
    }

    if (program.is_subcommand_used("__probe")) {
        return run_probe_cmd(probe_parser.get<std::string>("--netns"),
                             probe_parser.get<std::string>("--iface"),
                             probe_parser.get<std::string>("--fallback-urls"),
                             probe_parser.get<int>("--timeout"));
    }
    if (program.is_subcommand_used("version")) {
        print_version();
        return 0;
    }
    if (program.is_subcommand_used("run")) {
        std::string config_path = run_parser.get<std::string>("--config");
        const std::string dir_path = run_parser.get<std::string>("--dir");
        if (config_path.empty()) {
            if (dir_path.empty()) {
                std::println(stderr, "错误：缺少配置文件参数 --config/-c，或请通过--dir/-d 指定工作目录以在其中查找 config.json");
                std::cerr << run_parser;
                return 1;
            }
            config_path = (std::filesystem::path(dir_path) / "config.json").string();
            if (!std::filesystem::exists(config_path)) {
                std::println(stderr, "配置文件未找到：{}", config_path);
                return 1;
            }
        }
        return run_cmd(config_path, dir_path, run_parser.get<bool>("--ignore"),
                       run_parser.get<int>("--timeout"), run_parser.get<std::string>("--netns"));
    }

    std::cerr << program;
    return 1;
}
