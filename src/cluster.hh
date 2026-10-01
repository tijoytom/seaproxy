#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace seaproxy::cluster {

constexpr std::size_t slot_count = 16384;

struct Endpoint {
    std::string address;
    std::uint16_t port;

    bool operator==(const Endpoint&) const = default;
};

struct Topology {
    std::vector<Endpoint> primaries;
    std::array<std::size_t, slot_count> slots;
};

enum class RouteStatus {
    ok,
    no_key,
    cross_slot,
    unsupported,
    invalid,
};

struct Route {
    RouteStatus status;
    std::optional<std::uint16_t> slot;
};

std::uint16_t key_slot(std::string_view key);
Route route_request(std::string_view request);
Topology parse_cluster_slots(
        std::string_view response,
        std::string_view fallback_address);

}  // namespace seaproxy::cluster
