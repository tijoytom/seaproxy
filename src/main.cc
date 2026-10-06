#include "proxy.hh"

#include <seastar/core/app-template.hh>
#include <seastar/core/condition-variable.hh>
#include <seastar/core/signal.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/thread.hh>
#include <seastar/util/defer.hh>

#include <boost/program_options.hpp>

#include <csignal>
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
        ("config",
         bpo::value<std::string>()->default_value(""),
         "Path to the required SeaProxy TOML configuration file");

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
        const auto config_path = options["config"].as<std::string>();
        if (config_path.empty()) {
            throw std::invalid_argument("config must be provided");
        }
        auto config = seaproxy::load_config(config_path);

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
