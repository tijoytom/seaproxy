# benchclient

`benchclient` runs a concurrent mix of Redis `GET` and `SET` commands using
[`go-redis`](https://github.com/redis/go-redis). By default it connects to
SeaProxy at `127.0.0.1:7000`, uses 50 workers for 10 seconds, and sends 20%
`SET` commands.

```sh
go run . -addr 127.0.0.1:7000
```

Use the `multi` workload to benchmark connection-affine transactions. This
example creates a new client connection for every transaction, queues four
GET/SET operations between `MULTI` and `EXEC`, and reads backend connection
counters directly from Redis:

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
-keyspace int
      number of keys to use (default 10000)
-key-hash-tag string
      optional Redis Cluster hash tag applied to every benchmark key
-password-file string
      file containing the frontend password
-set-percent int
      percentage of operations that are SETs (default 20)
-transaction-size int
      number of GET/SET commands queued in each MULTI transaction (default 2)
-value-size int
      value size in bytes (default 128)
-username string
      frontend ACL username
-workload string
      workload to run: getset or multi (default "getset")
```
