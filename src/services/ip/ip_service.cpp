#include "services/ip/ip_service.hpp"
#include "core/logger.hpp"
#include "core/result.hpp"
#include "core/types.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <expected>
#include <net/if.h>
#include <ranges>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#if defined(__linux__)
#include <asm/types.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#endif

#include <arpa/inet.h>
#include <netinet/in.h>

// HTTP client for API fallback
#include "http/http_client.hpp"

namespace alasia::services {

namespace {

// ─── Utilities ────────────────────────────────────────────────────────────────

class SocketGuard {
    int fd_;
public:
    explicit SocketGuard(int fd) : fd_(fd) {}
    ~SocketGuard() { if (fd_ >= 0) close(fd_); }
    int get() const { return fd_; }
    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;
    SocketGuard(SocketGuard&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    SocketGuard& operator=(SocketGuard&& other) noexcept {
        if (this != &other) { fd_ = other.fd_; other.fd_ = -1; }
        return *this;
    }
};

bool is_link_local(const uint8_t* addr) {
    return addr[0] == 0xfe && (addr[1] & 0xc0) == 0x80;
}

bool is_loopback(const uint8_t* addr) {
    for (int i = 0; i < 15; ++i) if (addr[i] != 0) return false;
    return addr[15] == 1;
}

bool is_ula(const uint8_t* addr) {
    return (addr[0] == 0xfc || addr[0] == 0xfd);
}

std::string format_ipv6(const uint8_t* addr) {
    char buf[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, addr, buf, sizeof(buf));
    return buf;
}

void populate_info(IPv6Info* info) {
    if (info->ip.empty()) return;

    uint8_t addr[16];
    if (inet_pton(AF_INET6, info->ip.c_str(), addr) != 1) return;

    info->is_unique_local = is_ula(addr);

    if (is_link_local(addr)) {
        info->scope = "Link Local";
    } else if (info->is_unique_local) {
        info->scope = "Unique Local (ULA)";
    } else {
        info->scope = "Global Unicast";
    }

    info->is_deprecated = (info->preferred_lft <= 0 && info->valid_lft > 0);

    if (info->valid_lft == 0) {
        info->address_state = "Expired";
    } else if (info->is_deprecated) {
        info->address_state = "Deprecated";
    } else if (info->preferred_lft < info->valid_lft) {
        info->address_state = "Preferred/Dynamic";
    } else {
        info->address_state = "Preferred/Static";
    }

    info->is_candidate = (info->scope == "Global Unicast" &&
                          !info->is_deprecated &&
                          !info->is_unique_local &&
                          info->valid_lft > 0);
}

// ─── Default IP Service Implementation ────────────────────────────────────────

class DefaultIpService : public IpService {
public:
    explicit DefaultIpService(IpServiceConfig config)
        : config_(std::move(config)) {}
    
    Result<std::string> get_current_ip() override;

private:
    Result<std::string> get_from_interface();
    Result<std::string> get_from_apis();
    Result<std::string> select_best(const std::vector<IPv6Info>& infos);
    
    IpServiceConfig config_;
};

#if defined(__linux__)

Result<std::string> DefaultIpService::get_from_interface() {
    unsigned int iface_idx = if_nametoindex(config_.interface_name.c_str());
    if (iface_idx == 0) {
        return Result<std::string>::error("Interface not found: " + config_.interface_name);
    }

    SocketGuard sock(socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE));
    if (sock.get() < 0) {
        return Result<std::string>::error("socket() failed: " + std::string(strerror(errno)));
    }

    struct {
        nlmsghdr  nlh;
        ifaddrmsg ifa;
    } req{};
    req.nlh.nlmsg_len   = NLMSG_LENGTH(sizeof(ifaddrmsg));
    req.nlh.nlmsg_type  = RTM_GETADDR;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nlh.nlmsg_seq   = 1;
    req.ifa.ifa_family  = AF_INET6;

    if (send(sock.get(), &req, req.nlh.nlmsg_len, 0) < 0) {
        return Result<std::string>::error("send() failed: " + std::string(strerror(errno)));
    }

    std::vector<IPv6Info> result;
    char buf[8192];

    while (true) {
        ssize_t len = recv(sock.get(), buf, sizeof(buf), 0);
        if (len < 0) {
            return Result<std::string>::error("recv() failed: " + std::string(strerror(errno)));
        }

        const nlmsghdr* nlh = reinterpret_cast<const nlmsghdr*>(buf);
        ssize_t len_remaining = len;

        for (; NLMSG_OK(nlh, static_cast<size_t>(len_remaining)); nlh = NLMSG_NEXT(nlh, len_remaining)) {
            if (nlh->nlmsg_type == NLMSG_DONE) { break; }
            if (nlh->nlmsg_type == NLMSG_ERROR) { return Result<std::string>::error("netlink error"); }
            if (nlh->nlmsg_type != RTM_NEWADDR) continue;

            const ifaddrmsg* ifa = reinterpret_cast<const ifaddrmsg*>(NLMSG_DATA(nlh));
            if (ifa->ifa_index != iface_idx) continue;
            if (ifa->ifa_family != AF_INET6) continue;

            const rtattr* rta = IFA_RTA(ifa);
            auto rta_len = IFA_PAYLOAD(nlh);

            const uint8_t* addr = nullptr;
            uint32_t preferred_lft = 0, valid_lft = 0;
            bool deprecated = (ifa->ifa_flags & IFA_F_DEPRECATED) != 0;

            for (; RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len)) {
                if (rta->rta_type == IFA_ADDRESS) {
                    addr = reinterpret_cast<const uint8_t*>(RTA_DATA(rta));
                }
                if (rta->rta_type == IFA_CACHEINFO) {
                    const ifa_cacheinfo* ci = reinterpret_cast<const ifa_cacheinfo*>(RTA_DATA(rta));
                    preferred_lft = ci->ifa_prefered;
                    valid_lft     = ci->ifa_valid;
                }
            }

            if (!addr) continue;
            if (is_link_local(addr) || is_loopback(addr)) continue;
            if (valid_lft == 0) continue;

            IPv6Info info;
            info.ip            = format_ipv6(addr);
            info.preferred_lft = (preferred_lft == ND6_INFINITE_LIFETIME) ? INFINITE_LIFETIME_SECONDS : static_cast<long>(preferred_lft);
            info.valid_lft     = (valid_lft     == ND6_INFINITE_LIFETIME) ? INFINITE_LIFETIME_SECONDS : static_cast<long>(valid_lft);
            info.is_deprecated = deprecated;
            populate_info(&info);

            if (info.is_candidate) {
                result.push_back(info);
            }
        }
    }

    if (result.empty()) {
        return Result<std::string>::error("No suitable IPv6 address on interface " + config_.interface_name);
    }

    return select_best(result);
}

#else
// Non-Linux platforms: only API fallback
Result<std::string> DefaultIpService::get_from_interface() {
    return Result<std::string>::error("Interface-based IP acquisition only supported on Linux");
}
#endif

Result<std::string> DefaultIpService::get_from_apis() {
    if (config_.api_urls.empty()) {
        return Result<std::string>::error("No API URLs configured");
    }

    auto http_client = http::create_default_client();
    
    for (const auto& url : config_.api_urls) {
        logger::info("Querying API: {}", url);
        
        auto result = http_client->get(url);
        if (result.is_ok()) {
            std::string ip = result.value().body;
            
            // Trim whitespace and extract first line
            auto newline_pos = ip.find('\n');
            if (newline_pos != std::string::npos) {
                ip = ip.substr(0, newline_pos);
            }
            ip.erase(0, ip.find_first_not_of(" \t\r\n"));
            ip.erase(ip.find_last_not_of(" \t\r\n") + 1);
            
            if (!ip.empty()) {
                logger::info("API {} succeeded: {}", url, ip);
                return Result<std::string>(std::move(ip));
            }
        }
        
        logger::error("API {} failed", url);
    }
    
    return Result<std::string>::error("All API requests failed");
}

Result<std::string> DefaultIpService::select_best(const std::vector<IPv6Info>& infos) {
    auto candidates = infos | std::views::filter([](const IPv6Info& info) {
        return info.is_candidate;
    });

    if (candidates.empty()) {
        return Result<std::string>::error("No suitable global unicast IPv6 candidate found");
    }

    auto best = std::ranges::max_element(candidates, {}, &IPv6Info::preferred_lft);
    return Result<std::string>(best->ip);
}

Result<std::string> DefaultIpService::get_current_ip() {
    if (config_.use_interface()) {
        auto result = get_from_interface();
        if (result.is_ok()) {
            return result;
        }
        logger::info("Interface method failed: {}. Trying API fallback...", result.error_message());
    }
    
    return get_from_apis();
}

} // anonymous namespace

Result<std::unique_ptr<IpService>> create_ip_service(const IpServiceConfig& config) {
    if (!config.use_interface() && !config.has_api_urls()) {
        return Result<std::unique_ptr<IpService>>::error(
            "Either interface_name or api_urls must be configured");
    }
    
    return Result<std::unique_ptr<IpService>>(
        std::make_unique<DefaultIpService>(config));
}

} // namespace alasia::services
