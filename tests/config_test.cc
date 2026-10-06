#include "config.hh"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <unistd.h>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        auto pattern =
                (std::filesystem::temp_directory_path() /
                 "seaproxy-config-XXXXXX").string();
        pattern.push_back('\0');
        const auto* created = mkdtemp(pattern.data());
        if (!created) {
            throw std::runtime_error(
                    "failed to create temporary directory");
        }
        _path = created;
    }

    ~TemporaryDirectory() {
        std::filesystem::remove_all(_path);
    }

    const std::filesystem::path& path() const {
        return _path;
    }

private:
    std::filesystem::path _path;
};

void write_file(
        const std::filesystem::path& path,
        const std::string& contents) {
    std::ofstream output(path);
    output << contents;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace seaproxy;

    assert(argc == 2);
    const Config defaults;
    const auto loaded_defaults = load_config(argv[1]);
    assert(loaded_defaults.listen_address == defaults.listen_address);
    assert(loaded_defaults.listen_port == defaults.listen_port);
    assert(loaded_defaults.redis_address == defaults.redis_address);
    assert(loaded_defaults.redis_port == defaults.redis_port);
    assert(loaded_defaults.redis_username == defaults.redis_username);
    assert(loaded_defaults.redis_password == defaults.redis_password);
    assert(loaded_defaults.redis_tls == defaults.redis_tls);
    assert(loaded_defaults.redis_tls_ca_file == defaults.redis_tls_ca_file);
    assert(loaded_defaults.redis_tls_server_name ==
            defaults.redis_tls_server_name);
    assert(loaded_defaults.frontend_username ==
            defaults.frontend_username);
    assert(loaded_defaults.frontend_password ==
            defaults.frontend_password);
    assert(loaded_defaults.redis_pool_size == defaults.redis_pool_size);
    assert(loaded_defaults.pipeline_depth == defaults.pipeline_depth);
    assert(loaded_defaults.private_pool_size == defaults.private_pool_size);
    assert(loaded_defaults.private_max_connections ==
            defaults.private_max_connections);
    assert(loaded_defaults.worker_queue_capacity ==
            defaults.worker_queue_capacity);
    assert(loaded_defaults.max_clients_per_shard ==
            defaults.max_clients_per_shard);
    assert(loaded_defaults.backend_connect_timeout_ms ==
            defaults.backend_connect_timeout_ms);
    assert(loaded_defaults.backend_response_timeout_ms ==
            defaults.backend_response_timeout_ms);
    assert(loaded_defaults.private_checkout_timeout_ms ==
            defaults.private_checkout_timeout_ms);
    assert(loaded_defaults.client_idle_timeout_ms ==
            defaults.client_idle_timeout_ms);
    assert(loaded_defaults.backend_reconnect_delay_ms ==
            defaults.backend_reconnect_delay_ms);
    assert(loaded_defaults.redis_mode == defaults.redis_mode);

    TemporaryDirectory directory;
    write_file(directory.path() / "redis-password", "back-secret\n");
    write_file(directory.path() / "frontend-password", "front-secret\r\n");
    const auto config_path = directory.path() / "config.toml";
    write_file(
            config_path,
            "[listener]\n"
            "address = \"127.0.0.1\"\n"
            "port = 7100\n"
            "max_clients_per_shard = 500\n"
            "\n"
            "[redis]\n"
            "address = \"redis.internal\"\n"
            "port = 6380\n"
            "mode = \"cluster\"\n"
            "username = \"service\"\n"
            "password_file = \"redis-password\"\n"
            "tls = true\n"
            "tls_ca_file = \"/etc/ssl/redis-ca.pem\"\n"
            "tls_server_name = \"redis.example.com\"\n"
            "\n"
            "[frontend]\n"
            "username = \"application\"\n"
            "password_file = \"frontend-password\"\n"
            "\n"
            "[pools]\n"
            "multiplexed_connections_per_shard = 3\n"
            "pipeline_depth = 32\n"
            "private_pool_size_per_shard = 4\n"
            "private_max_connections_per_shard = 20\n"
            "worker_queue_capacity = 200\n"
            "\n"
            "[timeouts]\n"
            "backend_connect_ms = 1500\n"
            "backend_response_ms = 2500\n"
            "private_checkout_ms = 3500\n"
            "client_idle_ms = 4500\n"
            "backend_reconnect_delay_ms = 500\n");

    const auto config = load_config(config_path);
    assert(config.listen_address == "127.0.0.1");
    assert(config.listen_port == 7100);
    assert(config.max_clients_per_shard == 500);
    assert(config.redis_address == "redis.internal");
    assert(config.redis_port == 6380);
    assert(config.redis_mode == RedisMode::cluster);
    assert(config.redis_username == "service");
    assert(config.redis_password == "back-secret");
    assert(config.redis_tls);
    assert(config.redis_tls_ca_file == "/etc/ssl/redis-ca.pem");
    assert(config.redis_tls_server_name == "redis.example.com");
    assert(config.frontend_username == "application");
    assert(config.frontend_password == "front-secret");
    assert(config.redis_pool_size == 3);
    assert(config.pipeline_depth == 32);
    assert(config.private_pool_size == 4);
    assert(config.private_max_connections == 20);
    assert(config.worker_queue_capacity == 200);
    assert(config.backend_connect_timeout_ms == 1500);
    assert(config.backend_response_timeout_ms == 2500);
    assert(config.private_checkout_timeout_ms == 3500);
    assert(config.client_idle_timeout_ms == 4500);
    assert(config.backend_reconnect_delay_ms == 500);

    const auto invalid_path = directory.path() / "invalid.toml";
    write_file(
            invalid_path,
            "[timeouts]\n"
            "backend_reconnect_delay_ms = 0\n");
    bool rejected = false;
    try {
        static_cast<void>(load_config(invalid_path));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}
