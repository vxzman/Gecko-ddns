#include "commands/command_handler.hpp"
#include "config/config.hpp"
#include "services/ip/ip_service.hpp"
#include "services/dns/dns_factory.hpp"
#include "services/cache/cache_service.hpp"
#include "core/logger.hpp"
#include "core/types.hpp"

#include <atomic>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <thread>

namespace fs = std::filesystem;
namespace alasia::commands {

namespace {

// Global shutdown flag - using volatile sig_atomic_t for async-signal-safety
volatile sig_atomic_t g_shutdown_requested = 0;

void signal_handler(int signum) {
    (void)signum;
    g_shutdown_requested = 1;  // async-signal-safe
}

// Global timeout flag - shared across threads
std::atomic<bool> g_timeout_reached{false};

struct RecordUpdateTask {
    config::RecordConfig record;
    std::string proxy_url;
    std::string zone_id_cache_file;
};

Result<void> update_single_record(const RecordUpdateTask& task,
                                   const std::string& current_ip) {
    std::string record_name = task.record.record + "." + task.record.zone;
    
    logger::info("Processing record: {} ({})", record_name, task.record.provider);
    
    // Create DNS provider
    auto provider_result = services::DnsProviderFactory::create(task.record.provider, task.record);
    if (provider_result.is_error()) {
        std::string err = "Failed to create DNS provider: " + provider_result.error_message();
        logger::error("{}", err);
        return Result<void>::error(err);
    }
    
    auto& provider = provider_result.value();
    
    // Prepare extra parameters
    std::map<std::string, std::string> extra;
    
    // Handle Cloudflare-specific parameters
    if (task.record.provider == "cloudflare") {
        if (task.record.cloudflare) {
            // Load zone_id from cache if not configured
            std::string zone_id = task.record.cloudflare->zone_id;
            if (zone_id.empty()) {
                auto cached = cache::read_zone_id_cache(task.zone_id_cache_file);
                auto it = cached.find(task.record.zone);
                if (it != cached.end() && !it->second.empty()) {
                    zone_id = it->second;
                    logger::debug("Zone ID loaded from cache for {}: {}", task.record.zone, zone_id);
                }
            }
            
            // Fetch zone_id if still empty
            if (zone_id.empty()) {
                logger::info("Zone ID not configured, fetching for zone: {}", task.record.zone);
                
                // Need to get zone_id from provider
                // This requires casting to CloudflareDnsProvider - TODO: improve this
                // For now, we'll handle it in a simplified way
            }
            
            extra["zone_id"] = zone_id;
            extra["proxied"] = (task.record.cloudflare->proxied || task.record.proxied) ? "true" : "false";
        }
    }
    
    // Get effective TTL
    int ttl = config::get_record_ttl(task.record);
    
    // Execute update
    auto result = provider->upsert_record(task.record.zone, task.record.record, current_ip, ttl, extra);
    
    if (result.is_error()) {
        logger::error("Failed to update {}: {}", record_name, result.error_message());
        return result;
    }
    
    logger::success("Record {} updated successfully", record_name);
    return Result<void>();
}

} // anonymous namespace

Result<int> RunCommand::execute(const CommandContext& ctx) {
    // Setup signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    // Resolve config path
    std::error_code ec;
    fs::path abs_config = fs::absolute(ctx.config_path, ec);
    if (ec) {
        return Result<int>::error("Failed to resolve config path: " + ec.message());
    }
    
    // Load configuration
    auto config_result = config::load_config(abs_config.string());
    if (config_result.is_error()) {
        logger::error("Failed to load configuration: {}", config_result.error_message());
        return Result<int>::error("Failed to load configuration");
    }
    config::Config cfg = config_result.value();
    
    // Determine base directory
    std::string base_dir = ctx.work_dir;
    if (base_dir.empty()) {
        base_dir = abs_config.parent_path().string();
    }
    
    // Resolve log output path
    std::string log_output = cfg.general.log_output;
    if (!log_output.empty() && log_output != "shell" && !fs::path(log_output).is_absolute()) {
        log_output = (fs::path(base_dir) / log_output).string();
        fs::create_directories(fs::path(log_output).parent_path(), ec);
    }
    
    // Initialize logger
    if (!logger::init(log_output)) {
        return Result<int>::error("Failed to initialize logger");
    }
    
    logger::info("alasia starting with {} record(s)", cfg.records.size());
    
    // Create IP service
    services::IpServiceConfig ip_config;
    ip_config.interface_name = cfg.general.get_ip.interface_name;
    ip_config.api_urls = cfg.general.get_ip.urls;
    
    auto ip_service_result = services::create_ip_service(ip_config);
    if (ip_service_result.is_error()) {
        logger::error("Failed to create IP service: {}", ip_service_result.error_message());
        return Result<int>::error("Failed to create IP service");
    }
    
    // Get current IP
    auto ip_result = ip_service_result.value()->get_current_ip();
    if (ip_result.is_error()) {
        logger::error("Failed to get current IP: {}", ip_result.error_message());
        return Result<int>::error("Failed to get current IP");
    }
    
    std::string current_ip = ip_result.value();
    logger::info("Current IPv6 address: {}", current_ip);
    
    // Check cache
    std::string cache_file = cache::get_cache_file_path(abs_config.string(), base_dir);
    std::string last_ip = cache::read_last_ip(cache_file);
    
    if (!ctx.ignore_cache && !last_ip.empty()) {
        if (last_ip == current_ip) {
            logger::info("IP has not changed since last run: {}", current_ip);
        } else {
            logger::info("IP changed from {} to {}", last_ip, current_ip);
        }
    }
    
    // Update records
    std::string zone_id_cache_file = cache::get_zone_id_cache_path(abs_config.string());
    int success_count = 0;
    int fail_count = 0;
    
    std::vector<std::thread> threads;
    std::vector<UpdateResult> results(cfg.records.size());
    
    for (size_t i = 0; i < cfg.records.size(); ++i) {
        threads.emplace_back([&, i]() {
            // Check shutdown and timeout before starting
            if (g_shutdown_requested || g_timeout_reached.load()) {
                results[i] = {cfg.records[i].record + "." + cfg.records[i].zone, false, "shutdown requested"};
                return;
            }
            
            RecordUpdateTask task{
                cfg.records[i],
                config::get_record_proxy(cfg, cfg.records[i]),
                zone_id_cache_file
            };

            auto result = update_single_record(task, current_ip);
            if (result.is_ok()) {
                results[i] = {task.record.record + "." + task.record.zone, true, ""};
            } else {
                results[i] = {task.record.record + "." + task.record.zone, false, result.error_message()};
            }
        });
    }
    
    // Wait for threads with timeout
    auto start = std::chrono::steady_clock::now();
    for (auto& t : threads) {
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed >= std::chrono::seconds(ctx.timeout_seconds)) {
            logger::warning("Timeout reached ({} seconds), stopping remaining threads", ctx.timeout_seconds);
            g_timeout_reached.store(true);  // Signal timeout to all threads
            break;
        }

        if (t.joinable()) {
            t.join();
        }

        if (g_shutdown_requested) {
            logger::warning("Shutdown requested, stopping remaining threads");
            break;
        }
    }

    // Wait for remaining threads to finish (they should exit quickly after timeout/shutdown)
    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    
    // Count results
    for (const auto& r : results) {
        if (r.success) {
            ++success_count;
        } else {
            ++fail_count;
        }
    }
    
    logger::info("Update completed: {} succeeded, {} failed", success_count, fail_count);
    
    // Update cache if successful
    if (success_count > 0 && last_ip != current_ip) {
        if (!cache::write_last_ip(cache_file, current_ip)) {
            logger::warning("Warning: failed to write cache file");
        }
    }
    
    logger::shutdown();
    
    return Result<int>(fail_count > 0 ? 1 : 0);
}

Result<int> VersionCommand::execute(const CommandContext& /*ctx*/) {
    std::cout << "alasia " APP_VERSION << "\n";
    if (std::string(APP_COMMIT).size() > 0) {
        std::cout << "commit: " APP_COMMIT << "\n";
    }
    if (std::string(APP_BUILD_DATE).size() > 0) {
        std::cout << "built:  " APP_BUILD_DATE << "\n";
    }
    return Result<int>(0);
}

// CommandRegistry implementation
CommandRegistry& CommandRegistry::instance() {
    static CommandRegistry registry;
    return registry;
}

void CommandRegistry::register_command(std::unique_ptr<Command> cmd) {
    commands_[cmd->name()] = std::move(cmd);
}

Command* CommandRegistry::get_command(const std::string& name) {
    auto it = commands_.find(name);
    return (it != commands_.end()) ? it->second.get() : nullptr;
}

std::vector<std::string> CommandRegistry::get_command_names() const {
    std::vector<std::string> names;
    for (const auto& [name, _] : commands_) {
        names.push_back(name);
    }
    return names;
}

} // namespace alasia::commands
