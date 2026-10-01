#pragma once

#include <seastar/core/future.hh>
#include <seastar/net/api.hh>

#include <cstddef>
#include <cstdint>
#include <memory>
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
    std::string frontend_username;
    std::string frontend_password;
    std::size_t redis_pool_size{1};
    std::size_t pipeline_depth{64};
    std::size_t private_pool_size{10};
    std::size_t private_max_connections{64};
    std::size_t worker_queue_capacity{1024};
    std::size_t max_clients_per_shard{10000};
    RedisMode redis_mode{RedisMode::auto_detect};
};

seastar::future<RedisMode> detect_redis_mode(const Config& config);

class ProxyService {
public:
    explicit ProxyService(Config config);
    ~ProxyService();

    seastar::future<> start();
    seastar::future<> stop();

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

}  // namespace seaproxy
