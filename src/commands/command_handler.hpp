#pragma once

#include "core/result.hpp"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace alasia::commands {

/// Command execution context
struct CommandContext {
    std::string config_path;
    std::string work_dir;
    bool ignore_cache = false;
    int timeout_seconds = 300;
};

/// Command interface
class Command {
public:
    virtual ~Command() = default;
    
    /// Execute the command
    virtual Result<int> execute(const CommandContext& ctx) = 0;
    
    /// Get command name
    virtual std::string name() const = 0;
    
    /// Get command description
    virtual std::string description() const = 0;
};

/// Run command - executes DDNS update
class RunCommand : public Command {
public:
    std::string name() const override { return "run"; }
    std::string description() const override { return "Run DDNS update"; }
    Result<int> execute(const CommandContext& ctx) override;
};

/// Version command - displays version information
class VersionCommand : public Command {
public:
    std::string name() const override { return "version"; }
    std::string description() const override { return "Display version information"; }
    Result<int> execute(const CommandContext& /*ctx*/) override;
};

/// Command registry - manages available commands
class CommandRegistry {
public:
    static CommandRegistry& instance();
    
    void register_command(std::unique_ptr<Command> cmd);
    Command* get_command(const std::string& name);
    std::vector<std::string> get_command_names() const;

private:
    CommandRegistry() = default;
    std::map<std::string, std::unique_ptr<Command>> commands_;
};

} // namespace alasia::commands
