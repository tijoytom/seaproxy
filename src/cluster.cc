#include "cluster.hh"

#include "resp.hh"

#include <algorithm>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <variant>

namespace seaproxy::cluster {
namespace {

bool equal_ascii_case(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    return std::equal(
            left.begin(), left.end(), right.begin(),
            [] (unsigned char a, unsigned char b) {
                if (a >= 'a' && a <= 'z') {
                    a -= 'a' - 'A';
                }
                if (b >= 'a' && b <= 'z') {
                    b -= 'a' - 'A';
                }
                return a == b;
            });
}

std::uint16_t crc16(std::string_view value) {
    std::uint16_t crc = 0;
    for (const unsigned char byte : value) {
        crc ^= static_cast<std::uint16_t>(byte) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) != 0
                    ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021)
                    : static_cast<std::uint16_t>(crc << 1);
        }
    }
    return crc;
}

std::string_view hash_tag(std::string_view key) {
    const auto opening = key.find('{');
    if (opening == std::string_view::npos) {
        return key;
    }
    const auto closing = key.find('}', opening + 1);
    if (closing == std::string_view::npos || closing == opening + 1) {
        return key;
    }
    return key.substr(opening + 1, closing - opening - 1);
}

struct RespValue {
    using Array = std::vector<RespValue>;
    std::variant<std::monostate, std::int64_t, std::string_view, Array> value;
};

std::string_view read_line(std::string_view input, std::size_t& position) {
    const auto end = input.find("\r\n", position);
    if (end == std::string_view::npos) {
        throw std::runtime_error("incomplete RESP line");
    }
    auto line = input.substr(position, end - position);
    position = end + 2;
    return line;
}

std::int64_t parse_integer(std::string_view text) {
    std::int64_t value = 0;
    const auto [end, error] =
            std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::runtime_error("invalid RESP integer");
    }
    return value;
}

RespValue parse_value(std::string_view input, std::size_t& position) {
    if (position >= input.size()) {
        throw std::runtime_error("incomplete RESP value");
    }
    const char type = input[position++];
    if (type == ':' || type == '+' || type == '-') {
        auto line = read_line(input, position);
        if (type == ':') {
            return RespValue{parse_integer(line)};
        }
        return RespValue{line};
    }
    if (type == '$') {
        const auto length = parse_integer(read_line(input, position));
        if (length == -1) {
            return RespValue{};
        }
        if (length < 0 ||
            static_cast<std::uint64_t>(length) >
                    input.size() - position) {
            throw std::runtime_error("invalid RESP bulk length");
        }
        auto value = input.substr(position, static_cast<std::size_t>(length));
        position += static_cast<std::size_t>(length);
        if (position + 2 > input.size() ||
            input.substr(position, 2) != "\r\n") {
            throw std::runtime_error("invalid RESP bulk terminator");
        }
        position += 2;
        return RespValue{value};
    }
    if (type == '*') {
        const auto count = parse_integer(read_line(input, position));
        if (count == -1) {
            return RespValue{};
        }
        if (count < 0 || count > 1'000'000) {
            throw std::runtime_error("invalid RESP array length");
        }
        RespValue::Array values;
        values.reserve(static_cast<std::size_t>(count));
        for (std::int64_t index = 0; index < count; ++index) {
            values.push_back(parse_value(input, position));
        }
        return RespValue{std::move(values)};
    }
    throw std::runtime_error("unsupported RESP type in CLUSTER SLOTS");
}

const RespValue::Array& as_array(const RespValue& value) {
    const auto* array = std::get_if<RespValue::Array>(&value.value);
    if (!array) {
        throw std::runtime_error("expected RESP array");
    }
    return *array;
}

std::int64_t as_integer(const RespValue& value) {
    const auto* integer = std::get_if<std::int64_t>(&value.value);
    if (!integer) {
        throw std::runtime_error("expected RESP integer");
    }
    return *integer;
}

std::string_view as_string(const RespValue& value) {
    const auto* string = std::get_if<std::string_view>(&value.value);
    if (!string) {
        throw std::runtime_error("expected RESP string");
    }
    return *string;
}

bool is_no_key_command(std::string_view command) {
    static constexpr std::string_view commands[] = {
            "COMMAND", "DBSIZE", "ECHO", "INFO", "LASTSAVE", "PING",
            "ROLE", "TIME",
    };
    return std::ranges::any_of(commands, [command] (auto candidate) {
        return equal_ascii_case(command, candidate);
    });
}

bool is_all_key_arguments(std::string_view command) {
    static constexpr std::string_view commands[] = {
            "DEL", "EXISTS", "MGET", "PFCOUNT", "PFMERGE", "SDIFF",
            "SDIFFSTORE", "SINTER", "SINTERSTORE", "SUNION",
            "SUNIONSTORE", "TOUCH", "UNLINK", "WATCH",
    };
    return std::ranges::any_of(commands, [command] (auto candidate) {
        return equal_ascii_case(command, candidate);
    });
}

bool is_first_key_command(std::string_view command) {
    static constexpr std::string_view commands[] = {
            "APPEND", "BITCOUNT", "BITFIELD", "BITFIELD_RO", "BITPOS",
            "BLMOVE", "BLPOP", "BRPOP", "BRPOPLPUSH", "DECR", "DECRBY",
            "DUMP", "EXPIRE", "EXPIREAT", "EXPIRETIME", "GET", "GETDEL",
            "GETEX", "GETRANGE", "GETSET", "HDEL", "HEXISTS", "HGET",
            "HGETALL", "HINCRBY", "HINCRBYFLOAT", "HKEYS", "HLEN",
            "HMGET", "HMSET", "HRANDFIELD", "HSCAN", "HSET", "HSETNX",
            "HSTRLEN", "HVALS", "INCR", "INCRBY", "INCRBYFLOAT", "LINDEX",
            "LINSERT", "LLEN", "LMOVE", "LPOP", "LPOS", "LPUSH",
            "LPUSHX", "LRANGE", "LREM", "LSET", "LTRIM", "PERSIST",
            "PEXPIRE", "PEXPIREAT", "PEXPIRETIME", "PFADD", "PFCOUNT",
            "PTTL", "RENAME", "RENAMENX", "RESTORE", "RPOP", "RPOPLPUSH",
            "RPUSH", "RPUSHX", "SADD", "SCARD", "SDIFF", "SET", "SETEX",
            "SETNX", "SETRANGE", "SINTER", "SISMEMBER", "SMEMBERS",
            "SMISMEMBER", "SMOVE", "SPOP", "SRANDMEMBER", "SREM", "SSCAN",
            "STRLEN", "SUNION", "TTL", "TYPE", "XACK", "XADD", "XAUTOCLAIM",
            "XCLAIM", "XDEL", "XGROUP", "XINFO", "XLEN", "XPENDING",
            "XRANGE", "XREAD", "XREADGROUP", "XREVRANGE", "XSETID", "XTRIM",
            "ZADD", "ZCARD", "ZCOUNT", "ZINCRBY", "ZLEXCOUNT", "ZMPOP",
            "ZMSCORE", "ZPOPMAX", "ZPOPMIN", "ZRANDMEMBER", "ZRANGE",
            "ZRANGEBYLEX", "ZRANGEBYSCORE", "ZRANK", "ZREM",
            "ZREMRANGEBYLEX", "ZREMRANGEBYRANK", "ZREMRANGEBYSCORE",
            "ZREVRANGE", "ZREVRANGEBYLEX", "ZREVRANGEBYSCORE", "ZREVRANK",
            "ZSCAN", "ZSCORE",
    };
    return std::ranges::any_of(commands, [command] (auto candidate) {
        return equal_ascii_case(command, candidate);
    });
}

Route route_keys(const std::vector<std::string_view>& keys) {
    if (keys.empty()) {
        return {RouteStatus::no_key, std::nullopt};
    }
    const auto slot = key_slot(keys.front());
    for (const auto key : keys) {
        if (key_slot(key) != slot) {
            return {RouteStatus::cross_slot, std::nullopt};
        }
    }
    return {RouteStatus::ok, slot};
}

std::optional<std::size_t> argument_count(std::string_view text) {
    std::size_t count = 0;
    const auto [end, error] =
            std::from_chars(text.data(), text.data() + text.size(), count);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return count;
}

Route route_range(
        const std::vector<std::string_view>& arguments,
        std::size_t first,
        std::size_t count) {
    if (first > arguments.size() || count > arguments.size() - first) {
        return {RouteStatus::invalid, std::nullopt};
    }
    return route_keys(std::vector<std::string_view>(
            arguments.begin() + first,
            arguments.begin() + first + count));
}

}  // namespace

std::uint16_t key_slot(std::string_view key) {
    return crc16(hash_tag(key)) % slot_count;
}

Route route_request(std::string_view request) {
    const auto parsed = resp::command_arguments(request);
    if (!parsed || parsed->empty()) {
        return {RouteStatus::invalid, std::nullopt};
    }
    const auto command = parsed->front();
    if (is_no_key_command(command)) {
        return {RouteStatus::no_key, std::nullopt};
    }
    if (is_all_key_arguments(command)) {
        return route_keys(std::vector<std::string_view>(
                parsed->begin() + 1, parsed->end()));
    }
    if (equal_ascii_case(command, "MSET") ||
        equal_ascii_case(command, "MSETNX")) {
        if (parsed->size() < 3 || parsed->size() % 2 == 0) {
            return {RouteStatus::invalid, std::nullopt};
        }
        std::vector<std::string_view> keys;
        for (std::size_t index = 1; index < parsed->size(); index += 2) {
            keys.push_back((*parsed)[index]);
        }
        return route_keys(keys);
    }
    static constexpr std::string_view two_key_commands[] = {
            "BLMOVE", "BRPOPLPUSH", "COPY", "LMOVE", "RENAME",
            "RENAMENX", "RPOPLPUSH",
    };
    if (std::ranges::any_of(
                two_key_commands, [command] (auto candidate) {
                    return equal_ascii_case(command, candidate);
                })) {
        return route_range(*parsed, 1, 2);
    }
    if (equal_ascii_case(command, "SMOVE")) {
        return route_range(*parsed, 1, 2);
    }
    if (equal_ascii_case(command, "BITOP")) {
        if (parsed->size() < 4) {
            return {RouteStatus::invalid, std::nullopt};
        }
        return route_range(*parsed, 2, parsed->size() - 2);
    }
    if (equal_ascii_case(command, "BLPOP") ||
        equal_ascii_case(command, "BRPOP") ||
        equal_ascii_case(command, "BZPOPMAX") ||
        equal_ascii_case(command, "BZPOPMIN")) {
        if (parsed->size() < 3) {
            return {RouteStatus::invalid, std::nullopt};
        }
        return route_range(*parsed, 1, parsed->size() - 2);
    }
    if (equal_ascii_case(command, "ZDIFF") ||
        equal_ascii_case(command, "ZINTER") ||
        equal_ascii_case(command, "ZUNION") ||
        equal_ascii_case(command, "SINTERCARD")) {
        if (parsed->size() < 2) {
            return {RouteStatus::invalid, std::nullopt};
        }
        const auto count = argument_count((*parsed)[1]);
        return count
                ? route_range(*parsed, 2, *count)
                : Route{RouteStatus::invalid, std::nullopt};
    }
    if (equal_ascii_case(command, "ZDIFFSTORE") ||
        equal_ascii_case(command, "ZINTERSTORE") ||
        equal_ascii_case(command, "ZUNIONSTORE")) {
        if (parsed->size() < 3) {
            return {RouteStatus::invalid, std::nullopt};
        }
        const auto count = argument_count((*parsed)[2]);
        if (!count) {
            return {RouteStatus::invalid, std::nullopt};
        }
        auto keys = std::vector<std::string_view>{(*parsed)[1]};
        if (3 + *count > parsed->size()) {
            return {RouteStatus::invalid, std::nullopt};
        }
        keys.insert(
                keys.end(), parsed->begin() + 3,
                parsed->begin() + 3 + *count);
        return route_keys(keys);
    }
    if (equal_ascii_case(command, "LMPOP") ||
        equal_ascii_case(command, "ZMPOP")) {
        if (parsed->size() < 2) {
            return {RouteStatus::invalid, std::nullopt};
        }
        const auto count = argument_count((*parsed)[1]);
        return count
                ? route_range(*parsed, 2, *count)
                : Route{RouteStatus::invalid, std::nullopt};
    }
    if (equal_ascii_case(command, "BLMPOP") ||
        equal_ascii_case(command, "BZMPOP")) {
        if (parsed->size() < 3) {
            return {RouteStatus::invalid, std::nullopt};
        }
        const auto count = argument_count((*parsed)[2]);
        return count
                ? route_range(*parsed, 3, *count)
                : Route{RouteStatus::invalid, std::nullopt};
    }
    if (equal_ascii_case(command, "XREAD") ||
        equal_ascii_case(command, "XREADGROUP")) {
        auto streams = std::ranges::find_if(
                *parsed, [] (std::string_view argument) {
                    return equal_ascii_case(argument, "STREAMS");
                });
        if (streams == parsed->end()) {
            return {RouteStatus::invalid, std::nullopt};
        }
        const auto first = static_cast<std::size_t>(
                std::distance(parsed->begin(), streams)) + 1;
        const auto remaining = parsed->size() - first;
        if (remaining == 0 || remaining % 2 != 0) {
            return {RouteStatus::invalid, std::nullopt};
        }
        return route_range(*parsed, first, remaining / 2);
    }
    if (equal_ascii_case(command, "EVAL") ||
        equal_ascii_case(command, "EVALSHA") ||
        equal_ascii_case(command, "FCALL") ||
        equal_ascii_case(command, "FCALL_RO")) {
        if (parsed->size() < 3) {
            return {RouteStatus::invalid, std::nullopt};
        }
        const auto count = argument_count((*parsed)[2]);
        if (!count || *count > parsed->size() - 3) {
            return {RouteStatus::invalid, std::nullopt};
        }
        return route_range(*parsed, 3, *count);
    }
    if (is_first_key_command(command)) {
        if (parsed->size() < 2) {
            return {RouteStatus::invalid, std::nullopt};
        }
        return {RouteStatus::ok, key_slot((*parsed)[1])};
    }
    return {RouteStatus::unsupported, std::nullopt};
}

Topology parse_cluster_slots(
        std::string_view response,
        std::string_view fallback_address) {
    std::size_t position = 0;
    const auto root = parse_value(response, position);
    if (position != response.size()) {
        throw std::runtime_error("trailing data after CLUSTER SLOTS response");
    }

    Topology topology;
    topology.slots.fill(std::numeric_limits<std::size_t>::max());
    std::unordered_map<std::string, std::size_t> node_indices;
    for (const auto& range_value : as_array(root)) {
        const auto& range = as_array(range_value);
        if (range.size() < 3) {
            throw std::runtime_error("invalid CLUSTER SLOTS range");
        }
        const auto first = as_integer(range[0]);
        const auto last = as_integer(range[1]);
        if (first < 0 || last < first ||
            last >= static_cast<std::int64_t>(slot_count)) {
            throw std::runtime_error("invalid CLUSTER SLOTS boundaries");
        }
        const auto& primary = as_array(range[2]);
        if (primary.size() < 2) {
            throw std::runtime_error("invalid CLUSTER SLOTS primary");
        }
        auto address = std::string(as_string(primary[0]));
        if (address.empty()) {
            address = fallback_address;
        }
        const auto port = as_integer(primary[1]);
        if (port <= 0 || port > 65535) {
            throw std::runtime_error("invalid CLUSTER SLOTS port");
        }
        const auto key = address + ":" + std::to_string(port);
        auto [entry, inserted] =
                node_indices.emplace(key, topology.primaries.size());
        if (inserted) {
            topology.primaries.push_back(
                    Endpoint{std::move(address),
                             static_cast<std::uint16_t>(port)});
        }
        for (auto slot = first; slot <= last; ++slot) {
            auto& mapped = topology.slots[static_cast<std::size_t>(slot)];
            if (mapped != std::numeric_limits<std::size_t>::max()) {
                throw std::runtime_error("duplicate CLUSTER SLOTS mapping");
            }
            mapped = entry->second;
        }
    }
    if (topology.primaries.empty() ||
        std::ranges::any_of(topology.slots, [] (std::size_t node) {
            return node == std::numeric_limits<std::size_t>::max();
        })) {
        throw std::runtime_error("CLUSTER SLOTS does not cover all slots");
    }
    return topology;
}

}  // namespace seaproxy::cluster
