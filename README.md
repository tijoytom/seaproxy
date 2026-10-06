# SeaProxy

SeaProxy is a shard-per-core Redis multiplexing proxy implemented in C++ with
[Seastar](https://github.com/scylladb/seastar). Each Seastar shard owns its
listener, backend connections, queues, and client state, so requests do not
cross cores.
## AI Disclosure 
Most of the benchmark, test and build scripts are AI generated. Design and most 
of the code is human written.
## Architecture

- One or more multiplexed Redis connections per shard(per core).
- Ordered backend pipelines with coalesced writes.
- A preconnected private Redis pool per shard for connection-scoped commands(ex BLPOP)
- Shard-local Redis Cluster slot maps and per-primary connection pools.
- Bounded frontend admission and hard private backend connection limits.
- RESP2 request and RESP2/RESP3 response framing.
- Backend TLS support using Seastar OpenSSL integration.

Ordinary commands such as `GET` and `SET` use the multiplexed pool.
Connection-scoped, switch the client to a private backend connection for the remainder of that client connection. On a clean client EOF, SeaProxy drains every expected response, sends Redis `RESET` and returns the sanitized
connection to its shard-local pool. This avoids reconnect churn for short
transaction and connection-scoped sessions.SeaProxy conservatively closes and replaces a private connection when its state cannot be proven clean. TLS is supported for connections from SeaProxy to Redis. Frontend TLS is not
implemented.

## Redis Cluster

`redis.mode = "auto"` sends `CLUSTER INFO` to the configured Redis entry point
before accepting clients. A valid cluster response selects cluster mode; the
specific "cluster support disabled" response selects standalone mode. Any
authentication, permission, network, or unexpected protocol error fails
startup rather than silently selecting the wrong mode.

The selected mode cannot change during the process lifetime. Configure
`redis.mode = "standalone"` to skip detection or `redis.mode = "cluster"` to
require cluster mode explicitly.

In cluster mode, every Seastar shard:

- Fetches and validates complete slot coverage using `CLUSTER SLOTS`.
- Maintains a direct 16,384-entry slot table.
- Owns multiplexed and private pools for each primary it uses.
- Calculates Redis CRC16 slots, including hash tags such as `{key}:name`.
- Rejects cross-slot requests locally.
- Handles `MOVED` by updating the slot and refreshing topology.
- Handles `ASK` using an ordered `ASKING` plus command operation.
- Defers backend `MULTI` until queued keys select a node, and sanitizes the
  private connection with `RESET` after `EXEC`.

## Authentication

SeaProxy supports separate backend and frontend credentials. Passwords are
loaded from files rather than command-line values so they are not exposed in
the process argument list. Trailing CR and LF bytes are removed from secret
files. Relative password-file paths in a TOML configuration are resolved
relative to that configuration file.

Backend authentication settings:

```toml
[redis]
username = "service"
password_file = "/run/secrets/redis-password"
```

Frontend authentication settings:

```toml
[frontend]
username = "application"
password_file = "/run/secrets/frontend-password"
```

When a frontend password is configured, every client starts unauthenticated.
SeaProxy validates `AUTH` and `HELLO 3 AUTH` locally and returns `NOAUTH` for
other commands until authentication succeeds. Frontend credentials are never
forwarded to Redis. If the frontend username is empty, password-only
`AUTH <password>` and `AUTH default <password>` are accepted.
Authentication or protocol renegotiation after a connection has entered a
private pool is rejected by closing that client session, preventing it from
replacing SeaProxy's configured backend identity.


Backend TLS is enabled by default with `redis.tls = true`. SeaProxy verifies
the Redis certificate using the system trust store and `redis.address` as the
expected certificate name. Set `redis.tls_ca_file` for a private PEM CA bundle
or `redis.tls_server_name` when the certificate name differs from the address
used to connect. Set `redis.tls = false` only for an explicitly trusted
plaintext backend. Frontend credentials still travel as plaintext.

## Admission control

`listener.max_clients_per_shard` bounds simultaneously active frontend connections.
The default is 10,000 per shard, so a three-shard process admits at most 30,000
clients. Once a shard reaches its limit, newly accepted sockets are closed
immediately. Rejections do not create background tasks or consume admission
slots, preventing the rejection path itself from growing without bound.

Private Redis connections have two independent settings:

- `pools.private_pool_size_per_shard` is the warm idle target.
- `pools.private_max_connections_per_shard` is the hard total of idle, checked-out, and
  connecting sockets.

When a private pool reaches its hard maximum, additional connection-scoped
sessions wait for capacity instead of creating more Redis connections. A
sanitized returned connection is handed directly to a waiter. A discarded
connection releases capacity so a waiter can create its replacement. In
cluster mode, both values apply independently to every primary on every
SeaProxy shard.

The private warm pool size must not exceed its maximum. Waiting
private sessions still occupy frontend admission slots, so the frontend limit
also bounds the private checkout wait queue.

## Build

SeaProxy is developed against Seastar `seastar-25.05.0`.

On Ubuntu, `build.sh` installs Seastar's system dependencies, downloads and
builds the pinned Seastar release, builds SeaProxy, and runs its tests:

```sh
./build.sh
```

The script stores downloaded dependencies under `.deps/` so that directory can
be cached by CI. Set `SKIP_DEPENDENCY_INSTALL=1` when the required system
packages are already installed. `DEPS_DIR`, `SEASTAR_PREFIX`, `BUILD_DIR`,
`BUILD_TYPE`, and `JOBS` can also be overridden.

For example, a GitHub Actions build step can use:

```sh
JOBS=2 ./build.sh
```

Pull requests to `main` run the build, unit tests, and a Redis end-to-end test
covering ordinary `SET`/`GET` commands and a connection-scoped `MULTI`/`EXEC`
transaction. Configure the `Build, unit, and end-to-end tests` check as
required in the `main` branch ruleset so failed or pending checks block
merging.

## Run
You can find a config with defaults in src directory.

```sh
./build/seaproxy \
  --config ./config.toml \
  --smp 3 \
  --cpuset 1,3,5
```
Seastar runtime options such as `--smp`, `--memory`, and `--reactor-backend` is
available as command line options. The seastar is built with `io_uring`, `linux-aio`, and `epoll` support. On the current TCP proxy benchmark, `epoll` is
fastest and is therefore used by SeaProxy when no reactor option is supplied.
Explicit option `--reactor-backend io_uring` overrides the SeaProxy
default. Seastar also requires an appropriate memory allocation for the host. For small development runs, for example:

```sh
./build/seaproxy \
  --config ./config.toml \
  --smp 1 \
  --memory 256M \
  --overprovisioned
```

## Timeouts

The `[timeouts]` table supports:
`backend_connect_ms`, `backend_response_ms`, `private_checkout_ms`,
`client_idle_ms`, and `backend_reconnect_delay_ms`. The first four accept zero
to disable their deadline; the reconnect delay must be positive and defaults
to 100 ms.

A multiplexed response timeout aborts that backend connection, fails its
pending pipeline, and allows the worker to reconnect after the configured
delay. The response deadline also covers startup queries, authentication,
connection reset, and cluster transactions. It does not apply to the private
relay after a connection-scoped command, because blocking commands and
subscriptions can legitimately wait indefinitely. For the same reason, the
frontend idle timeout stops applying after a client enters private mode.

Use `--help-seastar` to see Seastar options such as `--smp`, `--cpuset`, and
`--memory`.
