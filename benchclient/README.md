# benchclient

`benchclient` runs a concurrent mix of Redis `GET` and `SET` commands using
[`rueidis`](https://github.com/redis/rueidis). Concurrent commands use
rueidis's automatic pipelining. By default it connects to
SeaProxy at `127.0.0.1:7000`, uses 50 workers for 10 seconds, and sends 20%
`SET` commands.

```sh
go run . -addr 127.0.0.1:7000
```

Use the `multi` workload to benchmark connection-affine transactions. This
example creates a new rueidis client for every transaction, queues four GET/SET
operations between `MULTI` and `EXEC`, and reads backend connection counters
directly from Redis:

```sh
go run . \
  -addr 127.0.0.1:7000 \
  -workload multi \
  -transaction-size 4 \
  -connection-mode per-transaction \
  -cluster-hash-tags \
  -backend-stats-addr 127.0.0.1:6379
```

The result reports transactions, logical GET/SET operations, total Redis
commands including `MULTI` and `EXEC`, frontend connections created and
closed, and backend connections created and closed. Backend closed connections
are derived from Redis's cumulative `total_connections_received` and current
`connected_clients` counters. Run this mode against an otherwise idle Redis
instance for accurate backend connection counts.

The `getset` workload issues one command at a time from each worker. Rueidis
automatically combines concurrent commands on its multiplexed connections
into Redis pipelines. The `multi` workload uses a dedicated connection and
sends `MULTI`, its queued commands, and `EXEC` together. Rueidis can create
additional transport connections internally for dedicated clients, so
`per-transaction` connection counts are not expected to equal the transaction
count.

Use `-client-per-worker` to create one non-multiplexed rueidis client and TCP
connection for each worker. This is useful when benchmarking a proxy with a
specific number of independent frontend sessions.

Use `-pipeline-multiplex` to control shared-client pipeline connections per
discovered Redis node. The value is a base-2 exponent, so `5` creates 32
connections per node.

Available options:

```text
-addr string
      Redis or SeaProxy address (default "127.0.0.1:7000")
-concurrency int
      number of concurrent workers (default 50)
-connection-mode string
      connection lifecycle: pooled or per-transaction (default "pooled")
-cluster-hash-tags
      use one Redis Cluster hash tag for all keys in each MULTI transaction
-duration duration
      benchmark duration (default 10s)
-backend-stats-addr string
      optional direct Redis address used to report backend connections created and closed
-cluster
      use the Redis Cluster client
-client-per-worker
      create one non-multiplexed rueidis client per worker
-keyspace int
      number of keys to use (default 10000)
-key-hash-tag string
      optional Redis Cluster hash tag applied to every benchmark key
-password-file string
      file containing the frontend password
-pipeline-multiplex int
      rueidis pipeline connection exponent; connections per node are 2^value (0-8)
-set-percent int
      percentage of operations that are SETs (default 20)
-tls
      use TLS for the Redis connection
-tls-server-name string
      TLS certificate server name; empty uses the host from -addr
-transaction-size int
      number of GET/SET commands queued in each MULTI transaction (default 2)
-value-size int
      value size in bytes (default 128)
-username string
      frontend ACL username
-workload string
      workload to run: getset or multi (default "getset")
```
