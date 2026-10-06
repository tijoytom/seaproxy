#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace seaproxy {

enum class RedisMode {
    auto_detect,
    standalone,
    cluster,
};

struct Config {
    std::string listen_address{"0.0.0.0"};
    std::uint16_t listen_port{7000};
    std::string redis_address{"127.0.0.1"};
    std::uint16_t redis_port{6379};
    std::string redis_username;
    std::string redis_password;
    bool redis_tls{true};
    std::string redis_tls_ca_file;
    std::string redis_tls_server_name;
    std::string frontend_username;
    std::string frontend_password;
    std::size_t redis_pool_size{1};
    std::size_t pipeline_depth{64};
    std::size_t private_pool_size{10};
    std::size_t private_max_connections{64};
    std::size_t worker_queue_capacity{1024};
    std::size_t max_clients_per_shard{10000};
    std::size_t backend_connect_timeout_ms{0};
    std::size_t backend_response_timeout_ms{0};
    std::size_t private_checkout_timeout_ms{0};
    std::size_t client_idle_timeout_ms{0};
    std::size_t backend_reconnect_delay_ms{100};
    RedisMode redis_mode{RedisMode::auto_detect};
};

Config load_config(const std::string& path);
void validate_config(const Config& config);

}  // namespace seaproxy
