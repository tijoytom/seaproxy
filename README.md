# SeaProxy

SeaProxy is a shard-per-core Redis multiplexing proxy implemented in C++ with
[Seastar](https://github.com/scylladb/seastar). Each Seastar shard owns its
listener, backend connections, queues, and client state, so requests do not
cross cores.

## Architecture

- One or more multiplexed Redis connections per shard(per core).
- Ordered backend pipelines with coalesced writes.
- A preconnected private Redis pool per shard for connection-scoped commands(ex BLPOP)
- Shard-local Redis Cluster slot maps and per-primary connection pools.
- Bounded frontend admission and hard private backend connection limits.
- RESP2 request and RESP2/RESP3 response framing.
- Backend TLS support using Seastar OpenSSL integration.

Ordinary commands such as `GET` and `SET` use the multiplexed pool.
Connection-scoped, blocking, transaction, and Pub/Sub commands switch the
client to a private backend connection for the remainder of that client
connection. On a clean client EOF, SeaProxy drains every expected response,
sends Redis `RESET`, validates the exact response, and returns the sanitized
connection to its shard-local pool. This avoids reconnect churn for short
transaction and connection-scoped sessions without leaking state such as
`AUTH`, `WATCH`, or client configuration.

SeaProxy conservatively closes and replaces a private connection when its
state cannot be proven clean. This includes partial requests, missing or
unexpected responses, reset failures, active blocking commands at disconnect,
and asynchronous modes such as Pub/Sub and `MONITOR`. The Redis user must have permission to run `RESET` for a private connection to be reusable.


TLS is supported for connections from SeaProxy to Redis. Frontend TLS is not
implemented.

## Redis Cluster

`--redis-mode auto` sends `CLUSTER INFO` to the configured Redis entry point
before accepting clients. A valid cluster response selects cluster mode; the
specific "cluster support disabled" response selects standalone mode. Any
authentication, permission, network, or unexpected protocol error fails
startup rather than silently selecting the wrong mode.

The selected mode cannot change during the process lifetime. Use
`--redis-mode standalone` to skip detection or `--redis-mode cluster` to
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
files.

Backend authentication options:

```text
--redis-username <acl-user>
--redis-password-file <path>
```

Frontend authentication options:

```text
--frontend-username <client-user>
--frontend-password-file <path>
```

When a frontend password is configured, every client starts unauthenticated.
SeaProxy validates `AUTH` and `HELLO 3 AUTH` locally and returns `NOAUTH` for
other commands until authentication succeeds. Frontend credentials are never
forwarded to Redis. If the frontend username is empty, password-only
`AUTH <password>` and `AUTH default <password>` are accepted.
Authentication or protocol renegotiation after a connection has entered a
private pool is rejected by closing that client session, preventing it from
replacing SeaProxy's configured backend identity.


Enable backend TLS with `--redis-tls`. SeaProxy verifies the Redis certificate
using the system trust store and `--redis-address` as the expected certificate
name. Use `--redis-tls-ca-file` for a private PEM CA bundle or
`--redis-tls-server-name` when the certificate name differs from the address
used to connect. Frontend credentials still travel as plaintext.

## Admission control

`--max-clients-per-shard` bounds simultaneously active frontend connections.
The default is 10,000 per shard, so a three-shard process admits at most 30,000
clients. Once a shard reaches its limit, newly accepted sockets are closed
immediately. Rejections do not create background tasks or consume admission
slots, preventing the rejection path itself from growing without bound.

Private Redis connections have two independent settings:

- `--private-pool-size` is the warm idle target.
- `--private-max-connections` is the hard total of idle, checked-out, and
  connecting sockets.

When a private pool reaches its hard maximum, additional connection-scoped
sessions wait for capacity instead of creating more Redis connections. A
sanitized returned connection is handed directly to a waiter. A discarded
connection releases capacity so a waiter can create its replacement. In
cluster mode, both values apply independently to every primary on every
SeaProxy shard.

`--private-pool-size` must not exceed `--private-max-connections`. Waiting
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

### Native release packages

After building, create Ubuntu 24.04 AMD64 `.deb` and `.tar.gz` packages with
SHA-256 checksums:

```sh
./package-release.sh 0.1.0
```

The version must match the project version in `CMakeLists.txt`. Packages are
written to `dist/`.

Pushing a matching semantic-version tag runs the GitHub release workflow:

```sh
git tag -a v0.1.0 -m "SeaProxy v0.1.0"
git push origin v0.1.0
```

The workflow builds and tests SeaProxy, creates both native packages, and
attaches them and their checksums to a generated GitHub Release. It does not
build or publish a container image.

### Container image

Build a local OCI image using Docker or Podman:

```sh
./build-container.sh
```

The default image is `seaproxy:dev`. Use `--image` to assign another local
tag:

```sh
./build-container.sh --image seaproxy:0.1.0 --jobs 4
```

Image publishing is intentionally opt-in. `--push` requires an explicitly
registry-qualified image name, preventing an accidental push while the
official image registry and namespace are undecided:

```sh
./build-container.sh \
  --image ghcr.io/owner/seaproxy:0.1.0 \
  --push
```

The runtime image is based on Ubuntu 24.04, runs as UID 10001, includes the
system CA store for backend TLS, and exposes port 7000. For production, mount
password and private CA files as read-only secrets rather than including them
in an image.

## Run

```sh
./build/seaproxy \
  --listen-address 0.0.0.0 \
  --listen-port 7000 \
  --redis-address 127.0.0.1 \
  --redis-port 6379 \
  --redis-mode auto \
  --redis-tls \
  --redis-username service \
  --redis-password-file /run/secrets/redis-password \
  --frontend-username application \
  --frontend-password-file /run/secrets/frontend-password \
  --redis-pool-size 1 \
  --pipeline-depth 64 \
  --private-pool-size 10 \
  --private-max-connections 64 \
  --worker-queue-capacity 1024 \
  --max-clients-per-shard 10000 \
  --smp 3 \
  --cpuset 1,3,5
```

seastar is built with `io_uring`, `linux-aio`, and `epoll` support.
On the current TCP proxy benchmark, `epoll` is
fastest and is therefore used by SeaProxy when no reactor option is supplied.
Explicit option `--reactor-backend io_uring` overrides the SeaProxy
default. Seastar also requires an appropriate memory allocation for the host. For small development runs, for example:

```sh
./build/seaproxy --smp 1 --memory 256M --overprovisioned
```

Application options:

| Option | Default | Description |
|---|---:|---|
| `--listen-address` | `0.0.0.0` | Proxy listen address |
| `--listen-port` | `7000` | Proxy listen port |
| `--redis-address` | `127.0.0.1` | Redis backend address |
| `--redis-port` | `6379` | Redis backend port |
| `--redis-mode` | `auto` | Backend mode: `auto`, `standalone`, or `cluster`; immutable after startup |
| `--redis-username` | empty | Redis backend ACL username; empty uses password-only `AUTH` |
| `--redis-password-file` | empty | File containing the Redis backend password |
| `--redis-tls` | disabled | Encrypt and authenticate Redis backend connections |
| `--redis-tls-ca-file` | empty | PEM CA bundle for Redis TLS; empty uses system trust |
| `--redis-tls-server-name` | empty | Expected Redis certificate name; empty uses `--redis-address` |
| `--frontend-username` | empty | Username required from clients; requires a frontend password |
| `--frontend-password-file` | empty | File containing the password required from clients |
| `--redis-pool-size` | `1` | Multiplexed connections per shard |
| `--pipeline-depth` | `64` | Maximum in-flight requests per backend connection |
| `--private-pool-size` | `10` | Preconnected private connections per shard and, in cluster mode, per primary; zero connects on demand |
| `--private-max-connections` | `64` | Hard private connection limit per shard-local pool; must be at least the warm pool size |
| `--worker-queue-capacity` | `1024` | Queued requests per multiplexed connection |
| `--max-clients-per-shard` | `10000` | Maximum active frontend connections accepted by each shard |

Use `--help-seastar` to see Seastar options such as `--smp`, `--cpuset`, and
`--memory`.
