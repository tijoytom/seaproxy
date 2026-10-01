# SeaProxy Benchmarks

These benchmarks compare SeaProxy with direct Redis access for ordinary
multiplexed commands and connection-scoped `MULTI` transactions. Results were
collected on October 1, 2026.

## Test environment

- Redis 7.0.15 on CPU 0.
- SeaProxy Release build using Seastar 25.05.0 and the epoll reactor.
- Three SeaProxy shards on CPUs 2, 4, and 6.
- Go benchmark client on CPUs 3, 5, and 7.
- CPUs 2/3, 4/5, and 6/7 are sibling hardware threads.
- One multiplexed Redis connection per SeaProxy shard.
- Pipeline depth 64.
- Worker queue capacity 1024 per multiplexed connection.
- 50 concurrent go-redis workers.
- 10-second measured runs after a warm-up.

SeaProxy was started through an outer `taskset` in addition to receiving its
Seastar `--cpuset` option. With `--overprovisioned`, the Seastar option alone
did not restrict every Linux thread in this environment.

All reported workloads completed with zero command errors and zero GET misses.

## Multiplexed GET/SET benchmark

This workload uses ordinary commands handled by SeaProxy's multiplexed path:

- 80% `GET` and 20% `SET`.
- 10,000-key keyspace.
- 128-byte values.
- Pooled client connections.

### Results

| Target | Samples (ops/sec) | Mean | Median | Relative to direct Redis |
|---|---|---:|---:|---:|
| Direct Redis | 191,013; 194,202; 195,509; 195,423; 195,460 | 194,321 | 195,423 | Baseline |
| SeaProxy | 266,501; 266,685; 262,594; 265,812; 256,972 | **263,713** | **265,812** | **+35.7% mean throughput** |

SeaProxy multiplexes 50 frontend client connections over three backend Redis
connections. This reduces Redis socket handling and enables ordered,
coalesced backend pipelines. Under this workload, those savings outweigh the
additional proxy hop.

### Reproduction

Build the in-repository benchmark client from the SeaProxy root:

```sh
cd benchclient
go build -o ../build/benchclient .
cd ..
```

Start SeaProxy:

```sh
taskset -c 2,4,6 ./build/seaproxy \
  --listen-address 127.0.0.1 \
  --listen-port 7000 \
  --redis-address 127.0.0.1 \
  --redis-port 6379 \
  --redis-pool-size 1 \
  --pipeline-depth 64 \
  --private-pool-size 10 \
  --worker-queue-capacity 1024 \
  --smp 3 \
  --cpuset 2,4,6 \
  --memory 1G \
  --overprovisioned
```

Run through SeaProxy:

```sh
taskset -c 3,5,7 ./build/benchclient \
  -addr 127.0.0.1:7000 \
  -workload getset \
  -concurrency 50 \
  -duration 10s \
  -keyspace 10000 \
  -set-percent 20 \
  -value-size 128
```

Run directly against Redis:

```sh
taskset -c 3,5,7 ./build/benchclient \
  -addr 127.0.0.1:6379 \
  -workload getset \
  -concurrency 50 \
  -duration 10s \
  -keyspace 10000 \
  -set-percent 20 \
  -value-size 128
```

## Connection-scoped MULTI benchmark

This workload executes transactions containing:

1. `MULTI`
2. Four queued GET/SET operations using the same 80/20 mix
3. `EXEC`

Each transaction therefore represents four logical operations and six Redis
commands. Every transaction creates and closes a frontend connection,
exercising private connection checkout, `RESET`, and recycling.

### Results

| Target | Transactions/sec | Commands/sec | Frontend connections created/run | Backend connections created/closed per run |
|---|---:|---:|---:|
| Direct Redis | 14,349 | 86,095 | about 143,545 | about 143,558/143,559 |
| SeaProxy, private pool 10/shard | 12,915 | 77,488 | about 129,213 | 17,704/17,704 |
| SeaProxy, private pool 20/shard | **14,504** | **87,021** | about 145,106 | **11/11** |

The five-run transaction-rate samples were:

- Direct Redis: 14,351; 14,333; 14,387; 14,324; 14,351.
- SeaProxy with 10 private connections per shard: 12,929; 12,926; 12,902;
  12,900; 12,917.
- SeaProxy with 20 private connections per shard: 14,487; 14,482; 14,529;
  14,520; 14,500.

With 10 private connections per shard, approximately 86.3% of private
sessions reused an existing backend connection. The remaining sessions needed
temporary backend connections when a shard exceeded its local pool capacity.
Throughput was 10.0% below direct Redis.

Increasing the private pool to 20 connections per shard reduced backend churn
to an average of 11 connections across more than 145,000 frontend sessions.
More than 99.99% of sessions reused a sanitized backend connection. SeaProxy
then reached 14,504 transactions/sec, 1.1% above direct Redis, because Redis
avoided almost all TCP connection setup and teardown.

Private pools are shard-local. The configured total should exceed expected
private concurrency with enough headroom for uneven connection distribution
between shards.

### Reproduction

Run one connection per transaction through SeaProxy:

```sh
taskset -c 3,5,7 ./build/benchclient \
  -addr 127.0.0.1:7000 \
  -workload multi \
  -connection-mode per-transaction \
  -transaction-size 4 \
  -concurrency 50 \
  -duration 10s \
  -backend-stats-addr 127.0.0.1:6379
```

For the direct comparison, change `-addr` to `127.0.0.1:6379`. Keep
`-backend-stats-addr` pointed at Redis.

## Summary

- Multiplexed GET/SET throughput through SeaProxy reached a 263,713 ops/sec
  mean, 35.7% higher than direct Redis, because 50 frontend connections shared
  three pipelined backend connections.
- Short-lived `MULTI` sessions require a private pool sized for peak
  per-shard concurrency. Increasing the pool from 10 to 20 connections per
  shard reduced measured backend churn from 17,704 connections to 11.
- With the larger private pool, short-lived transaction throughput through
  SeaProxy was 1.1% higher than direct Redis while recycling more than 99.99%
  of private backend connections.
