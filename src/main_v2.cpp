#include "commands/command_handler.hpp"
#include "core/logger.hpp"

#include <argparse/argparse.hpp>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

#ifndef APP_VERSION
#  define APP_VERSION "dev"
#endif
#ifndef APP_COMMIT
#  define APP_COMMIT ""
#endif
#ifndef APP_BUILD_DATE
#  define APP_BUILD_DATE ""
#endif

namespace alasia {

/// Initialize and register built-in commands
void register_builtin_commands(commands::CommandRegistry& registry) {
    registry.register_command(std::make_unique<commands::RunCommand>());
    registry.register_command(std::make_unique<commands::VersionCommand>());
}

} // namespace alasia

int main(int argc, char* argv[]) {
    argparse::ArgumentParser program("alasia", APP_VERSION);
    program.add_description("强大的动态 DNS 客户端 - 支持多域名多服务商");

    // Run command
    argparse::ArgumentParser run_cmd("run");
    run_cmd.add_description("运行 DDNS 更新");
    run_cmd.add_argument("-c", "--config")
        .help("配置文件路径 (JSON 格式)")
        .default_value(std::string(""));
    run_cmd.add_argument("-d", "--dir")
        .help("工作目录（用于存放缓存和相对日志路径）")
        .default_value(std::string(""));
    run_cmd.add_argument("-i", "--ignore-cache")
        .help("忽略缓存 IP，强制更新")
        .default_value(false)
        .implicit_value(true);
    run_cmd.add_argument("-t", "--timeout")
        .help("超时时间（秒），默认 300 秒")
        .default_value(300);

    // Version command
    argparse::ArgumentParser version_cmd("version");
    version_cmd.add_description("显示版本信息");

    program.add_subparser(run_cmd);
    program.add_subparser(version_cmd);

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        std::cerr << program;
        return 1;
    }

    // Handle version command
    if (program.is_subcommand_used("version")) {
        std::cout << "alasia " APP_VERSION << "\n";
        if (std::string(APP_COMMIT).size() > 0)
            std::cout << "commit: " APP_COMMIT << "\n";
        if (std::string(APP_BUILD_DATE).size() > 0)
            std::cout << "built:  " APP_BUILD_DATE << "\n";
        return 0;
    }

    // Handle run command
    if (program.is_subcommand_used("run")) {
        std::string config_path = run_cmd.get<std::string>("--config");
        std::string dir_path    = run_cmd.get<std::string>("--dir");
        [[maybe_unused]] bool ignore_cache       = run_cmd.get<bool>("--ignore-cache");
        [[maybe_unused]] int timeout             = run_cmd.get<int>("--timeout");

        // Validate arguments
        if (config_path.empty()) {
            if (dir_path.empty()) {
                std::cerr << "错误：缺少配置文件参数 --config/-c，或请通过--dir/-d 指定工作目录以在其中查找 config.json\n";
                std::cerr << run_cmd;
                return 1;
            }
            config_path = (fs::path(dir_path) / "config.json").string();
            if (!fs::exists(config_path)) {
                std::cerr << "配置文件未找到：" << config_path << "\n";
                return 1;
            }
        }

        // Execute command
        alasia::commands::CommandContext ctx{
            .config_path = config_path,
            .work_dir = dir_path,
            .ignore_cache = ignore_cache,
            .timeout_seconds = timeout
        };

        alasia::commands::RunCommand cmd;
        auto result = cmd.execute(ctx);

        if (result.is_error()) {
            std::cerr << result.error_message() << "\n";
            return 1;
        }

        return result.value();
    }

    // No valid command provided
    std::cerr << program;
    return 1;
}
