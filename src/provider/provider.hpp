#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <variant>

namespace provider {

struct CloudflareOpts {
    bool proxied = false;
    std::string zone_id;
    std::string type = "AAAA";
};

struct AliyunOpts {
    std::string type = "AAAA";
};

using ProviderOpts = std::variant<CloudflareOpts, AliyunOpts>;

template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

/// Abstract DNS provider. One process updates every record with the same address.
class DNSProvider {
public:
    virtual ~DNSProvider() = default;

    virtual std::string name() const = 0;

    /// Create or update an AAAA record. Pass the alternative that matches the provider.
    [[nodiscard]] virtual std::expected<void, std::string>
    upsert_record(std::string_view zone,
                  std::string_view record_name,
                  std::string_view ip,
                  int ttl,
                  const ProviderOpts& opts) = 0;
};

} // namespace provider
