#include "netns_probe.hpp"

#include <iostream>
#include <print>
#include <string>
#include <string_view>

namespace {

int g_failed = 0;

void expect(bool cond, std::string_view name) {
    if (!cond) {
        std::println(stderr, "FAIL {}", name);
        ++g_failed;
    }
}

} // namespace

int main() {
    const auto vpn = netns::resolve_netns_path("vpn");
    expect(vpn.has_value() && *vpn == "/run/netns/vpn", "name vpn");

    const auto dotted = netns::resolve_netns_path("ok_name-1.2");
    expect(dotted.has_value() && *dotted == "/run/netns/ok_name-1.2", "dotted name");

    const auto absolute = netns::resolve_netns_path("/run/netns/vpn");
    expect(absolute.has_value() && *absolute == "/run/netns/vpn", "absolute vpn");

    expect(!netns::resolve_netns_path("../vpn").has_value(), "reject relative dotdot");
    expect(!netns::resolve_netns_path("/run/netns/../etc/passwd").has_value(), "reject escaped absolute");
    expect(!netns::resolve_netns_path("/etc/passwd").has_value(), "reject outside /run/netns");
    expect(!netns::resolve_netns_path("").has_value(), "reject empty");
    expect(!netns::resolve_netns_path("vpn/mv").has_value(), "reject slash");
    expect(!netns::resolve_netns_path("has..dot").has_value(), "reject embedded dotdot");
    expect(!netns::resolve_netns_path(std::string(65, 'a')).has_value(), "reject long name");
    expect(!netns::resolve_netns_path("/run/netns/vpn/extra").has_value(), "reject nested path");

    constexpr std::string_view kSample =
        R"({"ip":"2001:db8::1","source":"interface","iface":"vpn-mv","netns":"vpn","preferred_lft":172594,"valid_lft":258994})";
    const auto parsed = netns::parse_probe_output(kSample);
    expect(parsed.has_value(), "parse sample");
    if (parsed) {
        expect(parsed->ip == "2001:db8::1", "ip");
        expect(parsed->source == "interface", "source");
        expect(parsed->iface == "vpn-mv", "iface");
        expect(parsed->netns == "vpn", "netns");
        expect(parsed->preferred_lft == 172594L, "preferred lifetime");
        expect(parsed->valid_lft == 258994L, "valid lifetime");
        expect(netns::validate_global_ipv6(parsed->ip).has_value(), "sample ip is global");
    }

    expect(!netns::parse_probe_output("not-json").has_value(), "reject bad json");
    expect(!netns::parse_probe_output("{}").has_value(), "reject missing fields");
    expect(!netns::parse_probe_output(
               R"({"ip":"2001:db8::1","source":"other","iface":"vpn-mv","netns":"vpn","preferred_lft":1,"valid_lft":1})")
               .has_value(),
           "reject bad source");

    constexpr std::string_view kApi =
        R"({"ip":"2409:8a00::10","source":"api","iface":"vpn-mv","netns":"vpn","preferred_lft":1000000000000,"valid_lft":1000000000000})";
    const auto api = netns::parse_probe_output(kApi);
    expect(api.has_value() && api->source == "api", "api source");
    expect(api.has_value() && api->preferred_lft == 1000000000000L, "infinite preferred lifetime");
    if (api) expect(netns::validate_global_ipv6(api->ip).has_value(), "2409 is global");

    expect(netns::validate_global_ipv6("2001:db8::1").has_value(), "accept global");
    expect(!netns::validate_global_ipv6("fc00::1").has_value(), "reject ULA fc00");
    expect(!netns::validate_global_ipv6("fd12:3456::1").has_value(), "reject ULA fd00");
    expect(!netns::validate_global_ipv6("fe80::1").has_value(), "reject link-local");
    expect(!netns::validate_global_ipv6("::1").has_value(), "reject loopback");
    expect(!netns::validate_global_ipv6("::ffff:192.0.2.1").has_value(), "reject v4-mapped");
    expect(!netns::validate_global_ipv6("192.0.2.1").has_value(), "reject ipv4");
    expect(!netns::validate_global_ipv6("ff02::1").has_value(), "reject multicast");

    if (g_failed != 0) {
        std::println(stderr, "{} check(s) failed", g_failed);
        return 1;
    }
    std::println("probe_parse_test ok");
    return 0;
}
