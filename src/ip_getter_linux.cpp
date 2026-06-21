// Linux Netlink implementation for IPv6 address retrieval
// Uses the raw Netlink socket API (RTM_GETADDR + RTM_NEWADDR).

#include "ip_getter.hpp"
#include "config.hpp"
#include "log.hpp"

#if defined(__linux__)

#include <asm/types.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

namespace ip_getter {

namespace {

// ─── RAII wrapper for socket file descriptor ──────────────────────────────────

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

/// Format raw IPv6 bytes to string.
static std::string format_ipv6(const uint8_t* addr16) {
    char buf[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, addr16, buf, sizeof(buf));
    return buf;
}

} // anonymous namespace

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
                continue;
            }
            return std::unexpected(std::string("recv() failed: ") + strerror(errno));
        }

        const nlmsghdr* nlh = reinterpret_cast<const nlmsghdr*>(buf);
        for (; NLMSG_OK(nlh, static_cast<unsigned>(len)); nlh = NLMSG_NEXT(nlh, len)) {
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
            info.preferred_lft = (preferred_lft == config::ND6_INFINITE_LIFETIME) ? config::INFINITE_LIFETIME_SECONDS : static_cast<long>(preferred_lft);
            info.valid_lft     = (valid_lft     == config::ND6_INFINITE_LIFETIME) ? config::INFINITE_LIFETIME_SECONDS : static_cast<long>(valid_lft);
            // is_deprecated is calculated by populate_info() from lifetime values
            populate_info(&info);

            if (info.is_candidate) result.push_back(info);
        }
    }

done:
    if (result.empty()) return std::unexpected("No suitable IPv6 address on interface " + std::string(iface_name));
    return result;
}

} // namespace ip_getter

#endif // __linux__
