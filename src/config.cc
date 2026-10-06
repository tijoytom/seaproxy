#include "config.hh"

#include <toml++/toml.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace seaproxy {
namespace {

std::filesystem::path resolve_path(
        const std::filesystem::path& config_path,
        const std::string& configured_path) {
    auto path = std::filesystem::path(configured_path);
    if (path.is_relative()) {
        path = config_path.parent_path() / path;
    }
    return path;
}

std::string read_password_file(
        const std::filesystem::path& config_path,
        const std::string& configured_path,
        std::string_view owner) {
    if (configured_path.empty()) {
        return {};
    }
    const auto path = resolve_path(config_path, configured_path);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::invalid_argument(
                "cannot open password file for " +
                std::string(owner) + ": " + path.string());
    }
    std::string password{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
    while (!password.empty() &&
           (password.back() == '\n' || password.back() == '\r')) {
        password.pop_back();
    }
    if (password.empty()) {
        throw std::invalid_argument(
                "password file for " + std::string(owner) +
                " must contain a non-empty password");
    }
    return password;
}

std::string optional_string(
        const toml::table& table,
        std::string_view key,
        std::string fallback) {
    return table[key].value_or(std::move(fallback));
}

std::size_t size_value(
        const toml::table& table,
        std::string_view key,
        std::size_t fallback,
        std::string_view owner,
        bool allow_zero) {
    const auto value = table[key].value<std::int64_t>();
    if (!value) {
        return fallback;
    }
    if (*value < 0 || (!allow_zero && *value == 0) ||
        static_cast<std::uint64_t>(*value) >
                std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument(
                std::string(owner) + "." + std::string(key) +
                (allow_zero
                         ? " must be a non-negative integer"
                         : " must be a positive integer"));
    }
    return static_cast<std::size_t>(*value);
}

std::uint16_t port(
        const toml::table& table,
        std::string_view key,
        std::uint16_t fallback,
        std::string_view owner) {
    const auto value = table[key].value<std::int64_t>();
    if (!value) {
        return fallback;
    }
    if (*value <= 0 || *value > 65535) {
        throw std::invalid_argument(
                std::string(owner) + "." + std::string(key) +
                " must be in 1..65535");
    }
    return static_cast<std::uint16_t>(*value);
}

RedisMode redis_mode(std::string_view value) {
    if (value == "auto") {
        return RedisMode::auto_detect;
    }
    if (value == "standalone") {
        return RedisMode::standalone;
    }
    if (value == "cluster") {
        return RedisMode::cluster;
    }
    throw std::invalid_argument(
            "redis.mode must be auto, standalone, or cluster");
}

}  // namespace

Config load_config(const std::string& path) {
    toml::table document;
    try {
        document = toml::parse_file(path);
    } catch (const toml::parse_error& error) {
        throw std::invalid_argument(
                "cannot parse config file " + path +
                ": " + std::string(error.description()));
    }

    Config config;
    if (const auto* listener = document["listener"].as_table()) {
        config.listen_address = optional_string(
                *listener, "address", config.listen_address);
        config.listen_port = port(
                *listener, "port", config.listen_port, "listener");
        config.max_clients_per_shard = size_value(
                *listener,
                "max_clients_per_shard",
                config.max_clients_per_shard,
                "listener",
                false);
    }
    if (const auto* redis = document["redis"].as_table()) {
        config.redis_address = optional_string(
                *redis, "address", config.redis_address);
        config.redis_port = port(
                *redis, "port", config.redis_port, "redis");
        config.redis_mode = redis_mode(optional_string(
                *redis, "mode", "auto"));
        config.redis_username = optional_string(
                *redis, "username", config.redis_username);
        config.redis_tls = redis->get("tls")
                ? redis->get("tls")->value_or(config.redis_tls)
                : config.redis_tls;
        config.redis_tls_ca_file = optional_string(
                *redis, "tls_ca_file", config.redis_tls_ca_file);
        config.redis_tls_server_name = optional_string(
                *redis,
                "tls_server_name",
                config.redis_tls_server_name);
        config.redis_password = read_password_file(
                path,
                optional_string(*redis, "password_file", {}),
                "redis");
    }
    if (const auto* frontend = document["frontend"].as_table()) {
        config.frontend_username = optional_string(
                *frontend, "username", config.frontend_username);
        config.frontend_password = read_password_file(
                path,
                optional_string(*frontend, "password_file", {}),
                "frontend");
    }
    if (const auto* pools = document["pools"].as_table()) {
        config.redis_pool_size = size_value(
                *pools,
                "multiplexed_connections_per_shard",
                config.redis_pool_size,
                "pools",
                false);
        config.pipeline_depth = size_value(
                *pools,
                "pipeline_depth",
                config.pipeline_depth,
                "pools",
                false);
        config.private_pool_size = size_value(
                *pools,
                "private_pool_size_per_shard",
                config.private_pool_size,
                "pools",
                true);
        config.private_max_connections = size_value(
                *pools,
                "private_max_connections_per_shard",
                config.private_max_connections,
                "pools",
                false);
        config.worker_queue_capacity = size_value(
                *pools,
                "worker_queue_capacity",
                config.worker_queue_capacity,
                "pools",
                false);
    }
    if (const auto* timeouts = document["timeouts"].as_table()) {
        config.backend_connect_timeout_ms = size_value(
                *timeouts,
                "backend_connect_ms",
                config.backend_connect_timeout_ms,
                "timeouts",
                true);
        config.backend_response_timeout_ms = size_value(
                *timeouts,
                "backend_response_ms",
                config.backend_response_timeout_ms,
                "timeouts",
                true);
        config.private_checkout_timeout_ms = size_value(
                *timeouts,
                "private_checkout_ms",
                config.private_checkout_timeout_ms,
                "timeouts",
                true);
        config.client_idle_timeout_ms = size_value(
                *timeouts,
                "client_idle_ms",
                config.client_idle_timeout_ms,
                "timeouts",
                true);
        config.backend_reconnect_delay_ms = size_value(
                *timeouts,
                "backend_reconnect_delay_ms",
                config.backend_reconnect_delay_ms,
                "timeouts",
                false);
    }

    validate_config(config);
    return config;
}

void validate_config(const Config& config) {
    if (!config.redis_username.empty() &&
        config.redis_password.empty()) {
        throw std::invalid_argument(
                "redis username requires a Redis password file");
    }
    if (!config.redis_tls &&
        (!config.redis_tls_ca_file.empty() ||
         !config.redis_tls_server_name.empty())) {
        throw std::invalid_argument(
                "Redis TLS CA file and server name require Redis TLS");
    }
    if (!config.frontend_username.empty() &&
        config.frontend_password.empty()) {
        throw std::invalid_argument(
                "frontend username requires a frontend password file");
    }
    if (config.redis_pool_size == 0 ||
        config.pipeline_depth == 0 ||
        config.pipeline_depth > 1024 ||
        config.private_max_connections == 0 ||
        config.private_pool_size > config.private_max_connections ||
        config.worker_queue_capacity == 0 ||
        config.max_clients_per_shard == 0 ||
        config.backend_reconnect_delay_ms == 0) {
        throw std::invalid_argument(
                "pool and listener limits must be valid and reconnect delay "
                "must be positive");
    }
}

}  // namespace seaproxy
