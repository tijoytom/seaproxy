#include "cluster.hh"
#include "resp.hh"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    using seaproxy::resp::command_argument;
    using seaproxy::resp::command_arguments;
    using seaproxy::resp::command_name;
    using seaproxy::resp::frame_length;

    const std::string nested =
            "*2\r\n$3\r\nGET\r\n*2\r\n:1\r\n$3\r\ntwo\r\n";
    require(frame_length(nested) == nested.size(), "nested frame length");
    require(!frame_length("*2\r\n$3\r\nGET\r\n$4\r\nke"), "partial frame");

    bool rejected = false;
    try {
        static_cast<void>(frame_length("$3\r\nfooXX"));
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "invalid blob terminator");

    const std::string request = "*2\r\n$3\r\nget\r\n$3\r\nkey\r\n";
    require(command_name(request) == "get", "array command name");
    require(command_argument(request, 1) == "key", "array command argument");
    require(command_arguments(request)->size() == 2, "array command arguments");
    require(command_name("ping\r\n") == "ping", "inline command name");

    const std::string first = "*1\r\n$4\r\nPING\r\n";
    require(frame_length(first + request) == first.size(), "first pipelined frame");

    using seaproxy::cluster::RouteStatus;
    using seaproxy::cluster::key_slot;
    using seaproxy::cluster::parse_cluster_slots;
    using seaproxy::cluster::route_request;

    require(key_slot("foo{shared}1") == key_slot("bar{shared}2"),
            "cluster hash tags");
    require(key_slot("123456789") == 12739, "Redis CRC16 test vector");
    require(route_request(request).status == RouteStatus::ok,
            "route single-key command");
    const std::string same_slot =
            "*3\r\n$4\r\nMGET\r\n$7\r\n{a}:one\r\n$7\r\n{a}:two\r\n";
    require(route_request(same_slot).status == RouteStatus::ok,
            "route same-slot multi-key command");
    const std::string cross_slot =
            "*3\r\n$4\r\nMGET\r\n$3\r\none\r\n$3\r\ntwo\r\n";
    require(route_request(cross_slot).status == RouteStatus::cross_slot,
            "reject cross-slot command");
    const std::string rename_cross_slot =
            "*3\r\n$6\r\nRENAME\r\n$3\r\none\r\n$3\r\ntwo\r\n";
    require(route_request(rename_cross_slot).status == RouteStatus::cross_slot,
            "reject cross-slot two-key command");
    const std::string blocking_cross_slot =
            "*4\r\n$5\r\nBLPOP\r\n$3\r\none\r\n$3\r\ntwo\r\n$1\r\n0\r\n";
    require(route_request(blocking_cross_slot).status == RouteStatus::cross_slot,
            "reject cross-slot blocking command");

    std::string slots =
            "*1\r\n"
            "*3\r\n"
            ":0\r\n"
            ":16383\r\n"
            "*3\r\n"
            "$9\r\n127.0.0.1\r\n"
            ":7001\r\n"
            "$6\r\nnode-1\r\n";
    const auto topology = parse_cluster_slots(slots, "127.0.0.1");
    require(topology.primaries.size() == 1, "cluster primary count");
    require(topology.slots[0] == 0 && topology.slots[16383] == 0,
            "cluster slot map");
}
