// FreeBSD Netlink implementation for IPv6 address retrieval
// Uses the Netlink SNL (Simple Netlink) API available in FreeBSD 14+.
// Reference: FreeBSD ifconfig af_inet6.c (in6_status_nl / show_lifetime)
//
// The Netlink cacheinfo (ifa_cacheinfo) reports remaining lifetimes
// (ifa_prefered / ifa_valid in seconds) measured at tstamp (CLOCK_MONOTONIC ms).
// For DDNS purposes the raw remaining values are directly usable.

#include "ip_getter.hpp"
#include "log.hpp"

#if defined(__FreeBSD__)

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <net/if.h>
#include <netinet/in.h>
#include <netinet/in_var.h>      /* IN6_IFF_* flags */
#include <arpa/inet.h>

#include <netlink/netlink.h>
#include <netlink/netlink_route.h>
#include <netlink/netlink_snl.h>
#include <netlink/netlink_snl_route.h>
#include <netlink/netlink_snl_route_parsers.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

namespace ip_getter {

namespace {

constexpr uint32_t ND6_INFINITE_LIFETIME = 0xFFFFFFFFU;

} // anonymous namespace

std::expected<std::vector<IPv6Info>, std::string> get_from_interface(std::string_view iface_name) {
    unsigned int iface_idx = if_nametoindex(iface_name.data());
    if (iface_idx == 0) {
        return std::unexpected(std::string("Interface not found: ") + std::string(iface_name));
    }

    // Initialize SNL (Simple Netlink) state
    struct snl_state ss = {};
    if (!snl_init(&ss, NETLINK_ROUTE)) {
        return std::unexpected("snl_init(NETLINK_ROUTE) failed");
    }

    // Build RTM_GETADDR dump request
    struct snl_writer nw = {};
    snl_init_writer(&ss, &nw);
    struct nlmsghdr *hdr = snl_create_msg_request(&nw, RTM_GETADDR);
    if (hdr == nullptr) {
        snl_free(&ss);
        return std::unexpected("snl_create_msg_request failed");
    }
    hdr->nlmsg_flags |= NLM_F_DUMP;
    snl_reserve_msg_object(&nw, struct ifaddrmsg);

    hdr = snl_finalize_msg(&nw);
    if (hdr == nullptr || !snl_send_message(&ss, hdr)) {
        snl_free(&ss);
        return std::unexpected("RTM_GETADDR send failed");
    }

    uint32_t nlmsg_seq = hdr->nlmsg_seq;
    struct snl_errmsg_data e = {};
    std::vector<IPv6Info> result;

    while ((hdr = snl_read_reply_multi(&ss, nlmsg_seq, &e)) != nullptr) {
        struct snl_parsed_addr addr;
        memset(&addr, 0, sizeof(addr));

        if (!snl_parse_nlmsg(&ss, hdr, &snl_rtm_addr_parser, &addr)) {
            continue;
        }
        if (addr.ifa_family != AF_INET6) {
            continue;
        }
        if (addr.ifa_index != iface_idx) {
            continue;
        }

        // Determine which sockaddr to use (ifa_local is preferred)
        struct sockaddr_in6 *sin6;
        if (addr.ifa_local != nullptr) {
            sin6 = reinterpret_cast<struct sockaddr_in6 *>(addr.ifa_local);
        } else if (addr.ifa_address != nullptr) {
            sin6 = reinterpret_cast<struct sockaddr_in6 *>(addr.ifa_address);
        } else {
            continue;
        }

        // Skip loopback and link-local
        if (IN6_IS_ADDR_LOOPBACK(&sin6->sin6_addr)) {
            continue;
        }
        if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr)) {
            continue;
        }

        // Convert address to string
        char addr_str[INET6_ADDRSTRLEN];
        if (inet_ntop(AF_INET6, &sin6->sin6_addr, addr_str, sizeof(addr_str)) == nullptr) {
            continue;
        }

        // Extract lifetimes from cacheinfo
        // Netlink cacheinfo: ifa_prefered/ifa_valid are remaining seconds
        // at measurement time (tstamp is CLOCK_MONOTONIC ms).
        long preferred_lft = static_cast<long>(1e12);
        long valid_lft = static_cast<long>(1e12);

        if (addr.ifa_cacheinfo != nullptr) {
            auto *ci = addr.ifa_cacheinfo;
            if (ci->ifa_prefered != 0 && ci->ifa_prefered != ND6_INFINITE_LIFETIME) {
                preferred_lft = static_cast<long>(ci->ifa_prefered);
            } else if (ci->ifa_prefered == 0) {
                preferred_lft = 0;  // expired / no preferred lifetime
            }
            if (ci->ifa_valid != 0 && ci->ifa_valid != ND6_INFINITE_LIFETIME) {
                valid_lft = static_cast<long>(ci->ifa_valid);
            } else if (ci->ifa_valid == 0) {
                valid_lft = 0;  // expired / no valid lifetime
            }
        }

        bool deprecated = (addr.ifaf_flags & IN6_IFF_DEPRECATED) != 0;

        IPv6Info info;
        info.ip = addr_str;
        info.preferred_lft = preferred_lft;
        info.valid_lft = valid_lft;
        info.is_deprecated = deprecated;
        populate_info(&info);

        if (info.is_candidate) {
            result.push_back(info);
        }
    }

    snl_free(&ss);

    if (result.empty()) {
        return std::unexpected("No suitable IPv6 address on interface " + std::string(iface_name));
    }

    return result;
}

} // namespace ip_getter

#endif // __FreeBSD__
