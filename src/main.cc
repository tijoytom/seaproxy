#include "proxy.hh"

#include <seastar/core/app-template.hh>
#include <seastar/core/condition-variable.hh>
#include <seastar/core/signal.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/thread.hh>
#include <seastar/util/defer.hh>

#include <boost/program_options.hpp>

#include <csignal>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

class StopSignal {
public:
    StopSignal() {
        seastar::handle_signal(SIGINT, [this] { signal(); });
        seastar::handle_signal(SIGTERM, [this] { signal(); });
    }

    ~StopSignal() {
        seastar::handle_signal(SIGINT, [] {});
        seastar::handle_signal(SIGTERM, [] {});
    }

    seastar::future<> wait() {
        return _condition.wait([this] { return _signaled; });
    }

private:
    void signal() {
        if (!_signaled) {
            _signaled = true;
            _condition.broadcast();
        }
    }

    bool _signaled{false};
    seastar::condition_variable _condition;
};

std::uint16_t checked_port(
        const boost::program_options::variables_map& options,
        const char* name) {
    const auto value = options[name].as<unsigned>();
    if (value == 0 || value > 65535) {
        throw std::invalid_argument(std::string(name) + " must be in 1..65535");
    }
    return static_cast<std::uint16_t>(value);
}

std::size_t positive_size(
        const boost::program_options::variables_map& options,
        const char* name) {
    const auto value = options[name].as<std::size_t>();
    if (value == 0) {
        throw std::invalid_argument(std::string(name) + " must be greater than zero");
    }
    return value;
}

std::size_t pipeline_depth(
        const boost::program_options::variables_map& options) {
    const auto value = positive_size(options, "pipeline-depth");
    if (value > 1024) {
        throw std::invalid_argument("pipeline-depth must not exceed 1024");
    }
    return value;
}

seaproxy::RedisMode redis_mode(
        const boost::program_options::variables_map& options) {
    const auto value = options["redis-mode"].as<std::string>();
    if (value == "auto") {
        return seaproxy::RedisMode::auto_detect;
    }
    if (value == "standalone") {
        return seaproxy::RedisMode::standalone;
    }
    if (value == "cluster") {
        return seaproxy::RedisMode::cluster;
    }
    throw std::invalid_argument(
            "redis-mode must be auto, standalone, or cluster");
}

std::string read_password_file(
        const boost::program_options::variables_map& options,
        const char* name) {
    const auto path = options[name].as<std::string>();
    if (path.empty()) {
        return {};
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::invalid_argument(
                std::string("cannot open ") + name + ": " + path);
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
                std::string(name) + " must contain a non-empty password");
    }
    return password;
}

bool has_reactor_backend_option(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--reactor-backend" ||
            argument.starts_with("--reactor-backend=")) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    // seastar anyways need boost, so we are using it as 
    // a convenient way to handle program options
    namespace bpo = boost::program_options;

    seastar::app_template::config app_config;
    app_config.name = "seaproxy";
    app_config.description =
            "shard-per-core Redis multiplexing proxy built with Seastar.";
    app_config.auto_handle_sigint_sigterm = false;
    seastar::app_template app(std::move(app_config));

    app.add_options()
        ("listen-address",
         bpo::value<std::string>()->default_value("0.0.0.0"),
         "Address on which the proxy listens")
        ("listen-port",
         bpo::value<unsigned>()->default_value(7000),
         "TCP port on which the proxy listens")
        ("redis-address",
         bpo::value<std::string>()->default_value("127.0.0.1"),
         "Redis backend address")
        ("redis-port",
         bpo::value<unsigned>()->default_value(6379),
         "Redis backend TCP port")
        ("redis-mode",
         bpo::value<std::string>()->default_value("auto"),
         "Redis backend mode: auto, standalone, or cluster")
        ("redis-username",
         bpo::value<std::string>()->default_value(""),
         "Redis backend ACL username; empty uses password-only AUTH")
        ("redis-password-file",
         bpo::value<std::string>()->default_value(""),
         "File containing the Redis backend password")
        ("redis-tls",
         bpo::bool_switch()->default_value(false),
         "Use TLS for Redis backend connections")
        ("redis-tls-ca-file",
         bpo::value<std::string>()->default_value(""),
         "PEM CA file for Redis TLS; empty uses the system trust store")
        ("redis-tls-server-name",
         bpo::value<std::string>()->default_value(""),
         "Redis TLS certificate name; empty uses redis-address")
        ("frontend-username",
         bpo::value<std::string>()->default_value(""),
         "Frontend ACL username; empty accepts password-only AUTH")
        ("frontend-password-file",
         bpo::value<std::string>()->default_value(""),
         "File containing the password required from frontend clients")
        ("redis-pool-size",
         bpo::value<std::size_t>()->default_value(1),
         "Multiplexed Redis connections per shard")
        ("pipeline-depth",
         bpo::value<std::size_t>()->default_value(64),
         "Maximum in-flight requests per multiplexed Redis connection")
        ("private-pool-size",
         bpo::value<std::size_t>()->default_value(10),
         "Preconnected private Redis connections per shard")
        ("private-max-connections",
         bpo::value<std::size_t>()->default_value(64),
         "Hard private Redis connection limit per shard-local pool")
        ("worker-queue-capacity",
         bpo::value<std::size_t>()->default_value(1024),
         "Maximum queued requests per multiplexed Redis connection")
        ("max-clients-per-shard",
         bpo::value<std::size_t>()->default_value(10000),
         "Maximum simultaneously active frontend clients per shard");

    std::vector<std::string> arguments;
    std::vector<char*> argument_pointers;
    if (!has_reactor_backend_option(argc, argv)) {
        arguments.reserve(static_cast<std::size_t>(argc) + 1);
        arguments.emplace_back(argv[0]);
        arguments.emplace_back("--reactor-backend=epoll");
        for (int index = 1; index < argc; ++index) {
            arguments.emplace_back(argv[index]);
        }
        argument_pointers.reserve(arguments.size());
        for (auto& argument : arguments) {
            argument_pointers.push_back(argument.data());
        }
        argc = static_cast<int>(argument_pointers.size());
        argv = argument_pointers.data();
    }

    return app.run(argc, argv, [&app] {
        const auto& options = app.configuration();
        seaproxy::Config config;
        config.listen_address = options["listen-address"].as<std::string>();
        config.listen_port = checked_port(options, "listen-port");
        config.redis_address = options["redis-address"].as<std::string>();
        config.redis_port = checked_port(options, "redis-port");
        config.redis_mode = redis_mode(options);
        config.redis_username =
                options["redis-username"].as<std::string>();
        config.redis_password =
                read_password_file(options, "redis-password-file");
        config.redis_tls = options["redis-tls"].as<bool>();
        config.redis_tls_ca_file =
                options["redis-tls-ca-file"].as<std::string>();
        config.redis_tls_server_name =
                options["redis-tls-server-name"].as<std::string>();
        config.frontend_username =
                options["frontend-username"].as<std::string>();
        config.frontend_password =
                read_password_file(options, "frontend-password-file");
        if (!config.redis_username.empty() &&
            config.redis_password.empty()) {
            throw std::invalid_argument(
                    "redis-username requires redis-password-file");
        }
        if (!config.redis_tls &&
            (!config.redis_tls_ca_file.empty() ||
             !config.redis_tls_server_name.empty())) {
            throw std::invalid_argument(
                    "redis-tls-ca-file and redis-tls-server-name require redis-tls");
        }
        if (!config.frontend_username.empty() &&
            config.frontend_password.empty()) {
            throw std::invalid_argument(
                    "frontend-username requires frontend-password-file");
        }
        config.redis_pool_size = positive_size(options, "redis-pool-size");
        config.pipeline_depth = pipeline_depth(options);
        config.private_pool_size =
                options["private-pool-size"].as<std::size_t>();
        config.private_max_connections =
                positive_size(options, "private-max-connections");
        if (config.private_pool_size > config.private_max_connections) {
            throw std::invalid_argument(
                    "private-pool-size must not exceed private-max-connections");
        }
        config.worker_queue_capacity =
                positive_size(options, "worker-queue-capacity");
        config.max_clients_per_shard =
                positive_size(options, "max-clients-per-shard");

        return seastar::async([config = std::move(config)] () mutable {
            StopSignal stop_signal;
            config.redis_mode =
                    seaproxy::detect_redis_mode(config).get();
            seastar::sharded<seaproxy::ProxyService> services;
            services.start(config).get();
            auto stop_services = seastar::defer([&services] {
                services.stop().get();
            });
            services.invoke_on_all(&seaproxy::ProxyService::start).get();
            stop_signal.wait().get();
        });
    });
}
