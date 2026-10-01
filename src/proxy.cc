#include "proxy.hh"

#include "cluster.hh"
#include "resp.hh"

#include <seastar/core/abort_source.hh>
#include <seastar/core/condition-variable.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/future-util.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/print.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/sleep.hh>
#include <seastar/net/inet_address.hh>

#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <string>
#include <utility>
#include <vector>

namespace seaproxy {
namespace {

using namespace std::chrono_literals;

seastar::socket_address address_of(
        const std::string& address,
        std::uint16_t port) {
    return seastar::make_ipv4_address(seastar::ipv4_addr(address, port));
}

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

std::string exception_message(std::exception_ptr error) {
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& exception) {
        return exception.what();
    } catch (...) {
        return "unknown non-standard exception";
    }
}

bool is_connection_scoped(std::string_view command) {
    static constexpr std::string_view commands[] = {
            "ASKING", "AUTH", "BLMOVE", "BLMPOP", "BLPOP", "BRPOP",
            "BRPOPLPUSH", "BZMPOP", "BZPOPMAX", "BZPOPMIN", "CLIENT",
            "DISCARD", "EXEC", "HELLO", "MONITOR", "MULTI", "PSUBSCRIBE",
            "PUNSUBSCRIBE", "QUIT", "READONLY", "READWRITE", "RESET",
            "SSUBSCRIBE", "SUBSCRIBE", "SUNSUBSCRIBE", "UNSUBSCRIBE",
            "UNWATCH", "WAIT", "WAITAOF", "WATCH", "XREAD", "XREADGROUP",
    };
    return std::ranges::any_of(commands, [command] (std::string_view candidate) {
        return equal_ascii_case(command, candidate);
    });
}

class RespReader {
public:
    explicit RespReader(
            seastar::input_stream<char>& input,
            seastar::sstring initial_buffer = {})
        : _input(input)
        , _buffer(std::move(initial_buffer)) {
    }

    seastar::future<std::optional<seastar::sstring>> read_frame() {
        while (true) {
            const auto buffered =
                    std::string_view(_buffer.data(), _buffer.size());
            if (const auto length = resp::frame_length(buffered)) {
                seastar::sstring frame(_buffer.data(), *length);
                _buffer.erase(_buffer.begin(), _buffer.begin() + *length);
                co_return std::move(frame);
            }
            if (buffered.size() >= resp::max_frame_size) {
                throw std::runtime_error("RESP frame exceeds the 64 MiB limit");
            }

            auto chunk = co_await _input.read_up_to(8192);
            if (chunk.empty()) {
                if (_buffer.empty()) {
                    co_return std::nullopt;
                }
                throw std::runtime_error(
                        "connection closed in the middle of a RESP frame");
            }
            _buffer.append(chunk.get(), chunk.size());
            if (_buffer.size() > resp::max_frame_size) {
                throw std::runtime_error("RESP frame exceeds the 64 MiB limit");
            }
        }
    }

    seastar::future<std::optional<seastar::sstring>> read_response() {
        auto first = co_await read_frame();
        if (!first) {
            co_return std::nullopt;
        }
        if (first->empty() || first->front() != '|') {
            co_return std::move(first);
        }

        auto response = std::move(*first);
        while (true) {
            auto frame = co_await read_frame();
            if (!frame) {
                throw std::runtime_error(
                        "Redis closed after RESP attributes without a response");
            }
            const bool attribute = !frame->empty() && frame->front() == '|';
            response.append(frame->data(), frame->size());
            if (!attribute) {
                co_return std::move(response);
            }
        }
    }

    seastar::sstring take_buffer() {
        return std::exchange(_buffer, {});
    }

    bool buffer_empty() const noexcept {
        return _buffer.empty();
    }

private:
    seastar::input_stream<char>& _input;
    seastar::sstring _buffer;
};

struct RedisConnection {
    explicit RedisConnection(seastar::connected_socket socket)
        : socket(std::move(socket))
        , input(this->socket.input())
        , output(this->socket.output())
        , reader(input) {
        this->socket.set_nodelay(true);
    }

    void abort() noexcept {
        try {
            socket.shutdown_input();
        } catch (...) {
        }
        try {
            socket.shutdown_output();
        } catch (...) {
        }
    }

    seastar::connected_socket socket;
    seastar::input_stream<char> input;
    seastar::output_stream<char> output;
    RespReader reader;
};

struct BackendCredentials {
    std::string username;
    std::string password;

    bool enabled() const noexcept {
        return !password.empty();
    }
};

BackendCredentials backend_credentials(const Config& config) {
    return {config.redis_username, config.redis_password};
}

void append_bulk(seastar::sstring& request, std::string_view value) {
    request += "$";
    request += std::to_string(value.size());
    request += "\r\n";
    request.append(value.data(), value.size());
    request += "\r\n";
}

seastar::future<bool> authenticate_redis(
        RedisConnection& connection,
        const BackendCredentials& credentials) {
    if (!credentials.enabled()) {
        co_return true;
    }
    seastar::sstring request =
            credentials.username.empty() ? "*2\r\n" : "*3\r\n";
    append_bulk(request, "AUTH");
    if (!credentials.username.empty()) {
        append_bulk(request, credentials.username);
    }
    append_bulk(request, credentials.password);
    co_await connection.output.write(request.data(), request.size());
    co_await connection.output.flush();
    auto response = co_await connection.reader.read_response();
    co_return response &&
            std::string_view(response->data(), response->size()) ==
                    "+OK\r\n" &&
            connection.reader.buffer_empty();
}

seastar::future<seastar::sstring> query_redis(
        seastar::socket_address address,
        const BackendCredentials& credentials,
        std::string_view request) {
    auto socket = co_await seastar::engine().net().connect(address);
    RedisConnection connection(std::move(socket));
    try {
        if (!co_await authenticate_redis(connection, credentials)) {
            throw std::runtime_error(
                    "Redis backend authentication failed");
        }
        co_await connection.output.write(request.data(), request.size());
        co_await connection.output.flush();
        auto response = co_await connection.reader.read_response();
        if (!response) {
            throw std::runtime_error(
                    "Redis closed the detection connection without a response");
        }
        co_await connection.input.close();
        co_await connection.output.close();
        co_return std::move(*response);
    } catch (...) {
        connection.abort();
        throw;
    }
}

seastar::future<> validate_redis_connection(
        seastar::socket_address address,
        const BackendCredentials& credentials) {
    auto socket = co_await seastar::engine().net().connect(address);
    RedisConnection connection(std::move(socket));
    try {
        if (!co_await authenticate_redis(connection, credentials)) {
            throw std::runtime_error(
                    "Redis backend authentication failed");
        }
        co_await connection.input.close();
        co_await connection.output.close();
    } catch (...) {
        connection.abort();
        throw;
    }
}

class PrivateConnectionPool {
public:
    PrivateConnectionPool(
            seastar::socket_address address,
            std::size_t target,
            std::size_t maximum,
            BackendCredentials credentials)
        : _address(address)
        , _target(target)
        , _maximum(maximum)
        , _credentials(std::move(credentials)) {
        if (_target > _maximum) {
            throw std::invalid_argument(
                    "private pool target exceeds its hard maximum");
        }
    }

    seastar::future<> start() {
        _idle.reserve(_target);
        for (std::size_t index = 0; index < _target; ++index) {
            _idle.push_back(co_await connect_one());
        }
    }

    seastar::future<std::unique_ptr<RedisConnection>> checkout() {
        while (_idle.empty() &&
               connection_count() >= _maximum &&
               !_stopping) {
            ++_waiters;
            try {
                co_await _connection_available.when([this] {
                    return _stopping || !_idle.empty() ||
                            connection_count() < _maximum;
                });
            } catch (...) {
                --_waiters;
                throw;
            }
            --_waiters;
        }
        if (_stopping) {
            throw std::runtime_error("private Redis pool is stopping");
        }
        if (_idle.empty()) {
            ++_connecting;
            std::unique_ptr<RedisConnection> connection;
            try {
                connection = co_await connect_one();
            } catch (...) {
                --_connecting;
                _connection_available.broadcast();
                throw;
            }
            --_connecting;
            ++_checked_out;
            _connection_available.broadcast();
            co_return std::move(connection);
        }

        auto connection = std::move(_idle.back());
        _idle.pop_back();
        ++_checked_out;
        co_return std::move(connection);
    }

    void checkin(std::unique_ptr<RedisConnection> connection) {
        --_checked_out;
        if (!_stopping &&
            (_waiters > 0 || _idle.size() + _checked_out < _target)) {
            _idle.push_back(std::move(connection));
        }
        _connection_available.broadcast();
    }

    void discard() {
        --_checked_out;
        _connection_available.broadcast();
        replenish();
    }

    seastar::future<> stop() {
        _stopping = true;
        _connection_available.broadcast();
        co_await _replenishing.close();
        _idle.clear();
    }

private:
    seastar::future<std::unique_ptr<RedisConnection>> connect_one() {
        auto socket = co_await seastar::engine().net().connect(_address);
        auto connection =
                std::make_unique<RedisConnection>(std::move(socket));
        if (!co_await authenticate_redis(*connection, _credentials)) {
            connection->abort();
            throw std::runtime_error(
                    "private Redis connection authentication failed");
        }
        co_return connection;
    }

    void replenish() {
        if (_stopping) {
            return;
        }
        while (connection_count() < _target &&
               connection_count() < _maximum) {
            ++_connecting;
            (void)seastar::with_gate(_replenishing, [this] {
                return connect_one().then([this] (
                        std::unique_ptr<RedisConnection> connection) {
                    if (!_stopping) {
                        _idle.push_back(std::move(connection));
                    }
                }).finally([this] {
                    --_connecting;
                    _connection_available.broadcast();
                });
            }).handle_exception([] (std::exception_ptr error) {
                fmt::print(
                        stderr,
                        "private Redis pool replenish failed: {}\n",
                        exception_message(error));
            });
        }
    }

    std::size_t connection_count() const noexcept {
        return _idle.size() + _checked_out + _connecting;
    }

    seastar::socket_address _address;
    std::size_t _target;
    std::size_t _maximum;
    BackendCredentials _credentials;
    std::vector<std::unique_ptr<RedisConnection>> _idle;
    std::size_t _checked_out{0};
    std::size_t _connecting{0};
    std::size_t _waiters{0};
    bool _stopping{false};
    seastar::gate _replenishing;
    seastar::condition_variable _connection_available;
};

struct BackendJob {
    seastar::sstring request;
    std::size_t response_count{1};
    seastar::promise<seastar::sstring> response;
};

class BackendWorker {
public:
    BackendWorker(
            seastar::socket_address address,
            std::size_t pipeline_depth,
            std::size_t queue_capacity,
            BackendCredentials credentials)
        : _address(address)
        , _pipeline_depth(pipeline_depth)
        , _queue_slots(queue_capacity)
        , _credentials(std::move(credentials)) {
    }

    seastar::future<> start() {
        _task.emplace(run());
        co_return;
    }

    seastar::future<seastar::sstring> execute(
            seastar::sstring request,
            std::size_t response_count = 1) {
        co_await _queue_slots.wait(1);
        auto job = std::make_unique<BackendJob>();
        job->request = std::move(request);
        job->response_count = response_count;
        auto result = job->response.get_future();
        _jobs.push_back(std::move(job));
        _jobs_available.signal();
        co_return co_await std::move(result);
    }

    seastar::future<> stop() {
        _stopping = true;
        _queue_slots.broken();
        _jobs_available.broadcast();
        if (_connection) {
            _connection->abort();
        }
        if (_task) {
            try {
                co_await std::move(*_task);
            } catch (...) {
            }
            _task.reset();
        }
        fail_jobs(_jobs, std::make_exception_ptr(
                std::runtime_error("Redis worker stopped")));
    }

private:
    using JobList = std::deque<std::unique_ptr<BackendJob>>;

    seastar::future<> run() {
        while (!_stopping) {
            std::exception_ptr error;
            try {
                auto socket =
                        co_await seastar::engine().net().connect(_address);
                _connection =
                        std::make_unique<RedisConnection>(std::move(socket));
                if (!co_await authenticate_redis(
                            *_connection, _credentials)) {
                    throw std::runtime_error(
                            "multiplexed Redis connection authentication failed");
                }
                co_await run_session();
            } catch (...) {
                error = std::current_exception();
            }
            if (error) {
                fail_jobs(_pending, error);
            }
            _connection.reset();
            if (!_stopping) {
                co_await seastar::sleep(100ms);
            }
        }
    }

    seastar::future<> run_session() {
        while (!_stopping) {
            if (_pending.empty() && _jobs.empty()) {
                co_await _jobs_available.when([this] {
                    return _stopping || !_jobs.empty();
                });
                if (_stopping) {
                    co_return;
                }
            }

            std::vector<std::unique_ptr<BackendJob>> batch;
            while (!_jobs.empty() &&
                   _pending.size() + batch.size() < _pipeline_depth) {
                batch.push_back(std::move(_jobs.front()));
                _jobs.pop_front();
                _queue_slots.signal(1);
            }

            if (!batch.empty()) {
                std::size_t size = 0;
                for (const auto& job : batch) {
                    size += job->request.size();
                }
                seastar::sstring write_buffer(
                        seastar::sstring::initialized_later(), size);
                auto destination = write_buffer.begin();
                for (const auto& job : batch) {
                    destination = std::copy(
                            job->request.begin(),
                            job->request.end(),
                            destination);
                }
                co_await _connection->output.write(
                        write_buffer.data(), write_buffer.size());
                co_await _connection->output.flush();
                for (auto& job : batch) {
                    _pending.push_back(std::move(job));
                }
                continue;
            }

            std::optional<seastar::sstring> response;
            for (std::size_t index = 0;
                 index < _pending.front()->response_count;
                 ++index) {
                response = co_await _connection->reader.read_response();
                if (!response) {
                    throw std::runtime_error(
                            "Redis closed the connection without a response");
                }
            }
            auto job = std::move(_pending.front());
            _pending.pop_front();
            job->response.set_value(std::move(*response));
        }
    }

    static void fail_jobs(JobList& jobs, std::exception_ptr error) {
        while (!jobs.empty()) {
            auto job = std::move(jobs.front());
            jobs.pop_front();
            job->response.set_exception(error);
        }
    }

    seastar::socket_address _address;
    std::size_t _pipeline_depth;
    seastar::semaphore _queue_slots;
    BackendCredentials _credentials;
    seastar::condition_variable _jobs_available;
    JobList _jobs;
    JobList _pending;
    std::unique_ptr<RedisConnection> _connection;
    std::optional<seastar::future<>> _task;
    bool _stopping{false};
};

seastar::future<bool> reset_private_connection(
        RedisConnection& backend,
        const BackendCredentials& credentials);

class RedisPool {
public:
    explicit RedisPool(const Config& config)
        : RedisPool(
                  config,
                  address_of(config.redis_address, config.redis_port),
                  config.private_pool_size) {
    }

    RedisPool(
            const Config& config,
            seastar::socket_address address,
            std::size_t private_pool_size)
        : _credentials(backend_credentials(config))
        , _private(
                  address,
                  private_pool_size,
                  config.private_max_connections,
                  _credentials) {
        _workers.reserve(config.redis_pool_size);
        for (std::size_t index = 0; index < config.redis_pool_size; ++index) {
            _workers.push_back(std::make_unique<BackendWorker>(
                    address,
                    config.pipeline_depth,
                    config.worker_queue_capacity,
                    backend_credentials(config)));
        }
    }

    seastar::future<> start() {
        co_await _private.start();
        for (auto& worker : _workers) {
            co_await worker->start();
        }
    }

    seastar::future<seastar::sstring> execute(
            seastar::sstring request,
            std::size_t response_count = 1) {
        auto& worker = _workers[_next_worker];
        _next_worker = (_next_worker + 1) % _workers.size();
        co_return co_await worker->execute(
                std::move(request), response_count);
    }

    seastar::future<std::unique_ptr<RedisConnection>> checkout_private() {
        co_return co_await _private.checkout();
    }

    void recycle_private(std::unique_ptr<RedisConnection> connection) {
        _private.checkin(std::move(connection));
    }

    void discard_private() {
        _private.discard();
    }

    seastar::future<bool> sanitize_private(RedisConnection& connection) {
        co_return co_await reset_private_connection(
                connection, _credentials);
    }

    seastar::future<> stop() {
        for (auto& worker : _workers) {
            co_await worker->stop();
        }
        co_await _private.stop();
    }

private:
    std::vector<std::unique_ptr<BackendWorker>> _workers;
    std::size_t _next_worker{0};
    BackendCredentials _credentials;
    PrivateConnectionPool _private;
};

struct Redirection {
    bool asking;
    std::uint16_t slot;
    cluster::Endpoint endpoint;
};

std::optional<Redirection> parse_redirection(std::string_view response) {
    bool asking = false;
    std::size_t position = 0;
    if (response.starts_with("-MOVED ")) {
        position = 7;
    } else if (response.starts_with("-ASK ")) {
        asking = true;
        position = 5;
    } else {
        return std::nullopt;
    }
    const auto slot_end = response.find(' ', position);
    if (slot_end == std::string_view::npos) {
        return std::nullopt;
    }
    unsigned slot = 0;
    const auto slot_text = response.substr(position, slot_end - position);
    const auto [slot_parse_end, slot_error] = std::from_chars(
            slot_text.data(), slot_text.data() + slot_text.size(), slot);
    if (slot_error != std::errc{} ||
        slot_parse_end != slot_text.data() + slot_text.size() ||
        slot >= cluster::slot_count) {
        return std::nullopt;
    }
    const auto endpoint_end = response.find("\r\n", slot_end + 1);
    if (endpoint_end == std::string_view::npos) {
        return std::nullopt;
    }
    const auto endpoint_text =
            response.substr(slot_end + 1, endpoint_end - slot_end - 1);
    const auto colon = endpoint_text.rfind(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    unsigned port = 0;
    const auto port_text = endpoint_text.substr(colon + 1);
    const auto [port_parse_end, port_error] = std::from_chars(
            port_text.data(), port_text.data() + port_text.size(), port);
    if (port_error != std::errc{} ||
        port_parse_end != port_text.data() + port_text.size() ||
        port == 0 || port > 65535) {
        return std::nullopt;
    }
    return Redirection{
            asking,
            static_cast<std::uint16_t>(slot),
            cluster::Endpoint{
                    std::string(endpoint_text.substr(0, colon)),
                    static_cast<std::uint16_t>(port)}};
}

class ClusterRedisPool {
public:
    struct NodeRoute {
        std::optional<std::size_t> node;
        std::optional<std::uint16_t> slot;
        seastar::sstring error;
    };

    explicit ClusterRedisPool(Config config)
        : _config(std::move(config))
        , _seed(address_of(_config.redis_address, _config.redis_port)) {
        _slots.fill(0);
    }

    seastar::future<> start() {
        co_await refresh_topology();
    }

    seastar::future<> refresh_topology() {
        static constexpr std::string_view cluster_slots =
                "*2\r\n$7\r\nCLUSTER\r\n$5\r\nSLOTS\r\n";
        auto response = co_await query_redis(
                _seed, backend_credentials(_config), cluster_slots);
        auto topology = cluster::parse_cluster_slots(
                std::string_view(response.data(), response.size()),
                _config.redis_address);

        std::vector<std::size_t> topology_nodes;
        topology_nodes.reserve(topology.primaries.size());
        for (const auto& endpoint : topology.primaries) {
            topology_nodes.push_back(co_await ensure_node(endpoint));
        }
        std::array<std::size_t, cluster::slot_count> slots;
        for (std::size_t slot = 0; slot < cluster::slot_count; ++slot) {
            slots[slot] = topology_nodes[topology.slots[slot]];
        }
        _slots = std::move(slots);
    }

    seastar::future<seastar::sstring> execute(seastar::sstring request) {
        co_return co_await execute(std::move(request), true);
    }

    NodeRoute route(std::string_view request) const {
        const auto route = cluster::route_request(request);
        switch (route.status) {
        case cluster::RouteStatus::ok:
            return {_slots[*route.slot], route.slot, {}};
        case cluster::RouteStatus::no_key:
            return {0, std::nullopt, {}};
        case cluster::RouteStatus::cross_slot:
            return {
                    std::nullopt,
                    std::nullopt,
                    "-CROSSSLOT Keys in request don't hash to the same slot\r\n"};
        case cluster::RouteStatus::unsupported:
            return {
                    std::nullopt,
                    std::nullopt,
                    "-ERR seaproxy does not know how to route this command in cluster mode\r\n"};
        case cluster::RouteStatus::invalid:
            return {
                    std::nullopt,
                    std::nullopt,
                    "-ERR invalid command arguments for cluster routing\r\n"};
        }
        return {
                std::nullopt,
                std::nullopt,
                "-ERR cluster routing failed\r\n"};
    }

    RedisPool& node_pool(std::size_t node) {
        return *_nodes.at(node).pool;
    }

    seastar::future<seastar::sstring> execute_transaction(
            std::size_t node,
            const std::vector<seastar::sstring>& requests) {
        auto& pool = node_pool(node);
        auto connection = co_await pool.checkout_private();
        bool recyclable = false;
        std::exception_ptr error;
        std::optional<seastar::sstring> result;
        try {
            static constexpr std::string_view multi =
                    "*1\r\n$5\r\nMULTI\r\n";
            static constexpr std::string_view exec =
                    "*1\r\n$4\r\nEXEC\r\n";
            std::size_t size = multi.size() + exec.size();
            for (const auto& request : requests) {
                size += request.size();
            }
            seastar::sstring pipeline(
                    seastar::sstring::initialized_later(), size);
            auto destination = std::copy(
                    multi.begin(), multi.end(), pipeline.begin());
            for (const auto& request : requests) {
                destination = std::copy(
                        request.begin(), request.end(), destination);
            }
            std::copy(exec.begin(), exec.end(), destination);
            co_await connection->output.write(
                    pipeline.data(), pipeline.size());
            co_await connection->output.flush();

            auto response = co_await connection->reader.read_response();
            if (!response ||
                std::string_view(response->data(), response->size()) !=
                        "+OK\r\n") {
                throw std::runtime_error(
                        "Redis rejected MULTI on a private cluster connection");
            }
            for (std::size_t index = 0; index < requests.size(); ++index) {
                response = co_await connection->reader.read_response();
                if (!response) {
                    throw std::runtime_error(
                            "Redis closed while queueing a transaction");
                }
            }
            result = co_await connection->reader.read_response();
            if (!result) {
                throw std::runtime_error(
                        "Redis closed without an EXEC response");
            }
            recyclable = co_await pool.sanitize_private(*connection);
        } catch (...) {
            error = std::current_exception();
            connection->abort();
        }

        if (recyclable) {
            pool.recycle_private(std::move(connection));
        } else {
            connection->abort();
            connection.reset();
            pool.discard_private();
        }
        if (error) {
            std::rethrow_exception(error);
        }
        co_return std::move(*result);
    }

    seastar::future<> stop() {
        for (auto& node : _nodes) {
            co_await node.pool->stop();
        }
    }

private:
    struct Node {
        cluster::Endpoint endpoint;
        std::unique_ptr<RedisPool> pool;
    };

    seastar::future<std::size_t> ensure_node(
            const cluster::Endpoint& endpoint) {
        auto units = co_await seastar::get_units(_node_lock, 1);
        for (std::size_t index = 0; index < _nodes.size(); ++index) {
            if (_nodes[index].endpoint == endpoint) {
                co_return index;
            }
        }
        auto pool = std::make_unique<RedisPool>(
                _config,
                address_of(endpoint.address, endpoint.port),
                _config.private_pool_size);
        co_await pool->start();
        _nodes.push_back(Node{endpoint, std::move(pool)});
        co_return _nodes.size() - 1;
    }

    seastar::future<seastar::sstring> execute(
            seastar::sstring request,
            bool allow_redirect) {
        const auto routed = route(
                std::string_view(request.data(), request.size()));
        if (!routed.node) {
            co_return routed.error;
        }
        const auto node = *routed.node;

        auto response =
                co_await _nodes[node].pool->execute(request);
        if (!allow_redirect) {
            co_return response;
        }
        const auto redirect = parse_redirection(
                std::string_view(response.data(), response.size()));
        if (!redirect) {
            co_return response;
        }

        const auto redirected_node =
                co_await ensure_node(redirect->endpoint);
        if (!redirect->asking) {
            _slots[redirect->slot] = redirected_node;
            try {
                co_await refresh_topology();
            } catch (...) {
                fmt::print(
                        stderr,
                        "cluster topology refresh after MOVED failed: {}\n",
                        exception_message(std::current_exception()));
            }
            co_return co_await _nodes[redirected_node].pool->execute(
                    std::move(request));
        }

        static constexpr std::string_view asking =
                "*1\r\n$6\r\nASKING\r\n";
        seastar::sstring combined(
                seastar::sstring::initialized_later(),
                asking.size() + request.size());
        std::copy(asking.begin(), asking.end(), combined.begin());
        std::copy(
                request.begin(), request.end(),
                combined.begin() + asking.size());
        co_return co_await _nodes[redirected_node].pool->execute(
                std::move(combined), 2);
    }

    Config _config;
    seastar::socket_address _seed;
    std::vector<Node> _nodes;
    std::array<std::size_t, cluster::slot_count> _slots;
    seastar::semaphore _node_lock{1};
};

bool has_asynchronous_responses(std::string_view command) {
    static constexpr std::string_view commands[] = {
            "CLIENT", "MONITOR", "PSUBSCRIBE", "PUNSUBSCRIBE", "QUIT",
            "SSUBSCRIBE", "SUBSCRIBE", "SUNSUBSCRIBE", "UNSUBSCRIBE",
    };
    return std::ranges::any_of(commands, [command] (std::string_view candidate) {
        return equal_ascii_case(command, candidate);
    });
}

bool can_block_indefinitely(std::string_view command) {
    static constexpr std::string_view commands[] = {
            "BLMOVE", "BLMPOP", "BLPOP", "BRPOP", "BRPOPLPUSH", "BZMPOP",
            "BZPOPMAX", "BZPOPMIN", "WAIT", "WAITAOF", "XREAD",
            "XREADGROUP",
    };
    return std::ranges::any_of(commands, [command] (std::string_view candidate) {
        return equal_ascii_case(command, candidate);
    });
}

struct PrivateRelayState {
    std::deque<bool> pending;
    bool client_eof{false};
    bool asynchronous{false};
    bool failed{false};
    seastar::condition_variable changed;
};

seastar::future<> relay_private_client_requests(
        seastar::connected_socket& client_socket,
        seastar::input_stream<char>& client_input,
        RedisConnection& backend,
        PrivateRelayState& state,
        seastar::sstring initial_requests) {
    RespReader reader(client_input, std::move(initial_requests));
    try {
        while (auto request = co_await reader.read_frame()) {
            const auto view =
                    std::string_view(request->data(), request->size());
            const auto command = resp::command_name(view);
            if (!command) {
                state.failed = true;
                throw std::runtime_error(
                        "private client sent a request without a command");
            }
            if (equal_ascii_case(*command, "AUTH") ||
                equal_ascii_case(*command, "HELLO")) {
                state.failed = true;
                throw std::runtime_error(
                        "frontend authentication or protocol renegotiation "
                        "is not allowed after entering private mode");
            }
            state.asynchronous |= has_asynchronous_responses(*command);
            state.pending.push_back(can_block_indefinitely(*command));
            state.changed.broadcast();
            try {
                co_await backend.output.write(
                        request->data(), request->size());
                co_await backend.output.flush();
            } catch (...) {
                state.pending.pop_back();
                throw;
            }
        }
        state.client_eof = true;
        if (std::ranges::any_of(state.pending, [] (bool blocking) {
                return blocking;
            })) {
            state.failed = true;
            backend.abort();
        } else if (state.asynchronous) {
            backend.socket.shutdown_input();
        }
        state.changed.broadcast();
    } catch (...) {
        state.failed = true;
        state.client_eof = true;
        state.changed.broadcast();
        backend.abort();
        try {
            client_socket.shutdown_output();
        } catch (...) {
        }
        throw;
    }
}

seastar::future<> relay_private_backend_responses(
        seastar::connected_socket& client_socket,
        seastar::output_stream<char>& client_output,
        RedisConnection& backend,
        PrivateRelayState& state) {
    try {
        while (true) {
            co_await state.changed.when([&state] {
                return state.failed || state.asynchronous ||
                        !state.pending.empty() || state.client_eof;
            });
            if (state.failed ||
                (state.client_eof && state.pending.empty())) {
                co_return;
            }

            auto response = co_await backend.reader.read_response();
            if (!response) {
                throw std::runtime_error(
                        "Redis closed private connection without a response");
            }
            co_await client_output.write(
                    response->data(), response->size());
            co_await client_output.flush();
            if (!state.asynchronous) {
                if (state.pending.empty()) {
                    throw std::runtime_error(
                            "Redis sent an unexpected private response");
                }
                state.pending.pop_front();
                state.changed.broadcast();
            }
        }
    } catch (...) {
        if (state.client_eof &&
            (state.asynchronous ||
             std::ranges::any_of(state.pending, [] (bool blocking) {
                 return blocking;
             }))) {
            co_return;
        }
        state.failed = true;
        state.changed.broadcast();
        backend.abort();
        try {
            client_socket.shutdown_input();
            client_socket.shutdown_output();
        } catch (...) {
        }
        throw;
    }
}

seastar::future<bool> reset_private_connection(
        RedisConnection& backend,
        const BackendCredentials& credentials) {
    static constexpr std::string_view reset_request =
            "*1\r\n$5\r\nRESET\r\n";
    co_await backend.output.write(
            reset_request.data(), reset_request.size());
    co_await backend.output.flush();
    auto response = co_await backend.reader.read_response();
    if (!response ||
        std::string_view(response->data(), response->size()) !=
                "+RESET\r\n" ||
        !backend.reader.buffer_empty()) {
        co_return false;
    }
    co_return co_await authenticate_redis(backend, credentials);
}

seastar::future<> relay_private(
        seastar::connected_socket& client_socket,
        seastar::input_stream<char>& client_input,
        seastar::output_stream<char>& client_output,
        RedisPool& pool,
        std::unique_ptr<RedisConnection> backend,
        seastar::sstring first_request,
        seastar::sstring buffered_requests) {
    first_request.append(
            buffered_requests.data(), buffered_requests.size());
    PrivateRelayState state;
    auto client_to_backend = relay_private_client_requests(
            client_socket,
            client_input,
            *backend,
            state,
            std::move(first_request));
    auto backend_to_client = relay_private_backend_responses(
            client_socket,
            client_output,
            *backend,
            state);

    bool recyclable = false;
    std::exception_ptr relay_error;
    try {
        co_await seastar::when_all_succeed(
                std::move(client_to_backend),
                std::move(backend_to_client));
        recyclable = state.client_eof &&
                state.pending.empty() &&
                !state.asynchronous &&
                !state.failed &&
                co_await pool.sanitize_private(*backend);
    } catch (...) {
        relay_error = std::current_exception();
        backend->abort();
    }

    if (recyclable) {
        pool.recycle_private(std::move(backend));
    } else {
        if (!relay_error && !state.failed && !state.asynchronous) {
            fmt::print(
                    stderr,
                    "private Redis connection RESET failed; discarding it\n");
        }
        backend->abort();
        backend.reset();
        pool.discard_private();
    }
    co_await client_input.close();
    co_await client_output.close();
    if (relay_error) {
        std::rethrow_exception(relay_error);
    }
}

bool constant_time_equal(std::string_view left, std::string_view right) {
    const auto size = std::max(left.size(), right.size());
    std::size_t difference = left.size() ^ right.size();
    for (std::size_t index = 0; index < size; ++index) {
        const unsigned char left_byte =
                index < left.size()
                ? static_cast<unsigned char>(left[index])
                : 0;
        const unsigned char right_byte =
                index < right.size()
                ? static_cast<unsigned char>(right[index])
                : 0;
        difference |= left_byte ^ right_byte;
    }
    return difference == 0;
}

bool valid_frontend_credentials(
        const Config& config,
        std::optional<std::string_view> username,
        std::string_view password) {
    if (config.frontend_password.empty()) {
        return false;
    }
    const std::string_view expected_username =
            config.frontend_username.empty()
            ? std::string_view("default")
            : std::string_view(config.frontend_username);
    if (!username) {
        if (!config.frontend_username.empty()) {
            return false;
        }
    } else if (!constant_time_equal(*username, expected_username)) {
        return false;
    }
    return constant_time_equal(password, config.frontend_password);
}

seastar::future<> write_frontend_response(
        seastar::output_stream<char>& output,
        std::string_view response) {
    co_await output.write(response.data(), response.size());
    co_await output.flush();
}

seastar::future<bool> handle_frontend_control(
        std::string_view request,
        std::string_view command,
        seastar::output_stream<char>& output,
        const Config& config,
        bool& authenticated) {
    if (equal_ascii_case(command, "AUTH")) {
        const auto arguments = resp::command_arguments(request);
        if (config.frontend_password.empty()) {
            co_await write_frontend_response(
                    output,
                    "-ERR AUTH called without any frontend password configured\r\n");
            co_return true;
        }
        std::optional<std::string_view> username;
        std::optional<std::string_view> password;
        if (arguments && arguments->size() == 2) {
            password = (*arguments)[1];
        } else if (arguments && arguments->size() == 3) {
            username = (*arguments)[1];
            password = (*arguments)[2];
        }
        if (!password ||
            !valid_frontend_credentials(
                    config, username, *password)) {
            co_await write_frontend_response(
                    output,
                    "-WRONGPASS invalid username-password pair or user is disabled.\r\n");
            co_return true;
        }
        authenticated = true;
        co_await write_frontend_response(output, "+OK\r\n");
        co_return true;
    }

    if (equal_ascii_case(command, "HELLO")) {
        const auto arguments = resp::command_arguments(request);
        if (arguments) {
            for (std::size_t index = 2;
                 index < arguments->size();
                 ++index) {
                if (!equal_ascii_case((*arguments)[index], "AUTH")) {
                    continue;
                }
                if (index + 2 >= arguments->size() ||
                    !valid_frontend_credentials(
                            config,
                            (*arguments)[index + 1],
                            (*arguments)[index + 2])) {
                    co_await write_frontend_response(
                            output,
                            "-WRONGPASS invalid username-password pair or user is disabled.\r\n");
                    co_return true;
                }
                authenticated = true;
                break;
            }
        }
        if (!authenticated) {
            co_await write_frontend_response(
                    output, "-NOAUTH Authentication required.\r\n");
        } else {
            co_await write_frontend_response(
                    output,
                    "-ERR seaproxy cannot change protocol while multiplexing\r\n");
        }
        co_return true;
    }

    if (!authenticated) {
        co_await write_frontend_response(
                output, "-NOAUTH Authentication required.\r\n");
        co_return true;
    }
    if (equal_ascii_case(command, "SELECT")) {
        co_await write_frontend_response(
                output,
                "-ERR seaproxy supports only Redis database 0\r\n");
        co_return true;
    }
    if (equal_ascii_case(command, "CLIENT")) {
        const auto argument = resp::command_argument(request, 1);
        if (argument && equal_ascii_case(*argument, "SETINFO")) {
            co_await write_frontend_response(output, "+OK\r\n");
            co_return true;
        }
    }
    co_return false;
}

bool needs_frontend_control(
        std::string_view command,
        bool authenticated) {
    return !authenticated ||
            equal_ascii_case(command, "AUTH") ||
            equal_ascii_case(command, "HELLO") ||
            equal_ascii_case(command, "SELECT") ||
            equal_ascii_case(command, "CLIENT");
}

seastar::future<> handle_client(
        seastar::connected_socket socket,
        RedisPool& pool,
        const Config& config,
        seastar::abort_source& abort) {
    socket.set_nodelay(true);
    auto input = socket.input();
    auto output = socket.output();
    RespReader reader(input);
    bool authenticated = config.frontend_password.empty();
    auto subscription = abort.subscribe([&socket] () noexcept {
        try {
            socket.shutdown_input();
            socket.shutdown_output();
        } catch (...) {
        }
    });

    while (true) {
        auto request = co_await reader.read_frame();
        if (!request) {
            break;
        }

        const auto view =
                std::string_view(request->data(), request->size());
        const auto command = resp::command_name(view);
        if (command) {
            if (needs_frontend_control(*command, authenticated) &&
                co_await handle_frontend_control(
                        view,
                        *command,
                        output,
                        config,
                        authenticated)) {
                continue;
            }
            // if it's a connection-scoped command, handle it with a private backend
            // we can't multiplex them
            if (is_connection_scoped(*command)) {
                auto backend = co_await pool.checkout_private();
                co_await relay_private(
                        socket,
                        input,
                        output,
                        pool,
                        std::move(backend),
                        std::move(*request),
                        reader.take_buffer());
                co_return;
            }
        }

        auto response = co_await pool.execute(std::move(*request));
        co_await output.write(response.data(), response.size());
        co_await output.flush();
    }

    co_await input.close();
    co_await output.close();
}

seastar::future<bool> handle_cluster_transaction(
        RespReader& reader,
        seastar::output_stream<char>& output,
        ClusterRedisPool& pool) {
    std::vector<seastar::sstring> requests;
    std::optional<std::uint16_t> transaction_slot;
    std::optional<std::size_t> transaction_node;
    bool dirty = false;

    while (true) {
        auto request = co_await reader.read_frame();
        if (!request) {
            co_return false;
        }
        const auto view =
                std::string_view(request->data(), request->size());
        const auto command = resp::command_name(view);
        if (!command) {
            dirty = true;
            co_await output.write("-ERR invalid command in transaction\r\n");
            co_await output.flush();
            continue;
        }
        if (equal_ascii_case(*command, "MULTI")) {
            dirty = true;
            co_await output.write(
                    "-ERR MULTI calls can not be nested\r\n");
            co_await output.flush();
            continue;
        }
        if (equal_ascii_case(*command, "DISCARD")) {
            co_await output.write("+OK\r\n");
            co_await output.flush();
            co_return true;
        }
        if (equal_ascii_case(*command, "EXEC")) {
            if (dirty) {
                co_await output.write(
                        "-EXECABORT Transaction discarded because of previous errors.\r\n");
                co_await output.flush();
                co_return true;
            }
            const auto node = transaction_node.value_or(0);
            auto response =
                    co_await pool.execute_transaction(node, requests);
            co_await output.write(response.data(), response.size());
            co_await output.flush();
            co_return true;
        }
        if (equal_ascii_case(*command, "WATCH") ||
            equal_ascii_case(*command, "UNWATCH")) {
            dirty = true;
            co_await output.write(
                    "-ERR WATCH inside MULTI is not allowed\r\n");
            co_await output.flush();
            continue;
        }

        const auto route = pool.route(view);
        if (!route.node) {
            dirty = true;
            co_await output.write(route.error.data(), route.error.size());
            co_await output.flush();
            continue;
        }
        if (route.slot) {
            if (transaction_slot && transaction_slot != route.slot) {
                dirty = true;
                co_await output.write(
                        "-CROSSSLOT Keys in request don't hash to the same slot\r\n");
                co_await output.flush();
                continue;
            }
            transaction_slot = route.slot;
            transaction_node = route.node;
        }
        requests.push_back(std::move(*request));
        co_await output.write("+QUEUED\r\n");
        co_await output.flush();
    }
}

seastar::future<> handle_cluster_client(
        seastar::connected_socket socket,
        ClusterRedisPool& pool,
        const Config& config,
        seastar::abort_source& abort) {
    socket.set_nodelay(true);
    auto input = socket.input();
    auto output = socket.output();
    RespReader reader(input);
    bool authenticated = config.frontend_password.empty();
    auto subscription = abort.subscribe([&socket] () noexcept {
        try {
            socket.shutdown_input();
            socket.shutdown_output();
        } catch (...) {
        }
    });

    while (true) {
        auto request = co_await reader.read_frame();
        if (!request) {
            break;
        }
        const auto view =
                std::string_view(request->data(), request->size());
        const auto command = resp::command_name(view);
        if (command) {
            if (needs_frontend_control(*command, authenticated) &&
                co_await handle_frontend_control(
                        view,
                        *command,
                        output,
                        config,
                        authenticated)) {
                continue;
            }
            if (equal_ascii_case(*command, "MULTI")) {
                co_await output.write("+OK\r\n");
                co_await output.flush();
                if (!co_await handle_cluster_transaction(
                            reader, output, pool)) {
                    break;
                }
                continue;
            }
            if (is_connection_scoped(*command)) {
                const auto route = pool.route(view);
                if (!route.node) {
                    co_await output.write(
                            route.error.data(), route.error.size());
                    co_await output.flush();
                    continue;
                }
                auto& node_pool = pool.node_pool(*route.node);
                auto backend = co_await node_pool.checkout_private();
                co_await relay_private(
                        socket,
                        input,
                        output,
                        node_pool,
                        std::move(backend),
                        std::move(*request),
                        reader.take_buffer());
                co_return;
            }
        }

        auto response = co_await pool.execute(std::move(*request));
        co_await output.write(response.data(), response.size());
        co_await output.flush();
    }

    co_await input.close();
    co_await output.close();
}

}  // namespace

seastar::future<RedisMode> detect_redis_mode(const Config& config) {
    if (config.redis_mode == RedisMode::standalone) {
        co_await validate_redis_connection(
                address_of(config.redis_address, config.redis_port),
                backend_credentials(config));
        co_return RedisMode::standalone;
    }
    static constexpr std::string_view cluster_info =
            "*2\r\n$7\r\nCLUSTER\r\n$4\r\nINFO\r\n";
    auto response = co_await query_redis(
            address_of(config.redis_address, config.redis_port),
            backend_credentials(config),
            cluster_info);
    const auto view = std::string_view(response.data(), response.size());
    if (!view.empty() && view.front() != '-') {
        co_return RedisMode::cluster;
    }
    if (view.find("cluster support disabled") != std::string_view::npos) {
        if (config.redis_mode == RedisMode::cluster) {
            throw std::runtime_error(
                    "Redis cluster mode was required but the server has cluster support disabled");
        }
        co_return RedisMode::standalone;
    }
    throw std::runtime_error(
            "CLUSTER INFO failed during Redis mode detection: " +
            std::string(view));
}

class ProxyService::Impl {
public:
    explicit Impl(Config config)
        : _config(std::move(config))
        , _client_slots(_config.max_clients_per_shard) {
        if (_config.redis_mode == RedisMode::standalone) {
            _standalone = std::make_unique<RedisPool>(_config);
        } else if (_config.redis_mode == RedisMode::cluster) {
            _cluster = std::make_unique<ClusterRedisPool>(_config);
        } else {
            throw std::logic_error(
                    "Redis mode must be detected before ProxyService starts");
        }
    }

    seastar::future<> start() {
        // Start the appropriate Redis pool based on the configured mode
        if (_standalone) {
            co_await _standalone->start();
        } else {
            co_await _cluster->start();
        }

        seastar::listen_options options;
        options.reuse_address = true;
        // least-connections balancer across shards
        options.lba = seastar::server_socket::load_balancing_algorithm::
                connection_distribution;
        _listener = seastar::engine().net().listen(
                address_of(_config.listen_address, _config.listen_port),
                options);
        _accept_task.emplace(accept_loop());
        fmt::print(
                stderr,
                "shard {} listening on {}:{} with {} multiplexed and {} "
                "preconnected private redis connections to {}:{}; "
                "mode={}; pipeline_depth={}; private_max_connections={}; "
                "max_clients={}\n",
                seastar::this_shard_id(),
                _config.listen_address,
                _config.listen_port,
                _config.redis_pool_size,
                _config.private_pool_size,
                _config.redis_address,
                _config.redis_port,
                _standalone ? "standalone" : "cluster",
                _config.pipeline_depth,
                _config.private_max_connections,
                _config.max_clients_per_shard);
    }

    seastar::future<> stop() {
        _stopping = true;
        _abort.request_abort();
        _listener.abort_accept();
        if (_accept_task) {
            try {
                co_await std::move(*_accept_task);
            } catch (...) {
            }
            _accept_task.reset();
        }
        co_await _connections.close();
        if (_standalone) {
            co_await _standalone->stop();
        } else {
            co_await _cluster->stop();
        }
    }

private:
// per-shard accept loop for handling incoming frontend connections
    seastar::future<> accept_loop() {
        while (!_stopping) {
            try {
                auto accepted = co_await _listener.accept();
                // reject clients if max-clients-per-shard is reached
                // try_wait immediately returns false if no slots are available
                if (!_client_slots.try_wait(1)) {
                    try {
                        accepted.connection.shutdown_input();
                        accepted.connection.shutdown_output();
                    } catch (...) {
                    }
                    ++_rejected_clients;
                    if ((_rejected_clients &
                         (_rejected_clients - 1)) == 0) {
                        fmt::print(
                                stderr,
                                "shard {} rejected {} frontend connections "
                                "after reaching max-clients-per-shard={}\n",
                                seastar::this_shard_id(),
                                _rejected_clients,
                                _config.max_clients_per_shard);
                    }
                    continue;
                }
                (void)seastar::with_gate(
                        _connections,
                        [this, client = std::move(accepted.connection)] () mutable {
                            if (_standalone) {
                                return handle_client(
                                        std::move(client),
                                        *_standalone,
                                        _config,
                                        _abort).finally([this] {
                                    _client_slots.signal(1);
                                });
                            }
                            return handle_cluster_client(
                                    std::move(client),
                                    *_cluster,
                                    _config,
                                    _abort).finally([this] {
                                _client_slots.signal(1);
                            });
                        }).handle_exception([] (std::exception_ptr error) {
                    fmt::print(
                            stderr,
                            "client connection failed: {}\n",
                            exception_message(error));
                });
            } catch (...) {
                if (_stopping) {
                    co_return;
                }
                throw;
            }
        }
    }

    Config _config;
    seastar::semaphore _client_slots;
    std::unique_ptr<RedisPool> _standalone;
    std::unique_ptr<ClusterRedisPool> _cluster;
    seastar::server_socket _listener;
    seastar::gate _connections;
    seastar::abort_source _abort;
    std::optional<seastar::future<>> _accept_task;
    std::size_t _rejected_clients{0};
    bool _stopping{false};
};

ProxyService::ProxyService(Config config)
    : _impl(std::make_unique<Impl>(std::move(config))) {
}

ProxyService::~ProxyService() = default;

seastar::future<> ProxyService::start() {
    return _impl->start();
}

seastar::future<> ProxyService::stop() {
    return _impl->stop();
}

}  // namespace seaproxy
