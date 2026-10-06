#pragma once

#include "config.hh"

#include <seastar/core/future.hh>
#include <seastar/net/api.hh>

#include <memory>

namespace seaproxy {

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
