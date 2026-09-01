// FreeBSD Netlink implementation for IPv6 address retrieval
// Uses the raw Netlink socket API (RTM_GETADDR + RTM_NEWADDR).
//
// We deliberately avoid the SNL (Simple Netlink) convenience headers
// (<netlink/netlink_snl.h>): they are written for C and break under C++
// because snl_realloc_msg_buffer() assigns the void* result of snl_allocz()
// to a char* (FreeBSD 15.1 / clang 19: "assigning to 'char *' from
// incompatible type 'void *'"). The raw socket API below is equivalent
// and keeps the C++ build independent of that header.

#include "ip_getter.hpp"
#include "config.hpp"

#if defined(__FreeBSD__)

#include <sys/socket.h>
#include <sys/time.h>

#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <netlink/netlink.h>
#include <netlink/netlink_route.h>

#include <unistd.h>

#include <cerrno>
#include <cstdint>
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
        if (this != &other) {
            fd_ = other.fd_;
            other.fd_ = -1;
        }
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

    // Send RTM_GETADDR dump request; the kernel filters by ifa_family and
    // ifa_index (same kernel path as `ifconfig`).
    struct {
        nlmsghdr  nlh;
        ifaddrmsg ifa;
    } req{};
    req.nlh.nlmsg_len   = NLMSG_LENGTH(sizeof(ifaddrmsg));
    req.nlh.nlmsg_type  = RTM_GETADDR;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nlh.nlmsg_seq   = 1;
    req.ifa.ifa_family  = AF_INET6;
    req.ifa.ifa_index   = iface_idx;

    if (send(sock.get(), &req, req.nlh.nlmsg_len, 0) < 0) {
        return std::unexpected(std::string("send() failed: ") + strerror(errno));
    }

    std::vector<IPv6Info> result;
    char buf[8192];
    bool dump_finished = false;

    while (!dump_finished) {
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

        // Iterate the netlink messages in this datagram.
        int remaining = static_cast<int>(len);
        const nlmsghdr* nlh = reinterpret_cast<const nlmsghdr*>(buf);

        while (remaining >= static_cast<int>(sizeof(nlmsghdr)) &&
               nlh->nlmsg_len >= sizeof(nlmsghdr) &&
               nlh->nlmsg_len <= static_cast<unsigned>(remaining)) {
            if (nlh->nlmsg_type == NLMSG_DONE) {
                dump_finished = true;
                break;
            }

            if (nlh->nlmsg_type == NLMSG_ERROR) {
                const nlmsgerr* err = reinterpret_cast<const nlmsgerr*>(NLMSG_DATA(nlh));
                if (err->error != 0) {
                    int errno_value = err->error < 0 ? -err->error : err->error;
                    return std::unexpected(std::string("netlink error: ") + strerror(errno_value));
                }
                // ACK (error == 0): keep waiting for the dump.
            } else if (nlh->nlmsg_type == RTM_NEWADDR) {
                const ifaddrmsg* ifa = reinterpret_cast<const ifaddrmsg*>(NLMSG_DATA(nlh));
                if (ifa->ifa_family == AF_INET6 && ifa->ifa_index == iface_idx) {
                    // Parse attributes. IFA_ADDRESS is always present; for
                    // point-to-point links IFA_LOCAL carries the local address.
                    const rtattr* rta = IFA_RTA(ifa);
                    int rta_len = static_cast<int>(IFA_PAYLOAD(nlh));

                    const uint8_t* addr = nullptr;   // IFA_ADDRESS
                    const uint8_t* local = nullptr;  // IFA_LOCAL
                    uint32_t preferred_lft = 0;
                    uint32_t valid_lft = 0;
                    bool has_cacheinfo = false;

                    for (; RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len)) {
                        switch (rta->rta_type) {
                        case IFA_ADDRESS:
                            if (RTA_PAYLOAD(rta) >= static_cast<int>(sizeof(struct in6_addr))) {
                                addr = static_cast<const uint8_t*>(RTA_DATA(rta));
                            }
                            break;
                        case IFA_LOCAL:
                            if (RTA_PAYLOAD(rta) >= static_cast<int>(sizeof(struct in6_addr))) {
                                local = static_cast<const uint8_t*>(RTA_DATA(rta));
                            }
                            break;
                        case IFA_CACHEINFO:
                            if (RTA_PAYLOAD(rta) >= static_cast<int>(sizeof(ifa_cacheinfo))) {
                                const auto* ci = static_cast<const ifa_cacheinfo*>(RTA_DATA(rta));
                                preferred_lft = ci->ifa_prefered;
                                valid_lft = ci->ifa_valid;
                                has_cacheinfo = true;
                            }
                            break;
                        default:
                            break;
                        }
                    }

                    const uint8_t* chosen = local != nullptr ? local : addr;
                    if (chosen != nullptr && !is_link_local(chosen) && !is_loopback(chosen)) {
                        // Remaining lifetimes in seconds as reported by the kernel
                        // (same semantics as the previous SNL-based implementation).
                        long preferred_lft_out = config::INFINITE_LIFETIME_SECONDS;
                        long valid_lft_out = config::INFINITE_LIFETIME_SECONDS;

                        if (has_cacheinfo) {
                            if (preferred_lft != 0 && preferred_lft != config::ND6_INFINITE_LIFETIME) {
                                preferred_lft_out = static_cast<long>(preferred_lft);
                            } else if (preferred_lft == 0) {
                                preferred_lft_out = 0;  // expired / no preferred lifetime
                            }
                            if (valid_lft != 0 && valid_lft != config::ND6_INFINITE_LIFETIME) {
                                valid_lft_out = static_cast<long>(valid_lft);
                            } else if (valid_lft == 0) {
                                valid_lft_out = 0;  // expired / no valid lifetime
                            }
                        }

                        IPv6Info info;
                        info.ip = format_ipv6(chosen);
                        info.preferred_lft = preferred_lft_out;
                        info.valid_lft = valid_lft_out;
                        // is_deprecated is calculated by populate_info() from lifetime values
                        populate_info(&info);

                        if (info.is_candidate) {
                            result.push_back(info);
                        }
                    }
                }
            }

            const int step = static_cast<int>(NLMSG_ALIGN(nlh->nlmsg_len));
            remaining -= step;
            nlh = reinterpret_cast<const nlmsghdr*>(
                reinterpret_cast<const char*>(nlh) + step);
        }
    }

    if (result.empty()) {
        return std::unexpected("No suitable IPv6 address on interface " + std::string(iface_name));
    }

    return result;
}

} // namespace ip_getter

#endif // __FreeBSD__
