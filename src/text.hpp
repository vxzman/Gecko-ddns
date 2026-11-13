#pragma once

#include <ranges>
#include <string_view>

namespace text {

/// Leading and trailing ASCII whitespace removed. The view aliases `in`.
[[nodiscard]] inline std::string_view trim(std::string_view in) {
    constexpr auto is_space = [](char c) noexcept {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
    };
    const auto leading = std::ranges::distance(in | std::views::take_while(is_space));
    const auto trailing = std::ranges::distance(in | std::views::reverse | std::views::take_while(is_space));
    if (leading < 0 || trailing < 0) return {};
    const auto lead = static_cast<std::size_t>(leading);
    const auto trail = static_cast<std::size_t>(trailing);
    if (lead >= in.size()) return {};
    const auto rest = in.size() - lead;
    if (trail >= rest) return {};
    return in.substr(lead, rest - trail);
}

} // namespace text
