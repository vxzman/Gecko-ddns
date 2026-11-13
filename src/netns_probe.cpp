#include "netns_probe.hpp"

#if defined(__linux__)

#include "ip_getter.hpp"
#include "log.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <format>
#include <print>
#include <string>
#include <vector>

namespace netns {

namespace {

void close_fd(int fd) {
    if (fd < 0) return;
    while (::close(fd) < 0 && errno == EINTR) {}
}

std::string read_all(int fd) {
    std::string buf;
    std::array<char, 4096> chunk{};
    while (true) {
        const ssize_t n = ::read(fd, chunk.data(), chunk.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        buf.append(chunk.data(), static_cast<std::size_t>(n));
    }
    return buf;
}

std::string join_urls(const std::vector<std::string>& urls) {
    std::string joined;
    for (const auto& url : urls) {
        if (!joined.empty()) joined.push_back(',');
        joined.append(url);
    }
    return joined;
}

/// Bind a resolv.conf the netns can actually use.
/// unshare(CLONE_NEWNS) is required: without a private mount namespace the bind would
/// replace the host's /etc/resolv.conf. Host resolv.conf usually points at 127.0.0.53
/// (systemd-resolved), which is this netns's own loopback and answers nothing.
void bind_netns_resolv_conf(const std::filesystem::path& netns_path) {
    if (::unshare(CLONE_NEWNS) != 0) {
        std::println(stderr, "unshare(CLONE_NEWNS) failed: {}; not bind-mounting resolv.conf",
                     std::strerror(errno));
        return;
    }
    if (::mount("none", "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
        std::println(stderr, "mount MS_PRIVATE failed: {}; not bind-mounting resolv.conf",
                     std::strerror(errno));
        return;
    }

    const std::string ns = netns_path.filename().string();
    const std::string from_netns = std::format("/etc/netns/{}/resolv.conf", ns);
    const char* source = nullptr;
    if (::access(from_netns.c_str(), R_OK) == 0) {
        source = from_netns.c_str();
    } else if (::access("/usr/local/etc/resolv.conf", R_OK) == 0) {
        source = "/usr/local/etc/resolv.conf";
    } else {
        std::println(stderr, "no resolv.conf for netns {}; DNS lookup inside the namespace may fail", ns);
        return;
    }
    if (::mount(source, "/etc/resolv.conf", nullptr, MS_BIND, nullptr) != 0) {
        std::println(stderr, "bind mount resolv.conf failed: {}", std::strerror(errno));
    }
}

std::string_view failure_text(int code) {
    switch (code) {
    case kExitMissing: return "netns does not exist";
    case kExitPermission: return "netns probe needs root or CAP_SYS_ADMIN";
    case kExitNoAddress: return "no usable IPv6 address inside netns";
    case kExitNoIface: return "interface not found inside netns";
    case kExitBadOutput: return "failed to parse netns probe output";
    case 127: return "failed to exec probe";
    default: return "netns probe failed";
    }
}

} // namespace

int execute_probe(std::string_view netns_name,
                  std::string_view iface,
                  const std::vector<std::string>& fallback_urls,
                  int timeout_sec) {
    // Probe stdout is the JSON protocol. Every log line goes to stderr.
    logger::set_output(stderr);

    const auto resolved = resolve_netns_path(netns_name);
    if (!resolved) {
        std::println(stderr, "{}", resolved.error());
        return kExitMissing;
    }

    const int fd = ::open(resolved->c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == EACCES || errno == EPERM) {
            std::println(stderr, "open {}: permission denied", resolved->string());
            return kExitPermission;
        }
        std::println(stderr, "open {}: {}", resolved->string(), std::strerror(errno));
        return kExitMissing;
    }
    if (::setns(fd, CLONE_NEWNET) != 0) {
        const int err = errno;
        close_fd(fd);
        std::println(stderr, "setns {}: {}", resolved->string(), std::strerror(err));
        return kExitPermission;
    }
    close_fd(fd);

    bind_netns_resolv_conf(*resolved);

    bool interface_missing = false;
    auto infos = ip_getter::get_from_interface(iface);
    std::string source = "interface";
    if (!infos) {
        interface_missing = infos.error().starts_with("Interface not found");
        logger::info("Interface IP source failed: {}. Trying API fallback...", infos.error());
        infos = ip_getter::get_from_apis(fallback_urls, timeout_sec);
        source = "api";
        if (!infos) {
            std::println(stderr, "{}", infos.error());
            return interface_missing ? kExitNoIface : kExitNoAddress;
        }
    }

    // select_best's filter is intentionally unchanged. Lifetimes come from the same record.
    const auto ip = ip_getter::select_best(*infos);
    if (!ip) {
        std::println(stderr, "{}", ip.error());
        return kExitNoAddress;
    }
    long preferred = 0;
    long valid = 0;
    for (const auto& info : *infos) {
        if (info.ip == *ip) {
            preferred = info.preferred_lft;
            valid = info.valid_lft;
            break;
        }
    }

    const nlohmann::json line{
        {"ip", *ip},
        {"source", source},
        {"iface", std::string{iface}},
        {"netns", std::string{netns_name}},
        {"preferred_lft", preferred},
        {"valid_lft", valid},
    };
    std::println("{}", line.dump());
    return kExitOk;
}

std::expected<ProbeResult, int> run_probe(std::string_view netns_name,
                                          std::string_view iface,
                                          const std::vector<std::string>& fallback_urls,
                                          int timeout_sec) {
    // Why execv, not "do the work in the forked child":
    // The parent may already be multithreaded (libcurl). After fork only the calling
    // thread exists, and any lock held by another thread is stuck forever. nlohmann::json,
    // std::string, and malloc are therefore unsafe between fork and exec. The child may
    // only call async-signal-safe functions (dup2, close, execv, _exit). execv replaces
    // the image with a single-threaded process, and that process runs the probe.
    // Argument strings are built here, before fork, so the child does not allocate.
    const auto resolved = resolve_netns_path(netns_name);
    if (!resolved) {
        logger::error("Config: {}", resolved.error());
        logger::info("netns={} probe exit={}", netns_name, kExitMissing);
        return std::unexpected(kExitMissing);
    }

    std::vector<std::string> storage;
    storage.emplace_back("gecko-ddns");
    storage.emplace_back("__probe");
    storage.emplace_back("--netns");
    storage.emplace_back(netns_name);
    storage.emplace_back("--iface");
    storage.emplace_back(iface);
    const std::string joined = join_urls(fallback_urls);
    if (!joined.empty()) {
        storage.emplace_back("--fallback-urls");
        storage.emplace_back(joined);
    }
    storage.emplace_back("--timeout");
    storage.emplace_back(std::to_string(timeout_sec));

    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& arg : storage) argv.push_back(arg.data());
    argv.push_back(nullptr);

    std::array<int, 2> fds{-1, -1};
    if (::pipe2(fds.data(), O_CLOEXEC) != 0) {
        logger::error("pipe2: {}", std::strerror(errno));
        logger::info("netns={} probe exit={}", netns_name, kExitBadOutput);
        return std::unexpected(kExitBadOutput);
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        const int err = errno;
        close_fd(fds[0]);
        close_fd(fds[1]);
        logger::error("fork: {}", std::strerror(err));
        logger::info("netns={} probe exit={}", netns_name, kExitBadOutput);
        return std::unexpected(kExitBadOutput);
    }
    if (pid == 0) {
        if (::dup2(fds[1], STDOUT_FILENO) < 0) ::_exit(127);
        close_fd(fds[0]);
        close_fd(fds[1]);
        ::execv("/proc/self/exe", argv.data());
        ::_exit(127);
    }

    close_fd(fds[1]);
    const std::string output = read_all(fds[0]);
    close_fd(fds[0]);

    int status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);

    int code = kExitBadOutput;
    if (waited == pid && WIFEXITED(status)) code = WEXITSTATUS(status);

    logger::info("netns={} probe exit={}", netns_name, code);
    if (code != kExitOk) {
        logger::error("{}", failure_text(code));
        return std::unexpected(code);
    }

    auto parsed = parse_probe_output(output);
    if (!parsed) {
        logger::error("{}", parsed.error());
        return std::unexpected(kExitBadOutput);
    }
    auto valid = validate_global_ipv6(parsed->ip);
    if (!valid) {
        logger::error("{}", valid.error());
        return std::unexpected(kExitBadOutput);
    }
    return *parsed;
}

} // namespace netns

#endif // __linux__
