#include "ip_getter.hpp"
#include "config.hpp"
#include "curl_pool.hpp"
#include "http_client.hpp"
#include "log.hpp"

// RAII wrapper for socket file descriptor
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

// Linux netlink headers (only on Linux)
#if defined(__linux__)
#include <asm/types.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#endif

#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

// CURL for HTTP API fallback
#include <curl/curl.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ranges>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ip_getter {

// ─── Utilities ────────────────────────────────────────────────────────────────

static bool is_link_local(const uint8_t* addr16) {
    return addr16[0] == 0xfe && (addr16[1] & 0xc0) == 0x80;
}

static bool is_loopback(const uint8_t* addr16) {
    for (int i = 0; i < 15; ++i) if (addr16[i] != 0) return false;
    return addr16[15] == 1;
}

static bool is_ula(const uint8_t* addr16) {
    return (addr16[0] == 0xfc || addr16[0] == 0xfd);
}

static std::string format_ipv6(const uint8_t* addr16) {
    char buf[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, addr16, buf, sizeof(buf));
    return buf;
}

/// Populate IPv6Info fields (matching Go version's PopulateInfo)
static void populate_info(IPv6Info* info) {
    if (info->ip.empty()) return;

    // Convert IP to bytes for checking
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

// ─── Netlink IPv6 getter (Linux only) ────────────────────────────────────────

#if defined(__linux__)

std::expected<std::vector<IPv6Info>, std::string> get_from_interface(std::string_view iface_name) {
    unsigned int iface_idx = if_nametoindex(iface_name.data());
    if (iface_idx == 0) {
        return std::unexpected(std::string("Interface not found: ") + std::string(iface_name));
    }

    // Open netlink socket with RAII
    SocketGuard sock(socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE));
    if (sock.get() < 0) {
        return std::unexpected(std::string("socket() failed: ") + strerror(errno));
    }
    
    // Set receive timeout (5 seconds)
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(sock.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Send RTM_GETADDR request
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
        return std::unexpected(std::string("send() failed: ") + strerror(errno));
    }

    // Read response
    std::vector<IPv6Info> result;
    char buf[8192];

    while (true) {
        ssize_t len = recv(sock.get(), buf, sizeof(buf), 0);
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return std::unexpected("netlink recv timeout");
            }
            if (errno == EINTR) {
                continue;  // interrupted by signal, retry
            }
            return std::unexpected(std::string("recv() failed: ") + strerror(errno));
        }

        const nlmsghdr* nlh = reinterpret_cast<const nlmsghdr*>(buf);
        for (; NLMSG_OK(nlh, (unsigned)len); nlh = NLMSG_NEXT(nlh, len)) {
            if (nlh->nlmsg_type == NLMSG_DONE)  { goto done; }
            if (nlh->nlmsg_type == NLMSG_ERROR) { return std::unexpected("netlink error"); }
            if (nlh->nlmsg_type != RTM_NEWADDR)  continue;

            const ifaddrmsg* ifa = reinterpret_cast<const ifaddrmsg*>(NLMSG_DATA(nlh));
            if (ifa->ifa_index != iface_idx) continue;
            if (ifa->ifa_family != AF_INET6) continue;

            // Parse attributes
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
            if (valid_lft == 0) continue; // expired

            IPv6Info info;
            info.ip            = format_ipv6(addr);
            info.preferred_lft = (preferred_lft == config::ND6_INFINITE_LIFETIME) ? config::INFINITE_LIFETIME_SECONDS : (long)preferred_lft;
            info.valid_lft     = (valid_lft     == config::ND6_INFINITE_LIFETIME) ? config::INFINITE_LIFETIME_SECONDS : (long)valid_lft;
            info.is_deprecated = deprecated;
            populate_info(&info);

            if (info.is_candidate) result.push_back(info);
        }
    }

done:
    if (result.empty()) return std::unexpected("No suitable IPv6 address on interface " + std::string(iface_name));
    return result;
}

#endif // __linux__

// ─── HTTP API getter ──────────────────────────────────────────────────────────

namespace {

static std::string fetch_ip_from_url(const std::string& url, std::string& err) {
    // Use HttpClient for consistent behavior
    auto result = HttpClient::get(url, {}, config::HTTP_TIMEOUT_SECONDS, config::HTTP_MAX_RETRIES);
    if (!result) {
        err = result.error();
        return "";
    }
    
    // Parse response - extract first line and trim
    std::string body = result->body;
    auto newline_pos = body.find('\n');
    if (newline_pos != std::string::npos) {
        body = body.substr(0, newline_pos);
    }
    
    // Trim whitespace
    auto is_space = [](unsigned char c) { return std::isspace(c); };
    auto trimmed = body | std::views::drop_while(is_space) 
                         | std::views::reverse 
                         | std::views::drop_while(is_space) 
                         | std::views::reverse;
    std::string ip(trimmed.begin(), trimmed.end());
    
    if (ip.empty()) {
        err = "Empty response from API";
        return "";
    }
    
    return ip;
}

} // anonymous namespace

std::expected<std::vector<IPv6Info>, std::string> get_from_apis(const std::vector<std::string>& urls) {
    if (urls.empty()) { return std::unexpected("No API URLs configured"); }

    for (const auto& url : urls) {
        logger::info("Querying API: {}", url);
        std::string err;
        std::string ip = fetch_ip_from_url(url, err);
        if (!ip.empty()) {
            logger::info("API {} succeeded: {}", url, ip);
            IPv6Info info;
            info.ip            = ip;
            info.preferred_lft = config::INFINITE_LIFETIME_SECONDS; // treat as permanent
            info.valid_lft     = config::INFINITE_LIFETIME_SECONDS;
            populate_info(&info);
            return std::vector<IPv6Info>{info};
        }
        logger::error("API {} failed: {}", url, err);
        // keep last err
    }
    return std::unexpected("All API requests failed");
}

// ─── Select best ─────────────────────────────────────────────────────────────

std::expected<std::string, std::string> select_best(const std::vector<IPv6Info>& infos) {
    auto candidates = infos | std::views::filter([](const IPv6Info& info) { return info.is_candidate; });
    
    if (candidates.empty()) {
        return std::unexpected("No suitable global unicast IPv6 candidate found");
    }

    auto best = std::ranges::max_element(candidates, {}, &IPv6Info::preferred_lft);
    return best->ip;
}

} // namespace ip_getter
